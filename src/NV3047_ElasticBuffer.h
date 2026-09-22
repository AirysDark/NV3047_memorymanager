#pragma once

#include "NV3047_MemoryBroker.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace NV3047Memory
{

template <typename T>
class ElasticBuffer
{
public:
    ElasticBuffer()
        : broker_(nullptr),
          client_(
              INVALID_BROKER_CLIENT
          ),
          data_(nullptr),
          resident_count_(0),
          desired_count_(0),
          purpose_(
              MemoryPurpose::General
          )
    {
        tag_[0] = '\0';
    }

    ~ElasticBuffer()
    {
        release();
    }

    ElasticBuffer(
        const ElasticBuffer&
    ) = delete;

    ElasticBuffer& operator=(
        const ElasticBuffer&
    ) = delete;

    bool begin(
        MemoryBroker* broker,
        BrokerClientId client,
        size_t count,
        MemoryPurpose purpose =
            MemoryPurpose::General,
        const char* tag = "elastic-buffer"
    )
    {
        release();

        broker_ = broker;
        client_ = client;
        desired_count_ = count;
        purpose_ = purpose;

        tag_[0] = '\0';

        if (tag)
        {
            strncpy(
                tag_,
                tag,
                sizeof(tag_) - 1
            );

            tag_[
                sizeof(tag_) - 1
            ] = '\0';
        }

        return ensure();
    }

    bool ensure()
    {
        if (
            data_ &&
            resident()
        )
        {
            broker_->touch(
                client_,
                data_
            );

            return true;
        }

        data_ = nullptr;
        resident_count_ = 0;

        if (
            !broker_ ||
            !broker_->isReady() ||
            client_ ==
                INVALID_BROKER_CLIENT ||
            desired_count_ == 0 ||
            desired_count_ >
                (
                    SIZE_MAX /
                    sizeof(T)
                )
        )
        {
            return false;
        }

        void* pointer =
            broker_->requestElastic(
                client_,
                desired_count_ *
                    sizeof(T),
                purpose_,
                alignof(T),
                tag_[0]
                    ? tag_
                    : nullptr,
                &ElasticBuffer::
                    handleReclaimed,
                this
            );

        if (!pointer)
        {
            return false;
        }

        data_ =
            static_cast<T*>(
                pointer
            );

        resident_count_ =
            desired_count_;

        return true;
    }

    void release()
    {
        if (
            data_ &&
            broker_ &&
            broker_->isReady()
        )
        {
            T* pointer =
                data_;

            // release() invokes the invalidation callback for elastic leases.
            broker_->release(
                client_,
                pointer
            );
        }

        data_ = nullptr;
        resident_count_ = 0;
    }

    bool resident() const
    {
        return
            data_ != nullptr &&
            broker_ != nullptr &&
            broker_->isReady() &&
            broker_->owns(
                client_,
                data_
            );
    }

    T* data()
    {
        if (!resident())
        {
            data_ = nullptr;
            resident_count_ = 0;
            return nullptr;
        }

        broker_->touch(
            client_,
            data_
        );

        return data_;
    }

    const T* data() const
    {
        if (!resident())
        {
            return nullptr;
        }

        // Const access still means the workload is using the allocation.
        const_cast<MemoryBroker*>(
            broker_
        )->touch(
            client_,
            data_
        );

        return data_;
    }

    T& operator[](
        size_t index
    )
    {
        broker_->touch(
            client_,
            data_
        );

        return data_[index];
    }

    const T& operator[](
        size_t index
    ) const
    {
        const_cast<MemoryBroker*>(
            broker_
        )->touch(
            client_,
            data_
        );

        return data_[index];
    }

    size_t size() const
    {
        return
            resident()
                ? resident_count_
                : 0;
    }

    size_t desiredSize() const
    {
        return desired_count_;
    }

    size_t sizeBytes() const
    {
        return
            size() *
            sizeof(T);
    }

    explicit operator bool() const
    {
        return resident();
    }

private:
    static void handleReclaimed(
        void* userData,
        void* pointer
    )
    {
        ElasticBuffer* self =
            static_cast<ElasticBuffer*>(
                userData
            );

        if (
            self &&
            self->data_ ==
                pointer
        )
        {
            self->data_ = nullptr;
            self->resident_count_ = 0;
        }
    }

    MemoryBroker* broker_;
    BrokerClientId client_;

    T* data_;
    size_t resident_count_;
    size_t desired_count_;

    MemoryPurpose purpose_;
    char tag_[24];
};

} // namespace NV3047Memory
