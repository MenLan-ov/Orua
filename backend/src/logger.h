#pragma once
#include <string>

namespace logx {
bool init(const std::string& logDir);
void info(const std::string& msg);
void warn(const std::string& msg);
void error(const std::string& msg);
}
