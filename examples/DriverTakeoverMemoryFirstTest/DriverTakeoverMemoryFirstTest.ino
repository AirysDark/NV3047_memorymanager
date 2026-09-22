#include <Arduino.h>
#include <NV3047_Memory.h>
#include <NV3047_Driver.h>

NV3047 hardware;
NV3047_Driver display;

void setup()
{
    Serial.begin(115200);

    if (!display.begin(&hardware))
    {
        Serial.println("NV3047 init failed");
        return;
    }

    Serial.print("External memory takeover: ");
    Serial.println(
        display.isExternalMemoryManagerActive()
            ? "ACTIVE"
            : "NOT ACTIVE"
    );
}

void loop()
{
    delay(1000);
}
