#pragma once

#include "NV3047_MemoryManager.h"
#include <stdint.h>
#include <stddef.h>

namespace NV3047Memory
{

class FramebufferPair
{
public:
    FramebufferPair();
    ~FramebufferPair();

    FramebufferPair(const FramebufferPair&) = delete;
    FramebufferPair& operator=(const FramebufferPair&) = delete;

    bool begin(
        uint16_t width,
        uint16_t height,
        MemoryManager* manager = nullptr
    );

    void end();

    bool isReady() const;

    uint16_t width() const;
    uint16_t height() const;
    size_t pixels() const;
    size_t bytesPerBuffer() const;
    size_t totalBytes() const;

    uint16_t* front();
    const uint16_t* front() const;

    uint16_t* back();
    const uint16_t* back() const;

    void swapRoles();

    void clearBack(uint16_t color = 0x0000);
    void clearFront(uint16_t color = 0x0000);
    void clearBoth(uint16_t color = 0x0000);

private:
    MemoryManager* manager_;

    uint16_t* buffer_a_;
    uint16_t* buffer_b_;

    uint16_t width_;
    uint16_t height_;

    bool a_is_front_;

    void clearBuffer(
        uint16_t* buffer,
        uint16_t color
    );
};

} // namespace NV3047Memory
