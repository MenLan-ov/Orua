#include "memory_service.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <stdexcept>

#include "dpapi_util.h"
#include "json_util.h"
#include "llm_provider.h"
#include "logger.h"
#include "openai_provider.h"

namespace mem {
namespace {

std::vector<std::string> tokensOf(const std::string& text) {
  std::vector<std::string> out;
  size_t i = 0;
  std::string word;
  auto flush = [&] {
    if (word.size() >= 2) out.push_back(word);
    word.clear();
  };
  while (i < text.size()) {
    unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
      if (std::isalnum(c)) word += static_cast<char>(c);
      else flush();
      ++i;
    } else {
      flush();
      size_t len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : 2;   // UTF-8 序列长度
      if (i + len <= text.size()) out.push_back(text.substr(i, len));   // 单个 CJK 字符
      i += len;
    }
  }
  flush();
  return out;
}

}  // namespace

MemoryService& MemoryService::instance() {
  static MemoryService svc;
  return svc;
}

MemoryService::~MemoryService() {
  {
    std::lock_guard<std::mutex> lk(mtx_);
    quit_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void MemoryService::ensureWorker() {
  if (workerStarted_) return;
  workerStarted_ = true;
  worker_ = std::thread([this] { workerLoop(); });
}

void MemoryService::enqueueExtract(int64_t botId, const std::string& scope,
                                   int64_t groupId, const std::string& dialogue,
                                   int64_t sourceMsgId) {
  if (db::getSetting("memory_extraction").value_or("1") != "1") return;
  {
    std::lock_guard<std::mutex> lk(mtx_);
    queue_.push_back({botId, scope, groupId, dialogue, sourceMsgId});
    ensureWorker();
  }
  cv_.notify_one();
}

void MemoryService::workerLoop() {
  for (;;) {
    ExtractJob job;
    {
      std::unique_lock<std::mutex> lk(mtx_);
      cv_.wait(lk, [&] { return !queue_.empty() || quit_; });
      if (quit_) return;
      job = queue_.front();
      queue_.pop_front();
    }
    try {
      extractOnce(job);
    } catch (const std::exception& e) {
      logx::error(std::string("memory extract failed: ") + e.what());
      std::this_thread::sleep_for(std::chrono::seconds(2));
      try {
        extractOnce(job);
      } catch (const std::exception& e2) {
        logx::error(std::string("memory extract retry failed: ") + e2.what());
      }
    }
  }
}

llm::ChatRequest MemoryService::buildExtractRequest(const db::Bot& bot) {
  llm::ChatRequest req;
  req.base_url = !bot.api_base_url.empty() ? bot.api_base_url
                                           : db::getSetting("base_url").value_or("");
  std::string keyEnc = !bot.api_key_enc.empty() ? bot.api_key_enc
                                                : db::getSetting("api_key_enc").value_or("");
  if (!keyEnc.empty()) {
    try {
      req.api_key = dpapi::unprotect(keyEnc);
    } catch (...) {
    }
  }
  req.model = !bot.api_model.empty() ? bot.api_model : db::getSetting("model").value_or("");
  req.max_tokens = bot.max_tokens;
  req.proxy_enabled = db::getSetting("proxy_enabled").value_or("0") == "1";
  req.proxy_host = db::getSetting("proxy_host").value_or("");
  try {
    req.proxy_port = std::stoi(db::getSetting("proxy_port").value_or("0"));
  } catch (...) {
    req.proxy_port = 0;
  }
  return req;
}

void MemoryService::extractOnce(const ExtractJob& job) {
  if (db::getSetting("memory_extraction").value_or("1") != "1") return;

  if (std::getenv("LLM_MOCK")) {
    // 固定两条：一条 new、一条 update 占位（规格 §4.1，测试确定性；幂等：重复提取不新增）
    auto existing = db::listMemories(job.bot_id, job.scope,
                                     job.scope == "group" ? job.group_id : -1);
    bool hasCoffee = false;
    for (auto& m : existing)
      if (m.content.find("咖啡") != std::string::npos) hasCoffee = true;
    if (hasCoffee && !existing.empty()) {
      db::updateMemoryContent(existing.front().id, existing.front().content);  // update 占位
    } else {
      db::addMemory(job.bot_id, job.scope, job.group_id, "记住：用户喜欢咖啡",
                    "[\"咖啡\",\"偏好\"]", job.source_msg_id);
    }
    if (existing.size() < 2)
      db::addMemory(job.bot_id, job.scope, job.group_id, "记住：助手应当先问好再回答",
                    "[\"礼仪\"]", job.source_msg_id);
    return;
  }

  auto bot = db::getBot(job.bot_id);
  if (!bot) return;

  llm::ChatRequest req = buildExtractRequest(*bot);
  std::string existingList;
  auto mems = db::listMemories(job.bot_id, job.scope,
                               job.scope == "group" ? job.group_id : -1);
  int shown = 0;
  for (auto& m : mems) {
    if (shown++ >= 200) break;   // 输入上限 200 条
    existingList += std::to_string(m.id) + ": " + m.content + "\n";
  }
  if (existingList.empty()) existingList = "（无）";

  req.messages.push_back(
      {"system",
       "你是记忆提取器。根据对话提炼值得长期记住的事实（用户偏好、名字、约定、重要信息）。"
       "只输出 JSON：{\"ops\":[{\"action\":\"new\",\"content\":\"...\",\"tags\":[\"...\"]},"
       "{\"action\":\"update\",\"memory_id\":3,\"content\":\"...\",\"tags\":[\"...\"]}]}"
       "没有值得记住的则输出 {\"ops\":[]}"});
  req.messages.push_back({"user", "现有记忆（id: 内容）：\n" + existingList +
                                      "\n对话：\n" + job.dialogue});

  std::string full;
  llm::OpenAiCompatProvider provider;
  std::atomic_bool stop{false};
  provider.streamChat(
      req, [&](const llm::StreamEvent& e) {
        if (e.type == "delta") full += e.content;
      },
      stop);
  applyOps(full, job);
}

void MemoryService::applyOps(const std::string& rawJson, const ExtractJob& job) {
  auto a = rawJson.find('{');
  auto b = rawJson.rfind('}');
  if (a == std::string::npos || b == std::string::npos || b < a) return;
  json j;
  try {
    j = json::parse(rawJson.substr(a, b - a + 1));
  } catch (...) {
    return;
  }
  if (!j.contains("ops") || !j["ops"].is_array()) return;
  for (auto& op : j["ops"]) {
    std::string action = op.value("action", "");
    std::string content = op.value("content", "");
    if (content.empty()) continue;
    std::string tags = "[]";
    if (op.contains("tags") && op["tags"].is_array()) tags = op["tags"].dump();
    if (action == "new") {
      db::addMemory(job.bot_id, job.scope, job.group_id, content, tags, job.source_msg_id);
    } else if (action == "update") {
      int64_t id = op.value("memory_id", (int64_t)0);
      if (id > 0) db::updateMemoryContent(id, content);
    }
  }
}

std::vector<db::Memory> MemoryService::recallLocal(int64_t botId,
                                                   const std::string& scope,
                                                   int64_t groupId,
                                                   const std::string& dialogue) {
  auto all = db::listMemories(botId, scope, scope == "group" ? groupId : -1);
  if (all.empty()) return {};
  auto toks = tokensOf(dialogue);
  if (toks.empty()) return {};
  struct Scored {
    db::Memory m;
    int score = 0;
    std::string upd;
  };
  std::vector<Scored> hits;
  for (auto& m : all) {
    int s = 0;
    for (auto& t : toks) {
      if (m.content.find(t) != std::string::npos) s += 2;
      if (m.tags.find(t) != std::string::npos) s += 1;
    }
    if (s > 0) hits.push_back({m, s, m.updated_at});
  }
  std::sort(hits.begin(), hits.end(), [](const Scored& x, const Scored& y) {
    if (x.score != y.score) return x.score > y.score;
    return x.upd > y.upd;
  });
  std::vector<db::Memory> out;
  for (size_t i = 0; i < hits.size() && i < 30; ++i) out.push_back(hits[i].m);
  return out;
}

std::vector<int64_t> MemoryService::selectByLlm(
    const std::vector<db::Memory>& candidates, const std::string& dialogue,
    int maxK) {
  if (candidates.empty()) return {};
  auto bot = db::getBot(candidates.front().bot_id);
  if (!bot) return {};
  llm::ChatRequest req = buildExtractRequest(*bot);
  std::string list;
  for (auto& c : candidates)
    list += std::to_string(c.id) + ": " + c.content + "\n";
  req.messages.push_back(
      {"system",
       "从候选记忆中选出与对话最相关的至多 " + std::to_string(maxK) +
           " 条。只输出 JSON 数组：[3, 7, ...]（无相关则 []）"});
  req.messages.push_back({"user", "候选记忆：\n" + list + "\n对话：\n" + dialogue});

  std::string full;
  llm::OpenAiCompatProvider provider;
  std::atomic_bool stop{false};
  try {
    provider.streamChat(
        req, [&](const llm::StreamEvent& e) {
          if (e.type == "delta") full += e.content;
        },
        stop);
    auto a = full.find('[');
    auto b = full.rfind(']');
    if (a == std::string::npos || b == std::string::npos || b < a) return {};
    json j = json::parse(full.substr(a, b - a + 1));
    std::vector<int64_t> ids;
    if (!j.is_array()) return {};
    for (auto& v : j) {
      if (!v.is_number()) continue;
      ids.push_back(v.get<int64_t>());
      if ((int)ids.size() >= maxK) break;
    }
    return ids;
  } catch (...) {
    return {};   // 失败 → 调用方降级
  }
}

std::vector<db::Memory> MemoryService::pickMemories(int64_t botId,
                                                    const std::string& scope,
                                                    int64_t groupId,
                                                    const std::string& dialogue,
                                                    int maxK) {
  auto cand = recallLocal(botId, scope, groupId, dialogue);
  if (cand.empty()) return {};
  std::vector<int64_t> ids;
  if (std::getenv("LLM_MOCK")) {
    for (size_t i = 0; i < cand.size() && i < (size_t)maxK; ++i)
      ids.push_back(cand[i].id);
  } else {
    ids = selectByLlm(cand, dialogue, maxK);
    if (ids.empty()) {
      // 降级：阶段 2 失败 → 阶段 1 前 maxK
      for (size_t i = 0; i < cand.size() && i < (size_t)maxK; ++i)
        ids.push_back(cand[i].id);
    }
  }
  std::vector<db::Memory> out;
  for (auto id : ids)
    for (auto& c : cand)
      if (c.id == id) {
        out.push_back(c);
        break;
      }
  return out;
}

}  // namespace mem
