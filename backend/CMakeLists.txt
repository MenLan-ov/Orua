cmake_minimum_required(VERSION 3.20)
project(ai_companion_backend LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_options(/utf-8)

add_library(sqlite3 STATIC third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party/sqlite)

add_executable(backend
  src/main.cpp
  src/logger.cpp
)
target_include_directories(backend PRIVATE src third_party)
target_link_libraries(backend PRIVATE sqlite3 winhttp crypt32 ws2_32)
