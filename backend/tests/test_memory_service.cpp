#include <doctest/doctest.h>
#include <cstdlib>
#include <filesystem>
#include <thread>

#include "db.h"
#include "memory_service.h"

namespace {
std::filesystem::path tmpDb(const char* name) {
  auto p = std::filesystem::temp_directory_path() / name;
  std::filesystem::remove(p);
  return p;
}

void waitUntil(const std::function<bool()>& cond, int timeoutMs = 8000) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (cond()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  REQUIRE(cond());
}
}  // namespace

TEST_CASE("memory_service: LLM_MOCK 提取两条固定记忆") {
  _putenv_s("LLM_MOCK", "1");
  auto path = tmpDb("ac_v3_ms_extract.db");
  REQUIRE(db::open(path.string()));
  auto b = db::createBot(db::Bot{0, "测试", "🤖", "", "人设"});

  mem::MemoryService::instance().enqueueExtract(
      b.id, "private", 0, "用户：我喜欢喝咖啡\n助手：好的我记住了");
  waitUntil([&] { return db::listMemories(b.id).size() >= 2; });

  auto mems = db::listMemories(b.id);
  REQUIRE(mems.size() == 2);
  CHECK(mems[0].scope == "private");
  bool hasCoffee = false;
  for (auto& m : mems) if (m.content.find("咖啡") != std::string::npos) hasCoffee = true;
  CHECK(hasCoffee);

  // 再次提取：update 占位合并，不产生第三条
  mem::MemoryService::instance().enqueueExtract(b.id, "private", 0, "用户：继续聊\n助手：好的");
  waitUntil([&] { return db::listMemories(b.id).size() >= 2; });
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(db::listMemories(b.id).size() == 2);

  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("memory_service: group scope 提取落 (bot,group) 维度") {
  _putenv_s("LLM_MOCK", "1");
  auto path = tmpDb("ac_v3_ms_group.db");
  REQUIRE(db::open(path.string()));
  auto b = db::createBot(db::Bot{0, "测试", "🤖", "", "人设"});
  auto g = db::createGroup("测试群", "");

  mem::MemoryService::instance().enqueueExtract(b.id, "group", g.id, "测试：大家好");
  waitUntil([&] { return db::listMemories(b.id, "group", g.id).size() >= 2; });
  CHECK(db::listMemories(b.id, "private").empty());

  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("memory_service: pickMemories scope 隔离与关键词召回") {
  _putenv_s("LLM_MOCK", "1");
  auto path = tmpDb("ac_v3_ms_pick.db");
  REQUIRE(db::open(path.string()));
  auto b = db::createBot(db::Bot{0, "测试", "🤖", "", "人设"});
  db::addMemory(b.id, "private", 0, "用户喜欢海边", "[\"海边\"]", 0);
  db::addMemory(b.id, "group", 7, "群里讨论过海边", "[\"海边\"]", 0);

  auto priv = mem::MemoryService::instance().pickMemories(b.id, "private", 0, "我想去海边玩", 5);
  REQUIRE(priv.size() == 1);
  CHECK(priv[0].content == "用户喜欢海边");
  auto grp = mem::MemoryService::instance().pickMemories(b.id, "group", 7, "我们聊聊海边", 5);
  REQUIRE(grp.size() == 1);
  CHECK(grp[0].group_id == 7);
  // 无关键词命中 → 空（退化纯窗口行为）
  CHECK(mem::MemoryService::instance()
            .pickMemories(b.id, "private", 0, "xyzzy", 5)
            .empty());

  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("memory_service: 提取开关关闭后不新增") {
  _putenv_s("LLM_MOCK", "1");
  auto path = tmpDb("ac_v3_ms_off.db");
  REQUIRE(db::open(path.string()));
  db::setSetting("memory_extraction", "0");
  auto b = db::createBot(db::Bot{0, "测试", "🤖", "", "人设"});

  mem::MemoryService::instance().enqueueExtract(b.id, "private", 0, "用户：你好");
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  CHECK(db::listMemories(b.id).empty());

  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("memory_service: LLM 不可达时静默失败不崩溃") {
  _putenv_s("LLM_MOCK", "");
  auto path = tmpDb("ac_v3_ms_fail.db");
  REQUIRE(db::open(path.string()));
  auto b = db::createBot(db::Bot{0, "测试", "🤖", "", "人设"});
  b.api_base_url = "http://127.0.0.1:1/v1";   // 必然连接失败
  b.api_model = "x";
  REQUIRE(db::updateBot(b));

  mem::MemoryService::instance().enqueueExtract(b.id, "private", 0, "用户：你好");
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  CHECK(db::listMemories(b.id).empty());   // 不落库、不崩溃
  // pickMemories 阶段 2 失败 → 降级阶段 1 前 maxK
  db::addMemory(b.id, "private", 0, "用户喜欢咖啡", "[\"咖啡\"]", 0);
  auto pick = mem::MemoryService::instance().pickMemories(b.id, "private", 0, "咖啡", 5);
  REQUIRE(pick.size() == 1);

  db::close();
  std::filesystem::remove(path);
}
