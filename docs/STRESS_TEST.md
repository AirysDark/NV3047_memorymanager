# Adaptive Stress Test

`examples/AdaptiveStressTest/AdaptiveStressTest.ino` is the runtime hardware test for the adaptive broker.

## Target

- ESP32-S3
- Arduino ESP32 Core 2.0.17
- PSRAM enabled
- NV3047 memory-manager defaults

## What it exercises

The test cycles every four seconds through:

1. UI-heavy
2. application-heavy
3. both active
4. idle/recovery

It deliberately keeps UI and application working sets in `ElasticBuffer` objects and creates optional unpinned PSRAM assets.

During workload changes the broker can reclaim lower-importance PSRAM donors and later recreate elastic buffers when their workload becomes active again.

## Region-aware test path

The UI/application handoff explicitly requests:

```cpp
broker.reclaimFor(
    requester,
    bytes,
    BrokerReclaimReason::Request,
    MemoryRegion::PSRAM
);
```

This verifies that PSRAM pressure does not cause unrelated internal-RAM donor selection.

## Fragmentation visibility

Every report prints:

- current validation state
- free PSRAM
- largest PSRAM free block
- UI elastic residency
- application elastic residency

The important distinction is:

```text
free PSRAM > requested allocation
largest block < requested allocation
```

That indicates fragmentation. The broker's request path now uses the largest-block value when deciding how much region-specific memory to reclaim before retrying.

## Scratch rewind

Each loop marks the scratch arena, allocates a temporary block, uses it, then rewinds to the mark in the same frame.

This verifies nested temporary work does not have to remain allocated until the next `beginFrame()`.

## Integrity checking

The test calls:

```cpp
memory.validate();
```

A healthy test should continue reporting:

```text
validate=OK
```

A FAIL result means one of the memory ownership/accounting invariants no longer agrees and should be investigated before integrating further changes.

## CI

GitHub Actions compiles this example on every push together with `MemoryManagerDemo`.

CI confirms API/build compatibility. Actual fragmentation/reclamation behavior still needs physical ESP32-S3 runtime testing because the host runner cannot reproduce the board's real heap layout.
