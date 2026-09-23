#include "NV3047_DriverTakeover.h"

#include "NV3047_AutoMemory.h"
#include "NV3047_AutoRuntime.h"

namespace {

struct DriverTakeoverSession
{
    volatile bool ready = false;
    bool startedByBridge = false;
    bool profiling = false;

    NV3047Memory::AutoMemory* automatic = nullptr;
    NV3047Memory::FramebufferPair* frames = nullptr;
    NV3047Memory::MemoryManager* manager = nullptr;
    NV3047Memory::DMAPool* dma = nullptr;

    size_t bufferSizeBytes = 0;
    size_t totalFramebufferBytes = 0;

    NV3047Memory::DriverTakeoverPerformanceStats perf;
};

DriverTakeoverSession session;

inline bool sessionReady()
{
    return session.ready;
}

void clearSessionRuntime()
{
    session.ready = false;
    session.startedByBridge = false;
    session.automatic = nullptr;
    session.frames = nullptr;
    session.manager = nullptr;
    session.dma = nullptr;
    session.bufferSizeBytes = 0;
    session.totalFramebufferBytes = 0;
    session.profiling = false;
}

void establishSession(
    NV3047Memory::AutoMemory& automatic,
    bool startedByBridge)
{
    session.automatic = &automatic;
    session.frames = &automatic.framebuffers();
    session.manager = &automatic.memory();
    session.dma = &automatic.dmaPool();

    session.bufferSizeBytes =
        session.frames->bytesPerBuffer();

    session.totalFramebufferBytes =
        session.frames->totalBytes();

    session.startedByBridge =
        startedByBridge;

    session.profiling =
        automatic.performanceProfilingEnabled();

    // Publish ready last. Provider callbacks trust the cached references while
    // this flag is true and stop using them as soon as providerEnd clears it.
    session.ready = true;
}

inline void recordProviderServiceElapsed(
    uint32_t startUs)
{
    if (!session.profiling)
    {
        return;
    }

    const uint32_t elapsed =
        static_cast<uint32_t>(
            micros() - startUs
        );

    session.perf.totalProviderServiceUs +=
        elapsed;

    if (
        elapsed >
        session.perf.maxProviderServiceUs
    )
    {
        session.perf.maxProviderServiceUs =
            elapsed;
    }
}

bool providerBegin(
    uint16_t width,
    uint16_t height,
    size_t buffer_count,
    size_t buffer_size_bytes,
    size_t buffer_alignment,
    bool zero_buffers,
    size_t dma_block_bytes,
    size_t dma_alignment)
{
    (void)zero_buffers;

    if (!nv3047_memorymanager_autoruntime_lock())
    {
        return false;
    }

    if (
        width == 0 ||
        height == 0 ||
        buffer_count != 2
    )
    {
        nv3047_memorymanager_autoruntime_unlock();
        return false;
    }

    const size_t expected_bytes =
        static_cast<size_t>(width) *
        static_cast<size_t>(height) *
        sizeof(uint16_t);

    if (
        buffer_size_bytes != expected_bytes ||
        buffer_alignment > 64
    )
    {
        nv3047_memorymanager_autoruntime_unlock();
        return false;
    }

    // Idempotent begin on an already-established session.
    if (sessionReady())
    {
        const bool matches =
            session.frames &&
            session.frames->width() == width &&
            session.frames->height() == height &&
            session.bufferSizeBytes ==
                buffer_size_bytes;

        nv3047_memorymanager_autoruntime_unlock();
        return matches;
    }

    NV3047Memory::AutoMemory& automatic =
        NV3047Memory::AutoMemory::instance();

    if (automatic.isReady())
    {
        NV3047Memory::FramebufferPair& frames =
            automatic.framebuffers();

        if (frames.isReady())
        {
            if (
                frames.width() != width ||
                frames.height() != height ||
                frames.bytesPerBuffer() !=
                    buffer_size_bytes ||
                !automatic.validate()
            )
            {
                nv3047_memorymanager_autoruntime_unlock();
                return false;
            }

            establishSession(
                automatic,
                false
            );

            automatic.noteUIActivity();

            nv3047_memorymanager_autoruntime_unlock();
            return true;
        }

        // The include-only runtime starts lightweight manager/broker mode
        // before setup(). It is safe to replace only when that runtime owns it.
        if (!nv3047_memorymanager_autoruntime_minimal_owned())
        {
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

    if (config.enableDMAPool)
    {
        config.dmaBlockBytes =
            dma_block_bytes;

        config.dmaBlockCount = 1;

        config.dmaAlignment =
            dma_alignment == 0
                ? 4
                : dma_alignment;
    }

    bool success = false;

    if (
        automatic.begin(config) &&
        automatic.framebuffers().isReady() &&
        automatic.validate()
    )
    {
        establishSession(
            automatic,
            true
        );

        automatic.noteUIActivity();
        success = true;
    }
    else if (automatic.isReady())
    {
        automatic.end();
    }

    nv3047_memorymanager_autoruntime_unlock();
    return success;
}

void providerEnd()
{
    if (!nv3047_memorymanager_autoruntime_lock())
    {
        return;
    }

    NV3047Memory::AutoMemory* automatic =
        session.automatic;

    const bool startedByBridge =
        session.startedByBridge;

    // Invalidate first so frame callbacks stop trusting cached references
    // before any owned resource is torn down.
    clearSessionRuntime();

    if (
        startedByBridge &&
        automatic &&
        automatic->isReady()
    )
    {
        automatic->end();
    }

    nv3047_memorymanager_autoruntime_unlock();
}

bool providerReady()
{
    if (session.profiling)
    {
        ++session.perf.providerReadyCalls;
    }

    return sessionReady();
}

uint16_t* providerFront()
{
    if (!sessionReady())
    {
        return nullptr;
    }

    if (session.profiling)
    {
        ++session.perf.providerFrontCalls;
    }

    return
        session.frames->
            frontUnchecked();
}

uint16_t* providerDraw()
{
    if (!sessionReady())
    {
        return nullptr;
    }

    if (session.profiling)
    {
        ++session.perf.providerDrawCalls;
    }

    return
        session.frames->
            backUnchecked();
}

void providerSwap()
{
    if (!sessionReady())
    {
        return;
    }

    if (session.profiling)
    {
        ++session.perf.providerSwapCalls;
    }

    session.frames->
        swapRolesUnchecked();
}

size_t providerBufferCount()
{
    return
        sessionReady()
            ? 2
            : 0;
}

size_t providerBufferSizeBytes()
{
    return
        sessionReady()
            ? session.bufferSizeBytes
            : 0;
}

size_t providerTotalAllocatedBytes()
{
    return
        sessionReady()
            ? session.totalFramebufferBytes
            : 0;
}

size_t providerFreeManagedBytes()
{
    if (!sessionReady())
    {
        return 0;
    }

    // Explicit diagnostics intentionally request a fresh heap sample.
    const NV3047Memory::MemoryStats stats =
        session.manager->getStats();

    return stats.psram.freeBytes;
}

size_t providerLargestFreeManagedBlockBytes()
{
    if (!sessionReady())
    {
        return 0;
    }

    // Explicit diagnostics intentionally request a fresh heap sample.
    const NV3047Memory::MemoryStats stats =
        session.manager->getStats();

    return stats.psram.largestFreeBlock;
}

void providerBeginFrame()
{
    if (!sessionReady())
    {
        return;
    }

    const bool profile =
        session.profiling;

    if (profile)
    {
        ++session.perf.
            providerBeginFrameCalls;
    }

    bool reset = false;

    if (
        session.manager->
            scratchWasUsed()
    )
    {
        reset =
            session.manager->
                beginFrameIfNeeded();
    }

    if (profile)
    {
        if (reset)
        {
            ++session.perf.
                providerBeginFrameResets;
        }
        else
        {
            ++session.perf.
                providerBeginFrameNoOps;
        }
    }
}

void providerService()
{
    if (!sessionReady())
    {
        return;
    }

    const bool profile =
        session.profiling;

    const uint32_t startUs =
        profile
            ? micros()
            : 0;

    if (profile)
    {
        ++session.perf.
            providerServiceCalls;
    }

    // One clock read at the top of the bridge. The same timestamp is passed
    // through AutoMemory, MemoryManager and MemoryBroker when work is due.
    const uint32_t now =
        millis();

    if (
        !session.automatic->
            serviceDue(now)
    )
    {
        if (profile)
        {
            ++session.perf.
                providerServiceFastExits;

            recordProviderServiceElapsed(
                startUs
            );
        }

        return;
    }

    if (profile)
    {
        ++session.perf.
            providerServiceFullPasses;
    }

    session.automatic->
        service(now);

    if (profile)
    {
        recordProviderServiceElapsed(
            startUs
        );
    }
}

void* providerAcquireDMA(
    size_t bytes,
    size_t alignment)
{
    if (
        !sessionReady() ||
        bytes == 0
    )
    {
        return nullptr;
    }

    if (session.profiling)
    {
        ++session.perf.
            providerDMAAcquireCalls;
    }

    if (
        session.dma->blockCount() != 0 &&
        session.dma->blockBytes() >= bytes
    )
    {
        void* pooled =
            session.dma->acquire();

        if (pooled)
        {
            return pooled;
        }
    }

    return
        session.manager->allocateDMA(
            bytes,
            alignment == 0
                ? 4
                : alignment,
            "nv3047-driver-dma"
        );
}

void providerReleaseDMA(
    void* pointer)
{
    if (
        !pointer ||
        !sessionReady()
    )
    {
        return;
    }

    if (session.profiling)
    {
        ++session.perf.
            providerDMAReleaseCalls;
    }

    if (session.dma->owns(pointer))
    {
        session.dma->release(pointer);
        return;
    }

    if (session.manager->owns(pointer))
    {
        session.manager->release(pointer);
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
nv3047_memorymanager_provider_v1()
{
    return &provider;
}

extern "C" bool
nv3047_memorymanager_driver_bridge_active()
{
    return sessionReady();
}

extern "C" void
nv3047_memorymanager_takeover_profiling_changed(
    bool enabled)
{
    session.profiling =
        enabled &&
        sessionReady();
}

extern "C" void
nv3047_memorymanager_takeover_auto_memory_ending()
{
    if (sessionReady())
    {
        clearSessionRuntime();
    }
}

namespace NV3047Memory
{

DriverTakeoverPerformanceStats
driverTakeoverPerformanceStats()
{
    return session.perf;
}

void resetDriverTakeoverPerformanceStats()
{
    session.perf =
        DriverTakeoverPerformanceStats();
}

} // namespace NV3047Memory
