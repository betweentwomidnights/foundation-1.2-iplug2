#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace keybed
{

constexpr const char* kModelRepo = "thepatch/foundation-1.2-keybeds-GGUF";

// The tiers published in kModelRepo, smallest last.
const std::vector<std::string>& ModelTiers();

// One file of a tier's set: the name libsa3 resolves (sa3::sat::model_paths) and its published size.
struct ModelFile
{
  std::string filename;
  long long bytes = 0;   // published size, for labels and the progress bar before the size probe
  const char* what = "";
};
std::vector<ModelFile> ModelFilesFor(const std::string& encoding);
long long ModelTierBytes(const std::string& encoding);
std::string HumanBytes(long long bytes);   // "1.4 GB"

// Downloads one tier's three GGUFs from Hugging Face with the system curl (Windows 10+, macOS), off the
// UI thread. Each file goes to "<name>.part", resumes with curl -C -, and is renamed only once its size
// matches the published one, so a half-downloaded file never looks like a model. Files already present
// at the right size are skipped.
class ModelDownloader
{
public:
  ~ModelDownloader();

  bool Start(const std::string& destDir, const std::string& encoding, std::string& error);
  void Cancel() { mCancel.store(true, std::memory_order_release); }
  bool Busy() const { return mBusy.load(std::memory_order_acquire); }
  float Progress() const { return mProgress.load(std::memory_order_acquire); }
  std::string Status() const;
  std::string Encoding() const;

  // After a download ends: true once, with the outcome. `ok` means the tier is complete in `dir`.
  bool TakeResult(bool& ok, std::string& dir, std::string& encoding, std::string& message);

private:
  void Run(std::string destDir, std::string encoding);
  void SetStatus(std::string text);
  void Finish(bool ok, std::string message);

  std::thread mWorker;
  std::atomic<bool> mBusy{false};
  std::atomic<bool> mCancel{false};
  std::atomic<float> mProgress{0.f};
  mutable std::mutex mMutex;
  std::string mStatus;
  std::string mDir, mEncoding;
  bool mHasResult = false, mResultOk = false;
  std::string mResultMessage;
};

} // namespace keybed
