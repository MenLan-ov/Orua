#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>

#include "db.h"
#include "skills.h"

namespace {
std::filesystem::path tmpDb(const char* name) {
  auto p = std::filesystem::temp_directory_path() / name;
  std::filesystem::remove(p);
  return p;
}
}  // namespace

TEST_CASE("skills_exec: registry 六技能") {
  auto reg = sk::registry();
  REQUIRE(reg.size() == 6);
  CHECK(reg[0].id == "calculator");
  CHECK(reg[1].id == "datetime");
  CHECK(reg[2].id == "sysinfo");
  CHECK(reg[3].id == "http_get");
  CHECK(reg[4].id == "file_read");
  CHECK(reg[5].id == "reminder");
  for (auto& s : reg) {
    CHECK(!s.name.empty());
    CHECK(!s.description.empty());
    CHECK(s.params_schema.is_object());
    REQUIRE(s.params_schema["properties"].is_object());
    if (s.params_schema["properties"].is_object())
      for (auto& [k, v] : s.params_schema["properties"].items()) CHECK(v.is_object());
  }
}

TEST_CASE("skills_exec: execute 非对象/错型参数不抛异常") {
  CHECK(!sk::execute("calculator", json::array()).contains("result"));
  CHECK(sk::execute("calculator", {{"expression", 123}}).contains("error"));
}

TEST_CASE("skills_exec: calculator 四则运算与非法输入") {
  CHECK(sk::execute("calculator", {{"expression", "1+2*3"}})["result"] == 7);
  CHECK(sk::execute("calculator", {{"expression", "(1+2)*3"}})["result"] == 9);
  CHECK(sk::execute("calculator", {{"expression", "10/4"}})["result"] == 2.5);
  CHECK(sk::execute("calculator", {{"expression", "2*(3+4)-5"}})["result"] == 9);
  CHECK(sk::execute("calculator", {{"expression", "1+abc"}}).contains("error"));
  CHECK(sk::execute("calculator", {{"expression", "1/0"}}).contains("error"));
  CHECK(sk::execute("calculator", {{"expression", ""}}).contains("error"));
}

TEST_CASE("skills_exec: datetime 与 sysinfo 形状") {
  auto dt = sk::execute("datetime", json::object());
  CHECK(dt.contains("now"));
  CHECK(dt.contains("date"));
  CHECK(dt.contains("weekday"));

  auto si = sk::execute("sysinfo", json::object());
  CHECK(si.contains("memory_total_mb"));
  CHECK(si.contains("memory_free_mb"));
  CHECK(si.contains("disk_free_gb"));
  CHECK(si["memory_total_mb"].is_number());
}

TEST_CASE("skills_exec: http_get 拒绝内网地址") {
  auto r1 = sk::execute("http_get", {{"url", "http://127.0.0.1:8765/health"}});
  CHECK(r1.contains("error"));
  CHECK(r1["error"].get<std::string>().find("拒绝") != std::string::npos);
  auto r2 = sk::execute("http_get", {{"url", "http://localhost/"}});
  CHECK(r2.contains("error"));
  auto r3 = sk::execute("http_get", {{"url", "http://192.168.1.1/"}});
  CHECK(r3.contains("error"));
  auto r4 = sk::execute("http_get", {{"url", "http://10.0.0.1/"}});
  CHECK(r4.contains("error"));
}

TEST_CASE("skills_exec: file_read 截断与错误") {
  auto big = std::filesystem::temp_directory_path() / "ac_v4_skill_big.txt";
  {
    std::ofstream ofs(big, std::ios::binary);
    std::string chunk(1000, 'A');
    for (int i = 0; i < 10; ++i) ofs << chunk;   // 10000 字节
  }
  auto r1 = sk::execute("file_read", {{"path", big.string()}});
  REQUIRE(r1.contains("content"));
  CHECK(r1["content"].get<std::string>().size() == 8192);
  std::filesystem::remove(big);

  auto r2 = sk::execute("file_read", {{"path", "Z:/不存在/文件.txt"}});
  CHECK(r2.contains("error"));
  auto r3 = sk::execute("file_read", {{"path", ""}});
  CHECK(r3.contains("error"));
}

TEST_CASE("skills_exec: 非法 UTF-8 结果按替换策略序列化（回填不中断对话）") {
  // 构造含非法 UTF-8 字节的临时文件：file_read 裸 append 字节 + 8192 硬切，必然产出非法序列
  auto p = std::filesystem::temp_directory_path() / "ac_v4_skill_badutf.bin";
  {
    std::ofstream ofs(p, std::ios::binary);
    ofs.put('\xFF'); ofs.put('\xFE'); ofs.put('\x00'); ofs.put('\x41');
    ofs.put('\xE4'); ofs.put('\xBD'); ofs.put('\xA0');   // 合法的「你」
    ofs.put('\xE0'); ofs.put('\xA0');                    // 被截断的三字节序列
  }
  json r;
  REQUIRE_NOTHROW(r = sk::execute("file_read", {{"path", p.string()}}));
  REQUIRE(r.contains("content"));
  CHECK_FALSE(r["content"].get<std::string>().empty());

  // 严格模式确实抛 type_error.316 —— 证明下面那条替换策略断言有鉴别力，不是空断言
  CHECK_THROWS_AS(r.dump(), json::type_error);

  // server.cpp 工具回填用的序列化方式：不抛、非空、可被再次解析回 JSON 对象
  std::string payload;
  REQUIRE_NOTHROW(payload = r.dump(-1, ' ', false, json::error_handler_t::replace));
  CHECK(payload.size() > 2);
  json back;
  REQUIRE_NOTHROW(back = json::parse(payload));
  CHECK(back.contains("content"));
  CHECK(back["content"].is_string());

  std::filesystem::remove(p);
}

TEST_CASE("skills_exec: reminder 写入数据库") {
  auto path = tmpDb("ac_v4_skill_reminder.db");
  REQUIRE(db::open(path.string()));
  auto r = sk::execute("reminder", {{"content", "测试提醒"}, {"due_at", "2099-01-01T00:00:00Z"}});
  CHECK(r.value("ok", false) == true);
  CHECK(r["id"].is_number());
  auto list = db::listReminders(false);
  REQUIRE(list.size() == 1);
  CHECK(list[0].content == "测试提醒");
  db::close();
  std::filesystem::remove(path);
}
