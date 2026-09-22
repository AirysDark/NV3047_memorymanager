# NV3047 Memory Takeover Plan

This document defines the future handoff from the existing NV3047 driver/UI allocation code to `NV3047_memorymanager`.

The memory-manager repository remains the owner of memory policy.

## Rule

The driver and UI may request memory.

They should not independently decide:

- Internal RAM versus PSRAM
- reserve thresholds
- cache eviction
- scratch lifetime
- DMA reuse policy
- framebuffer lifetime

Those decisions belong here.

## Driver takeover order

### 1. Startup

Create/start `AutoMemory` before the driver framebuffer subsystem performs any large allocations.

### 2. Framebuffers

Remove the driver's independent framebuffer allocation path.

Instead bind the framebuffer engine to:

- `AutoMemory::framebuffers().front()`
- `AutoMemory::framebuffers().back()`

After a successful physical display swap:

- call `FramebufferPair::swapRoles()`

No framebuffer malloc/free should occur during normal rendering.

### 3. Temporary DMA screen-fill memory

Replace the driver's temporary DMA allocation with:

```cpp
void* block =
    AutoMemory::instance().
        dmaPool().acquire();
```

Return it immediately after the transaction:

```cpp
AutoMemory::instance().
    dmaPool().release(block);
```

### 4. Driver scratch work

Short-lived CPU-only work buffers should use:

```cpp
AutoMemory::instance().
    memory().scratch(...);
```

They must not survive the next `beginFrame()`.

## UI takeover order

### 1. Widget metadata

Convert repeated widget-node heap allocations to a fixed `ObjectPool`.

The UI can choose the compile-time capacity, while allocation placement remains controlled by the memory manager.

### 2. Assets

Icons, RGB565 bitmaps and decoded graphical data should enter `AssetCache`.

Frequently required UI assets may be pinned.

Optional page-specific assets should remain unpinned so automatic pressure recovery can evict them.

### 3. Layout/render scratch

Temporary arrays, transformed coordinates, generated scanlines and other frame-local data should use the scratch arena.

### 4. Screen/page lifetime

Persistent page objects may use `MemoryManager::allocate(... MemoryPurpose::UIObject ...)` or typed `ManagedBuffer`/object-pool ownership.

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
    // render into memory.framebuffers().back()

    // submit back buffer to display

    memory.framebuffers().
        swapRoles();

    memory.service();
}
```

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

## Ownership rule

Every pointer has exactly one owner.

Examples:

| Pointer | Owner |
|---|---|
| front framebuffer | FramebufferPair |
| back framebuffer | FramebufferPair |
| DMA pool block | DMAPool |
| cached icon | AssetCache |
| scratch pointer | MemoryManager scratch arena |
| object-pool node | ObjectPool |
| general managed allocation | MemoryManager / ManagedBuffer |

Consumers borrow these pointers. Consumers do not directly `free()` them.

## Core compatibility

The memory system is developed and CI-compiled against Arduino ESP32 Core 2.0.17, matching the NV3047 driver target.
