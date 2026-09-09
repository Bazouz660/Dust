#include "../effects/common/LazyResources.h"
#include <cassert>
int main()
{
    LazyResources resources;
    int creates = 0, live = 0;
    auto release = [&] { live = 0; };
    auto fail = [&] { ++creates; live = 2; return false; };
    auto succeed = [&] { ++creates; live = 3; return true; };
    assert(!resources.Ready() && creates == 0); // disabled/startup owns no targets
    assert(!resources.Ensure(100, fail, release));
    assert(live == 0 && !resources.Ready());
    assert(!resources.Ensure(1099, succeed, release) && creates == 1);
    assert(resources.Ensure(1100, succeed, release) && live == 3);
    assert(resources.Ensure(2000, fail, release) && creates == 2); // reuse, no reallocation
    resources.Reset(release); // resizing while disabled releases without recreating
    assert(live == 0 && !resources.Ready() && creates == 2);
    assert(resources.Ensure(2001, succeed, release) && creates == 3);
    resources.Reset(release); // shutdown also clears the retry deadline for reinitialization
    assert(!resources.Ensure(0, fail, release));
    resources.Reset(release);
    assert(resources.Ensure(1, succeed, release));
}
