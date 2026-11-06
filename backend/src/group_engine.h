#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "db.h"
#include "json_util.h"
#include "llm_provider.h"

namespace grp {

using StreamFn = std::function<void(const llm::ChatRequest&, llm::StreamCallback,
                                    const std::atomic_bool&)>;
using Broadcaster = std::function<void(const std::string& event, const json& data)>;
using SubId = uint64_t;

// 每群一个实例：独立线程状态机 idle→running⇄paused→idle，SSE 广播给多订阅者。
// 生命周期：stop()/析构 join 线程；Registry 持有 shared_ptr 至群删除。
class GroupEngine {
 public:
  GroupEngine(int64_t groupId, StreamFn streamFn);
  ~GroupEngine();

  bool start();                       // idle→running；否则 false
  bool pause();                       // running→paused；否则 false
  bool resume();                      // paused→running；否则 false
  bool stop();                        // running/paused→idle（当前轮尽快中止）；已 idle 返回 false
  void interject(const std::string& content);   // 存 user 消息 + 广播

  std::string state() const;          // "idle" | "running" | "paused"
  int turn() const { return turn_.load(); }

  SubId subscribe(Broadcaster b);
  void unsubscribe(SubId id);

 private:
  enum class State { Idle, Running, Paused };
  struct Member {
    int64_t bot_id;
    std::string name;
    std::string avatar;
  };

  void loop();
  bool runTurn(const Member& m, int turn);   // 返回是否成功落库
  llm::ChatRequest buildRequest(const db::Bot& bot);
  void broadcast(const std::string& event, const json& data);

  int64_t groupId_;
  StreamFn streamFn_;
  std::vector<Member> members_;
  std::atomic<State> state_{State::Idle};
  std::atomic<int> turn_{0};
  std::atomic_bool stopFlag_{false};
  std::mutex pauseMtx_;
  std::condition_variable pauseCv_;
  std::thread thread_;
  std::map<int64_t, int> failCount_;   // 仅引擎线程访问
  std::mutex subMtx_;
  std::map<SubId, Broadcaster> subs_;
  SubId nextSubId_ = 0;
};

class GroupEngineRegistry {
 public:
  static GroupEngineRegistry& instance();

  std::shared_ptr<GroupEngine> getOrCreate(int64_t groupId, StreamFn streamFn);
  void remove(int64_t groupId);   // 若运行中先 stop（锁外 join）

 private:
  std::mutex mtx_;
  std::map<int64_t, std::shared_ptr<GroupEngine>> engines_;
};

}  // namespace grp
