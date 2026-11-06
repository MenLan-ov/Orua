#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace db {

struct Session {
  int64_t id = 0;
  std::string title;
  int64_t bot_id = 0;   // 0 = 未绑定 → 运行时按默认机器人处理
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
struct Bot {
  int64_t id = 0;
  std::string name;
  std::string avatar = "🤖";
  std::string greeting;
  std::string system_prompt;
  double temperature = 0.8;
  bool is_enabled = true;
  // API 覆盖项：空串 = 继承全局默认（settings）
  std::string api_base_url;
  std::string api_key_enc;
  std::string api_model;
  int max_tokens = 2048;
  std::string api_type = "openai";
  std::string avatar_path;   // 空 = 用 emoji 头像；否则相对 data 目录的图片路径
  std::string created_at;
  std::string updated_at;
};
struct Group {
  int64_t id = 0;
  std::string name;
  std::string topic;
  std::string status = "idle";   // idle | running | paused（运行态仅内存语义，落库便于恢复）
  int member_count = 0;          // 列表/详情查询时填充
  std::string last_active;       // 最后一条群消息时间，无则空
  std::string created_at;
  std::string updated_at;
};
struct GroupMember {
  int64_t id = 0;
  int64_t group_id = 0;
  int64_t bot_id = 0;
  int position = 0;
};
struct GroupMessage {
  int64_t id = 0;
  int64_t group_id = 0;
  int64_t bot_id = 0;   // 0 = 用户插话（库内 NULL）
  std::string role;     // "user" | "assistant"
  std::string content;
  std::string created_at;
};
struct Memory {
  int64_t id = 0;
  int64_t bot_id = 0;
  std::string scope;      // "private" | "group"
  int64_t group_id = 0;   // 库内 NULL；private 为 0
  std::string content;
  std::string tags;       // JSON 数组字符串，如 "[\"咖啡\"]"
  int64_t source_msg_id = 0;
  std::string created_at;
  std::string updated_at;
};
struct BotSkill {
  int64_t id = 0;
  int64_t bot_id = 0;
  std::string skill_id;
  bool enabled = true;
  std::string params = "{}";
  std::string created_at;
  std::string updated_at;
};
struct Reminder {
  int64_t id = 0;
  int64_t bot_id = 0;
  std::string content;
  std::string due_at;
  bool done = false;
  std::string created_at;
};

bool open(const std::string& dbPath);
void close();

Session createSession(const std::string& title = "新对话", int64_t botId = 0);
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

void setSessionBot(int64_t sessionId, int64_t botId);

Bot createBot(const Bot& b);
bool updateBot(const Bot& b);
bool deleteBot(int64_t id);            // 默认机器人返回 false
std::vector<Bot> listBots();
std::optional<Bot> getBot(int64_t id);
bool setBotEnabled(int64_t id, bool enabled);
Memory addMemory(int64_t botId, const std::string& scope, int64_t groupId,
                 const std::string& content, const std::string& tags,
                 int64_t sourceMsgId = 0);
bool updateMemoryContent(int64_t id, const std::string& content);
bool deleteMemory(int64_t id);
std::vector<Memory> listMemories(int64_t botId, const std::string& scope = "",
                                 int64_t groupId = -1, const std::string& q = "");
void setBotSkills(int64_t botId,
                  const std::vector<std::pair<std::string, bool>>& skills);
std::vector<BotSkill> listBotSkills(int64_t botId);
Reminder addReminder(int64_t botId, const std::string& content,
                     const std::string& dueAt);
std::vector<Reminder> listReminders(bool dueOnly);
bool markReminderDone(int64_t id);
std::optional<int64_t> memorySourceSession(int64_t msgId);   // messages.id → session_id（私聊记忆来源）
void deleteMemoriesByBot(int64_t botId);
void deleteMemoriesByGroup(int64_t groupId);
std::string dataDir();
std::optional<Bot> getDefaultBot();    // settings default_bot_id，兜底取第一个

Group createGroup(const std::string& name, const std::string& topic);
bool updateGroup(int64_t id, const std::optional<std::string>& name,
                 const std::optional<std::string>& topic);
bool setGroupStatus(int64_t id, const std::string& status);
void touchGroup(int64_t id);
void deleteGroup(int64_t id);
std::vector<Group> listGroups();
std::optional<Group> getGroup(int64_t id);
void setGroupMembers(int64_t groupId, const std::vector<int64_t>& botIds);
std::vector<GroupMember> listGroupMembers(int64_t groupId);
GroupMessage appendGroupMessage(int64_t groupId, int64_t botId,
                                const std::string& role, const std::string& content);
std::vector<GroupMessage> listGroupMessages(int64_t groupId, int limit = 0);

std::optional<std::string> getSetting(const std::string& key);
void setSetting(const std::string& key, const std::string& value);

}  // namespace db
