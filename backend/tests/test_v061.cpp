#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "chat_service.h"
#include "config.h"
#include "db.h"
#include "event_bus.h"

// v0.6.1：人设系统。
// 核心约束有三条：
//   1) 人设是独立实体，Bot 通过 persona_id 引用，一个人设可被多个 Bot 复用；
//   2) 绑定人设后，私聊与群聊的系统提示词都要跟着变（不能只有一处生效）；
//   3) 未绑定人设时行为必须与 v0.6.0 完全一致，旧库升级后不能静默改变说话风格。

namespace {

std::string freshDb(const std::string& tag) {
  auto path = std::filesystem::temp_directory_path() /
              ("orua_v061_" + tag + ".db");
  std::filesystem::remove(path);
  REQUIRE(db::open(path.string()));
  return path.string();
}

}  // namespace

TEST_CASE("v0.6.1 内置人设随库初始化生成") {
  auto path = freshDb("seed");
  auto ps = db::listPersonas();
  CHECK(ps.size() >= 3);
  bool hasWarm = false;
  for (auto& p : ps) {
    if (p.name == "温暖陪伴") {
      hasWarm = true;
      CHECK(p.builtin);
      CHECK_FALSE(p.role_text.empty());
    }
  }
  CHECK(hasWarm);
  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("v0.6.1 人设 CRUD 与内置保护") {
  auto path = freshDb("crud");
  db::Persona p;
  p.name = "测试人设";
  p.role_text = "# 角色：测试\n你是一个测试用的人设。";
  p.admin_rules = "保持简短。";
  p.group_display_name = "小测";
  p.participation = "moderate";
  auto saved = db::createPersona(p);
  CHECK(saved.id > 0);
  CHECK_FALSE(saved.builtin);

  auto got = db::getPersona(saved.id);
  REQUIRE(got.has_value());
  CHECK(got->name == "测试人设");
  CHECK(got->participation == "moderate");
  CHECK(got->group_display_name == "小测");

  got->name = "改名后";
  got->participation = "quiet";
  CHECK(db::updatePersona(*got));
  auto again = db::getPersona(saved.id);
  REQUIRE(again.has_value());
  CHECK(again->name == "改名后");
  CHECK(again->participation == "quiet");

  // 内置人设不可删
  int64_t builtinId = 0;
  for (auto& x : db::listPersonas())
    if (x.builtin) { builtinId = x.id; break; }
  CHECK(builtinId > 0);
  CHECK_FALSE(db::deletePersona(builtinId));

  CHECK(db::deletePersona(saved.id));
  CHECK_FALSE(db::getPersona(saved.id).has_value());

  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("v0.6.1 Bot 绑定人设后提示词跟随") {
  auto path = freshDb("bind");
  db::Persona p;
  p.name = "猫娘";
  p.role_text = "# 角色：猫娘\n你说话句尾会带「喵」。";
  p.admin_rules = "不要过度卖萌。";
  auto persona = db::createPersona(p);

  db::Bot b;
  b.name = "测试Bot";
  b.system_prompt = "你是一个助手。";
  b = db::createBot(b);
  CHECK(b.persona_id == 0);   // 新建默认未绑定

  // 未绑定时提示词不变——这是升级兼容性的关键
  CHECK(db::applyPersonaToPrompt(b.system_prompt, 0) == b.system_prompt);

  CHECK(db::setBotPersona(b.id, persona.id));
  auto reloaded = db::getBot(b.id);
  REQUIRE(reloaded.has_value());
  CHECK(reloaded->persona_id == persona.id);

  std::string merged = db::applyPersonaToPrompt(reloaded->system_prompt,
                                                reloaded->persona_id);
  CHECK(merged.find("猫娘") != std::string::npos);
  CHECK(merged.find("喵") != std::string::npos);
  CHECK(merged.find("[附加规则]") != std::string::npos);
  // Bot 自己的提示词必须保留，否则人设会把用户对人物的具体交代吃掉
  CHECK(merged.find("你是一个助手。") != std::string::npos);

  // 解绑后回到原样
  CHECK(db::setBotPersona(b.id, 0));
  auto unbound = db::getBot(b.id);
  REQUIRE(unbound.has_value());
  CHECK(unbound->persona_id == 0);
  CHECK(db::applyPersonaToPrompt(unbound->system_prompt, 0) ==
        unbound->system_prompt);

  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("v0.6.1 人设系统提示覆盖优先于拼接") {
  auto path = freshDb("override");
  db::Persona p;
  p.name = "覆盖型";
  p.role_text = "这段不应出现在结果里。";
  p.admin_rules = "这段也不应出现。";
  p.system_override = "完全替换后的系统提示。";
  auto persona = db::createPersona(p);

  std::string out = db::applyPersonaToPrompt("原始提示", persona.id);
  CHECK(out == "完全替换后的系统提示。");
  CHECK(out.find("这段不应出现在结果里") == std::string::npos);

  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("v0.6.1 删除人设会解绑引用它的 Bot") {
  auto path = freshDb("unbind");
  db::Persona p;
  p.name = "临时人设";
  p.role_text = "临时。";
  auto persona = db::createPersona(p);

  db::Bot b;
  b.name = "引用者";
  b.system_prompt = "提示";
  b.persona_id = persona.id;
  b = db::createBot(b);
  CHECK(b.persona_id == persona.id);

  CHECK(db::deletePersona(persona.id));
  auto after = db::getBot(b.id);
  REQUIRE(after.has_value());
  // 关键：不能留下指向不存在人设的悬空引用
  CHECK(after->persona_id == 0);

  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("v0.6.1 人设内容全空时退回原提示词") {
  auto path = freshDb("empty");
  db::Persona p;
  p.name = "空人设";
  auto persona = db::createPersona(p);

  std::string out = db::applyPersonaToPrompt("原提示", persona.id);
  CHECK(out == "原提示");   // 不能变成空串，否则系统提示丢失

  db::close();
  std::filesystem::remove(path);
}

// 用户对本版最核心的要求是「Bot 会跟着人设和记忆走」。
// 上面验证了人设拼接，这里再验证「人设 + 记忆」同时存在时的最终上下文顺序：
//   人设/系统提示 → [长期记忆] → 历史 → 本轮用户输入
// 顺序错了会让记忆盖过人设，或让记忆被历史淹没。
TEST_CASE("v0.6.1 人设与记忆在上下文中同时生效") {
  auto path = freshDb("ctx");
  db::Persona p;
  p.name = "记忆测试人设";
  p.role_text = "【人设标记】你是测试角色。";
  auto persona = db::createPersona(p);

  std::string sys = db::applyPersonaToPrompt("原始系统提示", persona.id);
  CHECK(sys.find("【人设标记】") != std::string::npos);

  std::vector<db::Message> history;
  db::Message m1;
  m1.role = "user";
  m1.content = "历史用户消息";
  history.push_back(m1);
  db::Message m2;
  m2.role = "assistant";
  m2.content = "历史助手消息";
  history.push_back(m2);

  std::vector<db::Memory> mems;
  db::Memory mem;
  mem.content = "用户喜欢喝美式";
  mems.push_back(mem);

  auto ctx = chat::buildContext(sys, history, 0, "本轮新消息", mems);
  REQUIRE(ctx.size() >= 4);

  // 第 1 条：人设 + 系统提示
  CHECK(ctx[0].role == "system");
  CHECK(ctx[0].content.find("【人设标记】") != std::string::npos);
  CHECK(ctx[0].content.find("原始系统提示") != std::string::npos);

  // 第 2 条：长期记忆块，且必须排在历史之前
  CHECK(ctx[1].role == "system");
  CHECK(ctx[1].content.find("[长期记忆]") != std::string::npos);
  CHECK(ctx[1].content.find("用户喜欢喝美式") != std::string::npos);

  // 末尾：本轮用户输入
  CHECK(ctx.back().role == "user");
  CHECK(ctx.back().content == "本轮新消息");

  // 记忆不能跑到人设前面——那会让人设的「我是谁」被记忆细节冲淡
  CHECK(ctx[0].content.find("[长期记忆]") == std::string::npos);

  db::close();
  std::filesystem::remove(path);
}
