#include <doctest/doctest.h>

#include "chat_service.h"
#include "json_util.h"

namespace {
db::Message msg(int64_t id, const std::string& role) {
  db::Message m;
  m.id = id;
  m.role = role;
  m.content = "content-" + std::to_string(id);
  return m;
}
}  // namespace

TEST_CASE("chat: 空历史 = system + 新 user") {
  auto ctx = chat::buildContext("你是测试助手", {}, 20, "你好");
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
  auto ctx = chat::buildContext("你是测试助手", h, 2, "新问题");
  // 期望: system + u2 a2 u3 a3 + 新 user = 6 条
  REQUIRE(ctx.size() == 6);
  CHECK(ctx[1].content == "content-3");
  CHECK(ctx[4].content == "content-6");
  CHECK(ctx[5].role == "user");
  CHECK(ctx[5].content == "新问题");
}

TEST_CASE("chat: keep_rounds=0 表示不限轮数") {
  std::vector<db::Message> h = {msg(1, "user"), msg(2, "assistant")};
  auto ctx = chat::buildContext("你是测试助手", h, 0, "再聊");
  REQUIRE(ctx.size() == 4);
  CHECK(ctx[1].content == "content-1");
}

TEST_CASE("chat: keep_rounds 大于实际轮数时不截断") {
  std::vector<db::Message> h = {msg(1, "user"), msg(2, "assistant")};
  auto ctx = chat::buildContext("你是测试助手", h, 10, "hi");
  REQUIRE(ctx.size() == 4);
}

TEST_CASE("chat: 历史中的 system 角色消息被跳过") {
  std::vector<db::Message> h = {msg(1, "system"), msg(2, "user"), msg(3, "assistant")};
  auto ctx = chat::buildContext("你是测试助手", h, 10, "hi");
  REQUIRE(ctx.size() == 4);  // system(人设) + user + assistant + 新user
  CHECK(ctx[0].content == "你是测试助手");
  CHECK(ctx[1].content == "content-2");
}

TEST_CASE("util: utf8Truncate 不截断中文中间") {
  CHECK(utf8Truncate("你好世界", 2) == "你好");
  CHECK(utf8Truncate("abc", 2) == "ab");
  CHECK(utf8Truncate("你好", 10) == "你好");
}

TEST_CASE("chat: newUser 为空时不追加（regenerate 场景）") {
  std::vector<db::Message> h = {msg(1, "user"), msg(2, "assistant")};
  auto ctx = chat::buildContext("你是测试助手", h, 10, "");
  REQUIRE(ctx.size() == 3);  // system + user + assistant，不重复追加 user
  CHECK(ctx[2].content == "content-2");
}

TEST_CASE("chat_service: 记忆块注入（system prompt 后、历史前）") {
  std::vector<db::Message> hist = {{1, 1, "user", "你好", ""}, {2, 1, "assistant", "你好呀", ""}};
  std::vector<db::Memory> mems = {
      {1, 3, "private", 0, "用户喜欢咖啡", "[\"咖啡\"]", 0, "", ""},
      {2, 3, "private", 0, "用户生日五月", "[\"生日\"]", 0, "", ""}};
  auto ctx = chat::buildContext("你是助手", hist, 20, "今天喝什么", mems);
  REQUIRE(ctx.size() == 5);   // system + 记忆块 + user + assistant + 新 user
  CHECK(ctx[0].role == "system");
  CHECK(ctx[0].content == "你是助手");
  CHECK(ctx[1].role == "system");
  CHECK(ctx[1].content.find("[长期记忆]") == 0);
  CHECK(ctx[1].content.find("· 用户喜欢咖啡") != std::string::npos);
  CHECK(ctx[1].content.find("· 用户生日五月") != std::string::npos);
  CHECK(ctx[2].content == "你好");
  CHECK(ctx[4].content == "今天喝什么");
}

TEST_CASE("chat_service: 空记忆 = v2 原行为") {
  std::vector<db::Message> hist = {{1, 1, "user", "你好", ""}};
  auto ctx = chat::buildContext("你是助手", hist, 20, "", {});
  REQUIRE(ctx.size() == 2);
  CHECK(ctx[1].content == "你好");
}
