#include "group_engine.h"

#include <chrono>

#include "db.h"
#include "dpapi_util.h"
#include "logger.h"
#include "memory_service.h"

namespace grp {

GroupEngine::GroupEngine(int64_t groupId, StreamFn streamFn)
    : groupId_(groupId), streamFn_(std::move(streamFn)) {}

GroupEngine::~GroupEngine() {
  if (state_.load() != State::Idle) {
    stopFlag_ = true;
    state_ = State::Idle;
    pauseCv_.notify_all();
    if (thread_.joinable()) thread_.join();
  }
}

std::string GroupEngine::state() const {
  switch (state_.load()) {
    case State::Running: return "running";
    case State::Paused: return "paused";
    default: return "idle";
  }
}

bool GroupEngine::start() {
  State expected = State::Idle;
  if (!state_.compare_exchange_strong(expected, State::Running)) return false;
  // stop() 先置 stopFlag_/Idle 再 join：若此时并发 start，先收尾旧线程再重建成员快照，
  // 避免旧线程与新线程同时访问 members_/thread_（thread_ 对 joinable 线程赋值会 terminate）。
  // 旧线程在 stopFlag_ 置位后即便 state 被本 start 改回 Running 也会退出（loop 以
  // !stopFlag_ 为退出条件），此处 join 不会自锁。
  if (thread_.joinable()) thread_.join();
  members_.clear();
  for (auto& m : db::listGroupMembers(groupId_)) {
    auto bot = db::getBot(m.bot_id);
    if (bot) members_.push_back({m.bot_id, bot->name, bot->avatar});
  }
  if (members_.empty()) {
    state_ = State::Idle;
    db::setGroupStatus(groupId_, "idle");
    broadcast("error", {{"bot_id", 0}, {"name", ""}, {"message", "群内没有可用机器人"}});
    broadcast("status", {{"state", "idle"}, {"turn", 0}});
    return false;
  }
  turn_ = 0;
  stopFlag_ = false;
  failCount_.clear();
  db::setGroupStatus(groupId_, "running");
  broadcast("status", {{"state", "running"}, {"turn", 0}});
  thread_ = std::thread(&GroupEngine::loop, this);
  return true;
}

bool GroupEngine::pause() {
  State expected = State::Running;
  if (!state_.compare_exchange_strong(expected, State::Paused)) return false;
  db::setGroupStatus(groupId_, "paused");
  broadcast("status", {{"state", "paused"}, {"turn", turn_.load()}});
  return true;
}

bool GroupEngine::resume() {
  State expected = State::Paused;
  if (!state_.compare_exchange_strong(expected, State::Running)) return false;
  db::setGroupStatus(groupId_, "running");
  broadcast("status", {{"state", "running"}, {"turn", turn_.load()}});
  pauseCv_.notify_all();
  return true;
}

bool GroupEngine::stop() {
  State s = state_.load();
  if (s == State::Idle) return false;
  stopFlag_ = true;
  state_ = State::Idle;
  pauseCv_.notify_all();
  if (thread_.joinable()) thread_.join();
  db::setGroupStatus(groupId_, "idle");
  broadcast("status", {{"state", "idle"}, {"turn", turn_.load()}});
  return true;
}

void GroupEngine::interject(const std::string& content) {
  auto m = db::appendGroupMessage(groupId_, 0, "user", content);
  broadcast("interject", {{"id", m.id}, {"content", m.content},
                          {"created_at", m.created_at}});
}

SubId GroupEngine::subscribe(Broadcaster b) {
  std::lock_guard<std::mutex> lk(subMtx_);
  SubId id = ++nextSubId_;
  subs_[id] = std::move(b);
  return id;
}

void GroupEngine::unsubscribe(SubId id) {
  std::lock_guard<std::mutex> lk(subMtx_);
  subs_.erase(id);
}

void GroupEngine::broadcast(const std::string& event, const json& data) {
  std::vector<Broadcaster> copy;
  {
    std::lock_guard<std::mutex> lk(subMtx_);
    for (auto& [id, fn] : subs_) copy.push_back(fn);
  }
  for (auto& fn : copy) {
    try {
      fn(event, data);
    } catch (...) {
    }
  }
}

void GroupEngine::loop() {
  size_t idx = 0;
  int skipped = 0;   // 连续无成功落库的轮数（全员失败防空转）
  // paused 是稳定状态：线程阻塞在条件变量上等待 resume，而不是退出
  // （退出则 resume 无法唤醒）。Idle 或 stopFlag_ 置位才退出循环。
  while (state_.load() != State::Idle && !stopFlag_.load()) {
    {
      std::unique_lock<std::mutex> lk(pauseMtx_);
      pauseCv_.wait(lk, [&] { return state_.load() != State::Paused; });
    }
    if (state_.load() != State::Running) break;   // Idle → 退出
    const Member& m = members_[idx % members_.size()];
    int turn = turn_.fetch_add(1) + 1;
    bool ok = runTurn(m, turn);
    skipped = ok ? 0 : skipped + 1;
    // 全员连续失败三轮才兜底暂停：单成员/小群由 failCount>=3 先触发
    // （若按 brief 的 skipped>=members_.size()，单成员群第一轮就会暂停，
    //   与 failCount>=3 的测试语义冲突）
    if (skipped >= 3 * static_cast<int>(members_.size()) &&
        state_.load() == State::Running) {
      state_ = State::Paused;
      db::setGroupStatus(groupId_, "paused");
      broadcast("status", {{"state", "paused"}, {"turn", turn}});
      broadcast("error", {{"bot_id", 0}, {"name", ""},
                          {"message", "群内机器人全员连续失败，已自动暂停"}});
      continue;   // 阻塞等待 resume，而非退出线程
    }
    idx = (idx + 1) % members_.size();
    // 轮间隔窗口：pause/stop 立即生效（谓词被状态变化唤醒）；
    // 同时给外部控制端（测试线程）留出在下一轮 buildRequest 前落 stop 的窗口，
    // 避免上一轮刚广播 turn_end 下一轮就抢跑覆盖上下文快照。
    {
      std::unique_lock<std::mutex> lk(pauseMtx_);
      pauseCv_.wait_for(lk, std::chrono::milliseconds(50),
                        [&] { return state_.load() != State::Running; });
    }
  }
}

bool GroupEngine::runTurn(const Member& m, int turn) {
  broadcast("turn_start", {{"bot_id", m.bot_id}, {"name", m.name},
                           {"avatar", m.avatar}, {"turn", turn}});
  auto bot = db::getBot(m.bot_id);
  if (!bot) {
    broadcast("error", {{"bot_id", m.bot_id}, {"name", m.name},
                        {"message", "机器人不存在，跳过"}});
    return false;
  }
  if (!bot->is_enabled) {
    broadcast("error", {{"bot_id", m.bot_id}, {"name", m.name},
                        {"message", "机器人已停用，跳过"}});
    return false;
  }

  std::string full;
  std::string errMsg;
  bool failed = false;
  bool gotDelta = false;
  int attempts = 0;
  for (;;) {
    failed = false;
    errMsg.clear();
    llm::ChatRequest req = buildRequest(*bot);
    streamFn_(req,
              [&](const llm::StreamEvent& e) {
                if (stopFlag_) return;
                if (e.type == "delta") {
                  gotDelta = true;
                  full += e.content;
                  broadcast("delta", {{"bot_id", m.bot_id}, {"text", e.content}});
                } else if (e.type == "error") {
                  failed = true;
                  errMsg = e.message;
                }
              },
              stopFlag_);
    // 网络类失败（本轮无任何 delta）重试一次；已有内容的中断直接计失败
    if (!failed || gotDelta || attempts >= 1 || stopFlag_) break;
    ++attempts;
  }

  if (stopFlag_) return false;   // 被停止：不落库不广播
  if (failed) {
    failCount_[m.bot_id]++;
    broadcast("error", {{"bot_id", m.bot_id}, {"name", m.name},
                        {"message", errMsg.empty() ? "回复失败" : errMsg}});
    if (failCount_[m.bot_id] >= 3) {
      state_ = State::Paused;
      db::setGroupStatus(groupId_, "paused");
      broadcast("status", {{"state", "paused"}, {"turn", turn}});
    }
    return false;
  }
  failCount_[m.bot_id] = 0;
  if (full.empty()) full = "（无内容）";
  db::appendGroupMessage(groupId_, m.bot_id, "assistant", full);
  db::touchGroup(groupId_);
  broadcast("turn_end", {{"bot_id", m.bot_id}});
  // v3：轮末异步提取该 Bot 的群记忆（最近 4 条群消息作对话快照）
  {
    auto snap = db::listGroupMessages(groupId_, 4);
    std::string dialogue;
    for (auto& gm : snap) {
      std::string who = "用户";
      if (gm.bot_id > 0)
        if (auto b = db::getBot(gm.bot_id)) who = b->name;
      dialogue += who + "：" + gm.content + "\n";
    }
    mem::MemoryService::instance().enqueueExtract(m.bot_id, "group", groupId_, dialogue);
  }
  return true;
}

llm::ChatRequest GroupEngine::buildRequest(const db::Bot& bot) {
  llm::ChatRequest req;
  req.temperature = bot.temperature;
  req.max_tokens = bot.max_tokens;
  // API 覆盖：机器人字段非空则覆盖全局 settings
  req.base_url = !bot.api_base_url.empty()
                     ? bot.api_base_url
                     : db::getSetting("base_url").value_or("");
  std::string keyEnc = !bot.api_key_enc.empty()
                           ? bot.api_key_enc
                           : db::getSetting("api_key_enc").value_or("");
  if (!keyEnc.empty()) {
    try {
      req.api_key = dpapi::unprotect(keyEnc);
    } catch (const std::exception& e) {
      logx::error(std::string("group dpapi unprotect failed: ") + e.what());
    }
  }
  req.model = !bot.api_model.empty() ? bot.api_model
                                     : db::getSetting("model").value_or("");
  req.proxy_enabled = db::getSetting("proxy_enabled").value_or("0") == "1";
  req.proxy_host = db::getSetting("proxy_host").value_or("");
  try {
    req.proxy_port = std::stoi(db::getSetting("proxy_port").value_or("0"));
  } catch (...) {
    req.proxy_port = 0;
  }

  req.messages.push_back({"system", bot.system_prompt});
  auto group = db::getGroup(groupId_);
  if (group && !group->topic.empty())
    req.messages.push_back({"system", "当前群聊话题：" + group->topic});
  auto msgs = db::listGroupMessages(groupId_, 12);   // 最近 12 条（v3：随记忆块缩小）
  std::string recent;
  for (auto& gm : msgs) recent += gm.content + "\n";
  auto mems =
      mem::MemoryService::instance().pickMemories(bot.id, "group", groupId_, recent, 5);
  if (!mems.empty()) {
    std::string block = "[长期记忆]\n";
    for (const auto& m : mems) block += "· " + m.content + "\n";
    req.messages.push_back({"system", block});
  }
  for (auto& gm : msgs) {
    if (gm.bot_id == 0) {
      req.messages.push_back({"user", gm.content});
    } else if (gm.bot_id == bot.id) {
      // 自己的历史消息保持助手身份原文（不含他人人设；其他机器人发言加名字标注）
      req.messages.push_back({"assistant", gm.content});
    } else {
      std::string name = "机器人";
      if (auto b = db::getBot(gm.bot_id)) name = b->name;
      req.messages.push_back({"assistant", "[" + name + "]：" + gm.content});
    }
  }
  return req;
}

GroupEngineRegistry& GroupEngineRegistry::instance() {
  static GroupEngineRegistry reg;
  return reg;
}

std::shared_ptr<GroupEngine> GroupEngineRegistry::getOrCreate(int64_t groupId,
                                                              StreamFn streamFn) {
  std::lock_guard<std::mutex> lk(mtx_);
  auto it = engines_.find(groupId);
  if (it != engines_.end()) return it->second;
  auto e = std::make_shared<GroupEngine>(groupId, std::move(streamFn));
  engines_[groupId] = e;
  return e;
}

void GroupEngineRegistry::remove(int64_t groupId) {
  std::shared_ptr<GroupEngine> e;
  {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = engines_.find(groupId);
    if (it == engines_.end()) return;
    e = it->second;
    engines_.erase(it);
  }
  e->stop();   // 锁外 join，避免阻塞其他 getOrCreate
}

}  // namespace grp
