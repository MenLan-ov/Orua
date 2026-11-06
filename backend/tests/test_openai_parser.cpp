#include <doctest/doctest.h>
#include <string>
#include <vector>

#include "openai_provider.h"

TEST_CASE("sse: 标准 OpenAI 流（事件边界 + [DONE]）") {
  llm::SseParser p;
  std::vector<std::pair<std::string, std::string>> got;
  p.onEvent = [&](const std::string& e, const std::string& d) {
    got.push_back({e, d});
  };
  p.feed("data: {\"choices\":[{\"delta\":{\"content\":\"你\"}}]}\n\n");
  p.feed("data: {\"choices\":[{\"delta\":{\"content\":\"好\"}}]}\n\n");
  p.feed("data: [DONE]\n\n");
  p.finish();
  REQUIRE(got.size() == 3);
  CHECK(got[0].first == "message");
  CHECK(got[0].second.find("你") != std::string::npos);
  CHECK(got[2].second == "[DONE]");
}

TEST_CASE("sse: 跨块分片的行被正确拼回") {
  llm::SseParser p;
  std::vector<std::string> datas;
  p.onEvent = [&](const std::string&, const std::string& d) { datas.push_back(d); };
  p.feed("data: {\"cho");                     // 半行
  p.feed("ices\":[{\"delta\":{\"content\":\"喵\"}}]}\n\n");
  p.finish();
  REQUIRE(datas.size() == 1);
  CHECK(datas[0].find("喵") != std::string::npos);
}

TEST_CASE("sse: 多行 data 拼接 + 自定义事件名 + 注释行") {
  llm::SseParser p;
  std::vector<std::pair<std::string, std::string>> got;
  p.onEvent = [&](const std::string& e, const std::string& d) {
    got.push_back({e, d});
  };
  p.feed(": keep-alive comment\n");
  p.feed("event: usage\ndata: {\"a\":1}\ndata: {\"b\":2}\n\n");
  p.finish();
  REQUIRE(got.size() == 1);
  CHECK(got[0].first == "usage");
  CHECK(got[0].second == "{\"a\":1}\n{\"b\":2}");
}

TEST_CASE("sse: \\r\\n 行尾与 event 字段混排") {
  llm::SseParser p;
  std::vector<std::pair<std::string, std::string>> got;
  p.onEvent = [&](const std::string& e, const std::string& d) {
    got.push_back({e, d});
  };
  p.feed("event: delta\r\ndata: {\"x\":\"y\"}\r\n\r\n");
  p.finish();
  REQUIRE(got.size() == 1);
  CHECK(got[0].first == "delta");
  CHECK(got[0].second == "{\"x\":\"y\"}");
}
