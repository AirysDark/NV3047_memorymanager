# NV3047 Memory Takeover Plan

This document defines the handoff from the active NV3047 overhaul branches into `NV3047_memorymanager`. The driver-side framebuffer/DMA takeover described below is now implemented through the optional provider ABI.

## Reference branches

Use these branches as the source of truth when designing integration:

- `NV3047_drivers:driver_overhaul_v2`
- `NV3047_UI:ui-overhaul-v2`

Do not base memory integration decisions on the older default-branch code when these overhaul branches differ.

## Current overhaul state

### Driver

`driver_overhaul_v2` already contains:

- `Core_Matrices/MemoryManager.h/.cpp`
- a framebuffer class that owns that local manager
- configurable framebuffer count, size, alignment and allocation caps in `Config::MemoryManager`
- explicit framebuffer diagnostics exposed through the high-level driver
- two 480 x 272 RGB565 buffers by default
- 64-byte framebuffer alignment

The future takeover should therefore replace the driver's **local framebuffer MemoryManager implementation**, not bolt a second framebuffer allocator beside it.

### UI

`ui-overhaul-v2` already uses deterministic fixed-size widget storage:

```cpp
static const uint8_t MAX_WIDGETS = 40;
WidgetSlot widgets[MAX_WIDGETS];
```

Therefore the framework's screen registration path no longer needs an object-pool conversion merely to eliminate per-widget node heap allocation.

`ObjectPool<T, Capacity>` remains useful for application-owned dynamic UI objects, dialogs, page objects, model objects or future components that genuinely need pooled lifetime.

The UI also already exposes driver framebuffer/memory diagnostics through `UIDriverStats`.

## Adaptive broker contract

The external manager now contains a workload-aware `MemoryBroker`.

The takeover should expose activity and reclaimability, not fixed memory partitions.

Built-in broker clients are:

- driver-fixed — Critical, permanent framebuffer/DMA ownership
- ui — Normal priority with activity decay
- assets — Normal priority and linked to UI activity
- application — Normal priority for non-UI work

The UI and application may temporarily exceed their soft budgets when memory is available. Under pressure or a higher-importance request, lower-importance idle/background clients are asked to reclaim memory.

Reclamation is region-aware:

- PSRAM requests reclaim PSRAM-backed donors
- DMA requests reclaim DMA-capable donors
- Internal requests may use Internal or DMA-capable internal donors
- generic Auto requests may use any eligible donor

Large failed allocations also inspect the target region's largest contiguous free block and retry after bounded reclaim passes. This lets the broker respond to fragmentation instead of relying only on total free bytes.

`ElasticBuffer<T>` provides automatically reclaimable working memory with safe invalidation and recreation.

## Permanent broker memory

On ESP32-S3 / Arduino Core 2.0.17 the current broker metadata layout is compile-time locked to:

- 16 client records × 76 bytes = 1,216 bytes
- 128 lease records × 28 bytes = 3,584 bytes
- exact required arena = **4,800 bytes**
- requested headroom = **2,048 bytes**
- rounded permanent control arena = **7,168 bytes**
- remaining control headroom = **2,368 bytes**

The arena is internal-RAM-only `MemoryPurpose::Control` storage.

## Rule

The driver and UI may request memory.

They should not independently decide:

- Internal RAM versus PSRAM
- reserve thresholds
- cache eviction
- scratch lifetime
- DMA reuse policy
- framebuffer lifetime

Those decisions belong in `NV3047_memorymanager`.

## Driver takeover order — implemented

### 1. Startup

`NV3047_Memory.h` is the complete normal sketch integration. It installs the automatic runtime and provider registrar before Arduino `setup()`.

The runtime starts lightweight manager/broker mode without allocating a duplicate framebuffer or DMA pool. When the driver initializes, the provider upgrades that lightweight instance into the full framebuffer/DMA configuration automatically.

No sketch-side `AutoMemory::begin()`, `beginFrame()`, `service()` or provider-registration call is required.

### 2. Replace local driver MemoryManager — implemented

The driver overhaul currently owns its own class:

```text
Core_Matrices/MemoryManager
    -> allocates Config::MemoryManager::BUFFER_COUNT
    -> exposes front/draw buffers
    -> swaps buffer indexes
```

The adapter preserves that external behavior while moving actual allocation ownership to:

- `NV3047Memory::AutoMemory`
- `NV3047Memory::FramebufferPair`

The driver-facing API can remain stable while its allocator becomes a thin adapter.

### 3. Framebuffers

Bind the driver framebuffer engine to:

- `AutoMemory::framebuffers().front()`
- `AutoMemory::framebuffers().back()`

After a successful physical display presentation:

- call `FramebufferPair::swapRoles()`

No framebuffer malloc/free should occur during normal rendering.

### 4. Config handoff

The values currently defined under `Config::MemoryManager` should map cleanly into the external memory-manager configuration:

- buffer count
- framebuffer size
- alignment
- allocation preference
- zero-on-init behavior

The first external integration target should preserve the current production defaults:

- buffer count: 2
- frame size: 480 x 272 x 2 bytes
- alignment: 64 bytes
- preferred region: PSRAM
- framebuffer PSRAM requirement: enabled
- bitmap fallback to internal RAM: disabled by default
- scratch fallback to internal RAM: disabled by default
- zero on startup: enabled

### 5. Persistent DMA screen-fill memory

`driver_overhaul_v2` currently allocates one 9,600-byte DMA-capable fill buffer lazily on first use and retains it until `DisplayDriver` destruction.

The integration goal is to transfer ownership of that persistent working buffer to this library. One compatible path is:

```cpp
void* block =
    AutoMemory::instance().
        dmaPool().acquire();
```

The driver can retain the borrowed block for its lifetime and return it during shutdown:

```cpp
AutoMemory::instance().
    dmaPool().release(block);
```

The default pool now contains one 9,600-byte block, matching the current overhaul driver behavior.

### 6. Driver scratch work

Short-lived CPU-only work buffers should use:

```cpp
AutoMemory::instance().
    memory().scratch(...);
```

They must not survive the next `beginFrame()`.

## UI takeover order

### 1. Keep fixed widget slots

The overhaul UI already avoids per-widget heap churn with `Screen::MAX_WIDGETS = 40` and an inline `WidgetSlot widgets[MAX_WIDGETS]` table.

Do not replace this table just for the sake of routing everything through the memory-manager library.

Deterministic member storage is already the correct embedded design here.

### 2. UI activity

The sketch must not report UI activity manually.

UI activity should be generated internally by the UI/memory integration from genuine workload signals such as:

- successful touch/input handling
- active elastic-buffer access
- asset-cache use
- dynamic layout/render working-set use

Driver frame swaps are only frame-lifetime boundaries and are **not** treated as proof that the UI is heavily used.

The broker automatically decays inactive UI clients through Background to Idle.

### 3. Assets

Icons, RGB565 bitmaps and decoded graphical data should enter `AssetCache`.

Frequently required UI assets may be pinned.

Optional page-specific assets should remain unpinned so automatic pressure recovery can evict them.

Asset-client activity follows UI activity. Active UI assets therefore are not treated as low-value donor memory merely because they are cache entries.

### 4. Elastic UI work memory

Large disposable page/layout/decoded working sets should use `ElasticBuffer<T>` or `MemoryBroker::requestElastic()`.

When the UI is idle and another active workload needs memory, the broker may reclaim these allocations. Their handles are invalidated safely and can be recreated with `ensure()`.

### 5. Layout/render scratch

Temporary arrays, transformed coordinates, generated scanlines and other frame-local data should use the scratch arena.

Nested operations can checkpoint and rewind within the same frame:

```cpp
size_t mark =
    AutoMemory::instance().
        memory().scratchMark();

// allocate temporary scratch...

AutoMemory::instance().
    memory().rewindScratch(mark);
```

### 6. Optional dynamic UI objects

Use `ObjectPool<T, Capacity>` only where the UI/application actually creates and destroys objects dynamically.

Good candidates include:

- runtime-created pages
- reusable dialog instances
- transient model/view objects
- future dynamically composed widgets

### 7. Diagnostics

The overhaul UI already expects driver diagnostics including:

- framebuffer count
- framebuffer size
- total framebuffer bytes
- free managed memory
- largest free managed block

When the external manager takes control, preserve these diagnostic semantics so `UIDriverStats` continues working without UI redesign.

## Render-loop target

The sketch should not contain memory-manager lifecycle code.

Normal sketch:

```cpp
#include <NV3047_Memory.h>
```

Internally:

```text
umbrella include
    -> automatic runtime starts manager/broker service

driver successful frame
    -> provider beginFrame()
    -> scratch lifetime reset

background runtime
    -> periodic service()
    -> pressure monitoring
    -> activity decay
    -> reclaim/fragmentation maintenance

driver presentation
    -> FramebufferPair role swap
```

The driver remains responsible for presentation cadence and physical display submission. The memory manager owns storage and background memory policy.

Application subsystems that can discard/rebuild working data should register a broker reclaimer or use elastic leases. This is what allows memory to move back from an idle non-UI workload to an active UI workload as well as in the opposite direction.

The adapter must not silently fall back to internal RAM for framebuffer storage. If the required PSRAM framebuffer allocation cannot be satisfied, initialization should fail cleanly rather than starving the ESP32 runtime.

## Pressure rules

### Normal

No automatic cache destruction.

### Warning

Reduce optional asset-cache footprint.

### Critical

Purge unpinned cache and recycle scratch.

Do not destroy:

- live framebuffers
- pinned assets
- live UI objects
- currently checked-out DMA blocks
- the UI's fixed WidgetSlot table

## Ownership rule

Every pointer has exactly one owner.

| Pointer | Owner |
|---|---|
| front framebuffer | FramebufferPair |
| back framebuffer | FramebufferPair |
| DMA pool block | DMAPool |
| cached icon | AssetCache |
| scratch pointer | MemoryManager scratch arena |
| elastic working buffer | MemoryBroker / ElasticBuffer |
| optional pooled object | ObjectPool |
| general managed allocation | MemoryManager / ManagedBuffer |

Consumers borrow these pointers. Consumers do not directly `free()` them.

## Integration validation

During future `driver_overhaul_v2` / `ui-overhaul-v2` integration, call:

```cpp
if (!AutoMemory::instance().validate())
{
    // Memory ownership/accounting inconsistency.
}
```

This checks the base manager, broker lease totals, duplicate broker pointers, asset-cache ownership, framebuffer readiness and DMA pool accounting.

The `AdaptiveStressTest` example should be run on the actual ESP32-S3 hardware before and after takeover changes. It alternates UI/application workload importance and reports PSRAM free bytes, largest contiguous block, elastic residency and validation state.

## Automatic runtime safety

The include-only runtime waits for Arduino's `loopTaskHandle` before starting `AutoMemory`. On Arduino ESP32 Core 2.0.17 that handle is assigned after `initArduino()`, and `initArduino()` performs PSRAM initialization.

The automatic service task uses a permanent **4096-byte** static stack. `automaticRuntimeStats()` reports current stack headroom and the combined permanent manager/runtime control footprint.

While the external NV3047 driver takeover is active, background full reclaim is suppressed so unpinned cache data cannot be evicted asynchronously in the middle of rendering. The driver invokes full service at successful swap boundaries. Broker activity is refreshed again inside `reclaimFor()`, so a static display does not leave stale workload importance when another subsystem requests memory.

### Driver re-initialization caveat

The current `driver_overhaul_v2` HAL keeps its optional lazy screen-fill DMA pointer as a `DisplayDriver` member. Normal high-level `NV3047_Driver::fillScreen()` uses framebuffer clear/present and does not use that HAL buffer.

If a future application explicitly uses the low-level HAL fill path and repeatedly tears down/re-initializes the same hardware object, the driver should reset or reacquire that cached HAL DMA pointer as part of re-initialization. This is a driver-side lifecycle hardening item; the memory-manager provider already rejects stale release requests after provider shutdown.

## Core compatibility

The memory system is developed and CI-compiled against Arduino ESP32 Core 2.0.17, matching both overhaul branches.
