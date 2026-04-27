#pragma once

// Asynchronous folder picker using IFileDialog on a worker thread.
// The dialog has its own message pump, so the render thread keeps drawing
// while the user browses.
//
// Usage:
//   if (clicked) FilePicker::StartFolderPicker("Choose preset folder");
//   std::string path;
//   if (FilePicker::Poll(path)) { /* path is empty on cancel */ }

#include <string>

namespace FilePicker
{
    // Spawn a worker thread that shows a folder picker. Returns false if a
    // picker is already in flight (one at a time).
    bool StartFolderPicker(const char* title);

    // Returns true once the dialog has closed. outPath holds the chosen
    // folder, or is empty on cancel/error. After Poll returns true the
    // module is idle again and StartFolderPicker can be called again.
    bool Poll(std::string& outPath);

    // True while a picker is running.
    bool IsBusy();

    // Join any pending thread. Call on framework shutdown.
    void Shutdown();
}
