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
      driver_client_(
          INVALID_BROKER_CLIENT
      ),
      ui_client_(
          INVALID_BROKER_CLIENT
      ),
      asset_client_(
          INVALID_BROKER_CLIENT
      ),
      application_client_(
          INVALID_BROKER_CLIENT
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

    if (config_.enableBroker)
    {
        if (
            !broker_.begin(
                manager_,
                config_.broker
            )
        )
        {
            manager_->end();
            return false;
        }
    }

    if (
        !assets_.begin(
            config_.assetCacheBudgetBytes,
            manager_
        )
    )
    {
        broker_.end();
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
            broker_.end();
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
            broker_.end();
            manager_->end();
            return false;
        }
    }

    if (
        broker_.isReady() &&
        !registerBrokerClients()
    )
    {
        dma_pool_.end();
        framebuffers_.end();
        assets_.end();
        broker_.end();
        manager_->end();

        dma_ready_ = false;
        framebuffer_ready_ = false;

        return false;
    }

    ready_ = true;

    syncBrokerUsage();

    const MemoryPressure initialPressure =
        manager_->pressure();

    applyPressurePolicy(
        initialPressure,
        true
    );

    syncBrokerUsage();

    last_pressure_ =
        manager_->pressure();

    return true;
}

void AutoMemory::end()
{
    ready_ = false;

    if (broker_.isReady())
    {
        broker_.end();
    }

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

    driver_client_ =
        INVALID_BROKER_CLIENT;

    ui_client_ =
        INVALID_BROKER_CLIENT;

    asset_client_ =
        INVALID_BROKER_CLIENT;

    application_client_ =
        INVALID_BROKER_CLIENT;

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

    syncBrokerUsage();

    if (broker_.isReady())
    {
        broker_.service();

        // A reclaim callback can change cache usage immediately.
        syncBrokerUsage();
    }

    const MemoryPressure current =
        manager_->pressure();

    const bool stateChanged =
        current != last_pressure_;

    applyPressurePolicy(
        current,
        stateChanged
    );

    syncBrokerUsage();

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

MemoryBroker& AutoMemory::broker()
{
    return broker_;
}

BrokerClientId AutoMemory::driverClient() const
{
    return driver_client_;
}

BrokerClientId AutoMemory::uiClient() const
{
    return ui_client_;
}

BrokerClientId AutoMemory::assetClient() const
{
    return asset_client_;
}

BrokerClientId AutoMemory::applicationClient() const
{
    return application_client_;
}

bool AutoMemory::noteUIActivity()
{
    if (
        !broker_.isReady() ||
        ui_client_ ==
            INVALID_BROKER_CLIENT
    )
    {
        return false;
    }

    const bool uiActive =
        broker_.noteActivity(
            ui_client_
        );

    // UI assets follow UI activity. While the UI is actively being used,
    // its cached assets have the same importance as the UI itself. Once the
    // UI goes idle, those assets decay to a donor state automatically.
    if (
        asset_client_ !=
        INVALID_BROKER_CLIENT
    )
    {
        broker_.noteActivity(
            asset_client_
        );
    }

    return uiActive;
}

bool AutoMemory::noteApplicationActivity()
{
    return
        broker_.isReady() &&
        application_client_ !=
            INVALID_BROKER_CLIENT &&
        broker_.noteActivity(
            application_client_
        );
}

size_t AutoMemory::staticControlBytes()
{
    return
        sizeof(MemoryManager) +
        sizeof(AutoMemory);
}

size_t AutoMemory::totalPermanentControlBytes() const
{
    const BrokerStats brokerStats =
        broker_.stats();

    return
        staticControlBytes() +
        brokerStats.
            permanentReservedBytes;
}

const AutoMemoryConfig& AutoMemory::config() const
{
    return config_;
}

MemoryPressure AutoMemory::pressure() const
{
    if (
        !ready_ ||
        !manager_ ||
        !manager_->isReady()
    )
    {
        return
            MemoryPressure::Critical;
    }

    return manager_->pressure();
}

size_t AutoMemory::reclaimAssets(
    void* userData,
    size_t targetBytes,
    BrokerReclaimReason reason
)
{
    AssetCache* cache =
        static_cast<AssetCache*>(
            userData
        );

    if (
        !cache ||
        targetBytes == 0
    )
    {
        return 0;
    }

    if (
        reason ==
        BrokerReclaimReason::
            CriticalPressure
    )
    {
        const AssetCache::Stats stats =
            cache->stats();

        if (
            targetBytes >=
            stats.reclaimableBytes
        )
        {
            return
                cache->purgeUnpinned();
        }
    }

    return
        cache->evictLRU(
            targetBytes
        );
}

bool AutoMemory::registerBrokerClients()
{
    if (!broker_.isReady())
    {
        return true;
    }

    size_t driverFixedBytes = 0;

    if (framebuffer_ready_)
    {
        driverFixedBytes =
            framebuffers_.totalBytes();
    }

    if (dma_ready_)
    {
        const size_t dmaBytes =
            dma_pool_.blockBytes();

        const size_t dmaCount =
            dma_pool_.blockCount();

        if (
            dmaCount != 0 &&
            dmaBytes <=
                (
                    SIZE_MAX /
                    dmaCount
                )
        )
        {
            const size_t totalDMA =
                dmaBytes *
                dmaCount;

            if (
                totalDMA <=
                SIZE_MAX -
                    driverFixedBytes
            )
            {
                driverFixedBytes +=
                    totalDMA;
            }
        }
    }

    BrokerClientConfig driverConfig;

    driverConfig.name =
        "driver-fixed";

    driverConfig.priority =
        BrokerPriority::Critical;

    driverConfig.minimumBytes =
        driverFixedBytes;

    driverConfig.softLimitBytes =
        driverFixedBytes;

    driverConfig.backgroundAfterMs = 0;
    driverConfig.idleAfterMs = 0;

    driver_client_ =
        broker_.registerClient(
            driverConfig
        );

    if (
        driver_client_ ==
        INVALID_BROKER_CLIENT
    )
    {
        return false;
    }

    broker_.setActivity(
        driver_client_,
        BrokerActivity::Active
    );

    broker_.setObservedUsage(
        driver_client_,
        driverFixedBytes,
        0
    );

    BrokerClientConfig uiConfig;

    uiConfig.name =
        "ui";

    uiConfig.priority =
        BrokerPriority::Normal;

    uiConfig.softLimitBytes =
        config_.uiSoftBudgetBytes;

    ui_client_ =
        broker_.registerClient(
            uiConfig
        );

    if (
        ui_client_ ==
        INVALID_BROKER_CLIENT
    )
    {
        return false;
    }

    BrokerClientConfig assetConfig;

    assetConfig.name =
        "assets";

    assetConfig.priority =
        BrokerPriority::Normal;

    assetConfig.softLimitBytes =
        config_.assetCacheBudgetBytes;

    assetConfig.reclaim =
        &AutoMemory::reclaimAssets;

    assetConfig.userData =
        &assets_;

    asset_client_ =
        broker_.registerClient(
            assetConfig
        );

    if (
        asset_client_ ==
        INVALID_BROKER_CLIENT
    )
    {
        return false;
    }

    broker_.setActivity(
        asset_client_,
        BrokerActivity::Background
    );

    BrokerClientConfig applicationConfig;

    applicationConfig.name =
        "application";

    applicationConfig.priority =
        BrokerPriority::Normal;

    applicationConfig.softLimitBytes =
        config_.
            applicationSoftBudgetBytes;

    application_client_ =
        broker_.registerClient(
            applicationConfig
        );

    return
        application_client_ !=
        INVALID_BROKER_CLIENT;
}

void AutoMemory::syncBrokerUsage()
{
    if (!broker_.isReady())
    {
        return;
    }

    if (
        driver_client_ !=
        INVALID_BROKER_CLIENT
    )
    {
        size_t driverFixedBytes = 0;

        if (framebuffer_ready_)
        {
            driverFixedBytes =
                framebuffers_.totalBytes();
        }

        if (dma_ready_)
        {
            const size_t dmaBytes =
                dma_pool_.blockBytes();

            const size_t dmaCount =
                dma_pool_.blockCount();

            if (
                dmaCount != 0 &&
                dmaBytes <=
                    (
                        SIZE_MAX /
                        dmaCount
                    )
            )
            {
                const size_t totalDMA =
                    dmaBytes *
                    dmaCount;

                if (
                    totalDMA <=
                    SIZE_MAX -
                        driverFixedBytes
                )
                {
                    driverFixedBytes +=
                        totalDMA;
                }
            }
        }

        broker_.setObservedUsage(
            driver_client_,
            driverFixedBytes,
            0
        );
    }

    if (
        asset_client_ !=
        INVALID_BROKER_CLIENT
    )
    {
        const AssetCache::Stats cache =
            assets_.stats();

        broker_.setObservedUsage(
            asset_client_,
            cache.usedBytes,
            cache.reclaimableBytes,
            MemoryRegion::PSRAM
        );
    }
}

void AutoMemory::applyPressurePolicy(
    MemoryPressure current,
    bool stateChanged
)
{
    bool acted = false;

    if (broker_.isReady())
    {
        if (
            stateChanged &&
            current ==
                MemoryPressure::Warning
        )
        {
            const BrokerStats stats =
                broker_.stats();

            size_t target =
                (
                    stats.reclaimableBytes *
                    config_.broker.
                        warningReclaimPercent
                ) /
                100;

            if (
                target != 0 &&
                broker_.reclaimFor(
                    INVALID_BROKER_CLIENT,
                    target,
                    BrokerReclaimReason::
                        WarningPressure
                ) > 0
            )
            {
                acted = true;
            }
        }
        else if (
            stateChanged &&
            current ==
                MemoryPressure::Critical
        )
        {
            const BrokerStats stats =
                broker_.stats();

            if (
                stats.reclaimableBytes != 0 &&
                broker_.reclaimFor(
                    INVALID_BROKER_CLIENT,
                    stats.reclaimableBytes,
                    BrokerReclaimReason::
                        CriticalPressure
                ) > 0
            )
            {
                acted = true;
            }
        }
    }
    else if (
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
    }

    // Scratch memory is transient. Recycling its contents is safe, but the
    // arena itself stays available for the next frame.
    if (
        current ==
            MemoryPressure::Critical &&
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

    if (
        !ready_ ||
        !manager_ ||
        !manager_->isReady()
    )
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

    size_t freed = 0;

    syncBrokerUsage();

    if (broker_.isReady())
    {
        const BrokerStats stats =
            broker_.stats();

        if (
            stats.reclaimableBytes != 0
        )
        {
            freed =
                broker_.reclaimFor(
                    INVALID_BROKER_CLIENT,
                    stats.reclaimableBytes,
                    BrokerReclaimReason::
                        CriticalPressure
                );
        }
    }
    else
    {
        freed =
            assets_.purgeUnpinned();
    }

    manager_->resetScratch();

    if (freed != 0)
    {
        ++pressure_actions_;
    }

    syncBrokerUsage();

    return freed;
}

bool AutoMemory::validate() const
{
    if (
        !ready_ ||
        !manager_ ||
        !manager_->isReady() ||
        !manager_->validate() ||
        !assets_.validate()
    )
    {
        return false;
    }

    if (
        broker_.isReady() &&
        !broker_.validate()
    )
    {
        return false;
    }

    if (
        framebuffer_ready_ &&
        !framebuffers_.isReady()
    )
    {
        return false;
    }

    if (
        dma_ready_ &&
        (
            dma_pool_.blockCount() == 0 ||
            (
                dma_pool_.usedCount() +
                dma_pool_.freeCount()
            ) !=
                dma_pool_.blockCount()
        )
    )
    {
        return false;
    }

    return true;
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

    output.print(
        "Static control bytes: "
    );

    output.println(
        staticControlBytes()
    );

    output.print(
        "Total permanent control bytes: "
    );

    output.println(
        totalPermanentControlBytes()
    );

    if (broker_.isReady())
    {
        broker_.dump(output);
    }

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
        "Asset pinned/reclaimable: "
    );
    output.print(
        cache.pinnedBytes
    );
    output.print('/');
    output.println(
        cache.reclaimableBytes
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
