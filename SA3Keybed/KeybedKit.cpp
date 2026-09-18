#include "KeybedKit.h"

#include "sat/keybed.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace fs = std::filesystem;
namespace kb = sa3::sat::keybed;

namespace keybed
{
namespace
{

fs::path PathFromUtf8(const std::string& path)
{
#if defined(_WIN32) && __cplusplus < 202002L
  return fs::u8path(path);
#else
  return fs::path(path);
#endif
}

std::string Utf8FromPath(const fs::path& path)
{
#if __cplusplus >= 202002L
  const auto u8 = path.u8string();
  return std::string(u8.begin(), u8.end());
#else
  return path.u8string();
#endif
}

std::string DocumentsDirectory()
{
#ifdef _WIN32
  PWSTR raw = nullptr;
  std::string out;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &raw)) && raw)
    out = Utf8FromPath(fs::path(raw));
  if (raw)
    CoTaskMemFree(raw);
  return out;
#else
  const char* home = std::getenv("HOME");
  return home ? std::string(home) + "/Documents" : std::string();
#endif
}

std::mutex& SettingsMutex()
{
  static std::mutex mutex;
  return mutex;
}

std::string JsonEscape(const std::string& text)
{
  std::string out;
  for (unsigned char c : text)
  {
    switch (c)
    {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20)
        {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        }
        else
          out += (char)c;
    }
  }
  return out;
}

// kit.json is written by WriteKitManifest with one "key": value per line; this reader only has to
// understand that shape (strings, numbers, booleans, flat arrays).
std::map<std::string, std::string> ReadFlatJson(const std::string& text)
{
  std::map<std::string, std::string> out;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line))
  {
    const size_t q1 = line.find('"');
    if (q1 == std::string::npos) continue;
    const size_t q2 = line.find('"', q1 + 1);
    const size_t colon = q2 == std::string::npos ? std::string::npos : line.find(':', q2);
    if (colon == std::string::npos) continue;
    std::string value = line.substr(colon + 1);
    while (!value.empty() && (std::isspace((unsigned char)value.back()) || value.back() == ','))
      value.pop_back();
    size_t start = 0;
    while (start < value.size() && std::isspace((unsigned char)value[start])) ++start;
    out[line.substr(q1 + 1, q2 - q1 - 1)] = value.substr(start);
  }
  return out;
}

std::string JsonString(const std::string& raw)
{
  if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"')
    return {};
  std::string out;
  for (size_t i = 1; i + 1 < raw.size(); ++i)
  {
    if (raw[i] == '\\' && i + 2 < raw.size())
    {
      const char e = raw[++i];
      out += e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
    }
    else
      out += raw[i];
  }
  return out;
}

std::vector<std::string> JsonArrayItems(const std::string& raw)
{
  std::vector<std::string> items;
  if (raw.size() < 2 || raw.front() != '[')
    return items;
  std::string current;
  bool inString = false;
  for (size_t i = 1; i + 1 < raw.size(); ++i)
  {
    const char c = raw[i];
    if (c == '"' && (i == 0 || raw[i - 1] != '\\'))
      inString = !inString;
    if (c == ',' && !inString)
    {
      items.push_back(current);
      current.clear();
      continue;
    }
    current += c;
  }
  if (!current.empty())
    items.push_back(current);
  for (auto& item : items)
  {
    size_t b = 0, e = item.size();
    while (b < e && std::isspace((unsigned char)item[b])) ++b;
    while (e > b && std::isspace((unsigned char)item[e - 1])) --e;
    item = item.substr(b, e - b);
  }
  return items;
}

std::string JoinInts(const std::vector<int>& values)
{
  std::string out = "[";
  for (size_t i = 0; i < values.size(); ++i)
    out += (i ? ", " : "") + std::to_string(values[i]);
  return out + "]";
}

void PutU16(std::ofstream& out, uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); }
void PutU32(std::ofstream& out, uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); }

} // namespace

std::string AppDirectory()
{
  const std::string docs = DocumentsDirectory();
  if (docs.empty())
    return {};
  const fs::path dir = PathFromUtf8(docs) / "sa3-keybed";
  std::error_code ec;
  fs::create_directories(dir, ec);
  return ec ? std::string() : Utf8FromPath(dir);
}

std::string KitsDirectory()
{
  const std::string app = AppDirectory();
  if (app.empty())
    return {};
  const fs::path dir = PathFromUtf8(app) / "kits";
  std::error_code ec;
  fs::create_directories(dir, ec);
  return ec ? std::string() : Utf8FromPath(dir);
}

std::string DefaultModelsDirectory()
{
  const std::string app = AppDirectory();
  if (app.empty())
    return {};
  const fs::path dir = PathFromUtf8(app) / "models";
  std::error_code ec;
  fs::create_directories(dir, ec);
  return ec ? std::string() : Utf8FromPath(dir);
}

std::string CreateKitDirectory(const std::string& descriptor, uint64_t seed)
{
  const std::string kits = KitsDirectory();
  if (kits.empty())
    return {};
  const std::vector<std::string> body = kb::split_descriptor_tokens(descriptor).body;
  std::string slug;
  for (size_t i = 0; i < body.size() && i < 2; ++i)
  {
    for (unsigned char c : body[i])
      slug += std::isalnum(c) ? (char)std::tolower(c) : '-';
    slug += '-';
  }
  if (slug.empty())
    slug = "keybed-";
  if (slug.size() > 32)
    slug.resize(32);

  const std::time_t now = std::time(nullptr);
  std::tm local = {};
#ifdef _WIN32
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  char stamp[32];
  std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &local);
  fs::path dir = PathFromUtf8(kits) / (std::string(stamp) + "-" + slug + std::to_string(seed));
  std::error_code ec;
  for (int n = 2; fs::exists(dir, ec); ++n)
    dir = PathFromUtf8(kits) / (std::string(stamp) + "-" + slug + std::to_string(seed) + "-" + std::to_string(n));
  fs::create_directories(dir, ec);   // the render adds chunks/ beside the WAVs it writes
  return ec ? std::string() : Utf8FromPath(dir);
}

std::string LoadSetting(const std::string& key)
{
  std::lock_guard<std::mutex> lock(SettingsMutex());
  const std::string app = AppDirectory();
  if (app.empty())
    return {};
  std::ifstream in(PathFromUtf8(app) / "settings.txt");
  std::string line;
  while (std::getline(in, line))
  {
    const size_t eq = line.find('=');
    if (eq != std::string::npos && line.compare(0, eq, key) == 0)
    {
      std::string value = line.substr(eq + 1);
      if (!value.empty() && value.back() == '\r') value.pop_back();
      return value;
    }
  }
  return {};
}

bool SaveSetting(const std::string& key, const std::string& value)
{
  std::lock_guard<std::mutex> lock(SettingsMutex());
  const std::string app = AppDirectory();
  if (app.empty())
    return false;
  const fs::path path = PathFromUtf8(app) / "settings.txt";
  std::vector<std::string> lines;
  {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line))
    {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const size_t eq = line.find('=');
      if (eq == std::string::npos || line.compare(0, eq, key) != 0)
        lines.push_back(line);
    }
  }
  lines.push_back(key + "=" + value);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  for (const auto& line : lines)
    out << line << "\n";
  return (bool)out;
}

std::string NoteFileName(int midi)
{
  return kb::note_filename(midi) + ".wav";
}

bool WritePlanarWav(const std::string& path, const float* planar, int channels, int frames, int sampleRate,
                    std::string& error)
{
  std::ofstream out(PathFromUtf8(path), std::ios::binary | std::ios::trunc);
  if (!out)
  {
    error = "cannot write " + path;
    return false;
  }
  // IEEE float WAV keeps the generated samples bit-exact.
  const uint32_t dataBytes = (uint32_t)((size_t)frames * (size_t)channels * sizeof(float));
  out.write("RIFF", 4); PutU32(out, 4 + 26 + 12 + 8 + dataBytes); out.write("WAVE", 4);
  out.write("fmt ", 4); PutU32(out, 18); PutU16(out, 3); PutU16(out, (uint16_t)channels);
  PutU32(out, (uint32_t)sampleRate); PutU32(out, (uint32_t)(sampleRate * channels * sizeof(float)));
  PutU16(out, (uint16_t)(channels * sizeof(float))); PutU16(out, 32); PutU16(out, 0);
  out.write("fact", 4); PutU32(out, 4); PutU32(out, (uint32_t)frames);
  out.write("data", 4); PutU32(out, dataBytes);
  std::vector<float> frame((size_t)channels);
  for (int i = 0; i < frames; ++i)
  {
    for (int c = 0; c < channels; ++c)
      frame[(size_t)c] = planar[(size_t)c * (size_t)frames + (size_t)i];
    out.write(reinterpret_cast<const char*>(frame.data()), (std::streamsize)(frame.size() * sizeof(float)));
  }
  if (!out)
  {
    error = "short write " + path;
    return false;
  }
  return true;
}

bool WriteNoteWav(const std::string& path, const NoteSample& note, std::string& error)
{
  std::vector<float> planar(note.left);
  planar.insert(planar.end(), note.right.begin(), note.right.end());
  return WritePlanarWav(path, planar.data(), 2, note.frames, note.sampleRate, error);
}

NoteSamplePtr ReadNoteWav(const std::string& path, int midi, std::string& error, int layer, bool layered)
{
  std::ifstream in(PathFromUtf8(path), std::ios::binary);
  if (!in)
  {
    error = "cannot open " + path;
    return nullptr;
  }
  std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  auto u16 = [&](size_t at) { uint16_t v; std::memcpy(&v, bytes.data() + at, 2); return v; };
  auto u32 = [&](size_t at) { uint32_t v; std::memcpy(&v, bytes.data() + at, 4); return v; };
  if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) || std::memcmp(bytes.data() + 8, "WAVE", 4))
  {
    error = path + " is not a WAV file";
    return nullptr;
  }
  uint16_t format = 0, channels = 0, bits = 0;
  uint32_t rate = 0;
  size_t dataAt = 0, dataSize = 0;
  for (size_t at = 12; at + 8 <= bytes.size();)
  {
    const uint32_t size = u32(at + 4);
    if (!std::memcmp(bytes.data() + at, "fmt ", 4) && at + 8 + 16 <= bytes.size())
    {
      format = u16(at + 8); channels = u16(at + 10); rate = u32(at + 12); bits = u16(at + 22);
      if (format == 0xFFFE && size >= 40) format = u16(at + 8 + 24);  // WAVE_FORMAT_EXTENSIBLE subformat
    }
    else if (!std::memcmp(bytes.data() + at, "data", 4))
    {
      dataAt = at + 8;
      dataSize = std::min<size_t>(size, bytes.size() - dataAt);
    }
    at += 8 + (size_t)size + (size & 1);
  }
  const bool pcm16 = format == 1 && bits == 16;
  const bool float32 = format == 3 && bits == 32;
  if (!dataAt || !channels || !rate || (!pcm16 && !float32))
  {
    error = path + ": unsupported WAV (need 16-bit PCM or 32-bit float)";
    return nullptr;
  }
  const size_t sampleBytes = bits / 8u;
  const int frames = (int)(dataSize / (sampleBytes * channels));
  auto note = std::make_shared<NoteSample>();
  note->midi = midi;
  note->layer = layer;
  note->layered = layered;
  note->sampleRate = (int)rate;
  note->frames = frames;
  note->left.resize((size_t)frames);
  note->right.resize((size_t)frames);
  for (int i = 0; i < frames; ++i)
  {
    for (int c = 0; c < 2; ++c)
    {
      const int src = std::min<int>(c, channels - 1);
      const size_t at = dataAt + ((size_t)i * channels + (size_t)src) * sampleBytes;
      float v;
      if (float32)
        std::memcpy(&v, bytes.data() + at, 4);
      else
      {
        int16_t s;
        std::memcpy(&s, bytes.data() + at, 2);
        v = (float)s / 32768.f;
      }
      (c == 0 ? note->left : note->right)[(size_t)i] = v;
      note->peak = std::max(note->peak, std::fabs(v));
    }
  }
  return note;
}

bool WriteKitManifest(const std::string& kitDir, const KitManifest& m, std::string& error)
{
  std::ofstream out(PathFromUtf8(kitDir) / "kit.json", std::ios::binary | std::ios::trunc);
  if (!out)
  {
    error = "cannot write kit.json in " + kitDir;
    return false;
  }
  const auto stringArray = [](const std::vector<std::string>& items) {
    std::string out = "[";
    for (size_t i = 0; i < items.size(); ++i)
      out += (i ? ", \"" : "\"") + JsonEscape(items[i]) + "\"";
    return out + "]";
  };
  const std::string fx = stringArray(m.fx);
  std::string layerSeeds = "[";
  for (size_t i = 0; i < m.layerSeeds.size(); ++i)
    layerSeeds += (i ? ", " : "") + std::to_string(m.layerSeeds[i]);
  layerSeeds += "]";
  char numbers[256];
  std::snprintf(numbers, sizeof numbers,
                "  \"seed\": %llu,\n  \"steps\": %d,\n  \"cfg_scale\": %.4g,\n  \"sigma_min\": %.4g,\n  \"sigma_max\": %.4g,\n",
                (unsigned long long)m.seed, m.steps, m.cfgScale, m.sigmaMin, m.sigmaMax);
  out << "{\n"
      << "  \"format\": \"sa3-keybed-kit-1\",\n"
      << "  \"descriptor\": \"" << JsonEscape(m.descriptor) << "\",\n"
      << "  \"wet\": " << (m.wet ? "true" : "false") << ",\n"
      << "  \"fx\": " << fx << ",\n"
      << numbers
      << "  \"model\": \"" << JsonEscape(m.model) << "\",\n"
      << "  \"encoding\": \"" << JsonEscape(m.encoding) << "\",\n"
      << "  \"range\": \"" << JsonEscape(m.range) << "\",\n"
      << "  \"label_midis\": " << JoinInts(m.labelMidis) << ",\n"
      << "  \"sounding_midis\": " << JoinInts(m.soundingMidis) << ",\n"
      << "  \"layer_descriptors\": " << stringArray(m.layerDescriptors) << ",\n"
      << "  \"layer_seeds\": " << layerSeeds << ",\n"
      << "  \"pitch_note\": \"samples are keyed by sounding pitch; Foundation-1.2 renders one octave below its prompt labels\",\n"
      << "  \"complete\": " << (m.complete ? "true" : "false") << "\n"
      << "}\n";
  if (!out)
  {
    error = "short write kit.json";
    return false;
  }
  return true;
}

bool ReadKitManifest(const std::string& kitDir, KitManifest& m, std::string& error)
{
  std::ifstream in(PathFromUtf8(kitDir) / "kit.json", std::ios::binary);
  if (!in)
  {
    error = "no kit.json in " + kitDir;
    return false;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  const auto json = ReadFlatJson(ss.str());
  auto get = [&](const char* key) { auto it = json.find(key); return it == json.end() ? std::string() : it->second; };
  m.descriptor = JsonString(get("descriptor"));
  m.wet = get("wet") == "true";
  m.fx.clear();
  for (const auto& item : JsonArrayItems(get("fx")))
    m.fx.push_back(JsonString(item));
  m.seed = std::strtoull(get("seed").c_str(), nullptr, 10);
  m.steps = std::atoi(get("steps").c_str());
  m.cfgScale = std::strtof(get("cfg_scale").c_str(), nullptr);
  m.sigmaMin = std::strtof(get("sigma_min").c_str(), nullptr);
  m.sigmaMax = std::strtof(get("sigma_max").c_str(), nullptr);
  m.model = JsonString(get("model"));
  m.encoding = JsonString(get("encoding"));
  m.range = JsonString(get("range"));
  m.labelMidis.clear();
  for (const auto& item : JsonArrayItems(get("label_midis")))
    m.labelMidis.push_back(std::atoi(item.c_str()));
  m.soundingMidis.clear();
  for (const auto& item : JsonArrayItems(get("sounding_midis")))
    m.soundingMidis.push_back(std::atoi(item.c_str()));
  m.complete = get("complete") == "true";
  m.layerDescriptors.clear();
  for (const auto& item : JsonArrayItems(get("layer_descriptors")))
    m.layerDescriptors.push_back(JsonString(item));
  m.layerSeeds.clear();
  for (const auto& item : JsonArrayItems(get("layer_seeds")))
    m.layerSeeds.push_back(std::strtoull(item.c_str(), nullptr, 10));
  return true;
}

bool WriteKitSfz(const std::string& kitDir, const std::vector<int>& soundingMidis, std::string& error)
{
  std::vector<kb::SfzRegion> regions;
  for (int midi : soundingMidis)
    regions.push_back({NoteFileName(midi), midi});
  const std::string text = kb::sfz_text(regions);
  std::ofstream out(PathFromUtf8(kitDir) / "kit.sfz", std::ios::binary | std::ios::trunc);
  out << text;
  if (!out)
  {
    error = "cannot write kit.sfz in " + kitDir;
    return false;
  }
  return true;
}

bool WriteLayeredKitSfz(const std::string& kitDir, const std::vector<std::vector<int>>& layerMidis,
                        std::string& error)
{
  std::string text = "// Auto-generated Foundation-1.2 layered keybed export (sa3.cpp)\n";
  char line[200];
  for (size_t l = 0; l < layerMidis.size() && l < (size_t)kb::kLayerCount; ++l)
  {
    const double db = 20.0 * std::log10((double)(kb::kLayerMasterVolume * kb::kLayerDefaultVolumes[l]));
    std::snprintf(line, sizeof line,
                  "\n// %s\n<group> volume=%.2f ampeg_attack=0.0050 ampeg_decay=0.0000 ampeg_sustain=100 ampeg_release=0.2500\n",
                  kb::kLayerRoles[l], db);
    text += line;
    std::vector<int> midis = layerMidis[l];
    std::sort(midis.begin(), midis.end());
    for (int midi : midis)
    {
      std::snprintf(line, sizeof line, "<region> sample=%s/%s lokey=%d hikey=%d pitch_keycenter=%d\n",
                    kb::kLayerDirNames[l], NoteFileName(midi).c_str(), midi, midi, midi);
      text += line;
    }
  }
  std::ofstream out(PathFromUtf8(kitDir) / "kit.sfz", std::ios::binary | std::ios::trunc);
  out << text;
  if (!out)
  {
    error = "cannot write kit.sfz in " + kitDir;
    return false;
  }
  return true;
}

namespace
{
std::vector<NoteSamplePtr> LoadLayerSamples(const std::string& dir, int layer, bool layered, KitManifest& manifest,
                                            std::string& error)
{
  std::vector<NoteSamplePtr> notes;
  std::string manifestError;
  std::vector<int> keys;
  if (ReadKitManifest(dir, manifest, manifestError) && !manifest.soundingMidis.empty())
    keys = manifest.soundingMidis;
  else
    for (int midi = 0; midi < 128; ++midi)
      keys.push_back(midi);
  for (int midi : keys)
  {
    const fs::path path = PathFromUtf8(dir) / PathFromUtf8(NoteFileName(midi));
    std::error_code ec;
    if (!fs::is_regular_file(path, ec))
      continue;
    std::string noteError;
    if (auto note = ReadNoteWav(Utf8FromPath(path), midi, noteError, layer, layered))
      notes.push_back(std::move(note));
    else
      error = noteError;
  }
  return notes;
}
} // namespace

std::vector<NoteSamplePtr> LoadKitSamples(const std::string& kitDir, KitManifest& manifest, std::string& error)
{
  std::vector<NoteSamplePtr> notes;
  std::string manifestError;
  if (ReadKitManifest(kitDir, manifest, manifestError) && manifest.layerDescriptors.size() > 1)
  {
    for (size_t l = 0; l < manifest.layerDescriptors.size() && l < (size_t)kb::kLayerCount; ++l)
    {
      KitManifest layerManifest;
      auto layerNotes = LoadLayerSamples(Utf8FromPath(PathFromUtf8(kitDir) / kb::kLayerDirNames[l]), (int)l, true,
                                         layerManifest, error);
      notes.insert(notes.end(), layerNotes.begin(), layerNotes.end());
    }
  }
  else
    notes = LoadLayerSamples(kitDir, 0, false, manifest, error);
  if (notes.empty() && error.empty())
    error = "no note samples in " + kitDir;
  return notes;
}

std::string FolderName(const std::string& path)
{
  return Utf8FromPath(PathFromUtf8(path).filename());
}

} // namespace keybed
