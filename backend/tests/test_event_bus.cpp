#include <doctest/doctest.h>
#include <chrono>
#include <string>
#include <thread>

#include <httplib.h>

#include "event_bus.h"

TEST_CASE("event_bus: 本进程监听者收到事件") {
  ev::EventBus bus;
  std::string gotType;
  int gotSid = 0;
  bus.subscribe([&](const std::string& t, const json& d) {
    gotType = t;
    gotSid = d.value("session_id", 0);
  });
  bus.emit("chat.completed", {{"session_id", 7}, {"length", 42}});
  CHECK(gotType == "chat.completed");
  CHECK(gotSid == 7);
}

TEST_CASE("event_bus: 本进程监听者收到全部事件（过滤只发生在 Webhook 层）") {
  ev::EventBus bus;
  int calls = 0;
  bus.subscribe([&](const std::string&, const json&) { ++calls; });
  bus.emit("other.event", json::object());
  CHECK(calls == 1);
}

TEST_CASE("event_bus: webhook 分发到本地 HTTP 服务（载荷格式正确）") {
  httplib::Server s;
  std::string gotBody;
  s.Post("/hook", [&](const httplib::Request& req, httplib::Response& res) {
    gotBody = req.body;
    res.set_content("{}", "application/json");
  });
  const int port = 18900;
  std::thread t([&] { s.listen("127.0.0.1", port); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  ev::EventBus bus;
  cfg::Plugin p;
  p.name = "test-plugin";
  p.url = "http://127.0.0.1:18900/hook";
  p.events = {"chat.completed"};
  bus.loadPlugins({p});
  bus.emit("chat.completed", {{"session_id", 7}, {"length", 5}});
  std::this_thread::sleep_for(std::chrono::milliseconds(400));

  REQUIRE(!gotBody.empty());
  auto j = json::parse(gotBody);
  CHECK(j["type"] == "chat.completed");
  CHECK(j["ts"].is_string());
  CHECK(j["data"]["session_id"] == 7);

  s.stop();
  t.join();
}

TEST_CASE("event_bus: 插件未订阅的事件不分发") {
  httplib::Server s;
  int hits = 0;
  s.Post("/hook", [&](const httplib::Request&, httplib::Response& res) {
    ++hits;
    res.set_content("{}", "application/json");
  });
  const int port = 18901;
  std::thread t([&] { s.listen("127.0.0.1", port); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  ev::EventBus bus;
  cfg::Plugin p;
  p.name = "test-plugin";
  p.url = "http://127.0.0.1:18901/hook";
  p.events = {"session.created"};
  bus.loadPlugins({p});
  bus.emit("chat.completed", json::object());
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(hits == 0);

  s.stop();
  t.join();
}
