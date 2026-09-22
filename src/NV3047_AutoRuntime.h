#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Strong symbols intentionally referenced by NV3047_Memory.h. This forces the
// automatic runtime object into Arduino's final link even when the sketch does
// not call any memory-manager API directly.
void nv3047_memorymanager_autoruntime_install();

bool nv3047_memorymanager_autoruntime_lock();
void nv3047_memorymanager_autoruntime_unlock();

#ifdef __cplusplus
}
#endif

namespace NV3047Memory
{

struct AutoRuntimeStats
{
    bool installed = false;
    bool taskRunning = false;
    bool managerReady = false;
    bool driverTakeoverActive = false;

    uint32_t automaticStarts = 0;
    uint32_t failedStarts = 0;
    uint32_t backgroundServicePasses = 0;

    // Permanent compile-time storage used by the background runtime task,
    // mutex and state. The broker arena is reported separately by AutoMemory.
    size_t staticRuntimeBytes = 0;
};

AutoRuntimeStats automaticRuntimeStats();

namespace Detail
{

class AutoRuntimeHeaderInstall final
{
public:
    AutoRuntimeHeaderInstall()
    {
        nv3047_memorymanager_autoruntime_install();
    }
};

// Header-local by design. install() is idempotent, and the strong symbol
// reference guarantees that simply including NV3047_Memory.h activates the
// runtime even when no other library API is referenced by the sketch.
static AutoRuntimeHeaderInstall
    auto_runtime_header_install;

} // namespace Detail
} // namespace NV3047Memory
