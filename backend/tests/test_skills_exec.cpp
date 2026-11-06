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

TEST_CASE("skills_exec: isBlockedHost 主机归一化与内网判定（纯函数，不发请求）") {
  // 大小写不敏感 + FQDN 尾点
  CHECK(sk::isBlockedHost("localhost"));
  CHECK(sk::isBlockedHost("LOCALHOST"));
  CHECK(sk::isBlockedHost("LocalHost."));
  CHECK(sk::isBlockedHost("localHOST."));
  CHECK(sk::isBlockedHost("API.LOCALHOST"));
  // 内网/链路本地/未指定段
  CHECK(sk::isBlockedHost("127.0.0.1"));
  CHECK(sk::isBlockedHost("127.9.9.9"));
  CHECK(sk::isBlockedHost("10.1.2.3"));
  CHECK(sk::isBlockedHost("192.168.2.2"));
  CHECK(sk::isBlockedHost("169.254.1.1"));
  CHECK(sk::isBlockedHost("0.0.0.0"));
  CHECK(sk::isBlockedHost("0.1.2.3"));
  CHECK(sk::isBlockedHost("172.16.0.1"));
  CHECK(sk::isBlockedHost("172.31.255.255"));
  // 十进制/十六进制/八进制简写
  CHECK(sk::isBlockedHost("2130706433"));
  CHECK(sk::isBlockedHost("0x7f000001"));
  CHECK(sk::isBlockedHost("0X7F000001"));
  CHECK(sk::isBlockedHost("0177.0.0.1"));
  // IPv6 回环与 v4 映射，带/不带方括号、带端口
  CHECK(sk::isBlockedHost("::1"));
  CHECK(sk::isBlockedHost("::"));
  CHECK(sk::isBlockedHost("[::1]"));
  CHECK(sk::isBlockedHost("[::1]:8765"));
  CHECK(sk::isBlockedHost("0:0:0:0:0:0:0:1"));
  CHECK(sk::isBlockedHost("[::ffff:127.0.0.1]"));   // 带点形式解析不了 → 从严拒绝
  CHECK(sk::isBlockedHost("[::ffff:7f00:1]"));      // = ::ffff:127.0.0.1
  CHECK(sk::isBlockedHost("[::ffff:a00:1]"));       // = ::ffff:10.0.0.1
  CHECK(sk::isBlockedHost("[::]"));
  // 公网放行（这些只能走纯函数断言，测试绝不发真实网络请求）
  CHECK_FALSE(sk::isBlockedHost("example.com"));
  CHECK_FALSE(sk::isBlockedHost("EXAMPLE.COM."));
  CHECK_FALSE(sk::isBlockedHost("172.32.0.1"));
  CHECK_FALSE(sk::isBlockedHost("172.15.0.1"));
  CHECK_FALSE(sk::isBlockedHost("8.8.8.8"));
  CHECK_FALSE(sk::isBlockedHost("2606:4700:4700::1111"));
  CHECK_FALSE(sk::isBlockedHost("[::ffff:808:808]"));  // = ::ffff:8.8.8.8
  CHECK_FALSE(sk::isBlockedHost("[2606:4700:4700::1111]"));      // 公网 v6，无端口
  CHECK_FALSE(sk::isBlockedHost("[2606:4700:4700::1111]:443"));  // 公网 v6，带端口
}

TEST_CASE("skills_exec: http_get URL 层拒绝（归一化后仍拦得住）") {
  // 每个 URL 都在建连前被拒，返回 error 且不发起请求
  const char* urls[] = {
      "http://LOCALHOST/",        "http://LocalHost./",     "http://0.0.0.0/",
      "http://2130706433/",       "http://0x7f000001/",     "http://0177.0.0.1/",
      "http://[::1]/",            "http://[::1]:8765/x",    "http://::1/",
      "http://169.254.1.1/",      "http://172.16.0.1/",     "http://172.31.255.255/",
      "http://admin@127.0.0.1/",  "http://user:pass@localhost:8080/",
  };
  for (const char* u : urls) {
    auto r = sk::execute("http_get", {{"url", u}});
    INFO(u);
    CHECK(r.contains("error"));
    CHECK(r["error"].get<std::string>().find("拒绝") != std::string::npos);
    CHECK_FALSE(r.contains("body"));
  }
  // 非法/缺主机的 URL 走错误文本而非崩溃（绝不落到建连步骤）
  const char* bad[] = {"http://", "http:///path", "ftp://example.com/",
                       "http://[::1", "http://[::1]x/", ""};
  for (const char* u : bad) {
    auto r = sk::execute("http_get", {{"url", u}});
    INFO(u);
    CHECK(r.contains("error"));
    CHECK_FALSE(r.contains("body"));
  }
}

TEST_CASE("skills_exec: http_get 描述披露不跟随重定向") {
  bool found = false;
  for (auto& s : sk::registry()) {
    if (s.id != "http_get") continue;
    found = true;
    CHECK(s.description.find("不跟随重定向") != std::string::npos);
    CHECK(s.description.find("内网") != std::string::npos);
  }
  CHECK(found);
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
