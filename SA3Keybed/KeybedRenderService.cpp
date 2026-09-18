#include "KeybedRenderService.h"

#include "Sa3Runtime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>

namespace kb = sa3::sat::keybed;

namespace keybed
{
namespace
{

struct CallbackState
{
  KeybedRenderService* service = nullptr;
  uint64_t requestId = 0;
  int chunk = 0;          // within the layer
  int chunks = 1;         // per layer
  int done = 0;           // chunks finished across all layers
  int total = 1;          // chunks across all layers
  const char* layer = "";  // role name for layered builds, empty otherwise
};

std::string JoinPath(const std::string& dir, const std::string& name)
{
  return dir.empty() ? name : dir + "/" + name;
}

} // namespace

KeybedRenderService::~KeybedRenderService()
{
  Cancel();
  if (mWorker.joinable())
    mWorker.join();
  ReleaseModels();
}

bool KeybedRenderService::Start(KeybedJob job, std::string& error)
{
  if (Busy())
  {
    error = "a keybed is already rendering";
    return false;
  }
  if (mWorker.joinable())
    mWorker.join();
  if (job.chunks.empty())
  {
    error = "nothing to render";
    return false;
  }
  if (job.layers.empty() || job.layers.size() > (size_t)kb::kLayerCount)
  {
    error = "a keybed has one to three layers";
    return false;
  }
  for (const auto& descriptor : job.layers)
    if (kb::split_descriptor_tokens(descriptor).body.empty())
    {
      error = "describe every layer's instrument first (or roll the dice)";
      return false;
    }
  mCancel.store(false, std::memory_order_release);
  mProgress.store(0.f, std::memory_order_release);
  mChunk.store(0, std::memory_order_release);
  mTotalChunks.store((int)(job.chunks.size() * job.layers.size()), std::memory_order_release);
  mBusy.store(true, std::memory_order_release);
  const uint64_t requestId = mRequestId.fetch_add(1, std::memory_order_acquire) + 1;
  SetStatus("starting");
  mWorker = std::thread([this, job = std::move(job), requestId]() mutable { Run(std::move(job), requestId); });
  return true;
}

void KeybedRenderService::Cancel()
{
  mCancel.store(true, std::memory_order_release);
  mRequestId.fetch_add(1, std::memory_order_acquire);
}

void KeybedRenderService::Collect(bool keepResident)
{
  if (Busy() || !mWorker.joinable())
    return;
  mWorker.join();
  if (!keepResident)
    ReleaseModels();
}

void KeybedRenderService::ReleaseModels()
{
  if (Busy())
    return;
  if (mWorker.joinable())
    mWorker.join();
  if (mApi && mContext)
    mApi->context_destroy(mContext);
  mContext = nullptr;
  mContextKey.clear();
}

std::string KeybedRenderService::Status() const
{
  std::lock_guard<std::mutex> lock(mMutex);
  return mStatus;
}

std::vector<int> KeybedRenderService::ActiveLabels() const
{
  std::lock_guard<std::mutex> lock(mMutex);
  return mActiveLabels;
}

std::vector<KeybedEvent> KeybedRenderService::DrainEvents()
{
  std::lock_guard<std::mutex> lock(mMutex);
  std::vector<KeybedEvent> out(std::make_move_iterator(mEvents.begin()), std::make_move_iterator(mEvents.end()));
  mEvents.clear();
  return out;
}

void KeybedRenderService::SetStatus(std::string status)
{
  std::lock_guard<std::mutex> lock(mMutex);
  mStatus = std::move(status);
}

void KeybedRenderService::Push(KeybedEvent event)
{
  std::lock_guard<std::mutex> lock(mMutex);
  mEvents.push_back(std::move(event));
}

void SA3_CALL KeybedRenderService::OnProgress(void* user, const sa3_progress_v1* progress)
{
  const auto* state = static_cast<const CallbackState*>(user);
  if (!state || !progress)
    return;
  KeybedRenderService& self = *state->service;
  const float overall = ((float)state->done + std::clamp(progress->fraction, 0.f, 1.f)) / (float)std::max(1, state->total);
  self.mProgress.store(overall, std::memory_order_release);
  char prefix[48];
  if (*state->layer)
    std::snprintf(prefix, sizeof prefix, "%s · chunk %d/%d", state->layer, state->chunk + 1, state->chunks);
  else
    std::snprintf(prefix, sizeof prefix, "chunk %d/%d", state->chunk + 1, state->chunks);
  char text[128];
  if (progress->stage == SA3_PROGRESS_SAMPLING_V1 && progress->total > 0)
    std::snprintf(text, sizeof text, "%s · sampling %d/%d", prefix, progress->step, progress->total);
  else
    std::snprintf(text, sizeof text, "%s · %s", prefix, progress->stage_name ? progress->stage_name : "working");
  self.SetStatus(text);
}

int32_t SA3_CALL KeybedRenderService::ShouldCancel(void* user)
{
  const auto* state = static_cast<const CallbackState*>(user);
  const KeybedRenderService& self = *state->service;
  return self.mCancel.load(std::memory_order_acquire) ||
         state->requestId != self.mRequestId.load(std::memory_order_acquire);
}

void KeybedRenderService::Run(KeybedJob job, uint64_t requestId)
{
  const int layerCount = std::clamp((int)job.layers.size(), 1, kb::kLayerCount);
  const bool layered = layerCount > 1;
  const int totalChunks = layerCount * (int)job.chunks.size();

  // RC resolves one base seed up front (resolve_keybed_seed); a single keybed renders with it and
  // a layered one derives each layer's seed from it, so both are reproducible from the base.
  uint64_t baseSeed = job.seed >= 0 ? (uint64_t)job.seed : 0;
  if (job.seed < 0)
  {
    std::random_device device;
    baseSeed = std::uniform_int_distribution<uint64_t>(0, 2147483647u)(device);
  }

  const auto newManifest = [&](int layer) {
    KitManifest m;
    m.descriptor = job.layers[(size_t)layer];
    m.wet = job.wet;
    m.fx = job.fx;
    m.steps = job.steps;
    m.cfgScale = job.cfgScale;
    m.model = job.variant;
    m.encoding = job.encoding;
    m.range = job.rangeLabel;
    m.seed = layered ? kb::layer_seed(baseSeed, layer) : baseSeed;
    for (const auto& chunk : job.chunks)
      m.labelMidis.insert(m.labelMidis.end(), chunk.label_midis.begin(), chunk.label_midis.end());
    return m;
  };
  std::vector<KitManifest> manifests;
  for (int l = 0; l < layerCount; ++l)
    manifests.push_back(newManifest(l));

  std::string kitDir;
  const auto layerDir = [&](int layer) {
    return layered ? JoinPath(kitDir, kb::kLayerDirNames[layer]) : kitDir;
  };
  const auto writeManifests = [&](bool complete) {
    if (kitDir.empty())
      return;
    std::string ignored;
    for (int l = 0; l < layerCount; ++l)
    {
      if (manifests[(size_t)l].soundingMidis.empty())
        continue;
      manifests[(size_t)l].complete = complete;
      WriteKitManifest(layerDir(l), manifests[(size_t)l], ignored);
      WriteKitSfz(layerDir(l), manifests[(size_t)l].soundingMidis, ignored);
    }
    if (layered)
    {
      // The top-level kit.json names the layers; kit.sfz plays them together at RC's mix.
      KitManifest top = manifests[0];
      top.seed = baseSeed;
      top.complete = complete;
      top.soundingMidis.clear();
      std::vector<std::vector<int>> midis;
      for (int l = 0; l < layerCount; ++l)
      {
        top.layerDescriptors.push_back(manifests[(size_t)l].descriptor);
        top.layerSeeds.push_back(manifests[(size_t)l].seed);
        midis.push_back(manifests[(size_t)l].soundingMidis);
      }
      WriteKitManifest(kitDir, top, ignored);
      WriteLayeredKitSfz(kitDir, midis, ignored);
    }
  };
  const auto countNotes = [&]() {
    size_t n = 0;
    for (const auto& m : manifests)
      n += m.soundingMidis.size();
    return n;
  };

  auto finish = [&](KeybedEvent::Kind kind, std::string message) {
    writeManifests(kind == KeybedEvent::Kind::Finished);
    {
      std::lock_guard<std::mutex> lock(mMutex);
      mActiveLabels.clear();
      mStatus = message;
    }
    KeybedEvent event;
    event.kind = kind;
    event.kitDir = kitDir;
    event.message = std::move(message);
    event.seed = baseSeed;
    Push(std::move(event));
    mBusy.store(false, std::memory_order_release);
  };

  std::string error;
  mApi = LoadSa3Api(error);
  if (!mApi)
    return finish(KeybedEvent::Kind::Failed, "libsa3: " + error);

  const std::string key = job.modelsDir + "|" + job.variant + "|" + job.encoding;
  if (mContext && key != mContextKey)
  {
    mApi->context_destroy(mContext);
    mContext = nullptr;
  }
  if (!mContext)
  {
    SetStatus("loading libsa3 context");
    sa3_error_v1 err = {};
    err.size = sizeof err;
    sa3_context_config_v1 config = {};
    config.size = sizeof config;
    mApi->context_config_init(&config);
    config.models_dir = job.modelsDir.c_str();
    config.variant = job.variant.c_str();
    config.dit_encoding = job.encoding.c_str();
    if (mApi->context_create(&config, &mContext, &err) != SA3_STATUS_OK_V1)
    {
      mContext = nullptr;
      return finish(KeybedEvent::Kind::Failed, std::string("models: ") + err.message);
    }
    mContextKey = key;
  }

  kitDir = CreateKitDirectory(job.layers[0], baseSeed);

  // Layer by layer, Main first (RC's order): the main keyboard is complete and playable after a
  // third of a layered build, and a cancel still leaves a finished single-layer instrument.
  int done = 0;
  for (int layer = 0; layer < layerCount; ++layer)
  {
    KitManifest& manifest = manifests[(size_t)layer];
    const std::string dir = layerDir(layer);
    const std::vector<std::string> fx = job.fx;
    for (size_t index = 0; index < job.chunks.size(); ++index, ++done)
    {
      const kb::Chunk& chunk = job.chunks[index];
      const auto chunkStarted = std::chrono::steady_clock::now();
      mChunk.store(done, std::memory_order_release);
      {
        std::lock_guard<std::mutex> lock(mMutex);
        mActiveLabels = chunk.label_midis;
      }
      const std::string prompt = kb::build_sequence_prompt(manifest.descriptor, chunk.label_midis, job.wet, &fx);
      CallbackState state{this, requestId, (int)index, (int)job.chunks.size(), done, totalChunks,
                          layered ? kb::kLayerRoles[layer] : ""};

      sa3_request_v1 request = {};
      request.size = sizeof request;
      mApi->request_init(&request);
      request.prompt = prompt.c_str();
      request.duration_seconds = chunk.actual_seconds;
      request.conditioning_seconds_total = chunk.seconds_total;
      request.steps = job.steps;
      request.cfg_scale = job.cfgScale;
      request.seed = (int64_t)manifest.seed;   // one seed for every chunk of a layer
      request.sampler = SA3_SAMPLER_DPMPP_3M_SDE_V1;
      request.sigma_min = 0.03f;
      request.sigma_max = 500.f;
      request.residency = SA3_RESIDENCY_RESIDENT_V1;   // the service decides residency between jobs
      request.loudness.peak_normalize = 0;              // raw: per-chunk gain would make levels jump
      request.loudness.limiter = 0;
      request.on_progress = &KeybedRenderService::OnProgress;
      request.should_cancel = &KeybedRenderService::ShouldCancel;
      request.callback_user = &state;

      sa3_result_v1 result = {};
      result.size = sizeof result;
      mApi->result_init(&result);
      sa3_error_v1 err = {};
      err.size = sizeof err;
      const sa3_status_v1 status = mApi->generate(mContext, &request, &result, &err);
      if (status == SA3_STATUS_CANCELLED_V1)
        return finish(KeybedEvent::Kind::Cancelled, "cancelled after " + std::to_string(countNotes()) + " notes");
      if (status != SA3_STATUS_OK_V1)
        return finish(KeybedEvent::Kind::Failed, std::string("generate: ") + err.message);

      const int channels = (int)result.n_channels;
      const int sampleRate = (int)result.sample_rate;
      std::vector<float> audio;
      try
      {
        audio = kb::finish_chunk_audio(result.samples, channels, (int64_t)result.n_samples, sampleRate,
                                       chunk.actual_seconds);
      }
      catch (const std::exception& e)
      {
        mApi->result_free(&result);
        return finish(KeybedEvent::Kind::Failed, std::string("slice: ") + e.what());
      }
      mApi->result_free(&result);
      const int64_t frames = (int64_t)audio.size() / std::max(1, channels);

      std::string ioError;
      if (!kitDir.empty())
      {
        char name[64];
        std::snprintf(name, sizeof name, "chunk_%02zu_labels_%s-%s.wav", index + 1,
                      kb::note_filename(chunk.label_midis.front()).c_str(),
                      kb::note_filename(chunk.label_midis.back()).c_str());
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(JoinPath(dir, "chunks")), ec);
        WritePlanarWav(JoinPath(dir, std::string("chunks/") + name), audio.data(), channels, (int)frames,
                       sampleRate, ioError);
      }

      for (const kb::NoteSlice& slice : kb::note_slices(chunk.label_midis, sampleRate, frames))
      {
        if (std::find(manifest.soundingMidis.begin(), manifest.soundingMidis.end(), slice.sounding_midi) !=
            manifest.soundingMidis.end())
          continue;
        const std::vector<float> planar = kb::extract_note_sample(audio, channels, sampleRate, slice);
        auto note = std::make_shared<NoteSample>();
        note->midi = slice.sounding_midi;
        note->layer = layer;
        note->sampleRate = sampleRate;
        note->frames = (int)(planar.size() / (size_t)channels);
        note->left.assign(planar.begin(), planar.begin() + note->frames);
        note->right.assign(planar.begin() + (channels > 1 ? note->frames : 0),
                           planar.begin() + (channels > 1 ? 2 * note->frames : note->frames));
        for (int i = 0; i < note->frames; ++i)
          note->peak = std::max({note->peak, std::fabs(note->left[(size_t)i]), std::fabs(note->right[(size_t)i])});
        if (!kitDir.empty())
          WriteNoteWav(JoinPath(dir, NoteFileName(note->midi)), *note, ioError);
        manifest.soundingMidis.push_back(note->midi);

        KeybedEvent event;
        event.kind = KeybedEvent::Kind::Note;
        event.note = std::move(note);
        event.kitDir = kitDir;
        event.seed = baseSeed;
        Push(std::move(event));
      }
      writeManifests(false);   // keep a usable kit on disk even if a later chunk fails
      mProgress.store((float)(done + 1) / (float)totalChunks, std::memory_order_release);
      mLastChunkSeconds.store(std::chrono::duration<double>(std::chrono::steady_clock::now() - chunkStarted).count(),
                              std::memory_order_release);
    }
  }

  char message[96];
  if (layered)
    std::snprintf(message, sizeof message, "%d layers x %zu notes ready · seed %llu", layerCount,
                  manifests[0].soundingMidis.size(), (unsigned long long)baseSeed);
  else
    std::snprintf(message, sizeof message, "%zu notes ready · seed %llu", manifests[0].soundingMidis.size(),
                  (unsigned long long)baseSeed);
  finish(KeybedEvent::Kind::Finished, message);
}

} // namespace keybed
