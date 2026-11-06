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

TEST_CASE("skills: normalizeDueAt 绝对形式（含时区与本地墙钟）") {
  // 显式 UTC：原样
  CHECK(sk::normalizeDueAt("2026-12-01T10:00:00Z") == "2026-12-01T10:00:00Z");
  CHECK(sk::normalizeDueAt("2026-12-01T10:00:00.250Z") == "2026-12-01T10:00:00Z");
  CHECK(sk::normalizeDueAt("2026-12-01t10:00:00z") == "2026-12-01T10:00:00Z");
  CHECK(sk::normalizeDueAt("2026-12-01T10:00Z") == "2026-12-01T10:00:00Z");   // 分钟精度补秒
  // 显式偏移：换算成 UTC
  CHECK(sk::normalizeDueAt("2026-12-01T10:00:00+08:00") == "2026-12-01T02:00:00Z");
  CHECK(sk::normalizeDueAt("2026-12-01T10:00:00+0800") == "2026-12-01T02:00:00Z");
  CHECK(sk::normalizeDueAt("2026-12-01T02:00:00-05:00") == "2026-12-01T07:00:00Z");
  // 无时区标记（空格分隔）：按本地墙钟解释，再转 UTC —— 与 CRT 同算法比对，不受机器时区影响
  {
    std::tm local{};
    local.tm_year = 2026 - 1900; local.tm_mon = 11; local.tm_mday = 1;
    local.tm_hour = 10; local.tm_min = 0; local.tm_sec = 0; local.tm_isdst = -1;
    std::time_t e = std::mktime(&local);
    REQUIRE(e != static_cast<std::time_t>(-1));
    std::tm utc{};
    REQUIRE(gmtime_s(&utc, &e) == 0);
    char want[32];
    REQUIRE(std::strftime(want, sizeof(want), "%Y-%m-%dT%H:%M:%SZ", &utc) > 0);
    auto got = sk::normalizeDueAt("2026-12-01 10:00:00");
    CHECK(got.size() == 20);
    CHECK(got.back() == 'Z');
    CHECK(got == std::string(want));
    // 输出必须与到期过滤用的 nowIso() 逐字符兼容
    CHECK(sk::normalizeDueAt(nowIso()) == nowIso());
  }
  // 字符串比较定位：过去/未来在 due_at <= nowIso() 语义下站得住
  CHECK(sk::normalizeDueAt("2000-01-01T00:00:00Z") < nowIso());
  CHECK(sk::normalizeDueAt("2099-01-01T00:00:00Z") > nowIso());
}

TEST_CASE("skills: normalizeDueAt 相对形式与失败形式") {
  auto lo = sk::normalizeDueAt("14分钟后");
  auto hi = sk::normalizeDueAt("16分钟后");
  REQUIRE(lo.size() == 20);
  REQUIRE(hi.size() == 20);
  // 相对分钟族：都落在 now+15min 附近（用 14/16 分钟边界卡住，能鉴别秒/小时误读）
  const char* minForms[] = {"15分钟后", "15 分钟后", "in 15 minutes", "+15m", "15",
                            "15min", "15 分钟"};
  for (const char* s : minForms) {
    INFO(s);
    auto got = sk::normalizeDueAt(s);
    CHECK_FALSE(got.empty());
    CHECK(got > lo);
    CHECK(got < hi);
  }
  // 小时族
  auto hLo = sk::normalizeDueAt("59分钟后");
  auto hHi = sk::normalizeDueAt("61分钟后");
  const char* hourForms[] = {"1小时后", "1 小时后", "1h", "+1h", "in 1 hours"};
  for (const char* s : hourForms) {
    INFO(s);
    auto got = sk::normalizeDueAt(s);
    CHECK(got > hLo);
    CHECK(got < hHi);
  }
  // 过去写法 = 已到期（早于 now）
  CHECK(sk::normalizeDueAt("5分钟前") < nowIso());
  CHECK(sk::normalizeDueAt("2小时前") < nowIso());
  // 失败形式一律空串
  const char* badForms[] = {"垃圾", "", "   ", "15秒后", "15天后", "abc15", "in 15 分钟后 多余",
                            "2026-13-01T00:00:00Z", "2026-02-30", "1969-12-31T23:59:59Z",
                            "2026-12-01T25:00:00Z", "2100-01-01T00:00:00Z", "12345678",
                            "2026-12-01T10:00:00+99:00"};
  for (const char* s : badForms) {
    INFO(s);
    CHECK(sk::normalizeDueAt(s).empty());
  }
}

TEST_CASE("skills: 归一化 due_at 与到期过滤器的字符串比较契约") {
  auto path = tmpDb("ac_v4_due_contract.db");
  REQUIRE(db::open(path.string()));
  auto b = db::createBot(db::Bot{0, "契约", "🤖", "", "人设"});
  REQUIRE(b.id > 0);
  auto fut = db::addReminder(b.id, "未到期", sk::normalizeDueAt("1分钟后"));
  auto past = db::addReminder(b.id, "已到期", sk::normalizeDueAt("1分钟前"));
  REQUIRE(fut.id > 0);
  REQUIRE(past.id > 0);
  auto due = db::listReminders(true);
  REQUIRE(due.size() == 1);
  CHECK(due[0].content == "已到期");          // 未来查不到、过去查得到
  CHECK(due[0].due_at.size() == 20);
  // 反面证据：旧行为把 LLM 原文直接入库时，"10分钟后" 因字符串比 nowIso() 小而「立刻到期」
  db::addReminder(b.id, "脏数据", "10分钟后");
  CHECK(db::listReminders(true).size() == 2);
  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("skills_exec: reminder 归属 Bot（bot_id 链路）与 deleteBot 级联") {
  auto path = tmpDb("ac_v4_skill_reminder_bot.db");
  REQUIRE(db::open(path.string()));
  auto keeper = db::createBot(db::Bot{0, "默认", "🤖", "", "人设"});   // 首个 Bot 为默认，不可删
  auto victim = db::createBot(db::Bot{0, "待删", "🤖", "", "人设"});
  REQUIRE(keeper.id > 0);
  REQUIRE(victim.id > 0);
  auto r = sk::execute("reminder", {{"content", "级联测试"},
                                    {"due_at", "2099-01-01T00:00:00Z"},
                                    {"bot_id", victim.id}});
  REQUIRE(r.value("ok", false) == true);
  auto all = db::listReminders(false);
  REQUIRE(all.size() == 1);
  CHECK(all[0].bot_id == victim.id);                    // 不再是原先的 bot_id=0
  CHECK(all[0].due_at == "2099-01-01T00:00:00Z");
  CHECK(db::deleteBot(victim.id));
  CHECK(db::listReminders(false).empty());              // deleteBot 级联在真实路径生效
  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("skills_exec: reminder 无法解析 due_at 时不写库") {
  auto path = tmpDb("ac_v4_skill_reminder_bad.db");
  REQUIRE(db::open(path.string()));
  auto r = sk::execute("reminder", {{"content", "坏时间"}, {"due_at", "明天见"}});
  CHECK(r.contains("error"));
  CHECK_FALSE(r.contains("ok"));
  CHECK(r["error"].get<std::string>().find("due_at") != std::string::npos);
  CHECK(db::listReminders(false).empty());              // 失败绝不写库
  // 整数 due_at 等价于「N 分钟」，且入库值是归一化后的 UTC ISO
  auto num = sk::execute("reminder", {{"content", "数字分钟"}, {"due_at", 30}});
  CHECK(num.value("ok", false) == true);
  auto list = db::listReminders(false);
  REQUIRE(list.size() == 1);
  CHECK(list[0].due_at.size() == 20);
  CHECK(list[0].due_at > nowIso());
  CHECK(num.value("due_at", "") == list[0].due_at);
  db::close();
  std::filesystem::remove(path);
}

TEST_CASE("skills_exec: reminder 注册表描述披露相对分钟") {
  bool found = false;
  for (auto& s : sk::registry()) {
    if (s.id != "reminder") continue;
    found = true;
    auto desc = s.params_schema["properties"]["due_at"]["description"].get<std::string>();
    CHECK(desc.find("ISO 8601") != std::string::npos);
    CHECK(desc.find("相对分钟") != std::string::npos);
  }
  CHECK(found);
}
