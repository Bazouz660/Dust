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
static std::string  sLabel;
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

static std::string SanitizeLabel(const char* raw)
{
    if (!raw || !raw[0])
        return "";
    std::string out;
    for (const char* p = raw; *p && out.size() < 48; ++p)
    {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_')
            out += c;
        else if (c == ' ')
            out += '_';
    }
    return out;
}

void Start(int numFrames, int detailLevel, const char* label)
{
    if (sActive)
    {
        Log("SURVEY: Already active, ignoring Start()");
        return;
    }

    sLabel = label ? label : "";
    std::string sanitized = SanitizeLabel(label);

    // Build a timestamped output path so captures are never overwritten
    // e.g. survey/2026-04-20_143052_desert_night/
    std::string surveyRoot = DustLogDir() + "survey\\";
    if (!CreateDirectoryA(surveyRoot.c_str(), nullptr))
    {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS)
        {
            Log("SURVEY: Failed to create survey root (error %u)", err);
            return;
        }
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    char stamp[64];
    snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d_%02d%02d%02d",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);

    std::string folderName = stamp;
    if (!sanitized.empty())
        folderName += "_" + sanitized;

    sOutputDir = surveyRoot + folderName + "\\";
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
    Log("SURVEY: Started capture (%d frames, detail level %d, label: '%s', output: %s)",
        sTotalFrames, sDetailLevel, sLabel.c_str(), sOutputDir.c_str());
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

const char* GetLabel()
{
    return sLabel.c_str();
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
