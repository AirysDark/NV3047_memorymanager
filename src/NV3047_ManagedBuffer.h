#pragma once

#include "NV3047_MemoryManager.h"

namespace NV3047Memory
{

template <typename T>
class ManagedBuffer
{
public:
    ManagedBuffer()
        : manager_(
              &MemoryManager::instance()
          ),
          data_(nullptr),
          count_(0)
    {
    }

    explicit ManagedBuffer(
        size_t count,
        MemoryPurpose purpose =
            MemoryPurpose::General,
        const char* tag = nullptr
    )
        : ManagedBuffer()
    {
        allocate(
            count,
            purpose,
            tag
        );
    }

    ~ManagedBuffer()
    {
        release();
    }

    ManagedBuffer(
        const ManagedBuffer&
    ) = delete;

    ManagedBuffer& operator=(
        const ManagedBuffer&
    ) = delete;

    ManagedBuffer(
        ManagedBuffer&& other
    ) noexcept
        : manager_(other.manager_),
          data_(other.data_),
          count_(other.count_)
    {
        other.data_ = nullptr;
        other.count_ = 0;
    }

    ManagedBuffer& operator=(
        ManagedBuffer&& other
    ) noexcept
    {
        if (this != &other)
        {
            release();

            manager_ =
                other.manager_;

            data_ =
                other.data_;

            count_ =
                other.count_;

            other.data_ = nullptr;
            other.count_ = 0;
        }

        return *this;
    }

    bool allocate(
        size_t count,
        MemoryPurpose purpose =
            MemoryPurpose::General,
        const char* tag = nullptr
    )
    {
        release();

        if (
            count != 0 &&
            count >
                (
                    SIZE_MAX /
                    sizeof(T)
                )
        )
        {
            return false;
        }

        data_ =
            manager_->allocateArray<T>(
                count,
                purpose,
                tag
            );

        if (!data_)
        {
            count_ = 0;
            return false;
        }

        count_ = count;
        return true;
    }

    void release()
    {
        if (data_)
        {
            manager_->release(
                data_
            );

            data_ = nullptr;
            count_ = 0;
        }
    }

    T* data()
    {
        return data_;
    }

    const T* data() const
    {
        return data_;
    }

    size_t size() const
    {
        return count_;
    }

    size_t sizeBytes() const
    {
        return count_ *
            sizeof(T);
    }

    bool empty() const
    {
        return data_ == nullptr;
    }

    explicit operator bool() const
    {
        return data_ != nullptr;
    }

    T& operator[](
        size_t index
    )
    {
        return data_[index];
    }

    const T& operator[](
        size_t index
    ) const
    {
        return data_[index];
    }

private:
    MemoryManager* manager_;
    T* data_;
    size_t count_;
};

} // namespace NV3047Memory
