#pragma once

#include "NV3047_MemoryManager.h"
#include "NV3047_FramebufferPair.h"
#include "NV3047_DMAPool.h"
#include "NV3047_AssetCache.h"

namespace NV3047Memory
{

struct AutoMemoryConfig
{
    MemoryConfig memory;

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

    const AutoMemoryConfig& config() const;

    MemoryPressure pressure() const;
    FragmentationStats fragmentation() const;

    size_t emergencyPurge();

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

    bool ready_;
    bool framebuffer_ready_;
    bool dma_ready_;

    MemoryPressure last_pressure_;
    uint32_t pressure_actions_;

    static float fragmentationPercent(
        const HeapStats& heap
    );
};

} // namespace NV3047Memory
