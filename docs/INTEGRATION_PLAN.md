# NV3047 Memory Takeover Plan

This document defines the future handoff from the active NV3047 overhaul branches into `NV3047_memorymanager`.

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

## Driver takeover order

### 1. Startup

Start `AutoMemory` before the driver's framebuffer subsystem performs any large allocations.

### 2. Replace local driver MemoryManager

The driver overhaul currently owns its own class:

```text
Core_Matrices/MemoryManager
    -> allocates Config::MemoryManager::BUFFER_COUNT
    -> exposes front/draw buffers
    -> swaps buffer indexes
```

The future integration should preserve that external behavior while moving actual allocation ownership to:

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

### 2. Assets

Icons, RGB565 bitmaps and decoded graphical data should enter `AssetCache`.

Frequently required UI assets may be pinned.

Optional page-specific assets should remain unpinned so automatic pressure recovery can evict them.

### 3. Layout/render scratch

Temporary arrays, transformed coordinates, generated scanlines and other frame-local data should use the scratch arena.

### 4. Optional dynamic UI objects

Use `ObjectPool<T, Capacity>` only where the UI/application actually creates and destroys objects dynamically.

Good candidates include:

- runtime-created pages
- reusable dialog instances
- transient model/view objects
- future dynamically composed widgets

### 5. Diagnostics

The overhaul UI already expects driver diagnostics including:

- framebuffer count
- framebuffer size
- total framebuffer bytes
- free managed memory
- largest free managed block

When the external manager takes control, preserve these diagnostic semantics so `UIDriverStats` continues working without UI redesign.

## Render-loop target

The eventual top-level frame lifecycle should resemble:

```cpp
AutoMemory& memory =
    AutoMemory::instance();

void loop()
{
    memory.beginFrame();

    // UI input
    // UI layout
    // render into the driver's draw buffer,
    // backed by memory.framebuffers().back()

    // driver presents the draw buffer

    memory.framebuffers().
        swapRoles();

    memory.service();
}
```

The driver remains responsible for presentation cadence and physical display submission. The memory manager owns the storage.

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
| optional pooled object | ObjectPool |
| general managed allocation | MemoryManager / ManagedBuffer |

Consumers borrow these pointers. Consumers do not directly `free()` them.

## Core compatibility

The memory system is developed and CI-compiled against Arduino ESP32 Core 2.0.17, matching both overhaul branches.
