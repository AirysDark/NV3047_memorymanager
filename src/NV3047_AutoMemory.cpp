#include "NV3047_AutoMemory.h"

namespace NV3047Memory
{

AutoMemory& AutoMemory::instance()
{
    static AutoMemory automatic;
    return automatic;
}

AutoMemory::AutoMemory()
    : manager_(
          &MemoryManager::instance()
      ),
      ready_(false),
      framebuffer_ready_(false),
      dma_ready_(false),
      last_pressure_(
          MemoryPressure::Normal
      ),
      pressure_actions_(0)
{
}

bool AutoMemory::begin(
    const AutoMemoryConfig& config
)
{
    end();

    config_ = config;

    manager_ =
        &MemoryManager::instance();

    if (
        !manager_->begin(
            config_.memory
        )
    )
    {
        return false;
    }

    if (
        !assets_.begin(
            config_.assetCacheBudgetBytes,
            manager_
        )
    )
    {
        manager_->end();
        return false;
    }

    if (
        config_.allocateFramebufferPair
    )
    {
        framebuffer_ready_ =
            framebuffers_.begin(
                config_.framebufferWidth,
                config_.framebufferHeight,
                manager_
            );

        if (!framebuffer_ready_)
        {
            assets_.end();
            manager_->end();
            return false;
        }
    }

    if (config_.enableDMAPool)
    {
        dma_ready_ =
            dma_pool_.begin(
                config_.dmaBlockBytes,
                config_.dmaBlockCount,
                manager_,
                config_.dmaAlignment
            );

        if (!dma_ready_)
        {
            framebuffers_.end();
            assets_.end();
            manager_->end();
            return false;
        }
    }

    ready_ = true;

    const MemoryPressure initialPressure =
        manager_->pressure();

    applyPressurePolicy(
        initialPressure,
        true
    );

    last_pressure_ =
        manager_->pressure();

    return true;
}

void AutoMemory::end()
{
    if (dma_ready_)
    {
        dma_pool_.end();
    }

    if (framebuffer_ready_)
    {
        framebuffers_.end();
    }

    assets_.end();

    dma_ready_ = false;
    framebuffer_ready_ = false;
    ready_ = false;

    if (
        manager_ &&
        manager_->isReady()
    )
    {
        manager_->end();
    }

    last_pressure_ =
        MemoryPressure::Normal;

    pressure_actions_ = 0;
}

bool AutoMemory::isReady() const
{
    return ready_;
}

void AutoMemory::beginFrame()
{
    if (!ready_)
    {
        return;
    }

    manager_->beginFrame();
}

void AutoMemory::service()
{
    if (!ready_)
    {
        return;
    }

    manager_->service();

    const MemoryPressure current =
        manager_->pressure();

    const bool stateChanged =
        current != last_pressure_;

    applyPressurePolicy(
        current,
        stateChanged
    );

    // Recovery actions can change the pressure state immediately.
    last_pressure_ =
        manager_->pressure();
}

MemoryManager& AutoMemory::memory()
{
    return *manager_;
}

FramebufferPair& AutoMemory::framebuffers()
{
    return framebuffers_;
}

DMAPool& AutoMemory::dmaPool()
{
    return dma_pool_;
}

AssetCache& AutoMemory::assets()
{
    return assets_;
}

const AutoMemoryConfig& AutoMemory::config() const
{
    return config_;
}

MemoryPressure AutoMemory::pressure() const
{
    if (!manager_)
    {
        return
            MemoryPressure::Critical;
    }

    return manager_->pressure();
}

void AutoMemory::applyPressurePolicy(
    MemoryPressure current,
    bool stateChanged
)
{
    bool acted = false;

    if (
        current ==
        MemoryPressure::Warning
    )
    {
        const AssetCache::Stats before =
            assets_.stats();

        const uint8_t percent =
            config_.warningCachePercent > 100
                ? 100
                : config_.warningCachePercent;

        // A fixed cache budget has a stable warning target and can be
        // continuously enforced. With no hard budget, trim only when
        // entering Warning so repeated service() calls do not exponentially
        // shrink the cache toward zero.
        if (before.budgetBytes != 0)
        {
            const size_t target =
                (
                    before.budgetBytes *
                    percent
                ) /
                100;

            if (before.usedBytes > target)
            {
                assets_.trimToBytes(target);
            }
        }
        else if (
            stateChanged &&
            before.usedBytes != 0
        )
        {
            const size_t target =
                (
                    before.usedBytes *
                    percent
                ) /
                100;

            assets_.trimToBytes(target);
        }

        const AssetCache::Stats after =
            assets_.stats();

        acted =
            after.usedBytes <
            before.usedBytes;
    }
    else if (
        current ==
        MemoryPressure::Critical
    )
    {
        if (
            config_.
                purgeUnpinnedOnCritical
        )
        {
            acted =
                assets_.purgeUnpinned() > 0 ||
                acted;
        }

        // Reset scratch once on entry to Critical. beginFrame()
        // already recycles it on following frames.
        if (
            config_.
                resetScratchOnCritical &&
            stateChanged
        )
        {
            if (
                manager_->
                    scratchUsed() > 0
            )
            {
                acted = true;
            }

            manager_->
                resetScratch();
        }
    }

    if (acted)
    {
        ++pressure_actions_;
    }
}

float AutoMemory::fragmentationPercent(
    const HeapStats& heap
)
{
    if (
        heap.freeBytes == 0 ||
        heap.largestFreeBlock >=
            heap.freeBytes
    )
    {
        return 0.0f;
    }

    const float largest =
        static_cast<float>(
            heap.largestFreeBlock
        );

    const float freeBytes =
        static_cast<float>(
            heap.freeBytes
        );

    return
        100.0f *
        (
            1.0f -
            (
                largest /
                freeBytes
            )
        );
}

FragmentationStats AutoMemory::fragmentation() const
{
    FragmentationStats result;

    if (!manager_)
    {
        return result;
    }

    const MemoryStats stats =
        manager_->getStats();

    result.internalPercent =
        fragmentationPercent(
            stats.internal
        );

    if (stats.psramAvailable)
    {
        result.psramPercent =
            fragmentationPercent(
                stats.psram
            );
    }

    return result;
}

size_t AutoMemory::emergencyPurge()
{
    if (!ready_)
    {
        return 0;
    }

    const size_t freed =
        assets_.purgeUnpinned();

    manager_->resetScratch();

    ++pressure_actions_;

    return freed;
}

void AutoMemory::dump(
    Stream& output
) const
{
    output.println();
    output.println(
        "=== NV3047 Auto Memory ==="
    );

    if (!ready_)
    {
        output.println("Not ready");
        return;
    }

    manager_->dump(output);

    const AssetCache::Stats cache =
        assets_.stats();

    output.print(
        "Asset cache: "
    );
    output.print(cache.usedBytes);
    output.print('/');
    output.println(
        cache.budgetBytes
    );

    output.print(
        "Asset entries: "
    );
    output.print(cache.entryCount);
    output.print(
        " pinned="
    );
    output.println(
        cache.pinnedCount
    );

    output.print(
        "Cache hit/miss/evict: "
    );
    output.print(cache.hits);
    output.print('/');
    output.print(cache.misses);
    output.print('/');
    output.println(
        cache.evictions
    );

    output.print(
        "Framebuffer pair: "
    );
    output.println(
        framebuffer_ready_
            ? "ready"
            : "disabled"
    );

    if (framebuffer_ready_)
    {
        output.print(
            "Framebuffer bytes: "
        );
        output.println(
            framebuffers_.
                totalBytes()
        );
    }

    output.print(
        "DMA pool: "
    );
    output.println(
        dma_ready_
            ? "ready"
            : "disabled"
    );

    if (dma_ready_)
    {
        output.print(
            "DMA blocks free/total: "
        );
        output.print(
            dma_pool_.freeCount()
        );
        output.print('/');
        output.println(
            dma_pool_.blockCount()
        );
    }

    const FragmentationStats frag =
        fragmentation();

    output.print(
        "Internal fragmentation %: "
    );
    output.println(
        frag.internalPercent,
        1
    );

    output.print(
        "PSRAM fragmentation %: "
    );
    output.println(
        frag.psramPercent,
        1
    );

    output.print(
        "Automatic pressure actions: "
    );
    output.println(
        pressure_actions_
    );
}

} // namespace NV3047Memory
