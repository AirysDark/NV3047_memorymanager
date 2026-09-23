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

        const NV3047Memory::
            DriverTakeoverPerformanceStats bridge =
                NV3047Memory::
                    driverTakeoverPerformanceStats();

        const uint32_t avgServiceUs =
            perf.serviceFullPasses != 0
                ? static_cast<uint32_t>(
                      perf.totalServiceUs /
                      perf.serviceFullPasses
                  )
                : 0;

        const uint32_t avgProviderServiceUs =
            bridge.providerServiceCalls != 0
                ? static_cast<uint32_t>(
                      bridge.totalProviderServiceUs /
                      bridge.providerServiceCalls
                  )
                : 0;

        Serial.println();
        Serial.println(
            "=== Memory Manager Overhaul V1 profile ==="
        );

        Serial.print(
            "provider ready/front/draw/swap: "
        );
        Serial.print(
            bridge.providerReadyCalls
        );
        Serial.print('/');
        Serial.print(
            bridge.providerFrontCalls
        );
        Serial.print('/');
        Serial.print(
            bridge.providerDrawCalls
        );
        Serial.print('/');
        Serial.println(
            bridge.providerSwapCalls
        );

        Serial.print(
            "provider beginFrame calls/reset/no-op: "
        );
        Serial.print(
            bridge.providerBeginFrameCalls
        );
        Serial.print('/');
        Serial.print(
            bridge.providerBeginFrameResets
        );
        Serial.print('/');
        Serial.println(
            bridge.providerBeginFrameNoOps
        );

        Serial.print(
            "provider service calls/full/fast-exit: "
        );
        Serial.print(
            bridge.providerServiceCalls
        );
        Serial.print('/');
        Serial.print(
            bridge.providerServiceFullPasses
        );
        Serial.print('/');
        Serial.println(
            bridge.providerServiceFastExits
        );

        Serial.print(
            "provider service avg/max us: "
        );
        Serial.print(
            avgProviderServiceUs
        );
        Serial.print('/');
        Serial.println(
            bridge.maxProviderServiceUs
        );

        Serial.print(
            "AutoMemory service full passes/avg/max us: "
        );
        Serial.print(
            perf.serviceFullPasses
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
            "broker service due/skipped: "
        );
        Serial.print(
            perf.brokerServiceDuePasses
        );
        Serial.print('/');
        Serial.println(
            perf.brokerServiceSkipped
        );

        Serial.print(
            "broker usage sync/update skips: "
        );
        Serial.print(
            perf.brokerUsageSyncs
        );
        Serial.print('/');
        Serial.println(
            perf.brokerUsageSyncSkips
        );

        Serial.print(
            "asset usage syncs: "
        );
        Serial.println(
            perf.assetUsageSyncs
        );

        Serial.print(
            "pressure policy actions: "
        );
        Serial.println(
            perf.pressurePolicyActions
        );

        Serial.print(
            "provider DMA acquire/release: "
        );
        Serial.print(
            bridge.providerDMAAcquireCalls
        );
        Serial.print('/');
        Serial.println(
            bridge.providerDMAReleaseCalls
        );

        memory.resetPerformanceStats();
    }
}
