#include "NV3047_AutoRuntime.h"

#include "NV3047_AutoMemory.h"
#include "NV3047_DriverTakeover.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace
{

// ESP-IDF's FreeRTOS API uses task stack depth in bytes. StackType_t is
// uint8_t on ESP32-S3, but size from bytes so this remains correct if the
// underlying port type changes later.
static constexpr size_t RUNTIME_STACK_BYTES =
    4096;

static_assert(
    RUNTIME_STACK_BYTES %
        sizeof(StackType_t) == 0,
    "Runtime stack byte size must align to StackType_t"
);

static StackType_t runtime_task_stack[
    RUNTIME_STACK_BYTES /
    sizeof(StackType_t)
];
static StaticTask_t runtime_task_tcb;
static StaticSemaphore_t runtime_mutex_storage;

static SemaphoreHandle_t runtime_mutex =
    nullptr;

static TaskHandle_t runtime_task =
    nullptr;

static portMUX_TYPE install_mux =
    portMUX_INITIALIZER_UNLOCKED;

static volatile bool installed = false;
static volatile bool task_running = false;
static volatile bool minimal_mode_owned = false;

static volatile uint32_t automatic_starts = 0;
static volatile uint32_t failed_starts = 0;
static volatile uint32_t background_service_passes = 0;

static const TickType_t SERVICE_DELAY =
    pdMS_TO_TICKS(250);

bool startMinimalAutomaticMemory()
{
    NV3047Memory::AutoMemory& automatic =
        NV3047Memory::AutoMemory::instance();

    if (automatic.isReady())
    {
        return true;
    }

    NV3047Memory::AutoMemoryConfig config;

    // Header-only activation must not duplicate the driver's current
    // framebuffer/DMA ownership before the external-provider takeover begins.
    config.allocateFramebufferPair = false;
    config.enableDMAPool = false;

    // Scratch is frame-lifetime memory. It is enabled automatically once the
    // driver takeover starts and real frame boundaries exist.
    config.memory.scratchBytes = 0;

    config.enableBroker = true;

    if (!automatic.begin(config))
    {
        ++failed_starts;
        return false;
    }

    minimal_mode_owned = true;

    ++automatic_starts;
    return true;
}

void runtimeTask(void*)
{
    task_running = true;

    // Yield once so Arduino/core startup can finish before the first automatic
    // heap reservation. Driver takeover may start first; that path is valid.
    vTaskDelay(1);

    while (true)
    {
        if (runtime_mutex)
        {
            xSemaphoreTake(
                runtime_mutex,
                portMAX_DELAY
            );
        }

        NV3047Memory::AutoMemory& automatic =
            NV3047Memory::AutoMemory::instance();

        if (!automatic.isReady())
        {
            startMinimalAutomaticMemory();
        }
        else
        {
            // Service is intentionally independent of display swaps. A static
            // screen or sleeping display must not stop pressure monitoring,
            // activity decay, cache trimming or broker maintenance.
            automatic.service();
            ++background_service_passes;
        }

        if (runtime_mutex)
        {
            xSemaphoreGive(
                runtime_mutex
            );
        }

        vTaskDelay(
            SERVICE_DELAY
        );
    }
}

} // namespace

extern "C" void
nv3047_memorymanager_autoruntime_install()
{
    portENTER_CRITICAL(
        &install_mux
    );

    if (installed)
    {
        portEXIT_CRITICAL(
            &install_mux
        );

        return;
    }

    installed = true;

    portEXIT_CRITICAL(
        &install_mux
    );

    runtime_mutex =
        xSemaphoreCreateMutexStatic(
            &runtime_mutex_storage
        );

    if (!runtime_mutex)
    {
        installed = false;
        return;
    }

    runtime_task =
        xTaskCreateStaticPinnedToCore(
            &runtimeTask,
            "nv3047-mem",
            RUNTIME_STACK_BYTES,
            nullptr,
            tskIDLE_PRIORITY + 1,
            runtime_task_stack,
            &runtime_task_tcb,
            tskNO_AFFINITY
        );

    if (!runtime_task)
    {
        installed = false;
        runtime_mutex = nullptr;
    }
}

extern "C" bool
nv3047_memorymanager_autoruntime_lock()
{
    if (!runtime_mutex)
    {
        return true;
    }

    return
        xSemaphoreTake(
            runtime_mutex,
            portMAX_DELAY
        ) == pdTRUE;
}

extern "C" void
nv3047_memorymanager_autoruntime_unlock()
{
    if (runtime_mutex)
    {
        xSemaphoreGive(
            runtime_mutex
        );
    }
}

extern "C" bool
nv3047_memorymanager_autoruntime_minimal_owned()
{
    return minimal_mode_owned;
}

extern "C" void
nv3047_memorymanager_autoruntime_clear_minimal_owned()
{
    minimal_mode_owned = false;
}

namespace NV3047Memory
{

AutoRuntimeStats automaticRuntimeStats()
{
    AutoRuntimeStats result;

    result.installed =
        installed;

    result.taskRunning =
        task_running;

    result.managerReady =
        AutoMemory::instance().
            isReady();

    result.driverTakeoverActive =
        nv3047_memorymanager_driver_bridge_active();

    result.minimalModeOwned =
        minimal_mode_owned;

    result.automaticStarts =
        automatic_starts;

    result.failedStarts =
        failed_starts;

    result.backgroundServicePasses =
        background_service_passes;

    result.runtimeStackBytes =
        sizeof(runtime_task_stack);

    result.minimumFreeStackBytes =
        runtime_task
            ? static_cast<size_t>(
                  uxTaskGetStackHighWaterMark(
                      runtime_task
                  )
              ) *
                  sizeof(StackType_t)
            : 0;

    result.staticRuntimeBytes =
        sizeof(runtime_task_stack) +
        sizeof(runtime_task_tcb) +
        sizeof(runtime_mutex_storage) +
        sizeof(runtime_mutex) +
        sizeof(runtime_task) +
        sizeof(runtime_mutex) +
        sizeof(runtime_task) +
        sizeof(install_mux) +
        sizeof(installed) +
        sizeof(task_running) +
        sizeof(minimal_mode_owned) +
        sizeof(automatic_starts) +
        sizeof(failed_starts) +
        sizeof(background_service_passes);

    return result;
}

} // namespace NV3047Memory
