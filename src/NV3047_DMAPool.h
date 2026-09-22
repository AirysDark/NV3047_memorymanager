#pragma once

#include "NV3047_MemoryManager.h"
#include <stddef.h>
#include <stdint.h>

namespace NV3047Memory
{

class DMAPool
{
public:
    static constexpr uint8_t MAX_BLOCKS = 8;

    DMAPool();
    ~DMAPool();

    DMAPool(const DMAPool&) = delete;
    DMAPool& operator=(const DMAPool&) = delete;

    bool begin(
        size_t blockBytes,
        uint8_t blockCount,
        MemoryManager* manager = nullptr,
        size_t alignment = 4
    );

    void end();

    void* acquire();
    void release(void* pointer);

    bool owns(const void* pointer) const;
    bool isInUse(const void* pointer) const;

    size_t blockBytes() const;
    uint8_t blockCount() const;
    uint8_t usedCount() const;
    uint8_t freeCount() const;

private:
    struct Block
    {
        void* pointer;
        bool inUse;
    };

    MemoryManager* manager_;
    Block blocks_[MAX_BLOCKS];

    size_t block_bytes_;
    size_t alignment_;

    uint8_t block_count_;
    uint8_t used_count_;
};

} // namespace NV3047Memory
