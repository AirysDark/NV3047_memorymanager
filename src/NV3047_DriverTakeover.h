#pragma once

#include "NV3047_MemoryProviderABI.h"

#ifdef __cplusplus
extern "C" {
#endif

// Implemented by NV3047_drivers when that library is linked.
// Weak declaration keeps NV3047_memorymanager usable on its own.
bool nv3047_driver_register_memory_provider(
    const NV3047MemoryProviderV1* provider)
    __attribute__((weak));

// Strong symbol implemented by this library. Referencing it from the automatic
// registrar ensures Arduino's linker pulls the provider bridge into the image
// when NV3047_Memory.h is included by the sketch.
const NV3047MemoryProviderV1*
nv3047_memorymanager_provider_v1();

#ifdef __cplusplus
}
#endif

namespace NV3047Memory {
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
