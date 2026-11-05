#include "server.h"

#include <atomic>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>

#include "chat_service.h"
#include "config.h"
#include "db.h"
#include "dpapi_util.h"
#include "event_bus.h"
#include "json_util.h"
#include "llm_provider.h"
#include "logger.h"
#include "openai_provider.h"

namespace srv {
namespace {

json parseBody(const httplib::Request& req, httplib::Response& res) {
  try {
    return json::parse(req.body);
  } catch (...) {
    res.status = 400;
    res.set_content(errJson(400, "请求体不是合法 JSON").dump(), "application/json; charset=utf-8");
    return json();
  }
}

std::string maskKey(const std::string& key) {
  if (key.size() <= 10) return "****";
  return key.substr(0, 6) + "..." + key.substr(key.size() - 4);
}

json personaToJson(const db::Persona& p) {
  return {{"id", p.id},
          {"name", p.name},
          {"avatar", p.avatar},
          {"greeting", p.greeting},
          {"system_prompt", p.system_prompt},
          {"temperature", p.temperature},
          {"is_active", p.is_active},
          {"created_at", p.created_at},
          {"updated_at", p.updated_at}};
}

json sessionToJson(const db::Session& s) {
  return {{"id", s.id},
          {"title", s.title},
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

std::mutex g_stopMtx;
std::map<int64_t, std::shared_ptr<std::atomic_bool>> g_stopFlags;

int64_t parseSessionId(const httplib::Request& req, httplib::Response& res) {
  json b = parseBody(req, res);
  if (res.status == 400) return 0;
  int64_t sid = b.value("session_id", (int64_t)0);
  if (!sid) {
    res.status = 400;
    res.set_content(errJson(400, "缺少 session_id").dump(), "application/json; charset=utf-8");
    return 0;
  }
  return sid;
}

// 核心流式管线：组装上下文 → 调用 provider → SSE 转发 → 落库
// 注意：内容提供器在 handler 返回后才执行，lambda 必须按值捕获
void runChat(int64_t sessionId, const std::string& content, bool appendUser,
             httplib::Response& res) {
  if (!db::getSession(sessionId)) {
    res.status = 404;
    res.set_content(errJson(404, "会话不存在").dump(), "application/json; charset=utf-8");
    return;
  }
  auto persona = db::getActivePersona();
  if (!persona) {
    res.status = 500;
    res.set_content(errJson(500, "没有激活的人设").dump(), "application/json; charset=utf-8");
    return;
  }
  std::string key;
  std::string keyEnc = db::getSetting("api_key_enc").value_or("");
  if (!keyEnc.empty()) {
    try {
      key = dpapi::unprotect(keyEnc);
    } catch (const std::exception& e) {
      logx::error(std::string("dpapi unprotect failed: ") + e.what());
    }
  }
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
  auto ctx = chat::buildContext(*persona, history, keep, appendUser ? content : "");

  if (appendUser) {
    auto sess = db::getSession(sessionId);
    if (sess && sess->title == "新对话") db::renameSession(sessionId, utf8Truncate(content, 20));
    db::appendMessage(sessionId, "user", content);
    db::touchSession(sessionId);
  }

  llm::ChatRequest req;
  req.messages = ctx;
  req.temperature = persona->temperature;
  req.base_url = db::getSetting("base_url").value_or("");
  req.api_key = key;
  req.model = db::getSetting("model").value_or("");
  req.proxy_enabled = db::getSetting("proxy_enabled").value_or("0") == "1";
  req.proxy_host = db::getSetting("proxy_host").value_or("");
  req.proxy_port = proxyPort;

  auto stop = std::make_shared<std::atomic_bool>(false);
  {
    std::lock_guard<std::mutex> lk(g_stopMtx);
    g_stopFlags[sessionId] = stop;
  }

  auto full = std::make_shared<std::string>();
  auto hadError = std::make_shared<bool>(false);
  auto errCode = std::make_shared<int>(0);
  auto errMsg = std::make_shared<std::string>();

  res.set_header("Cache-Control", "no-cache");
  res.set_chunked_content_provider(
      "text/event-stream",
      [req, sessionId, stop, full, hadError, errCode, errMsg](size_t,
                                                             httplib::DataSink& sink) {
        try {
          llm::OpenAiCompatProvider provider;
          provider.streamChat(
              req,
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
          {
            std::lock_guard<std::mutex> lk(g_stopMtx);
            g_stopFlags.erase(sessionId);
          }
          if (!*hadError && !full->empty()) {
            db::appendMessage(sessionId, "assistant", *full);
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
        // httplib 0.18 的 chunked provider 会反复调用直到 sink.done()
        sink.done();
        return true;
      });
}

void registerChatRoutes(httplib::Server& svr) {
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
                 {"proxy_port", db::getSetting("proxy_port").value_or("0")}};
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
    res.set_content(okJson(json::object()).dump(), "application/json; charset=utf-8");
  });
}

void registerPersonaRoutes(httplib::Server& svr) {
  svr.Get("/api/v1/personas", [](const httplib::Request&, httplib::Response& res) {
    json arr = json::array();
    for (auto& p : db::listPersonas()) arr.push_back(personaToJson(p));
    res.set_content(okJson(arr).dump(), "application/json; charset=utf-8");
  });

  svr.Post("/api/v1/personas", [](const httplib::Request& req, httplib::Response& res) {
    json b = parseBody(req, res);
    if (res.status == 400) return;
    if (b.value("name", "").empty() || b.value("system_prompt", "").empty()) {
      res.status = 400;
      res.set_content(errJson(400, "name 与 system_prompt 为必填").dump(), "application/json; charset=utf-8");
      return;
    }
    db::Persona p;
    p.name = b["name"].get<std::string>();
    p.system_prompt = b["system_prompt"].get<std::string>();
    p.avatar = b.value("avatar", "🌸");
    p.greeting = b.value("greeting", "");
    p.temperature = b.value("temperature", 0.8);
    if (b.contains("id") && b["id"].get<int64_t>() > 0) {
      p.id = b["id"].get<int64_t>();
      if (!db::updatePersona(p)) {
        res.status = 404;
        res.set_content(errJson(404, "人设不存在").dump(), "application/json; charset=utf-8");
        return;
      }
    } else {
      p = db::createPersona(p);
    }
    res.set_content(okJson(personaToJson(p)).dump(), "application/json; charset=utf-8");
  });

  svr.Delete(R"(/api/v1/personas/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
    int64_t id = std::stoll(req.matches[1].str());
    if (!db::deletePersona(id)) {
      res.status = 409;
      res.set_content(errJson(409, "不能删除当前激活的人设").dump(), "application/json; charset=utf-8");
      return;
    }
    res.set_content(okJson(json::object()).dump(), "application/json; charset=utf-8");
  });

  svr.Post(R"(/api/v1/personas/(\d+)/activate)", [](const httplib::Request& req, httplib::Response& res) {
    int64_t id = std::stoll(req.matches[1].str());
    if (!db::activatePersona(id)) {
      res.status = 404;
      res.set_content(errJson(404, "人设不存在").dump(), "application/json; charset=utf-8");
      return;
    }
    // 可选 query: ?session_id=N，若该会话为空则插入开场白
    auto persona = db::getActivePersona();
    if (req.has_param("session_id") && persona && !persona->greeting.empty()) {
      int64_t sid = std::stoll(req.get_param_value("session_id"));
      auto sess = db::getSession(sid);
      if (sess && db::listMessages(sid).empty()) {
        db::appendMessage(sid, "assistant", persona->greeting);
        db::touchSession(sid);
      }
    }
    ev::EventBus::instance().emit("persona.activated",
                                  {{"id", id}, {"name", persona ? persona->name : ""}});
    res.set_content(okJson(json::object()).dump(), "application/json; charset=utf-8");
  });
}

void registerSessionRoutes(httplib::Server& svr) {
  svr.Get("/api/v1/sessions", [](const httplib::Request&, httplib::Response& res) {
    json arr = json::array();
    for (auto& s : db::listSessions()) arr.push_back(sessionToJson(s));
    res.set_content(okJson(arr).dump(), "application/json; charset=utf-8");
  });

  svr.Post("/api/v1/sessions", [](const httplib::Request& req, httplib::Response& res) {
    json b = parseBody(req, res);
    if (res.status == 400) return;
    auto s = db::createSession(b.value("title", "新对话"));
    ev::EventBus::instance().emit("session.created", {{"id", s.id}, {"title", s.title}});
    res.set_content(okJson(sessionToJson(s)).dump(), "application/json; charset=utf-8");
  });

  svr.Patch(R"(/api/v1/sessions/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
    json b = parseBody(req, res);
    if (res.status == 400) return;
    int64_t id = std::stoll(req.matches[1].str());
    std::string title = b.value("title", "");
    if (title.empty()) {
      res.status = 400;
      res.set_content(errJson(400, "title 不能为空").dump(), "application/json; charset=utf-8");
      return;
    }
    db::renameSession(id, title);
    res.set_content(okJson(json::object()).dump(), "application/json; charset=utf-8");
  });

  svr.Delete(R"(/api/v1/sessions/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
    int64_t id = std::stoll(req.matches[1].str());
    db::deleteSession(id);
    ev::EventBus::instance().emit("session.deleted", {{"id", id}});
    res.set_content(okJson(json::object()).dump(), "application/json; charset=utf-8");
  });
}

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
  // CORS：允许 WebView2 虚拟主机与本地浏览器调试来源
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
    res.set_content("{\"status\":\"ok\",\"version\":\"1.0.0\"}", "application/json; charset=utf-8");
  });

  registerConfigRoutes(svr);
  registerPersonaRoutes(svr);
  registerSessionRoutes(svr);
  registerMessageRoutes(svr);
  registerExportRoute(svr);
  registerChatRoutes(svr);

  svr.set_error_handler([](const httplib::Request&, httplib::Response& res) {
    // 仅对"路由未匹配"的默认 404 补充统一 JSON；
    // 业务 400/500 若未写 body 保持原状态码，避免被错误改写
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
