#pragma once

#include <cstdint>

namespace Survey
{
    enum DetailLevel
    {
        DETAIL_MINIMAL  = 0,  // draw type, counts, RT format+dimensions
        DETAIL_STANDARD = 1,  // + shaders, SRVs, CBs (slot+size), IA, OM states
        DETAIL_DEEP     = 2,  // + CB data readback (first 512 bytes), SRV resource details
        DETAIL_FULL     = 3   // + vertex/index buffer metadata, viewport/scissor
    };

    struct Config
    {
        int  numFrames   = 3;
        int  detailLevel = DETAIL_STANDARD;
        int  maxFrameBudgetMs = 500; // auto-reduce detail if exceeded
    };

    // Start a survey capture. Creates timestamped output directory, resets state.
    // Optional label (e.g. "desert_night") is included in the folder name and metadata.
    void Start(int numFrames, int detailLevel, const char* label = nullptr);

    // Stop an in-progress survey early.
    void Stop();

    // True while actively capturing.
    bool IsActive();

    // Current capture progress.
    int  CurrentFrame();
    int  TotalFrames();

    // Detail level for the current (or last) survey.
    int  GetDetailLevel();

    // Output directory path (e.g. "<DLL dir>/survey/2026-04-20_143052_label/").
    const char* GetOutputDir();

    // Label for the current (or last) survey capture.
    const char* GetLabel();

    // Called once at startup to read defaults from Dust.ini [Survey] section.
    void InitFromINI(const char* iniPath);

    // Called by Present hook to advance frame counter + finalize.
    // Returns true if the survey just finished (last frame captured).
    bool OnFrameEnd();
}
