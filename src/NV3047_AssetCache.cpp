#include "NV3047_AssetCache.h"
#include <string.h>

namespace NV3047Memory
{

AssetCache::AssetCache()
    : manager_(nullptr),
      budget_bytes_(0),
      used_bytes_(0),
      peak_bytes_(0),
      hits_(0),
      misses_(0),
      evictions_(0),
      use_counter_(0)
{
    memset(
        entries_,
        0,
        sizeof(entries_)
    );
}

AssetCache::~AssetCache()
{
    end();
}

bool AssetCache::begin(
    size_t budgetBytes,
    MemoryManager* manager
)
{
    end();

    manager_ =
        manager
            ? manager
            : &MemoryManager::instance();

    if (!manager_->isReady())
    {
        manager_ = nullptr;
        return false;
    }

    budget_bytes_ = budgetBytes;

    return true;
}

void AssetCache::end()
{
    clear();

    manager_ = nullptr;
    budget_bytes_ = 0;
    used_bytes_ = 0;
    peak_bytes_ = 0;
    hits_ = 0;
    misses_ = 0;
    evictions_ = 0;
    use_counter_ = 0;

    memset(
        entries_,
        0,
        sizeof(entries_)
    );
}

int AssetCache::findIndex(
    uint32_t key
) const
{
    for (
        uint8_t i = 0;
        i < MAX_ENTRIES;
        ++i
    )
    {
        if (
            entries_[i].used &&
            entries_[i].key == key
        )
        {
            return i;
        }
    }

    return -1;
}

int AssetCache::findFreeIndex() const
{
    for (
        uint8_t i = 0;
        i < MAX_ENTRIES;
        ++i
    )
    {
        if (!entries_[i].used)
        {
            return i;
        }
    }

    return -1;
}

int AssetCache::findLRUUnpinned(
    int excludeIndex
) const
{
    int candidate = -1;
    uint32_t oldest = 0;

    for (
        uint8_t i = 0;
        i < MAX_ENTRIES;
        ++i
    )
    {
        const Entry& entry =
            entries_[i];

        if (
            !entry.used ||
            entry.pinned ||
            static_cast<int>(i) ==
                excludeIndex
        )
        {
            continue;
        }

        if (
            candidate < 0 ||
            entry.lastUse < oldest
        )
        {
            candidate = i;
            oldest = entry.lastUse;
        }
    }

    return candidate;
}

void AssetCache::touch(
    Entry& entry
)
{
    ++use_counter_;

    if (use_counter_ == 0)
    {
        use_counter_ = 1;

        for (
            uint8_t i = 0;
            i < MAX_ENTRIES;
            ++i
        )
        {
            if (entries_[i].used)
            {
                entries_[i].lastUse = 1;
            }
        }
    }

    entry.lastUse =
        use_counter_;
}

bool AssetCache::makeRoom(
    size_t bytes
)
{
    if (!manager_)
    {
        return false;
    }

    // A single entry larger than the entire cache budget can never fit.
    // Reject it without evicting valid cached assets first.
    if (
        budget_bytes_ != 0 &&
        bytes > budget_bytes_
    )
    {
        return false;
    }

    while (
        (
            budget_bytes_ != 0 &&
            used_bytes_ >
                (
                    budget_bytes_ -
                    bytes
                )
        ) ||
        findFreeIndex() < 0
    )
    {
        const int lru =
            findLRUUnpinned();

        if (lru < 0)
        {
            return false;
        }

        const uint32_t key =
            entries_[lru].key;

        if (!remove(key))
        {
            return false;
        }

        ++evictions_;
    }

    return true;
}

void* AssetCache::put(
    uint32_t key,
    const void* source,
    size_t bytes,
    bool pinned,
    size_t alignment
)
{
    if (
        !manager_ ||
        !manager_->isReady() ||
        bytes == 0
    )
    {
        return nullptr;
    }

    const int existing =
        findIndex(key);

    if (existing >= 0)
    {
        Entry& entry =
            entries_[existing];

        if (entry.bytes == bytes)
        {
            if (source)
            {
                memcpy(
                    entry.pointer,
                    source,
                    bytes
                );
            }

            entry.pinned = pinned;
            touch(entry);

            return entry.pointer;
        }

        if (
            budget_bytes_ != 0 &&
            bytes > budget_bytes_
        )
        {
            return nullptr;
        }

        // Keep the old entry alive until its replacement is successfully
        // allocated. This prevents a failed resize from destroying a
        // previously valid cached asset.
        size_t effectiveUsed =
            used_bytes_ >= entry.bytes
                ? used_bytes_ - entry.bytes
                : 0;

        while (
            budget_bytes_ != 0 &&
            effectiveUsed >
                (
                    budget_bytes_ -
                    bytes
                )
        )
        {
            const int lru =
                findLRUUnpinned(
                    existing
                );

            if (lru < 0)
            {
                return nullptr;
            }

            const size_t evictedBytes =
                entries_[lru].bytes;

            const uint32_t lruKey =
                entries_[lru].key;

            if (!remove(lruKey))
            {
                return nullptr;
            }

            ++evictions_;

            if (
                effectiveUsed >=
                evictedBytes
            )
            {
                effectiveUsed -=
                    evictedBytes;
            }
            else
            {
                effectiveUsed = 0;
            }
        }

        void* replacement =
            manager_->allocate(
                bytes,
                MemoryPurpose::Bitmap,
                alignment,
                "asset-cache"
            );

        while (!replacement)
        {
            const int lru =
                findLRUUnpinned(
                    existing
                );

            if (lru < 0)
            {
                break;
            }

            const uint32_t lruKey =
                entries_[lru].key;

            if (!remove(lruKey))
            {
                break;
            }

            ++evictions_;

            replacement =
                manager_->allocate(
                    bytes,
                    MemoryPurpose::Bitmap,
                    alignment,
                    "asset-cache"
                );
        }

        if (!replacement)
        {
            return nullptr;
        }

        if (source)
        {
            memcpy(
                replacement,
                source,
                bytes
            );
        }

        void* oldPointer =
            entry.pointer;

        const size_t oldBytes =
            entry.bytes;

        manager_->release(
            oldPointer
        );

        if (
            used_bytes_ >=
            oldBytes
        )
        {
            used_bytes_ -=
                oldBytes;
        }
        else
        {
            used_bytes_ = 0;
        }

        entry.pointer =
            replacement;

        entry.bytes = bytes;
        entry.pinned = pinned;
        entry.used = true;

        touch(entry);

        used_bytes_ += bytes;

        if (
            used_bytes_ >
            peak_bytes_
        )
        {
            peak_bytes_ =
                used_bytes_;
        }

        return replacement;
    }

    if (!makeRoom(bytes))
    {
        return nullptr;
    }

    const int slot =
        findFreeIndex();

    if (slot < 0)
    {
        return nullptr;
    }

    void* pointer =
        manager_->allocate(
            bytes,
            MemoryPurpose::Bitmap,
            alignment,
            "asset-cache"
        );

    while (!pointer)
    {
        // Under heap pressure, progressively evict optional LRU assets and
        // retry. Pinned entries are never selected.
        const int lru =
            findLRUUnpinned();

        if (lru < 0)
        {
            break;
        }

        const uint32_t lruKey =
            entries_[lru].key;

        if (!remove(lruKey))
        {
            break;
        }

        ++evictions_;

        pointer =
            manager_->allocate(
                bytes,
                MemoryPurpose::Bitmap,
                alignment,
                "asset-cache"
            );
    }

    if (!pointer)
    {
        return nullptr;
    }

    if (source)
    {
        memcpy(
            pointer,
            source,
            bytes
        );
    }

    Entry& entry =
        entries_[slot];

    entry.key = key;
    entry.pointer = pointer;
    entry.bytes = bytes;
    entry.used = true;
    entry.pinned = pinned;

    touch(entry);

    used_bytes_ += bytes;

    if (
        used_bytes_ >
        peak_bytes_
    )
    {
        peak_bytes_ =
            used_bytes_;
    }

    return pointer;
}

uint16_t* AssetCache::putRGB565(
    uint32_t key,
    const uint16_t* source,
    size_t pixelCount,
    bool pinned
)
{
    if (
        pixelCount >
        (
            SIZE_MAX /
            sizeof(uint16_t)
        )
    )
    {
        return nullptr;
    }

    return static_cast<uint16_t*>(
        put(
            key,
            source,
            pixelCount *
                sizeof(uint16_t),
            pinned,
            4
        )
    );
}

void* AssetCache::get(
    uint32_t key
)
{
    const int index =
        findIndex(key);

    if (
        index < 0 ||
        !manager_ ||
        !manager_->isReady() ||
        !manager_->owns(
            entries_[index].pointer
        )
    )
    {
        ++misses_;
        return nullptr;
    }

    Entry& entry =
        entries_[index];

    ++hits_;
    touch(entry);

    return entry.pointer;
}

const void* AssetCache::peek(
    uint32_t key
) const
{
    const int index =
        findIndex(key);

    if (
        index < 0 ||
        !manager_ ||
        !manager_->isReady() ||
        !manager_->owns(
            entries_[index].pointer
        )
    )
    {
        return nullptr;
    }

    return
        entries_[index].pointer;
}

bool AssetCache::contains(
    uint32_t key
) const
{
    return
        peek(key) != nullptr;
}

bool AssetCache::pin(
    uint32_t key
)
{
    const int index =
        findIndex(key);

    if (
        index < 0 ||
        !manager_ ||
        !manager_->isReady() ||
        !manager_->owns(
            entries_[index].pointer
        )
    )
    {
        return false;
    }

    entries_[index].pinned = true;
    touch(entries_[index]);

    return true;
}

bool AssetCache::unpin(
    uint32_t key
)
{
    const int index =
        findIndex(key);

    if (
        index < 0 ||
        !manager_ ||
        !manager_->isReady() ||
        !manager_->owns(
            entries_[index].pointer
        )
    )
    {
        return false;
    }

    entries_[index].pinned = false;
    touch(entries_[index]);

    return true;
}

bool AssetCache::remove(
    uint32_t key
)
{
    const int index =
        findIndex(key);

    if (index < 0)
    {
        return false;
    }

    Entry& entry =
        entries_[index];

    if (
        manager_ &&
        entry.pointer
    )
    {
        manager_->release(
            entry.pointer
        );
    }

    if (
        used_bytes_ >=
        entry.bytes
    )
    {
        used_bytes_ -=
            entry.bytes;
    }
    else
    {
        used_bytes_ = 0;
    }

    memset(
        &entry,
        0,
        sizeof(Entry)
    );

    return true;
}

size_t AssetCache::purgeUnpinned()
{
    size_t freed = 0;

    for (
        uint8_t i = 0;
        i < MAX_ENTRIES;
        ++i
    )
    {
        if (
            entries_[i].used &&
            !entries_[i].pinned
        )
        {
            const size_t bytes =
                entries_[i].bytes;

            const uint32_t key =
                entries_[i].key;

            if (remove(key))
            {
                freed += bytes;
                ++evictions_;
            }
        }
    }

    return freed;
}

size_t AssetCache::evictLRU(
    size_t bytesToFree
)
{
    size_t freed = 0;

    while (freed < bytesToFree)
    {
        const int index =
            findLRUUnpinned();

        if (index < 0)
        {
            break;
        }

        const size_t bytes =
            entries_[index].bytes;

        const uint32_t key =
            entries_[index].key;

        if (!remove(key))
        {
            break;
        }

        freed += bytes;
        ++evictions_;
    }

    return freed;
}

void AssetCache::clear()
{
    if (!manager_)
    {
        memset(
            entries_,
            0,
            sizeof(entries_)
        );

        used_bytes_ = 0;
        return;
    }

    for (
        uint8_t i = 0;
        i < MAX_ENTRIES;
        ++i
    )
    {
        if (entries_[i].used)
        {
            if (entries_[i].pointer)
            {
                manager_->release(
                    entries_[i].pointer
                );
            }

            memset(
                &entries_[i],
                0,
                sizeof(Entry)
            );
        }
    }

    used_bytes_ = 0;
}

void AssetCache::trimToBytes(
    size_t targetBytes
)
{
    while (
        used_bytes_ > targetBytes
    )
    {
        const int index =
            findLRUUnpinned();

        if (index < 0)
        {
            break;
        }

        const uint32_t key =
            entries_[index].key;

        if (!remove(key))
        {
            break;
        }

        ++evictions_;
    }
}

void AssetCache::trimToPercent(
    uint8_t percent
)
{
    if (percent > 100)
    {
        percent = 100;
    }

    size_t reference =
        budget_bytes_;

    if (reference == 0)
    {
        reference =
            used_bytes_;
    }

    const size_t target =
        (
            reference *
            percent
        ) /
        100;

    trimToBytes(target);
}

AssetCache::Stats AssetCache::stats() const
{
    Stats result;

    result.usedBytes =
        used_bytes_;

    result.peakBytes =
        peak_bytes_;

    result.budgetBytes =
        budget_bytes_;

    result.hits = hits_;
    result.misses = misses_;
    result.evictions =
        evictions_;

    for (
        uint8_t i = 0;
        i < MAX_ENTRIES;
        ++i
    )
    {
        if (entries_[i].used)
        {
            ++result.entryCount;

            if (entries_[i].pinned)
            {
                ++result.pinnedCount;
            }
        }
    }

    return result;
}

} // namespace NV3047Memory
