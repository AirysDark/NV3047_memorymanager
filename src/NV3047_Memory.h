#pragma once

#include "NV3047_MemoryManager.h"
#include "NV3047_MemoryBroker.h"
#include "NV3047_ElasticBuffer.h"
#include "NV3047_ManagedBuffer.h"
#include "NV3047_FramebufferPair.h"
#include "NV3047_DMAPool.h"
#include "NV3047_ObjectPool.h"
#include "NV3047_AssetCache.h"
#include "NV3047_AutoMemory.h"
#include "NV3047_DriverTakeover.h"
#include "NV3047_AutoRuntime.h"

#ifndef NV3047_MEMORY_DISABLE_AUTORUNTIME
namespace NV3047Memory {
namespace Detail {
static AutoRuntimeHeaderInstall
    nv3047_memory_auto_runtime_install;
} // namespace Detail
} // namespace NV3047Memory
#endif
