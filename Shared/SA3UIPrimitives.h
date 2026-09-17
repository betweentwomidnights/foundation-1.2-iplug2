#pragma once

#include "IPlug_include_in_plug_hdr.h"
#include "SA3UITheme.h"

#include <algorithm>
#include <cmath>

namespace gary::ui
{

using namespace iplug;
using namespace igraphics;

enum class TransportIcon
{
  Play,
  Pause,
  Stop,
  Crop,
  Undo,
  Signal,
  Dice
};

inline void DrawPlayIcon(IGraphics& g, const IRECT& bounds, const IColor& color)
{
  const IRECT b = bounds.GetPadded(-3.f);
  auto x = [&](float value) { return b.L + (value / 24.f) * b.W(); };
  auto y = [&](float value) { return b.T + (value / 24.f) * b.H(); };

  g.PathClear();
  g.PathMoveTo(x(5.f), y(3.f));
  g.PathLineTo(x(19.f), y(12.f));
  g.PathLineTo(x(5.f), y(21.f));
  g.PathLineTo(x(5.f), y(3.f));
  g.PathClose();
  g.PathStroke(color, 2.f);
}

inline void DrawPauseIcon(IGraphics& g, const IRECT& bounds, const IColor& color)
{
  const IRECT b = bounds.GetPadded(-3.f);
  auto x = [&](float value) { return b.L + (value / 24.f) * b.W(); };
  auto y = [&](float value) { return b.T + (value / 24.f) * b.H(); };

  g.DrawRoundRect(color, IRECT(x(6.f), y(4.f), x(10.f), y(20.f)), 0.5f, nullptr, 2.f);
  g.DrawRoundRect(color, IRECT(x(14.f), y(4.f), x(18.f), y(20.f)), 0.5f, nullptr, 2.f);
}

inline void DrawStopIcon(IGraphics& g, const IRECT& bounds, const IColor& color)
{
  const IRECT b = bounds.GetPadded(-3.f);
  auto x = [&](float value) { return b.L + (value / 24.f) * b.W(); };
  auto y = [&](float value) { return b.T + (value / 24.f) * b.H(); };

  g.DrawRoundRect(color, IRECT(x(3.f), y(3.f), x(21.f), y(21.f)), 2.f, nullptr, 2.f);
}

inline void DrawCropIcon(IGraphics& g, const IRECT& bounds, const IColor& color)
{
  const IRECT b = bounds.GetPadded(-3.f);
  auto x = [&](float value) { return b.L + (value / 24.f) * b.W(); };
  auto y = [&](float value) { return b.T + (value / 24.f) * b.H(); };

  g.PathClear();
  g.PathMoveTo(x(6.13f), y(1.f));
  g.PathLineTo(x(6.f), y(16.f));
  g.PathQuadraticBezierTo(x(6.f), y(18.f), x(8.f), y(18.f));
  g.PathLineTo(x(23.f), y(18.f));
  g.PathStroke(color, 2.f);

  g.PathClear();
  g.PathMoveTo(x(1.f), y(6.13f));
  g.PathLineTo(x(16.f), y(6.f));
  g.PathQuadraticBezierTo(x(18.f), y(6.f), x(18.f), y(8.f));
  g.PathLineTo(x(18.f), y(23.f));
  g.PathStroke(color, 2.f);
}

inline void DrawUndoIcon(IGraphics& g, const IRECT& bounds, const IColor& color)
{
  const IRECT b = bounds.GetPadded(-3.f);
  auto x = [&](float value) { return b.L + (value / 24.f) * b.W(); };
  auto y = [&](float value) { return b.T + (value / 24.f) * b.H(); };

  g.PathClear();
  g.PathMoveTo(x(10.f), y(6.f));
  g.PathLineTo(x(5.f), y(6.f));
  g.PathLineTo(x(5.f), y(1.f));
  g.PathStroke(color, 2.f);

  g.PathClear();
  g.PathMoveTo(x(5.5f), y(6.f));
  g.PathCubicBezierTo(x(8.f), y(3.7f), x(12.4f), y(3.5f), x(15.7f), y(5.6f));
  g.PathCubicBezierTo(x(20.4f), y(8.5f), x(20.3f), y(15.5f), x(15.3f), y(18.4f));
  g.PathCubicBezierTo(x(11.4f), y(20.6f), x(6.8f), y(19.4f), x(4.7f), y(16.2f));
  g.PathStroke(color, 2.f);
}

inline void DrawSignalBarsIcon(IGraphics& g, const IRECT& bounds, const IColor& color)
{
  const IRECT b = bounds.GetPadded(-3.f);
  auto x = [&](float value) { return b.L + (value / 24.f) * b.W(); };
  auto y = [&](float value) { return b.T + (value / 24.f) * b.H(); };

  g.FillRect(color, IRECT(x(5.f), y(15.f), x(8.f), y(20.f)));
  g.FillRect(color, IRECT(x(11.f), y(10.f), x(14.f), y(20.f)));
  g.FillRect(color, IRECT(x(17.f), y(5.f), x(20.f), y(20.f)));
}

inline void DrawDiceIcon(IGraphics& g, const IRECT& bounds, const IColor& color)
{
  const IRECT b = bounds.GetPadded(-4.f);
  g.DrawRoundRect(color, b, 2.f, nullptr, 1.7f);
  const float r = std::max(1.2f, std::min(b.W(), b.H()) * 0.055f);
  const float x1 = b.L + b.W() * 0.30f;
  const float x2 = b.L + b.W() * 0.50f;
  const float x3 = b.L + b.W() * 0.70f;
  const float y1 = b.T + b.H() * 0.30f;
  const float y2 = b.T + b.H() * 0.50f;
  const float y3 = b.T + b.H() * 0.70f;
  g.FillCircle(color, x1, y1, r);
  g.FillCircle(color, x3, y1, r);
  g.FillCircle(color, x2, y2, r);
  g.FillCircle(color, x1, y3, r);
  g.FillCircle(color, x3, y3, r);
}

inline void DrawGaryLogo(IGraphics& g, const IRECT& bounds)
{
  const IRECT b = bounds.GetPadded(-1.f);
  const float cx = b.MW();
  const float cy = b.MH();
  const float r = std::min(b.W(), b.H()) * 0.48f;

  g.FillCircle(IColor(255, 4, 4, 4), cx, cy, r);
  g.DrawCircle(Red(), cx, cy, r, nullptr, 2.f);
  g.DrawCircle(Red(), cx, cy, r * 0.72f, nullptr, 1.f);

  g.DrawCircle(COLOR_WHITE, cx - r * 0.34f, cy - r * 0.12f, r * 0.16f, nullptr, 1.5f);
  g.DrawCircle(COLOR_WHITE, cx + r * 0.34f, cy - r * 0.12f, r * 0.16f, nullptr, 1.5f);
  g.FillCircle(Red(), cx - r * 0.34f, cy - r * 0.12f, r * 0.06f);
  g.FillCircle(Red(), cx + r * 0.34f, cy - r * 0.12f, r * 0.06f);

  g.DrawLine(COLOR_WHITE, cx - r * 0.28f, cy + r * 0.35f, cx + r * 0.28f, cy + r * 0.35f, nullptr, 1.5f);
  g.DrawLine(Red(), cx, cy + r * 0.08f, cx - r * 0.10f, cy + r * 0.22f, nullptr, 1.3f);
  g.DrawLine(Red(), cx, cy + r * 0.08f, cx + r * 0.10f, cy + r * 0.22f, nullptr, 1.3f);

  for (int i = 0; i < 8; ++i)
  {
    const float angle = static_cast<float>(i) * 0.78539816f;
    const float inner = r * 0.82f;
    const float outer = r * 1.02f;
    g.DrawLine(Red(),
               cx + std::cos(angle) * inner,
               cy + std::sin(angle) * inner,
               cx + std::cos(angle) * outer,
               cy + std::sin(angle) * outer,
               nullptr,
               1.f);
  }
}

inline void DrawIconButton(IGraphics& g, const IRECT& bounds, TransportIcon icon, bool hovered = false, bool enabled = true)
{
  g.FillRoundRect(ButtonBackground(hovered, enabled), bounds, 4.f);
  g.DrawRoundRect(ButtonBorder(enabled), bounds, 4.f);

  const IRECT iconBounds = bounds.GetCentredInside(std::min(bounds.W(), bounds.H()) - 10.f);
  const IColor iconColor = enabled ? ButtonText(hovered) : TextFaint();
  switch (icon)
  {
    case TransportIcon::Play:
      DrawPlayIcon(g, iconBounds, iconColor);
      break;
    case TransportIcon::Pause:
      DrawPauseIcon(g, iconBounds, iconColor);
      break;
    case TransportIcon::Stop:
      DrawStopIcon(g, iconBounds, iconColor);
      break;
    case TransportIcon::Crop:
      DrawCropIcon(g, iconBounds, iconColor);
      break;
    case TransportIcon::Undo:
      DrawUndoIcon(g, iconBounds, iconColor);
      break;
    case TransportIcon::Signal:
      DrawSignalBarsIcon(g, iconBounds, iconColor);
      break;
    case TransportIcon::Dice:
      DrawDiceIcon(g, iconBounds, iconColor);
      break;
  }
}

inline void DrawFolderIcon(IGraphics& g, const IRECT& bounds, const IColor& color)
{
  const IRECT b = bounds.GetPadded(-1.5f);
  auto x = [&](float value) { return b.L + (value / 24.f) * b.W(); };
  auto y = [&](float value) { return b.T + (value / 24.f) * b.H(); };

  g.PathClear();
  g.PathMoveTo(x(22.f), y(19.f));
  g.PathQuadraticBezierTo(x(22.f), y(21.f), x(20.f), y(21.f));
  g.PathLineTo(x(4.f), y(21.f));
  g.PathQuadraticBezierTo(x(2.f), y(21.f), x(2.f), y(19.f));
  g.PathLineTo(x(2.f), y(5.f));
  g.PathQuadraticBezierTo(x(2.f), y(3.f), x(4.f), y(3.f));
  g.PathLineTo(x(9.f), y(3.f));
  g.PathLineTo(x(11.f), y(6.f));
  g.PathLineTo(x(20.f), y(6.f));
  g.PathQuadraticBezierTo(x(22.f), y(6.f), x(22.f), y(8.f));
  g.PathLineTo(x(22.f), y(19.f));
  g.PathClose();
  g.PathStroke(color, 1.7f);
}

}
