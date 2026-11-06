#pragma once
#include <string>

#include <httplib.h>

namespace srv {
void registerRoutes(httplib::Server& svr, const std::string& staticDir);
}  // namespace srv
