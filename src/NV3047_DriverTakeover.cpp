#include "NV3047_DriverTakeover.h"

#include "NV3047_AutoMemory.h"

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

    if (width == 0 || height == 0) {
        return false;
    }

    // Current external framebuffer owner is deliberately a pair.
    if (buffer_count != 2) {
        return false;
    }

    const size_t expected_bytes =
        static_cast<size_t>(width) *
        static_cast<size_t>(height) *
        sizeof(uint16_t);

    if (buffer_size_bytes != expected_bytes) {
        return false;
    }

    // FramebufferPair currently guarantees 64-byte framebuffer alignment.
    if (buffer_alignment > 64) {
        return false;
    }

    NV3047Memory::AutoMemory& automatic =
        NV3047Memory::AutoMemory::instance();

    if (automatic.isReady()) {
        NV3047Memory::FramebufferPair& frames =
            automatic.framebuffers();

        if (!frames.isReady() ||
            frames.width() != width ||
            frames.height() != height ||
            frames.bytesPerBuffer() != buffer_size_bytes) {
            return false;
        }

        bridge_active = true;
        started_by_bridge = false;
        return true;
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

    if (!automatic.begin(config)) {
        return false;
    }

    if (!automatic.framebuffers().isReady()) {
        automatic.end();
        return false;
    }

    bridge_active = true;
    started_by_bridge = true;
    return true;
}

void providerEnd() {
    NV3047Memory::AutoMemory& automatic =
        NV3047Memory::AutoMemory::instance();

    bridge_active = false;

    // If the application started AutoMemory before the driver, ownership of
    // its lifetime stays with the application. If the bridge started it, the
    // bridge also performs cleanup.
    if (started_by_bridge &&
        automatic.isReady()) {
        automatic.end();
    }

    started_by_bridge = false;
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

    NV3047Memory::AutoMemory::instance().
        beginFrame();
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
