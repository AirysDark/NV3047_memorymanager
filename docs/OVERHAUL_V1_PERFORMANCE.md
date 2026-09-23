# Memory Manager Overhaul V1 — Performance Plan

Branch:

```text
memory_manager_overhaul_v1
```

Read-only driver reference:

```text
AirysDark/NV3047_drivers
driver_overhaul_v3
d718d71eac960b4b876f4afd358d0cf6a0a09b59
```

Target:

```text
Elecrow CrowPanel 4.3"
ESP32-S3
Arduino-ESP32 Core 2.0.17
```

## Hardware baseline that triggered the overhaul

The previous staged benchmark measured:

```text
Driver-only V2:
    625 frames
    13.631 FPS overall
    73,304 us average interval

Memory-manager V2 path:
    577 frames
    12.644 FPS overall
    78,945 us average interval

Difference:
    -0.987 FPS
    roughly 7.2% lower throughput
```

The gap was almost absent at one object and grew to approximately 1.3–1.5 FPS under 96–220-object workloads.

That shape strongly indicated work scaling with primitive count. Driver Overhaul V3 addresses that portion by caching the draw-buffer pointer per frame.

Memory Manager Overhaul V1 addresses the remaining manager-side costs.

## Overhaul V1 hot-path changes

### 1. Cached heap pressure

Normal `pressure()` reads no longer perform heap queries.

Heap statistics are sampled only when:

- memory allocation/release changes the heap revision
- the configured monitoring interval expires
- an explicit forced refresh is requested

The sampled result is cached and reused by broker/service logic.

### 2. Dirty/revision-driven broker accounting

The fixed driver framebuffer/DMA footprint is constant during one takeover session and is published once.

Asset cache usage has a revision counter. Broker usage is updated only when:

- cache bytes change
- an entry is pinned/unpinned
- an entry is added/removed/replaced
- reclaim actually changes cache accounting

Cache reads/touches do not cause usage resynchronization.

### 3. O(1) AssetCache stats

The cache now maintains incrementally:

- used bytes
- pinned bytes
- reclaimable bytes
- entry count
- pinned count

`AssetCache::stats()` returns those counters directly rather than scanning all 32 entries.

### 4. Constant-time readiness

Normal runtime readiness checks no longer prove ownership by scanning the global allocation table.

Deep ownership checks remain available through `validate()`.

This applies to:

- FramebufferPair readiness
- MemoryBroker readiness
- DMA pool hot APIs
- AssetCache hit/peek paths

### 5. One pressure reclaim owner

When the broker is enabled, MemoryBroker owns Warning/Critical donor reclaim.

AutoMemory no longer performs a second broker reclaim pass for the same pressure transition.

AutoMemory still owns:

- broker-disabled fallback cache trimming
- critical scratch reset policy

### 6. Broker Normal-pressure fast exit

At broker service cadence:

1. decay client activity
2. read cached pressure
3. if pressure is Normal, return

Client reclaimable/lease scans are skipped unless pressure is actually Warning or Critical.

### 7. Frame callback synchronization

During active driver takeover, the autoruntime background task does not perform full service.

Therefore `providerBeginFrame()` and `providerService()` no longer take the outer autoruntime semaphore every frame.

Internal MemoryManager and MemoryBroker synchronization remains intact.

### 8. Dedicated frame-scratch synchronization

`beginFrame()` only resets the preallocated scratch offset. Overhaul V1 no longer takes the global allocation-table semaphore for this operation.

Scratch offset allocation, rewind and reset now use a dedicated short ESP32 critical section. Allocation-table ownership and heap bookkeeping keep their existing manager mutex.

Deep validation samples scratch state through the same scratch critical section.

### 9. Trusted takeover session

Provider startup now validates the complete memory configuration once and caches stable references to:

- AutoMemory
- FramebufferPair
- MemoryManager
- DMAPool

The session-ready flag is published only after validation succeeds and is cleared before teardown begins.

Normal provider callbacks no longer reconstruct readiness through the manager hierarchy.

### 10. Trusted framebuffer access

FramebufferPair keeps its checked public API, but the established takeover session uses inline trusted accessors for:

- front buffer
- draw/back buffer
- role swap

These helpers perform no ownership/readiness scans and are valid only while the takeover session is ready.

### 11. Provider service gate

The driver still calls the provider service callback after successful presentation, preserving the ABI and frame-boundary contract.

The bridge now checks one shared timestamp against:

- dirty heap revision
- asset accounting revision
- cached pressure transition
- memory-monitor deadline
- broker-service deadline

If nothing is pending or due, provider service returns before entering AutoMemory service.

### 12. Event/deadline-driven maintenance

Memory maintenance is now driven by state changes and deadlines rather than frame rate.

A healthy frame with no memory changes normally performs:

```text
swap framebuffer role
scratch used? no -> skip reset
maintenance due? no -> return
draw pointer -> direct trusted session access
```

Heap and broker work still runs immediately when allocation/accounting state changes, and periodically at the configured 1000 ms / 500 ms deadlines.

### 13. Shared service timestamp

Provider service reads `millis()` once when checking maintenance.

That same timestamp is passed to AutoMemory, MemoryManager and MemoryBroker for the maintenance pass. The lower layers no longer each need a separate frame-time clock read.

### 14. Conditional post-policy synchronization

Broker usage is synchronized before broker service only when driver or asset accounting revisions require it.

A second synchronization happens only if broker reclaim or fallback pressure policy actually changes the asset accounting revision.

Normal stable frames do not enter broker synchronization.

### 15. Scratch no-op fast path

Successful scratch allocation sets a touched flag.

At the next frame boundary:

- untouched scratch -> return without entering the scratch critical section
- touched scratch -> reset offset and clear touched state

Rewinding scratch back to offset zero also clears the touched state.

### 16. Expanded profiling

Profiling now counts both callback entries and real work:

- provider ready/front/draw/swap calls
- provider begin-frame reset/no-op counts
- provider service full/fast-exit counts
- provider service timing
- broker service due/skipped counts
- broker usage update/skip counts
- heap samples
- pressure-policy actions
- DMA acquire/release calls

This is more useful than timing ultra-small callbacks whose individual duration may fall below `micros()` resolution.

## Central configuration

Normal defaults are now centralized in:

```text
src/NV3047_MemoryConfig.h
```

This includes:

- RAM reserves
- pressure thresholds
- scratch size
- heap monitor interval
- broker service interval
- broker reclaim percentages
- UI/application soft budgets
- framebuffer dimensions
- DMA pool defaults
- asset cache budget
- profiling default

## Hardware profiling

Use:

```text
examples/PerformanceProfile/PerformanceProfile.ino
```

It uses normal include-only takeover and then enables profiler timing.

The important outputs are:

```text
provider ready/front/draw/swap
provider beginFrame calls/reset/no-op
provider service calls/full/fast-exit
provider service avg/max us
AutoMemory service full passes/avg/max us
heap sample passes
broker service due/skipped
broker usage sync/update skips
asset usage syncs
pressure policy actions
provider DMA acquire/release
```

For a healthy steady workload, expected behavior is:

- provider service callbacks still occur at frame boundaries because the ABI remains unchanged
- most provider service callbacks fast-exit before AutoMemory
- begin-frame is normally a no-op when the render workload did not use scratch
- heap samples occur far less frequently than frames
- fixed driver usage is not repeatedly republished
- asset usage sync count remains near zero when the cache does not change
- normal-pressure broker passes do not scan reclaimable lease totals

## Required comparison

Run the same staged benchmark in separate boots:

```text
A. driver_overhaul_v3 only
B. driver_overhaul_v3 + NV3047_Memory.h from memory_manager_overhaul_v1
```

Keep the exact same:

- object stages
- stage durations
- touch sampling
- panel cadence
- colour path
- touch mapping

Compare:

- overall FPS
- average frame interval
- slowest interval
- interval spread
- each heavy stage (96 / 160 / 200 / 220)
- present errors
- profiler average/max service microseconds

Do not modify touch or colour while evaluating performance.


## Deferred experiments

Overhaul V1 intentionally does **not** force O2/O3, broad `always_inline`, or IRAM placement at this stage.

Tiny session/framebuffer accessors are already inline where their fixed-state nature is clear. More aggressive compiler attributes will only be tested after the physical ESP32-S3 benchmark identifies a remaining measurable hotspot, because larger forced code can increase I-cache/flash pressure under Arduino-ESP32 Core 2.0.17.
