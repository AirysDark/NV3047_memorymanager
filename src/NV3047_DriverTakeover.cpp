#include "NV3047_DriverTakeover.h"

#include "NV3047_AutoMemory.h"
#include "NV3047_AutoRuntime.h"

namespace {

bool bridge_active = false;
bool started_by_bridge = false;

bool providerBegin(
    uint16_t width,
    uint16_t height,
    size_t buffer_count,
    size_t buffer_size_bytes,
    size_t buffer_alignment,
    bool zero_buffers,
    size_t dma_block_bytes,
    size_t dma_alignment) {

    (void)zero_buffers;

    if (!nv3047_memorymanager_autoruntime_lock()) {
        return false;
    }

    bool success = false;

    if (width == 0 || height == 0) {
        nv3047_memorymanager_autoruntime_unlock();
        return false;
    }

    if (buffer_count != 2) {
        nv3047_memorymanager_autoruntime_unlock();
        return false;
    }

    const size_t expected_bytes =
        static_cast<size_t>(width) *
        static_cast<size_t>(height) *
        sizeof(uint16_t);

    if (buffer_size_bytes != expected_bytes) {
        nv3047_memorymanager_autoruntime_unlock();
        return false;
    }

    if (buffer_alignment > 64) {
        nv3047_memorymanager_autoruntime_unlock();
        return false;
    }

    NV3047Memory::AutoMemory& automatic =
        NV3047Memory::AutoMemory::instance();

    if (automatic.isReady()) {
        NV3047Memory::FramebufferPair& frames =
            automatic.framebuffers();

        if (frames.isReady()) {
            if (frames.width() != width ||
                frames.height() != height ||
                frames.bytesPerBuffer() != buffer_size_bytes) {
                nv3047_memorymanager_autoruntime_unlock();
                return false;
            }

            bridge_active = true;
            started_by_bridge = false;

            automatic.noteUIActivity();

            nv3047_memorymanager_autoruntime_unlock();
            return true;
        }

        // The include-only runtime starts a deliberately lightweight broker
        // before setup(). That instance is safe to replace with the driver's
        // full framebuffer/DMA configuration. A manually-created AutoMemory
        // instance is not silently destroyed.
        if (!nv3047_memorymanager_autoruntime_minimal_owned()) {
            nv3047_memorymanager_autoruntime_unlock();
            return false;
        }

        automatic.end();
        nv3047_memorymanager_autoruntime_clear_minimal_owned();
    }

    NV3047Memory::AutoMemoryConfig config;

    config.allocateFramebufferPair = true;
    config.framebufferWidth = width;
    config.framebufferHeight = height;

    config.memory.requirePSRAMForFramebuffer = true;
    config.memory.preferPSRAM = true;

    config.enableDMAPool =
        dma_block_bytes != 0;

    if (config.enableDMAPool) {
        config.dmaBlockBytes = dma_block_bytes;
        config.dmaBlockCount = 1;
        config.dmaAlignment =
            dma_alignment == 0
                ? 4
                : dma_alignment;
    }

    if (automatic.begin(config) &&
        automatic.framebuffers().isReady()) {

        bridge_active = true;
        started_by_bridge = true;

        automatic.noteUIActivity();
        success = true;
    } else if (automatic.isReady()) {
        automatic.end();
    }

    nv3047_memorymanager_autoruntime_unlock();
    return success;
}

void providerEnd() {
    if (!nv3047_memorymanager_autoruntime_lock()) {
        return;
    }

    NV3047Memory::AutoMemory& automatic =
        NV3047Memory::AutoMemory::instance();

    bridge_active = false;

    // If the bridge started the full framebuffer/DMA configuration it also
    // tears it down. The background include-only runtime will automatically
    // restore lightweight manager-only mode afterward.
    if (started_by_bridge &&
        automatic.isReady()) {
        automatic.end();
    }

    started_by_bridge = false;

    nv3047_memorymanager_autoruntime_unlock();
}

bool providerReady() {
    if (!bridge_active) {
        return false;
    }

    NV3047Memory::AutoMemory& automatic =
        NV3047Memory::AutoMemory::instance();

    return
        automatic.isReady() &&
        automatic.framebuffers().isReady();
}

uint16_t* providerFront() {
    if (!providerReady()) {
        return nullptr;
    }

    return
        NV3047Memory::AutoMemory::instance().
            framebuffers().front();
}

uint16_t* providerDraw() {
    if (!providerReady()) {
        return nullptr;
    }

    return
        NV3047Memory::AutoMemory::instance().
            framebuffers().back();
}

void providerSwap() {
    if (!providerReady()) {
        return;
    }

    NV3047Memory::AutoMemory::instance().
        framebuffers().swapRoles();
}

size_t providerBufferCount() {
    return providerReady() ? 2 : 0;
}

size_t providerBufferSizeBytes() {
    if (!providerReady()) {
        return 0;
    }

    return
        NV3047Memory::AutoMemory::instance().
            framebuffers().bytesPerBuffer();
}

size_t providerTotalAllocatedBytes() {
    if (!providerReady()) {
        return 0;
    }

    return
        NV3047Memory::AutoMemory::instance().
            framebuffers().totalBytes();
}

size_t providerFreeManagedBytes() {
    if (!providerReady()) {
        return 0;
    }

    const NV3047Memory::MemoryStats stats =
        NV3047Memory::AutoMemory::instance().
            memory().getStats();

    return stats.psram.freeBytes;
}

size_t providerLargestFreeManagedBlockBytes() {
    if (!providerReady()) {
        return 0;
    }

    const NV3047Memory::MemoryStats stats =
        NV3047Memory::AutoMemory::instance().
            memory().getStats();

    return stats.psram.largestFreeBlock;
}

void providerBeginFrame() {
    if (!providerReady()) {
        return;
    }

    NV3047Memory::AutoMemory& automatic =
        NV3047Memory::AutoMemory::instance();

    // Real display-frame activity is the automatic UI activity signal.
    // No sketch-side noteUIActivity() call is required.
    automatic.noteUIActivity();
    automatic.beginFrame();
}

void providerService() {
    if (!providerReady()) {
        return;
    }

    NV3047Memory::AutoMemory::instance().
        service();
}

void* providerAcquireDMA(
    size_t bytes,
    size_t alignment) {

    if (!providerReady() || bytes == 0) {
        return nullptr;
    }

    NV3047Memory::AutoMemory& automatic =
        NV3047Memory::AutoMemory::instance();

    NV3047Memory::DMAPool& pool =
        automatic.dmaPool();

    if (pool.blockCount() != 0 &&
        pool.blockBytes() >= bytes) {

        void* pooled = pool.acquire();

        if (pooled) {
            return pooled;
        }
    }

    return automatic.memory().allocateDMA(
        bytes,
        alignment == 0 ? 4 : alignment,
        "nv3047-driver-dma");
}

void providerReleaseDMA(void* pointer) {
    if (!pointer) {
        return;
    }

    NV3047Memory::AutoMemory& automatic =
        NV3047Memory::AutoMemory::instance();

    if (!automatic.isReady()) {
        return;
    }

    NV3047Memory::DMAPool& pool =
        automatic.dmaPool();

    if (pool.owns(pointer)) {
        pool.release(pointer);
        return;
    }

    NV3047Memory::MemoryManager& manager =
        automatic.memory();

    if (manager.owns(pointer)) {
        manager.release(pointer);
    }
}

const NV3047MemoryProviderV1 provider = {
    NV3047_MEMORY_PROVIDER_ABI_VERSION,
    &providerBegin,
    &providerEnd,
    &providerReady,
    &providerFront,
    &providerDraw,
    &providerSwap,
    &providerBufferCount,
    &providerBufferSizeBytes,
    &providerTotalAllocatedBytes,
    &providerFreeManagedBytes,
    &providerLargestFreeManagedBlockBytes,
    &providerBeginFrame,
    &providerService,
    &providerAcquireDMA,
    &providerReleaseDMA
};

} // namespace

extern "C" const NV3047MemoryProviderV1*
nv3047_memorymanager_provider_v1() {
    return &provider;
}

extern "C" bool
nv3047_memorymanager_driver_bridge_active() {
    return bridge_active;
}
