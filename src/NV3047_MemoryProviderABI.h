#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef NV3047_MEMORY_PROVIDER_ABI_V1_H
#define NV3047_MEMORY_PROVIDER_ABI_V1_H

// Keep this ABI definition byte-for-byte compatible with
// NV3047_drivers/Core_Matrices/ExternalMemoryProvider.h.
struct NV3047MemoryProviderV1 {
    uint32_t abi_version;

    bool (*begin)(
        uint16_t width,
        uint16_t height,
        size_t buffer_count,
        size_t buffer_size_bytes,
        size_t buffer_alignment,
        bool zero_buffers,
        size_t dma_block_bytes,
        size_t dma_alignment);

    void (*end)();
    bool (*is_ready)();

    uint16_t* (*front_buffer)();
    uint16_t* (*draw_buffer)();
    void (*swap_buffers)();

    size_t (*buffer_count)();
    size_t (*buffer_size_bytes)();
    size_t (*total_allocated_bytes)();
    size_t (*free_managed_bytes)();
    size_t (*largest_free_managed_block_bytes)();

    void (*begin_frame)();
    void (*service)();

    void* (*acquire_dma)(
        size_t bytes,
        size_t alignment);

    void (*release_dma)(void* pointer);
};

static constexpr uint32_t NV3047_MEMORY_PROVIDER_ABI_VERSION = 1;

#endif
