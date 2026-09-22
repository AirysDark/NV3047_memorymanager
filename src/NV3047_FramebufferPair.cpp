#include "NV3047_FramebufferPair.h"

namespace NV3047Memory
{

FramebufferPair::FramebufferPair()
    : manager_(nullptr),
      buffer_a_(nullptr),
      buffer_b_(nullptr),
      width_(0),
      height_(0),
      a_is_front_(true)
{
}

FramebufferPair::~FramebufferPair()
{
    end();
}

bool FramebufferPair::begin(
    uint16_t width,
    uint16_t height,
    MemoryManager* manager
)
{
    end();

    if (
        width == 0 ||
        height == 0
    )
    {
        return false;
    }

    manager_ =
        manager
            ? manager
            : &MemoryManager::instance();

    if (!manager_->isReady())
    {
        return false;
    }

    width_ = width;
    height_ = height;
    a_is_front_ = true;

    buffer_a_ =
        manager_->allocateFramebuffer(
            width_,
            height_,
            "framebuffer-a"
        );

    if (!buffer_a_)
    {
        end();
        return false;
    }

    buffer_b_ =
        manager_->allocateFramebuffer(
            width_,
            height_,
            "framebuffer-b"
        );

    if (!buffer_b_)
    {
        end();
        return false;
    }

    clearBoth(0x0000);

    return true;
}

void FramebufferPair::end()
{
    if (manager_)
    {
        if (buffer_a_)
        {
            manager_->release(buffer_a_);
        }

        if (buffer_b_)
        {
            manager_->release(buffer_b_);
        }
    }

    buffer_a_ = nullptr;
    buffer_b_ = nullptr;

    width_ = 0;
    height_ = 0;

    a_is_front_ = true;

    manager_ = nullptr;
}

bool FramebufferPair::isReady() const
{
    return
        manager_ != nullptr &&
        manager_->isReady() &&
        buffer_a_ != nullptr &&
        buffer_b_ != nullptr &&
        manager_->owns(buffer_a_) &&
        manager_->owns(buffer_b_);
}

uint16_t FramebufferPair::width() const
{
    return width_;
}

uint16_t FramebufferPair::height() const
{
    return height_;
}

size_t FramebufferPair::pixels() const
{
    return
        static_cast<size_t>(width_) *
        static_cast<size_t>(height_);
}

size_t FramebufferPair::bytesPerBuffer() const
{
    return
        pixels() *
        sizeof(uint16_t);
}

size_t FramebufferPair::totalBytes() const
{
    return
        bytesPerBuffer() * 2;
}

uint16_t* FramebufferPair::front()
{
    if (!isReady())
    {
        return nullptr;
    }

    return
        a_is_front_
            ? buffer_a_
            : buffer_b_;
}

const uint16_t* FramebufferPair::front() const
{
    if (!isReady())
    {
        return nullptr;
    }

    return
        a_is_front_
            ? buffer_a_
            : buffer_b_;
}

uint16_t* FramebufferPair::back()
{
    if (!isReady())
    {
        return nullptr;
    }

    return
        a_is_front_
            ? buffer_b_
            : buffer_a_;
}

const uint16_t* FramebufferPair::back() const
{
    if (!isReady())
    {
        return nullptr;
    }

    return
        a_is_front_
            ? buffer_b_
            : buffer_a_;
}

void FramebufferPair::swapRoles()
{
    if (!isReady())
    {
        return;
    }

    a_is_front_ =
        !a_is_front_;
}

void FramebufferPair::clearBuffer(
    uint16_t* buffer,
    uint16_t color
)
{
    if (!buffer)
    {
        return;
    }

    const size_t count =
        pixels();

    const uint32_t packed =
        (
            static_cast<uint32_t>(color)
            << 16
        ) |
        color;

    uint32_t* words =
        reinterpret_cast<uint32_t*>(
            buffer
        );

    const size_t wordCount =
        count / 2;

    for (
        size_t i = 0;
        i < wordCount;
        ++i
    )
    {
        words[i] = packed;
    }

    if ((count & 1U) != 0)
    {
        buffer[count - 1] =
            color;
    }
}

void FramebufferPair::clearBack(
    uint16_t color
)
{
    clearBuffer(
        back(),
        color
    );
}

void FramebufferPair::clearFront(
    uint16_t color
)
{
    clearBuffer(
        front(),
        color
    );
}

void FramebufferPair::clearBoth(
    uint16_t color
)
{
    clearBuffer(
        buffer_a_,
        color
    );

    clearBuffer(
        buffer_b_,
        color
    );
}

} // namespace NV3047Memory
