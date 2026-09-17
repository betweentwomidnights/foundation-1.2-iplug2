#pragma once

#include "IControl.h"

#include "KeybedKit.h"

#include <array>
#include <chrono>
#include <string>
#include <vector>

class SA3Keybed;

constexpr int kKeybedControlTag = 1000;

using namespace iplug;
using namespace igraphics;

// The whole plugin surface, drawn immediately each frame in the sa3 demo's visual language
// (black shell, red outline buttons, custom sliders) and hit-tested against stored rects.
class KeybedControl final : public IControl
{
public:
  KeybedControl(const IRECT& bounds, SA3Keybed& plugin);

  void Draw(IGraphics& g) override;
  void OnMouseDown(float x, float y, const IMouseMod& mod) override;
  void OnMouseDrag(float x, float y, float dX, float dY, const IMouseMod& mod) override;
  void OnMouseUp(float x, float y, const IMouseMod& mod) override;
  void OnMouseDblClick(float x, float y, const IMouseMod& mod) override;
  void OnPopupMenuSelection(IPopupMenu* pMenu, int valIdx) override;
  void OnTextEntryCompletion(const char* str, int valIdx) override;

private:
  enum class Popup { None, Fx, PreviewRoot };
  enum class Edit { None, Descriptor, Seed };
  enum class Drag { None, Steps, Cfg, Param, Key, FileOut };

  struct ParamSlider
  {
    int param = -1;
    IRECT rect;
  };

  void DrawMain(IGraphics& g, const IRECT& shell);
  void DrawSettings(IGraphics& g, const IRECT& shell);
  float DrawHeader(IGraphics& g, float left, float right, float y);
  float DrawStatus(IGraphics& g, float left, float right, float y);
  float DrawDescriptor(IGraphics& g, float left, float right, float y);
  float DrawGeneration(IGraphics& g, float left, float right, float y);
  float DrawActions(IGraphics& g, float left, float right, float y);
  float DrawKeyboard(IGraphics& g, float left, float right, float y);
  float DrawNoteWaveform(IGraphics& g, float left, float right, float y);
  float DrawSound(IGraphics& g, float left, float right, float y);
  float DrawKit(IGraphics& g, float left, float right, float y);

  void DrawSlider(IGraphics& g, const IRECT& bounds, const char* label, const char* valueText, float fraction,
                  IRECT& sliderRect);
  void DrawToggle(IGraphics& g, const IRECT& bounds, const char* label, bool on, IRECT& hitRect);
  void DrawDropButton(IGraphics& g, const IRECT& bounds, const char* text);
  void DrawParamSlider(IGraphics& g, const IRECT& bounds, const char* label, int param);

  void SetParamFromX(int param, const IRECT& rect, float x);
  void EndParamDrag();
  int KeyAt(float x, float y) const;
  void PressKey(int key);
  void ReleaseKey();
  void OpenFxMenu();
  void OpenPreviewRootMenu();
  std::string PickDirectory(const std::string& seed);

  SA3Keybed& mPlugin;
  bool mSettingsOpen = false;
  Popup mPopup = Popup::None;
  Edit mEdit = Edit::None;
  Drag mDrag = Drag::None;
  int mDragParam = -1;
  IRECT mDragRect;
  int mPressedKey = -1;
  IPopupMenu mMenu;
  std::string mDragOutPath;   // WAV or kit folder armed by a mouse-down, started after a short drag
  IRECT mDragOutRect;
  float mDragStartX = 0.f, mDragStartY = 0.f;
  int mWaveformKey = -1;

  // hit rects, refreshed every Draw
  IRECT mSettingsRect, mDescriptorRect, mDiceRect, mDryRect, mWetRect, mFxRect;
  IRECT mStepsRect, mCfgRect, mSeedToggleRect, mSeedFieldRect;
  std::array<IRECT, 3> mPreviewCountRects{};
  IRECT mPreviewRootRect, mPreviewRect;
  std::array<IRECT, 3> mRangeRects{};
  IRECT mBuildRect, mKeyboardRect;
  std::vector<ParamSlider> mParamSliders;
  IRECT mOctaveDownRect, mOctaveUpRect, mFillGapsRect;
  IRECT mRevealKitRect, mLoadKitRect, mKitLabelRect, mWaveformRect;
  IRECT mModelsFolderRect, mResidentRect, mReleaseRect, mCloseRect;
  std::array<IRECT, 3> mEncodingRects{};

  // cached peaks for the last-played sample
  const keybed::NoteSample* mPeaksFor = nullptr;
  int mPeaksWidth = 0;
  std::vector<std::pair<float, float>> mPeaks;
};
