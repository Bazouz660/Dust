#pragma once

#include <d3d11.h>
#include <string>

// Bug-report bundle generator. Collects everything needed to diagnose a user
// report into a single zip: report.txt (versions, hardware, live effect
// settings, overlay detection) plus verbatim copies of the Dust/game config
// files, load order, and recent logs. Generation runs on a worker thread so
// the render thread never blocks on file I/O; DustGUI polls GetStatus().
namespace BugReport
{
    enum class Status { Idle, Running, Done, Failed };

    // Live data only the GUI knows at click time.
    struct LiveStats {
        ID3D11Device* device      = nullptr;  // rendering device (adapter/feature level); may be null
        float         fps         = 0.0f;
        float         frameTimeMs = 0.0f;
        float         displayW    = 0.0f;     // backbuffer size
        float         displayH    = 0.0f;
    };

    // Snapshot all in-memory state synchronously (fast), then start the file
    // copy + zip on a worker thread. Returns false if a report is already
    // being generated.
    bool StartGeneration(const LiveStats& stats);

    Status GetStatus();

    // Full path of the generated zip (valid once Done).
    std::string GetResultPath();

    // Human-readable failure reason (valid once Failed).
    std::string GetError();

    // Short plaintext system summary for the clipboard / GitHub issue prefill.
    // Built synchronously by StartGeneration, so valid immediately after it.
    std::string GetSummary();

    // New-issue URL with the system summary prefilled into the issue form.
    std::string BuildGitHubIssueURL();
}
