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
  void OnMouseWheel(float x, float y, const IMouseMod& mod, float d) override;
  void OnPopupMenuSelection(IPopupMenu* pMenu, int valIdx) override;
  void OnTextEntryCompletion(const char* str, int valIdx) override;

private:
  enum class Popup { None, PreviewRoot, Family, Type, Second, Knob };
  enum class Edit { None, Descriptor, Seed, NewExtra, Extra };
  enum class Drag { None, Steps, Cfg, Param, Key, FileOut, Knob };

  // Sound sheet: stepped knobs over a vocabulary, and the buttons around them.
  enum class KnobKind { Character, Articulation, Oscillator, Fx };
  struct KnobHit
  {
    IRECT rect;
    KnobKind kind = KnobKind::Character;
    int index = 0;   // character slot, or FX category
  };
  enum class SheetAction
  {
    Done, Dice, Lock, Family, Type, AddSecond, Second, RemoveSecond, AddCharacter, RemoveCharacter,
    Articulation, Oscillator, Dry, Wet, Pedal, AddExtra, EditExtra, RemoveExtra
  };
  struct SheetHit
  {
    IRECT rect;
    SheetAction action = SheetAction::Done;
    int index = 0;
  };

  struct ParamSlider
  {
    int param = -1;
    IRECT rect;
  };

  void DrawMain(IGraphics& g, const IRECT& shell);
  void DrawSettings(IGraphics& g, const IRECT& shell);
  void DrawSoundSheet(IGraphics& g, const IRECT& shell);
  float DrawSectionLabel(IGraphics& g, float left, float right, float y, const char* label, int lockSection,
                         const char* addLabel, SheetAction addAction);
  float DrawKnobRow(IGraphics& g, float left, float right, float y, const std::vector<KnobHit>& knobs,
                    bool removable);
  void DrawKnob(IGraphics& g, const IRECT& bounds, const std::string& value, int step, int steps);
  void DrawLock(IGraphics& g, const IRECT& bounds, bool locked);
  void OnSheetMouseDown(float x, float y, const IMouseMod& mod);
  const std::vector<std::string>& KnobVocabulary(KnobKind kind, int index) const;
  std::string KnobValue(KnobKind kind, int index) const;
  void SetKnobValue(KnobKind kind, int index, const std::string& value);
  void StepKnob(const KnobHit& knob, int delta);
  const KnobHit* KnobAt(float x, float y) const;
  void OpenListMenu(Popup popup, const std::vector<std::string>& items, const std::string& current,
                    const IRECT& anchor, bool allowNone);
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
  void OpenPreviewRootMenu();
  std::string PickDirectory(const std::string& seed);

  SA3Keybed& mPlugin;
  bool mSettingsOpen = false;
  bool mSoundOpen = false;
  std::vector<KnobHit> mKnobs;
  std::vector<SheetHit> mSheetHits;
  KnobHit mActiveKnob;          // knob being dragged or whose menu is open
  int mKnobDragStartStep = 0;
  std::vector<std::string> mMenuItems;   // items behind the open list menu (after a leading "none")
  bool mMenuHasNone = false;
  int mEditIndex = -1;
  IRECT mNewExtraRect;          // where a new extra-descriptor row is typed
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
  IRECT mSettingsRect, mDescriptorRect, mDiceRect, mEditSoundRect;
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
