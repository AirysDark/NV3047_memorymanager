#include "NV3047_DMAPool.h"
#include <string.h>

namespace NV3047Memory
{

DMAPool::DMAPool()
    : manager_(nullptr),
      block_bytes_(0),
      alignment_(4),
      block_count_(0),
      used_count_(0)
{
    memset(
        blocks_,
        0,
        sizeof(blocks_)
    );
}

DMAPool::~DMAPool()
{
    end();
}

bool DMAPool::begin(
    size_t blockBytes,
    uint8_t blockCount,
    MemoryManager* manager,
    size_t alignment
)
{
    end();

    if (
        blockBytes == 0 ||
        blockCount == 0 ||
        blockCount > MAX_BLOCKS
    )
    {
        return false;
    }

    manager_ =
        manager
            ? manager
            : &MemoryManager::instance();

    if (!manager_->isReady())
    {
        manager_ = nullptr;
        return false;
    }

    block_bytes_ = blockBytes;
    block_count_ = blockCount;
    alignment_ = alignment;
    used_count_ = 0;

    for (
        uint8_t i = 0;
        i < block_count_;
        ++i
    )
    {
        blocks_[i].pointer =
            manager_->allocateDMA(
                block_bytes_,
                alignment_,
                "dma-pool"
            );

        blocks_[i].inUse = false;

        if (!blocks_[i].pointer)
        {
            end();
            return false;
        }
    }

    return true;
}

void DMAPool::end()
{
    if (manager_)
    {
        for (
            uint8_t i = 0;
            i < MAX_BLOCKS;
            ++i
        )
        {
            if (blocks_[i].pointer)
            {
                manager_->release(
                    blocks_[i].pointer
                );
            }
        }
    }

    memset(
        blocks_,
        0,
        sizeof(blocks_)
    );

    manager_ = nullptr;
    block_bytes_ = 0;
    alignment_ = 4;
    block_count_ = 0;
    used_count_ = 0;
}

void* DMAPool::acquire()
{
    if (
        !manager_ ||
        !manager_->isReady()
    )
    {
        return nullptr;
    }

    for (
        uint8_t i = 0;
        i < block_count_;
        ++i
    )
    {
        if (
            blocks_[i].pointer &&
            !blocks_[i].inUse
        )
        {
            blocks_[i].inUse = true;
            ++used_count_;

            return
                blocks_[i].pointer;
        }
    }

    return nullptr;
}

void DMAPool::release(
    void* pointer
)
{
    if (!pointer)
    {
        return;
    }

    for (
        uint8_t i = 0;
        i < block_count_;
        ++i
    )
    {
        if (
            blocks_[i].pointer ==
            pointer
        )
        {
            if (blocks_[i].inUse)
            {
                blocks_[i].inUse =
                    false;

                if (used_count_ > 0)
                {
                    --used_count_;
                }
            }

            return;
        }
    }
}

bool DMAPool::owns(
    const void* pointer
) const
{
    if (
        !pointer ||
        !manager_ ||
        !manager_->isReady()
    )
    {
        return false;
    }

    for (
        uint8_t i = 0;
        i < block_count_;
        ++i
    )
    {
        if (
            blocks_[i].pointer ==
            pointer
        )
        {
            return true;
        }
    }

    return false;
}

bool DMAPool::isInUse(
    const void* pointer
) const
{
    if (
        !pointer ||
        !manager_ ||
        !manager_->isReady()
    )
    {
        return false;
    }

    for (
        uint8_t i = 0;
        i < block_count_;
        ++i
    )
    {
        if (
            blocks_[i].pointer ==
            pointer &&
            manager_->owns(pointer)
        )
        {
            return
                blocks_[i].inUse;
        }
    }

    return false;
}

size_t DMAPool::blockBytes() const
{
    return block_bytes_;
}

uint8_t DMAPool::blockCount() const
{
    return block_count_;
}

uint8_t DMAPool::usedCount() const
{
    if (
        !manager_ ||
        !manager_->isReady()
    )
    {
        return 0;
    }

    uint8_t count = 0;

    for (
        uint8_t i = 0;
        i < block_count_;
        ++i
    )
    {
        if (
            blocks_[i].pointer &&
            blocks_[i].inUse
        )
        {
            ++count;
        }
    }

    return count;
}

uint8_t DMAPool::freeCount() const
{
    if (
        !manager_ ||
        !manager_->isReady()
    )
    {
        return 0;
    }

    uint8_t count = 0;

    for (
        uint8_t i = 0;
        i < block_count_;
        ++i
    )
    {
        if (
            blocks_[i].pointer &&
            !blocks_[i].inUse
        )
        {
            ++count;
        }
    }

    return count;
}


bool DMAPool::validate() const
{
    if (
        !manager_ ||
        !manager_->isReady() ||
        block_count_ == 0 ||
        block_count_ > MAX_BLOCKS
    )
    {
        return false;
    }

    uint8_t inUse = 0;

    for (
        uint8_t i = 0;
        i < block_count_;
        ++i
    )
    {
        const Block& block =
            blocks_[i];

        if (
            !block.pointer ||
            !manager_->owns(
                block.pointer
            )
        )
        {
            return false;
        }

        if (block.inUse)
        {
            ++inUse;
        }
    }

    for (
        uint8_t i = block_count_;
        i < MAX_BLOCKS;
        ++i
    )
    {
        if (
            blocks_[i].pointer ||
            blocks_[i].inUse
        )
        {
            return false;
        }
    }

    return
        inUse ==
            used_count_;
}

} // namespace NV3047Memory
