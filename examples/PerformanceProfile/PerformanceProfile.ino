#include <Arduino.h>
#include <NV3047_Memory.h>
#include <NV3047_Driver.h>

NV3047 hardware;
NV3047_Driver display;

static uint32_t last_report_ms = 0;
static uint32_t frame_seed = 0;

void setup()
{
    Serial.begin(115200);
    delay(300);

    if (!display.begin(&hardware))
    {
        Serial.println(
            "PerformanceProfile: driver init failed"
        );
        return;
    }

    Serial.print(
        "External memory manager: "
    );

    Serial.println(
        display.isExternalMemoryManagerActive()
            ? "ACTIVE"
            : "NOT ACTIVE"
    );

    NV3047Memory::AutoMemory::instance().
        setPerformanceProfiling(
            true,
            true
        );
}

void loop()
{
    if (!display.getCanvas())
    {
        delay(1000);
        return;
    }

    display.clear(
        Config::COLOR_BLACK
    );

    // Deliberately issue many independent primitives so the test resembles
    // the staged object benchmark that exposed V2 provider overhead.
    for (
        uint16_t i = 0;
        i < 160;
        ++i
    )
    {
        const int16_t x =
            static_cast<int16_t>(
                (
                    i * 17U +
                    frame_seed
                ) %
                450U
            );

        const int16_t y =
            static_cast<int16_t>(
                (
                    i * 11U +
                    frame_seed / 2U
                ) %
                242U
            );

        display.fillRect(
            x,
            y,
            12,
            8,
            Config::COLOR_RED
        );

        display.drawRect(
            x,
            y,
            18,
            12,
            Config::COLOR_GREEN
        );

        display.drawHLine(
            x,
            y + 14,
            20,
            Config::COLOR_BLUE
        );

        display.drawVLine(
            x + 21,
            y,
            15,
            Config::COLOR_WHITE
        );
    }

    if (!display.present())
    {
        Serial.println(
            "Present failed"
        );
    }

    ++frame_seed;

    const uint32_t now =
        millis();

    if (
        static_cast<uint32_t>(
            now -
            last_report_ms
        ) >= 5000U
    )
    {
        last_report_ms = now;

        NV3047Memory::AutoMemory&
            memory =
                NV3047Memory::AutoMemory::
                    instance();

        const NV3047Memory::
            AutoMemoryPerformanceStats perf =
                memory.performanceStats();

        const uint32_t avgFrameBoundaryUs =
            perf.beginFrameCalls != 0
                ? static_cast<uint32_t>(
                      perf.totalBeginFrameUs /
                      perf.beginFrameCalls
                  )
                : 0;

        const uint32_t avgServiceUs =
            perf.serviceCalls != 0
                ? static_cast<uint32_t>(
                      perf.totalServiceUs /
                      perf.serviceCalls
                  )
                : 0;

        Serial.println();
        Serial.println(
            "=== Memory Manager Overhaul V1 profile ==="
        );

        Serial.print(
            "beginFrame calls/avg/max us: "
        );
        Serial.print(
            perf.beginFrameCalls
        );
        Serial.print('/');
        Serial.print(
            avgFrameBoundaryUs
        );
        Serial.print('/');
        Serial.println(
            perf.maxBeginFrameUs
        );

        Serial.print(
            "service calls/avg/max us: "
        );
        Serial.print(
            perf.serviceCalls
        );
        Serial.print('/');
        Serial.print(
            avgServiceUs
        );
        Serial.print('/');
        Serial.println(
            perf.maxServiceUs
        );

        Serial.print(
            "heap sample passes: "
        );
        Serial.println(
            perf.heapSamplePasses
        );

        Serial.print(
            "manager heap samples total: "
        );
        Serial.println(
            memory.memory().
                heapSampleCount()
        );

        Serial.print(
            "broker usage syncs: "
        );
        Serial.println(
            perf.brokerUsageSyncs
        );

        Serial.print(
            "asset usage syncs: "
        );
        Serial.println(
            perf.assetUsageSyncs
        );

        memory.resetPerformanceStats();
    }
}
