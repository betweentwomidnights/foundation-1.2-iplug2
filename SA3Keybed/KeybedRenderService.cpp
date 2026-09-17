#include "KeybedRenderService.h"

#include "Sa3Runtime.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace kb = sa3::sat::keybed;

namespace keybed
{
namespace
{

struct CallbackState
{
  KeybedRenderService* service = nullptr;
  uint64_t requestId = 0;
  int chunk = 0;
  int chunks = 1;
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
  if (kb::split_descriptor_tokens(job.descriptor).body.empty())
  {
    error = "describe an instrument first (or roll the dice)";
    return false;
  }
  mCancel.store(false, std::memory_order_release);
  mProgress.store(0.f, std::memory_order_release);
  mChunk.store(0, std::memory_order_release);
  mTotalChunks.store((int)job.chunks.size(), std::memory_order_release);
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
  const float overall = ((float)state->chunk + std::clamp(progress->fraction, 0.f, 1.f)) / (float)std::max(1, state->chunks);
  self.mProgress.store(overall, std::memory_order_release);
  char text[128];
  if (progress->stage == SA3_PROGRESS_SAMPLING_V1 && progress->total > 0)
    std::snprintf(text, sizeof text, "chunk %d/%d · sampling %d/%d", state->chunk + 1, state->chunks,
                  progress->step, progress->total);
  else
    std::snprintf(text, sizeof text, "chunk %d/%d · %s", state->chunk + 1, state->chunks,
                  progress->stage_name ? progress->stage_name : "working");
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
  KitManifest manifest;
  manifest.descriptor = job.descriptor;
  manifest.wet = job.wet;
  manifest.fx = job.fx;
  manifest.steps = job.steps;
  manifest.cfgScale = job.cfgScale;
  manifest.model = job.variant;
  manifest.encoding = job.encoding;
  manifest.range = job.rangeLabel;
  for (const auto& chunk : job.chunks)
    manifest.labelMidis.insert(manifest.labelMidis.end(), chunk.label_midis.begin(), chunk.label_midis.end());

  std::string kitDir;
  auto finish = [&](KeybedEvent::Kind kind, std::string message) {
    if (!kitDir.empty() && !manifest.soundingMidis.empty())
    {
      std::string ignored;
      manifest.complete = kind == KeybedEvent::Kind::Finished;
      WriteKitManifest(kitDir, manifest, ignored);
      WriteKitSfz(kitDir, manifest.soundingMidis, ignored);
    }
    {
      std::lock_guard<std::mutex> lock(mMutex);
      mActiveLabels.clear();
      mStatus = message;
    }
    KeybedEvent event;
    event.kind = kind;
    event.kitDir = kitDir;
    event.message = std::move(message);
    event.seed = manifest.seed;
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

  int64_t seed = job.seed;
  for (size_t index = 0; index < job.chunks.size(); ++index)
  {
    const kb::Chunk& chunk = job.chunks[index];
    mChunk.store((int)index, std::memory_order_release);
    {
      std::lock_guard<std::mutex> lock(mMutex);
      mActiveLabels = chunk.label_midis;
    }
    const std::string prompt = kb::build_sequence_prompt(job.descriptor, chunk.label_midis, job.wet,
                                                         job.fx.empty() ? nullptr : &job.fx);
    CallbackState state{this, requestId, (int)index, (int)job.chunks.size()};

    sa3_request_v1 request = {};
    request.size = sizeof request;
    mApi->request_init(&request);
    request.prompt = prompt.c_str();
    request.duration_seconds = chunk.actual_seconds;
    request.conditioning_seconds_total = chunk.seconds_total;
    request.steps = job.steps;
    request.cfg_scale = job.cfgScale;
    request.seed = seed;
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
      return finish(KeybedEvent::Kind::Cancelled, "cancelled after " + std::to_string(manifest.soundingMidis.size()) + " notes");
    if (status != SA3_STATUS_OK_V1)
      return finish(KeybedEvent::Kind::Failed, std::string("generate: ") + err.message);

    if (seed < 0)
    {
      seed = (int64_t)result.seed;
      manifest.seed = result.seed;
    }
    else
      manifest.seed = (uint64_t)seed;
    if (kitDir.empty())
      kitDir = CreateKitDirectory(job.descriptor, manifest.seed);

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
      WritePlanarWav(JoinPath(kitDir, std::string("chunks/") + name), audio.data(), channels, (int)frames,
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
      note->sampleRate = sampleRate;
      note->frames = (int)(planar.size() / (size_t)channels);
      note->left.assign(planar.begin(), planar.begin() + note->frames);
      note->right.assign(planar.begin() + (channels > 1 ? note->frames : 0),
                         planar.begin() + (channels > 1 ? 2 * note->frames : note->frames));
      for (int i = 0; i < note->frames; ++i)
        note->peak = std::max({note->peak, std::fabs(note->left[(size_t)i]), std::fabs(note->right[(size_t)i])});
      if (!kitDir.empty())
        WriteNoteWav(JoinPath(kitDir, NoteFileName(note->midi)), *note, ioError);
      manifest.soundingMidis.push_back(note->midi);

      KeybedEvent event;
      event.kind = KeybedEvent::Kind::Note;
      event.note = std::move(note);
      event.kitDir = kitDir;
      event.seed = manifest.seed;
      Push(std::move(event));
    }
    if (!kitDir.empty())
    {
      std::string ignored;
      WriteKitManifest(kitDir, manifest, ignored);   // keep a usable kit on disk even if a later chunk fails
    }
    mProgress.store((float)(index + 1) / (float)job.chunks.size(), std::memory_order_release);
  }

  if (!kitDir.empty())
  {
    std::string sfzError;
    WriteKitSfz(kitDir, manifest.soundingMidis, sfzError);
  }
  char done[96];
  std::snprintf(done, sizeof done, "%zu notes ready · seed %llu", manifest.soundingMidis.size(),
                (unsigned long long)manifest.seed);
  finish(KeybedEvent::Kind::Finished, done);
}

} // namespace keybed
