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
beginFrame calls/avg/max us
service calls/avg/max us
heap sample passes
manager heap samples total
broker usage syncs
asset usage syncs
```

For a healthy steady workload, expected behavior is:

- service is called every presented frame because the driver contract remains unchanged
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
