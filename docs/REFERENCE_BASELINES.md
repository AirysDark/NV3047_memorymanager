# Reference Baselines

These are the read-only source branches used when developing `NV3047_memorymanager`.

## NV3047_drivers

Branch:

```text
driver_overhaul_v2
```

Original reference commit:

```text
25e0b825b72683b7d88b42652e2ba013c540e042
```

Current observed branch head after external integration changes:

```text
7fa9382de2709121b8a33c9ae56318f7acef6d61
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
- current branch can delegate framebuffer ownership through `NV3047MemoryProviderV1`
- current branch can delegate the persistent 9,600-byte DMA fill buffer through the same provider
- once a provider is registered, takeover failure does not silently create a competing local framebuffer owner
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
- UI is explicitly tuned to `driver_overhaul_v2`

## Notes

- The UI branch name is hyphenated: `ui-overhaul-v2`.
- The driver branch name uses underscores: `driver_overhaul_v2`.
- The UI repository remains unchanged/read-only.
- The driver branch now contains provider integration changes made outside this memory-manager development pass.
- This pass modifies only `NV3047_memorymanager`; it does not make further driver/UI changes.
