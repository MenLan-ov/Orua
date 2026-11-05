#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace db {

struct Session {
  int64_t id = 0;
  std::string title;
  std::string created_at;
  std::string updated_at;
};
struct Message {
  int64_t id = 0;
  int64_t session_id = 0;
  std::string role;      // "user" | "assistant" | "system"
  std::string content;
  std::string created_at;
};
struct Persona {
  int64_t id = 0;
  std::string name;
  std::string avatar = "🌸";
  std::string greeting;
  std::string system_prompt;
  double temperature = 0.8;
  bool is_active = false;
  std::string created_at;
  std::string updated_at;
};

bool open(const std::string& dbPath);
void close();

Session createSession(const std::string& title = "新对话");
std::vector<Session> listSessions();
void renameSession(int64_t id, const std::string& title);
void deleteSession(int64_t id);
void touchSession(int64_t id);
std::optional<Session> getSession(int64_t id);

Message appendMessage(int64_t sessionId, const std::string& role,
                      const std::string& content);
std::vector<Message> listMessages(int64_t sessionId, int limit = 1000);
void deleteMessage(int64_t id);
void clearMessages(int64_t sessionId);
std::optional<Message> lastMessageByRole(int64_t sessionId,
                                         const std::string& role);

Persona createPersona(const Persona& p);
bool updatePersona(const Persona& p);
bool deletePersona(int64_t id);
std::vector<Persona> listPersonas();
bool activatePersona(int64_t id);
std::optional<Persona> getActivePersona();

std::optional<std::string> getSetting(const std::string& key);
void setSetting(const std::string& key, const std::string& value);

}  // namespace db
