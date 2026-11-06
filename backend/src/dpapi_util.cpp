#include "dpapi_util.h"

#include <windows.h>
#include <dpapi.h>

#include <stdexcept>
#include <vector>

namespace dpapi {
namespace {

const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string toB64(const std::vector<unsigned char>& in) {
  std::string out;
  size_t i = 0;
  while (i + 3 <= in.size()) {
    unsigned n = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
    out += kB64[(n >> 18) & 63];
    out += kB64[(n >> 12) & 63];
    out += kB64[(n >> 6) & 63];
    out += kB64[n & 63];
    i += 3;
  }
  size_t rem = in.size() - i;
  if (rem == 1) {
    unsigned n = in[i] << 16;
    out += kB64[(n >> 18) & 63];
    out += kB64[(n >> 12) & 63];
    out += "==";
  } else if (rem == 2) {
    unsigned n = (in[i] << 16) | (in[i + 1] << 8);
    out += kB64[(n >> 18) & 63];
    out += kB64[(n >> 12) & 63];
    out += kB64[(n >> 6) & 63];
    out += '=';
  }
  return out;
}

int val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

std::vector<unsigned char> fromB64(const std::string& in) {
  std::vector<unsigned char> out;
  unsigned acc = 0;
  int bits = 0;
  for (char ch : in) {
    if (ch == '=') break;
    int v = val(ch);
    if (v < 0) continue;
    acc = (acc << 6) | (unsigned)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back((acc >> bits) & 0xFF);
    }
  }
  return out;
}

std::vector<unsigned char> crypt(const std::vector<unsigned char>& in,
                                 bool protectMode) {
  DATA_BLOB src{static_cast<DWORD>(in.size()),
                const_cast<BYTE*>(in.data())};
  DATA_BLOB dst{0, nullptr};
  BOOL ok = protectMode
                ? CryptProtectData(&src, L"AICompanion", nullptr, nullptr,
                                   nullptr, CRYPTPROTECT_UI_FORBIDDEN, &dst)
                : CryptUnprotectData(&src, nullptr, nullptr, nullptr, nullptr,
                                     CRYPTPROTECT_UI_FORBIDDEN, &dst);
  if (!ok) throw std::runtime_error(protectMode ? "CryptProtectData failed"
                                                : "CryptUnprotectData failed");
  std::vector<unsigned char> out(dst.pbData, dst.pbData + dst.cbData);
  LocalFree(dst.pbData);
  return out;
}

}  // namespace

std::string protect(const std::string& plain) {
  if (plain.empty()) return "";
  std::vector<unsigned char> in(plain.begin(), plain.end());
  return toB64(crypt(in, true));
}

std::string unprotect(const std::string& b64) {
  auto raw = crypt(fromB64(b64), false);
  return std::string(raw.begin(), raw.end());
}

}  // namespace dpapi
