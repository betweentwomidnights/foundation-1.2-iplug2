#include "ModelDownload.h"

#include "sat/model_paths.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace keybed
{
namespace
{
constexpr const char* kVariant = "foundation-1.2-keybeds";

// Published sizes (huggingface.co/thepatch/foundation-1.2-keybeds-GGUF). Only labels and the first
// progress estimate use these; each download checks the size Hugging Face reports.
struct Published
{
  const char* encoding;
  long long dit, t5, oobleck;
};
constexpr Published kPublished[] = {
  {"F16", 2116602240LL, 219954784LL, 156319168LL},
  {"Q8_0", 1126243200LL, 117196384LL, 115707328LL},
  {"Q5_K_M", 786641280LL, 80310112LL, 99548608LL},
  {"Q4_K_M", 707690880LL, 70578016LL, 94141888LL},
};

fs::path PathFromUtf8(const std::string& s) { return fs::u8path(s); }

std::string Utf8(const fs::path& path)
{
  const auto s = path.u8string();   // std::u8string from C++20
  return std::string(s.begin(), s.end());
}

long long FileBytes(const fs::path& path)
{
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  return ec ? -1 : (long long)size;
}

std::string ResolveUrl(const std::string& filename)
{
  return std::string("https://huggingface.co/") + kModelRepo + "/resolve/main/" + filename;
}

// --- a child curl process -------------------------------------------------------------------------
struct Process
{
#if defined(_WIN32)
  HANDLE handle = nullptr;
#else
  pid_t pid = -1;
#endif
  bool valid = false;
};

#if defined(_WIN32)
std::wstring Wide(const std::string& s)
{
  if (s.empty())
    return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

// Every curl joins one kill-on-close job, so a host that quits or crashes mid-download takes curl with
// it instead of leaving an orphan writing into the .part file.
HANDLE DownloadJob()
{
  static HANDLE job = []() {
    HANDLE h = CreateJobObjectW(nullptr, nullptr);
    if (h)
    {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
      limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      SetInformationJobObject(h, JobObjectExtendedLimitInformation, &limits, sizeof limits);
    }
    return h;
  }();
  return job;
}

std::wstring QuoteArg(const std::wstring& arg)
{
  if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos)
    return arg;
  std::wstring out = L"\"";
  size_t slashes = 0;
  for (wchar_t c : arg)
  {
    if (c == L'\\')
    {
      ++slashes;
      continue;
    }
    out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
    slashes = 0;
    out.push_back(c);
  }
  out.append(slashes * 2, L'\\');
  out.push_back(L'"');
  return out;
}
#endif

Process Spawn(const std::vector<std::string>& argv, std::string& error)
{
  Process proc;
#if defined(_WIN32)
  std::wstring cmd;
  for (const auto& arg : argv)
    cmd += (cmd.empty() ? L"" : L" ") + QuoteArg(Wide(arg));
  STARTUPINFOW startup = {};
  startup.cb = sizeof startup;
  startup.dwFlags = STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION info = {};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                      nullptr, &startup, &info))
  {
    error = "could not run curl (win32 error " + std::to_string(GetLastError()) + ")";
    return proc;
  }
  if (HANDLE job = DownloadJob())
    AssignProcessToJobObject(job, info.hProcess);   // before it runs, so it cannot outlive us
  ResumeThread(info.hThread);
  CloseHandle(info.hThread);
  proc.handle = info.hProcess;
#else
  std::vector<char*> args;
  for (const auto& arg : argv)
    args.push_back(const_cast<char*>(arg.c_str()));
  args.push_back(nullptr);
  if (const int rc = posix_spawnp(&proc.pid, args[0], nullptr, nullptr, args.data(), environ); rc != 0)
  {
    error = "could not run curl (" + std::to_string(rc) + ")";
    return proc;
  }
#endif
  proc.valid = true;
  return proc;
}

// True once the process has exited; `exitCode` is then set.
bool TryWait(Process& proc, int& exitCode)
{
  if (!proc.valid)
  {
    exitCode = -1;
    return true;
  }
#if defined(_WIN32)
  if (WaitForSingleObject(proc.handle, 0) != WAIT_OBJECT_0)
    return false;
  DWORD code = 1;
  GetExitCodeProcess(proc.handle, &code);
  exitCode = (int)code;
  CloseHandle(proc.handle);
#else
  int status = 0;
  const pid_t r = waitpid(proc.pid, &status, WNOHANG);
  if (r == 0)
    return false;
  exitCode = r > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
  proc.valid = false;
  return true;
}

void Kill(Process& proc)
{
  if (!proc.valid)
    return;
#if defined(_WIN32)
  TerminateProcess(proc.handle, 1);
  WaitForSingleObject(proc.handle, 5000);
  CloseHandle(proc.handle);
#else
  kill(proc.pid, SIGTERM);
  int status = 0;
  waitpid(proc.pid, &status, 0);
#endif
  proc.valid = false;
}

// Runs curl to completion (or until `cancel`); returns its exit code, or -1 when it could not start.
int RunCurl(const std::vector<std::string>& argv, const std::atomic<bool>& cancel, std::string& error)
{
  Process proc = Spawn(argv, error);
  if (!proc.valid)
    return -1;
  int code = 1;
  while (!TryWait(proc, code))
  {
    if (cancel.load(std::memory_order_acquire))
    {
      Kill(proc);
      return -2;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return code;
}

// Hugging Face answers a HEAD on /resolve/ with a redirect carrying X-Linked-Size (the file's size);
// the CDN's final content-length is the fallback.
long long RemoteBytes(const std::string& url, const fs::path& scratch, const std::atomic<bool>& cancel)
{
  std::string error;
  const int code = RunCurl({"curl", "-sIL", "-o", Utf8(scratch), url}, cancel, error);
  long long linked = -1, length = -1;
  std::ifstream in(scratch, std::ios::binary);
  std::string line;
  const auto valueOf = [](const std::string& text) {
    std::string digits;
    for (char c : text)
      if (std::isdigit((unsigned char)c))
        digits.push_back(c);
    return digits.empty() ? -1LL : std::strtoll(digits.c_str(), nullptr, 10);
  };
  while (std::getline(in, line))
  {
    std::string low = line;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (low.rfind("x-linked-size:", 0) == 0)
      linked = valueOf(line.substr(14));
    else if (low.rfind("content-length:", 0) == 0)
      length = valueOf(line.substr(15));
  }
  in.close();
  std::error_code ec;
  fs::remove(scratch, ec);
  if (code != 0)
    return -1;
  return linked > 0 ? linked : length;
}
} // namespace

const std::vector<std::string>& ModelTiers()
{
  static const std::vector<std::string> tiers = {"F16", "Q8_0", "Q5_K_M", "Q4_K_M"};
  return tiers;
}

std::vector<ModelFile> ModelFilesFor(const std::string& encoding)
{
  const std::string enc = sa3::sat::normalize_encoding(encoding);
  long long dit = 0, t5 = 0, oobleck = 0;
  for (const auto& p : kPublished)
    if (enc == p.encoding)
    {
      dit = p.dit;
      t5 = p.t5;
      oobleck = p.oobleck;
    }
  return {
    {sa3::sat::sat_large_dit_relative_path(kVariant, enc), dit, "DiT"},
    {sa3::sat::sat_t5_128_relative_path(enc), t5, "T5"},
    {sa3::sat::sat_oobleck_relative_path(enc), oobleck, "decoder"},
  };
}

long long ModelTierBytes(const std::string& encoding)
{
  long long total = 0;
  for (const auto& file : ModelFilesFor(encoding))
    total += file.bytes;
  return total;
}

std::string HumanBytes(long long bytes)
{
  char text[32];
  if (bytes >= 1000LL * 1000 * 1000)
    std::snprintf(text, sizeof text, "%.1f GB", bytes / 1e9);
  else
    std::snprintf(text, sizeof text, "%.0f MB", bytes / 1e6);
  return text;
}

ModelDownloader::~ModelDownloader()
{
  Cancel();
  if (mWorker.joinable())
    mWorker.join();
}

bool ModelDownloader::Start(const std::string& destDir, const std::string& encoding, std::string& error)
{
  if (destDir.empty())
  {
    error = "choose a models folder first";
    return false;
  }
  for (const auto& file : ModelFilesFor(encoding))
    if (file.filename.empty())
    {
      error = "unknown model tier " + encoding;
      return false;
    }
  if (mBusy.exchange(true, std::memory_order_acq_rel))
  {
    error = "a download is already running";
    return false;
  }
  if (mWorker.joinable())
    mWorker.join();
  mCancel.store(false, std::memory_order_release);
  mProgress.store(0.f, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(mMutex);
    mDir = destDir;
    mEncoding = sa3::sat::normalize_encoding(encoding);
    mHasResult = false;
    mStatus = "starting download";
  }
  mWorker = std::thread([this, destDir, encoding = sa3::sat::normalize_encoding(encoding)]() { Run(destDir, encoding); });
  return true;
}

std::string ModelDownloader::Status() const
{
  std::lock_guard<std::mutex> lock(mMutex);
  return mStatus;
}

std::string ModelDownloader::Encoding() const
{
  std::lock_guard<std::mutex> lock(mMutex);
  return mEncoding;
}

bool ModelDownloader::TakeResult(bool& ok, std::string& dir, std::string& encoding, std::string& message)
{
  std::lock_guard<std::mutex> lock(mMutex);
  if (!mHasResult)
    return false;
  mHasResult = false;
  ok = mResultOk;
  dir = mDir;
  encoding = mEncoding;
  message = mResultMessage;
  return true;
}

void ModelDownloader::SetStatus(std::string text)
{
  std::lock_guard<std::mutex> lock(mMutex);
  mStatus = std::move(text);
}

void ModelDownloader::Finish(bool ok, std::string message)
{
  {
    std::lock_guard<std::mutex> lock(mMutex);
    mHasResult = true;
    mResultOk = ok;
    mResultMessage = message;
    mStatus = std::move(message);
  }
  mBusy.store(false, std::memory_order_release);
}

void ModelDownloader::Run(std::string destDir, std::string encoding)
{
  const fs::path dir = PathFromUtf8(destDir);
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (!fs::is_directory(dir, ec))
    return Finish(false, "cannot create " + destDir);

  std::vector<ModelFile> files = ModelFilesFor(encoding);
  const auto cancelled = [&]() { return mCancel.load(std::memory_order_acquire); };
  const std::string cancelMessage = "download cancelled - partial files are kept, and it resumes next time";

  // The size Hugging Face reports is the truth; the table is a fallback when the probe fails.
  long long total = 0;
  for (size_t i = 0; i < files.size(); ++i)
  {
    if (cancelled())
      return Finish(false, cancelMessage);
    SetStatus("checking " + std::string(files[i].what) + " on Hugging Face");
    const long long remote = RemoteBytes(ResolveUrl(files[i].filename), dir / (files[i].filename + ".head"), mCancel);
    if (remote > 0)
      files[i].bytes = remote;
    total += std::max(0LL, files[i].bytes);
  }

  long long done = 0;
  for (size_t i = 0; i < files.size(); ++i)
  {
    const ModelFile& file = files[i];
    const fs::path target = dir / PathFromUtf8(file.filename);
    const fs::path part = dir / PathFromUtf8(file.filename + ".part");
    if (file.bytes > 0 && FileBytes(target) == file.bytes)   // already here
    {
      done += file.bytes;
      mProgress.store(total > 0 ? (float)((double)done / (double)total) : 0.f, std::memory_order_release);
      continue;
    }
    if (file.bytes > 0 && FileBytes(part) > file.bytes)   // not the file we expect: start over
      fs::remove(part, ec);

    std::string error;
    Process proc = Spawn({"curl", "-fL", "--retry", "3", "-C", "-", "-o", Utf8(part), ResolveUrl(file.filename)}, error);
    if (!proc.valid)
      return Finish(false, error + " - is curl installed?");
    int code = 1;
    for (;;)
    {
      const bool exited = TryWait(proc, code);
      const long long now = std::max(0LL, FileBytes(part));
      const double fraction = total > 0 ? std::clamp((double)(done + now) / (double)total, 0.0, 1.0) : 0.0;
      mProgress.store((float)fraction, std::memory_order_release);
      char text[128];
      std::snprintf(text, sizeof text, "downloading %s %s (%zu/%zu) - %s of %s", encoding.c_str(), file.what, i + 1,
                    files.size(), HumanBytes(done + now).c_str(), HumanBytes(total).c_str());
      SetStatus(text);
      if (exited)
        break;
      if (cancelled())
      {
        Kill(proc);
        return Finish(false, cancelMessage);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (code != 0)
      return Finish(false, "download of the " + std::string(file.what) + " failed (curl exit " + std::to_string(code) +
                             ") - download again to resume");
    const long long got = FileBytes(part);
    if (file.bytes > 0 && got != file.bytes)
    {
      fs::remove(part, ec);   // truncated or garbage: the retry starts clean rather than resuming it
      return Finish(false, "the " + std::string(file.what) + " arrived incomplete - download again");
    }
    fs::rename(part, target, ec);
    if (ec)
    {
      fs::remove(target, ec);
      fs::rename(part, target, ec);
      if (ec)
        return Finish(false, "cannot move " + file.filename + " into place: " + ec.message());
    }
    done += got;
  }
  mProgress.store(1.f, std::memory_order_release);
  Finish(true, encoding + " models ready (" + HumanBytes(total) + ")");
}

} // namespace keybed
