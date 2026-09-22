#include <Arduino.h>
#define NV3047_MEMORY_DISABLE_AUTORUNTIME
#include <NV3047_Memory.h>

using namespace NV3047Memory;

AutoMemory& automaticMemory =
    AutoMemory::instance();

struct DemoRuntimeObject
{
    int value;

    explicit DemoRuntimeObject(
        int initialValue
    )
        : value(initialValue)
    {
    }
};

// ui-overhaul-v2 already uses a fixed 40-slot Screen widget table.
// This pool demonstrates optional dynamically-created application objects.
ObjectPool<DemoRuntimeObject, 8>
    runtimeObjectPool;

ManagedBuffer<uint8_t>
    managedBytes;

ElasticBuffer<uint8_t>
    uiElastic;

ElasticBuffer<uint8_t>
    applicationElastic;

static const uint16_t DEMO_ICON[16] =
{
    0xF800, 0xF800, 0x001F, 0x001F,
    0xF800, 0xFFFF, 0xFFFF, 0x001F,
    0x07E0, 0xFFFF, 0xFFFF, 0x07E0,
    0x07E0, 0x07E0, 0xFFFF, 0xFFFF
};

void setup()
{
    Serial.begin(115200);
    delay(500);

    AutoMemoryConfig config;

    config.memory.internalReserveBytes =
        48 * 1024;

    config.memory.psramReserveBytes =
        128 * 1024;

    config.memory.scratchBytes =
        64 * 1024;

    // Hardened NV3047 defaults: keep framebuffers in PSRAM and do
    // not let bitmap/scratch pressure spill into internal RAM.
    config.memory.requirePSRAMForFramebuffer =
        true;

    config.memory.allowBitmapFallback =
        false;

    config.memory.allowScratchFallback =
        false;

    // Broker permanent control storage is auto-sized from its actual
    // compiled tables, then given 2 KiB of expansion headroom.
    config.enableBroker = true;

    config.broker.permanentArenaBytes = 0;
    config.broker.permanentHeadroomBytes =
        2 * 1024;

    config.uiSoftBudgetBytes =
        512 * 1024;

    config.applicationSoftBudgetBytes =
        512 * 1024;

    // Matches driver_overhaul_v2:
    // 2 x 480x272 RGB565, 64-byte aligned in PSRAM.
    config.allocateFramebufferPair =
        true;

    config.framebufferWidth = 480;
    config.framebufferHeight = 272;

    config.enableDMAPool = true;

    // Matches driver_overhaul_v2 DisplayDriver:
    // one persistent 10-line RGB565 DMA fill buffer.
    config.dmaBlockBytes =
        480 * 10 * sizeof(uint16_t);

    config.dmaBlockCount = 1;

    config.assetCacheBudgetBytes =
        512 * 1024;

    if (!automaticMemory.begin(config))
    {
        Serial.println(
            "Auto memory startup failed"
        );

        return;
    }

    MemoryManager& memory =
        automaticMemory.memory();

    if (
        managedBytes.allocate(
            256,
            MemoryPurpose::General,
            "demo-managed-buffer"
        )
    )
    {
        Serial.println(
            "ManagedBuffer ready"
        );
    }

    MemoryBroker& broker =
        automaticMemory.broker();

    if (broker.isReady())
    {
        Serial.print(
            "Broker exact permanent bytes: "
        );

        Serial.println(
            MemoryBroker::
                requiredPermanentBytes()
        );

        Serial.print(
            "Broker reserved with headroom: "
        );

        Serial.println(
            broker.stats().
                permanentReservedBytes
        );

        // Add reclaimable PSRAM assets. These may be evicted automatically
        // if a more important active workload needs the space.
        automaticMemory.assets().put(
            0x2001,
            nullptr,
            64 * 1024,
            false,
            4
        );

        automaticMemory.assets().put(
            0x2002,
            nullptr,
            64 * 1024,
            false,
            4
        );

        automaticMemory.noteUIActivity();

        uiElastic.begin(
            &broker,
            automaticMemory.uiClient(),
            96 * 1024,
            MemoryPurpose::General,
            "demo-ui-elastic"
        );

        applicationElastic.begin(
            &broker,
            automaticMemory.applicationClient(),
            128 * 1024,
            MemoryPurpose::General,
            "demo-app-elastic"
        );

        // Simulate the UI going idle while application work becomes active.
        // The broker is now allowed to reclaim the UI elastic working set.
        broker.setActivity(
            automaticMemory.uiClient(),
            BrokerActivity::Idle
        );

        automaticMemory.noteApplicationActivity();

        broker.reclaimFor(
            automaticMemory.applicationClient(),
            64 * 1024,
            BrokerReclaimReason::Request
        );

        Serial.print(
            "UI elastic resident after app handoff: "
        );

        Serial.println(
            uiElastic.resident()
                ? "yes"
                : "no"
        );

        // UI becomes active again. ensure() recreates its working set if the
        // broker reclaimed it while the application was more important.
        automaticMemory.noteUIActivity();
        uiElastic.ensure();
    }

    if (
        runtimeObjectPool.begin(
            &memory,
            "demo-runtime-pool"
        )
    )
    {
        DemoRuntimeObject* object =
            runtimeObjectPool.create(42);

        if (object)
        {
            Serial.print(
                "Object pool test value: "
            );

            Serial.println(
                object->value
            );

            runtimeObjectPool.destroy(
                object
            );
        }
    }

    // PSRAM-backed cached RGB565 asset.
    uint16_t* icon =
        automaticMemory.assets().
            putRGB565(
                0x1001,
                DEMO_ICON,
                16,
                false
            );

    if (icon)
    {
        Serial.println(
            "Asset cache ready"
        );
    }

    // Reusable internal DMA memory.
    void* dma =
        automaticMemory.dmaPool().
            acquire();

    if (dma)
    {
        Serial.println(
            "DMA pool block acquired"
        );

        automaticMemory.dmaPool().
            release(dma);
    }

    // Double framebuffer pair is already
    // allocated and zeroed by AutoMemory.
    if (
        automaticMemory.framebuffers().
            isReady()
    )
    {
        Serial.print(
            "Framebuffer pair bytes: "
        );

        Serial.println(
            automaticMemory.framebuffers().
                totalBytes()
        );
    }

    automaticMemory.dump(Serial);
}

void loop()
{
    // One reset recycles all transient
    // rendering scratch memory for this frame.
    automaticMemory.beginFrame();

    uint16_t* scanline =
        static_cast<uint16_t*>(
            automaticMemory.memory().
                scratch(
                    480 *
                        sizeof(uint16_t),
                    4
                )
        );

    if (scanline)
    {
        for (
            size_t x = 0;
            x < 480;
            ++x
        )
        {
            scanline[x] = 0x0000;
        }
    }

    // Automatic warning/critical pressure
    // actions occur here.
    automaticMemory.service();

    delay(16);
}
