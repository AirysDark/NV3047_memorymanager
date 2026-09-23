#include "NV3047_MemoryManager.h"

#include <esp_heap_caps.h>
#include <string.h>

namespace NV3047Memory
{

MemoryManager& MemoryManager::instance()
{
    static MemoryManager manager;
    return manager;
}

MemoryManager::MemoryManager()
    : ready_(false),
      psram_available_(false),
      mutex_(nullptr),
      tracked_bytes_(0),
      peak_tracked_bytes_(0),
      active_allocations_(0),
      successful_allocations_(0),
      failed_allocations_(0),
      scratch_base_(nullptr),
      scratch_capacity_(0),
      scratch_offset_(0),
      scratch_peak_(0),
      scratch_region_(MemoryRegion::Auto),
      pressure_callback_(nullptr),
      last_pressure_(MemoryPressure::Normal),
      last_monitor_ms_(0),
      cached_stats_(),
      cached_stats_valid_(false),
      heap_revision_(0),
      sampled_heap_revision_(0),
      heap_sample_count_(0)
{
    memset(
        records_,
        0,
        sizeof(records_)
    );
}

bool MemoryManager::begin(
    const MemoryConfig& config
)
{
    if (ready_)
    {
        return true;
    }

    mutex_ =
        xSemaphoreCreateMutexStatic(
            &mutex_storage_
        );

    if (!mutex_)
    {
        return false;
    }

    config_ = config;

    tracked_bytes_ = 0;
    peak_tracked_bytes_ = 0;
    active_allocations_ = 0;
    successful_allocations_ = 0;
    failed_allocations_ = 0;

    scratch_base_ = nullptr;
    scratch_capacity_ = 0;
    scratch_offset_ = 0;
    scratch_peak_ = 0;
    scratch_region_ = MemoryRegion::Auto;

    pressure_callback_ = nullptr;
    last_monitor_ms_ = millis();

    cached_stats_ =
        MemoryStats();
    cached_stats_valid_ = false;
    heap_revision_ = 0;
    sampled_heap_revision_ = 0;
    heap_sample_count_ = 0;

    memset(
        records_,
        0,
        sizeof(records_)
    );

    psram_available_ =
        heap_caps_get_total_size(
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        ) > 0;

    ready_ = true;

    if (config_.scratchBytes > 0)
    {
        MemoryRegion preferred =
            (
                psram_available_ &&
                config_.preferPSRAM
            )
                ? MemoryRegion::PSRAM
                : MemoryRegion::Internal;

        scratch_base_ =
            static_cast<uint8_t*>(
                rawAllocate(
                    config_.scratchBytes,
                    preferred,
                    64
                )
            );

        if (
            !scratch_base_ &&
            config_.allowFallback &&
            config_.allowScratchFallback
        )
        {
            MemoryRegion fallback =
                (
                    preferred == MemoryRegion::PSRAM
                )
                    ? MemoryRegion::Internal
                    : MemoryRegion::PSRAM;

            if (
                fallback != MemoryRegion::PSRAM ||
                psram_available_
            )
            {
                scratch_base_ =
                    static_cast<uint8_t*>(
                        rawAllocate(
                            config_.scratchBytes,
                            fallback,
                            64
                        )
                    );

                if (scratch_base_)
                {
                    preferred = fallback;
                }
            }
        }

        if (scratch_base_)
        {
            scratch_capacity_ =
                config_.scratchBytes;

            scratch_region_ =
                preferred;
        }
        else
        {
            noteFailure();
        }
    }

    // Scratch is a real heap allocation even though it is intentionally
    // outside the tracked allocation table.
    markHeapStatsDirty();

    refreshStats(true);

    last_pressure_ =
        cached_stats_.pressure;

    return true;
}

void MemoryManager::end()
{
    if (!ready_)
    {
        return;
    }

    releaseAll();

    if (scratch_base_)
    {
        rawFree(scratch_base_);
        scratch_base_ = nullptr;
    }

    scratch_capacity_ = 0;
    scratch_offset_ = 0;
    scratch_peak_ = 0;
    scratch_region_ = MemoryRegion::Auto;

    ready_ = false;
    psram_available_ = false;
    pressure_callback_ = nullptr;

    cached_stats_ =
        MemoryStats();
    cached_stats_valid_ = false;
    heap_revision_ = 0;
    sampled_heap_revision_ = 0;

    // mutex_storage_ is permanent object storage; no heap memory is freed.
    mutex_ = nullptr;
}

bool MemoryManager::isReady() const
{
    return ready_;
}

bool MemoryManager::hasPSRAM() const
{
    return psram_available_;
}

void MemoryManager::lock() const
{
    if (mutex_)
    {
        xSemaphoreTake(
            mutex_,
            portMAX_DELAY
        );
    }
}

void MemoryManager::unlock() const
{
    if (mutex_)
    {
        xSemaphoreGive(mutex_);
    }
}

size_t MemoryManager::normalizeAlignment(
    size_t alignment
)
{
    if (alignment < 4)
    {
        alignment = 4;
    }

    // heap_caps_aligned_alloc requires a power-of-two alignment.
    if ((alignment & (alignment - 1)) == 0)
    {
        return alignment;
    }

    size_t rounded = 4;

    while (rounded < alignment)
    {
        if (rounded > (SIZE_MAX >> 1))
        {
            return 0;
        }

        rounded <<= 1;
    }

    return rounded;
}

MemoryRegion MemoryManager::chooseRegion(
    size_t bytes,
    MemoryPurpose purpose
) const
{
    switch (purpose)
    {
        case MemoryPurpose::DMA:
            return MemoryRegion::DMA;

        case MemoryPurpose::Framebuffer:
        case MemoryPurpose::Bitmap:
        case MemoryPurpose::Scratch:
            if (
                psram_available_ &&
                config_.preferPSRAM
            )
            {
                return MemoryRegion::PSRAM;
            }

            return MemoryRegion::Internal;

        case MemoryPurpose::UIObject:
        case MemoryPurpose::Control:
            return MemoryRegion::Internal;

        case MemoryPurpose::General:
        default:
            if (
                psram_available_ &&
                config_.preferPSRAM &&
                bytes >=
                    config_.largeAllocationThreshold
            )
            {
                return MemoryRegion::PSRAM;
            }

            return MemoryRegion::Internal;
    }
}

void* MemoryManager::rawAllocate(
    size_t bytes,
    MemoryRegion region,
    size_t alignment
) const
{
    if (bytes == 0)
    {
        return nullptr;
    }

    alignment =
        normalizeAlignment(
            alignment
        );

    if (alignment == 0)
    {
        return nullptr;
    }

    uint32_t caps = 0;
    size_t reserve = 0;

    switch (region)
    {
        case MemoryRegion::PSRAM:
            if (!psram_available_)
            {
                return nullptr;
            }

            caps =
                MALLOC_CAP_SPIRAM |
                MALLOC_CAP_8BIT;

            reserve =
                config_.psramReserveBytes;
            break;

        case MemoryRegion::DMA:
            caps =
                MALLOC_CAP_DMA |
                MALLOC_CAP_INTERNAL |
                MALLOC_CAP_8BIT;

            reserve =
                config_.internalReserveBytes;
            break;

        case MemoryRegion::Internal:
            caps =
                MALLOC_CAP_INTERNAL |
                MALLOC_CAP_8BIT;

            reserve =
                config_.internalReserveBytes;
            break;

        case MemoryRegion::Auto:
        default:
            return nullptr;
    }

    // Protect the general internal heap even when requesting DMA memory.
    if (region == MemoryRegion::DMA)
    {
        const size_t internalFree =
            heap_caps_get_free_size(
                MALLOC_CAP_INTERNAL |
                MALLOC_CAP_8BIT
            );

        if (
            internalFree <= reserve ||
            bytes >
                (internalFree - reserve)
        )
        {
            return nullptr;
        }
    }

    const size_t freeBytes =
        heap_caps_get_free_size(caps);

    const size_t largestBlock =
        heap_caps_get_largest_free_block(
            caps
        );

    if (
        freeBytes <= reserve ||
        bytes > (freeBytes - reserve) ||
        bytes > largestBlock
    )
    {
        return nullptr;
    }

    return heap_caps_aligned_alloc(
        alignment,
        bytes,
        caps
    );
}

void MemoryManager::rawFree(
    void* pointer
)
{
    if (pointer)
    {
        heap_caps_free(pointer);
    }
}

bool MemoryManager::trackAllocation(
    void* pointer,
    size_t bytes,
    size_t alignment,
    MemoryRegion region,
    MemoryPurpose purpose,
    const char* tag
)
{
    if (!pointer)
    {
        return false;
    }

    lock();

    for (
        size_t i = 0;
        i < MAX_TRACKED_ALLOCATIONS;
        ++i
    )
    {
        AllocationRecord& record =
            records_[i];

        if (!record.used)
        {
            record.pointer = pointer;
            record.bytes = bytes;
            record.alignment = alignment;
            record.region = region;
            record.purpose = purpose;
            record.used = true;

            record.tag[0] = '\0';

            if (tag)
            {
                strncpy(
                    record.tag,
                    tag,
                    TAG_LENGTH - 1
                );

                record.tag[
                    TAG_LENGTH - 1
                ] = '\0';
            }

            tracked_bytes_ += bytes;
            ++active_allocations_;
            ++successful_allocations_;

            if (
                tracked_bytes_ >
                peak_tracked_bytes_
            )
            {
                peak_tracked_bytes_ =
                    tracked_bytes_;
            }

            unlock();
            markHeapStatsDirty();
            return true;
        }
    }

    ++failed_allocations_;
    unlock();

    return false;
}

void MemoryManager::noteFailure()
{
    lock();
    ++failed_allocations_;
    unlock();
}

void* MemoryManager::allocateTracked(
    size_t bytes,
    MemoryPurpose purpose,
    MemoryRegion preferredRegion,
    size_t alignment,
    const char* tag,
    bool allowFallback
)
{
    if (
        !ready_ ||
        bytes == 0
    )
    {
        if (ready_)
        {
            noteFailure();
        }

        return nullptr;
    }

    alignment =
        normalizeAlignment(
            alignment
        );

    if (alignment == 0)
    {
        noteFailure();
        return nullptr;
    }

    MemoryRegion actualRegion =
        preferredRegion;

    if (actualRegion == MemoryRegion::Auto)
    {
        actualRegion =
            chooseRegion(
                bytes,
                purpose
            );
    }

    void* pointer =
        rawAllocate(
            bytes,
            actualRegion,
            alignment
        );

    if (
        !pointer &&
        allowFallback &&
        actualRegion != MemoryRegion::DMA
    )
    {
        MemoryRegion fallback =
            (
                actualRegion ==
                MemoryRegion::PSRAM
            )
                ? MemoryRegion::Internal
                : MemoryRegion::PSRAM;

        if (
            fallback != MemoryRegion::PSRAM ||
            psram_available_
        )
        {
            pointer =
                rawAllocate(
                    bytes,
                    fallback,
                    alignment
                );

            if (pointer)
            {
                actualRegion =
                    fallback;
            }
        }
    }

    if (!pointer)
    {
        noteFailure();
        return nullptr;
    }

    if (
        !trackAllocation(
            pointer,
            bytes,
            alignment,
            actualRegion,
            purpose,
            tag
        )
    )
    {
        rawFree(pointer);
        return nullptr;
    }

    return pointer;
}

void* MemoryManager::allocate(
    size_t bytes,
    MemoryPurpose purpose,
    size_t alignment,
    const char* tag
)
{
    bool allowFallback =
        config_.allowFallback;

    MemoryRegion region =
        chooseRegion(
            bytes,
            purpose
        );

    if (
        purpose ==
        MemoryPurpose::Framebuffer &&
        config_.requirePSRAMForFramebuffer
    )
    {
        if (!psram_available_)
        {
            noteFailure();
            return nullptr;
        }

        region =
            MemoryRegion::PSRAM;

        allowFallback = false;
    }
    else if (
        purpose ==
        MemoryPurpose::Bitmap
    )
    {
        allowFallback =
            allowFallback &&
            config_.allowBitmapFallback;
    }
    else if (
        purpose ==
        MemoryPurpose::Scratch
    )
    {
        allowFallback =
            allowFallback &&
            config_.allowScratchFallback;
    }

    return allocateTracked(
        bytes,
        purpose,
        region,
        alignment,
        tag,
        allowFallback
    );
}

void* MemoryManager::allocateIn(
    size_t bytes,
    MemoryRegion region,
    size_t alignment,
    const char* tag,
    bool allowFallback
)
{
    return allocateTracked(
        bytes,
        MemoryPurpose::General,
        region,
        alignment,
        tag,
        allowFallback
    );
}

void* MemoryManager::calloc(
    size_t count,
    size_t itemSize,
    MemoryPurpose purpose,
    size_t alignment,
    const char* tag
)
{
    if (
        count != 0 &&
        itemSize >
            (SIZE_MAX / count)
    )
    {
        if (ready_)
        {
            noteFailure();
        }

        return nullptr;
    }

    const size_t bytes =
        count * itemSize;

    void* pointer =
        allocate(
            bytes,
            purpose,
            alignment,
            tag
        );

    if (pointer)
    {
        memset(
            pointer,
            0,
            bytes
        );
    }

    return pointer;
}

void* MemoryManager::reallocate(
    void* pointer,
    size_t newBytes,
    MemoryPurpose purpose,
    size_t alignment,
    const char* tag
)
{
    if (!pointer)
    {
        return allocate(
            newBytes,
            purpose,
            alignment,
            tag
        );
    }

    if (newBytes == 0)
    {
        release(pointer);
        return nullptr;
    }

    if (!ready_)
    {
        return nullptr;
    }

    size_t oldBytes = 0;
    MemoryPurpose oldPurpose =
        MemoryPurpose::General;

    char oldTag[TAG_LENGTH] = {};

    lock();

    for (
        size_t i = 0;
        i < MAX_TRACKED_ALLOCATIONS;
        ++i
    )
    {
        const AllocationRecord& record =
            records_[i];

        if (
            record.used &&
            record.pointer == pointer
        )
        {
            oldBytes = record.bytes;
            oldPurpose = record.purpose;

            strncpy(
                oldTag,
                record.tag,
                TAG_LENGTH - 1
            );

            break;
        }
    }

    unlock();

    if (oldBytes == 0)
    {
        noteFailure();
        return nullptr;
    }

    if (
        purpose == MemoryPurpose::General &&
        oldPurpose != MemoryPurpose::General
    )
    {
        purpose = oldPurpose;
    }

    const char* effectiveTag =
        tag ? tag : oldTag;

    void* replacement =
        allocate(
            newBytes,
            purpose,
            alignment,
            effectiveTag
        );

    if (!replacement)
    {
        return nullptr;
    }

    memcpy(
        replacement,
        pointer,
        (
            oldBytes < newBytes
        )
            ? oldBytes
            : newBytes
    );

    release(pointer);

    return replacement;
}

uint16_t* MemoryManager::allocateFramebuffer(
    uint16_t width,
    uint16_t height,
    const char* tag
)
{
    if (
        width == 0 ||
        height == 0
    )
    {
        if (ready_)
        {
            noteFailure();
        }

        return nullptr;
    }

    const size_t pixels =
        static_cast<size_t>(width) *
        static_cast<size_t>(height);

    if (
        pixels >
        (SIZE_MAX / sizeof(uint16_t))
    )
    {
        if (ready_)
        {
            noteFailure();
        }

        return nullptr;
    }

    if (
        config_.requirePSRAMForFramebuffer &&
        !psram_available_
    )
    {
        noteFailure();
        return nullptr;
    }

    const MemoryRegion region =
        config_.requirePSRAMForFramebuffer
            ? MemoryRegion::PSRAM
            : chooseRegion(
                  pixels * sizeof(uint16_t),
                  MemoryPurpose::Framebuffer
              );

    return static_cast<uint16_t*>(
        allocateTracked(
            pixels * sizeof(uint16_t),
            MemoryPurpose::Framebuffer,
            region,
            64,
            tag,
            config_.requirePSRAMForFramebuffer
                ? false
                : config_.allowFallback
        )
    );
}

uint16_t* MemoryManager::allocateRGB565(
    size_t pixelCount,
    const char* tag
)
{
    if (
        pixelCount >
        (SIZE_MAX / sizeof(uint16_t))
    )
    {
        if (ready_)
        {
            noteFailure();
        }

        return nullptr;
    }

    return static_cast<uint16_t*>(
        allocate(
            pixelCount *
                sizeof(uint16_t),
            MemoryPurpose::Bitmap,
            4,
            tag
        )
    );
}

void* MemoryManager::allocateDMA(
    size_t bytes,
    size_t alignment,
    const char* tag
)
{
    return allocateTracked(
        bytes,
        MemoryPurpose::DMA,
        MemoryRegion::DMA,
        alignment,
        tag,
        false
    );
}

void* MemoryManager::allocateControl(
    size_t bytes,
    size_t alignment,
    const char* tag
)
{
    return allocateTracked(
        bytes,
        MemoryPurpose::Control,
        MemoryRegion::Internal,
        alignment,
        tag,
        false
    );
}

void MemoryManager::release(
    void* pointer
)
{
    if (
        !ready_ ||
        !pointer
    )
    {
        return;
    }

    bool found = false;

    lock();

    for (
        size_t i = 0;
        i < MAX_TRACKED_ALLOCATIONS;
        ++i
    )
    {
        AllocationRecord& record =
            records_[i];

        if (
            record.used &&
            record.pointer == pointer
        )
        {
            if (
                tracked_bytes_ >=
                record.bytes
            )
            {
                tracked_bytes_ -=
                    record.bytes;
            }
            else
            {
                tracked_bytes_ = 0;
            }

            if (
                active_allocations_ > 0
            )
            {
                --active_allocations_;
            }

            record.pointer = nullptr;
            record.bytes = 0;
            record.alignment = 0;
            record.region =
                MemoryRegion::Auto;
            record.purpose =
                MemoryPurpose::General;
            record.used = false;
            record.tag[0] = '\0';

            found = true;
            break;
        }
    }

    unlock();

    // Never free a pointer that is not owned by this manager.
    if (found)
    {
        rawFree(pointer);
        markHeapStatsDirty();
    }
}

size_t MemoryManager::releaseTag(
    const char* tag
)
{
    if (
        !ready_ ||
        !tag ||
        tag[0] == '\0'
    )
    {
        return 0;
    }

    char normalizedTag[TAG_LENGTH] = {};

    strncpy(
        normalizedTag,
        tag,
        TAG_LENGTH - 1
    );

    normalizedTag[
        TAG_LENGTH - 1
    ] = '\0';

    size_t released = 0;

    while (true)
    {
        void* target = nullptr;

        lock();

        for (
            size_t i = 0;
            i < MAX_TRACKED_ALLOCATIONS;
            ++i
        )
        {
            const AllocationRecord& record =
                records_[i];

            if (
                record.used &&
                strcmp(
                    record.tag,
                    normalizedTag
                ) == 0
            )
            {
                target =
                    record.pointer;
                break;
            }
        }

        unlock();

        if (!target)
        {
            break;
        }

        release(target);
        ++released;
    }

    return released;
}

void MemoryManager::releaseAll()
{
    if (!ready_)
    {
        return;
    }

    while (true)
    {
        void* target = nullptr;

        lock();

        for (
            size_t i = 0;
            i < MAX_TRACKED_ALLOCATIONS;
            ++i
        )
        {
            if (records_[i].used)
            {
                target =
                    records_[i].pointer;
                break;
            }
        }

        unlock();

        if (!target)
        {
            break;
        }

        release(target);
    }
}

bool MemoryManager::owns(
    const void* pointer
) const
{
    if (
        !ready_ ||
        !pointer
    )
    {
        return false;
    }

    bool found = false;

    lock();

    for (
        size_t i = 0;
        i < MAX_TRACKED_ALLOCATIONS;
        ++i
    )
    {
        if (
            records_[i].used &&
            records_[i].pointer ==
                pointer
        )
        {
            found = true;
            break;
        }
    }

    unlock();

    return found;
}

size_t MemoryManager::allocationSize(
    const void* pointer
) const
{
    if (
        !ready_ ||
        !pointer
    )
    {
        return 0;
    }

    size_t bytes = 0;

    lock();

    for (
        size_t i = 0;
        i < MAX_TRACKED_ALLOCATIONS;
        ++i
    )
    {
        if (
            records_[i].used &&
            records_[i].pointer ==
                pointer
        )
        {
            bytes =
                records_[i].bytes;
            break;
        }
    }

    unlock();

    return bytes;
}

MemoryRegion MemoryManager::allocationRegion(
    const void* pointer
) const
{
    if (
        !ready_ ||
        !pointer
    )
    {
        return MemoryRegion::Auto;
    }

    MemoryRegion region =
        MemoryRegion::Auto;

    lock();

    for (
        size_t i = 0;
        i < MAX_TRACKED_ALLOCATIONS;
        ++i
    )
    {
        if (
            records_[i].used &&
            records_[i].pointer ==
                pointer
        )
        {
            region =
                records_[i].region;
            break;
        }
    }

    unlock();

    return region;
}

MemoryRegion MemoryManager::preferredRegion(
    size_t bytes,
    MemoryPurpose purpose
) const
{
    if (!ready_)
    {
        return MemoryRegion::Auto;
    }

    if (
        purpose ==
            MemoryPurpose::Framebuffer &&
        config_.requirePSRAMForFramebuffer
    )
    {
        return
            psram_available_
                ? MemoryRegion::PSRAM
                : MemoryRegion::Auto;
    }

    return
        chooseRegion(
            bytes,
            purpose
        );
}

HeapStats MemoryManager::regionStats(
    MemoryRegion region
) const
{
    switch (region)
    {
        case MemoryRegion::Internal:
            return
                readHeap(
                    MALLOC_CAP_INTERNAL |
                    MALLOC_CAP_8BIT
                );

        case MemoryRegion::PSRAM:
            if (!psram_available_)
            {
                return HeapStats();
            }

            return
                readHeap(
                    MALLOC_CAP_SPIRAM |
                    MALLOC_CAP_8BIT
                );

        case MemoryRegion::DMA:
            return
                readHeap(
                    MALLOC_CAP_DMA |
                    MALLOC_CAP_INTERNAL |
                    MALLOC_CAP_8BIT
                );

        case MemoryRegion::Auto:
        default:
            return HeapStats();
    }
}

bool MemoryManager::validate() const
{
    if (!ready_)
    {
        return false;
    }

    size_t bytes = 0;
    size_t count = 0;
    bool valid = true;

    lock();

    for (
        size_t i = 0;
        i < MAX_TRACKED_ALLOCATIONS;
        ++i
    )
    {
        const AllocationRecord& record =
            records_[i];

        if (!record.used)
        {
            continue;
        }

        if (
            !record.pointer ||
            record.bytes == 0 ||
            record.alignment == 0 ||
            record.region ==
                MemoryRegion::Auto
        )
        {
            valid = false;
            break;
        }

        if (
            record.bytes >
            SIZE_MAX - bytes
        )
        {
            valid = false;
            break;
        }

        bytes +=
            record.bytes;

        ++count;
    }

    if (
        valid &&
        (
            bytes !=
                tracked_bytes_ ||
            count !=
                active_allocations_ ||
            scratch_offset_ >
                scratch_capacity_ ||
            (
                scratch_capacity_ != 0 &&
                !scratch_base_
            )
        )
    )
    {
        valid = false;
    }

    unlock();

    return valid;
}

void MemoryManager::beginFrame()
{
    resetScratch();
}

void* MemoryManager::scratch(
    size_t bytes,
    size_t alignment
)
{
    if (
        !ready_ ||
        !scratch_base_ ||
        bytes == 0
    )
    {
        if (
            ready_ &&
            bytes != 0
        )
        {
            noteFailure();
        }

        return nullptr;
    }

    alignment =
        normalizeAlignment(
            alignment
        );

    if (alignment == 0)
    {
        noteFailure();
        return nullptr;
    }

    lock();

    const uintptr_t base =
        reinterpret_cast<uintptr_t>(
            scratch_base_
        );

    const uintptr_t current =
        base + scratch_offset_;

    if (
        current >
        UINTPTR_MAX -
            (alignment - 1)
    )
    {
        ++failed_allocations_;
        unlock();
        return nullptr;
    }

    const uintptr_t aligned =
        (
            current +
            alignment - 1
        ) &
        ~static_cast<uintptr_t>(
            alignment - 1
        );

    const size_t startOffset =
        static_cast<size_t>(
            aligned - base
        );

    if (
        startOffset >
            scratch_capacity_ ||
        bytes >
            (
                scratch_capacity_ -
                startOffset
            )
    )
    {
        ++failed_allocations_;
        unlock();
        return nullptr;
    }

    scratch_offset_ =
        startOffset + bytes;

    if (
        scratch_offset_ >
        scratch_peak_
    )
    {
        scratch_peak_ =
            scratch_offset_;
    }

    void* result =
        reinterpret_cast<void*>(
            aligned
        );

    unlock();

    return result;
}

void MemoryManager::resetScratch()
{
    if (!ready_)
    {
        return;
    }

    lock();
    scratch_offset_ = 0;
    unlock();
}

size_t MemoryManager::scratchMark() const
{
    return scratchUsed();
}

bool MemoryManager::rewindScratch(
    size_t mark
)
{
    if (!ready_)
    {
        return false;
    }

    lock();

    if (mark > scratch_offset_)
    {
        unlock();
        return false;
    }

    scratch_offset_ =
        mark;

    unlock();

    return true;
}

size_t MemoryManager::scratchCapacity() const
{
    lock();
    const size_t value =
        scratch_capacity_;
    unlock();

    return value;
}

size_t MemoryManager::scratchUsed() const
{
    lock();
    const size_t value =
        scratch_offset_;
    unlock();

    return value;
}

HeapStats MemoryManager::readHeap(
    uint32_t caps
) const
{
    HeapStats stats;

    stats.totalBytes =
        heap_caps_get_total_size(
            caps
        );

    stats.freeBytes =
        heap_caps_get_free_size(
            caps
        );

    stats.minimumFreeBytes =
        heap_caps_get_minimum_free_size(
            caps
        );

    stats.largestFreeBlock =
        heap_caps_get_largest_free_block(
            caps
        );

    return stats;
}

MemoryStats MemoryManager::sampleStats() const
{
    MemoryStats stats;

    stats.internal =
        readHeap(
            MALLOC_CAP_INTERNAL |
            MALLOC_CAP_8BIT
        );

    stats.psramAvailable =
        psram_available_;

    if (psram_available_)
    {
        stats.psram =
            readHeap(
                MALLOC_CAP_SPIRAM |
                MALLOC_CAP_8BIT
            );
    }

    lock();

    stats.trackedBytes =
        tracked_bytes_;

    stats.peakTrackedBytes =
        peak_tracked_bytes_;

    stats.activeAllocations =
        active_allocations_;

    stats.scratchCapacity =
        scratch_capacity_;

    stats.scratchUsed =
        scratch_offset_;

    stats.scratchPeakUsed =
        scratch_peak_;

    stats.successfulAllocations =
        successful_allocations_;

    stats.failedAllocations =
        failed_allocations_;

    unlock();

    stats.pressure =
        evaluatePressure(stats);

    return stats;
}

MemoryStats MemoryManager::getStats() const
{
    // Explicit diagnostics remain fresh by design. The presentation hot path
    // uses cachedStats()/pressure() and service-driven refreshes instead.
    return sampleStats();
}

MemoryStats MemoryManager::cachedStats() const
{
    lock();

    const bool valid =
        cached_stats_valid_;

    const MemoryStats cached =
        cached_stats_;

    unlock();

    if (valid)
    {
        return cached;
    }

    return sampleStats();
}

MemoryPressure MemoryManager::pressure() const
{
    return
        cachedStats().pressure;
}

void MemoryManager::markHeapStatsDirty()
{
    if (!ready_)
    {
        return;
    }

    lock();

    ++heap_revision_;

    if (heap_revision_ == 0)
    {
        heap_revision_ = 1;
        sampled_heap_revision_ = 0;
    }

    unlock();
}

bool MemoryManager::refreshStats(
    bool force
)
{
    if (!ready_)
    {
        return false;
    }

    const uint32_t now =
        millis();

    uint32_t revisionBefore = 0;
    uint32_t sampledRevision = 0;
    bool valid = false;

    lock();

    revisionBefore =
        heap_revision_;

    sampledRevision =
        sampled_heap_revision_;

    valid =
        cached_stats_valid_;

    const uint32_t age =
        static_cast<uint32_t>(
            now -
            last_monitor_ms_
        );

    const bool intervalDue =
        config_.monitorIntervalMs == 0 ||
        age >=
            config_.monitorIntervalMs;

    const bool dirty =
        revisionBefore !=
            sampledRevision;

    const bool shouldSample =
        force ||
        !valid ||
        dirty ||
        intervalDue;

    unlock();

    if (!shouldSample)
    {
        return false;
    }

    const MemoryStats fresh =
        sampleStats();

    PressureCallback callback =
        nullptr;

    bool pressureChanged = false;

    lock();

    // If an allocation/release raced with the heap query, preserve the dirty
    // revision so the next service pass samples again.
    const uint32_t revisionAfter =
        heap_revision_;

    cached_stats_ =
        fresh;

    cached_stats_valid_ = true;
    sampled_heap_revision_ =
        revisionBefore;

    ++heap_sample_count_;
    last_monitor_ms_ =
        now;

    pressureChanged =
        fresh.pressure !=
            last_pressure_;

    if (pressureChanged)
    {
        last_pressure_ =
            fresh.pressure;

        callback =
            pressure_callback_;
    }

    // revisionAfter intentionally is not copied into sampled_heap_revision_
    // unless it matches the state that was actually sampled.
    (void)revisionAfter;

    unlock();

    if (
        pressureChanged &&
        callback
    )
    {
        callback(
            fresh.pressure,
            fresh
        );
    }

    return true;
}

uint32_t MemoryManager::heapSampleCount() const
{
    lock();

    const uint32_t value =
        heap_sample_count_;

    unlock();

    return value;
}

MemoryPressure MemoryManager::evaluatePressure(
    const MemoryStats& stats
) const
{
    if (
        stats.internal.freeBytes <=
            config_.criticalInternalFreeBytes ||
        (
            stats.psramAvailable &&
            stats.psram.freeBytes <=
                config_.criticalPSRAMFreeBytes
        )
    )
    {
        return MemoryPressure::Critical;
    }

    if (
        stats.internal.freeBytes <=
            config_.warningInternalFreeBytes ||
        (
            stats.psramAvailable &&
            stats.psram.freeBytes <=
                config_.warningPSRAMFreeBytes
        )
    )
    {
        return MemoryPressure::Warning;
    }

    return MemoryPressure::Normal;
}

void MemoryManager::setPressureCallback(
    PressureCallback callback
)
{
    pressure_callback_ =
        callback;
}

void MemoryManager::service()
{
    if (!ready_)
    {
        return;
    }

    refreshStats(false);
}

const char* MemoryManager::regionName(
    MemoryRegion region
)
{
    switch (region)
    {
        case MemoryRegion::Internal:
            return "INTERNAL";

        case MemoryRegion::PSRAM:
            return "PSRAM";

        case MemoryRegion::DMA:
            return "DMA";

        case MemoryRegion::Auto:
        default:
            return "AUTO";
    }
}

const char* MemoryManager::purposeName(
    MemoryPurpose purpose
)
{
    switch (purpose)
    {
        case MemoryPurpose::UIObject:
            return "UI";

        case MemoryPurpose::Framebuffer:
            return "FRAMEBUFFER";

        case MemoryPurpose::Bitmap:
            return "BITMAP";

        case MemoryPurpose::Scratch:
            return "SCRATCH";

        case MemoryPurpose::DMA:
            return "DMA";

        case MemoryPurpose::Control:
            return "CONTROL";

        case MemoryPurpose::General:
        default:
            return "GENERAL";
    }
}

void MemoryManager::dump(
    Stream& output
) const
{
    const MemoryStats stats =
        getStats();

    output.println();
    output.println(
        "=== NV3047 Memory Manager ==="
    );

    output.print("PSRAM: ");
    output.println(
        stats.psramAvailable
            ? "available"
            : "not available"
    );

    output.print(
        "Internal free: "
    );
    output.println(
        stats.internal.freeBytes
    );

    output.print(
        "Internal largest block: "
    );
    output.println(
        stats.internal.largestFreeBlock
    );

    if (stats.psramAvailable)
    {
        output.print(
            "PSRAM free: "
        );
        output.println(
            stats.psram.freeBytes
        );

        output.print(
            "PSRAM largest block: "
        );
        output.println(
            stats.psram.largestFreeBlock
        );
    }

    output.print(
        "Tracked bytes: "
    );
    output.println(
        stats.trackedBytes
    );

    output.print(
        "Peak tracked bytes: "
    );
    output.println(
        stats.peakTrackedBytes
    );

    output.print(
        "Active allocations: "
    );
    output.println(
        stats.activeAllocations
    );

    output.print(
        "Scratch: "
    );
    output.print(
        stats.scratchUsed
    );
    output.print('/');
    output.println(
        stats.scratchCapacity
    );

    output.print(
        "Allocation success/fail: "
    );
    output.print(
        stats.successfulAllocations
    );
    output.print('/');
    output.println(
        stats.failedAllocations
    );

    output.print(
        "Pressure: "
    );

    switch (stats.pressure)
    {
        case MemoryPressure::Critical:
            output.println("CRITICAL");
            break;

        case MemoryPressure::Warning:
            output.println("WARNING");
            break;

        case MemoryPressure::Normal:
        default:
            output.println("NORMAL");
            break;
    }

    if (!config_.enableTracking)
    {
        output.println(
            "Detailed allocation tags disabled."
        );
        return;
    }

    output.println(
        "-- Managed allocations --"
    );

    for (
        size_t i = 0;
        i < MAX_TRACKED_ALLOCATIONS;
        ++i
    )
    {
        AllocationRecord snapshot = {};

        lock();

        if (records_[i].used)
        {
            snapshot =
                records_[i];
        }

        unlock();

        if (!snapshot.used)
        {
            continue;
        }

        output.print('#');
        output.print(i);
        output.print(' ');
        output.print(
            snapshot.bytes
        );
        output.print(" B ");
        output.print(
            regionName(
                snapshot.region
            )
        );
        output.print(' ');
        output.print(
            purposeName(
                snapshot.purpose
            )
        );

        if (
            snapshot.tag[0] !=
            '\0'
        )
        {
            output.print(" tag=");
            output.print(
                snapshot.tag
            );
        }

        output.println();
    }
}

} // namespace NV3047Memory
