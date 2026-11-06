#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "db.h"
#include "llm_provider.h"

namespace mem {

struct ExtractJob {
  int64_t bot_id = 0;
  std::string scope;   // "private" | "group"
  int64_t group_id = 0;
  std::string dialogue;
};

// 提取：异步、worker 串行、失败重试一次后放弃；LLM_MOCK 固定两条占位记忆。
// 检索：两段式（本地关键词召回 ≤30 → LLM 精选 ≤maxK），失败降级、无候选为空。
class MemoryService {
 public:
  static MemoryService& instance();

  void enqueueExtract(int64_t botId, const std::string& scope, int64_t groupId,
                      const std::string& dialogue);
  std::vector<db::Memory> pickMemories(int64_t botId, const std::string& scope,
                                       int64_t groupId, const std::string& dialogue,
                                       int maxK);

 private:
  MemoryService() = default;
  ~MemoryService();   // 退出时停止并 join worker（否则 joinable 线程析构 → terminate）

  void ensureWorker();
  void workerLoop();
  void extractOnce(const ExtractJob& job);
  llm::ChatRequest buildExtractRequest(const db::Bot& bot);   // 与引擎同款解析
  void applyOps(const std::string& rawJson, const ExtractJob& job);
  std::vector<db::Memory> recallLocal(int64_t botId, const std::string& scope,
                                      int64_t groupId, const std::string& dialogue);
  std::vector<int64_t> selectByLlm(const std::vector<db::Memory>& candidates,
                                   const std::string& dialogue, int maxK);

  std::mutex mtx_;
  std::condition_variable cv_;
  std::deque<ExtractJob> queue_;
  std::thread worker_;
  bool workerStarted_ = false;
  bool quit_ = false;
};

}  // namespace mem
