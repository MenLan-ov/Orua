#include <doctest/doctest.h>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "db.h"
#include "group_engine.h"
#include "memory_service.h"

namespace {
std::filesystem::path tmpDb(const char* name) {
  auto p = std::filesystem::temp_directory_path() / name;
  std::filesystem::remove(p);
  return p;
}

// 注入式假流：前 failTimes 次调用直接发 error；onCall 可捕获请求上下文
struct FakeStream {
  std::vector<std::string> pieces = {"你好"};
  int failTimes = 0;
  std::atomic<int> calls{0};
  std::function<void()> onCall;
  std::mutex reqMtx;
  std::vector<llm::ChatMsg> lastMessages;
  int lastMaxTokens = 0;

  void operator()(const llm::ChatRequest& req, llm::StreamCallback cb,
                  const std::atomic_bool& stop) {
    {
      std::lock_guard<std::mutex> lk(reqMtx);
      lastMessages = req.messages;
      lastMaxTokens = req.max_tokens;
    }
    int n = calls.fetch_add(1) + 1;
    if (onCall) onCall();
    if (n <= failTimes) {
      cb({"error", "", 500, "模拟网络错误"});
      return;
    }
    for (auto& p : pieces) {
      if (stop) return;
      cb({"delta", p, 0, ""});
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
};

// 广播收集器
struct Collector {
  std::mutex mtx;
  std::vector<std::pair<std::string, json>> events;
  void operator()(const std::string& event, const json& data) {
    std::lock_guard<std::mutex> lk(mtx);
    events.push_back({event, data});
  }
  int count(const std::string& ev) {
    std::lock_guard<std::mutex> lk(mtx);
    int n = 0;
    for (auto& [e, d] : events) if (e == ev) ++n;
    return n;
  }
  json last(const std::string& ev) {
    std::lock_guard<std::mutex> lk(mtx);
    for (auto it = events.rbegin(); it != events.rend(); ++it)
      if (it->first == ev) return it->second;
    return json();
  }
};

void waitUntil(const std::function<bool()>& cond, int timeoutMs = 5000) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (cond()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE(cond());
}
}  // namespace

TEST_CASE("engine: 按 position 轮转并落库") {
  auto path = tmpDb("ac_v2_engine_rotate.db");
  REQUIRE(db::open(path.string()));
  db::setSetting("memory_extraction", "0");   // 引擎测试不触发提取 LLM 调用
  db::Bot ba; ba.name = "甲"; ba.system_prompt = "你是甲";
  db::Bot bb; bb.name = "乙"; bb.system_prompt = "你是乙";
  auto b1 = db::createBot(ba), b2 = db::createBot(bb);
  auto g = db::createGroup("测试群", "话题X");
  db::setGroupMembers(g.id, {b1.id, b2.id});

  FakeStream fake;
  fake.pieces = {"你好"};
  Collector col;
  grp::GroupEngine e(g.id, std::ref(fake));
  e.subscribe(std::ref(col));
  REQUIRE(e.start());
  CHECK(e.state() == "running");
  waitUntil([&] { return col.count("turn_end") >= 4; });
  REQUIRE(e.stop());
  CHECK(e.state() == "idle");

  CHECK(col.count("turn_start") == 4);
  CHECK(col.count("delta") == 4);
  CHECK(col.last("turn_end")["bot_id"] == b2.id);   // 第4轮是乙 → 甲,乙,甲,乙
  auto msgs = db::listGroupMessages(g.id);
  REQUIRE(msgs.size() == 4);
  CHECK(msgs[0].bot_id == b1.id);
  CHECK(msgs[1].bot_id == b2.id);
  CHECK(msgs[0].content == "你好");
  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("engine: 上下文 = 人设 + 话题 + 最近12条（不含他人人设）") {
  auto path = tmpDb("ac_v2_engine_ctx.db");
  REQUIRE(db::open(path.string()));
  db::setSetting("memory_extraction", "0");   // 引擎测试不触发提取 LLM 调用
  db::Bot ba; ba.name = "甲"; ba.system_prompt = "你是甲"; ba.max_tokens = 1234;
  auto b1 = db::createBot(ba);
  auto g = db::createGroup("上下文群", "话题Y");
  db::setGroupMembers(g.id, {b1.id});
  for (int i = 0; i < 25; ++i)
    db::appendGroupMessage(g.id, i % 2 ? b1.id : 0, i % 2 ? "assistant" : "user",
                           "msg" + std::to_string(i));

  FakeStream fake;
  fake.pieces = {"好"};
  Collector col;
  grp::GroupEngine e(g.id, std::ref(fake));
  e.subscribe(std::ref(col));
  REQUIRE(e.start());
  waitUntil([&] { return col.count("turn_end") >= 1; });
  REQUIRE(e.stop());

  CHECK(fake.lastMaxTokens == 1234);          // bot 配置 max_tokens 进入请求

  std::vector<llm::ChatMsg> ctx;
  {
    std::lock_guard<std::mutex> lk(fake.reqMtx);
    ctx = fake.lastMessages;
  }
  REQUIRE(ctx.size() == 14);               // system + 话题 + 12 条窗口
  CHECK(ctx[0].role == "system");
  CHECK(ctx[0].content == "你是甲");
  CHECK(ctx[1].role == "system");
  CHECK(ctx[1].content == "当前群聊话题：话题Y");
  CHECK(ctx[2].content == "msg13");        // 25 条截取最近 12 → 从 msg13 开始
  CHECK(ctx[13].content == "msg24");
  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("engine: 单轮失败重试一次并跳过（不计成功轮）") {
  auto path = tmpDb("ac_v2_engine_fail.db");
  REQUIRE(db::open(path.string()));
  db::setSetting("memory_extraction", "0");   // 引擎测试不触发提取 LLM 调用
  auto b1 = db::createBot([] {
    db::Bot b; b.name = "坏"; b.system_prompt = "x"; return b;
  }());
  auto b2 = db::createBot([] {
    db::Bot b; b.name = "好"; b.system_prompt = "x"; return b;
  }());
  auto g = db::createGroup("故障群", "");
  db::setGroupMembers(g.id, {b1.id, b2.id});

  FakeStream fake;
  fake.failTimes = 1;   // 第一次调用失败，之后成功 → 坏机器人重试成功
  Collector col;
  grp::GroupEngine e(g.id, std::ref(fake));
  e.subscribe(std::ref(col));
  REQUIRE(e.start());
  waitUntil([&] { return col.count("turn_end") >= 2; });
  REQUIRE(e.stop());
  CHECK(col.count("error") == 0);          // 重试成功不计失败
  auto msgs = db::listGroupMessages(g.id);
  REQUIRE(msgs.size() == 2);               // 两个机器人都成功落库
  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("engine: 永远失败 → 失败原因落库留痕（重开房间能看到为什么停了）") {
  auto path = tmpDb("ac_v2_engine_fail2.db");
  REQUIRE(db::open(path.string()));
  db::setSetting("memory_extraction", "0");   // 引擎测试不触发提取 LLM 调用
  auto b1 = db::createBot([] {
    db::Bot b; b.name = "坏"; b.system_prompt = "x"; return b;
  }());
  auto g = db::createGroup("故障群", "");
  db::setGroupMembers(g.id, {b1.id});

  FakeStream fake;
  fake.failTimes = 99;
  Collector col;
  grp::GroupEngine e(g.id, std::ref(fake));
  e.subscribe(std::ref(col));
  REQUIRE(e.start());
  waitUntil([&] { return col.count("error") >= 3; });   // 连续3次失败 → 自动暂停
  CHECK(e.state() == "paused");
  CHECK(col.last("status")["state"] == "paused");
  CHECK(fake.calls.load() >= 6);                        // 每轮重试1次 = 2 次调用
  REQUIRE(e.stop());

  // 失败轮不落机器人正文，但必须落 system 留痕：关掉引擎再进房间时，REST 读到的就是这些行
  int botMsgs = 0, notes = 0;
  bool hasReason = false, hasPause = false;
  for (auto& m : db::listGroupMessages(g.id)) {
    if (m.role == "system") {
      ++notes;
      CHECK(m.bot_id == 0);
      if (m.content.find("模拟网络错误") != std::string::npos) hasReason = true;
      if (m.content.find("自动暂停") != std::string::npos) hasPause = true;
    } else if (m.role == "assistant") {
      ++botMsgs;
    }
  }
  CHECK(botMsgs == 0);
  CHECK(notes >= 3);
  CHECK(hasReason);   // 留痕带错误原文，否则重开房间只知道「停了」不知道「为什么」
  CHECK(hasPause);    // 自动暂停这件事本身也要留痕
  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("engine: 失败留痕不占最近12条窗口（不进模型上下文）") {
  auto path = tmpDb("ac_f12_engine_note_ctx.db");
  REQUIRE(db::open(path.string()));
  db::setSetting("memory_extraction", "0");
  auto b1 = db::createBot([] {
    db::Bot b; b.name = "甲"; b.system_prompt = "你是甲"; return b;
  }());
  auto g = db::createGroup("留痕上下文群", "");
  db::setGroupMembers(g.id, {b1.id});
  for (int i = 0; i < 15; ++i)
    db::appendGroupMessage(g.id, 0, "user", "u" + std::to_string(i));
  db::appendGroupMessage(g.id, 0, "system", "⚠ 甲 回复失败：模拟网络错误");
  db::appendGroupMessage(g.id, 0, "user", "u15");

  FakeStream fake;
  fake.pieces = {"好"};
  Collector col;
  grp::GroupEngine e(g.id, std::ref(fake));
  e.subscribe(std::ref(col));
  REQUIRE(e.start());
  waitUntil([&] { return col.count("turn_end") >= 1; });
  REQUIRE(e.stop());

  std::vector<llm::ChatMsg> ctx;
  {
    std::lock_guard<std::mutex> lk(fake.reqMtx);
    ctx = fake.lastMessages;
  }
  REQUIRE(ctx.size() == 13);                 // 人设 + 最近 12 条真实对话
  CHECK(ctx[0].content == "你是甲");
  CHECK(ctx[1].content == "u4");             // 留痕不占窗口：12 条仍是 u4..u15
  CHECK(ctx.back().content == "u15");
  for (auto& m : ctx)
    CHECK(m.content.find("回复失败") == std::string::npos);
  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("engine: 暂停/继续/停止") {
  auto path = tmpDb("ac_v2_engine_pause.db");
  REQUIRE(db::open(path.string()));
  db::setSetting("memory_extraction", "0");   // 引擎测试不触发提取 LLM 调用
  auto def = db::getDefaultBot();
  REQUIRE(def.has_value());
  auto g = db::createGroup("控制群", "");
  db::setGroupMembers(g.id, {def->id});

  FakeStream fake;
  fake.pieces = {"哈"};
  Collector col;
  grp::GroupEngine e(g.id, std::ref(fake));
  e.subscribe(std::ref(col));
  REQUIRE(e.start());
  waitUntil([&] { return col.count("turn_end") >= 2; });
  REQUIRE(e.pause());
  CHECK(e.state() == "paused");
  std::this_thread::sleep_for(std::chrono::milliseconds(300));   // 让在途轮结束
  int ends = col.count("turn_end");
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK(col.count("turn_end") == ends);      // 暂停后无新轮

  REQUIRE(e.resume());
  CHECK(e.state() == "running");
  waitUntil([&] { return col.count("turn_end") > ends; });

  REQUIRE(e.stop());
  CHECK(e.state() == "idle");
  CHECK(col.last("status")["state"] == "idle");
  // 已 idle：再次 stop 返回 false（幂等由路由层处理）
  CHECK(e.stop() == false);
  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("engine: 插话落库并广播 interject 事件") {
  auto path = tmpDb("ac_v2_engine_interject.db");
  REQUIRE(db::open(path.string()));
  db::setSetting("memory_extraction", "0");   // 引擎测试不触发提取 LLM 调用
  auto def = db::getDefaultBot();
  REQUIRE(def.has_value());
  auto g = db::createGroup("插话群", "");
  db::setGroupMembers(g.id, {def->id});

  FakeStream fake;
  fake.pieces = {"收到"};
  Collector col;
  grp::GroupEngine e(g.id, std::ref(fake));
  e.subscribe(std::ref(col));
  REQUIRE(e.start());
  waitUntil([&] { return col.count("turn_end") >= 1; });
  e.interject("用户插话内容");
  waitUntil([&] { return col.count("interject") >= 1; });
  auto ev = col.last("interject");
  CHECK(ev["content"] == "用户插话内容");
  CHECK(ev["id"].get<int64_t>() > 0);

  auto msgs = db::listGroupMessages(g.id);
  bool found = false;
  for (auto& m : msgs)
    if (m.content == "用户插话内容" && m.bot_id == 0 && m.role == "user") found = true;
  CHECK(found);
  REQUIRE(e.stop());
  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("engine: 停用/被删机器人跳过且多群并行互不干扰") {
  auto path = tmpDb("ac_v2_engine_skip.db");
  REQUIRE(db::open(path.string()));
  db::setSetting("memory_extraction", "0");   // 引擎测试不触发提取 LLM 调用
  auto b1 = db::createBot([] {
    db::Bot b; b.name = "A"; b.system_prompt = "x"; return b;
  }());
  auto b2 = db::createBot([] {
    db::Bot b; b.name = "B"; b.system_prompt = "x"; return b;
  }());
  db::setBotEnabled(b2.id, false);   // 停用 → 跳过

  auto g1 = db::createGroup("群1", "");
  db::setGroupMembers(g1.id, {b1.id, b2.id});
  auto g2 = db::createGroup("群2", "");
  db::setGroupMembers(g2.id, {b1.id});

  FakeStream f1, f2;
  f1.pieces = {"一"};
  f2.pieces = {"二"};
  Collector c1, c2;
  grp::GroupEngine e1(g1.id, std::ref(f1));
  grp::GroupEngine e2(g2.id, std::ref(f2));
  e1.subscribe(std::ref(c1));
  e2.subscribe(std::ref(c2));
  REQUIRE(e1.start());
  REQUIRE(e2.start());
  waitUntil([&] { return c1.count("turn_end") >= 2 && c2.count("turn_end") >= 2; });

  // 群1 每轮都是 b1（b2 被跳过并广播 error）
  auto msgs = db::listGroupMessages(g1.id);
  for (auto& m : msgs) CHECK(m.bot_id == b1.id);
  CHECK(c1.count("error") >= 1);

  // 群1 暂停不影响群2
  REQUIRE(e1.pause());
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  int ends2 = c2.count("turn_end");
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK(c2.count("turn_end") > ends2);

  REQUIRE(e1.stop());
  REQUIRE(e2.stop());
  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("engine: registry 单实例与移除") {
  auto path = tmpDb("ac_v2_engine_registry.db");
  REQUIRE(db::open(path.string()));
  db::setSetting("memory_extraction", "0");   // 引擎测试不触发提取 LLM 调用
  auto def = db::getDefaultBot();
  REQUIRE(def.has_value());
  auto g = db::createGroup("注册群", "");
  db::setGroupMembers(g.id, {def->id});

  FakeStream fake;
  fake.pieces = {"好"};
  Collector col;
  grp::StreamFn fn = std::ref(fake);
  auto s1 = grp::GroupEngineRegistry::instance().getOrCreate(g.id, fn);
  auto s2 = grp::GroupEngineRegistry::instance().getOrCreate(g.id, fn);
  CHECK(s1.get() == s2.get());
  auto sub = s1->subscribe(std::ref(col));
  REQUIRE(s1->start());
  waitUntil([&] { return col.count("turn_end") >= 1; });
  s1->unsubscribe(sub);
  grp::GroupEngineRegistry::instance().remove(g.id);   // 内部 stop+join
  CHECK(s1->state() == "idle");
  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("engine: 群记忆块注入与 turn_end 提取触发") {
  auto path = tmpDb("ac_v3_engine_memory.db");
  REQUIRE(db::open(path.string()));
  db::setSetting("memory_extraction", "0");   // 本用例前半段关闭提取
  db::Bot ba; ba.name = "甲"; ba.system_prompt = "你是甲";
  auto b1 = db::createBot(ba);
  auto g = db::createGroup("记忆群", "");
  db::setGroupMembers(g.id, {b1.id});
  db::appendGroupMessage(g.id, 0, "user", "msg海边测试");
  // 标签与最近对话同词 → 本地召回命中；阶段 2 无网络 → 降级阶段 1 前 5
  db::addMemory(b1.id, "group", g.id, "用户喜欢海边", "[\"海边\"]", 0);

  FakeStream fake;
  fake.pieces = {"好"};
  Collector col;
  grp::GroupEngine e(g.id, std::ref(fake));
  e.subscribe(std::ref(col));
  REQUIRE(e.start());
  waitUntil([&] { return col.count("turn_end") >= 1; });
  REQUIRE(e.stop());

  bool found = false;
  {
    std::lock_guard<std::mutex> lk(fake.reqMtx);
    for (auto& m : fake.lastMessages)
      if (m.role == "system" && m.content.find("用户喜欢海边") != std::string::npos)
        found = true;
  }
  CHECK(found);
  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("engine: turn_end 后提取群记忆落库（LLM_MOCK）") {
  _putenv_s("LLM_MOCK", "1");
  auto path = tmpDb("ac_v3_engine_extract.db");
  REQUIRE(db::open(path.string()));
  db::Bot ba; ba.name = "甲"; ba.system_prompt = "你是甲";
  auto b1 = db::createBot(ba);
  auto g = db::createGroup("提取群", "");
  db::setGroupMembers(g.id, {b1.id});

  FakeStream fake;
  fake.pieces = {"你好"};
  Collector col;
  grp::GroupEngine e(g.id, std::ref(fake));
  e.subscribe(std::ref(col));
  REQUIRE(e.start());
  waitUntil([&] { return col.count("turn_end") >= 1; });
  REQUIRE(e.stop());
  waitUntil([&] { return db::listMemories(b1.id, "group", g.id).size() >= 2; });

  auto mems = db::listMemories(b1.id, "group", g.id);
  REQUIRE(mems.size() == 2);
  CHECK(mems[0].bot_id == b1.id);
  CHECK(mems[0].group_id == g.id);
  CHECK(mems[0].scope == "group");
  db::close();
  std::filesystem::remove(path);
  _putenv_s("LLM_MOCK", "");
}
