#include <doctest/doctest.h>

#include "chat_service.h"
#include "json_util.h"

namespace {
db::Persona makePersona() {
  db::Persona p;
  p.name = "测试";
  p.system_prompt = "你是测试助手";
  return p;
}
db::Message msg(int64_t id, const std::string& role) {
  db::Message m;
  m.id = id;
  m.role = role;
  m.content = "content-" + std::to_string(id);
  return m;
}
}  // namespace

TEST_CASE("chat: 空历史 = system + 新 user") {
  auto ctx = chat::buildContext(makePersona(), {}, 20, "你好");
  REQUIRE(ctx.size() == 2);
  CHECK(ctx[0].role == "system");
  CHECK(ctx[0].content == "你是测试助手");
  CHECK(ctx[1].role == "user");
  CHECK(ctx[1].content == "你好");
}

TEST_CASE("chat: N 轮截断保留最后 keep_rounds 轮") {
  // 历史: u1 a1 u2 a2 u3 a3（3 轮）
  std::vector<db::Message> h = {msg(1, "user"),    msg(2, "assistant"),
                                msg(3, "user"),    msg(4, "assistant"),
                                msg(5, "user"),    msg(6, "assistant")};
  auto ctx = chat::buildContext(makePersona(), h, 2, "新问题");
  // 期望: system + u2 a2 u3 a3 + 新 user = 6 条
  REQUIRE(ctx.size() == 6);
  CHECK(ctx[1].content == "content-3");
  CHECK(ctx[4].content == "content-6");
  CHECK(ctx[5].role == "user");
  CHECK(ctx[5].content == "新问题");
}

TEST_CASE("chat: keep_rounds=0 表示不限轮数") {
  std::vector<db::Message> h = {msg(1, "user"), msg(2, "assistant")};
  auto ctx = chat::buildContext(makePersona(), h, 0, "再聊");
  REQUIRE(ctx.size() == 4);
  CHECK(ctx[1].content == "content-1");
}

TEST_CASE("chat: keep_rounds 大于实际轮数时不截断") {
  std::vector<db::Message> h = {msg(1, "user"), msg(2, "assistant")};
  auto ctx = chat::buildContext(makePersona(), h, 10, "hi");
  REQUIRE(ctx.size() == 4);
}

TEST_CASE("chat: 历史中的 system 角色消息被跳过") {
  std::vector<db::Message> h = {msg(1, "system"), msg(2, "user"), msg(3, "assistant")};
  auto ctx = chat::buildContext(makePersona(), h, 10, "hi");
  REQUIRE(ctx.size() == 4);  // system(人设) + user + assistant + 新user
  CHECK(ctx[0].content == "你是测试助手");
  CHECK(ctx[1].content == "content-2");
}

TEST_CASE("util: utf8Truncate 不截断中文中间") {
  CHECK(utf8Truncate("你好世界", 2) == "你好");
  CHECK(utf8Truncate("abc", 2) == "ab");
  CHECK(utf8Truncate("你好", 10) == "你好");
}
