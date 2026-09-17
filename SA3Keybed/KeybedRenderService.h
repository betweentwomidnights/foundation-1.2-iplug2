#pragma once

#include "KeybedKit.h"
#include "libsa3_v1.h"
#include "sat/keybed.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace keybed
{

struct KeybedJob
{
  std::string modelsDir;
  std::string variant = "foundation-1.2-keybeds";
  std::string encoding = "F16";
  std::string descriptor;
  bool wet = false;
  std::vector<std::string> fx;
  std::vector<sa3::sat::keybed::Chunk> chunks;
  std::string rangeLabel;
  int64_t seed = -1;   // -1: pick once, then reuse for every chunk
  int steps = 80;
  float cfgScale = 6.f;
  bool keepResident = true;
};

struct KeybedEvent
{
  enum class Kind { Note, Finished, Failed, Cancelled };
  Kind kind = Kind::Note;
  NoteSamplePtr note;         // Kind::Note
  std::string kitDir;
  std::string message;
  uint64_t seed = 0;
};

// One worker thread renders a job's chunks sequentially through libsa3 with one shared seed and a
// context that stays resident across chunks. Each finished chunk is sliced into notes that are saved
// into the kit folder and queued for the UI thread (DrainEvents), so keys become playable as they land.
class KeybedRenderService
{
public:
  KeybedRenderService() = default;
  ~KeybedRenderService();
  KeybedRenderService(const KeybedRenderService&) = delete;
  KeybedRenderService& operator=(const KeybedRenderService&) = delete;

  bool Start(KeybedJob job, std::string& error);
  void Cancel();
  // Joins a finished worker and frees the context unless it should stay resident.
  void Collect(bool keepResident);
  void ReleaseModels();

  bool Busy() const noexcept { return mBusy.load(std::memory_order_acquire); }
  float Progress() const noexcept { return mProgress.load(std::memory_order_acquire); }
  int CurrentChunk() const noexcept { return mChunk.load(std::memory_order_acquire); }
  int TotalChunks() const noexcept { return mTotalChunks.load(std::memory_order_acquire); }
  std::string Status() const;
  std::vector<int> ActiveLabels() const;      // labels of the chunk being rendered
  std::vector<KeybedEvent> DrainEvents();

private:
  void Run(KeybedJob job, uint64_t requestId);
  void SetStatus(std::string status);
  void Push(KeybedEvent event);
  static void SA3_CALL OnProgress(void* user, const sa3_progress_v1* progress);
  static int32_t SA3_CALL ShouldCancel(void* user);

  std::thread mWorker;
  std::atomic<bool> mBusy{false};
  std::atomic<bool> mCancel{false};
  std::atomic<uint64_t> mRequestId{0};
  std::atomic<float> mProgress{0.f};
  std::atomic<int> mChunk{0};
  std::atomic<int> mTotalChunks{0};

  mutable std::mutex mMutex;          // guards everything below
  std::string mStatus = "ready";
  std::vector<int> mActiveLabels;
  std::deque<KeybedEvent> mEvents;

  // Owned by whichever thread is not running the worker (the worker is joined before touching these).
  sa3_context* mContext = nullptr;
  const sa3_api_v1* mApi = nullptr;
  std::string mContextKey;            // modelsDir|variant|encoding the context was created for
};

} // namespace keybed
