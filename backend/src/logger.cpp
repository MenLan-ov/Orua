#include "logger.h"
#include <windows.h>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace logx {
namespace {
std::ofstream g_file;
std::mutex g_mtx;
constexpr int kMaxFiles = 5;
constexpr long long kMaxSize = 2LL * 1024 * 1024;

std::string stamp() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &t);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
  return buf;
}

void rotate(const std::filesystem::path& base) {
  for (int i = kMaxFiles - 1; i >= 1; --i) {
    auto from = base.string() + (i == 1 ? "" : "." + std::to_string(i - 1));
    auto to = base.string() + "." + std::to_string(i);
    std::error_code ec;
    std::filesystem::remove(to, ec);
    std::filesystem::rename(from, to, ec);
  }
}

void write(const std::string& level, const std::string& msg) {
  std::lock_guard<std::mutex> lock(g_mtx);
  std::string line = "[" + stamp() + "] [" + level + "] " + msg + "\n";
  if (g_file.is_open()) g_file << line << std::flush;
  OutputDebugStringA(line.c_str());
}
}  // namespace

bool init(const std::string& logDir) {
  std::lock_guard<std::mutex> lock(g_mtx);
  try {
    std::filesystem::create_directories(logDir);
    auto base = std::filesystem::path(logDir) / "backend.log";
    if (std::filesystem::exists(base) &&
        std::filesystem::file_size(base) > kMaxSize) {
      g_file.close();
      rotate(base);
    }
    g_file.open(base, std::ios::app);
    return g_file.is_open();
  } catch (...) {
    return false;
  }
}

void info(const std::string& msg) { write("INFO", msg); }
void warn(const std::string& msg) { write("WARN", msg); }
void error(const std::string& msg) { write("ERROR", msg); }
}  // namespace logx
