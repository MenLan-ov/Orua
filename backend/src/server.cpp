#include "server.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>

#include "chat_service.h"
#include "config.h"
#include "db.h"
#include "dpapi_util.h"
#include "event_bus.h"
#include "group_engine.h"
#include "json_util.h"
#include "llm_provider.h"
#include "logger.h"
#include "memory_service.h"
#include "openai_provider.h"
#include "skills.h"

namespace srv {
namespace {

json parseBody(const httplib::Request& req, httplib::Response& res) {
  try {
    return json::parse(req.body);
  } catch (...) {
    res.status = 400;
    res.set_content(errJson(400, "请求体不是合法 JSON").dump(),
                    "application/json; charset=utf-8");
    return json();
  }
}

void fail(httplib::Response& res, int httpStatus, int code,
          const std::string& msg) {
  res.status = httpStatus;
  res.set_content(errJson(code, msg).dump(), "application/json; charset=utf-8");
}

std::string maskKey(const std::string& key) {
  if (key.size() <= 10) return "****";
  return key.substr(0, 6) + "..." + key.substr(key.size() - 4);
}

json botToJson(const db::Bot& b) {
  std::string masked;
  if (!b.api_key_enc.empty()) {
    try {
      masked = maskKey(dpapi::unprotect(b.api_key_enc));
    } catch (...) {
      masked = "****";
    }
  }
  return {{"id", b.id},
          {"name", b.name},
          {"avatar", b.avatar},
          {"greeting", b.greeting},
          {"system_prompt", b.system_prompt},
          {"temperature", b.temperature},
          {"is_enabled", b.is_enabled},
          {"api_base_url", b.api_base_url},
          {"api_key", masked},
          {"api_model", b.api_model},
          {"max_tokens", b.max_tokens},
          {"api_type", b.api_type},
          {"avatar_url", b.avatar_path.empty() ? json(nullptr) : json(b.avatar_path)},
          {"created_at", b.created_at},
          {"updated_at", b.updated_at}};
}

json sessionToJson(const db::Session& s) {
  return {{"id", s.id},
          {"title", s.title},
          {"bot_id", s.bot_id},
          {"created_at", s.created_at},
          {"updated_at", s.updated_at}};
}

json messageToJson(const db::Message& m) {
  return {{"id", m.id},
          {"session_id", m.session_id},
          {"role", m.role},
          {"content", m.content},
          {"created_at", m.created_at}};
}

json groupToJson(const db::Group& g) {
  return {{"id", g.id},
          {"name", g.name},
          {"topic", g.topic},
          {"status", g.status},
          {"member_count", g.member_count},
          {"last_active", g.last_active},
          {"created_at", g.created_at},
          {"updated_at", g.updated_at}};
}

json memberToJson(const db::GroupMember& m) {
  std::string name = "已删除", avatar = "❓";
  bool enabled = false;
  auto bot = db::getBot(m.bot_id);
  if (bot) {
    name = bot->name;
    avatar = bot->avatar;
    enabled = bot->is_enabled;
  }
  return {{"id", m.id},
          {"group_id", m.group_id},
          {"bot_id", m.bot_id},
          {"name", name},
          {"avatar", avatar},
          {"position", m.position},
          {"avatar_url", bot && !bot->avatar_path.empty() ? json(bot->avatar_path) : json(nullptr)},
          {"is_enabled", enabled}};
}

json groupMessageToJson(const db::GroupMessage& m) {
  std::string name = "用户", avatar = "🙋";
  json avatarUrl = nullptr;
  if (m.bot_id > 0) {
    auto bot = db::getBot(m.bot_id);
    if (bot) {
      name = bot->name;
      avatar = bot->avatar;
      avatarUrl = bot->avatar_path.empty() ? json(nullptr) : json(bot->avatar_path);
    } else {
      name = "已删除";
      avatar = "❓";
    }
  }
  return {{"id", m.id},
          {"group_id", m.group_id},
          {"bot_id", m.bot_id},
          {"name", name},
          {"avatar", avatar},
          {"avatar_url", avatarUrl},
          {"role", m.role},
          {"content", m.content},
          {"created_at", m.created_at}};
}

// 标准 base64 解码（头像上传用），返回空串表示解码失败
std::string b64Decode(const std::string& in) {
  static const std::string tbl =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  int val = 0, bits = -8;
  for (unsigned char c : in) {
    if (std::isspace(c) || c == '=') continue;
    size_t pos = tbl.find(static_cast<char>(c));
    if (pos == std::string::npos) return "";
    val = (val << 6) | static_cast<int>(pos);
    bits += 6;
    if (bits >= 0) {
      out.push_back(static_cast<char>((val >> bits) & 0xFF));
      bits -= 8;
    }
  }
  return out;
}

json memoryToJson(const db::Memory& m) {
  json sid = nullptr;
  if (m.scope == "private" && m.source_msg_id > 0)
    if (auto s = db::memorySourceSession(m.source_msg_id)) sid = *s;
  return {{"id", m.id},
          {"bot_id", m.bot_id},
          {"scope", m.scope},
          {"group_id", m.group_id == 0 ? json(nullptr) : json(m.group_id)},
          {"content", m.content},
          {"tags", m.tags},
          {"source_msg_id", m.source_msg_id == 0 ? json(nullptr) : json(m.source_msg_id)},
          {"source_session_id", sid},
          {"created_at", m.created_at},
          {"updated_at", m.updated_at}};
}

/* ---------- 机器人配置解析 ---------- */

int64_t resolveDefaultBot() {
  auto id = db::getSetting("default_bot_id");
  if (id) {
    try {
      int64_t botId = std::stoll(*id);
      if (db::getBot(botId)) return botId;
    } catch (...) {
    }
  }
  auto bots = db::listBots();
  if (!bots.empty()) return bots.front().id;
  return 0;
}

std::string unprotectOrEmpty(const std::string& enc) {
  if (enc.empty()) return "";
  try {
    return dpapi::unprotect(enc);
  } catch (const std::exception& e) {
    logx::error(std::string("dpapi unprotect failed: ") + e.what());
    return "";
  }
}

struct BotConfig {
  std::string system_prompt;
  std::string base_url;
  std::string api_key;
  std::string model;
  double temperature = 0.8;
  int max_tokens = 0;
};

// 覆盖项为空 = 继承全局默认（settings）
BotConfig resolveBotConfig(const db::Bot& bot) {
  BotConfig cfg;
  cfg.system_prompt = bot.system_prompt;
  cfg.temperature = bot.temperature;
  cfg.max_tokens = bot.max_tokens;
  cfg.base_url = bot.api_base_url;
  cfg.model = bot.api_model;
  cfg.api_key = unprotectOrEmpty(bot.api_key_enc);
  if (cfg.base_url.empty()) cfg.base_url = db::getSetting("base_url").value_or("");
  if (cfg.api_key.empty())
    cfg.api_key = unprotectOrEmpty(db::getSetting("api_key_enc").value_or(""));
  if (cfg.model.empty()) cfg.model = db::getSetting("model").value_or("");
  return cfg;
}

std::optional<db::Bot> botForSession(int64_t sessionId) {
  auto sess = db::getSession(sessionId);
  if (!sess) return std::nullopt;
  if (sess->bot_id > 0) {
    auto bot = db::getBot(sess->bot_id);
    if (bot) return bot;
  }
  int64_t def = resolveDefaultBot();
  if (def) return db::getBot(def);
  return std::nullopt;
}

/* ---------- 群聊引擎接入 ---------- */

grp::StreamFn engineStreamFn() {
  return [](const llm::ChatRequest& req, llm::StreamCallback cb,
            const std::atomic_bool& stop) {
    llm::OpenAiCompatProvider provider;
    provider.streamChat(req, std::move(cb), stop);
  };
}

// 每群控制调用互斥：引擎 stop()+start() 并发会双 join 旧线程
// （std::thread 对 joinable 线程 join 会抛错），start/pause/resume/stop 与
// 删除端点必须按群串行。SSE 订阅回调只写各自队列，不触达控制路径。
std::mutex g_ctrlRegMtx;
std::map<int64_t, std::shared_ptr<std::mutex>> g_ctrlMtxes;

std::shared_ptr<std::mutex> ctrlMutex(int64_t groupId) {
  std::lock_guard<std::mutex> lk(g_ctrlRegMtx);
  auto it = g_ctrlMtxes.find(groupId);
  if (it == g_ctrlMtxes.end())
    it = g_ctrlMtxes.emplace(groupId, std::make_shared<std::mutex>()).first;
  return it->second;
}

// 校验群成员：2-10 个、须存在且启用
bool validateBotIds(const json& arr, std::vector<int64_t>& out,
                    std::string& err, int& errCode) {
  if (!arr.is_array() || arr.size() < 2 || arr.size() > 10) {
    err = "群成员需为 2-10 个机器人";
    errCode = 1003;
    return false;
  }
  for (auto& v : arr) {
    int64_t id = v.get<int64_t>();
    auto bot = db::getBot(id);
    if (!bot) {
      err = "机器人不存在: " + std::to_string(id);
      errCode = 404;
      return false;
    }
    if (!bot->is_enabled) {
      err = "机器人已停用: " + bot->name;
      errCode = 1004;
      return false;
    }
    out.push_back(id);
  }
  return true;
}

/* ---------- 单聊流式管线（按会话机器人配置） ---------- */

std::mutex g_stopMtx;
std::map<int64_t, std::shared_ptr<std::atomic_bool>> g_stopFlags;

int64_t parseSessionId(const httplib::Request& req, httplib::Response& res) {
  json b = parseBody(req, res);
  if (res.status == 400) return 0;
  int64_t sid = b.value("session_id", (int64_t)0);
  if (!sid) {
    fail(res, 400, 400, "缺少 session_id");
    return 0;
  }
  return sid;
}

void runChat(int64_t sessionId, const std::string& content, bool appendUser,
             httplib::Response& res) {
  if (!db::getSession(sessionId)) {
    fail(res, 404, 404, "会话不存在");
    return;
  }
  auto bot = botForSession(sessionId);
  if (!bot || bot->system_prompt.empty()) {
    fail(res, 500, 500, "没有可用机器人");
    return;
  }
  BotConfig cfg = resolveBotConfig(*bot);
  int keep = 20;
  try {
    keep = std::stoi(db::getSetting("keep_rounds").value_or("20"));
  } catch (...) {
  }
  int proxyPort = 0;
  try {
    proxyPort = std::stoi(db::getSetting("proxy_port").value_or("0"));
  } catch (...) {
  }

  // 先组装上下文再落库，避免 user 消息在 LLM 上下文中重复出现
  auto history = db::listMessages(sessionId);
  // v3：私聊记忆注入（增强式；检索失败为空时等价 v2）
  std::vector<db::Memory> mems;
  if (bot) mems = mem::MemoryService::instance().pickMemories(
                    bot->id, "private", 0, appendUser ? content : "", 5);
  auto ctx = chat::buildContext(cfg.system_prompt, history, keep,
                                appendUser ? content : "", mems);

  if (appendUser) {
    auto sess = db::getSession(sessionId);
    if (sess && sess->title == "新对话") db::renameSession(sessionId, utf8Truncate(content, 20));
    db::appendMessage(sessionId, "user", content);
    db::touchSession(sessionId);
  }

  llm::ChatRequest req;
  req.messages = ctx;
  req.temperature = cfg.temperature;
  req.max_tokens = cfg.max_tokens;
  req.base_url = cfg.base_url;
  req.api_key = cfg.api_key;
  req.model = cfg.model;
  req.proxy_enabled = db::getSetting("proxy_enabled").value_or("0") == "1";
  req.proxy_host = db::getSetting("proxy_host").value_or("");
  req.proxy_port = proxyPort;

  // v4：携带该 Bot 启用技能的 tools 定义
  for (auto& s : db::listBotSkills(bot ? bot->id : 0)) {
    if (!s.enabled) continue;
    for (auto& def : sk::registry()) {
      if (def.id != s.skill_id) continue;
      req.tools.push_back({{"type", "function"},
                           {"function", {{"name", def.id},
                                         {"description", def.description},
                                         {"parameters", def.params_schema}}}});
    }
  }

  auto stop = std::make_shared<std::atomic_bool>(false);
  {
    std::lock_guard<std::mutex> lk(g_stopMtx);
    g_stopFlags[sessionId] = stop;
  }

  auto full = std::make_shared<std::string>();
  auto hadError = std::make_shared<bool>(false);
  auto errCode = std::make_shared<int>(0);
  auto errMsg = std::make_shared<std::string>();

  int64_t botId = bot ? bot->id : 0;
  std::string userText = appendUser ? content : (history.empty() ? "" : history.back().content);

  res.set_header("Cache-Control", "no-cache");
  res.set_chunked_content_provider(
      "text/event-stream",
      [req, sessionId, botId, userText, stop, full, hadError, errCode, errMsg](size_t,
                                                             httplib::DataSink& sink) {
        try {
          llm::OpenAiCompatProvider provider;
          llm::ChatRequest cur = req;
          for (int round = 0; round < 3; ++round) {
            std::string toolCallsJson;
            provider.streamChat(
                cur,
                [&](const llm::StreamEvent& e) {
                  if (!sink.is_writable()) {
                    stop->store(true);
                    return;
                  }
                  if (e.type == "delta") {
                    *full += e.content;
                    json j = {{"content", e.content}};
                    std::string sse = "event: delta\ndata: " + j.dump() + "\n\n";
                    sink.write(sse.data(), sse.size());
                  } else if (e.type == "tool_calls") {
                    toolCallsJson = e.data;
                  } else if (e.type == "error") {
                    *hadError = true;
                    *errCode = e.error_code;
                    *errMsg = e.message;
                    json j = {{"code", e.error_code}, {"message", e.message}};
                    std::string sse = "event: error\ndata: " + j.dump() + "\n\n";
                    sink.write(sse.data(), sse.size());
                  }
                },
                *stop);
            if (*hadError || *stop || toolCallsJson.empty()) break;
            // 执行工具并回填，进入下一轮
            json tcs;
            try {
              tcs = json::parse(toolCallsJson);
            } catch (...) {
              break;
            }
            json assistantExtra = {{"tool_calls", tcs}};
            cur.messages.push_back({"assistant", "", assistantExtra});
            for (auto& tc : tcs) {
              std::string name = tc["function"].value("name", "");
              std::string callId = tc.value("id", "call_unknown");
              json params = json::object();
              try {
                std::string args = tc["function"].value("arguments", "{}");
                auto parsed = json::parse(args);
                if (parsed.is_object()) params = parsed;
              } catch (...) {
              }
              json result = sk::execute(name, params);
              cur.messages.push_back(
                  {"tool", result.dump(), json{{"tool_call_id", callId}}});
            }
          }
          {
            std::lock_guard<std::mutex> lk(g_stopMtx);
            g_stopFlags.erase(sessionId);
          }
          if (!*hadError && !full->empty()) {
            int64_t aid = db::appendMessage(sessionId, "assistant", *full).id;
            if (botId > 0)
              mem::MemoryService::instance().enqueueExtract(
                  botId, "private", 0, "用户：" + userText + "\n助手：" + *full + "\n", aid);
            db::touchSession(sessionId);
            ev::EventBus::instance().emit(
                "chat.completed",
                {{"session_id", sessionId}, {"length", full->size()}});
          } else if (*hadError) {
            ev::EventBus::instance().emit(
                "chat.error", {{"session_id", sessionId},
                               {"code", *errCode},
                               {"message", *errMsg}});
          }
        } catch (const std::exception& e) {
          logx::error(std::string("chat stream failed: ") + e.what());
          if (sink.is_writable()) {
            json j = {{"code", 500}, {"message", std::string("服务内部错误: ") + e.what()}};
            std::string sse = "event: error\ndata: " + j.dump() + "\n\n";
            sink.write(sse.data(), sse.size());
          }
        }
        if (sink.is_writable()) {
          json done = {{"session_id", sessionId}};
          std::string sse = "event: done\ndata: " + done.dump() + "\n\n";
          sink.write(sse.data(), sse.size());
        }
        sink.done();
        return true;
      });
}

void registerChatRoutes(httplib::Server& svr) {
  // 与 v1 完全一致：POST /api/v1/chat/stream、/regenerate、/stop（原样保留）
  logx::info("registering chat routes");
  svr.Post("/api/v1/chat/stream", [](const httplib::Request& req, httplib::Response& res) {
    json b = parseBody(req, res);
    if (res.status == 400) return;
    int64_t sid = b.value("session_id", (int64_t)0);
    std::string content = b.value("content", "");
    if (!sid) {
      res.status = 400;
      res.set_content(errJson(400, "缺少 session_id").dump(), "application/json; charset=utf-8");
      return;
    }
    if (content.empty()) {
      res.status = 400;
      res.set_content(errJson(400, "消息内容为空").dump(), "application/json; charset=utf-8");
      return;
    }
    runChat(sid, content, true, res);
  });

  svr.Post("/api/v1/chat/regenerate", [](const httplib::Request& req, httplib::Response& res) {
    int64_t sid = parseSessionId(req, res);
    if (!sid) return;
    auto lastU = db::lastMessageByRole(sid, "user");
    if (!lastU) {
      res.status = 400;
      res.set_content(errJson(400, "没有可重新生成的消息").dump(), "application/json; charset=utf-8");
      return;
    }
    auto lastA = db::lastMessageByRole(sid, "assistant");
    if (lastA) db::deleteMessage(lastA->id);
    // 历史已以最后一条 user 消息结尾，buildContext 直接复用即可
    runChat(sid, "", false, res);
  });

  svr.Post("/api/v1/chat/stop", [](const httplib::Request& req, httplib::Response& res) {
    int64_t sid = parseSessionId(req, res);
    if (!sid) return;
    {
      std::lock_guard<std::mutex> lk(g_stopMtx);
      auto it = g_stopFlags.find(sid);
      if (it != g_stopFlags.end()) it->second->store(true);
    }
    res.set_content(okJson(json{{"stopped", true}}).dump(), "application/json; charset=utf-8");
  });
}

/* ---------- 机器人路由 ---------- */

void registerBotRoutes(httplib::Server& svr) {
  svr.Get("/api/v1/bots", [](const httplib::Request&, httplib::Response& res) {
    json arr = json::array();
    for (auto& b : db::listBots()) arr.push_back(botToJson(b));
    json data = {{"bots", arr}, {"default_bot_id", resolveDefaultBot()}};
    res.set_content(okJson(data).dump(), "application/json; charset=utf-8");
  });

  svr.Post("/api/v1/bots", [](const httplib::Request& req, httplib::Response& res) {
    json b = parseBody(req, res);
    if (res.status == 400) return;
    if (b.value("name", "").empty() || b.value("system_prompt", "").empty()) {
      fail(res, 400, 400, "name 与 system_prompt 为必填");
      return;
    }
    db::Bot bot;
    bot.name = b["name"].get<std::string>();
    bot.system_prompt = b["system_prompt"].get<std::string>();
    bot.avatar = b.value("avatar", "🤖");
    bot.greeting = b.value("greeting", "");
    bot.temperature = b.value("temperature", 0.8);
    bot.api_base_url = b.value("api_base_url", "");
    bot.api_model = b.value("api_model", "");
    bot.max_tokens = b.value("max_tokens", 2048);
    bot.api_type = b.value("api_type", "openai");
    if (bot.api_type != "openai") {
      fail(res, 400, 400, "api_type 仅支持 openai");
      return;
    }
    std::string key = b.value("api_key", "");
    if (!key.empty()) bot.api_key_enc = dpapi::protect(key);
    bot = db::createBot(bot);
    res.set_content(okJson(botToJson(bot)).dump(), "application/json; charset=utf-8");
  });

  svr.Patch(R"(/api/v1/bots/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
    json b = parseBody(req, res);
    if (res.status == 400) return;
    int64_t id = std::stoll(req.matches[1].str());
    auto bot = db::getBot(id);
    if (!bot) {
      fail(res, 404, 404, "机器人不存在");
      return;
    }
    if (b.contains("name")) bot->name = b["name"].get<std::string>();
    if (b.contains("avatar")) bot->avatar = b["avatar"].get<std::string>();
    if (b.contains("greeting")) bot->greeting = b["greeting"].get<std::string>();
    if (b.contains("system_prompt")) bot->system_prompt = b["system_prompt"].get<std::string>();
    if (b.contains("temperature")) bot->temperature = b["temperature"].get<double>();
    if (b.contains("api_base_url")) bot->api_base_url = b["api_base_url"].get<std::string>();
    if (b.contains("api_model")) bot->api_model = b["api_model"].get<std::string>();
    if (b.contains("max_tokens")) bot->max_tokens = b["max_tokens"].get<int>();
    if (b.contains("api_type")) {
      bot->api_type = b["api_type"].get<std::string>();
      if (bot->api_type != "openai") {
        fail(res, 400, 400, "api_type 仅支持 openai");
        return;
      }
    }
    if (b.contains("avatar_path")) bot->avatar_path = b["avatar_path"].get<std::string>();
    if (b.contains("api_key")) {
      // 空串 = 清除覆盖，不传 = 不变
      std::string key = b["api_key"].get<std::string>();
      bot->api_key_enc = key.empty() ? "" : dpapi::protect(key);
    }
    if (!db::updateBot(*bot)) {
      fail(res, 404, 404, "机器人不存在");
      return;
    }
    res.set_content(okJson(botToJson(*bot)).dump(), "application/json; charset=utf-8");
  });

  svr.Delete(R"(/api/v1/bots/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
    int64_t id = std::stoll(req.matches[1].str());
    if (!db::getBot(id)) {
      fail(res, 404, 404, "机器人不存在");
      return;
    }
    if (!db::deleteBot(id)) {
      fail(res, 409, 1001, "默认机器人不可删除");
      return;
    }
    res.set_content(okJson(json::object()).dump(), "application/json; charset=utf-8");
  });

  svr.Post(R"(/api/v1/bots/(\d+)/enable)", [](const httplib::Request& req, httplib::Response& res) {
    json b = parseBody(req, res);
    if (res.status == 400) return;
    int64_t id = std::stoll(req.matches[1].str());
    if (!db::getBot(id)) {
      fail(res, 404, 404, "机器人不存在");
      return;
    }
    db::setBotEnabled(id, b.value("enabled", true));
    res.set_content(okJson(json::object()).dump(), "application/json; charset=utf-8");
  });
}

/* ---------- 群路由 ---------- */

void registerGroupRoutes(httplib::Server& svr) {
  svr.Get("/api/v1/groups", [](const httplib::Request&, httplib::Response& res) {
    json arr = json::array();
    for (auto& g : db::listGroups()) arr.push_back(groupToJson(g));
    res.set_content(okJson(arr).dump(), "application/json; charset=utf-8");
  });

  svr.Post("/api/v1/groups", [](const httplib::Request& req, httplib::Response& res) {
    json b = parseBody(req, res);
    if (res.status == 400) return;
    std::string name = b.value("name", "");
    if (name.empty()) {
      fail(res, 400, 400, "name 为必填");
      return;
    }
    std::vector<int64_t> ids;
    std::string err;
    int errCode = 0;
    if (!validateBotIds(b.value("bot_ids", json::array()), ids, err, errCode)) {
      int http = (errCode == 1003 || errCode == 1004) ? 400 : 404;
      fail(res, http, errCode, err);
      return;
    }
    auto g = db::createGroup(name, b.value("topic", ""));
    db::setGroupMembers(g.id, ids);
    auto g2 = db::getGroup(g.id);
    res.set_content(okJson(groupToJson(*g2)).dump(), "application/json; charset=utf-8");
  });

  svr.Get(R"(/api/v1/groups/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
    int64_t id = std::stoll(req.matches[1].str());
    auto g = db::getGroup(id);
    if (!g) {
      fail(res, 404, 404, "群不存在");
      return;
    }
    json members = json::array();
    for (auto& m : db::listGroupMembers(id)) members.push_back(memberToJson(m));
    json data = groupToJson(*g);
    data["members"] = members;
    res.set_content(okJson(data).dump(), "application/json; charset=utf-8");
  });

  svr.Patch(R"(/api/v1/groups/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
    json b = parseBody(req, res);
    if (res.status == 400) return;
    int64_t id = std::stoll(req.matches[1].str());
    auto g = db::getGroup(id);
    if (!g) {
      fail(res, 404, 404, "群不存在");
      return;
    }
    auto engine = grp::GroupEngineRegistry::instance().getOrCreate(id, engineStreamFn());
    if (b.contains("bot_ids") && engine->state() != "idle") {
      fail(res, 409, 1002, "群聊进行中，不能修改成员");
      return;
    }
    db::updateGroup(id, b.value("name", g->name), b.value("topic", g->topic));
    if (b.contains("bot_ids")) {
      std::vector<int64_t> ids;
      std::string err;
      int errCode = 0;
      if (!validateBotIds(b["bot_ids"], ids, err, errCode)) {
        int http = (errCode == 1003 || errCode == 1004) ? 400 : 404;
        fail(res, http, errCode, err);
        return;
      }
      db::setGroupMembers(id, ids);
    }
    auto g2 = db::getGroup(id);
    res.set_content(okJson(groupToJson(*g2)).dump(), "application/json; charset=utf-8");
  });

  svr.Delete(R"(/api/v1/groups/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
    int64_t id = std::stoll(req.matches[1].str());
    if (!db::getGroup(id)) {
      fail(res, 404, 404, "群不存在");
      return;
    }
    std::lock_guard<std::mutex> lk(*ctrlMutex(id));  // 与 start/pause/resume/stop 按群串行
    auto engine = grp::GroupEngineRegistry::instance().getOrCreate(id, engineStreamFn());
    engine->stop();  // 运行中先停止（idle 时 stop 为无副作用 no-op）
    db::deleteGroup(id);
    grp::GroupEngineRegistry::instance().remove(id);
    res.set_content(okJson(json::object()).dump(), "application/json; charset=utf-8");
  });

  svr.Get(R"(/api/v1/groups/(\d+)/messages)", [](const httplib::Request& req, httplib::Response& res) {
    int64_t id = std::stoll(req.matches[1].str());
    if (!db::getGroup(id)) {
      fail(res, 404, 404, "群不存在");
      return;
    }
    int limit = req.has_param("limit") ? std::stoi(req.get_param_value("limit")) : 200;
    auto msgs = db::listGroupMessages(id, limit);  // 升序的最后 limit 条
    json arr = json::array();
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) arr.push_back(groupMessageToJson(*it));
    res.set_content(okJson(arr).dump(), "application/json; charset=utf-8");
  });

  // 控制端点共享实现
  auto ctrl = [](const httplib::Request& req, httplib::Response& res, const std::string& op) {
    int64_t id = std::stoll(req.matches[1].str());
    if (!db::getGroup(id)) {
      fail(res, 404, 404, "群不存在");
      return;
    }
    std::lock_guard<std::mutex> lk(*ctrlMutex(id));  // 控制调用按群串行，避免 stop()+start() 竞态
    auto engine = grp::GroupEngineRegistry::instance().getOrCreate(id, engineStreamFn());
    if (op == "start") {
      json b = parseBody(req, res);
      if (res.status == 400) return;
      std::string topic = b.value("topic", "");
      auto g = db::getGroup(id);
      if (!topic.empty()) db::updateGroup(id, g->name, topic);
      if (engine->state() != "idle") {
        fail(res, 409, 1002, "群聊已在进行中");
        return;
      }
      if (!engine->start()) {
        fail(res, 409, 1002, "群内没有可用机器人");
        return;
      }
    } else if (op == "pause") {
      if (!engine->pause()) {
        fail(res, 409, 1002, "当前不在运行状态");
        return;
      }
    } else if (op == "resume") {
      if (!engine->resume()) {
        fail(res, 409, 1002, "当前不在暂停状态");
        return;
      }
    } else if (op == "stop") {
      engine->stop();  // 幂等，任意状态可调
    } else if (op == "interject") {
      json b = parseBody(req, res);
      if (res.status == 400) return;
      std::string content = b.value("content", "");
      if (content.empty()) {
        fail(res, 400, 400, "插话内容为空");
        return;
      }
      engine->interject(content);
    }
    res.set_content(okJson(json{{"state", engine->state()}}).dump(),
                    "application/json; charset=utf-8");
  };
  svr.Post(R"(/api/v1/groups/(\d+)/start)", [ctrl](const httplib::Request& req, httplib::Response& res) { ctrl(req, res, "start"); });
  svr.Post(R"(/api/v1/groups/(\d+)/pause)", [ctrl](const httplib::Request& req, httplib::Response& res) { ctrl(req, res, "pause"); });
  svr.Post(R"(/api/v1/groups/(\d+)/resume)", [ctrl](const httplib::Request& req, httplib::Response& res) { ctrl(req, res, "resume"); });
  svr.Post(R"(/api/v1/groups/(\d+)/stop)", [ctrl](const httplib::Request& req, httplib::Response& res) { ctrl(req, res, "stop"); });
  svr.Post(R"(/api/v1/groups/(\d+)/interject)", [ctrl](const httplib::Request& req, httplib::Response& res) { ctrl(req, res, "interject"); });

  // 群流 SSE：多客户端各自订阅，事件经每订阅者独立队列转发
  svr.Get(R"(/api/v1/groups/(\d+)/stream)", [](const httplib::Request& req, httplib::Response& res) {
    int64_t id = std::stoll(req.matches[1].str());
    if (!db::getGroup(id)) {
      fail(res, 404, 404, "群不存在");
      return;
    }
    auto engine = grp::GroupEngineRegistry::instance().getOrCreate(id, engineStreamFn());
    struct SubState {
      std::mutex mtx;
      std::condition_variable cv;
      std::deque<std::string> events;
    };
    auto st = std::make_shared<SubState>();
    grp::SubId sub = engine->subscribe([st](const std::string& event, const json& data) {
      {
        std::lock_guard<std::mutex> lk(st->mtx);
        st->events.push_back("event: " + event + "\ndata: " + data.dump() + "\n\n");
      }
      st->cv.notify_all();
    });
    // 初始 status：客户端立刻拿到当前状态与轮数
    // （与订阅回调同一把锁保护 deque，避免并发 push_back 数据竞争）
    {
      std::lock_guard<std::mutex> lk(st->mtx);
      st->events.push_back(
          "event: status\ndata: " +
          json{{"state", engine->state()}, {"turn", engine->turn()}}.dump() + "\n\n");
    }

    res.set_header("Cache-Control", "no-cache");
    res.set_chunked_content_provider(
        "text/event-stream",
        [engine, sub, st](size_t, httplib::DataSink& sink) {
          for (;;) {
            if (!sink.is_writable()) break;
            std::string sse;
            {
              std::unique_lock<std::mutex> lk(st->mtx);
              st->cv.wait_for(lk, std::chrono::milliseconds(500),
                              [&] { return !st->events.empty(); });
              if (!st->events.empty()) {
                sse = st->events.front();
                st->events.pop_front();
              }
            }
            if (sse.empty()) continue;
            if (!sink.is_writable()) break;
            sink.write(sse.data(), sse.size());
          }
          engine->unsubscribe(sub);
          sink.done();
          return true;
        });
  });
}

/* ---------- 技能与提醒路由（v4） ---------- */

void registerSkillRoutes(httplib::Server& svr) {
  svr.Get("/api/v1/skills", [](const httplib::Request&, httplib::Response& res) {
    json arr = json::array();
    for (auto& s : sk::registry())
      arr.push_back({{"id", s.id},
                     {"name", s.name},
                     {"description", s.description},
                     {"params_schema", s.params_schema}});
    res.set_content(okJson(json{{"skills", arr}}).dump(), "application/json; charset=utf-8");
  });

  svr.Get(R"(/api/v1/bots/(\d+)/skills)", [](const httplib::Request& req, httplib::Response& res) {
    int64_t id = std::stoll(req.matches[1].str());
    if (!db::getBot(id)) {
      fail(res, 404, 404, "Bot 不存在");
      return;
    }
    json arr = json::array();
    for (auto& s : db::listBotSkills(id))
      arr.push_back({{"skill_id", s.skill_id}, {"enabled", s.enabled}});
    res.set_content(okJson(json{{"skills", arr}}).dump(), "application/json; charset=utf-8");
  });

  svr.Put(R"(/api/v1/bots/(\d+)/skills)", [](const httplib::Request& req, httplib::Response& res) {
    int64_t id = std::stoll(req.matches[1].str());
    if (!db::getBot(id)) {
      fail(res, 404, 404, "Bot 不存在");
      return;
    }
    json b = parseBody(req, res);
    if (res.status == 400) return;
    if (!b.contains("skills") || !b["skills"].is_array()) {
      fail(res, 400, 400, "skills 必须为数组");
      return;
    }
    std::vector<std::pair<std::string, bool>> skills;
    for (auto& s : b["skills"]) {
      std::string sid = s.value("skill_id", "");
      bool on = s.value("enabled", true);
      bool known = false;
      for (auto& def : sk::registry())
        if (def.id == sid) known = true;
      if (!known) {
        fail(res, 400, 400, "未知技能：" + sid);
        return;
      }
      skills.push_back({sid, on});
    }
    db::setBotSkills(id, skills);
    res.set_content(okJson(json::object()).dump(), "application/json; charset=utf-8");
  });

  svr.Get("/api/v1/reminders", [](const httplib::Request& req, httplib::Response& res) {
    bool dueOnly = req.get_param_value("due") == "1";
    json arr = json::array();
    for (auto& r : db::listReminders(dueOnly))
      arr.push_back({{"id", r.id},
                     {"bot_id", r.bot_id},
                     {"content", r.content},
                     {"due_at", r.due_at},
                     {"done", r.done}});
    res.set_content(okJson(json{{"reminders", arr}}).dump(), "application/json; charset=utf-8");
  });

  svr.Post(R"(/api/v1/reminders/(\d+)/done)", [](const httplib::Request& req, httplib::Response& res) {
    int64_t id = std::stoll(req.matches[1].str());
    if (!db::markReminderDone(id)) {
      fail(res, 404, 404, "提醒不存在");
      return;
    }
    res.set_content(okJson(json::object()).dump(), "application/json; charset=utf-8");
  });
}

/* ---------- 记忆路由（v3） ---------- */

void registerMemoryRoutes(httplib::Server& svr) {
  svr.Get(R"(/api/v1/bots/(\d+)/memories)", [](const httplib::Request& req, httplib::Response& res) {
    int64_t id = std::stoll(req.matches[1].str());
    if (!db::getBot(id)) {
      fail(res, 404, 404, "Bot 不存在");
      return;
    }
    std::string scope = req.get_param_value("scope");
    if (!scope.empty() && scope != "private" && scope != "group") {
      fail(res, 400, 400, "scope 非法");
      return;
    }
    int64_t groupId = -1;
    std::string gs = req.get_param_value("group_id");
    if (!gs.empty()) {
      try {
        groupId = std::stoll(gs);
      } catch (...) {
        fail(res, 400, 400, "group_id 非法");
        return;
      }
    }
    std::string q = req.get_param_value("q");
    json arr = json::array();
    for (auto& m : db::listMemories(id, scope, groupId, q))
      arr.push_back(memoryToJson(m));
    res.set_content(okJson(json{{"memories", arr}}).dump(), "application/json; charset=utf-8");
  });

  svr.Put(R"(/api/v1/bots/(\d+)/memories/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
    int64_t id = std::stoll(req.matches[1].str());
    int64_t memId = std::stoll(req.matches[2].str());
    if (!db::getBot(id)) {
      fail(res, 404, 404, "Bot 不存在");
      return;
    }
    json b = parseBody(req, res);
    if (res.status == 400) return;
    std::string content = b.value("content", "");
    if (content.empty()) {
      fail(res, 400, 400, "记忆内容为空");
      return;
    }
    if (!db::updateMemoryContent(memId, content)) {
      fail(res, 404, 404, "记忆不存在");
      return;
    }
    res.set_content(okJson(json::object()).dump(), "application/json; charset=utf-8");
  });

  svr.Delete(R"(/api/v1/bots/(\d+)/memories/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
    int64_t id = std::stoll(req.matches[1].str());
    int64_t memId = std::stoll(req.matches[2].str());
    if (!db::getBot(id)) {
      fail(res, 404, 404, "Bot 不存在");
      return;
    }
    if (!db::deleteMemory(memId)) {
      fail(res, 404, 404, "记忆不存在");
      return;
    }
    res.set_content(okJson(json::object()).dump(), "application/json; charset=utf-8");
  });

  svr.Post(R"(/api/v1/bots/(\d+)/avatar)", [](const httplib::Request& req, httplib::Response& res) {
    int64_t id = std::stoll(req.matches[1].str());
    auto bot = db::getBot(id);
    if (!bot) {
      fail(res, 404, 404, "Bot 不存在");
      return;
    }
    json b = parseBody(req, res);
    if (res.status == 400) return;
    std::string raw = b64Decode(b.value("image_base64", ""));
    if (raw.empty() || raw.size() > 256 * 1024) {
      fail(res, 400, 400, "头像图片无效或超过 256KB");
      return;
    }
    std::string upDir = db::dataDir() + "/uploads";
    std::error_code ec;
    std::filesystem::create_directories(upDir, ec);
    std::string rel = "uploads/" + std::to_string(id) + ".png";
    {
      std::ofstream ofs(db::dataDir() + "/" + rel, std::ios::binary);
      ofs.write(raw.data(), static_cast<std::streamsize>(raw.size()));
    }
    bot->avatar_path = rel;
    db::updateBot(*bot);
    res.set_content(okJson(json{{"avatar_url", rel}}).dump(), "application/json; charset=utf-8");
  });

  svr.Get(R"(/api/v1/bots/(\d+)/avatar)", [](const httplib::Request& req, httplib::Response& res) {
    int64_t id = std::stoll(req.matches[1].str());
    auto bot = db::getBot(id);
    if (!bot || bot->avatar_path.empty()) {
      fail(res, 404, 404, "头像不存在");
      return;
    }
    std::ifstream ifs(db::dataDir() + "/" + bot->avatar_path, std::ios::binary);
    if (!ifs) {
      fail(res, 404, 404, "头像文件缺失");
      return;
    }
    std::string bytes((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    res.set_content(bytes, "image/png");
  });
}

/* ---------- 配置路由（与 v1 完全一致，原样保留） ---------- */

void registerConfigRoutes(httplib::Server& svr) {
  svr.Get("/api/v1/config", [](const httplib::Request&, httplib::Response& res) {
    std::string keyEnc = db::getSetting("api_key_enc").value_or("");
    std::string masked;
    if (!keyEnc.empty()) {
      try {
        masked = maskKey(dpapi::unprotect(keyEnc));
      } catch (...) {
        masked = "****";
      }
    }
    json data = {{"base_url", db::getSetting("base_url").value_or("")},
                 {"api_key", masked},
                 {"model", db::getSetting("model").value_or("")},
                 {"keep_rounds", db::getSetting("keep_rounds").value_or("20")},
                 {"theme", db::getSetting("theme").value_or("dark")},
                 {"proxy_enabled", db::getSetting("proxy_enabled").value_or("0")},
                 {"proxy_host", db::getSetting("proxy_host").value_or("")},
                 {"proxy_port", db::getSetting("proxy_port").value_or("0")},
                 {"memory_extraction", db::getSetting("memory_extraction").value_or("1")},
                 {"chat_pinned", db::getSetting("chat_pinned").value_or("[]")},
                 {"chat_reads", db::getSetting("chat_reads").value_or("{}")}};
    res.set_content(okJson(data).dump(), "application/json; charset=utf-8");
  });

  svr.Post("/api/v1/config", [](const httplib::Request& req, httplib::Response& res) {
    json b = parseBody(req, res);
    if (res.status == 400) return;
    if (b.contains("base_url")) db::setSetting("base_url", b["base_url"].get<std::string>());
    if (b.contains("api_key")) {
      std::string key = b["api_key"].get<std::string>();
      if (!key.empty()) db::setSetting("api_key_enc", dpapi::protect(key));
    }
    if (b.contains("model")) db::setSetting("model", b["model"].get<std::string>());
    if (b.contains("keep_rounds")) db::setSetting("keep_rounds", std::to_string(b["keep_rounds"].get<int>()));
    if (b.contains("theme")) db::setSetting("theme", b["theme"].get<std::string>());
    if (b.contains("proxy_enabled")) db::setSetting("proxy_enabled", b["proxy_enabled"].get<bool>() ? "1" : "0");
    if (b.contains("proxy_host")) db::setSetting("proxy_host", b["proxy_host"].get<std::string>());
    if (b.contains("proxy_port")) db::setSetting("proxy_port", std::to_string(b["proxy_port"].get<int>()));
    if (b.contains("memory_extraction"))
      db::setSetting("memory_extraction",
                     b["memory_extraction"].get<std::string>() == "1" ? "1" : "0");
    if (b.contains("chat_pinned")) db::setSetting("chat_pinned", b["chat_pinned"].get<std::string>());
    if (b.contains("chat_reads")) db::setSetting("chat_reads", b["chat_reads"].get<std::string>());
    res.set_content(okJson(json::object()).dump(), "application/json; charset=utf-8");
  });
}

/* ---------- 会话路由（v2：支持 bot_id） ---------- */

void registerSessionRoutes(httplib::Server& svr) {
  svr.Get("/api/v1/sessions", [](const httplib::Request&, httplib::Response& res) {
    json arr = json::array();
    for (auto& s : db::listSessions()) arr.push_back(sessionToJson(s));
    res.set_content(okJson(arr).dump(), "application/json; charset=utf-8");
  });

  svr.Post("/api/v1/sessions", [](const httplib::Request& req, httplib::Response& res) {
    json b = parseBody(req, res);
    if (res.status == 400) return;
    int64_t botId = b.value("bot_id", (int64_t)0);
    if (botId == 0) botId = resolveDefaultBot();
    auto bot = db::getBot(botId);
    if (!bot || !bot->is_enabled) {
      fail(res, 400, 400, "机器人不存在或已停用");
      return;
    }
    auto s = db::createSession(b.value("title", "新对话"), botId);
    ev::EventBus::instance().emit("session.created",
                                  {{"id", s.id}, {"title", s.title}, {"bot_id", s.bot_id}});
    res.set_content(okJson(sessionToJson(s)).dump(), "application/json; charset=utf-8");
  });

  svr.Patch(R"(/api/v1/sessions/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
    json b = parseBody(req, res);
    if (res.status == 400) return;
    int64_t id = std::stoll(req.matches[1].str());
    if (b.contains("bot_id")) {
      int64_t botId = b["bot_id"].get<int64_t>();
      auto bot = db::getBot(botId);
      if (!bot || !bot->is_enabled) {
        fail(res, 400, 400, "机器人不存在或已停用");
        return;
      }
      db::setSessionBot(id, botId);
    }
    if (b.contains("title")) {
      std::string title = b["title"].get<std::string>();
      if (title.empty()) {
        fail(res, 400, 400, "title 不能为空");
        return;
      }
      db::renameSession(id, title);
    }
    res.set_content(okJson(json::object()).dump(), "application/json; charset=utf-8");
  });

  svr.Delete(R"(/api/v1/sessions/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
    int64_t id = std::stoll(req.matches[1].str());
    db::deleteSession(id);
    ev::EventBus::instance().emit("session.deleted", {{"id", id}});
    res.set_content(okJson(json::object()).dump(), "application/json; charset=utf-8");
  });
}

/* ---------- 消息路由（与 v1 完全一致，原样保留） ---------- */

void registerMessageRoutes(httplib::Server& svr) {
  svr.Get("/api/v1/messages", [](const httplib::Request& req, httplib::Response& res) {
    int64_t sid = req.has_param("session_id") ? std::stoll(req.get_param_value("session_id")) : 0;
    if (!sid) {
      res.status = 400;
      res.set_content(errJson(400, "缺少 session_id").dump(), "application/json; charset=utf-8");
      return;
    }
    int limit = req.has_param("limit") ? std::stoi(req.get_param_value("limit")) : 100;
    auto msgs = db::listMessages(sid, limit);
    json arr = json::array();
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) arr.push_back(messageToJson(*it));
    res.set_content(okJson(arr).dump(), "application/json; charset=utf-8");
  });

  // 清空会话消息（query 版）
  svr.Delete("/api/v1/messages", [](const httplib::Request& req, httplib::Response& res) {
    int64_t sid = req.has_param("session_id") ? std::stoll(req.get_param_value("session_id")) : 0;
    if (!sid) {
      res.status = 400;
      res.set_content(errJson(400, "缺少 session_id").dump(), "application/json; charset=utf-8");
      return;
    }
    db::clearMessages(sid);
    res.set_content(okJson(json::object()).dump(), "application/json; charset=utf-8");
  });

  svr.Delete(R"(/api/v1/messages/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
    int64_t id = std::stoll(req.matches[1].str());
    db::deleteMessage(id);
    res.set_content(okJson(json::object()).dump(), "application/json; charset=utf-8");
  });
}

/* ---------- 导出路由（与 v1 完全一致，原样保留） ---------- */

void registerExportRoute(httplib::Server& svr) {
  svr.Post("/api/v1/export", [](const httplib::Request& req, httplib::Response& res) {
    int64_t sid = req.has_param("session_id") ? std::stoll(req.get_param_value("session_id")) : 0;
    std::string fmt = req.has_param("format") ? req.get_param_value("format") : "md";
    if (!sid) {
      res.status = 400;
      res.set_content(errJson(400, "缺少 session_id").dump(), "application/json; charset=utf-8");
      return;
    }
    auto sess = db::getSession(sid);
    if (!sess) {
      res.status = 404;
      res.set_content(errJson(404, "会话不存在").dump(), "application/json; charset=utf-8");
      return;
    }
    auto msgs = db::listMessages(sid);

    std::string body, mime, ext;
    if (fmt == "json") {
      mime = "application/json; charset=utf-8";
      ext = "json";
      json arr = json::array();
      for (auto& m : msgs)
        arr.push_back({{"role", m.role}, {"content", m.content}, {"created_at", m.created_at}});
      body = json{{"title", sess->title}, {"exported_at", nowIso()}, {"messages", arr}}.dump(2);
    } else if (fmt == "txt") {
      mime = "text/plain; charset=utf-8";
      ext = "txt";
      body = "会话：" + sess->title + "\n导出时间：" + nowIso() + "\n\n";
      for (auto& m : msgs) {
        std::string who = m.role == "user" ? "用户" : (m.role == "assistant" ? "AI" : "系统");
        body += "[" + who + "] " + m.created_at + "\n" + m.content + "\n\n";
      }
    } else {
      mime = "text/markdown; charset=utf-8";
      ext = "md";
      body = "# " + sess->title + "\n\n> 导出时间：" + nowIso() + "\n\n";
      for (auto& m : msgs) {
        std::string who = m.role == "user" ? "**用户**" : (m.role == "assistant" ? "**AI**" : "**系统**");
        body += who + "（" + m.created_at + "）\n\n" + m.content + "\n\n---\n\n";
      }
    }
    std::string fname = "session_" + std::to_string(sid) + "_" + std::to_string(std::time(nullptr)) + "." + ext;
    res.set_header("Content-Disposition", "attachment; filename=\"" + fname + "\"");
    res.set_content(body, mime);
  });
}

}  // namespace

void registerRoutes(httplib::Server& svr, const std::string& staticDir) {
  svr.set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
    std::string origin = req.get_header_value("Origin");
    bool allowed = origin == "https://app.local" ||
                   origin.rfind("http://localhost:", 0) == 0 ||
                   origin.rfind("http://127.0.0.1:", 0) == 0;
    if (allowed) {
      res.set_header("Access-Control-Allow-Origin", origin);
      res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS");
      res.set_header("Access-Control-Allow-Headers", "Content-Type");
    }
    if (req.method == "OPTIONS") {
      res.status = 204;
      return httplib::Server::HandlerResponse::Handled;
    }
    return httplib::Server::HandlerResponse::Unhandled;
  });

  svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
    res.set_content("{\"status\":\"ok\",\"version\":\"3.0.0\"}", "application/json; charset=utf-8");
  });

  registerSkillRoutes(svr);
  registerMemoryRoutes(svr);
  registerConfigRoutes(svr);
  registerBotRoutes(svr);
  registerGroupRoutes(svr);
  registerSessionRoutes(svr);
  registerMessageRoutes(svr);
  registerExportRoute(svr);
  registerChatRoutes(svr);

  svr.set_error_handler([](const httplib::Request&, httplib::Response& res) {
    if (res.body.empty() && res.status == 404) {
      res.set_content(errJson(404, "接口不存在").dump(), "application/json; charset=utf-8");
      return httplib::Server::HandlerResponse::Handled;
    }
    return httplib::Server::HandlerResponse::Unhandled;
  });

  svr.set_exception_handler([](const httplib::Request&, httplib::Response& res,
                               std::exception_ptr ep) {
    std::string msg = "未知异常";
    try {
      if (ep) std::rethrow_exception(ep);
    } catch (const std::exception& e) {
      msg = e.what();
    } catch (...) {
    }
    logx::error(std::string("handler exception: ") + msg);
    res.status = 500;
    res.set_content(errJson(500, "服务内部错误").dump(), "application/json; charset=utf-8");
  });

  if (!staticDir.empty()) {
    svr.set_mount_point("/", staticDir);
    svr.set_file_extension_and_mimetype_mapping("css", "text/css");
    svr.set_file_extension_and_mimetype_mapping("js", "text/javascript");
    svr.set_file_extension_and_mimetype_mapping("html", "text/html");
    svr.set_file_extension_and_mimetype_mapping("json", "application/json; charset=utf-8");
    svr.set_file_extension_and_mimetype_mapping("png", "image/png");
    svr.set_file_extension_and_mimetype_mapping("svg", "image/svg+xml");
    svr.set_file_extension_and_mimetype_mapping("woff2", "font/woff2");
    logx::info("static file serving enabled: " + staticDir);
  }
}

}  // namespace srv
