#include <Arduino.h>
#include <NV3047_MemoryManager.h>
#include <NV3047_ManagedBuffer.h>

using namespace NV3047Memory;

MemoryManager& memory =
    MemoryManager::instance();

ManagedBuffer<uint16_t> iconBuffer;

void onMemoryPressure(
    MemoryPressure level,
    const MemoryStats& stats
)
{
    Serial.print(
        "Memory pressure changed: "
    );

    if (
        level ==
        MemoryPressure::Critical
    )
    {
        Serial.println("CRITICAL");
    }
    else if (
        level ==
        MemoryPressure::Warning
    )
    {
        Serial.println("WARNING");
    }
    else
    {
        Serial.println("NORMAL");
    }

    Serial.print(
        "Internal free: "
    );
    Serial.println(
        stats.internal.freeBytes
    );

    if (stats.psramAvailable)
    {
        Serial.print(
            "PSRAM free: "
        );
        Serial.println(
            stats.psram.freeBytes
        );
    }
}

void setup()
{
    Serial.begin(115200);
    delay(500);

    MemoryConfig config;

    config.internalReserveBytes =
        48 * 1024;

    config.psramReserveBytes =
        128 * 1024;

    config.scratchBytes =
        64 * 1024;

    if (!memory.begin(config))
    {
        Serial.println(
            "Memory manager start failed"
        );

        return;
    }

    memory.setPressureCallback(
        onMemoryPressure
    );

    // Example persistent RGB565 asset.
    if (
        iconBuffer.allocate(
            64 * 64,
            MemoryPurpose::Bitmap,
            "demo-icon"
        )
    )
    {
        Serial.println(
            "RGB565 icon buffer allocated"
        );
    }

    // Example of the allocation size used by one
    // NV3047 480x272 RGB565 framebuffer.
    uint16_t* testFramebuffer =
        memory.allocateFramebuffer(
            480,
            272,
            "demo-frame"
        );

    if (testFramebuffer)
    {
        Serial.print(
            "Framebuffer bytes: "
        );

        Serial.println(
            memory.allocationSize(
                testFramebuffer
            )
        );

        // Demo only. A real driver integration
        // would keep the framebuffer alive.
        memory.release(
            testFramebuffer
        );
    }

    memory.dump(Serial);
}

void loop()
{
    // Recycle transient render memory once per frame.
    memory.beginFrame();

    uint16_t* scanline =
        static_cast<uint16_t*>(
            memory.scratch(
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
            scanline[x] =
                0x0000;
        }
    }

    memory.service();

    delay(16);
}
