#include "KeybedControl.h"

#include "SA3Keybed.h"
#include "SA3UIPrimitives.h"
#include "SA3UITheme.h"

#include "sat/keybed.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

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
  if (mSettingsOpen)
    DrawSettings(g, shell);
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
  g.DrawText(Label(12.f, ready ? Green() : Red(), EAlign::Far),
             ready ? (mPlugin.Encoding() + " ready").c_str() : "no models - open settings",
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
  g.DrawText(Label(12.f, TextDim()), "instrument", IRECT(left, y, left + 120.f, y + 16.f));
  mDiceRect = IRECT(right - 34.f, y + 18.f, right, y + 50.f);
  DrawIconButton(g, mDiceRect, TransportIcon::Dice);
  mDescriptorRect = IRECT(left, y + 18.f, mDiceRect.L - 8.f, y + 50.f);
  g.FillRoundRect(ButtonFill(), mDescriptorRect, 3.f);
  g.DrawRoundRect(Frame(), mDescriptorRect, 3.f);
  const std::string descriptor = mPlugin.Descriptor();
  const bool empty = descriptor.empty();
  g.DrawText(Label(13.f, empty ? TextDim() : COLOR_WHITE),
             Compact(empty ? "describe a sound: Grand Piano, Warm, Soft" : descriptor,
                     FitChars(mDescriptorRect.W() - 16.f, 6.2f)).c_str(),
             mDescriptorRect.GetHPadded(-8.f));
  return y + 58.f;
}

float KeybedControl::DrawGeneration(IGraphics& g, float left, float right, float y)
{
  // Dry / Wet + FX tag. This is prompt conditioning, not a DSP effect: Wet asks the model to
  // render the sample with that space already in it.
  g.DrawText(Label(11.f, TextDim()), "render fx", IRECT(left, y, left + 86.f, y + 26.f));
  mDryRect = IRECT(left + 92.f, y + 1.f, left + 146.f, y + 25.f);
  mWetRect = IRECT(mDryRect.R + 6.f, y + 1.f, mDryRect.R + 60.f, y + 25.f);
  DrawTab(g, mDryRect, "dry", kFont, !mPlugin.Wet());
  DrawTab(g, mWetRect, "wet", kFont, mPlugin.Wet());
  mFxRect = {};
  if (mPlugin.Wet())
  {
    mFxRect = IRECT(mWetRect.R + 10.f, y + 1.f, right, y + 25.f);
    const int fx = mPlugin.FxIndex();
    const auto& choices = kb::vocab::fx_choices();
    DrawDropButton(g, mFxRect, fx >= 0 && fx < (int)choices.size() ? choices[(size_t)fx].c_str() : "fx: none");
  }
  y += 26.f;
  g.DrawText(Label(10.f, TextFaint()),
             mPlugin.Wet() ? "wet bakes the space into every sample - it cannot be removed later"
                           : "dry samples, so reverb and delay stay yours to add in the DAW",
             IRECT(left + 92.f, y, right, y + 14.f));
  y += 18.f;

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
  std::snprintf(detail, sizeof detail, "%s · %d chunks of 6 notes · %s at %d steps", RangeName(index),
                RangeChunks(index), ShortDuration(mPlugin.EstimatedSeconds(RangeChunks(index))).c_str(),
                mPlugin.Steps());
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
  const IRECT box(left, y, right, y + 76.f);
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
  g.DrawText(Label(12.f, TextDim()), "sound", IRECT(left, y, left + 80.f, y + 16.f));
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

  // models
  IRECT card(left, y, right, y + 196.f);
  g.FillRoundRect(PanelDark(), card, 5.f);
  g.DrawRoundRect(FrameSoft(), card, 5.f);
  g.DrawText(Label(14.f, COLOR_WHITE), "Foundation-1.2 Keybeds models", IRECT(card.L + 12.f, card.T + 8.f, card.R - 12.f, card.T + 30.f));
  mModelsFolderRect = IRECT(card.L + 12.f, card.T + 36.f, card.R - 12.f, card.T + 62.f);
  DrawDropButton(g, mModelsFolderRect, Compact(mPlugin.ModelsDir(), FitChars(mModelsFolderRect.W() - 24.f, 6.f)).c_str());
  std::string missing;
  const bool ready = mPlugin.ModelsReady(&missing);
  g.DrawMultiLineText(IText(10.f, ready ? Green() : Red(), kFont, EAlign::Near, EVAlign::Top),
                      ready ? "all three files found" : missing.c_str(),
                      IRECT(card.L + 12.f, card.T + 68.f, card.R - 12.f, card.T + 108.f));
  g.DrawText(Label(11.f, TextDim()), "tier", IRECT(card.L + 12.f, card.T + 112.f, card.L + 60.f, card.T + 136.f));
  static const char* tiers[] = {"F16", "Q8_0", "Q5_K_M"};
  float x = card.L + 60.f;
  for (int i = 0; i < 3; ++i)
  {
    mEncodingRects[(size_t)i] = IRECT(x, card.T + 112.f, x + 70.f, card.T + 136.f);
    DrawTab(g, mEncodingRects[(size_t)i], tiers[i], kFont, mPlugin.Encoding() == tiers[i]);
    x += 76.f;
  }
  g.DrawMultiLineText(IText(10.f, TextDim(), kFont, EAlign::Near, EVAlign::Top),
                      "expects foundation-1.2-keybeds-dit-1.1B-v1.0-<tier>.gguf, "
                      "t5-base-encoder-128tok-0.1B-v1.0-<tier>.gguf and "
                      "stable-audio-open-oobleck-v1.0-<tier>.gguf in one folder",
                      IRECT(card.L + 12.f, card.T + 144.f, card.R - 12.f, card.B - 6.f));
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
      const std::string dir = PickDirectory(mPlugin.ModelsDir());
      if (!dir.empty()) mPlugin.SetModelsDir(dir);
    }
    else if (mResidentRect.Contains(x, y)) mPlugin.SetKeepResident(!mPlugin.KeepResident());
    else if (mReleaseRect.Contains(x, y) && !mPlugin.Busy()) mPlugin.ReleaseModels();
    else
    {
      static const char* tiers[] = {"F16", "Q8_0", "Q5_K_M"};
      for (int i = 0; i < 3; ++i)
        if (mEncodingRects[(size_t)i].Contains(x, y)) mPlugin.SetEncoding(tiers[i]);
    }
    SetDirty(false);
    return;
  }

  if (mSettingsRect.Contains(x, y)) { mSettingsOpen = true; SetDirty(false); return; }
  if (mDiceRect.Contains(x, y)) { mPlugin.RollDescriptor(); SetDirty(false); return; }
  if (mDescriptorRect.Contains(x, y) && GetUI())
  {
    mEdit = Edit::Descriptor;
    GetUI()->CreateTextEntry(*this, IText(13.f, COLOR_WHITE, kFont, EAlign::Near, EVAlign::Middle).WithTEColors(PanelDark(), COLOR_WHITE),
                             mDescriptorRect, mPlugin.Descriptor().c_str(), 0);
    return;
  }
  if (mDryRect.Contains(x, y)) { mPlugin.SetWet(false); SetDirty(false); return; }
  if (mWetRect.Contains(x, y)) { mPlugin.SetWet(true); SetDirty(false); return; }
  if (mFxRect.Contains(x, y)) { OpenFxMenu(); return; }
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
    const std::string dir = PickDirectory(mPlugin.KitDir().empty() ? keybed::KitsDirectory() : mPlugin.KitDir());
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
  else if (mEdit == Edit::Seed)
  {
    mPlugin.SetSeedValue(ParseSeed(str, mPlugin.SeedValue()));
    mPlugin.SetUseSeed(true);
  }
  mEdit = Edit::None;
  SetDirty(false);
}

void KeybedControl::OpenFxMenu()
{
  if (!GetUI())
    return;
  mMenu.Clear();
  mMenu.AddItem("none");
  const auto& choices = kb::vocab::fx_choices();
  for (const auto& fx : choices)
    mMenu.AddItem(fx.c_str());
  mMenu.CheckItem(mPlugin.FxIndex() + 1, true);
  mPopup = Popup::Fx;
  GetUI()->CreatePopupMenu(*this, mMenu, mFxRect);
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
    if (mPopup == Popup::Fx)
      mPlugin.SetFxIndex(index - 1);
    else if (mPopup == Popup::PreviewRoot)
      mPlugin.SetPreviewRootLabel(kb::kPreviewRootMin + index);
  }
  mPopup = Popup::None;
  SetDirty(false);
}

std::string KeybedControl::PickDirectory(const std::string& seed)
{
  if (!GetUI())
    return {};
  WDL_String dir;
  if (!seed.empty())
    dir.Set(seed.c_str());
  GetUI()->PromptForDirectory(dir);
  return dir.GetLength() > 0 ? std::string(dir.Get()) : std::string();
}
