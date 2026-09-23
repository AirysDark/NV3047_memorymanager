#pragma once

#include "NV3047_MemoryManager.h"
#include <stddef.h>
#include <stdint.h>

namespace NV3047Memory
{

class AssetCache
{
public:
    static constexpr uint8_t MAX_ENTRIES = 32;

    struct Stats
    {
        size_t usedBytes = 0;
        size_t peakBytes = 0;
        size_t budgetBytes = 0;
        size_t pinnedBytes = 0;
        size_t reclaimableBytes = 0;

        uint8_t entryCount = 0;
        uint8_t pinnedCount = 0;

        uint32_t hits = 0;
        uint32_t misses = 0;
        uint32_t evictions = 0;
    };

    AssetCache();
    ~AssetCache();

    AssetCache(const AssetCache&) = delete;
    AssetCache& operator=(const AssetCache&) = delete;

    bool begin(
        size_t budgetBytes = 0,
        MemoryManager* manager = nullptr
    );

    void end();

    void* put(
        uint32_t key,
        const void* source,
        size_t bytes,
        bool pinned = false,
        size_t alignment = 4
    );

    uint16_t* putRGB565(
        uint32_t key,
        const uint16_t* source,
        size_t pixelCount,
        bool pinned = false
    );

    void* get(uint32_t key);
    const void* peek(uint32_t key) const;

    bool contains(uint32_t key) const;

    bool pin(uint32_t key);
    bool unpin(uint32_t key);

    bool remove(uint32_t key);

    size_t purgeUnpinned();
    size_t evictLRU(size_t bytesToFree);

    void clear();

    void trimToBytes(size_t targetBytes);
    void trimToPercent(uint8_t percent);

    // O(1) snapshot. Accounting is maintained when entries change instead
    // of rescanning the cache on every broker service pass.
    Stats stats() const;

    inline uint32_t usageRevision() const
    {
        return usage_revision_;
    }

    bool validate() const;

private:
    struct Entry
    {
        uint32_t key;
        void* pointer;
        size_t bytes;
        uint32_t lastUse;
        bool used;
        bool pinned;
    };

    MemoryManager* manager_;

    Entry entries_[MAX_ENTRIES];

    size_t budget_bytes_;
    size_t used_bytes_;
    size_t peak_bytes_;
    size_t pinned_bytes_;
    size_t reclaimable_bytes_;

    uint8_t entry_count_;
    uint8_t pinned_count_;

    // Changes only when cache byte/pin accounting changes. Reads/touches do
    // not advance it, allowing AutoMemory to skip redundant broker syncs.
    uint32_t usage_revision_;

    uint32_t hits_;
    uint32_t misses_;
    uint32_t evictions_;
    uint32_t use_counter_;

    int findIndex(uint32_t key) const;
    int findFreeIndex() const;
    int findLRUUnpinned(
        int excludeIndex = -1
    ) const;

    bool makeRoom(size_t bytes);
    void touch(Entry& entry);

    void accountAdd(const Entry& entry);
    void accountRemove(const Entry& entry);
    void accountPinChange(
        Entry& entry,
        bool pinned
    );
    void noteUsageChange();
};

} // namespace NV3047Memory
