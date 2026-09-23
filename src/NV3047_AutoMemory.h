#pragma once

#include "NV3047_MemoryManager.h"
#include "NV3047_FramebufferPair.h"
#include "NV3047_DMAPool.h"
#include "NV3047_AssetCache.h"
#include "NV3047_MemoryBroker.h"

namespace NV3047Memory
{

struct AutoMemoryConfig
{
    MemoryConfig memory;
    BrokerConfig broker;

    bool enableBroker = true;

    // Soft budgets are elastic guidance, not fixed partitions.
    size_t uiSoftBudgetBytes = 512 * 1024;
    size_t applicationSoftBudgetBytes = 512 * 1024;

    bool allocateFramebufferPair = true;

    // Matches driver_overhaul_v2 production defaults.
    uint16_t framebufferWidth = 480;
    uint16_t framebufferHeight = 272;

    bool enableDMAPool = true;

    // driver_overhaul_v2 uses one persistent 10-line RGB565
    // DMA-capable fill buffer: 480 * 10 * 2 = 9600 bytes.
    size_t dmaBlockBytes = 480 * 10 * sizeof(uint16_t);
    uint8_t dmaBlockCount = 1;
    size_t dmaAlignment = 4;

    // 0 means no hard cache byte budget.
    size_t assetCacheBudgetBytes =
        512 * 1024;

    // Automatic pressure actions.
    uint8_t warningCachePercent = 75;
    bool purgeUnpinnedOnCritical = true;
    bool resetScratchOnCritical = true;

    // Disabled by default so profiling itself has zero micros() overhead in
    // production. Enable only while measuring the manager hot path.
    bool enablePerformanceProfiling = false;
};

struct AutoMemoryPerformanceStats
{
    uint32_t beginFrameCalls = 0;
    uint32_t serviceCalls = 0;
    uint32_t heapSamplePasses = 0;
    uint32_t brokerUsageSyncs = 0;
    uint32_t assetUsageSyncs = 0;

    uint64_t totalBeginFrameUs = 0;
    uint32_t maxBeginFrameUs = 0;

    uint64_t totalServiceUs = 0;
    uint32_t maxServiceUs = 0;
};

struct FragmentationStats
{
    float internalPercent = 0.0f;
    float psramPercent = 0.0f;
};

class AutoMemory
{
public:
    static AutoMemory& instance();

    AutoMemory(const AutoMemory&) = delete;
    AutoMemory& operator=(const AutoMemory&) = delete;

    bool begin(
        const AutoMemoryConfig& config =
            AutoMemoryConfig()
    );

    void end();

    bool isReady() const;

    void beginFrame();
    void service();

    MemoryManager& memory();
    FramebufferPair& framebuffers();
    DMAPool& dmaPool();
    AssetCache& assets();
    MemoryBroker& broker();

    BrokerClientId driverClient() const;
    BrokerClientId uiClient() const;
    BrokerClientId assetClient() const;
    BrokerClientId applicationClient() const;

    bool noteUIActivity();
    bool noteApplicationActivity();

    static size_t staticControlBytes();
    size_t totalPermanentControlBytes() const;

    const AutoMemoryConfig& config() const;

    MemoryPressure pressure() const;
    FragmentationStats fragmentation() const;

    size_t emergencyPurge();

    bool validate() const;

    AutoMemoryPerformanceStats
        performanceStats() const;

    void resetPerformanceStats();

    void dump(
        Stream& output = Serial
    ) const;

private:
    AutoMemory();

    AutoMemoryConfig config_;

    MemoryManager* manager_;

    FramebufferPair framebuffers_;
    DMAPool dma_pool_;
    AssetCache assets_;
    MemoryBroker broker_;

    BrokerClientId driver_client_;
    BrokerClientId ui_client_;
    BrokerClientId asset_client_;
    BrokerClientId application_client_;

    bool ready_;
    bool framebuffer_ready_;
    bool dma_ready_;

    MemoryPressure last_pressure_;
    uint32_t pressure_actions_;

    size_t driver_fixed_bytes_;
    bool driver_usage_synced_;
    uint32_t synced_asset_revision_;

    AutoMemoryPerformanceStats
        performance_stats_;

    void applyPressurePolicy(
        MemoryPressure current,
        bool stateChanged
    );

    bool registerBrokerClients();
    bool syncBrokerUsage(
        bool force = false
    );

    static size_t reclaimAssets(
        void* userData,
        size_t targetBytes,
        BrokerReclaimReason reason
    );

    static float fragmentationPercent(
        const HeapStats& heap
    );
};

} // namespace NV3047Memory
