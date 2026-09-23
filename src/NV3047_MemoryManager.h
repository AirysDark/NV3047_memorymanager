#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace NV3047Memory
{

enum class MemoryRegion : uint8_t
{
    Auto = 0,
    Internal,
    PSRAM,
    DMA
};

enum class MemoryPurpose : uint8_t
{
    General = 0,
    UIObject,
    Framebuffer,
    Bitmap,
    Scratch,
    DMA,
    Control
};

enum class MemoryPressure : uint8_t
{
    Normal = 0,
    Warning,
    Critical
};

struct HeapStats
{
    size_t totalBytes = 0;
    size_t freeBytes = 0;
    size_t minimumFreeBytes = 0;
    size_t largestFreeBlock = 0;
};

struct MemoryStats
{
    HeapStats internal;
    HeapStats psram;

    bool psramAvailable = false;

    size_t trackedBytes = 0;
    size_t peakTrackedBytes = 0;
    size_t activeAllocations = 0;

    size_t scratchCapacity = 0;
    size_t scratchUsed = 0;
    size_t scratchPeakUsed = 0;

    uint32_t successfulAllocations = 0;
    uint32_t failedAllocations = 0;

    MemoryPressure pressure = MemoryPressure::Normal;
};

struct MemoryConfig
{
    // General allocations at or above this size prefer PSRAM.
    size_t largeAllocationThreshold = 4096;

    // RAM kept free so display/UI allocations do not starve the runtime.
    size_t internalReserveBytes = 48 * 1024;
    size_t psramReserveBytes = 128 * 1024;

    // Pressure thresholds.
    size_t warningInternalFreeBytes = 64 * 1024;
    size_t criticalInternalFreeBytes = 32 * 1024;
    size_t warningPSRAMFreeBytes = 256 * 1024;
    size_t criticalPSRAMFreeBytes = 128 * 1024;

    // Reusable temporary per-frame arena.
    size_t scratchBytes = 64 * 1024;

    bool preferPSRAM = true;
    bool allowFallback = true;

    // Keep optional graphics/scratch work from consuming internal RAM when
    // PSRAM is present but under pressure or fragmented.
    bool allowBitmapFallback = false;
    bool allowScratchFallback = false;

    // NV3047 driver_overhaul_v2 requires framebuffer storage in PSRAM.
    // Disable only for hardware that intentionally supports internal-RAM frames.
    bool requirePSRAMForFramebuffer = true;

    // Ownership bookkeeping and tags are always retained for safe cleanup.
    // This flag controls verbose per-allocation listing in dump().
    bool enableTracking = true;

    // service() monitoring cadence.
    uint32_t monitorIntervalMs = 1000;
};

using PressureCallback =
    void (*)(
        MemoryPressure level,
        const MemoryStats& stats
    );

class MemoryManager final
{
public:
    static MemoryManager& instance();

    MemoryManager(const MemoryManager&) = delete;
    MemoryManager& operator=(const MemoryManager&) = delete;

    bool begin(const MemoryConfig& config = MemoryConfig());
    void end();

    bool isReady() const;
    bool hasPSRAM() const;

    void* allocate(
        size_t bytes,
        MemoryPurpose purpose = MemoryPurpose::General,
        size_t alignment = 4,
        const char* tag = nullptr
    );

    void* allocateIn(
        size_t bytes,
        MemoryRegion region,
        size_t alignment = 4,
        const char* tag = nullptr,
        bool allowFallback = true
    );

    void* calloc(
        size_t count,
        size_t itemSize,
        MemoryPurpose purpose = MemoryPurpose::General,
        size_t alignment = 4,
        const char* tag = nullptr
    );

    void* reallocate(
        void* pointer,
        size_t newBytes,
        MemoryPurpose purpose = MemoryPurpose::General,
        size_t alignment = 4,
        const char* tag = nullptr
    );

    template <typename T>
    T* allocateArray(
        size_t count,
        MemoryPurpose purpose = MemoryPurpose::General,
        const char* tag = nullptr
    )
    {
        if (
            count != 0 &&
            count > (SIZE_MAX / sizeof(T))
        )
        {
            if (ready_)
            {
                noteFailure();
            }

            return nullptr;
        }

        return static_cast<T*>(
            allocate(
                count * sizeof(T),
                purpose,
                alignof(T),
                tag
            )
        );
    }

    uint16_t* allocateFramebuffer(
        uint16_t width,
        uint16_t height,
        const char* tag = "framebuffer"
    );

    uint16_t* allocateRGB565(
        size_t pixelCount,
        const char* tag = "rgb565"
    );

    void* allocateDMA(
        size_t bytes,
        size_t alignment = 4,
        const char* tag = "dma"
    );

    // Permanent manager/broker control memory. Always internal RAM,
    // never falls back to PSRAM, and is not intended for application data.
    void* allocateControl(
        size_t bytes,
        size_t alignment = 8,
        const char* tag = "manager-control"
    );

    void release(void* pointer);
    size_t releaseTag(const char* tag);
    void releaseAll();

    bool owns(const void* pointer) const;
    size_t allocationSize(const void* pointer) const;
    MemoryRegion allocationRegion(const void* pointer) const;

    // Public policy/heap queries used by the adaptive broker.
    MemoryRegion preferredRegion(
        size_t bytes,
        MemoryPurpose purpose
    ) const;

    HeapStats regionStats(
        MemoryRegion region
    ) const;

    bool validate() const;

    // Per-frame transient arena.
    void beginFrame();
    void* scratch(
        size_t bytes,
        size_t alignment = 4
    );
    void resetScratch();

    size_t scratchMark() const;
    bool rewindScratch(size_t mark);

    size_t scratchCapacity() const;
    size_t scratchUsed() const;

    // getStats() is an explicit fresh diagnostic sample.
    MemoryStats getStats() const;

    // Hot-path pressure reads use the most recent sampled state and never
    // perform heap-capability queries themselves.
    MemoryStats cachedStats() const;
    MemoryPressure pressure() const;

    // Refresh the cached heap/pressure snapshot when dirty or due. Returns
    // true only when a real heap sample was taken.
    bool refreshStats(
        bool force = false
    );

    uint32_t heapSampleCount() const;

    void setPressureCallback(
        PressureCallback callback
    );

    void service();

    void dump(Stream& output = Serial) const;

private:
    static constexpr size_t MAX_TRACKED_ALLOCATIONS = 128;
    static constexpr size_t TAG_LENGTH = 24;

    struct AllocationRecord
    {
        void* pointer;
        size_t bytes;
        size_t alignment;
        MemoryRegion region;
        MemoryPurpose purpose;
        bool used;
        char tag[TAG_LENGTH];
    };

    MemoryManager();

    MemoryConfig config_;

    bool ready_;
    bool psram_available_;

    mutable StaticSemaphore_t mutex_storage_;
    mutable SemaphoreHandle_t mutex_;

    AllocationRecord records_[MAX_TRACKED_ALLOCATIONS];

    size_t tracked_bytes_;
    size_t peak_tracked_bytes_;
    size_t active_allocations_;

    uint32_t successful_allocations_;
    uint32_t failed_allocations_;

    uint8_t* scratch_base_;
    size_t scratch_capacity_;
    size_t scratch_offset_;
    size_t scratch_peak_;
    MemoryRegion scratch_region_;

    PressureCallback pressure_callback_;
    MemoryPressure last_pressure_;
    uint32_t last_monitor_ms_;

    mutable MemoryStats cached_stats_;
    mutable bool cached_stats_valid_;

    uint32_t heap_revision_;
    uint32_t sampled_heap_revision_;
    uint32_t heap_sample_count_;

    void lock() const;
    void unlock() const;

    static size_t normalizeAlignment(
        size_t alignment
    );

    MemoryRegion chooseRegion(
        size_t bytes,
        MemoryPurpose purpose
    ) const;

    void* allocateTracked(
        size_t bytes,
        MemoryPurpose purpose,
        MemoryRegion preferredRegion,
        size_t alignment,
        const char* tag,
        bool allowFallback
    );

    void* rawAllocate(
        size_t bytes,
        MemoryRegion region,
        size_t alignment
    ) const;

    static void rawFree(void* pointer);

    bool trackAllocation(
        void* pointer,
        size_t bytes,
        size_t alignment,
        MemoryRegion region,
        MemoryPurpose purpose,
        const char* tag
    );

    void noteFailure();

    HeapStats readHeap(
        uint32_t caps
    ) const;

    MemoryStats sampleStats() const;
    void markHeapStatsDirty();

    MemoryPressure evaluatePressure(
        const MemoryStats& stats
    ) const;

    static const char* regionName(
        MemoryRegion region
    );

    static const char* purposeName(
        MemoryPurpose purpose
    );
};

} // namespace NV3047Memory
