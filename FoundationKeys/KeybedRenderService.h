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
  // One descriptor per layer: Main, then up to two Supports (RC's layered keybed). The range,
  // sampler, and render fx (wet + fx tags) are shared by every layer.
  std::vector<std::string> layers;
  bool wet = false;
  std::vector<std::string> fx;
  std::vector<sa3::sat::keybed::Chunk> chunks;
  std::string rangeLabel;
  std::string kitsDirectory;
  AudioFormat audioFormat = AudioFormat::Flac;
  int64_t seed = -1;   // base seed; -1 picks one. Layered jobs derive one seed per layer from it.
  int steps = 80;
  float cfgScale = 6.f;
  bool keepResident = true;
};

struct KeybedEvent
{
  enum class Kind { Note, Finished, Failed, Cancelled };
  Kind kind = Kind::Note;
  NoteSamplePtr note;         // Kind::Note
  KitManifest manifest;       // terminal event; samples remain in memory until exported
  std::string message;
  uint64_t seed = 0;
};

// One worker thread renders a job's chunks sequentially through libsa3 with one shared seed and a
// context that stays resident across chunks. Each finished chunk is sliced into notes and queued for
// the UI thread (DrainEvents), so keys become playable as they land without writing to disk.
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
  // Seconds the last completed chunk took, so the UI can estimate a build on this machine.
  double LastChunkSeconds() const noexcept { return mLastChunkSeconds.load(std::memory_order_acquire); }
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
  std::atomic<double> mLastChunkSeconds{0.0};
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
