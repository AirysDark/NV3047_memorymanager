# NV3047 Memory Manager

Automatic memory control for the NV3047 ESP32-S3 display stack.

This repository is the dedicated memory subsystem for:

- `NV3047_drivers`
- `NV3047_UI`

The driver and UI repositories are currently read-only references. All development and changes for memory management live in this repository.

## Target

- ESP32-S3
- Arduino ESP32 Core **2.0.17**
- PSRAM-backed 480 x 272 RGB565 graphics
- deterministic embedded allocations
- low-fragmentation long-running UI applications

## Current version

**0.2.0**

The library has moved beyond a basic allocator and now provides an automatic ownership layer for the major memory classes used by the NV3047 stack.

## Current NV3047 memory pressure

The reference driver currently uses two 480 x 272 RGB565 framebuffers:

- one framebuffer: **261,120 bytes**
- two framebuffers: **522,240 bytes**

The current driver also creates a 10-line RGB565 DMA-compatible temporary buffer:

- 480 x 10 x 2 bytes = **9,600 bytes**

The current UI dynamically creates widget-list nodes.

The memory manager is designed to take ownership of all three categories when the other libraries are later connected to it.

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
- 64-byte aligned framebuffer allocation
- RGB565 allocation helpers
- DMA-only allocation helpers
- allocation tagging
- grouped cleanup by tag
- ownership checking
- tracked allocation size
- high-water statistics
- failed-allocation counters
- minimum-free-heap statistics
- largest-free-block statistics
- warning and critical memory-pressure states
- pressure callbacks
- reusable frame scratch arena

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

This replaces repeated `heap_caps_malloc(... MALLOC_CAP_DMA)` / free cycles with stable reusable blocks.

The default automatic configuration creates two 9,600-byte blocks, matching the current driver's 10-line temporary screen-fill buffer size.

### `AssetCache`

PSRAM-oriented cache for RGB565 images, icons and other graphical data.

Features:

- fixed metadata table
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

Fixed-capacity typed object pool intended for UI/control structures such as widget nodes.

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

    config.dmaBlockCount = 2;

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

DMA requests never fall back into memory that is not DMA capable.

## Automatic pressure handling

`AutoMemory::service()` checks the base manager pressure state.

### Warning

The asset cache is trimmed toward the configured warning percentage.

### Critical

The automatic controller can:

- purge all unpinned cached assets
- reset transient scratch usage
- preserve pinned assets
- preserve live framebuffers
- preserve active UI objects
- preserve DMA pool ownership

The manager deliberately does **not** destroy live persistent application objects simply to recover RAM.

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

## UI object pool

```cpp
struct WidgetNode
{
    void* widget;
    WidgetNode* next;
    uint8_t z;
};

ObjectPool<WidgetNode, 64> nodes;

nodes.begin(
    &memory.memory(),
    "widget-nodes"
);

WidgetNode* node =
    nodes.create();

nodes.destroy(node);
```

This is the intended future replacement for repeated UI widget-node `new` / `delete` operations.

## Future driver takeover

When `NV3047_drivers` is later changed to allow this repository to control its memory, the intended ownership becomes:

```text
Current driver Framebuffer::buffer_a
    -> AutoMemory::framebuffers().front/back

Current driver Framebuffer::buffer_b
    -> AutoMemory::framebuffers().front/back

Current DisplayDriver temporary DMA malloc
    -> AutoMemory::dmaPool().acquire()

Current repeated graphical asset allocations
    -> AutoMemory::assets()
```

The driver should no longer independently allocate its own framebuffer memory after takeover.

## Future UI takeover

When `NV3047_UI` is later connected:

```text
Widget nodes
    -> ObjectPool

temporary render/layout working memory
    -> frame scratch arena

runtime icons/images
    -> AssetCache

large UI data
    -> MemoryManager automatic allocator
```

This keeps the UI focused on rendering and layout while this library decides where memory lives.

## Public include

For the complete system:

```cpp
#include <NV3047_Memory.h>
```

The umbrella header exposes:

- `MemoryManager`
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
│   └── INTEGRATION_PLAN.md
├── examples/
│   └── MemoryManagerDemo/
│       └── MemoryManagerDemo.ino
├── src/
│   ├── NV3047_Memory.h
│   ├── NV3047_MemoryManager.h
│   ├── NV3047_MemoryManager.cpp
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

GitHub Actions compiles the library and example against:

- `esp32:esp32@2.0.17`
- `ESP32-S3 Dev Module`

This keeps the library locked to the same Arduino core generation used by the NV3047 projects.
