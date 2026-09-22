#include <Arduino.h>
#include <NV3047_Memory.h>

using namespace NV3047Memory;

AutoMemory& memory =
    AutoMemory::instance();

ElasticBuffer<uint8_t> uiWorkA;
ElasticBuffer<uint8_t> uiWorkB;
ElasticBuffer<uint8_t> appWorkA;
ElasticBuffer<uint8_t> appWorkB;

uint32_t lastReport = 0;
uint8_t lastPhase = 0xFF;

static void printPhase(
    uint8_t phase
)
{
    Serial.print("Stress phase: ");

    switch (phase)
    {
        case 0:
            Serial.println("UI heavy");
            break;

        case 1:
            Serial.println("Application heavy");
            break;

        case 2:
            Serial.println("Both active");
            break;

        default:
            Serial.println("Idle/recovery");
            break;
    }
}

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

    config.memory.requirePSRAMForFramebuffer =
        true;

    config.memory.allowBitmapFallback =
        false;

    config.memory.allowScratchFallback =
        false;

    config.assetCacheBudgetBytes =
        384 * 1024;

    config.uiSoftBudgetBytes =
        256 * 1024;

    config.applicationSoftBudgetBytes =
        256 * 1024;

    config.broker.serviceIntervalMs =
        250;

    if (!memory.begin(config))
    {
        Serial.println(
            "AdaptiveStressTest startup failed"
        );

        return;
    }

    // Populate optional PSRAM cache entries. These are deliberately unpinned
    // so the broker can reclaim them if another PSRAM workload becomes more
    // important.
    for (
        uint32_t key = 0x3000;
        key < 0x3006;
        ++key
    )
    {
        memory.assets().put(
            key,
            nullptr,
            32 * 1024,
            false,
            4
        );
    }

    uiWorkA.begin(
        &memory.broker(),
        memory.uiClient(),
        64 * 1024,
        MemoryPurpose::General,
        "stress-ui-a"
    );

    uiWorkB.begin(
        &memory.broker(),
        memory.uiClient(),
        64 * 1024,
        MemoryPurpose::General,
        "stress-ui-b"
    );

    appWorkA.begin(
        &memory.broker(),
        memory.applicationClient(),
        64 * 1024,
        MemoryPurpose::General,
        "stress-app-a"
    );

    appWorkB.begin(
        &memory.broker(),
        memory.applicationClient(),
        64 * 1024,
        MemoryPurpose::General,
        "stress-app-b"
    );

    memory.dump(Serial);
}

void loop()
{
    if (!memory.isReady())
    {
        delay(1000);
        return;
    }

    memory.beginFrame();

    // Exercise same-frame scratch release. The second allocation can reuse
    // the area immediately after rewindScratch().
    const size_t mark =
        memory.memory().
            scratchMark();

    void* temporary =
        memory.memory().
            scratch(
                12 * 1024,
                16
            );

    if (temporary)
    {
        memset(
            temporary,
            0x5A,
            12 * 1024
        );
    }

    memory.memory().
        rewindScratch(mark);

    const uint8_t phase =
        static_cast<uint8_t>(
            (
                millis() /
                4000UL
            ) %
            4UL
        );

    if (phase != lastPhase)
    {
        lastPhase = phase;
        printPhase(phase);
    }

    MemoryBroker& broker =
        memory.broker();

    if (phase == 0)
    {
        // UI has priority through current activity. Application elastic
        // buffers can become donors if UI needs PSRAM.
        memory.noteUIActivity();

        broker.setActivity(
            memory.applicationClient(),
            BrokerActivity::Idle
        );

        uiWorkA.ensure();
        uiWorkB.ensure();

        broker.reclaimFor(
            memory.uiClient(),
            48 * 1024,
            BrokerReclaimReason::Request,
            MemoryRegion::PSRAM
        );
    }
    else if (phase == 1)
    {
        // Simulate UI inactivity and application-heavy work.
        broker.setActivity(
            memory.uiClient(),
            BrokerActivity::Idle
        );

        broker.setActivity(
            memory.assetClient(),
            BrokerActivity::Idle
        );

        memory.noteApplicationActivity();

        appWorkA.ensure();
        appWorkB.ensure();

        broker.reclaimFor(
            memory.applicationClient(),
            64 * 1024,
            BrokerReclaimReason::Request,
            MemoryRegion::PSRAM
        );
    }
    else if (phase == 2)
    {
        // Both workloads are active. The broker should preserve both unless
        // actual pressure requires optional reclamation.
        memory.noteUIActivity();
        memory.noteApplicationActivity();

        uiWorkA.ensure();
        appWorkA.ensure();
    }
    else
    {
        broker.setActivity(
            memory.uiClient(),
            BrokerActivity::Idle
        );

        broker.setActivity(
            memory.assetClient(),
            BrokerActivity::Idle
        );

        broker.setActivity(
            memory.applicationClient(),
            BrokerActivity::Idle
        );
    }

    memory.service();

    if (
        static_cast<uint32_t>(
            millis() -
            lastReport
        ) >= 1000
    )
    {
        lastReport = millis();

        const HeapStats psram =
            memory.memory().
                regionStats(
                    MemoryRegion::PSRAM
                );

        Serial.print(
            "validate="
        );

        Serial.print(
            memory.validate()
                ? "OK"
                : "FAIL"
        );

        Serial.print(
            " psramFree="
        );

        Serial.print(
            psram.freeBytes
        );

        Serial.print(
            " largest="
        );

        Serial.print(
            psram.largestFreeBlock
        );

        Serial.print(
            " uiA="
        );

        Serial.print(
            uiWorkA.resident()
                ? "R"
                : "-"
        );

        Serial.print(
            " appA="
        );

        Serial.println(
            appWorkA.resident()
                ? "R"
                : "-"
        );
    }

    delay(16);
}
