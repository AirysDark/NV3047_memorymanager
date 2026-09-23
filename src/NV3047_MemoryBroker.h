#pragma once

#include "NV3047_MemoryManager.h"
#include <stddef.h>
#include <stdint.h>

namespace NV3047Memory
{

using BrokerClientId = uint8_t;

static constexpr BrokerClientId INVALID_BROKER_CLIENT = 0;

enum class BrokerPriority : uint8_t
{
    Background = 0,
    Low,
    Normal,
    High,
    Critical
};

enum class BrokerActivity : uint8_t
{
    Idle = 0,
    Background,
    Active,
    Burst
};

enum class BrokerReclaimReason : uint8_t
{
    Request = 0,
    WarningPressure,
    CriticalPressure
};

using BrokerReclaimCallback =
    size_t (*)(
        void* userData,
        size_t targetBytes,
        BrokerReclaimReason reason
    );

using BrokerLeaseReclaimedCallback =
    void (*)(
        void* userData,
        void* pointer
    );

struct BrokerClientConfig
{
    const char* name = nullptr;

    BrokerPriority priority =
        BrokerPriority::Normal;

    // Minimum footprint this client should retain when another client asks
    // it to shrink. 0 means fully elastic.
    size_t minimumBytes = 0;

    // Soft limits guide donor selection but are not hard partitions.
    size_t softLimitBytes = 0;

    // 0 means no hard client-specific limit.
    size_t hardLimitBytes = 0;

    // Activity automatically decays when noteActivity() is not called.
    uint32_t backgroundAfterMs = 1500;
    uint32_t idleAfterMs = 5000;

    BrokerReclaimCallback reclaim = nullptr;
    void* userData = nullptr;
};

struct BrokerConfig
{
    bool enabled = Defaults::BROKER_ENABLED;

    // 0 = calculate exact table requirement and reserve requirement +
    // permanentHeadroomBytes, rounded to 1 KiB.
    size_t permanentArenaBytes =
        Defaults::BROKER_PERMANENT_ARENA_BYTES;

    // Small permanent expansion margin for future broker bookkeeping.
    size_t permanentHeadroomBytes =
        Defaults::BROKER_PERMANENT_HEADROOM_BYTES;

    uint32_t serviceIntervalMs =
        Defaults::BROKER_SERVICE_INTERVAL_MS;

    // Avoid asking a donor to perform tiny low-value evictions.
    size_t minimumReclaimBytes =
        Defaults::BROKER_MINIMUM_RECLAIM_BYTES;

    // Portion of currently reclaimable client memory targeted per service pass.
    uint8_t warningReclaimPercent =
        Defaults::BROKER_WARNING_RECLAIM_PERCENT;
    uint8_t criticalReclaimPercent =
        Defaults::BROKER_CRITICAL_RECLAIM_PERCENT;
};

struct BrokerClientStats
{
    BrokerClientId id = INVALID_BROKER_CLIENT;
    bool registered = false;

    char name[20] = {};

    BrokerPriority priority =
        BrokerPriority::Normal;

    BrokerActivity activity =
        BrokerActivity::Idle;

    size_t minimumBytes = 0;
    size_t softLimitBytes = 0;
    size_t hardLimitBytes = 0;

    size_t managedBytes = 0;
    size_t observedBytes = 0;
    size_t totalBytes = 0;
    size_t reclaimableBytes = 0;
    size_t peakBytes = 0;

    MemoryRegion observedRegion =
        MemoryRegion::Auto;

    uint32_t lastActivityMs = 0;
};

struct BrokerStats
{
    bool ready = false;

    size_t permanentRequiredBytes = 0;
    size_t permanentReservedBytes = 0;
    size_t permanentUsedBytes = 0;
    size_t permanentHeadroomBytes = 0;

    uint8_t registeredClients = 0;
    size_t activeLeases = 0;

    size_t managedBytes = 0;
    size_t observedBytes = 0;
    size_t reclaimableBytes = 0;

    uint32_t successfulRequests = 0;
    uint32_t failedRequests = 0;
    uint32_t reclaimPasses = 0;
    size_t reclaimedBytes = 0;
};

class MemoryBroker
{
public:
    static constexpr uint8_t MAX_CLIENTS = 16;
    static constexpr size_t MAX_LEASES = 128;
    static constexpr size_t CLIENT_NAME_LENGTH = 20;

    MemoryBroker();
    ~MemoryBroker();

    MemoryBroker(const MemoryBroker&) = delete;
    MemoryBroker& operator=(const MemoryBroker&) = delete;

    static size_t requiredPermanentBytes();

    static size_t recommendedPermanentBytes(
        size_t headroomBytes = 2 * 1024
    );

    bool begin(
        MemoryManager* manager = nullptr,
        const BrokerConfig& config =
            BrokerConfig()
    );

    void end();

    bool isReady() const;

    BrokerClientId registerClient(
        const BrokerClientConfig& config
    );

    bool unregisterClient(
        BrokerClientId client
    );

    bool setPriority(
        BrokerClientId client,
        BrokerPriority priority
    );

    bool setActivity(
        BrokerClientId client,
        BrokerActivity activity
    );

    bool noteActivity(
        BrokerClientId client
    );

    bool setObservedUsage(
        BrokerClientId client,
        size_t bytes,
        size_t reclaimableBytes,
        MemoryRegion region =
            MemoryRegion::Auto
    );

    bool setReclaimer(
        BrokerClientId client,
        BrokerReclaimCallback reclaim,
        void* userData = nullptr
    );

    bool setLimits(
        BrokerClientId client,
        size_t minimumBytes,
        size_t softLimitBytes,
        size_t hardLimitBytes = 0
    );

    void* request(
        BrokerClientId client,
        size_t bytes,
        MemoryPurpose purpose =
            MemoryPurpose::General,
        size_t alignment = 4,
        const char* tag = nullptr
    );

    // Elastic allocations are explicitly disposable. The broker may reclaim
    // them when their client becomes less important than another workload.
    // onReclaimed must invalidate any caller-side pointer/handle.
    void* requestElastic(
        BrokerClientId client,
        size_t bytes,
        MemoryPurpose purpose =
            MemoryPurpose::General,
        size_t alignment = 4,
        const char* tag = nullptr,
        BrokerLeaseReclaimedCallback onReclaimed = nullptr,
        void* userData = nullptr
    );

    bool touch(
        BrokerClientId client,
        const void* pointer
    );

    bool release(
        BrokerClientId client,
        void* pointer
    );

    size_t releaseClientAllocations(
        BrokerClientId client
    );

    bool owns(
        BrokerClientId client,
        const void* pointer
    ) const;

    size_t reclaimFor(
        BrokerClientId requester,
        size_t targetBytes,
        BrokerReclaimReason reason,
        MemoryRegion targetRegion =
            MemoryRegion::Auto
    );

    bool validate() const;

    inline bool serviceDue(
        uint32_t nowMs
    ) const
    {
        if (!ready_)
        {
            return false;
        }

        return
            config_.serviceIntervalMs == 0 ||
            static_cast<uint32_t>(
                nowMs -
                last_service_ms_
            ) >=
                config_.serviceIntervalMs;
    }

    void service();
    void service(uint32_t nowMs);

    bool clientStats(
        BrokerClientId client,
        BrokerClientStats& stats
    ) const;

    BrokerStats stats() const;

    void dump(
        Stream& output = Serial
    ) const;

private:
    struct ClientRecord
    {
        BrokerReclaimCallback reclaim;
        void* userData;

        size_t minimumBytes;
        size_t softLimitBytes;
        size_t hardLimitBytes;

        size_t managedBytes;
        size_t observedBytes;
        size_t reclaimableBytes;
        size_t peakBytes;

        uint32_t lastActivityMs;
        uint32_t backgroundAfterMs;
        uint32_t idleAfterMs;

        BrokerClientId id;
        BrokerPriority priority;
        BrokerActivity activity;
        bool used;
        MemoryRegion observedRegion;

        char name[CLIENT_NAME_LENGTH];
    };

    struct LeaseRecord
    {
        void* pointer;
        size_t bytes;

        BrokerLeaseReclaimedCallback onReclaimed;
        void* userData;

        uint32_t lastUseMs;

        BrokerClientId client;
        MemoryPurpose purpose;
        MemoryRegion region;
        bool reclaimable;
        bool used;
    };

    MemoryManager* manager_;
    BrokerConfig config_;

    uint8_t* permanent_arena_;
    size_t permanent_required_bytes_;
    size_t permanent_reserved_bytes_;
    size_t permanent_used_bytes_;

    ClientRecord* clients_;
    LeaseRecord* leases_;

    bool ready_;
    uint32_t last_service_ms_;

    uint32_t successful_requests_;
    uint32_t failed_requests_;
    uint32_t reclaim_passes_;
    size_t reclaimed_bytes_;

    mutable portMUX_TYPE mux_;

    static size_t alignUp(
        size_t value,
        size_t alignment
    );

    static const char* priorityName(
        BrokerPriority priority
    );

    static const char* activityName(
        BrokerActivity activity
    );

    ClientRecord* clientRecord(
        BrokerClientId client
    );

    const ClientRecord* clientRecord(
        BrokerClientId client
    ) const;

    size_t totalBytes(
        const ClientRecord& client
    ) const;

    size_t availableReclaim(
        const ClientRecord& client,
        MemoryRegion targetRegion =
            MemoryRegion::Auto
    ) const;

    static bool regionCanSatisfy(
        MemoryRegion donor,
        MemoryRegion target
    );

    uint16_t importance(
        const ClientRecord& client
    ) const;

    int findFreeLease() const;

    int findLease(
        BrokerClientId client,
        const void* pointer
    ) const;

    int findBestDonor(
        BrokerClientId requester,
        uint16_t attemptedMask,
        BrokerReclaimReason reason,
        MemoryRegion targetRegion
    ) const;

    size_t elasticReclaimable(
        BrokerClientId client,
        MemoryRegion targetRegion =
            MemoryRegion::Auto
    ) const;

    int findOldestElasticLease(
        BrokerClientId client,
        size_t maximumBytes,
        MemoryRegion targetRegion
    ) const;

    size_t reclaimElastic(
        BrokerClientId client,
        size_t targetBytes,
        MemoryRegion targetRegion
    );

    void* requestInternal(
        BrokerClientId client,
        size_t bytes,
        MemoryPurpose purpose,
        size_t alignment,
        const char* tag,
        bool reclaimable,
        BrokerLeaseReclaimedCallback onReclaimed,
        void* userData
    );

    void refreshActivities(
        uint32_t now
    );

    void accountPeak(
        ClientRecord& client
    );
};

} // namespace NV3047Memory
