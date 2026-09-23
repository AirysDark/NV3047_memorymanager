#pragma once

#include <stddef.h>
#include <stdint.h>

namespace NV3047Memory
{
namespace Defaults
{

// ============================================================
// BASE ALLOCATION POLICY
// ============================================================
static constexpr size_t LARGE_ALLOCATION_THRESHOLD =
    4096;

static constexpr size_t INTERNAL_RESERVE_BYTES =
    48 * 1024;

static constexpr size_t PSRAM_RESERVE_BYTES =
    128 * 1024;

static constexpr size_t WARNING_INTERNAL_FREE_BYTES =
    64 * 1024;

static constexpr size_t CRITICAL_INTERNAL_FREE_BYTES =
    32 * 1024;

static constexpr size_t WARNING_PSRAM_FREE_BYTES =
    256 * 1024;

static constexpr size_t CRITICAL_PSRAM_FREE_BYTES =
    128 * 1024;

static constexpr size_t SCRATCH_BYTES =
    64 * 1024;

static constexpr bool PREFER_PSRAM = true;
static constexpr bool ALLOW_FALLBACK = true;
static constexpr bool ALLOW_BITMAP_FALLBACK = false;
static constexpr bool ALLOW_SCRATCH_FALLBACK = false;
static constexpr bool REQUIRE_PSRAM_FRAMEBUFFER = true;
static constexpr bool ENABLE_TRACKING_DUMP = true;

// Heap/pressure sampling is intentionally slower than display cadence.
// Dirty heap state still triggers an immediate sample on the next service.
static constexpr uint32_t MEMORY_MONITOR_INTERVAL_MS =
    1000;

// ============================================================
// ADAPTIVE BROKER
// ============================================================
static constexpr bool BROKER_ENABLED = true;

static constexpr size_t BROKER_PERMANENT_ARENA_BYTES =
    0;

static constexpr size_t BROKER_PERMANENT_HEADROOM_BYTES =
    2 * 1024;

static constexpr uint32_t BROKER_SERVICE_INTERVAL_MS =
    500;

static constexpr size_t BROKER_MINIMUM_RECLAIM_BYTES =
    4 * 1024;

static constexpr uint8_t BROKER_WARNING_RECLAIM_PERCENT =
    25;

static constexpr uint8_t BROKER_CRITICAL_RECLAIM_PERCENT =
    100;

// ============================================================
// NV3047 AUTOMATIC TAKEOVER
// ============================================================
static constexpr size_t UI_SOFT_BUDGET_BYTES =
    512 * 1024;

static constexpr size_t APPLICATION_SOFT_BUDGET_BYTES =
    512 * 1024;

static constexpr uint16_t FRAMEBUFFER_WIDTH =
    480;

static constexpr uint16_t FRAMEBUFFER_HEIGHT =
    272;

static constexpr bool ALLOCATE_FRAMEBUFFER_PAIR =
    true;

static constexpr bool ENABLE_DMA_POOL =
    true;

static constexpr size_t DMA_BLOCK_BYTES =
    480 * 10 * sizeof(uint16_t);

static constexpr uint8_t DMA_BLOCK_COUNT =
    1;

static constexpr size_t DMA_ALIGNMENT =
    4;

static constexpr size_t ASSET_CACHE_BUDGET_BYTES =
    512 * 1024;

static constexpr uint8_t WARNING_CACHE_PERCENT =
    75;

static constexpr bool PURGE_UNPINNED_ON_CRITICAL =
    true;

static constexpr bool RESET_SCRATCH_ON_CRITICAL =
    true;

// Profiling adds micros() calls to frame/service paths, so production defaults
// keep it disabled. It can be enabled at runtime for hardware benchmarking.
static constexpr bool ENABLE_PERFORMANCE_PROFILING =
    false;

} // namespace Defaults
} // namespace NV3047Memory
