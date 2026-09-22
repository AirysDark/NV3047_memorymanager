# Reference Baselines

These are the read-only source branches used when developing `NV3047_memorymanager`.

## NV3047_drivers

Branch:

```text
driver_overhaul_v2
```

Reference commit when this file was written:

```text
25e0b825b72683b7d88b42652e2ba013c540e042
```

Important memory-related files:

- `src/Config.h`
- `src/Core_Matrices/MemoryManager.h`
- `src/Core_Matrices/MemoryManager.cpp`
- `src/Core_Matrices/framebuffer.h`
- `src/Core_Matrices/framebuffer.cpp`
- `src/Peripherals_HAL/DisplayDriver.cpp`
- `src/NV3047_Driver.cpp`

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

## Notes

- The UI branch name is hyphenated: `ui-overhaul-v2`.
- The driver branch name uses underscores: `driver_overhaul_v2`.
- These repositories remain read-only references during memory-manager development.
- Only `NV3047_memorymanager` is modified.
