#include "FlacAudio.h"

#ifndef FLAC__NO_DLL
#define FLAC__NO_DLL 1
#endif
#include "third_party/flac/all.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>

namespace keybed
{
namespace
{

std::filesystem::path PathFromUtf8(const std::string& path)
{
#if defined(_WIN32) && __cplusplus < 202002L
  return std::filesystem::u8path(path);
#else
  return std::filesystem::path(path);
#endif
}

struct DecodeContext
{
  std::ifstream file;
  FLAC__uint64 fileSize = 0;
  FLAC__uint64 position = 0;
  NoteSamplePtr note;
  int midi = 60;
  int layer = 0;
  bool layered = false;
  int sampleRate = 0;
  unsigned channels = 0;
  unsigned bitsPerSample = 0;
  bool invalidFrame = false;
  FLAC__StreamDecoderErrorStatus error = FLAC__STREAM_DECODER_ERROR_STATUS_LOST_SYNC;
  bool sawError = false;
};

struct EncodeContext
{
  std::fstream file;
};

FLAC__StreamEncoderWriteStatus EncodeWrite(const FLAC__StreamEncoder*, const FLAC__byte buffer[], size_t bytes,
                                           uint32_t, uint32_t, void* opaque)
{
  auto& context = *static_cast<EncodeContext*>(opaque);
  context.file.write(reinterpret_cast<const char*>(buffer), (std::streamsize)bytes);
  return context.file ? FLAC__STREAM_ENCODER_WRITE_STATUS_OK : FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
}

FLAC__StreamEncoderSeekStatus EncodeSeek(const FLAC__StreamEncoder*, FLAC__uint64 offset, void* opaque)
{
  auto& context = *static_cast<EncodeContext*>(opaque);
  if (offset > (FLAC__uint64)std::numeric_limits<std::streamoff>::max())
    return FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR;
  context.file.clear();
  context.file.seekp((std::streamoff)offset, std::ios::beg);
  return context.file ? FLAC__STREAM_ENCODER_SEEK_STATUS_OK : FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR;
}

FLAC__StreamEncoderTellStatus EncodeTell(const FLAC__StreamEncoder*, FLAC__uint64* offset, void* opaque)
{
  auto& context = *static_cast<EncodeContext*>(opaque);
  const auto position = context.file.tellp();
  if (position < 0)
    return FLAC__STREAM_ENCODER_TELL_STATUS_ERROR;
  *offset = (FLAC__uint64)position;
  return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
}

FLAC__StreamDecoderReadStatus DecodeRead(const FLAC__StreamDecoder*, FLAC__byte buffer[], size_t* bytes, void* opaque)
{
  auto& context = *static_cast<DecodeContext*>(opaque);
  if (*bytes == 0)
    return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
  if (context.position >= context.fileSize)
  {
    *bytes = 0;
    return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
  }
  const auto remaining = context.fileSize - context.position;
  const auto request = (size_t)std::min<FLAC__uint64>(remaining, (FLAC__uint64)*bytes);
  context.file.read(reinterpret_cast<char*>(buffer), (std::streamsize)request);
  const auto readCount = context.file.gcount();
  if (readCount <= 0)
  {
    *bytes = 0;
    return context.file.bad() ? FLAC__STREAM_DECODER_READ_STATUS_ABORT
                              : FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
  }
  *bytes = (size_t)readCount;
  context.position += (FLAC__uint64)readCount;
  return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

FLAC__StreamDecoderSeekStatus DecodeSeek(const FLAC__StreamDecoder*, FLAC__uint64 offset, void* opaque)
{
  auto& context = *static_cast<DecodeContext*>(opaque);
  if (offset > context.fileSize || offset > (FLAC__uint64)std::numeric_limits<std::streamoff>::max())
    return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
  context.file.clear();
  context.file.seekg((std::streamoff)offset, std::ios::beg);
  if (!context.file)
    return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
  context.position = offset;
  return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}

FLAC__StreamDecoderTellStatus DecodeTell(const FLAC__StreamDecoder*, FLAC__uint64* offset, void* opaque)
{
  *offset = static_cast<DecodeContext*>(opaque)->position;
  return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

FLAC__StreamDecoderLengthStatus DecodeLength(const FLAC__StreamDecoder*, FLAC__uint64* length, void* opaque)
{
  *length = static_cast<DecodeContext*>(opaque)->fileSize;
  return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}

FLAC__bool DecodeEof(const FLAC__StreamDecoder*, void* opaque)
{
  const auto& context = *static_cast<DecodeContext*>(opaque);
  return context.position >= context.fileSize;
}

FLAC__StreamDecoderWriteStatus DecodeFrame(const FLAC__StreamDecoder*, const FLAC__Frame* frame,
                                           const FLAC__int32* const buffers[], void* opaque)
{
  auto& context = *static_cast<DecodeContext*>(opaque);
  const unsigned channels = frame->header.channels;
  const unsigned bits = frame->header.bits_per_sample;
  const unsigned count = frame->header.blocksize;
  const unsigned rate = frame->header.sample_rate;
  if (channels == 0 || channels > FLAC__MAX_CHANNELS || bits == 0 || bits > 32 || rate == 0 ||
      count > (unsigned)std::numeric_limits<int>::max())
  {
    context.invalidFrame = true;
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }
  if (context.sampleRate && (context.sampleRate != (int)rate || context.channels != channels ||
                             context.bitsPerSample != bits))
  {
    context.invalidFrame = true;
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }
  context.sampleRate = (int)rate;
  context.channels = channels;
  context.bitsPerSample = bits;
  auto mutableNote = std::const_pointer_cast<NoteSample>(context.note);
  if (!mutableNote)
  {
    mutableNote = std::make_shared<NoteSample>();
    mutableNote->midi = context.midi;
    mutableNote->layer = context.layer;
    mutableNote->layered = context.layered;
    mutableNote->sampleRate = (int)rate;
    context.note = mutableNote;
  }
  if (mutableNote->left.size() + count > (size_t)std::numeric_limits<int>::max())
  {
    context.invalidFrame = true;
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }
  mutableNote->left.reserve(mutableNote->left.size() + count);
  mutableNote->right.reserve(mutableNote->right.size() + count);
  const double scale = std::ldexp(1.0, (int)bits - 1);
  for (unsigned i = 0; i < count; ++i)
  {
    const float left = (float)((double)buffers[0][i] / scale);
    const float right = (float)((double)buffers[std::min(1u, channels - 1)][i] / scale);
    mutableNote->left.push_back(left);
    mutableNote->right.push_back(right);
    mutableNote->peak = std::max({mutableNote->peak, std::fabs(left), std::fabs(right)});
  }
  return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void DecodeMetadata(const FLAC__StreamDecoder*, const FLAC__StreamMetadata*, void*) {}

void DecodeError(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus status, void* opaque)
{
  auto& context = *static_cast<DecodeContext*>(opaque);
  context.error = status;
  context.sawError = true;
}

} // namespace

bool WritePlanarFlac(const std::string& path, const float* planar, int channels, int frames, int sampleRate,
                     std::string& error)
{
  if (!planar || channels < 1 || channels > (int)FLAC__MAX_CHANNELS || frames <= 0 || sampleRate <= 0)
  {
    error = "invalid audio dimensions for FLAC";
    return false;
  }

  const std::filesystem::path outputPath = PathFromUtf8(path);
  std::error_code ec;
  if (!outputPath.parent_path().empty())
    std::filesystem::create_directories(outputPath.parent_path(), ec);
  if (ec)
  {
    error = "cannot create folder for " + path + ": " + ec.message();
    return false;
  }
  FLAC__StreamEncoder* encoder = FLAC__stream_encoder_new();
  if (!encoder)
  {
    error = "cannot allocate FLAC encoder";
    return false;
  }
  bool configured = FLAC__stream_encoder_set_channels(encoder, (uint32_t)channels) &&
                    FLAC__stream_encoder_set_bits_per_sample(encoder, 24) &&
                    FLAC__stream_encoder_set_sample_rate(encoder, (uint32_t)sampleRate) &&
                    FLAC__stream_encoder_set_compression_level(encoder, 5) &&
                    FLAC__stream_encoder_set_do_mid_side_stereo(encoder, channels == 2) &&
                    FLAC__stream_encoder_set_loose_mid_side_stereo(encoder, channels == 2);
  EncodeContext io;
  if (configured)
    io.file.open(outputPath, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
  FLAC__StreamEncoderInitStatus initStatus = FLAC__STREAM_ENCODER_INIT_STATUS_ENCODER_ERROR;
  if (configured && io.file.is_open())
    initStatus = FLAC__stream_encoder_init_stream(encoder, EncodeWrite, EncodeSeek, EncodeTell, nullptr, &io);
  if (!configured || !io.file.is_open() || initStatus != FLAC__STREAM_ENCODER_INIT_STATUS_OK)
  {
    error = !configured ? "cannot configure FLAC encoder"
            : !io.file.is_open() ? "cannot open FLAC output: " + path
                                 : std::string("cannot initialize FLAC output: ") + FLAC__StreamEncoderInitStatusString[initStatus];
    FLAC__stream_encoder_delete(encoder);
    std::filesystem::remove(outputPath, ec);
    return false;
  }

  std::vector<FLAC__int32> interleaved((size_t)frames * (size_t)channels);
  constexpr double kScale = 8388608.0;
  constexpr long long kMin = -8388608;
  constexpr long long kMax = 8388607;
  for (int frame = 0; frame < frames; ++frame)
  {
    for (int channel = 0; channel < channels; ++channel)
    {
      const double sample = std::clamp((double)planar[(size_t)channel * (size_t)frames + (size_t)frame], -1.0, 1.0);
      const long long pcm = std::clamp(std::llround(sample * kScale), kMin, kMax);
      interleaved[(size_t)frame * (size_t)channels + (size_t)channel] = (FLAC__int32)pcm;
    }
  }

  const bool processed = FLAC__stream_encoder_process_interleaved(encoder, interleaved.data(), (uint32_t)frames) != 0;
  const bool finished = FLAC__stream_encoder_finish(encoder) != 0;
  const FLAC__StreamEncoderState state = FLAC__stream_encoder_get_state(encoder);
  io.file.flush();
  const bool fileGood = (bool)io.file;
  io.file.close();
  FLAC__stream_encoder_delete(encoder);
  if (!processed || !finished || !fileGood)
  {
    error = !fileGood ? "failed writing FLAC output: " + path
                      : std::string("failed while encoding FLAC: ") + FLAC__StreamEncoderStateString[state];
    std::filesystem::remove(outputPath, ec);
    return false;
  }
  return true;
}

NoteSamplePtr ReadNoteFlac(const std::string& path, int midi, std::string& error, int layer, bool layered)
{
  DecodeContext context;
  context.midi = midi;
  context.layer = layer;
  context.layered = layered;
  const std::filesystem::path inputPath = PathFromUtf8(path);
  std::error_code ec;
  const auto fileSize = std::filesystem::file_size(inputPath, ec);
  if (ec || fileSize > (uintmax_t)std::numeric_limits<std::streamoff>::max())
  {
    error = path + ": cannot read FLAC file size";
    return nullptr;
  }
  context.fileSize = (FLAC__uint64)fileSize;
  context.file.open(inputPath, std::ios::binary);
  if (!context.file)
  {
    error = path + ": cannot open FLAC file";
    return nullptr;
  }
  FLAC__StreamDecoder* decoder = FLAC__stream_decoder_new();
  if (!decoder)
  {
    error = "cannot allocate FLAC decoder";
    return nullptr;
  }
  const FLAC__StreamDecoderInitStatus initStatus = FLAC__stream_decoder_init_stream(
      decoder, DecodeRead, DecodeSeek, DecodeTell, DecodeLength, DecodeEof, DecodeFrame, DecodeMetadata, DecodeError, &context);
  if (initStatus != FLAC__STREAM_DECODER_INIT_STATUS_OK)
  {
    error = std::string("cannot open FLAC file: ") + FLAC__StreamDecoderInitStatusString[initStatus];
    FLAC__stream_decoder_delete(decoder);
    return nullptr;
  }
  const bool decoded = FLAC__stream_decoder_process_until_end_of_stream(decoder) != 0;
  const FLAC__StreamDecoderState state = FLAC__stream_decoder_get_state(decoder);
  FLAC__stream_decoder_finish(decoder);
  FLAC__stream_decoder_delete(decoder);
  auto note = std::const_pointer_cast<NoteSample>(context.note);
  if (!decoded || context.invalidFrame || context.sawError || !note || note->left.empty())
  {
    error = context.invalidFrame ? path + ": unsupported FLAC stream layout"
           : context.sawError ? path + ": " + FLAC__StreamDecoderErrorStatusString[context.error]
           : std::string(path) + ": " + FLAC__StreamDecoderStateString[state];
    return nullptr;
  }
  note->frames = (int)note->left.size();
  return note;
}

} // namespace keybed
