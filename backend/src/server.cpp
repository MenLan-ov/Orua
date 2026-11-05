#include "server.h"

#include <ctime>

#include "config.h"
#include "db.h"
#include "dpapi_util.h"
#include "event_bus.h"
#include "json_util.h"
#include "logger.h"

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

  svr.set_error_handler([](const httplib::Request&, httplib::Response& res) {
    // httplib 会对所有 status>=400 的响应调用本处理器；
    // 仅当业务处理器未写任何内容（即路由未匹配）时补充统一 404 JSON
    if (res.body.empty()) {
      res.status = 404;
      res.set_content(errJson(404, "接口不存在").dump(), "application/json; charset=utf-8");
      return httplib::Server::HandlerResponse::Handled;
    }
    return httplib::Server::HandlerResponse::Unhandled;
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
