# NV3047 Memory Manager

Automatic ESP32-S3 memory management for the NV3047 display stack.

This library is designed to become the shared memory layer for:

- `NV3047_drivers`
- `NV3047_UI`

The two projects above are reference dependencies only. This repository owns all memory-manager code.

## Target

- ESP32-S3
- Arduino ESP32 Core 2.0.17
- PSRAM-aware
- RGB565 display workloads
- deterministic embedded allocations

## Why this exists

The current NV3047 driver allocates two 480 x 272 RGB565 framebuffers. Each framebuffer is 261,120 bytes, for 522,240 bytes total. It also creates temporary DMA-capable screen-fill memory. The current UI creates dynamic widget list nodes.

Those are exactly the kinds of allocations this manager is intended to centralise.

## Main features

- Automatic Internal RAM / PSRAM / DMA selection
- Fixed-reserve protection so display allocations cannot consume every free byte
- 64-byte aligned framebuffer helpers
- RGB565 bitmap helpers
- DMA-capable allocation helper
- Reusable per-frame scratch arena
- Allocation ownership tracking
- Tags for diagnostics and grouped cleanup
- Peak/high-water tracking
- Internal and PSRAM heap statistics
- Largest-free-block reporting for fragmentation visibility
- Low-memory pressure states and callback
- Safe fallback policy
- RAII `ManagedBuffer<T>`
- No STL containers and no dynamic bookkeeping tables

## Basic use

```cpp
#include <NV3047_MemoryManager.h>

using namespace NV3047Memory;

MemoryManager& memory = MemoryManager::instance();

void setup()
{
    Serial.begin(115200);

    MemoryConfig config;
    config.scratchBytes = 64 * 1024;

    if (!memory.begin(config))
    {
        Serial.println("Memory manager failed to start");
        return;
    }

    memory.dump(Serial);
}

void loop()
{
    memory.beginFrame();

    // Temporary memory is reused next frame instead of malloc/free churn.
    uint16_t* line =
        static_cast<uint16_t*>(
            memory.scratch(480 * sizeof(uint16_t), 4)
        );

    if (line)
    {
        // render/build temporary data...
    }

    memory.service();
}
```

## Purpose-based automatic placement

`MemoryManager::allocate()` uses the allocation purpose to choose memory automatically.

| Purpose | Preferred memory |
|---|---|
| General small allocation | Internal RAM |
| General large allocation | PSRAM |
| UI object | Internal RAM |
| Framebuffer | PSRAM |
| Bitmap / image asset | PSRAM |
| Scratch | PSRAM |
| DMA | Internal DMA-capable RAM |

If the preferred region cannot satisfy the request, the manager can fall back to another safe region when the configuration allows it. DMA allocations never fall back to non-DMA memory.

## NV3047-specific helpers

```cpp
uint16_t* framebuffer =
    memory.allocateFramebuffer(480, 272, "frame-a");

uint16_t* icon =
    memory.allocateRGB565(64 * 64, "settings-icon");

void* dma =
    memory.allocateDMA(9600, 4, "display-fill");
```

## Per-frame scratch memory

Repeated temporary allocations are a major source of fragmentation. The manager can reserve a scratch arena once, then recycle it every frame.

```cpp
memory.beginFrame();

void* workA = memory.scratch(2048, 16);
void* workB = memory.scratch(4096, 32);

// All scratch allocations become reusable together:
memory.resetScratch();
```

Scratch memory is never individually freed.

## Memory pressure monitoring

```cpp
void onPressure(
    MemoryPressure level,
    const MemoryStats& stats
)
{
    if (level == MemoryPressure::Critical)
    {
        // Drop optional caches/assets here.
    }
}

void setup()
{
    memory.begin();
    memory.setPressureCallback(onPressure);
}
```

Call `memory.service()` from the application loop. The callback fires when the pressure level changes.

## Read-only integration map

No changes are made by this repository to the driver or UI projects.

When those projects are later wired to this library, the intended replacements are:

### NV3047_drivers

Current framebuffer allocation:

```cpp
heap_caps_aligned_alloc(...)
```

Target:

```cpp
MemoryManager::instance().allocateFramebuffer(...)
```

Current temporary DMA allocation in `DisplayDriver::fillScreen()`:

```cpp
heap_caps_malloc(..., MALLOC_CAP_DMA)
```

Target:

```cpp
MemoryManager::instance().allocateDMA(...)
```

### NV3047_UI

Current widget-node creation uses `new WidgetNode`. A later UI integration can route widget metadata through manager-owned allocations or a dedicated node pool.

The memory manager deliberately does not override global `new` or `malloc`. Global overrides would make unrelated ESP32/Arduino components dependent on this library and would make failures harder to isolate.

## Design rules

1. PSRAM stores large graphical data.
2. Internal RAM is protected for the ESP32 runtime, stacks, networking, and control objects.
3. DMA requests always use DMA-capable internal memory.
4. Temporary render memory should use the scratch arena.
5. Every persistent manager allocation can be tracked and tagged.
6. The manager never silently frees a live persistent allocation to recover memory.
7. Allocation failure is explicit and measurable.

## Repository layout

```text
NV3047_memorymanager/
├── library.properties
├── README.md
├── examples/
│   └── MemoryManagerDemo/
│       └── MemoryManagerDemo.ino
└── src/
    ├── NV3047_MemoryManager.h
    ├── NV3047_MemoryManager.cpp
    └── NV3047_ManagedBuffer.h
```

## Status

Initial architecture and implementation. The library is intentionally isolated so the driver and UI overhauls can adopt it without circular dependencies.
