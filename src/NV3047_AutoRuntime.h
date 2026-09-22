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

bool nv3047_memorymanager_autoruntime_minimal_owned();
void nv3047_memorymanager_autoruntime_clear_minimal_owned();

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
    bool minimalModeOwned = false;

    uint32_t automaticStarts = 0;
    uint32_t failedStarts = 0;
    uint32_t backgroundServicePasses = 0;

    size_t runtimeStackBytes = 0;
    size_t minimumFreeStackBytes = 0;

    // Permanent compile-time storage used by the background runtime task,
    // mutex and state.
    size_t staticRuntimeBytes = 0;

    // MemoryManager + AutoMemory static objects + broker control arena.
    size_t managerControlBytes = 0;

    // Combined manager control + automatic runtime control footprint.
    size_t totalPermanentControlBytes = 0;
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

// NV3047_Memory.h owns the one include-triggered installer. Keeping the
// instance out of this internal header prevents library translation units from
// accidentally defeating NV3047_MEMORY_DISABLE_AUTORUNTIME in advanced tests.

} // namespace Detail
} // namespace NV3047Memory
