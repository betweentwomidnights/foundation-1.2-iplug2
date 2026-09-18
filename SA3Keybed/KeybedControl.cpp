#include "KeybedControl.h"

#include "SA3Keybed.h"
#include "SA3UIPrimitives.h"
#include "SA3UITheme.h"

#include "sat/keybed.h"

#if defined(OS_WIN)
#include <shobjidl.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <random>

namespace kb = sa3::sat::keybed;
using namespace gary::ui;

namespace
{
constexpr const char* kFont = gary::ui::FontName;
constexpr int kKeyboardLow = 24;    // C1: the lowest sounding key of every range
constexpr int kKeyboardHigh = 96;   // C7

std::string Compact(std::string text, size_t maxChars)
{
  if (text.size() <= maxChars)
    return text;
  return maxChars <= 3 ? text.substr(0, maxChars) : text.substr(0, maxChars - 3) + "...";
}

size_t FitChars(float width, float charWidth)
{
  return (size_t)std::max(4.f, width / std::max(1.f, charWidth));
}

bool IsBlackKey(int midi)
{
  const int pc = ((midi % 12) + 12) % 12;
  return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

IText Label(float size, const IColor& color, EAlign align = EAlign::Near)
{
  return IText(size, color, kFont, align, EVAlign::Middle);
}

int64_t ParseSeed(const char* text, int64_t fallback)
{
  if (!text)
    return fallback;
  while (std::isspace((unsigned char)*text)) ++text;
  if (!*text || *text == '-')
    return fallback;
  errno = 0;
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(text, &end, 10);
  if (end == text || errno == ERANGE)
    return fallback;
  return (int64_t)std::min<unsigned long long>(parsed, (unsigned long long)std::numeric_limits<int64_t>::max());
}

// Sounding pitch, note count, and chunk count for RC's C2-B5 / C2-F6 / C2-B6 prompt-label ranges.
const char* RangeName(int index)
{
  static const char* names[] = {"C1-B4", "C1-F5", "C1-B5"};
  return names[std::clamp(index, 0, 2)];
}

int RangeNotes(int index)
{
  static const int notes[] = {48, 54, 60};
  return notes[std::clamp(index, 0, 2)];
}

int RangeChunks(int index)
{
  return RangeNotes(index) / 6;
}

std::string ShortDuration(double seconds)
{
  char text[32];
  if (seconds < 120.0)
    std::snprintf(text, sizeof text, "~%.0fs", seconds);
  else
    std::snprintf(text, sizeof text, "~%.0fm", seconds / 60.0);
  return text;
}
} // namespace

KeybedControl::KeybedControl(const IRECT& bounds, SA3Keybed& plugin)
: IControl(bounds)
, mPlugin(plugin)
{
  SetTextEntryLength(1024);
  mIgnoreMouse = false;
}

// ---------------------------------------------------------------------------------------------------
// Drawing

void KeybedControl::Draw(IGraphics& g)
{
  g.FillRect(Background(), mRECT);
  const IRECT shell = mRECT.GetPadded(-14.f);
  g.FillRoundRect(Panel(), shell, 7.f);
  g.DrawRoundRect(Frame(), shell, 7.f);
  mParamSliders.clear();
  mKnobs.clear();
  mSheetHits.clear();
  if (mSettingsOpen)
    DrawSettings(g, shell);
  else if (mSoundOpen)
    DrawSoundSheet(g, shell);
  else
    DrawMain(g, shell);
}

void KeybedControl::DrawMain(IGraphics& g, const IRECT& shell)
{
  const float left = shell.L + 18.f;
  const float right = shell.R - 18.f;
  float y = shell.T + 14.f;
  y = DrawHeader(g, left, right, y);
  y = DrawStatus(g, left, right, y);
  y = DrawDescriptor(g, left, right, y);
  y = DrawGeneration(g, left, right, y);
  y = DrawActions(g, left, right, y);
  y = DrawKeyboard(g, left, right, y);
  y = DrawNoteWaveform(g, left, right, y);
  y = DrawSound(g, left, right, y);
  DrawKit(g, left, right, y);
}

float KeybedControl::DrawHeader(IGraphics& g, float left, float right, float y)
{
  g.DrawText(Label(TitleTextSize, COLOR_WHITE), "keybed", IRECT(left, y, left + 90.f, y + 26.f));
  g.DrawText(Label(11.f, TextFaint()), "foundation-1.2", IRECT(left + 72.f, y + 4.f, left + 170.f, y + 26.f));
  mSettingsRect = IRECT(right - 70.f, y + 2.f, right, y + 24.f);
  DrawButton(g, mSettingsRect, "settings", kFont);
  const bool ready = mPlugin.ModelsReady();
  char label[64];
  if (mPlugin.Downloading())
    std::snprintf(label, sizeof label, "downloading %s %.0f%%", mPlugin.DownloadingTier().c_str(),
                  mPlugin.DownloadProgress() * 100.f);
  else
    std::snprintf(label, sizeof label, "%s", ready ? (mPlugin.Encoding() + " ready").c_str() : "no models - open settings");
  g.DrawText(Label(12.f, mPlugin.Downloading() ? TextDim() : ready ? Green() : Red(), EAlign::Far), label,
             IRECT(left + 170.f, y, mSettingsRect.L - 8.f, y + 26.f));
  return y + 30.f;
}

float KeybedControl::DrawStatus(IGraphics& g, float left, float right, float y)
{
  const IRECT bar(left, y, right, y + 22.f);
  g.FillRoundRect(PanelDark(), bar, 3.f);
  if (mPlugin.Busy())
    g.FillRoundRect(RedDim(), IRECT(bar.L, bar.T, bar.L + bar.W() * std::clamp(mPlugin.Progress(), 0.f, 1.f), bar.B), 3.f);
  g.DrawRoundRect(FrameSoft(), bar, 3.f);
  const std::string status = mPlugin.StatusText();
  const IColor color = !mPlugin.Busy() && mPlugin.StatusIsError() ? Red() : COLOR_WHITE;
  g.DrawText(Label(12.f, color), Compact(status, FitChars(bar.W() - 16.f, 6.4f)).c_str(), bar.GetHPadded(-8.f));
  if (mPlugin.Busy() || mPlugin.StatusIsError())
    SetTooltip(status.c_str());
  return y + 30.f;
}

float KeybedControl::DrawDescriptor(IGraphics& g, float left, float right, float y)
{
  // Typing sorts the text onto the sound sheet's controls; "edit" opens those controls.
  g.DrawText(Label(12.f, TextDim()), "sound", IRECT(left, y, left + 120.f, y + 16.f));
  mDiceRect = IRECT(right - 34.f, y + 18.f, right, y + 50.f);
  DrawIconButton(g, mDiceRect, TransportIcon::Dice);
  mEditSoundRect = IRECT(mDiceRect.L - 52.f, y + 18.f, mDiceRect.L - 6.f, y + 50.f);
  DrawButton(g, mEditSoundRect, "edit", kFont);
  mDescriptorRect = IRECT(left, y + 18.f, mEditSoundRect.L - 6.f, y + 50.f);
  g.FillRoundRect(ButtonFill(), mDescriptorRect, 3.f);
  g.DrawRoundRect(Frame(), mDescriptorRect, 3.f);
  const std::string descriptor = mPlugin.Descriptor();
  const bool empty = descriptor.empty();
  g.DrawText(Label(13.f, empty ? TextDim() : COLOR_WHITE),
             Compact(empty ? "type a sound, e.g. Grand Piano, Warm" : descriptor,
                     FitChars(mDescriptorRect.W() - 16.f, 6.2f)).c_str(),
             mDescriptorRect.GetHPadded(-8.f));
  const std::string fx = mPlugin.FxLabel();
  const std::string space = !mPlugin.Wet() ? "dry - add reverb and delay in your DAW"
                          : fx.empty()     ? "wet - the model picks the space"
                                           : "wet - " + fx;
  g.DrawText(Label(10.f, mPlugin.Wet() ? TextDim() : TextFaint()),
             Compact(space, FitChars(mDescriptorRect.W(), 5.4f)).c_str(),
             IRECT(left + 2.f, y + 52.f, right, y + 66.f));
  if (mPlugin.LayerCount() > 1)
  {
    // Layered keybed: the support layers render after main, in the same space.
    const std::string supports = "+ " + mPlugin.LayerSummary();   // "edit" opens the layer tabs
    g.DrawText(Label(10.f, TextDim()), Compact(supports, FitChars(right - left, 5.4f)).c_str(),
               IRECT(left + 2.f, y + 66.f, right, y + 80.f));
    return y + 86.f;
  }
  return y + 72.f;
}

float KeybedControl::DrawGeneration(IGraphics& g, float left, float right, float y)
{
  char value[32];
  std::snprintf(value, sizeof value, "%d", mPlugin.Steps());
  DrawSlider(g, IRECT(left, y, right, y + 24.f), "steps", value, (mPlugin.Steps() - 2.f) / 148.f, mStepsRect);
  y += 26.f;
  std::snprintf(value, sizeof value, "%.1f", mPlugin.CfgScale());
  DrawSlider(g, IRECT(left, y, right, y + 24.f), "cfg", value, (mPlugin.CfgScale() - 1.f) / 11.f, mCfgRect);
  y += 28.f;

  // seed: locked value in white, otherwise the last seed greyed (click "use" to lock it)
  g.DrawText(Label(11.f, TextDim()), "seed", IRECT(left, y, left + 86.f, y + 24.f));
  DrawToggle(g, IRECT(left + 92.f, y, left + 140.f, y + 24.f), "use", mPlugin.UseSeed(), mSeedToggleRect);
  mSeedFieldRect = IRECT(left + 142.f, y + 1.f, right, y + 23.f);
  g.FillRoundRect(ButtonFill(), mSeedFieldRect, 3.f);
  g.DrawRoundRect(Frame(), mSeedFieldRect, 3.f);
  const std::string seedText = mPlugin.UseSeed() ? std::to_string(mPlugin.SeedValue())
                             : mPlugin.HasLastSeed() ? std::to_string(mPlugin.LastSeed()) + "  (last)"
                                                     : std::string("random");
  g.DrawText(Label(11.f, mPlugin.UseSeed() ? COLOR_WHITE : TextDim()), seedText.c_str(), mSeedFieldRect.GetHPadded(-6.f));
  return y + 34.f;
}

float KeybedControl::DrawActions(IGraphics& g, float left, float right, float y)
{
  const bool busy = mPlugin.Busy();
  const bool ready = mPlugin.ModelsReady();

  // preview: 6 / 12 / 24 notes from a root, one to four chunks
  g.DrawText(Label(11.f, TextDim()), "preview", IRECT(left, y, left + 86.f, y + 28.f));
  static const int counts[] = {6, 12, 24};
  float x = left + 92.f;
  for (int i = 0; i < 3; ++i)
  {
    mPreviewCountRects[(size_t)i] = IRECT(x, y + 2.f, x + 34.f, y + 26.f);
    DrawTab(g, mPreviewCountRects[(size_t)i], std::to_string(counts[i]).c_str(), kFont, mPlugin.PreviewCount() == counts[i]);
    x += 38.f;
  }
  mPreviewRootRect = IRECT(x + 2.f, y + 2.f, x + 64.f, y + 26.f);
  const std::string root = "from " + kb::midi_to_note_name(kb::label_to_sounding_midi(mPlugin.PreviewRootLabel()));
  DrawDropButton(g, mPreviewRootRect, root.c_str());
  mPreviewRect = IRECT(right - 100.f, y, right, y + 28.f);
  DrawButton(g, mPreviewRect, busy ? "cancel" : "preview", kFont, false, busy || ready);
  y += 36.f;

  // Build size: how many keys get generated, where they land, and roughly how long that takes here.
  g.DrawText(Label(11.f, TextDim()), "build size", IRECT(left, y, left + 86.f, y + 28.f));
  mBuildRect = IRECT(right - 100.f, y, right, y + 28.f);
  DrawButton(g, mBuildRect, busy ? "cancel" : "build", kFont, false, busy || ready);
  const float tabsR = mBuildRect.L - 10.f;
  const float tabW = (tabsR - (left + 92.f) - 12.f) / 3.f;
  x = left + 92.f;
  for (int i = 0; i < 3; ++i)
  {
    mRangeRects[(size_t)i] = IRECT(x, y + 2.f, x + tabW, y + 26.f);
    char label[48];
    std::snprintf(label, sizeof label, "%d keys", RangeNotes(i));
    DrawTab(g, mRangeRects[(size_t)i], label, kFont, (int)mPlugin.Range() == i);
    x += tabW + 6.f;
  }
  y += 30.f;
  const int index = (int)mPlugin.Range();
  char detail[96];
  const int layers = mPlugin.LayerCount();
  const std::string eta = ShortDuration(mPlugin.EstimatedSeconds(layers * RangeChunks(index)));
  if (layers > 1)   // every layer is a full keyboard of its own
    std::snprintf(detail, sizeof detail, "%s · %d layers x %d chunks · %s at %d steps", RangeName(index), layers,
                  RangeChunks(index), eta.c_str(), mPlugin.Steps());
  else
    std::snprintf(detail, sizeof detail, "%s · %d chunks of 6 notes · %s at %d steps", RangeName(index),
                  RangeChunks(index), eta.c_str(), mPlugin.Steps());
  g.DrawText(Label(10.f, TextFaint()), detail, IRECT(left + 92.f, y, right, y + 14.f));
  return y + 20.f;
}

float KeybedControl::DrawKeyboard(IGraphics& g, float left, float right, float y)
{
  const keybed::BankSnapshot* bank = mPlugin.Bank();
  g.DrawText(Label(11.f, TextDim()), "keys", IRECT(left, y, left + 80.f, y + 16.f));
  const std::string count = std::to_string(bank ? bank->count : 0) + " notes · click or play MIDI";
  g.DrawText(Label(11.f, TextFaint(), EAlign::Far), count.c_str(), IRECT(left + 80.f, y, right, y + 16.f));
  y += 18.f;

  mKeyboardRect = IRECT(left, y, right, y + 96.f);
  int whiteCount = 0;
  for (int k = kKeyboardLow; k <= kKeyboardHigh; ++k)
    whiteCount += IsBlackKey(k) ? 0 : 1;
  const float whiteW = mKeyboardRect.W() / (float)whiteCount;
  const float blackW = whiteW * 0.62f;
  const float blackH = mKeyboardRect.H() * 0.6f;

  std::vector<int> active;
  if (mPlugin.Busy())
    for (int label : mPlugin.ActiveLabels())
      active.push_back(kb::label_to_sounding_midi(label));
  const float pulse = 0.5f + 0.5f * std::sin((float)std::chrono::duration<double>(
                        std::chrono::steady_clock::now().time_since_epoch()).count() * 6.f);
  auto fillFor = [&](int key, bool black) {
    if (mPlugin.KeyHeld(key) || key == mPressedKey) return Green();
    if (std::find(active.begin(), active.end(), key) != active.end())
      return IColor((int)(60 + 120 * pulse), 230, 32, 32);
    if (bank && bank->exact[(size_t)key]) return black ? Red() : RedDim();
    return black ? PanelDark() : ButtonFill();
  };

  int white = 0;
  for (int k = kKeyboardLow; k <= kKeyboardHigh; ++k)
  {
    if (IsBlackKey(k)) continue;
    const IRECT r(mKeyboardRect.L + white * whiteW, mKeyboardRect.T, mKeyboardRect.L + (white + 1) * whiteW, mKeyboardRect.B);
    g.FillRect(fillFor(k, false), r.GetPadded(-0.5f));
    g.DrawRect(FrameSoft(), r);
    if (k % 12 == 0)
      g.DrawText(IText(9.f, TextDim(), kFont, EAlign::Center, EVAlign::Bottom), kb::midi_to_note_name(k).c_str(),
                 IRECT(r.L - 4.f, r.B - 14.f, r.R + 4.f, r.B - 2.f));
    ++white;
  }
  white = 0;
  for (int k = kKeyboardLow; k <= kKeyboardHigh; ++k)
  {
    if (!IsBlackKey(k)) { ++white; continue; }
    const float cx = mKeyboardRect.L + white * whiteW;
    const IRECT r(cx - blackW * 0.5f, mKeyboardRect.T, cx + blackW * 0.5f, mKeyboardRect.T + blackH);
    g.FillRect(fillFor(k, true), r);
    g.DrawRect(Frame(), r);
  }
  g.DrawRect(Frame(), mKeyboardRect);
  return mKeyboardRect.B + 8.f;
}

float KeybedControl::DrawNoteWaveform(IGraphics& g, float left, float right, float y)
{
  // A layered keybed adds the supports line and the layer mix row; the waveform gives up the room.
  const keybed::BankSnapshot* bank = mPlugin.Bank();
  const bool layered = mPlugin.LayerCount() > 1 || (bank && bank->layered);
  const IRECT box(left, y, right, y + (layered ? 52.f : 76.f));
  mWaveformRect = box;
  g.FillRoundRect(PanelDark(), box, 4.f);
  g.DrawRoundRect(FrameSoft(), box, 4.f);

  int key = mPressedKey >= 0 ? mPressedKey : mPlugin.LastPlayedKey();
  keybed::NoteSamplePtr note = mPlugin.SampleForKey(key);
  mWaveformKey = note ? key : -1;
  if (!note)
  {
    g.DrawText(Label(11.f, TextFaint(), EAlign::Center), "play a key to see its sample", box);
    return box.B + 10.f;
  }
  const int width = std::max(8, (int)box.W() - 16);
  if (note.get() != mPeaksFor || width != mPeaksWidth)
  {
    mPeaks.assign((size_t)width, {0.f, 0.f});
    for (int i = 0; i < note->frames; ++i)
    {
      auto& p = mPeaks[(size_t)std::min(width - 1, (int)((int64_t)i * width / std::max(1, note->frames)))];
      const float v = 0.5f * (note->left[(size_t)i] + note->right[(size_t)i]);
      p.first = std::min(p.first, v);
      p.second = std::max(p.second, v);
    }
    mPeaksFor = note.get();
    mPeaksWidth = width;
  }
  const float mid = box.MH();
  const float half = box.H() * 0.4f / std::max(0.05f, note->peak);
  for (int i = 0; i < width; ++i)
  {
    const float x = box.L + 8.f + (float)i;
    g.DrawLine(Red(), x, mid - mPeaks[(size_t)i].second * half, x, mid - mPeaks[(size_t)i].first * half, nullptr, 1.f);
  }
  char info[64];
  std::snprintf(info, sizeof info, "%s · %.2fs%s", kb::midi_to_note_name(key).c_str(),
                (double)note->frames / note->sampleRate, mPlugin.NoteFilePath(key).empty() ? "" : " · drag to export");
  g.DrawText(Label(10.f, TextDim(), EAlign::Far), info, IRECT(box.L, box.T + 2.f, box.R - 8.f, box.T + 16.f));
  return box.B + 10.f;
}

float KeybedControl::DrawSound(IGraphics& g, float left, float right, float y)
{
  g.DrawText(Label(12.f, TextDim()), "playback", IRECT(left, y, left + 80.f, y + 16.f));
  y += 18.f;
  static const std::pair<int, const char*> sliders[] = {
    {kParamGain, "gain"}, {kParamAttack, "attack"}, {kParamDecay, "decay"}, {kParamSustain, "sustain"},
    {kParamRelease, "release"}, {kParamVelocity, "velocity"}, {kParamTune, "tune"},
  };
  for (const auto& [param, label] : sliders)
  {
    DrawParamSlider(g, IRECT(left, y, right, y + 24.f), label, param);
    y += 25.f;
  }
  y = DrawLayerMix(g, left, right, y);

  g.DrawText(Label(11.f, TextDim()), "octave", IRECT(left, y, left + 86.f, y + 26.f));
  mOctaveDownRect = IRECT(left + 92.f, y + 2.f, left + 118.f, y + 24.f);
  mOctaveUpRect = IRECT(left + 150.f, y + 2.f, left + 176.f, y + 24.f);
  DrawButton(g, mOctaveDownRect, "-", kFont);
  DrawButton(g, mOctaveUpRect, "+", kFont);
  const int octave = mPlugin.GetParam(kParamOctave)->Int();
  g.DrawText(Label(12.f, COLOR_WHITE, EAlign::Center), (octave > 0 ? "+" + std::to_string(octave) : std::to_string(octave)).c_str(),
             IRECT(mOctaveDownRect.R, y, mOctaveUpRect.L, y + 26.f));
  DrawToggle(g, IRECT(left + 200.f, y, right, y + 26.f), "fill gaps by repitching", mPlugin.GetParam(kParamFillGaps)->Bool(),
             mFillGapsRect);
  return y + 34.f;
}

float KeybedControl::DrawLayerMix(IGraphics& g, float left, float right, float y)
{
  // RC's tri-layer mixer, shown once the kit (or the next build) has support layers.
  const keybed::BankSnapshot* bank = mPlugin.Bank();
  const bool layeredKit = bank && bank->layered;
  const int layers = layeredKit ? std::max(mPlugin.LayerCount(), bank->layerCount) : mPlugin.LayerCount();
  if (layers < 2)
    return y;
  if (bank && bank->count > 0 && !layeredKit)
  {
    // The sheet has supports but the kit playing is single-layer: nothing to mix yet.
    g.DrawText(Label(10.f, TextFaint()), "layer mix - build to hear the support layers", IRECT(left, y, right, y + 24.f));
    return y + 25.f;
  }
  static const char* const names[] = {"main", "sup 1", "sup 2"};
  const float gap = 10.f;
  const float w = (right - left - gap * 2.f) / 3.f;
  for (int l = 0; l < layers && l < 3; ++l)
  {
    const float x = left + l * (w + gap);
    const IRECT cell(x, y, x + w, y + 24.f);
    const IParam* p = mPlugin.GetParam(kParamLayerMain + l);
    if (layeredKit && !bank->hasLayer[(size_t)l])
    {
      // Not rendered yet (supports follow main), or the build stopped before this layer.
      const std::string state = std::string(names[l]) + (mPlugin.Busy() ? " - rendering..." : " - not built");
      g.DrawText(Label(10.f, TextFaint()), state.c_str(), cell);
      continue;
    }
    char value[16];
    std::snprintf(value, sizeof value, "%d%%", (int)std::lround(p->Value()));
    g.DrawText(Label(10.f, TextDim()), names[l], IRECT(cell.L, cell.T, cell.L + 34.f, cell.B));
    g.DrawText(Label(10.f, COLOR_WHITE, EAlign::Far), value, IRECT(cell.R - 30.f, cell.T, cell.R, cell.B));
    const IRECT sr(cell.L + 36.f, cell.MH() - 8.f, cell.R - 34.f, cell.MH() + 8.f);
    const IRECT track(sr.L, sr.MH() - 2.f, sr.R, sr.MH() + 2.f);
    g.FillRoundRect(FrameSoft(), track, 2.f);
    const float filled = sr.L + sr.W() * (float)p->GetNormalized();
    g.FillRoundRect(Red(), IRECT(track.L, track.T, filled, track.B), 2.f);
    g.FillCircle(COLOR_WHITE, filled, sr.MH(), 5.f);
    mParamSliders.push_back({kParamLayerMain + l, sr});
  }
  return y + 25.f;
}

float KeybedControl::DrawKit(IGraphics& g, float left, float right, float y)
{
  const IRECT row(left, y, right, y + 30.f);
  g.FillRoundRect(PanelDark(), row, 4.f);
  g.DrawRoundRect(FrameSoft(), row, 4.f);
  mLoadKitRect = IRECT(row.R - 74.f, row.T + 4.f, row.R - 6.f, row.B - 4.f);
  mRevealKitRect = IRECT(mLoadKitRect.L - 30.f, row.T + 4.f, mLoadKitRect.L - 6.f, row.B - 4.f);
  DrawButton(g, mLoadKitRect, "load kit", kFont);
  const bool hasKit = !mPlugin.KitDir().empty();
  DrawFolderIcon(g, mRevealKitRect, hasKit ? COLOR_WHITE : TextFaint());
  mKitLabelRect = IRECT(row.L + 8.f, row.T, mRevealKitRect.L - 6.f, row.B);
  const std::string label = mPlugin.LoadingKit() ? "loading kit..." : mPlugin.KitLabel();
  g.DrawText(Label(11.f, hasKit ? COLOR_WHITE : TextDim()),
             Compact(label, FitChars(mKitLabelRect.W() - 8.f, 6.f)).c_str(), mKitLabelRect);
  return row.B + 8.f;
}

void KeybedControl::DrawSettings(IGraphics& g, const IRECT& shell)
{
  const float left = shell.L + 18.f;
  const float right = shell.R - 18.f;
  float y = shell.T + 14.f;
  g.DrawText(Label(18.f, COLOR_WHITE), "settings", IRECT(left, y, right - 42.f, y + 28.f));
  mCloseRect = IRECT(right - 32.f, y + 2.f, right, y + 26.f);
  DrawButton(g, mCloseRect, "x", kFont);
  y += 42.f;

  // models: pick a tier, then a folder that has it or a download into that folder
  IRECT card(left, y, right, y + 196.f);
  g.FillRoundRect(PanelDark(), card, 5.f);
  g.DrawRoundRect(FrameSoft(), card, 5.f);
  g.DrawText(Label(14.f, COLOR_WHITE), "Foundation-1.2 Keybeds models", IRECT(card.L + 12.f, card.T + 8.f, card.R - 12.f, card.T + 30.f));
  const bool downloading = mPlugin.Downloading();
  const auto& tiers = keybed::ModelTiers();
  const float tabW = (card.W() - 24.f - 6.f * (float)(tiers.size() - 1)) / (float)tiers.size();
  float x = card.L + 12.f;
  for (size_t i = 0; i < tiers.size() && i < mEncodingRects.size(); ++i)
  {
    mEncodingRects[i] = IRECT(x, card.T + 36.f, x + tabW, card.T + 60.f);
    DrawTab(g, mEncodingRects[i], tiers[i].c_str(), kFont, mPlugin.Encoding() == tiers[i]);
    x += tabW + 6.f;
  }
  const std::string tier = mPlugin.Encoding();
  const std::string size = keybed::HumanBytes(keybed::ModelTierBytes(tier));
  g.DrawText(Label(10.f, TextFaint()),
             (tier + " - " + size + ", three files" + (tier == "F16" ? " - reference quality" : tier == "Q4_K_M" ? " - smallest and fastest to load" : "")).c_str(),
             IRECT(card.L + 12.f, card.T + 62.f, card.R - 12.f, card.T + 76.f));

  g.DrawText(Label(11.f, TextDim()), "folder", IRECT(card.L + 12.f, card.T + 80.f, card.L + 60.f, card.T + 104.f));
  mModelsFolderRect = IRECT(card.L + 60.f, card.T + 80.f, card.R - 12.f, card.T + 104.f);
  DrawDropButton(g, mModelsFolderRect, Compact(mPlugin.ModelsDir(), FitChars(mModelsFolderRect.W() - 24.f, 6.f)).c_str());

  const bool present = mPlugin.TierPresent(tier);
  mDownloadRect = IRECT(card.L + 12.f, card.T + 112.f, card.L + 172.f, card.T + 138.f);
  if (downloading)
  {
    DrawButton(g, mDownloadRect, "cancel download", kFont);
    const IRECT meter(mDownloadRect.R + 10.f, card.T + 118.f, card.R - 12.f, card.T + 132.f);
    g.FillRoundRect(ButtonFill(), meter, 3.f);
    g.FillRoundRect(RedDim(), IRECT(meter.L, meter.T, meter.L + meter.W() * std::clamp(mPlugin.DownloadProgress(), 0.f, 1.f), meter.B), 3.f);
    g.DrawRoundRect(FrameSoft(), meter, 3.f);
    g.DrawText(Label(10.f, TextDim()), Compact(mPlugin.DownloadStatus(), FitChars(card.W() - 24.f, 5.4f)).c_str(),
               IRECT(card.L + 12.f, card.T + 144.f, card.R - 12.f, card.T + 160.f));
  }
  else
  {
    DrawButton(g, mDownloadRect, present ? ("re-check " + tier).c_str() : ("download " + tier + " - " + size).c_str(), kFont,
               false, !mPlugin.Busy() || !present);
    g.DrawText(Label(11.f, present ? Green() : Red()), present ? "all three files found" : "not in this folder yet",
               IRECT(mDownloadRect.R + 10.f, mDownloadRect.T, card.R - 12.f, mDownloadRect.B));
  }
  g.DrawMultiLineText(IText(10.f, TextFaint(), kFont, EAlign::Near, EVAlign::Top),
                      "downloads from huggingface.co/thepatch/foundation-1.2-keybeds-GGUF into the folder above "
                      "and resume if interrupted. or choose a folder that already has the files.",
                      IRECT(card.L + 12.f, card.T + 164.f, card.R - 12.f, card.B - 4.f));
  y = card.B + 12.f;

  // lifecycle
  card = IRECT(left, y, right, y + 92.f);
  g.FillRoundRect(PanelDark(), card, 5.f);
  g.DrawRoundRect(FrameSoft(), card, 5.f);
  g.DrawText(Label(14.f, COLOR_WHITE), "model lifecycle", IRECT(card.L + 12.f, card.T + 8.f, card.R - 12.f, card.T + 30.f));
  g.DrawText(Label(10.f, TextDim()), "models always stay loaded across the chunks of one build",
             IRECT(card.L + 12.f, card.T + 30.f, card.R - 12.f, card.T + 46.f));
  DrawToggle(g, IRECT(card.L + 12.f, card.T + 52.f, card.L + 220.f, card.T + 80.f), "keep resident between builds",
             mPlugin.KeepResident(), mResidentRect);
  mReleaseRect = IRECT(card.R - 112.f, card.T + 54.f, card.R - 12.f, card.T + 78.f);
  DrawButton(g, mReleaseRect, "release now", kFont, false, !mPlugin.Busy());
  y = card.B + 12.f;

  // about pitch
  card = IRECT(left, y, right, y + 112.f);
  g.FillRoundRect(PanelDark(), card, 5.f);
  g.DrawRoundRect(FrameSoft(), card, 5.f);
  g.DrawText(Label(14.f, COLOR_WHITE), "how keys are made", IRECT(card.L + 12.f, card.T + 8.f, card.R - 12.f, card.T + 30.f));
  g.DrawMultiLineText(IText(10.f, TextDim(), kFont, EAlign::Near, EVAlign::Top),
                      "each chunk renders six chromatic notes in one 20 s pass with one shared seed, then "
                      "slices them into 3 s samples (RoyalCities' keybed recipe). the model renders one "
                      "octave below its prompt labels, so samples are keyed by their real pitch: key 60 "
                      "plays middle C. kits are saved with an .sfz under Documents/sa3-keybed/kits.",
                      IRECT(card.L + 12.f, card.T + 34.f, card.R - 12.f, card.B - 6.f));
}

void KeybedControl::DrawSlider(IGraphics& g, const IRECT& bounds, const char* label, const char* valueText,
                               float fraction, IRECT& sliderRect)
{
  g.DrawText(Label(11.f, TextDim()), label, IRECT(bounds.L, bounds.T, bounds.L + 86.f, bounds.B));
  g.DrawText(Label(11.f, COLOR_WHITE, EAlign::Far), valueText, IRECT(bounds.R - 64.f, bounds.T, bounds.R, bounds.B));
  const IRECT sr(bounds.L + 92.f, bounds.MH() - 8.f, bounds.R - 72.f, bounds.MH() + 8.f);
  const IRECT track(sr.L, sr.MH() - 2.f, sr.R, sr.MH() + 2.f);
  g.FillRoundRect(FrameSoft(), track, 2.f);
  const float filled = sr.L + sr.W() * std::clamp(fraction, 0.f, 1.f);
  g.FillRoundRect(Red(), IRECT(track.L, track.T, filled, track.B), 2.f);
  g.FillCircle(COLOR_WHITE, filled, sr.MH(), 6.f);
  sliderRect = sr;
}

void KeybedControl::DrawParamSlider(IGraphics& g, const IRECT& bounds, const char* label, int param)
{
  const IParam* p = mPlugin.GetParam(param);
  WDL_String display;
  p->GetDisplay(display);
  std::string text = display.Get();
  if (p->GetLabel() && *p->GetLabel())
    text += std::string(" ") + p->GetLabel();
  ParamSlider slider;
  slider.param = param;
  DrawSlider(g, bounds, label, text.c_str(), (float)p->GetNormalized(), slider.rect);
  mParamSliders.push_back(slider);
}

void KeybedControl::DrawToggle(IGraphics& g, const IRECT& bounds, const char* label, bool on, IRECT& hitRect)
{
  hitRect = bounds;
  const IRECT box(bounds.L, bounds.MH() - 8.f, bounds.L + 16.f, bounds.MH() + 8.f);
  g.DrawRoundRect(on ? Red() : Frame(), box, 2.f);
  if (on)
    g.FillRoundRect(Red(), box.GetPadded(-4.f), 1.f);
  g.DrawText(Label(11.f, on ? COLOR_WHITE : TextDim()), label, IRECT(box.R + 7.f, bounds.T, bounds.R, bounds.B));
}

void KeybedControl::DrawDropButton(IGraphics& g, const IRECT& bounds, const char* text)
{
  g.FillRoundRect(ButtonFill(), bounds, 3.f);
  g.DrawRoundRect(Frame(), bounds, 3.f);
  g.DrawText(Label(11.f, COLOR_WHITE), text, IRECT(bounds.L + 8.f, bounds.T, bounds.R - 14.f, bounds.B));
  const float cx = bounds.R - 9.f, cy = bounds.MH();
  g.FillTriangle(TextDim(), cx - 4.f, cy - 2.f, cx + 4.f, cy - 2.f, cx, cy + 3.f);
}

// ---------------------------------------------------------------------------------------------------
// Interaction

void KeybedControl::OnMouseDown(float x, float y, const IMouseMod& mod)
{
  mDrag = Drag::None;
  if (mSettingsOpen)
  {
    if (mCloseRect.Contains(x, y)) mSettingsOpen = false;
    else if (mModelsFolderRect.Contains(x, y))
    {
      const std::string dir = PickDirectory(mPlugin.ModelsDir(), "Choose the models folder");
      if (!dir.empty()) mPlugin.SetModelsDir(dir);
    }
    else if (mDownloadRect.Contains(x, y))
    {
      // "re-check" also runs the downloader: it skips files already complete and repairs the rest.
      if (mPlugin.Downloading()) mPlugin.CancelModelDownload();
      else mPlugin.StartModelDownload();
    }
    else if (mResidentRect.Contains(x, y)) mPlugin.SetKeepResident(!mPlugin.KeepResident());
    else if (mReleaseRect.Contains(x, y) && !mPlugin.Busy()) mPlugin.ReleaseModels();
    else
    {
      const auto& tiers = keybed::ModelTiers();
      for (size_t i = 0; i < tiers.size() && i < mEncodingRects.size() && !mPlugin.Downloading(); ++i)
        if (mEncodingRects[i].Contains(x, y)) mPlugin.SetEncoding(tiers[i]);
    }
    SetDirty(false);
    return;
  }

  if (mSoundOpen)
  {
    OnSheetMouseDown(x, y, mod);
    SetDirty(false);
    return;
  }

  if (mSettingsRect.Contains(x, y)) { mSettingsOpen = true; SetDirty(false); return; }
  if (mDiceRect.Contains(x, y)) { mPlugin.RollAll(); SetDirty(false); return; }
  if (mEditSoundRect.Contains(x, y)) { mPlugin.SetEditLayer(0); mSoundOpen = true; SetDirty(false); return; }
  if (mDescriptorRect.Contains(x, y) && GetUI())
  {
    mEdit = Edit::Descriptor;
    GetUI()->CreateTextEntry(*this, IText(13.f, COLOR_WHITE, kFont, EAlign::Near, EVAlign::Middle).WithTEColors(PanelDark(), COLOR_WHITE),
                             mDescriptorRect, mPlugin.Descriptor().c_str(), 0);
    return;
  }
  if (mStepsRect.Contains(x, y)) { mDrag = Drag::Steps; mDragRect = mStepsRect; OnMouseDrag(x, y, 0, 0, mod); return; }
  if (mCfgRect.Contains(x, y)) { mDrag = Drag::Cfg; mDragRect = mCfgRect; OnMouseDrag(x, y, 0, 0, mod); return; }
  if (mSeedToggleRect.Contains(x, y))
  {
    if (!mPlugin.UseSeed() && mPlugin.HasLastSeed())
      mPlugin.SetSeedValue((int64_t)(mPlugin.LastSeed() & 0x7fffffffffffffffull));
    mPlugin.SetUseSeed(!mPlugin.UseSeed());
    SetDirty(false);
    return;
  }
  if (mSeedFieldRect.Contains(x, y) && GetUI())
  {
    mEdit = Edit::Seed;
    const int64_t seed = mPlugin.UseSeed() ? mPlugin.SeedValue() : (int64_t)(mPlugin.LastSeed() & 0x7fffffffffffffffull);
    GetUI()->CreateTextEntry(*this, IText(11.f, COLOR_WHITE, kFont, EAlign::Near, EVAlign::Middle).WithTEColors(PanelDark(), COLOR_WHITE),
                             mSeedFieldRect, std::to_string(seed).c_str(), 0);
    return;
  }
  static const int counts[] = {6, 12, 24};
  for (int i = 0; i < 3; ++i)
    if (mPreviewCountRects[(size_t)i].Contains(x, y))
    {
      mPlugin.SetPreviewCount(counts[i]);
      mPlugin.SetPreviewRootLabel(mPlugin.PreviewRootLabel());
      SetDirty(false);
      return;
    }
  if (mPreviewRootRect.Contains(x, y)) { OpenPreviewRootMenu(); return; }
  for (int i = 0; i < 3; ++i)
    if (mRangeRects[(size_t)i].Contains(x, y)) { mPlugin.SetRange((SA3Keybed::RangeChoice)i); SetDirty(false); return; }
  if (mPreviewRect.Contains(x, y) || mBuildRect.Contains(x, y))
  {
    if (mPlugin.Busy()) mPlugin.CancelRender();
    else if (mPreviewRect.Contains(x, y)) mPlugin.StartPreview();
    else mPlugin.StartFullBuild();
    SetDirty(false);
    return;
  }
  if (mKeyboardRect.Contains(x, y)) { mDrag = Drag::Key; PressKey(KeyAt(x, y)); SetDirty(false); return; }
  for (const auto& slider : mParamSliders)
    if (slider.rect.Contains(x, y))
    {
      mDrag = Drag::Param;
      mDragParam = slider.param;
      mDragRect = slider.rect;
      mPlugin.BeginInformHostOfParamChangeFromUI(slider.param);
      SetParamFromX(slider.param, slider.rect, x);
      return;
    }
  auto stepOctave = [&](int delta) {
    const int value = std::clamp(mPlugin.GetParam(kParamOctave)->Int() + delta, -2, 2);
    mPlugin.BeginInformHostOfParamChangeFromUI(kParamOctave);
    mPlugin.SendParameterValueFromUI(kParamOctave, mPlugin.GetParam(kParamOctave)->ToNormalized(value));
    mPlugin.EndInformHostOfParamChangeFromUI(kParamOctave);
  };
  if (mOctaveDownRect.Contains(x, y)) { stepOctave(-1); SetDirty(false); return; }
  if (mOctaveUpRect.Contains(x, y)) { stepOctave(1); SetDirty(false); return; }
  if (mFillGapsRect.Contains(x, y))
  {
    const bool next = !mPlugin.GetParam(kParamFillGaps)->Bool();
    mPlugin.BeginInformHostOfParamChangeFromUI(kParamFillGaps);
    mPlugin.SendParameterValueFromUI(kParamFillGaps, next ? 1. : 0.);
    mPlugin.EndInformHostOfParamChangeFromUI(kParamFillGaps);
    SetDirty(false);
    return;
  }
  auto armDragOut = [&](const std::string& path, const IRECT& rect) {
    if (path.empty())
      return false;
    mDrag = Drag::FileOut;
    mDragOutPath = path;
    mDragOutRect = rect;
    mDragStartX = x;
    mDragStartY = y;
    return true;
  };
  if (mWaveformRect.Contains(x, y) && mWaveformKey >= 0 && armDragOut(mPlugin.NoteFilePath(mWaveformKey), mWaveformRect))
    return;
  if (mKitLabelRect.Contains(x, y) && armDragOut(mPlugin.KitDir(), mKitLabelRect))
    return;
  if (mRevealKitRect.Contains(x, y) && GetUI() && !mPlugin.KitDir().empty())
  {
    WDL_String path(mPlugin.KitDir().c_str());
    GetUI()->RevealPathInExplorerOrFinder(path, false);
    return;
  }
  if (mLoadKitRect.Contains(x, y))
  {
    const std::string dir = PickDirectory(keybed::KitsDirectory(), "Choose a kit folder");   // every kit side by side
    if (!dir.empty()) mPlugin.LoadKitFromFolder(dir);
    SetDirty(false);
    return;
  }
}

void KeybedControl::OnMouseDblClick(float x, float y, const IMouseMod& mod)
{
  // double-click a sound slider to reset it
  for (const auto& slider : mParamSliders)
    if (slider.rect.Contains(x, y))
    {
      const IParam* p = mPlugin.GetParam(slider.param);
      mPlugin.BeginInformHostOfParamChangeFromUI(slider.param);
      mPlugin.SendParameterValueFromUI(slider.param, p->ToNormalized(p->GetDefault()));
      mPlugin.EndInformHostOfParamChangeFromUI(slider.param);
      SetDirty(false);
      return;
    }
  OnMouseDown(x, y, mod);
}

void KeybedControl::OnMouseDrag(float x, float y, float dX, float dY, const IMouseMod& mod)
{
  const float fraction = std::clamp((x - mDragRect.L) / std::max(1.f, mDragRect.W()), 0.f, 1.f);
  switch (mDrag)
  {
    case Drag::Steps: mPlugin.SetSteps(2 + (int)std::lround(fraction * 148.f)); break;
    case Drag::Cfg: mPlugin.SetCfgScale(std::round((1.f + fraction * 11.f) * 10.f) / 10.f); break;
    case Drag::Param: SetParamFromX(mDragParam, mDragRect, x); break;
    case Drag::Key:
    {
      const int key = KeyAt(x, y);
      if (key != mPressedKey)
      {
        ReleaseKey();
        PressKey(key);
      }
      break;
    }
    case Drag::FileOut:
      if (std::hypot(x - mDragStartX, y - mDragStartY) > 8.f && GetUI())
      {
        const std::string path = mDragOutPath;
        mDrag = Drag::None;
        mDragOutPath.clear();
        GetUI()->InitiateExternalFileDragDrop(path.c_str(), mDragOutRect);
      }
      return;
    case Drag::Knob:
    {
      // Vertical drag steps through the vocabulary (gary4juce's SteppedKnob feel): at least
      // 100 px of travel for the whole list, and never less than 3 px per step.
      const int steps = (int)KnobVocabulary(mActiveKnob.kind, mActiveKnob.index).size();
      if (steps > 1)
      {
        const float perStep = std::max(3.f, 100.f / (float)(steps - 1));
        const int step = std::clamp(mKnobDragStartStep + (int)std::lround((mDragStartY - y) / perStep), 0, steps - 1);
        SetKnobValue(mActiveKnob.kind, mActiveKnob.index,
                     KnobVocabulary(mActiveKnob.kind, mActiveKnob.index)[(size_t)step]);
      }
      break;
    }
    case Drag::None: return;
  }
  SetDirty(false);
}

void KeybedControl::OnMouseUp(float x, float y, const IMouseMod& mod)
{
  if (mDrag == Drag::Param)
    EndParamDrag();
  if (mDrag == Drag::Key)
    ReleaseKey();
  mDrag = Drag::None;
  SetDirty(false);
}

void KeybedControl::SetParamFromX(int param, const IRECT& rect, float x)
{
  const double normalized = std::clamp((double)(x - rect.L) / std::max(1., (double)rect.W()), 0., 1.);
  mPlugin.SendParameterValueFromUI(param, normalized);
  SetDirty(false);
}

void KeybedControl::EndParamDrag()
{
  if (mDragParam >= 0)
    mPlugin.EndInformHostOfParamChangeFromUI(mDragParam);
  mDragParam = -1;
}

int KeybedControl::KeyAt(float x, float y) const
{
  if (!mKeyboardRect.Contains(x, y))
    return -1;
  int whiteCount = 0;
  for (int k = kKeyboardLow; k <= kKeyboardHigh; ++k)
    whiteCount += IsBlackKey(k) ? 0 : 1;
  const float whiteW = mKeyboardRect.W() / (float)whiteCount;
  const float blackW = whiteW * 0.62f;
  if (y < mKeyboardRect.T + mKeyboardRect.H() * 0.6f)
  {
    int white = 0;
    for (int k = kKeyboardLow; k <= kKeyboardHigh; ++k)
    {
      if (!IsBlackKey(k)) { ++white; continue; }
      const float cx = mKeyboardRect.L + white * whiteW;
      if (std::fabs(x - cx) <= blackW * 0.5f)
        return k;
    }
  }
  const int index = std::clamp((int)((x - mKeyboardRect.L) / whiteW), 0, whiteCount - 1);
  int white = 0;
  for (int k = kKeyboardLow; k <= kKeyboardHigh; ++k)
  {
    if (IsBlackKey(k)) continue;
    if (white++ == index)
      return k;
  }
  return -1;
}

void KeybedControl::PressKey(int key)
{
  if (key < 0)
    return;
  mPressedKey = key;
  mPlugin.AuditionKey(key, true);
}

void KeybedControl::ReleaseKey()
{
  if (mPressedKey >= 0)
    mPlugin.AuditionKey(mPressedKey, false);
  mPressedKey = -1;
}

void KeybedControl::OnTextEntryCompletion(const char* str, int valIdx)
{
  if (mEdit == Edit::Descriptor)
    mPlugin.SetDescriptor(str ? str : "");
  else if (mEdit == Edit::NewExtra || mEdit == Edit::Extra)
  {
    // Extras are free text, but vocabulary words typed here still land on their controls.
    kb::SoundSpec sound = mPlugin.Sound();
    const std::string text = kb::detail::trim(str ? str : "");
    if (mEdit == Edit::Extra && mEditIndex >= 0 && mEditIndex < (int)sound.extras.size())
      sound.extras.erase(sound.extras.begin() + mEditIndex);
    if (!text.empty())
    {
      std::vector<std::string> fx = sound.fx;
      kb::SoundSpec merged = kb::classify_descriptor(kb::descriptor_of(sound) + ", " + text);
      merged.wet = sound.wet || merged.wet;
      for (const auto& tag : fx)
        kb::set_fx(merged, tag);
      sound = merged;
    }
    mPlugin.SetSound(std::move(sound));
    mEditIndex = -1;
  }
  else if (mEdit == Edit::Seed)
  {
    mPlugin.SetSeedValue(ParseSeed(str, mPlugin.SeedValue()));
    mPlugin.SetUseSeed(true);
  }
  mEdit = Edit::None;
  SetDirty(false);
}

void KeybedControl::OpenPreviewRootMenu()
{
  if (!GetUI())
    return;
  mMenu.Clear();
  const int maxRoot = kb::clamp_preview_root(127, mPlugin.PreviewCount());
  for (int label = kb::kPreviewRootMin; label <= maxRoot; ++label)
  {
    mMenu.AddItem(kb::midi_to_note_name(kb::label_to_sounding_midi(label)).c_str());
    if (label == mPlugin.PreviewRootLabel())
      mMenu.CheckItem(mMenu.NItems() - 1, true);
  }
  mPopup = Popup::PreviewRoot;
  GetUI()->CreatePopupMenu(*this, mMenu, mPreviewRootRect);
}

void KeybedControl::OnPopupMenuSelection(IPopupMenu* pMenu, int valIdx)
{
  const int index = pMenu ? pMenu->GetChosenItemIdx() : -1;
  if (index >= 0)
  {
    if (mPopup == Popup::PreviewRoot)
      mPlugin.SetPreviewRootLabel(kb::kPreviewRootMin + index);
    else
    {
      // List menus: an optional leading "none", then mMenuItems.
      const int item = index - (mMenuHasNone ? 1 : 0);
      const std::string value = item >= 0 && item < (int)mMenuItems.size() ? mMenuItems[(size_t)item] : std::string();
      kb::SoundSpec sound = mPlugin.Sound();
      switch (mPopup)
      {
        case Popup::Family:
          if (value != sound.family)
          {
            sound.family = value;
            sound.subfamily.clear();   // types belong to a family
          }
          break;
        case Popup::Type: sound.subfamily = value; break;
        case Popup::Second: sound.second_instrument = value; break;
        case Popup::Knob:
          if (!value.empty())
          {
            SetKnobValue(mActiveKnob.kind, mActiveKnob.index, value);
            sound = mPlugin.Sound();
          }
          break;
        default: break;
      }
      mPlugin.SetSound(std::move(sound));
    }
  }
  mPopup = Popup::None;
  SetDirty(false);
}

std::string KeybedControl::PickDirectory(const std::string& start, const char* title)
{
  if (!GetUI())
    return {};
#if defined(OS_WIN)
  // iPlug2's Windows prompt (SHBrowseForFolder) ignores the starting folder and opens at the top of
  // the machine; the shell's folder dialog opens where we point it.
  namespace fs = std::filesystem;
  std::string picked;
  const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  IFileOpenDialog* dialog = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
  {
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    const int titleChars = MultiByteToWideChar(CP_UTF8, 0, title, -1, nullptr, 0);
    std::wstring wideTitle((size_t)std::max(0, titleChars), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wideTitle.data(), titleChars);
    dialog->SetTitle(wideTitle.c_str());

    std::error_code ec;
    fs::path folder = fs::u8path(start);
    while (!folder.empty() && !fs::is_directory(folder, ec) && folder.has_parent_path() && folder.parent_path() != folder)
      folder = folder.parent_path();
    IShellItem* item = nullptr;
    if (!folder.empty() && fs::is_directory(folder, ec) &&
        SUCCEEDED(SHCreateItemFromParsingName(fs::absolute(folder, ec).wstring().c_str(), nullptr, IID_PPV_ARGS(&item))))
    {
      dialog->SetFolder(item);
      item->Release();
    }
    if (SUCCEEDED(dialog->Show((HWND)GetUI()->GetWindow())))
    {
      IShellItem* result = nullptr;
      if (SUCCEEDED(dialog->GetResult(&result)))
      {
        PWSTR path = nullptr;
        if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        {
          const auto utf8 = fs::path(path).u8string();
          picked.assign(utf8.begin(), utf8.end());
          CoTaskMemFree(path);
        }
        result->Release();
      }
    }
    dialog->Release();
  }
  if (SUCCEEDED(init))
    CoUninitialize();
  GetUI()->ReleaseMouseCapture();
  return picked;
#else
  (void)title;
  WDL_String dir(start.c_str());
  GetUI()->PromptForDirectory(dir);
  return dir.GetLength() > 0 ? std::string(dir.Get()) : std::string();
#endif
}

// ---------------------------------------------------------------------------------------------------
// Sound sheet: the descriptor as a knob surface, after gary4juce's Foundation panel. Each
// category is a toggle that reveals a stepped knob over its vocabulary; FX are pedals.

namespace
{
constexpr int kMaxCharacter = 8;   // RC rolls up to seven timbre tags
constexpr int kMaxExtras = 5;
constexpr const char* kFxNames[] = {"reverb", "delay", "distortion", "phaser", "bitcrush"};

const std::vector<std::string>& FxTokens(int category)
{
  static const std::vector<std::vector<std::string>> tokens = []() {
    std::vector<std::vector<std::string>> out;
    for (const auto& c : kb::vocab::fx_categories())
    {
      std::vector<std::string> names;
      for (const auto& t : c.tokens)
        names.emplace_back(t.first);
      out.push_back(std::move(names));
    }
    return out;
  }();
  static const std::vector<std::string> none;
  return category >= 0 && category < (int)tokens.size() ? tokens[(size_t)category] : none;
}

// The category's most common tag (RC's weights), used when a pedal is switched on.
std::string DefaultFxToken(int category)
{
  const auto& categories = kb::vocab::fx_categories();
  if (category < 0 || category >= (int)categories.size())
    return {};
  const auto& tokens = categories[(size_t)category].tokens;
  return std::max_element(tokens.begin(), tokens.end(),
                          [](const auto& a, const auto& b) { return a.second < b.second; })->first;
}

int FxCategoryIndex(const std::string& tag)
{
  const auto* category = kb::vocab::fx_category_of(tag);
  const auto& categories = kb::vocab::fx_categories();
  for (size_t i = 0; i < categories.size(); ++i)
    if (&categories[i] == category)
      return (int)i;
  return -1;
}

int IndexOf(const std::vector<std::string>& list, const std::string& value)
{
  const auto it = std::find(list.begin(), list.end(), value);
  return it == list.end() ? 0 : (int)std::distance(list.begin(), it);
}

IText EntryText(float size)
{
  return IText(size, COLOR_WHITE, kFont, EAlign::Near, EVAlign::Middle).WithTEColors(PanelDark(), COLOR_WHITE);
}
} // namespace

const std::vector<std::string>& KeybedControl::KnobVocabulary(KnobKind kind, int index) const
{
  switch (kind)
  {
    case KnobKind::Character: return kb::vocab::character_tags();
    case KnobKind::Articulation: return kb::vocab::articulation_tags();
    case KnobKind::Oscillator: return kb::vocab::oscillator_tags();
    case KnobKind::Fx: return FxTokens(index);
  }
  return kb::vocab::character_tags();
}

std::string KeybedControl::KnobValue(KnobKind kind, int index) const
{
  const kb::SoundSpec& sound = mPlugin.Sound();
  switch (kind)
  {
    case KnobKind::Character:
      return index >= 0 && index < (int)sound.character.size() ? sound.character[(size_t)index] : std::string();
    case KnobKind::Articulation: return sound.articulation;
    case KnobKind::Oscillator: return sound.oscillator;
    case KnobKind::Fx:
      for (const auto& tag : sound.fx)
        if (FxCategoryIndex(tag) == index)
          return tag;
      return {};
  }
  return {};
}

void KeybedControl::SetKnobValue(KnobKind kind, int index, const std::string& value)
{
  kb::SoundSpec sound = mPlugin.Sound();
  switch (kind)
  {
    case KnobKind::Character:
      if (index >= 0 && index < (int)sound.character.size())
        sound.character[(size_t)index] = value;
      break;
    case KnobKind::Articulation: sound.articulation = value; break;
    case KnobKind::Oscillator: sound.oscillator = value; break;
    case KnobKind::Fx: kb::set_fx(sound, value); break;
  }
  mPlugin.SetSound(std::move(sound));
}

void KeybedControl::StepKnob(const KnobHit& knob, int delta)
{
  const auto& vocabulary = KnobVocabulary(knob.kind, knob.index);
  if (vocabulary.empty())
    return;
  const int step = std::clamp(IndexOf(vocabulary, KnobValue(knob.kind, knob.index)) + delta, 0,
                              (int)vocabulary.size() - 1);
  SetKnobValue(knob.kind, knob.index, vocabulary[(size_t)step]);
}

const KeybedControl::KnobHit* KeybedControl::KnobAt(float x, float y) const
{
  for (const auto& knob : mKnobs)
    if (knob.rect.Contains(x, y))
      return &knob;
  return nullptr;
}

void KeybedControl::DrawKnob(IGraphics& g, const IRECT& bounds, const std::string& value, int step, int steps)
{
  // IGraphics arcs are in degrees clockwise from 12 o'clock: sweep 270 degrees, 7 to 5 o'clock.
  const float size = std::min(bounds.W(), bounds.H() - 14.f);
  const float cx = bounds.MW();
  const float cy = bounds.T + size * 0.5f;
  const float r = size * 0.5f - 3.f;
  const float fraction = steps > 1 ? (float)step / (float)(steps - 1) : 0.f;
  const float angle = -135.f + fraction * 270.f;
  g.DrawArc(FrameSoft(), cx, cy, r, -135.f, 135.f, nullptr, 3.f);
  g.DrawArc(Red(), cx, cy, r, -135.f, std::max(angle, -134.f), nullptr, 3.f);
  g.FillCircle(ButtonFill(), cx, cy, r * 0.5f);
  g.DrawCircle(Frame(), cx, cy, r * 0.5f);
  const float rad = angle * 3.14159265f / 180.f;
  g.DrawLine(COLOR_WHITE, cx + std::sin(rad) * r * 0.2f, cy - std::cos(rad) * r * 0.2f,
             cx + std::sin(rad) * (r - 5.f), cy - std::cos(rad) * (r - 5.f), nullptr, 2.f);

  // The value under the knob, shrunk to fit its cell.
  float fontSize = 10.f;
  IRECT measured;
  while (fontSize > 7.f)
  {
    if (g.MeasureText(IText(fontSize, COLOR_WHITE, kFont), value.c_str(), measured) <= bounds.W() + 8.f)
      break;
    fontSize -= 0.5f;
  }
  g.DrawText(IText(fontSize, COLOR_WHITE, kFont, EAlign::Center, EVAlign::Middle), value.c_str(),
             IRECT(bounds.L - 6.f, bounds.B - 14.f, bounds.R + 6.f, bounds.B));
}

void KeybedControl::DrawLock(IGraphics& g, const IRECT& bounds, bool locked)
{
  const IColor color = locked ? Red() : TextFaint();
  const float cx = bounds.MW();
  const IRECT body(cx - 5.f, bounds.MH() - 1.f, cx + 5.f, bounds.MH() + 6.f);
  // Shackle: closed over the body when locked, swung open when not.
  g.DrawArc(color, cx, body.T, 3.5f, -90.f, locked ? 90.f : 20.f, nullptr, 1.5f);
  if (locked)
    g.FillRoundRect(color, body, 1.5f);
  else
    g.DrawRoundRect(color, body, 1.5f, nullptr, 1.2f);
}

float KeybedControl::DrawSectionLabel(IGraphics& g, float left, float right, float y, const char* label,
                                      int lockSection, const char* addLabel, SheetAction addAction)
{
  g.DrawText(Label(11.f, TextDim()), label, IRECT(left, y, left + 160.f, y + 16.f));
  float x = right;
  if (lockSection >= 0)
  {
    const IRECT lock(right - 16.f, y, right, y + 16.f);
    DrawLock(g, lock, mPlugin.Locked(lockSection));
    mSheetHits.push_back({lock.GetPadded(3.f), SheetAction::Lock, lockSection});
    x = lock.L - 10.f;
  }
  if (addLabel)
  {
    const IRECT add(x - 130.f, y, x, y + 16.f);
    g.DrawText(Label(11.f, Red(), EAlign::Far), addLabel, add);
    mSheetHits.push_back({add, addAction, 0});
  }
  return y + 20.f;
}

float KeybedControl::DrawKnobRow(IGraphics& g, float left, float right, float y, const std::vector<KnobHit>& knobs,
                                 bool removable)
{
  if (knobs.empty())
    return y;
  const float gap = 10.f;
  const int n = (int)knobs.size();
  const float size = std::clamp((right - left - gap * (n - 1)) / (float)n, 44.f, 64.f);
  float x = (left + right) * 0.5f - (size * n + gap * (n - 1)) * 0.5f;
  for (int i = 0; i < n; ++i)
  {
    KnobHit knob = knobs[(size_t)i];
    knob.rect = IRECT(x, y, x + size, y + size + 14.f);
    const auto& vocabulary = KnobVocabulary(knob.kind, knob.index);
    const std::string value = KnobValue(knob.kind, knob.index);
    DrawKnob(g, knob.rect, value, IndexOf(vocabulary, value), (int)vocabulary.size());
    mKnobs.push_back(knob);
    if (removable)
    {
      const IRECT remove(x, knob.rect.B + 1.f, x + size, knob.rect.B + 15.f);
      g.DrawText(IText(10.f, TextFaint(), kFont, EAlign::Center, EVAlign::Middle), "remove", remove);
      mSheetHits.push_back({remove, SheetAction::RemoveCharacter, knob.index});
    }
    x += size + gap;
  }
  return y + size + 14.f + (removable ? 16.f : 0.f) + 8.f;
}

void KeybedControl::DrawSoundSheet(IGraphics& g, const IRECT& shell)
{
  const float left = shell.L + 18.f;
  const float right = shell.R - 18.f;
  const kb::SoundSpec& sound = mPlugin.Sound();
  float y = shell.T + 14.f;

  g.DrawText(Label(18.f, COLOR_WHITE), "sound", IRECT(left, y, left + 120.f, y + 28.f));
  g.DrawText(Label(10.f, TextFaint()), "drag, scroll, or right-click a knob", IRECT(left + 64.f, y + 4.f, right - 110.f, y + 28.f));
  const IRECT done(right - 56.f, y + 2.f, right, y + 26.f);
  const IRECT dice(done.L - 38.f, y, done.L - 8.f, y + 28.f);
  DrawButton(g, done, "done", kFont);
  DrawIconButton(g, dice, TransportIcon::Dice);
  mSheetHits.push_back({done, SheetAction::Done, 0});
  mSheetHits.push_back({dice, SheetAction::Dice, 0});
  y += 34.f;
  y = DrawLayerTabs(g, left, right, y);

  // instrument: family and type, plus RC's optional second (hybrid) instrument
  y = DrawSectionLabel(g, left, right, y, "instrument", SA3Keybed::kSectionInstrument,
                       sound.second_instrument.empty() ? "+ second instrument" : nullptr, SheetAction::AddSecond);
  const float half = (right - left - 8.f) * 0.5f;
  const IRECT family(left, y, left + half, y + 24.f);
  const IRECT type(family.R + 8.f, y, right, y + 24.f);
  DrawDropButton(g, family, sound.family.empty() ? "family" : sound.family.c_str());
  const bool hasTypes = !kb::vocab::subfamilies(sound.family).empty();
  DrawDropButton(g, type, !sound.subfamily.empty() ? sound.subfamily.c_str() : hasTypes ? "type" : "-");
  mSheetHits.push_back({family, SheetAction::Family, 0});
  if (hasTypes)
    mSheetHits.push_back({type, SheetAction::Type, 0});
  y += 30.f;
  if (!sound.second_instrument.empty())
  {
    const IRECT second(left, y, right - 60.f, y + 24.f);
    const IRECT remove(second.R + 6.f, y, right, y + 24.f);
    DrawDropButton(g, second, ("+ " + sound.second_instrument).c_str());
    g.DrawText(Label(10.f, TextFaint(), EAlign::Center), "remove", remove);
    mSheetHits.push_back({second, SheetAction::Second, 0});
    mSheetHits.push_back({remove, SheetAction::RemoveSecond, 0});
    y += 30.f;
  }
  y += 4.f;

  // character: timbre knobs
  y = DrawSectionLabel(g, left, right, y, "character", SA3Keybed::kSectionCharacter,
                       (int)sound.character.size() < kMaxCharacter ? "+ add" : nullptr, SheetAction::AddCharacter);
  std::vector<KnobHit> knobs;
  for (int i = 0; i < (int)sound.character.size(); ++i)
    knobs.push_back({IRECT(), KnobKind::Character, i});
  if (knobs.empty())
  {
    g.DrawText(Label(10.f, TextFaint()), "no character tags - add one or roll the dice", IRECT(left, y, right, y + 16.f));
    y += 22.f;
  }
  else
    y = DrawKnobRow(g, left, right, y, knobs, true);

  // shape: articulation and oscillator, each a toggle revealing its knob
  y = DrawSectionLabel(g, left, right, y, "shape", SA3Keybed::kSectionShape, nullptr, SheetAction::Done);
  const float mid = (left + right) * 0.5f;
  const IRECT articulation(mid - 116.f, y, mid - 4.f, y + 24.f);
  const IRECT oscillator(mid + 4.f, y, mid + 116.f, y + 24.f);
  DrawTab(g, articulation, "articulation", kFont, !sound.articulation.empty());
  DrawTab(g, oscillator, "oscillator", kFont, !sound.oscillator.empty());
  mSheetHits.push_back({articulation, SheetAction::Articulation, 0});
  mSheetHits.push_back({oscillator, SheetAction::Oscillator, 0});
  y += 30.f;
  knobs.clear();
  if (!sound.articulation.empty()) knobs.push_back({IRECT(), KnobKind::Articulation, 0});
  if (!sound.oscillator.empty()) knobs.push_back({IRECT(), KnobKind::Oscillator, 0});
  y = DrawKnobRow(g, left, right, y, knobs, false) + (knobs.empty() ? 4.f : 0.f);

  // render fx: dry/wet, then one pedal per FX category. Every layer renders in main's space.
  if (mPlugin.EditLayer() > 0)
  {
    g.DrawText(Label(11.f, TextDim()), "render fx", IRECT(left, y, left + 160.f, y + 16.f));
    y += 20.f;
    const std::string fx = mPlugin.FxLabel();
    const std::string shared = std::string("shared with main: ") +
                               (!sound.wet ? "dry" : fx.empty() ? "wet, the model picks the space" : "wet, " + fx);
    g.DrawText(Label(10.f, TextFaint()), Compact(shared, FitChars(right - left, 5.4f)).c_str(),
               IRECT(left, y, right, y + 16.f));
    y += 24.f;
  }
  else
  {
  y = DrawSectionLabel(g, left, right, y, "render fx", SA3Keybed::kSectionFx, nullptr, SheetAction::Done);
  const IRECT dry(left, y, left + 54.f, y + 24.f);
  const IRECT wet(dry.R + 6.f, y, dry.R + 60.f, y + 24.f);
  DrawTab(g, dry, "dry", kFont, !sound.wet);
  DrawTab(g, wet, "wet", kFont, sound.wet);
  mSheetHits.push_back({dry, SheetAction::Dry, 0});
  mSheetHits.push_back({wet, SheetAction::Wet, 0});
  const char* hint = !sound.wet ? "dry samples: add reverb and delay in your DAW"
                   : sound.fx.empty() ? "no pedals on: the model picks the space"
                                      : "baked into every sample";
  g.DrawText(Label(10.f, TextFaint()), hint, IRECT(wet.R + 10.f, y, right, y + 24.f));
  y += 30.f;
  if (sound.wet)
  {
    const float pedalW = (right - left - 4.f * 6.f) / 5.f;
    knobs.clear();
    for (int i = 0; i < 5; ++i)
    {
      const IRECT pedal(left + i * (pedalW + 6.f), y, left + i * (pedalW + 6.f) + pedalW, y + 24.f);
      const bool on = !KnobValue(KnobKind::Fx, i).empty();
      DrawTab(g, pedal, kFxNames[i], kFont, on);
      mSheetHits.push_back({pedal, SheetAction::Pedal, i});
      if (on)
        knobs.push_back({IRECT(), KnobKind::Fx, i});
    }
    y += 30.f;
    y = DrawKnobRow(g, left, right, y, knobs, false);
  }
  }
  y += 2.f;

  // extras: free text the vocabulary does not cover
  y = DrawSectionLabel(g, left, right, y, "extra descriptors", -1,
                       (int)sound.extras.size() < kMaxExtras ? "+ add" : nullptr, SheetAction::AddExtra);
  for (int i = 0; i < (int)sound.extras.size(); ++i)
  {
    const IRECT field(left, y, right - 60.f, y + 22.f);
    const IRECT remove(field.R + 6.f, y, right, y + 22.f);
    g.FillRoundRect(ButtonFill(), field, 3.f);
    g.DrawRoundRect(Frame(), field, 3.f);
    g.DrawText(Label(11.f, COLOR_WHITE), Compact(sound.extras[(size_t)i], FitChars(field.W() - 12.f, 6.f)).c_str(),
               field.GetHPadded(-6.f));
    g.DrawText(Label(10.f, TextFaint(), EAlign::Center), "remove", remove);
    mSheetHits.push_back({field, SheetAction::EditExtra, i});
    mSheetHits.push_back({remove, SheetAction::RemoveExtra, i});
    y += 26.f;
  }
  mNewExtraRect = IRECT(left, y, right - 60.f, y + 22.f);
  if (sound.extras.empty())
  {
    g.DrawText(Label(10.f, TextFaint()), "words outside the vocabulary, e.g. tape wobble", IRECT(left, y, right, y + 16.f));
    y += 20.f;
  }
  y += 4.f;

  // the prompt one chunk will send
  g.DrawText(Label(11.f, TextDim()), "prompt", IRECT(left, y, right, y + 16.f));
  y += 18.f;
  const IRECT preview(left, y, right, std::min(shell.B - 14.f, y + 58.f));
  g.FillRoundRect(PanelDark(), preview, 3.f);
  g.DrawRoundRect(FrameSoft(), preview, 3.f);
  const std::string prompt = kb::sequence_prompt_of(sound, {60, 61}) + ", ...";
  g.DrawMultiLineText(IText(10.f, TextDim(), kFont, EAlign::Near, EVAlign::Top), prompt.c_str(), preview.GetPadded(-6.f));
}

float KeybedControl::DrawLayerTabs(IGraphics& g, float left, float right, float y)
{
  // RC's multi-layered keybeds: a main layer plus up to two supports, each its own full render.
  const int count = mPlugin.LayerCount();
  static const char* const names[] = {"main", "support 1", "support 2"};
  float x = left;
  for (int l = 0; l < count; ++l)
  {
    const IRECT tab(x, y, x + 80.f, y + 24.f);
    DrawTab(g, tab, names[l], kFont, mPlugin.EditLayer() == l);
    mSheetHits.push_back({tab, SheetAction::Layer, l});
    x = tab.R + 6.f;
  }
  if (count < kb::kLayerCount)
  {
    const IRECT add(x, y, x + 76.f, y + 24.f);
    g.DrawText(Label(11.f, Red(), EAlign::Center), "+ layer", add);
    mSheetHits.push_back({add, SheetAction::AddLayer, 0});
    if (count == 1)
      g.DrawText(Label(10.f, TextFaint()), "stack support sounds", IRECT(add.R + 4.f, y, right, y + 24.f));
  }
  if (mPlugin.EditLayer() > 0)
  {
    const IRECT remove(right - 60.f, y, right, y + 24.f);
    g.DrawText(Label(10.f, TextFaint(), EAlign::Far), "remove", remove);
    mSheetHits.push_back({remove, SheetAction::RemoveLayer, mPlugin.EditLayer()});
  }
  return y + 32.f;
}

void KeybedControl::OpenListMenu(Popup popup, const std::vector<std::string>& items, const std::string& current,
                                 const IRECT& anchor, bool allowNone)
{
  if (!GetUI())
    return;
  mMenu.Clear();
  mMenuItems = items;
  mMenuHasNone = allowNone;
  if (allowNone)
  {
    mMenu.AddItem("none");
    if (current.empty())
      mMenu.CheckItem(0, true);
  }
  for (const auto& item : items)
  {
    mMenu.AddItem(item.c_str());
    if (item == current)
      mMenu.CheckItem(mMenu.NItems() - 1, true);
  }
  mPopup = popup;
  GetUI()->CreatePopupMenu(*this, mMenu, anchor);
}

void KeybedControl::OnSheetMouseDown(float x, float y, const IMouseMod& mod)
{
  if (const KnobHit* knob = KnobAt(x, y))
  {
    mActiveKnob = *knob;
    if (mod.R)   // right-click: the whole vocabulary, current value ticked
      OpenListMenu(Popup::Knob, KnobVocabulary(knob->kind, knob->index), KnobValue(knob->kind, knob->index),
                   knob->rect, false);
    else
    {
      mDrag = Drag::Knob;
      mDragStartY = y;
      mKnobDragStartStep = IndexOf(KnobVocabulary(knob->kind, knob->index), KnobValue(knob->kind, knob->index));
    }
    return;
  }

  const SheetHit* hit = nullptr;
  for (const auto& h : mSheetHits)
    if (h.rect.Contains(x, y))
    {
      hit = &h;
      break;
    }
  if (!hit)
    return;

  kb::SoundSpec sound = mPlugin.Sound();
  switch (hit->action)
  {
    case SheetAction::Done: mSoundOpen = false; return;
    case SheetAction::Dice: mPlugin.RollSound(); return;
    case SheetAction::Lock: mPlugin.SetLocked(hit->index, !mPlugin.Locked(hit->index)); return;
    case SheetAction::Layer: mPlugin.SetEditLayer(hit->index); return;
    case SheetAction::AddLayer: mPlugin.AddLayer(); return;
    case SheetAction::RemoveLayer: mPlugin.RemoveLayer(hit->index); return;
    case SheetAction::Family:
      OpenListMenu(Popup::Family, kb::vocab::all_families(), sound.family, hit->rect, true);
      return;
    case SheetAction::Type:
    {
      std::vector<std::string> types;
      for (const auto& sub : kb::vocab::subfamilies(sound.family))
        types.emplace_back(sub.first);
      OpenListMenu(Popup::Type, types, sound.subfamily, hit->rect, true);
      return;
    }
    case SheetAction::AddSecond:
    case SheetAction::Second:
      OpenListMenu(Popup::Second, kb::vocab::instruments(), sound.second_instrument, hit->rect, false);
      return;
    case SheetAction::RemoveSecond: sound.second_instrument.clear(); break;
    case SheetAction::AddCharacter:
    {
      // A fresh knob starts on a tag the sound does not have yet.
      std::vector<std::string> unused;
      for (const auto& tag : kb::vocab::character_tags())
        if (std::find(sound.character.begin(), sound.character.end(), tag) == sound.character.end())
          unused.push_back(tag);
      if (!unused.empty())
      {
        std::mt19937 rng(std::random_device{}());
        sound.character.push_back(unused[std::uniform_int_distribution<size_t>(0, unused.size() - 1)(rng)]);
      }
      break;
    }
    case SheetAction::RemoveCharacter:
      if (hit->index >= 0 && hit->index < (int)sound.character.size())
        sound.character.erase(sound.character.begin() + hit->index);
      break;
    case SheetAction::Articulation:
      sound.articulation = sound.articulation.empty() ? "Sustained" : std::string();
      break;
    case SheetAction::Oscillator:
      sound.oscillator = sound.oscillator.empty() ? "Sine" : std::string();
      break;
    case SheetAction::Dry: sound.wet = false; break;
    case SheetAction::Wet: sound.wet = true; break;
    case SheetAction::Pedal:
    {
      const std::string current = KnobValue(KnobKind::Fx, hit->index);
      if (current.empty())
        kb::set_fx(sound, DefaultFxToken(hit->index));
      else
        sound.fx.erase(std::remove(sound.fx.begin(), sound.fx.end(), current), sound.fx.end());
      break;
    }
    case SheetAction::AddExtra:
      if (GetUI())
      {
        mEdit = Edit::NewExtra;
        GetUI()->CreateTextEntry(*this, EntryText(11.f), mNewExtraRect, "", 0);
      }
      return;
    case SheetAction::EditExtra:
      if (GetUI() && hit->index < (int)sound.extras.size())
      {
        mEdit = Edit::Extra;
        mEditIndex = hit->index;
        GetUI()->CreateTextEntry(*this, EntryText(11.f), hit->rect, sound.extras[(size_t)hit->index].c_str(), 0);
      }
      return;
    case SheetAction::RemoveExtra:
      if (hit->index >= 0 && hit->index < (int)sound.extras.size())
        sound.extras.erase(sound.extras.begin() + hit->index);
      break;
  }
  mPlugin.SetSound(std::move(sound));
}

void KeybedControl::OnMouseWheel(float x, float y, const IMouseMod& mod, float d)
{
  if (mSoundOpen)
    if (const KnobHit* knob = KnobAt(x, y))
    {
      StepKnob(*knob, d > 0.f ? 1 : -1);
      SetDirty(false);
      return;
    }
  IControl::OnMouseWheel(x, y, mod, d);
}
