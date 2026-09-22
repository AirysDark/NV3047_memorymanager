#pragma once

#include "NV3047_MemoryManager.h"
#include <new>
#include <stddef.h>
#include <stdint.h>
#include <utility>

namespace NV3047Memory
{

template <
    typename T,
    size_t Capacity
>
class ObjectPool
{
    static_assert(
        Capacity > 0,
        "ObjectPool Capacity must be greater than zero"
    );

public:
    ObjectPool()
        : manager_(nullptr),
          storage_(nullptr),
          used_count_(0)
    {
        for (
            size_t i = 0;
            i < Capacity;
            ++i
        )
        {
            used_[i] = false;
        }
    }

    ~ObjectPool()
    {
        end();
    }

    ObjectPool(
        const ObjectPool&
    ) = delete;

    ObjectPool& operator=(
        const ObjectPool&
    ) = delete;

    bool begin(
        MemoryManager* manager = nullptr,
        const char* tag = "ui-object-pool"
    )
    {
        end();

        manager_ =
            manager
                ? manager
                : &MemoryManager::instance();

        if (!manager_->isReady())
        {
            manager_ = nullptr;
            return false;
        }

        storage_ =
            static_cast<Slot*>(
                manager_->allocate(
                    sizeof(Slot) *
                        Capacity,
                    MemoryPurpose::UIObject,
                    alignof(Slot),
                    tag
                )
            );

        if (!storage_)
        {
            manager_ = nullptr;
            return false;
        }

        for (
            size_t i = 0;
            i < Capacity;
            ++i
        )
        {
            used_[i] = false;
        }

        used_count_ = 0;

        return true;
    }

    void end()
    {
        if (
            storage_ &&
            manager_ &&
            storageAlive()
        )
        {
            clear();

            manager_->release(
                storage_
            );
        }

        storage_ = nullptr;
        manager_ = nullptr;
        used_count_ = 0;

        for (
            size_t i = 0;
            i < Capacity;
            ++i
        )
        {
            used_[i] = false;
        }
    }

    template <typename... Args>
    T* create(
        Args&&... args
    )
    {
        if (!storageAlive())
        {
            return nullptr;
        }

        for (
            size_t i = 0;
            i < Capacity;
            ++i
        )
        {
            if (!used_[i])
            {
                T* object =
                    new (
                        storage_[i].bytes
                    )
                    T(
                        std::forward<Args>(
                            args
                        )...
                    );

                used_[i] = true;
                ++used_count_;

                return object;
            }
        }

        return nullptr;
    }

    bool destroy(
        T* object
    )
    {
        if (
            !storageAlive() ||
            !object
        )
        {
            return false;
        }

        for (
            size_t i = 0;
            i < Capacity;
            ++i
        )
        {
            T* slotObject =
                reinterpret_cast<T*>(
                    storage_[i].bytes
                );

            if (
                used_[i] &&
                slotObject == object
            )
            {
                object->~T();
                used_[i] = false;

                if (used_count_ > 0)
                {
                    --used_count_;
                }

                return true;
            }
        }

        return false;
    }

    void clear()
    {
        if (!storageAlive())
        {
            return;
        }

        for (
            size_t i = 0;
            i < Capacity;
            ++i
        )
        {
            if (used_[i])
            {
                T* object =
                    reinterpret_cast<T*>(
                        storage_[i].bytes
                    );

                object->~T();
                used_[i] = false;
            }
        }

        used_count_ = 0;
    }

    bool owns(
        const T* object
    ) const
    {
        if (
            !storageAlive() ||
            !object
        )
        {
            return false;
        }

        const uintptr_t address =
            reinterpret_cast<uintptr_t>(
                object
            );

        const uintptr_t start =
            reinterpret_cast<uintptr_t>(
                storage_
            );

        const uintptr_t end =
            start +
            sizeof(Slot) *
                Capacity;

        return
            address >= start &&
            address < end;
    }

    size_t capacity() const
    {
        return Capacity;
    }

    size_t used() const
    {
        return
            storageAlive()
                ? used_count_
                : 0;
    }

    size_t available() const
    {
        return
            storageAlive()
                ? Capacity - used_count_
                : 0;
    }

private:
    bool storageAlive() const
    {
        return
            storage_ != nullptr &&
            manager_ != nullptr &&
            manager_->isReady() &&
            manager_->owns(storage_);
    }

    struct Slot
    {
        alignas(T)
        uint8_t bytes[sizeof(T)];
    };

    MemoryManager* manager_;
    Slot* storage_;

    bool used_[Capacity];
    size_t used_count_;
};

} // namespace NV3047Memory
