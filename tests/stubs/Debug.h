#pragma once
// Standalone native probes never call into Kenshi's logging DLL.
inline void DebugLog(const char*) {}
