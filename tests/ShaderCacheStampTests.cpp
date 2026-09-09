#include "../src/ShaderCacheStamp.h"
#include <cassert>
#include <fstream>

static void Write(const std::string& path, const char* value) { std::ofstream(path) << value; }
static std::string Read(const std::string& path) { std::ifstream f(path); std::string line; std::getline(f, line); return line; }
int main()
{
    char tempRoot[MAX_PATH], unique[MAX_PATH];
    assert(GetTempPathA(MAX_PATH, tempRoot));
    assert(GetTempFileNameA(tempRoot, "dst", 0, unique));
    assert(DeleteFileA(unique)); assert(CreateDirectoryA(unique, nullptr));
    const std::string dir(unique), cache = dir + "\\shader_cache.sc", stamp = dir + "\\stamp.txt";
    using ShaderCacheStamp::Result;
    DWORD error = 0;
    Write(cache, "old bytecode"); Write(stamp, "old");
    HANDLE locked = CreateFileA(cache.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    assert(locked != INVALID_HANDLE_VALUE);
    assert(ShaderCacheStamp::Update(cache, stamp, "new", error) == Result::InvalidationFailed);
    assert(error == ERROR_SHARING_VIOLATION && Read(stamp) == "old" && Read(cache) == "old bytecode");
    CloseHandle(locked);
    assert(ShaderCacheStamp::Update(cache, stamp, "new", error) == Result::Updated);
    assert(Read(stamp) == "new" && GetFileAttributesA(cache.c_str()) == INVALID_FILE_ATTRIBUTES);
    Write(cache, "new bytecode");
    assert(ShaderCacheStamp::Update(cache, stamp, "new", error) == Result::Unchanged);
    assert(Read(cache) == "new bytecode");
    locked = CreateFileA(stamp.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    assert(locked != INVALID_HANDLE_VALUE);
    assert(ShaderCacheStamp::Update(cache, stamp, "newer", error) == Result::StampWriteFailed);
    assert(Read(stamp) == "new"); // replacement failure preserved the old stamp
    CloseHandle(locked);
    assert(ShaderCacheStamp::Update(cache, stamp, "newer", error) == Result::Updated); // absent cache is safe
    Write(stamp, "truncated stamp"); Write(cache, "stale");
    assert(ShaderCacheStamp::Update(cache, stamp, "newer", error) == Result::Updated);
    assert(DeleteFileA(stamp.c_str()));
    assert(RemoveDirectoryA(dir.c_str())); // also verifies failed writes left no temporary files
}
