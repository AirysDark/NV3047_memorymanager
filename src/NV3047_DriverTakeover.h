#pragma once

#include "NV3047_MemoryProviderABI.h"

#ifdef __cplusplus
extern "C" {
#endif

// Implemented by NV3047_drivers when that library is linked.
// If the driver header was already included, reuse its declaration.
// Otherwise use a weak declaration so NV3047_memorymanager remains standalone.
#ifndef NV3047_DRIVER_MEMORY_PROVIDER_API_DECLARED
bool nv3047_driver_register_memory_provider(
    const NV3047MemoryProviderV1* provider)
    __attribute__((weak));
#endif

// Strong symbol implemented by this library. Referencing it from the automatic
// registrar ensures Arduino's linker pulls the provider bridge into the image
// when NV3047_Memory.h is included by the sketch.
const NV3047MemoryProviderV1*
nv3047_memorymanager_provider_v1();

bool nv3047_memorymanager_driver_bridge_active();

void nv3047_memorymanager_takeover_profiling_changed(
    bool enabled
);

#ifdef __cplusplus
}
#endif

namespace NV3047Memory {

struct DriverTakeoverPerformanceStats
{
    uint32_t providerReadyCalls = 0;
    uint32_t providerFrontCalls = 0;
    uint32_t providerDrawCalls = 0;
    uint32_t providerSwapCalls = 0;

    uint32_t providerBeginFrameCalls = 0;
    uint32_t providerBeginFrameResets = 0;
    uint32_t providerBeginFrameNoOps = 0;

    uint32_t providerServiceCalls = 0;
    uint32_t providerServiceFastExits = 0;
    uint32_t providerServiceFullPasses = 0;

    uint32_t providerDMAAcquireCalls = 0;
    uint32_t providerDMAReleaseCalls = 0;

    uint64_t totalProviderServiceUs = 0;
    uint32_t maxProviderServiceUs = 0;
};

DriverTakeoverPerformanceStats
driverTakeoverPerformanceStats();

void resetDriverTakeoverPerformanceStats();

namespace Detail {

class DriverTakeoverAutoRegister final {
public:
    DriverTakeoverAutoRegister() {
        if (!nv3047_driver_register_memory_provider) {
            return;
        }

        const NV3047MemoryProviderV1* provider =
            nv3047_memorymanager_provider_v1();

        if (provider) {
            nv3047_driver_register_memory_provider(provider);
        }
    }
};

// Header-local instance intentionally runs before Arduino setup().
// Registration is idempotent on the driver side, so multiple translation
// units including the umbrella header are safe.
static DriverTakeoverAutoRegister
    driver_takeover_auto_register;

} // namespace Detail
} // namespace NV3047Memory
