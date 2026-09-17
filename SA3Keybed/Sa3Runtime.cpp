#include "Sa3Runtime.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#ifdef __APPLE__
#include <dlfcn.h>
#endif

#include <mutex>
#include <vector>

namespace keybed
{
namespace
{

using GetApiFn = const sa3_api_v1* (SA3_CALL*)(uint32_t);

#ifdef _WIN32
std::string WideToUtf8(const std::wstring& text)
{
  if (text.empty())
    return {};
  const int required = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (required <= 1)
    return {};
  std::string out((size_t)required - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), required, nullptr, nullptr);
  return out;
}

std::wstring ModuleDirectory(std::string& error)
{
  HMODULE self = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&ModuleDirectory), &self))
  {
    error = "GetModuleHandleExW failed (win32 " + std::to_string(GetLastError()) + ")";
    return {};
  }
  std::vector<wchar_t> buffer(1024);
  for (;;)
  {
    const DWORD length = GetModuleFileNameW(self, buffer.data(), (DWORD)buffer.size());
    if (length == 0)
    {
      error = "GetModuleFileNameW failed (win32 " + std::to_string(GetLastError()) + ")";
      return {};
    }
    if (length < buffer.size() - 1)
    {
      std::wstring path(buffer.data(), length);
      const size_t slash = path.find_last_of(L"\\/");
      if (slash == std::wstring::npos)
      {
        error = "could not derive module directory from " + WideToUtf8(path);
        return {};
      }
      return path.substr(0, slash);
    }
    buffer.resize(buffer.size() * 2);
  }
}

const sa3_api_v1* LoadPlatform(std::string& error)
{
  const std::wstring dir = ModuleDirectory(error);
  if (dir.empty())
    return nullptr;
  const std::wstring dllPath = dir + L"\\sa3.dll";
  HMODULE module = LoadLibraryExW(dllPath.c_str(), nullptr,
                                  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!module)
  {
    error = "LoadLibraryExW failed for " + WideToUtf8(dllPath) + " (win32 " + std::to_string(GetLastError()) + ")";
    return nullptr;
  }
  const auto getApi = reinterpret_cast<GetApiFn>(GetProcAddress(module, "sa3_get_api"));
  if (!getApi)
  {
    error = "GetProcAddress failed for sa3_get_api (win32 " + std::to_string(GetLastError()) + ")";
    return nullptr;
  }
  return getApi(SA3_ABI_VERSION_1);
}
#elif defined(__APPLE__)
const sa3_api_v1* LoadPlatform(std::string& error)
{
  Dl_info info = {};
  if (dladdr(reinterpret_cast<const void*>(&LoadPlatform), &info) == 0 || !info.dli_fname)
  {
    error = "dladdr failed while resolving module directory";
    return nullptr;
  }
  std::string path(info.dli_fname);
  const size_t slash = path.find_last_of('/');
  const std::string dylibPath = (slash == std::string::npos ? std::string(".") : path.substr(0, slash)) + "/libsa3.dylib";
  void* module = dlopen(dylibPath.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!module)
  {
    const char* detail = dlerror();
    error = "dlopen failed for " + dylibPath + (detail ? std::string(": ") + detail : std::string());
    return nullptr;
  }
  const auto getApi = reinterpret_cast<GetApiFn>(dlsym(module, "sa3_get_api"));
  if (!getApi)
  {
    error = "dlsym failed for sa3_get_api";
    return nullptr;
  }
  return getApi(SA3_ABI_VERSION_1);
}
#else
const sa3_api_v1* LoadPlatform(std::string& error)
{
  error = "runtime libsa3 loading is implemented for Windows and macOS";
  return nullptr;
}
#endif

} // namespace

const sa3_api_v1* LoadSa3Api(std::string& error)
{
  static std::mutex mutex;
  static const sa3_api_v1* api = nullptr;
  std::lock_guard<std::mutex> lock(mutex);
  if (api)
    return api;
  const sa3_api_v1* loaded = LoadPlatform(error);
  if (!loaded)
    return nullptr;
  if (loaded->abi_version != SA3_ABI_VERSION_1 || loaded->size < SA3_API_V1_MIN_SIZE)
  {
    error = "libsa3 does not provide the complete C ABI V1 table";
    return nullptr;
  }
  api = loaded;
  return api;
}

} // namespace keybed
