#pragma once
#include <cstdint>

// Plugin Init still prepares shaders/settings/fallbacks. Resolution-dependent
// textures are allocated only from the render path that consumes them.
class LazyResources
{
    bool ready_ = false;
    uint64_t retryAfter_ = 0;
public:
    bool Ready() const { return ready_; }
    template<class Release> void Reset(Release release)
    {
        release();
        ready_ = false;
        retryAfter_ = 0;
    }
    template<class Create, class Release> bool Ensure(uint64_t now, Create create, Release release)
    {
        if (ready_) return true;
        if (now < retryAfter_) return false;
        if (create()) { ready_ = true; retryAfter_ = 0; return true; }
        release(); // discard every partially-created texture/view before retrying
        retryAfter_ = now + 1000; // avoid allocation/log storms under memory pressure
        return false;
    }
};
