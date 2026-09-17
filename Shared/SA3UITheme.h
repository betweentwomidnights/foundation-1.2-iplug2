#pragma once

#include "IGraphicsStructs.h"

namespace gary::ui
{

using namespace iplug;
using namespace igraphics;

inline constexpr const char* FontName = "SA3Roboto";
inline constexpr float TitleTextSize = 20.f;
inline constexpr float BodyTextSize = 14.f;
inline constexpr float TabTextSize = 13.f;
inline constexpr float CornerRadius = 4.f;

inline IColor Background() { return IColor(255, 0, 0, 0); }
inline IColor Panel() { return IColor(255, 18, 18, 18); }
inline IColor PanelDark() { return IColor(255, 8, 8, 8); }
inline IColor Frame() { return IColor(255, 68, 68, 68); }
inline IColor FrameSoft() { return IColor(255, 38, 38, 38); }
inline IColor Red() { return IColor(255, 230, 32, 32); }
inline IColor RedDim() { return IColor(135, 230, 32, 32); }
inline IColor RedFaint() { return IColor(70, 230, 32, 32); }
inline IColor Green() { return IColor(255, 72, 210, 120); }
inline IColor TextDim() { return IColor(255, 165, 165, 165); }
inline IColor TextFaint() { return IColor(255, 105, 105, 105); }
inline IColor ButtonFill() { return IColor(255, 16, 16, 16); }
inline IColor ButtonText(bool hovered) { return hovered ? COLOR_BLACK : COLOR_WHITE; }
inline IColor ButtonBorder(bool enabled = true) { return enabled ? Red() : TextFaint(); }
inline IColor ButtonBackground(bool hovered, bool enabled = true)
{
  if (!enabled)
    return IColor(255, 12, 12, 12);
  return hovered ? Red() : ButtonFill();
}

inline void DrawButton(IGraphics& g, const IRECT& bounds, const char* label, const char* fontName,
                       bool hovered = false, bool enabled = true)
{
  g.FillRoundRect(ButtonBackground(hovered, enabled), bounds, CornerRadius);
  g.DrawRoundRect(ButtonBorder(enabled), bounds, CornerRadius);
  g.DrawText(IText(BodyTextSize, ButtonText(hovered), fontName, EAlign::Center, EVAlign::Middle),
             label, bounds.GetPadded(-4.f));
}

inline void DrawTab(IGraphics& g, const IRECT& bounds, const char* label, const char* fontName, bool active)
{
  g.FillRoundRect(active ? Red() : ButtonFill(), bounds, CornerRadius);
  g.DrawRoundRect(active ? Red() : Frame(), bounds, CornerRadius);
  g.DrawText(IText(TabTextSize, active ? COLOR_BLACK : COLOR_WHITE, fontName, EAlign::Center, EVAlign::Middle),
             label, bounds.GetPadded(-4.f));
}

}

