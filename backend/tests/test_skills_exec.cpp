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
  }
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
