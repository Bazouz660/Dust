#pragma once

#include "SurveyRecorder.h"

namespace SurveyWriter
{
    // Write a single frame's data as JSON.
    void WriteFrame(const SurveyFrameData& frame, const char* outputDir);

    // Dump all captured HLSL shader sources to outputDir/shaders/.
    void WriteShaders(const char* outputDir);

    // Write aggregate summary across all captured frames.
    void WriteSummary(const SurveyFrameData* frames, int numFrames, const char* outputDir);
}
