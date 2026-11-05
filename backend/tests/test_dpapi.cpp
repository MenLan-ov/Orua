#include <doctest/doctest.h>
#include <stdexcept>
#include <string>

#include "dpapi_util.h"

TEST_CASE("dpapi: 加解密往返") {
  std::string key = "sk-test-abcdef1234567890-中文也支持";
  auto enc = dpapi::protect(key);
  REQUIRE(!enc.empty());
  CHECK(enc != key);              // 密文不等于明文
  CHECK(dpapi::unprotect(enc) == key);
}

TEST_CASE("dpapi: 空串不加密") {
  CHECK(dpapi::protect("").empty());
}

TEST_CASE("dpapi: 非法密文抛异常") {
  CHECK_THROWS_AS(dpapi::unprotect("!!!!not-base64"), std::runtime_error);
}
