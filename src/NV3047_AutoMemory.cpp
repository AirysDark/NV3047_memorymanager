#include "NV3047_AutoMemory.h"
#include "NV3047_DriverTakeover.h"

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
      pressure_actions_(0),
      driver_fixed_bytes_(0),
      driver_usage_synced_(false),
      synced_asset_revision_(0),
      performance_stats_()
{
}

bool AutoMemory::begin(
    const AutoMemoryConfig& config
)
{
    end();

    config_ = config;

    driver_fixed_bytes_ = 0;
    driver_usage_synced_ = false;
    synced_asset_revision_ = 0;
    performance_stats_ =
        AutoMemoryPerformanceStats();

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

    syncBrokerUsage(true);

    // Manager startup is sampled before broker/framebuffer/DMA allocations.
    // Refresh once after the complete automatic layout is established so the
    // cached pressure state represents the real takeover footprint.
    manager_->refreshStats(true);

    const MemoryPressure initialPressure =
        manager_->pressure();

    applyPressurePolicy(
        initialPressure,
        true
    );

    // Broker reclaim or fallback cache trimming may have changed asset
    // accounting. Publish at most once after all recovery work is complete.
    syncBrokerUsage(false);

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

    driver_fixed_bytes_ = 0;
    driver_usage_synced_ = false;
    synced_asset_revision_ = 0;
    performance_stats_ =
        AutoMemoryPerformanceStats();
}

bool AutoMemory::isReady() const
{
    return ready_;
}

void AutoMemory::beginFrame()
{
    beginFrameIfNeeded();
}

bool AutoMemory::beginFrameIfNeeded()
{
    if (!ready_)
    {
        return false;
    }

    const bool profile =
        config_.enablePerformanceProfiling;

    const uint32_t start =
        profile
            ? micros()
            : 0;

    const bool reset =
        manager_->
            beginFrameIfNeeded();

    if (profile)
    {
        const uint32_t elapsed =
            static_cast<uint32_t>(
                micros() - start
            );

        ++performance_stats_.
            beginFrameCalls;

        if (reset)
        {
            ++performance_stats_.
                beginFrameResets;
        }
        else
        {
            ++performance_stats_.
                beginFrameNoOps;
        }

        performance_stats_.
            totalBeginFrameUs +=
                elapsed;

        if (
            elapsed >
            performance_stats_.
                maxBeginFrameUs
        )
        {
            performance_stats_.
                maxBeginFrameUs =
                    elapsed;
        }
    }

    return reset;
}
bool AutoMemory::serviceDue(
    uint32_t nowMs
) const
{
    if (!ready_)
    {
        return false;
    }

    if (
        !driver_usage_synced_ ||
        assets_.usageRevision() !=
            synced_asset_revision_ ||
        manager_->pressure() !=
            last_pressure_
    )
    {
        return true;
    }

    if (
        manager_->
            serviceDue(nowMs)
    )
    {
        return true;
    }

    return
        broker_.serviceDue(nowMs);
}

void AutoMemory::service()
{
    if (!ready_)
    {
        return;
    }

    const uint32_t now =
        millis();

    const bool profile =
        config_.enablePerformanceProfiling;

    if (profile)
    {
        ++performance_stats_.
            serviceCalls;
    }

    if (!serviceDue(now))
    {
        if (profile)
        {
            ++performance_stats_.
                serviceFastExits;
        }

        return;
    }

    service(now);
}

void AutoMemory::service(
    uint32_t nowMs
)
{
    if (!ready_)
    {
        return;
    }

    const bool profile =
        config_.enablePerformanceProfiling;

    const uint32_t start =
        profile
            ? micros()
            : 0;

    if (profile)
    {
        ++performance_stats_.
            serviceFullPasses;
    }

    uint32_t samplesBefore = 0;

    if (profile)
    {
        samplesBefore =
            manager_->
                heapSampleCount();
    }

    if (
        manager_->
            serviceDue(nowMs)
    )
    {
        manager_->
            service(nowMs);
    }

    if (
        profile &&
        manager_->heapSampleCount() !=
            samplesBefore
    )
    {
        ++performance_stats_.
            heapSamplePasses;
    }

    // This is revision-gated before any broker lock/stat work.
    syncBrokerUsage(false);

    if (
        broker_.
            serviceDue(nowMs)
    )
    {
        if (profile)
        {
            ++performance_stats_.
                brokerServiceDuePasses;
        }

        broker_.service(nowMs);
    }
    else if (
        profile &&
        broker_.isReady()
    )
    {
        ++performance_stats_.
            brokerServiceSkipped;
    }

    const MemoryPressure current =
        manager_->pressure();

    const bool stateChanged =
        current !=
            last_pressure_;

    const bool policyActed =
        applyPressurePolicy(
            current,
            stateChanged
        );

    // Broker reclaim or fallback pressure policy can mutate asset accounting.
    // Only enter the broker update path if its revision actually changed.
    if (
        assets_.usageRevision() !=
            synced_asset_revision_
    )
    {
        syncBrokerUsage(false);
    }

    // If a policy action released heap memory the manager revision is now
    // dirty. Do not force a second heap sample into this same frame; the next
    // serviceDue() call will immediately schedule it.
    last_pressure_ =
        current;

    if (
        profile &&
        policyActed
    )
    {
        ++performance_stats_.
            pressurePolicyActions;
    }

    if (profile)
    {
        const uint32_t elapsed =
            static_cast<uint32_t>(
                micros() - start
            );

        performance_stats_.
            totalServiceUs +=
                elapsed;

        if (
            elapsed >
            performance_stats_.
                maxServiceUs
        )
        {
            performance_stats_.
                maxServiceUs =
                    elapsed;
        }
    }
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

    driver_fixed_bytes_ = 0;

    if (framebuffer_ready_)
    {
        driver_fixed_bytes_ =
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
                    driver_fixed_bytes_
            )
            {
                driver_fixed_bytes_ +=
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
        driver_fixed_bytes_;

    driverConfig.softLimitBytes =
        driver_fixed_bytes_;

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
        driver_fixed_bytes_,
        0
    );

    driver_usage_synced_ = true;

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

bool AutoMemory::syncBrokerUsage(
    bool force
)
{
    const bool driverNeedsSync =
        driver_client_ !=
            INVALID_BROKER_CLIENT &&
        (
            force ||
            !driver_usage_synced_
        );

    const uint32_t assetRevision =
        assets_.usageRevision();

    const bool assetNeedsSync =
        asset_client_ !=
            INVALID_BROKER_CLIENT &&
        (
            force ||
            assetRevision !=
                synced_asset_revision_
        );

    if (
        !driverNeedsSync &&
        !assetNeedsSync
    )
    {
        if (
            config_.
                enablePerformanceProfiling
        )
        {
            ++performance_stats_.
                brokerUsageSyncSkips;
        }

        return false;
    }

    if (!broker_.isReady())
    {
        return false;
    }

    bool changed = false;

    if (driverNeedsSync)
    {
        broker_.setObservedUsage(
            driver_client_,
            driver_fixed_bytes_,
            0
        );

        driver_usage_synced_ = true;
        changed = true;
    }

    if (assetNeedsSync)
    {
        const AssetCache::Stats cache =
            assets_.stats();

        broker_.setObservedUsage(
            asset_client_,
            cache.usedBytes,
            cache.reclaimableBytes,
            MemoryRegion::PSRAM
        );

        synced_asset_revision_ =
            assetRevision;

        changed = true;

        if (
            config_.
                enablePerformanceProfiling
        )
        {
            ++performance_stats_.
                assetUsageSyncs;
        }
    }

    if (
        changed &&
        config_.
            enablePerformanceProfiling
    )
    {
        ++performance_stats_.
            brokerUsageSyncs;
    }

    return changed;
}
bool AutoMemory::applyPressurePolicy(
    MemoryPressure current,
    bool stateChanged
)
{
    bool acted = false;

    // With the broker enabled, MemoryBroker::service() is the sole owner of
    // Warning/Critical donor reclamation. This avoids duplicate reclaim scans
    // in one frame. The legacy cache-only policy remains for broker-disabled
    // configurations.
    if (!broker_.isReady())
    {
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
                    assets_.trimToBytes(
                        target
                    );
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

                assets_.trimToBytes(
                    target
                );
            }

            const AssetCache::Stats after =
                assets_.stats();

            acted =
                after.usedBytes <
                    before.usedBytes;
        }
        else if (
            current ==
                MemoryPressure::Critical &&
            config_.
                purgeUnpinnedOnCritical
        )
        {
            acted =
                assets_.purgeUnpinned() >
                    0;
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

    return acted;
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

    syncBrokerUsage(true);

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

    syncBrokerUsage(false);

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

    if (framebuffer_ready_)
    {
        if (!framebuffers_.isReady())
        {
            return false;
        }

        const uint16_t* front =
            framebuffers_.front();

        const uint16_t* back =
            framebuffers_.back();

        if (
            !front ||
            !back ||
            !manager_->owns(front) ||
            !manager_->owns(back)
        )
        {
            return false;
        }
    }

    if (
        dma_ready_ &&
        !dma_pool_.validate()
    )
    {
        return false;
    }

    return true;
}

void AutoMemory::setPerformanceProfiling(
    bool enabled,
    bool reset
)
{
    config_.enablePerformanceProfiling =
        enabled;

    nv3047_memorymanager_takeover_profiling_changed(
        enabled
    );

    if (reset)
    {
        resetPerformanceStats();
    }
}

bool AutoMemory::performanceProfilingEnabled() const
{
    return
        config_.
            enablePerformanceProfiling;
}

AutoMemoryPerformanceStats
AutoMemory::performanceStats() const
{
    return performance_stats_;
}

void AutoMemory::resetPerformanceStats()
{
    performance_stats_ =
        AutoMemoryPerformanceStats();

    resetDriverTakeoverPerformanceStats();
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
