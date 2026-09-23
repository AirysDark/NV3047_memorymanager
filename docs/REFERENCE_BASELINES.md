# Reference Baselines

These are the read-only source branches used when developing `NV3047_memorymanager`.

## NV3047_drivers

Active performance reference branch:

```text
driver_overhaul_v3
```

Current observed V3 head:

```text
d718d71eac960b4b876f4afd358d0cf6a0a09b59
```

Historical V2 baseline branch:

```text
driver_overhaul_v2
```

Original pre-integration reference commit:

```text
25e0b825b72683b7d88b42652e2ba013c540e042
```

Important memory-related files:

- `src/Config.h`
- `src/Core_Matrices/MemoryManager.h`
- `src/Core_Matrices/MemoryManager.cpp`
- `src/Core_Matrices/framebuffer.h`
- `src/Core_Matrices/framebuffer.cpp`
- `src/Core_Matrices/ExternalMemoryProvider.h`
- `src/Peripherals_HAL/DisplayDriver.cpp`
- `src/NV3047_Driver.cpp`

Observed production defaults / behavior:

- 480 x 272 RGB565
- 2 framebuffers
- 261,120 bytes per framebuffer
- 522,240 bytes total framebuffer storage
- 64-byte framebuffer alignment
- PSRAM + 8-bit framebuffer allocation caps
- zero buffers on initialization
- without an external provider, local driver `MemoryManager` owns front/draw buffer indexes
- V3 preserves the `NV3047MemoryProviderV1` takeover ABI
- V3 delegates the persistent 9,600-byte DMA fill buffer through the same provider
- once a provider is registered, takeover failure does not silently create a competing local framebuffer owner
- V3 caches the active draw-buffer pointer once per frame rather than resolving it through the provider for every drawing primitive
- V3 refreshes that cached pointer after successful buffer-role swaps
- high-level `NV3047_Driver::fillScreen()` uses framebuffer clear + present rather than the HAL fill-buffer path

## NV3047_UI

Branch:

```text
ui-overhaul-v2
```

Reference commit when this file was written:

```text
56ea96c4877f616b1e435744cd7ef8c5f2582e78
```

Important memory/integration files:

- `src/NV3047_UI.h`
- `src/NV3047_UI.cpp`
- `src/NV3047_UI_Extras.h`
- `README.md`

Observed memory/integration behavior:

- `Screen::MAX_WIDGETS = 40`
- widget registration uses fixed inline `WidgetSlot` storage
- normal screen/widget registration performs no per-widget heap allocation
- `UIDriverStats` exposes framebuffer count, size, allocated bytes, free managed memory and largest free managed block
- UI presentation cadence remains owned by the driver
- UI consumes the driver's already-mapped touch coordinates
- the UI reference branch predates Driver V3 and remains read-only during Memory Manager Overhaul V1

## Notes

- The UI branch name is hyphenated: `ui-overhaul-v2`.
- The active driver branch name is `driver_overhaul_v3`.
- The UI repository remains unchanged/read-only.
- The driver branch now contains provider integration changes made outside this memory-manager development pass.
- This pass modifies only `NV3047_memorymanager`; it does not make further driver/UI changes.
