#include <Arduino.h>
#include <NV3047_Memory.h>

void setup()
{
    // Intentionally empty.
    // The test proves that the umbrella include alone links the automatic
    // runtime and requires no explicit AutoMemory::begin()/service() calls.
}

void loop()
{
    delay(1000);
}
