# NV3047 Memory Manager

Automatic memory control for the NV3047 ESP32-S3 display stack.

This repository is the dedicated memory subsystem for:

- `NV3047_drivers`
- `NV3047_UI`

The memory policy and broker live in this repository. The active driver branch now contains the small optional provider adapter required for automatic takeover; the UI remains a reference consumer.

### Active reference branches

- `NV3047_drivers:driver_overhaul_v2`
- `NV3047_UI:ui-overhaul-v2`

These overhaul branches are the source of truth for integration decisions.

## Target

- ESP32-S3
- Arduino ESP32 Core **2.0.17**
- PSRAM-backed 480 x 272 RGB565 graphics
- deterministic embedded allocations
- low-fragmentation long-running UI applications

## Current version

**0.4.0**

The library has moved beyond a basic allocator and now provides an automatic ownership layer for the major memory classes used by the NV3047 stack.

## 0.4.0 region-aware recovery

The broker now understands **where** memory lives, not just how many bytes are reclaimable.

### Region-aware donor selection

Each broker lease records its actual allocation region. Observed/reclaimable client memory also records its region.

A failed request now targets only donors that can help that request:

- PSRAM request -> PSRAM donors
- DMA request -> DMA-capable donors
- Internal request -> Internal or DMA-capable internal donors
- Auto request -> any eligible donor

This prevents a large PSRAM request from evicting unrelated internal-RAM data that would not improve the allocation result.

### Fragmentation-aware allocation recovery

Allocation retry logic checks the target region's **largest contiguous free block**.

If total free PSRAM is healthy but the largest block is too small, the broker performs bounded region-specific reclaim passes and retries the allocation after each pass.

This directly addresses the case where enough total memory exists but is too fragmented to satisfy a large UI/application allocation.

### Runtime integrity validation

The library now exposes:

```cpp
memory.validate();
```

Validation covers:

- base allocation-record accounting
- active allocation byte/count totals
- scratch bounds
- broker client/lease accounting
- duplicate broker lease pointers
- broker permanent-arena bounds
- asset-cache pointer ownership and byte totals
- framebuffer readiness
- DMA-pool live block accounting

This is intended for hardware testing and future driver/UI integration.

### Scratch checkpoints

The frame scratch arena now supports same-frame checkpoints:

```cpp
size_t mark =
    memory.memory().scratchMark();

void* work =
    memory.memory().scratch(
        12 * 1024,
        16
    );

// temporary work...

memory.memory().rewindScratch(mark);
```

This allows nested render/layout operations to return temporary scratch capacity immediately instead of waiting for the next frame.

### Adaptive stress test

A new `AdaptiveStressTest` example continuously alternates:

- UI-heavy phase
- application-heavy phase
- both-active phase
- idle/recovery phase

It exercises:

- UI/application elastic handoff
- PSRAM-specific reclaim
- asset-cache donor behavior
- scratch checkpoint/rewind
- largest-free-block reporting
- continuous `validate()` checks

CI now compiles both the normal demo and this stress test against ESP32 Arduino Core 2.0.17.

## 0.4.0 automatic driver takeover

Adding the library to an NV3047 sketch now automatically hands framebuffer and driver-DMA ownership to `NV3047_memorymanager`.

The sketch only needs the normal public include:

```cpp
#include <NV3047_Memory.h>
#include <NV3047_Driver.h>
```

No explicit registration call and no mandatory `AutoMemory::begin()` call are required. `NV3047_Memory.h` installs a small startup registrar before Arduino `setup()`. When `NV3047_drivers:driver_overhaul_v2` starts, its local `Core_Matrices/MemoryManager` detects the registered provider and becomes a thin adapter over this library.

Takeover covers:

- front/back framebuffer ownership through `AutoMemory::framebuffers()`
- framebuffer role swaps
- framebuffer diagnostics
- per-frame `beginFrame()`
- adaptive `service()`
- the driver's persistent DMA fill buffer

When the provider is registered, the driver **does not silently fall back** to its internal allocator if external startup fails. This prevents two competing memory owners from being created.

Without `NV3047_Memory.h` in the sketch, `NV3047_drivers` behaves exactly as before and uses its own local framebuffer manager.

Runtime detection is available from the driver:

```cpp
display.isExternalMemoryManagerActive();
```

The bridge uses a versioned C-compatible provider ABI so neither repository needs a hard Arduino library dependency on the other. `NV3047_memorymanager` remains usable by itself.

## 0.3.0 adaptive broker

The memory manager now implements the workload-sharing behavior the project is aiming for.

### Permanent manager control memory

The broker does not guess a large fixed reservation. It calculates the exact compiled table requirement and adds a small permanent expansion margin.

For ESP32-S3 / Arduino Core 2.0.17 with the current layout:

- exact broker table requirement: **4,800 bytes**
- configured expansion margin: **2,048 bytes**
- final permanent broker arena after 1 KiB rounding: **7,168 bytes**
- effective spare control headroom: **2,368 bytes**

That arena is allocated from **internal RAM only** through `MemoryPurpose::Control`. It never falls back to PSRAM and is never offered to application workloads.

The existing `MemoryManager` and `AutoMemory` singleton objects are already permanent compile-time storage. `AutoMemory::staticControlBytes()` reports that static footprint on the actual target, while `totalPermanentControlBytes()` reports static control storage plus the broker arena.

The base manager mutex was also changed to static FreeRTOS storage, removing its previous hidden heap allocation.

### Adaptive workload sharing

The broker supports up to:

- **16 workload clients**
- **128 broker leases**

Each client has:

- priority
- current activity state
- automatic activity decay
- minimum retained memory
- soft budget
- optional hard limit
- observed memory usage
- reclaimable memory
- optional reclaim callback

Soft budgets are guidance, **not fixed partitions**. A workload can grow beyond its soft budget while memory is available.

When another workload needs memory, the broker prefers donors that are:

1. in a memory region that can actually satisfy the request
2. lower priority
3. less active
4. above their soft budget
5. holding more reclaimable memory

### UI-aware behavior

`AutoMemory::noteUIActivity()` marks both the UI and its asset cache active.

While the UI is active, its cached assets are protected at the same workload importance as the UI. When UI activity stops, both automatically decay through Background to Idle and become increasingly suitable donor memory.

This means an active application job can reclaim idle UI assets, while an active UI can retain/grow its working set instead of having assets treated as permanently low priority.

### Elastic buffers

`ElasticBuffer<T>` is explicitly reclaimable working memory.

The broker may automatically release it when its owner becomes a lower-importance donor. The handle is invalidated safely, so callers do not retain a dangling pointer. When the workload becomes active again, `ensure()` can recreate the buffer.

```cpp
ElasticBuffer<uint8_t> uiWork;

uiWork.begin(
    &memory.broker(),
    memory.uiClient(),
    96 * 1024,
    MemoryPurpose::General,
    "ui-work"
);

// Later, after another workload reclaimed it:
if (!uiWork.resident())
{
    uiWork.ensure();
}
```

### Existing hardening retained

- strict PSRAM framebuffer ownership
- bitmap/scratch protection from internal-RAM spill
- safe ownership bookkeeping
- LRU cache eviction
- replacement-safe cached assets
- stale-pointer guards
- fragmentation diagnostics
- fragmentation-aware allocation retries
- region-aware donor selection
- full integrity validation
- same-frame scratch rewind
- automatic Warning/Critical recovery

## Current NV3047 memory pressure

The reference driver currently uses two 480 x 272 RGB565 framebuffers:

- one framebuffer: **261,120 bytes**
- two framebuffers: **522,240 bytes**

In `driver_overhaul_v2`, the display HAL lazily allocates one persistent 10-line DMA-capable RGB565 fill buffer and keeps it until the display driver is destroyed:

- 480 x 10 x 2 bytes = **9,600 bytes**

The same driver branch already contains its own local `Core_Matrices/MemoryManager` that owns the framebuffer pair and exposes framebuffer diagnostics.

In `ui-overhaul-v2`, the screen manager already avoids per-widget heap allocation with a fixed table:

- `Screen::MAX_WIDGETS = 40`
- `WidgetSlot widgets[40]`

The external memory manager is therefore intended to replace the driver's local framebuffer allocation policy, own persistent DMA working memory, manage graphical assets and scratch memory, and provide optional pools only for genuinely dynamic application objects.

## Architecture

```text
                    NV3047 application
                           |
                    NV3047_AutoMemory
                           |
        +------------------+------------------+
        |                  |                  |
 FramebufferPair       AssetCache          DMAPool
        |                  |                  |
  PSRAM RGB565       PSRAM RGB565       Internal DMA
        |                  |                  |
        +------------------+------------------+
                           |
                    MemoryManager
                           |
          +----------------+----------------+
          |                |                |
      Internal RAM       PSRAM          DMA RAM
          |
      ObjectPool<T>
      UI/control nodes
```

## Main components

### `MemoryManager`

Core policy allocator.

Features:

- automatic Internal RAM / PSRAM / DMA selection
- allocation purpose classification
- configurable internal-RAM reserve
- configurable PSRAM reserve
- strict PSRAM framebuffer ownership by default
- bitmap/scratch fallback protection to preserve internal RAM
- 64-byte aligned framebuffer allocation
- RGB565 allocation helpers
- DMA-only allocation helpers
- allocation tagging
- grouped cleanup by tag
- always-on ownership bookkeeping for safe shutdown
- ownership checking
- tracked allocation size
- high-water statistics
- failed-allocation counters
- minimum-free-heap statistics
- largest-free-block statistics
- warning and critical memory-pressure states
- pressure callbacks
- reusable frame scratch arena

### `MemoryBroker`

Adaptive memory-sharing controller.

It tracks workload importance rather than assigning permanent fixed partitions. Requests can trigger reclamation from less-important idle/background clients before retrying.

The built-in `AutoMemory` setup registers:

- driver-fixed — Critical, non-reclaimable framebuffer/DMA ownership
- UI — elastic Normal-priority workload
- assets — UI-linked reclaimable cache
- application — elastic Normal-priority workload

### `AutoMemory`

High-level automatic controller.

It can start the complete memory system with one call and preallocate:

- a double framebuffer pair
- a reusable DMA block pool
- a PSRAM asset cache
- the base scratch arena

It also performs automatic pressure recovery from `service()`.

### `FramebufferPair`

Owns a complete double-buffer set.

Default NV3047 configuration:

```text
front: 480 x 272 RGB565
back : 480 x 272 RGB565
total: 522,240 bytes
```

Features:

- PSRAM-first allocation
- 64-byte alignment
- front/back role tracking
- role swap without reallocating
- fast 32-bit clear
- zeroed startup buffers

### `DMAPool`

Preallocates reusable DMA-capable internal-RAM blocks.

`driver_overhaul_v2` currently allocates its 9,600-byte fill buffer once on first use and keeps it until destruction. The takeover target is therefore ownership transfer rather than fixing per-frame allocation churn.

The default automatic configuration now creates **one 9,600-byte block**, matching that production behavior. Extra blocks can still be configured if future concurrent DMA work needs them.

### `AssetCache`

PSRAM-oriented cache for RGB565 images, icons and other graphical data.

Features:

- fixed metadata table
- replacement-safe cache updates
- oversized-entry rejection without destroying valid cache contents
- no STL containers
- configurable byte budget
- 32-bit asset keys
- cache hit/miss statistics
- pinned assets
- least-recently-used eviction
- automatic room creation
- emergency unpinned purge
- peak cache usage statistics

### `ObjectPool<T, Capacity>`

Fixed-capacity typed object pool for optional runtime-created application/UI objects. The overhaul UI's normal Screen widget registry already uses fixed member storage and does not need this pool.

Features:

- one stable backing allocation
- placement construction
- destructor-aware release
- exact `alignas(T)` storage
- no per-object heap allocation
- no per-object heap fragmentation

### Per-frame scratch arena

Temporary rendering memory is allocated linearly from a preallocated arena:

```cpp
automaticMemory.beginFrame();

void* temporary =
    automaticMemory.memory().scratch(
        4096,
        16
    );
```

At the beginning of the next frame, the entire temporary area is recycled in constant time.

No individual `free()` calls are required.

## Recommended automatic startup

```cpp
#include <NV3047_Memory.h>

using namespace NV3047Memory;

AutoMemory& memory =
    AutoMemory::instance();

void setup()
{
    AutoMemoryConfig config;

    config.framebufferWidth = 480;
    config.framebufferHeight = 272;

    config.memory.scratchBytes =
        64 * 1024;

    config.assetCacheBudgetBytes =
        512 * 1024;

    config.dmaBlockBytes =
        480 * 10 * sizeof(uint16_t);

    config.dmaBlockCount = 1;

    if (!memory.begin(config))
    {
        // Memory system could not reserve
        // the required resources.
        return;
    }
}
```

## Automatic placement policy

| Allocation | Preferred region |
|---|---|
| small general/control allocation | Internal RAM |
| large general allocation | PSRAM |
| UI object | Internal RAM |
| framebuffer | PSRAM |
| bitmap / RGB565 asset | PSRAM |
| frame scratch | PSRAM |
| DMA transfer memory | DMA-capable Internal RAM |

The manager protects configurable reserve amounts before approving allocations.

For the NV3047 defaults:

- framebuffers are required to live in PSRAM
- bitmap/cache allocations do not fall back into internal RAM when PSRAM is present but pressured
- the scratch arena does not fall back into internal RAM when PSRAM allocation fails
- DMA requests never fall back into memory that is not DMA capable

These policies are configurable in `MemoryConfig`, but the defaults are intentionally conservative for `driver_overhaul_v2`.

## Automatic pressure handling

`AutoMemory::service()` synchronizes real driver/cache usage into the broker, updates activity decay, and evaluates heap pressure.

### Normal

No forced reclamation. Workloads may grow past soft budgets while the protected heap reserves remain healthy.

### Warning

The broker reclaims a configurable portion of memory from the least-important eligible donors first. Idle/background clients and clients above their soft budget are preferred.

### Critical

The broker can reclaim all eligible optional memory needed to recover, including:

- unpinned asset-cache entries
- elastic UI/application working buffers
- memory exposed through client reclaim callbacks

It still preserves:

- framebuffer storage
- pinned assets
- non-elastic live allocations
- DMA pool ownership
- configured client minimum-retained floors

Scratch contents are reset on Critical entry because they are transient, but the scratch arena remains available for the next frame.

Pressure policy is evaluated at startup and during `service()`, so a system that starts already in Warning/Critical pressure receives recovery immediately.

## Fragmentation monitoring

The automatic controller reports fragmentation independently for:

- internal RAM
- PSRAM

The estimate is calculated from total free memory versus the largest contiguous free block.

```cpp
FragmentationStats frag =
    memory.fragmentation();

Serial.println(
    frag.internalPercent
);
```

This gives a direct indication of whether enough total memory exists but has become divided into unusable small blocks.

## RGB565 asset cache

```cpp
uint16_t* icon =
    memory.assets().putRGB565(
        0x1001,
        sourcePixels,
        pixelCount,
        false
    );

uint16_t* cached =
    static_cast<uint16_t*>(
        memory.assets().get(
            0x1001
        )
    );
```

Set the final argument to `true` to pin an asset so pressure recovery will not evict it.

## DMA pool

```cpp
void* dma =
    memory.dmaPool().acquire();

if (dma)
{
    // use DMA memory

    memory.dmaPool().release(
        dma
    );
}
```

The block remains allocated to the pool and is immediately reusable.

## Optional object pool

`ui-overhaul-v2` already uses a fixed 40-entry `WidgetSlot` table, so its normal screen registry should remain as-is.

`ObjectPool<T, Capacity>` is for objects that are genuinely created and destroyed at runtime, for example dynamic pages, reusable dialogs or model/view objects:

```cpp
struct RuntimeObject
{
    int value;
};

ObjectPool<RuntimeObject, 16> objects;

objects.begin(
    &memory.memory(),
    "runtime-objects"
);

RuntimeObject* object =
    objects.create();

objects.destroy(object);
```

## Driver takeover

`driver_overhaul_v2` already centralises its two framebuffers behind `Core_Matrices/MemoryManager`. The clean takeover is to keep the driver's public framebuffer behavior while replacing that local allocator with a thin adapter over this repository:

```text
driver Core_Matrices/MemoryManager
    -> NV3047_memorymanager AutoMemory / FramebufferPair

driver getFrontBuffer()
    -> FramebufferPair::front()

driver getDrawBuffer()
    -> FramebufferPair::back()

driver swapBuffers()
    -> FramebufferPair::swapRoles()

DisplayDriver persistent 9,600-byte DMA fill buffer
    -> managed DMA block ownership

driver framebuffer diagnostics
    -> external MemoryManager statistics
```

When this library is included in the sketch, there is one framebuffer owner: this library. The driver remains responsible for presentation timing and panel submission while this library owns storage policy and managed DMA memory.

## Future UI takeover

`ui-overhaul-v2` already has deterministic fixed storage for up to 40 registered widgets, so that part should not be replaced.

The useful integration points are:

```text
existing Screen::WidgetSlot[40]
    -> keep unchanged

temporary render/layout working memory
    -> frame scratch arena

runtime icons/images
    -> AssetCache

optional dynamically-created objects
    -> ObjectPool

large UI/application data
    -> MemoryManager automatic allocator

existing UIDriverStats memory fields
    -> preserve through driver adapter
```

This keeps the UI focused on rendering and layout while avoiding unnecessary changes to an already deterministic widget registry.

## Public include

For the complete system:

```cpp
#include <NV3047_Memory.h>
```

The umbrella header exposes:

- `MemoryManager`
- `MemoryBroker`
- `ElasticBuffer<T>`
- `ManagedBuffer<T>`
- `FramebufferPair`
- `DMAPool`
- `ObjectPool<T, Capacity>`
- `AssetCache`
- `AutoMemory`

## Repository layout

```text
NV3047_memorymanager/
├── .github/
│   └── workflows/
│       └── compile.yml
├── docs/
│   ├── INTEGRATION_PLAN.md
│   └── REFERENCE_BASELINES.md
├── examples/
│   ├── MemoryManagerDemo/
│   │   └── MemoryManagerDemo.ino
│   └── AdaptiveStressTest/
│       └── AdaptiveStressTest.ino
├── src/
│   ├── NV3047_Memory.h
│   ├── NV3047_MemoryManager.h
│   ├── NV3047_MemoryManager.cpp
│   ├── NV3047_MemoryBroker.h
│   ├── NV3047_MemoryBroker.cpp
│   ├── NV3047_ElasticBuffer.h
│   ├── NV3047_ManagedBuffer.h
│   ├── NV3047_FramebufferPair.h
│   ├── NV3047_FramebufferPair.cpp
│   ├── NV3047_DMAPool.h
│   ├── NV3047_DMAPool.cpp
│   ├── NV3047_ObjectPool.h
│   ├── NV3047_AssetCache.h
│   ├── NV3047_AssetCache.cpp
│   ├── NV3047_AutoMemory.h
│   └── NV3047_AutoMemory.cpp
├── library.properties
└── README.md
```

## Build verification

GitHub Actions compiles both `MemoryManagerDemo` and `AdaptiveStressTest` against:

- `esp32:esp32@2.0.17`
- `ESP32-S3 Dev Module`

This keeps the library locked to the same Arduino core generation used by the NV3047 projects.
