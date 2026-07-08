#pragma once
// No-op Tracy stub for the main / feature-upscaling tree.
//
// The dxr-experiment tree links the real Tracy profiler; main does not. The
// ported GeometryCapture uses a handful of Tracy zone macros — this stub makes
// them compile to nothing so the capture code drops in unchanged. If Tracy is
// ever vendored into this tree, delete this file and add the real include dir.

#define ZoneScoped
#define ZoneScopedN(name)
#define ZoneScopedC(color)
#define ZoneScopedNC(name, color)
#define ZoneName(name, size)
#define ZoneText(text, size)
#define ZoneValue(value)
#define FrameMark
#define FrameMarkNamed(name)
#define FrameMarkStart(name)
#define FrameMarkEnd(name)
#define TracyPlot(name, value)
#define TracyMessage(text, size)
#define TracyMessageL(text)
