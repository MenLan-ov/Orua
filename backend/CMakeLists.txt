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
  src/db.cpp
  src/config.cpp
  src/dpapi_util.cpp
  src/openai_provider.cpp
  src/chat_service.cpp
  src/event_bus.cpp
  src/group_engine.cpp
  src/memory_service.cpp
  src/skills.cpp
  src/server.cpp
)
target_include_directories(backend PRIVATE src third_party)
target_link_libraries(backend PRIVATE sqlite3 winhttp crypt32 ws2_32)

enable_testing()

add_executable(backend_tests
  tests/test_main.cpp
  tests/test_db.cpp
  tests/test_bots.cpp
  tests/test_groups.cpp
  tests/test_memories.cpp
  tests/test_skills.cpp
  tests/test_skills_exec.cpp
  tests/test_migration.cpp
  tests/test_config.cpp
  tests/test_dpapi.cpp
  tests/test_openai_parser.cpp
  tests/test_chat_service.cpp
  tests/test_event_bus.cpp
  tests/test_group_engine.cpp
  tests/test_memory_service.cpp
  src/db.cpp
  src/config.cpp
  src/dpapi_util.cpp
  src/openai_provider.cpp
  src/chat_service.cpp
  src/event_bus.cpp
  src/group_engine.cpp
  src/memory_service.cpp
  src/skills.cpp
  src/logger.cpp
)
target_include_directories(backend_tests PRIVATE src third_party)
target_link_libraries(backend_tests PRIVATE sqlite3 winhttp crypt32 ws2_32)
add_test(NAME backend_tests COMMAND backend_tests)
