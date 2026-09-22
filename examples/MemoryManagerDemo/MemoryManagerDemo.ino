#include <Arduino.h>
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
