#include "Survey.h"
#include "DustLog.h"
#include <windows.h>
#include <string>

namespace Survey
{

// ==================== State ====================

static bool         sActive       = false;
static int          sCurrentFrame = 0;
static int          sTotalFrames  = 3;
static int          sDetailLevel  = DETAIL_STANDARD;
static int          sMaxBudgetMs  = 500;
static std::string  sOutputDir;
static std::string  sIniPath;

// Timing
static LARGE_INTEGER sFreq;
static LARGE_INTEGER sFrameStart;
static bool          sFreqInit = false;

static void EnsureQPC()
{
    if (!sFreqInit)
    {
        QueryPerformanceFrequency(&sFreq);
        sFreqInit = true;
    }
}

// ==================== Output directory ====================

static bool CreateOutputDir(const std::string& dir)
{
    // Create main survey dir
    if (!CreateDirectoryA(dir.c_str(), nullptr))
    {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS)
        {
            Log("SURVEY: Failed to create directory '%s' (error %u)", dir.c_str(), err);
            return false;
        }
    }

    // Create shaders subdirectory
    std::string shadersDir = dir + "shaders\\";
    if (!CreateDirectoryA(shadersDir.c_str(), nullptr))
    {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS)
        {
            Log("SURVEY: Failed to create shaders directory (error %u)", err);
            return false;
        }
    }

    return true;
}

// ==================== Public API ====================

void InitFromINI(const char* iniPath)
{
    sIniPath = iniPath ? iniPath : "";
    if (sIniPath.empty())
        return;

    sTotalFrames = GetPrivateProfileIntA("Survey", "Frames", 3, sIniPath.c_str());
    if (sTotalFrames < 1) sTotalFrames = 1;
    if (sTotalFrames > 60) sTotalFrames = 60;

    sDetailLevel = GetPrivateProfileIntA("Survey", "DetailLevel", DETAIL_STANDARD, sIniPath.c_str());
    if (sDetailLevel < DETAIL_MINIMAL) sDetailLevel = DETAIL_MINIMAL;
    if (sDetailLevel > DETAIL_FULL) sDetailLevel = DETAIL_FULL;

    sMaxBudgetMs = GetPrivateProfileIntA("Survey", "MaxFrameBudgetMs", 500, sIniPath.c_str());
    if (sMaxBudgetMs < 50) sMaxBudgetMs = 50;
    if (sMaxBudgetMs > 5000) sMaxBudgetMs = 5000;

    Log("SURVEY: INI defaults: frames=%d detail=%d budget=%dms",
        sTotalFrames, sDetailLevel, sMaxBudgetMs);
}

void Start(int numFrames, int detailLevel)
{
    if (sActive)
    {
        Log("SURVEY: Already active, ignoring Start()");
        return;
    }

    // Build output path next to the DLL
    sOutputDir = DustLogDir() + "survey\\";
    if (!CreateOutputDir(sOutputDir))
    {
        Log("SURVEY: Cannot create output directory, aborting");
        return;
    }

    sTotalFrames = (numFrames >= 1 && numFrames <= 60) ? numFrames : 3;
    sDetailLevel = (detailLevel >= DETAIL_MINIMAL && detailLevel <= DETAIL_FULL) ? detailLevel : DETAIL_STANDARD;
    sCurrentFrame = 0;

    EnsureQPC();

    // Flush log so it's recoverable if we crash
    FILE* f = DustLogFile();
    if (f) fflush(f);

    sActive = true;
    Log("SURVEY: Started capture (%d frames, detail level %d, output: %s)",
        sTotalFrames, sDetailLevel, sOutputDir.c_str());
}

void Stop()
{
    if (!sActive)
        return;

    sActive = false;
    Log("SURVEY: Stopped at frame %d / %d", sCurrentFrame, sTotalFrames);
}

bool IsActive()
{
    return sActive;
}

int CurrentFrame()
{
    return sCurrentFrame;
}

int TotalFrames()
{
    return sTotalFrames;
}

int GetDetailLevel()
{
    return sDetailLevel;
}

const char* GetOutputDir()
{
    return sOutputDir.c_str();
}

bool OnFrameEnd()
{
    if (!sActive)
        return false;

    sCurrentFrame++;

    if (sCurrentFrame >= sTotalFrames)
    {
        sActive = false;
        Log("SURVEY: Capture complete (%d frames)", sTotalFrames);
        return true; // survey just finished
    }

    return false;
}

} // namespace Survey
