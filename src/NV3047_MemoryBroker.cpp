#include "NV3047_MemoryBroker.h"

#include <string.h>

namespace NV3047Memory
{

MemoryBroker::MemoryBroker()
    : manager_(nullptr),
      permanent_arena_(nullptr),
      permanent_required_bytes_(0),
      permanent_reserved_bytes_(0),
      permanent_used_bytes_(0),
      clients_(nullptr),
      leases_(nullptr),
      ready_(false),
      last_service_ms_(0),
      successful_requests_(0),
      failed_requests_(0),
      reclaim_passes_(0),
      reclaimed_bytes_(0),
      mux_(portMUX_INITIALIZER_UNLOCKED)
{
}

MemoryBroker::~MemoryBroker()
{
    end();
}

size_t MemoryBroker::alignUp(
    size_t value,
    size_t alignment
)
{
    if (alignment <= 1)
    {
        return value;
    }

    const size_t mask =
        alignment - 1;

    if (
        value >
        SIZE_MAX - mask
    )
    {
        return SIZE_MAX;
    }

    return
        (value + mask) &
        ~mask;
}

size_t MemoryBroker::requiredPermanentBytes()
{
#if UINTPTR_MAX == 0xFFFFFFFF
    // ESP32-S3 / Arduino Core 2.0.17 uses a 32-bit ABI.
    // Keep the exact permanent broker table footprint intentional.
    static_assert(
        sizeof(ClientRecord) == 72,
        "Broker ClientRecord footprint changed"
    );

    static_assert(
        sizeof(LeaseRecord) == 12,
        "Broker LeaseRecord footprint changed"
    );
#endif

    size_t offset = 0;

    offset =
        alignUp(
            offset,
            alignof(ClientRecord)
        );

    if (
        offset == SIZE_MAX ||
        sizeof(ClientRecord) >
            (
                SIZE_MAX - offset
            ) /
            MAX_CLIENTS
    )
    {
        return SIZE_MAX;
    }

    offset +=
        sizeof(ClientRecord) *
        MAX_CLIENTS;

    offset =
        alignUp(
            offset,
            alignof(LeaseRecord)
        );

    if (
        offset == SIZE_MAX ||
        sizeof(LeaseRecord) >
            (
                SIZE_MAX - offset
            ) /
            MAX_LEASES
    )
    {
        return SIZE_MAX;
    }

    offset +=
        sizeof(LeaseRecord) *
        MAX_LEASES;

    return offset;
}

size_t MemoryBroker::recommendedPermanentBytes(
    size_t headroomBytes
)
{
    const size_t required =
        requiredPermanentBytes();

    if (
        required == SIZE_MAX ||
        headroomBytes >
            SIZE_MAX - required
    )
    {
        return SIZE_MAX;
    }

    return
        alignUp(
            required +
                headroomBytes,
            1024
        );
}

bool MemoryBroker::begin(
    MemoryManager* manager,
    const BrokerConfig& config
)
{
    end();

    if (!config.enabled)
    {
        return false;
    }

    manager_ =
        manager
            ? manager
            : &MemoryManager::instance();

    if (!manager_->isReady())
    {
        manager_ = nullptr;
        return false;
    }

    config_ = config;

    permanent_required_bytes_ =
        requiredPermanentBytes();

    if (
        permanent_required_bytes_ ==
        SIZE_MAX
    )
    {
        manager_ = nullptr;
        return false;
    }

    size_t requested =
        config_.permanentArenaBytes;

    if (requested == 0)
    {
        requested =
            recommendedPermanentBytes(
                config_.
                    permanentHeadroomBytes
            );
    }

    if (
        requested == SIZE_MAX ||
        requested <
            permanent_required_bytes_
    )
    {
        manager_ = nullptr;
        return false;
    }

    permanent_arena_ =
        static_cast<uint8_t*>(
            manager_->allocateControl(
                requested,
                8,
                "broker-control"
            )
        );

    if (!permanent_arena_)
    {
        manager_ = nullptr;
        return false;
    }

    permanent_reserved_bytes_ =
        requested;

    size_t offset = 0;

    offset =
        alignUp(
            offset,
            alignof(ClientRecord)
        );

    clients_ =
        reinterpret_cast<ClientRecord*>(
            permanent_arena_ +
            offset
        );

    offset +=
        sizeof(ClientRecord) *
        MAX_CLIENTS;

    offset =
        alignUp(
            offset,
            alignof(LeaseRecord)
        );

    leases_ =
        reinterpret_cast<LeaseRecord*>(
            permanent_arena_ +
            offset
        );

    offset +=
        sizeof(LeaseRecord) *
        MAX_LEASES;

    permanent_used_bytes_ =
        offset;

    memset(
        clients_,
        0,
        sizeof(ClientRecord) *
            MAX_CLIENTS
    );

    memset(
        leases_,
        0,
        sizeof(LeaseRecord) *
            MAX_LEASES
    );

    successful_requests_ = 0;
    failed_requests_ = 0;
    reclaim_passes_ = 0;
    reclaimed_bytes_ = 0;

    last_service_ms_ =
        millis();

    ready_ = true;

    return true;
}

void MemoryBroker::end()
{
    if (
        ready_ &&
        manager_ &&
        manager_->isReady() &&
        leases_
    )
    {
        while (true)
        {
            void* pointer = nullptr;

            portENTER_CRITICAL(
                &mux_
            );

            for (
                size_t i = 0;
                i < MAX_LEASES;
                ++i
            )
            {
                if (leases_[i].used)
                {
                    pointer =
                        leases_[i].pointer;

                    const BrokerClientId id =
                        leases_[i].client;

                    ClientRecord* client =
                        clientRecord(id);

                    if (
                        client &&
                        client->managedBytes >=
                            leases_[i].bytes
                    )
                    {
                        client->managedBytes -=
                            leases_[i].bytes;
                    }

                    memset(
                        &leases_[i],
                        0,
                        sizeof(LeaseRecord)
                    );

                    break;
                }
            }

            portEXIT_CRITICAL(
                &mux_
            );

            if (!pointer)
            {
                break;
            }

            manager_->release(
                pointer
            );
        }
    }

    ready_ = false;

    if (
        permanent_arena_ &&
        manager_ &&
        manager_->isReady() &&
        manager_->owns(
            permanent_arena_
        )
    )
    {
        manager_->release(
            permanent_arena_
        );
    }

    manager_ = nullptr;
    permanent_arena_ = nullptr;
    permanent_required_bytes_ = 0;
    permanent_reserved_bytes_ = 0;
    permanent_used_bytes_ = 0;
    clients_ = nullptr;
    leases_ = nullptr;

    successful_requests_ = 0;
    failed_requests_ = 0;
    reclaim_passes_ = 0;
    reclaimed_bytes_ = 0;
    last_service_ms_ = 0;
}

bool MemoryBroker::isReady() const
{
    return
        ready_ &&
        manager_ &&
        manager_->isReady() &&
        permanent_arena_ &&
        manager_->owns(
            permanent_arena_
        );
}

MemoryBroker::ClientRecord*
MemoryBroker::clientRecord(
    BrokerClientId client
)
{
    if (
        !clients_ ||
        client ==
            INVALID_BROKER_CLIENT ||
        client > MAX_CLIENTS
    )
    {
        return nullptr;
    }

    ClientRecord& record =
        clients_[client - 1];

    return
        record.used
            ? &record
            : nullptr;
}

const MemoryBroker::ClientRecord*
MemoryBroker::clientRecord(
    BrokerClientId client
) const
{
    if (
        !clients_ ||
        client ==
            INVALID_BROKER_CLIENT ||
        client > MAX_CLIENTS
    )
    {
        return nullptr;
    }

    const ClientRecord& record =
        clients_[client - 1];

    return
        record.used
            ? &record
            : nullptr;
}

size_t MemoryBroker::totalBytes(
    const ClientRecord& client
) const
{
    if (
        client.observedBytes >
        SIZE_MAX -
            client.managedBytes
    )
    {
        return SIZE_MAX;
    }

    return
        client.managedBytes +
        client.observedBytes;
}

size_t MemoryBroker::availableReclaim(
    const ClientRecord& client
) const
{
    const size_t total =
        totalBytes(client);

    if (
        total <= client.minimumBytes ||
        client.reclaimableBytes == 0
    )
    {
        return 0;
    }

    const size_t aboveMinimum =
        total -
        client.minimumBytes;

    return
        client.reclaimableBytes <
            aboveMinimum
            ? client.reclaimableBytes
            : aboveMinimum;
}

uint16_t MemoryBroker::importance(
    const ClientRecord& client
) const
{
    return
        static_cast<uint16_t>(
            static_cast<uint8_t>(
                client.priority
            ) *
                16U +
            static_cast<uint8_t>(
                client.activity
            ) *
                4U
        );
}

void MemoryBroker::accountPeak(
    ClientRecord& client
)
{
    const size_t total =
        totalBytes(client);

    if (
        total >
        client.peakBytes
    )
    {
        client.peakBytes =
            total;
    }
}

BrokerClientId MemoryBroker::registerClient(
    const BrokerClientConfig& config
)
{
    if (!isReady())
    {
        return
            INVALID_BROKER_CLIENT;
    }

    portENTER_CRITICAL(
        &mux_
    );

    for (
        uint8_t i = 0;
        i < MAX_CLIENTS;
        ++i
    )
    {
        ClientRecord& record =
            clients_[i];

        if (!record.used)
        {
            memset(
                &record,
                0,
                sizeof(ClientRecord)
            );

            record.id =
                static_cast<BrokerClientId>(
                    i + 1
                );

            record.priority =
                config.priority;

            record.activity =
                BrokerActivity::Idle;

            record.minimumBytes =
                config.minimumBytes;

            record.softLimitBytes =
                config.softLimitBytes;

            record.hardLimitBytes =
                config.hardLimitBytes;

            record.backgroundAfterMs =
                config.backgroundAfterMs;

            record.idleAfterMs =
                config.idleAfterMs >=
                        config.backgroundAfterMs
                    ? config.idleAfterMs
                    : config.backgroundAfterMs;

            record.reclaim =
                config.reclaim;

            record.userData =
                config.userData;

            record.lastActivityMs =
                millis();

            record.name[0] =
                '\0';

            if (config.name)
            {
                strncpy(
                    record.name,
                    config.name,
                    CLIENT_NAME_LENGTH - 1
                );

                record.name[
                    CLIENT_NAME_LENGTH - 1
                ] = '\0';
            }

            record.used = true;

            const BrokerClientId id =
                record.id;

            portEXIT_CRITICAL(
                &mux_
            );

            return id;
        }
    }

    portEXIT_CRITICAL(
        &mux_
    );

    return
        INVALID_BROKER_CLIENT;
}

bool MemoryBroker::unregisterClient(
    BrokerClientId client
)
{
    if (!isReady())
    {
        return false;
    }

    portENTER_CRITICAL(
        &mux_
    );

    ClientRecord* record =
        clientRecord(client);

    if (
        !record ||
        record->managedBytes != 0 ||
        record->observedBytes != 0
    )
    {
        portEXIT_CRITICAL(
            &mux_
        );

        return false;
    }

    memset(
        record,
        0,
        sizeof(ClientRecord)
    );

    portEXIT_CRITICAL(
        &mux_
    );

    return true;
}

bool MemoryBroker::setPriority(
    BrokerClientId client,
    BrokerPriority priority
)
{
    if (!isReady())
    {
        return false;
    }

    portENTER_CRITICAL(
        &mux_
    );

    ClientRecord* record =
        clientRecord(client);

    if (record)
    {
        record->priority =
            priority;
    }

    portEXIT_CRITICAL(
        &mux_
    );

    return
        record != nullptr;
}

bool MemoryBroker::setActivity(
    BrokerClientId client,
    BrokerActivity activity
)
{
    if (!isReady())
    {
        return false;
    }

    const uint32_t now =
        millis();

    portENTER_CRITICAL(
        &mux_
    );

    ClientRecord* record =
        clientRecord(client);

    if (record)
    {
        record->activity =
            activity;

        record->lastActivityMs =
            now;
    }

    portEXIT_CRITICAL(
        &mux_
    );

    return
        record != nullptr;
}

bool MemoryBroker::noteActivity(
    BrokerClientId client
)
{
    return
        setActivity(
            client,
            BrokerActivity::Active
        );
}

bool MemoryBroker::setObservedUsage(
    BrokerClientId client,
    size_t bytes,
    size_t reclaimableBytes
)
{
    if (!isReady())
    {
        return false;
    }

    portENTER_CRITICAL(
        &mux_
    );

    ClientRecord* record =
        clientRecord(client);

    if (record)
    {
        record->observedBytes =
            bytes;

        record->reclaimableBytes =
            reclaimableBytes <= bytes
                ? reclaimableBytes
                : bytes;

        accountPeak(
            *record
        );
    }

    portEXIT_CRITICAL(
        &mux_
    );

    return
        record != nullptr;
}

int MemoryBroker::findFreeLease() const
{
    if (!leases_)
    {
        return -1;
    }

    for (
        size_t i = 0;
        i < MAX_LEASES;
        ++i
    )
    {
        if (!leases_[i].used)
        {
            return
                static_cast<int>(i);
        }
    }

    return -1;
}

int MemoryBroker::findLease(
    BrokerClientId client,
    const void* pointer
) const
{
    if (
        !leases_ ||
        !pointer
    )
    {
        return -1;
    }

    for (
        size_t i = 0;
        i < MAX_LEASES;
        ++i
    )
    {
        if (
            leases_[i].used &&
            leases_[i].pointer ==
                pointer &&
            (
                client ==
                    INVALID_BROKER_CLIENT ||
                leases_[i].client ==
                    client
            )
        )
        {
            return
                static_cast<int>(i);
        }
    }

    return -1;
}

void* MemoryBroker::request(
    BrokerClientId client,
    size_t bytes,
    MemoryPurpose purpose,
    size_t alignment,
    const char* tag
)
{
    if (
        !isReady() ||
        bytes == 0
    )
    {
        return nullptr;
    }

    bool permitted = false;

    portENTER_CRITICAL(
        &mux_
    );

    ClientRecord* record =
        clientRecord(client);

    if (record)
    {
        const size_t current =
            totalBytes(
                *record
            );

        permitted =
            record->hardLimitBytes == 0 ||
            (
                current <=
                    record->hardLimitBytes &&
                bytes <=
                    record->hardLimitBytes -
                    current
            );
    }

    portEXIT_CRITICAL(
        &mux_
    );

    if (!permitted)
    {
        ++failed_requests_;
        return nullptr;
    }

    void* pointer =
        manager_->allocate(
            bytes,
            purpose,
            alignment,
            tag
        );

    if (!pointer)
    {
        const size_t reclaimTarget =
            bytes >
                config_.minimumReclaimBytes
                ? bytes
                : config_.
                    minimumReclaimBytes;

        reclaimFor(
            client,
            reclaimTarget,
            BrokerReclaimReason::Request
        );

        pointer =
            manager_->allocate(
                bytes,
                purpose,
                alignment,
                tag
            );
    }

    if (!pointer)
    {
        ++failed_requests_;
        return nullptr;
    }

    bool recorded = false;

    portENTER_CRITICAL(
        &mux_
    );

    record =
        clientRecord(client);

    const int leaseIndex =
        findFreeLease();

    if (
        record &&
        leaseIndex >= 0
    )
    {
        LeaseRecord& lease =
            leases_[leaseIndex];

        lease.pointer =
            pointer;

        lease.bytes =
            bytes;

        lease.client =
            client;

        lease.purpose =
            purpose;

        lease.used = true;

        record->managedBytes +=
            bytes;

        accountPeak(
            *record
        );

        recorded = true;
    }

    portEXIT_CRITICAL(
        &mux_
    );

    if (!recorded)
    {
        manager_->release(
            pointer
        );

        ++failed_requests_;

        return nullptr;
    }

    ++successful_requests_;

    return pointer;
}

bool MemoryBroker::release(
    BrokerClientId client,
    void* pointer
)
{
    if (
        !isReady() ||
        !pointer
    )
    {
        return false;
    }

    bool found = false;

    portENTER_CRITICAL(
        &mux_
    );

    const int index =
        findLease(
            client,
            pointer
        );

    if (index >= 0)
    {
        LeaseRecord& lease =
            leases_[index];

        ClientRecord* record =
            clientRecord(
                lease.client
            );

        if (
            record &&
            record->managedBytes >=
                lease.bytes
        )
        {
            record->managedBytes -=
                lease.bytes;
        }

        memset(
            &lease,
            0,
            sizeof(LeaseRecord)
        );

        found = true;
    }

    portEXIT_CRITICAL(
        &mux_
    );

    if (found)
    {
        manager_->release(
            pointer
        );
    }

    return found;
}

size_t MemoryBroker::releaseClientAllocations(
    BrokerClientId client
)
{
    if (!isReady())
    {
        return 0;
    }

    size_t released = 0;

    while (true)
    {
        void* pointer = nullptr;

        portENTER_CRITICAL(
            &mux_
        );

        for (
            size_t i = 0;
            i < MAX_LEASES;
            ++i
        )
        {
            if (
                leases_[i].used &&
                leases_[i].client ==
                    client
            )
            {
                pointer =
                    leases_[i].pointer;
                break;
            }
        }

        portEXIT_CRITICAL(
            &mux_
        );

        if (!pointer)
        {
            break;
        }

        if (
            !release(
                client,
                pointer
            )
        )
        {
            break;
        }

        ++released;
    }

    return released;
}

bool MemoryBroker::owns(
    BrokerClientId client,
    const void* pointer
) const
{
    if (
        !isReady() ||
        !pointer
    )
    {
        return false;
    }

    portENTER_CRITICAL(
        &mux_
    );

    const bool found =
        findLease(
            client,
            pointer
        ) >= 0;

    portEXIT_CRITICAL(
        &mux_
    );

    return found;
}

void MemoryBroker::refreshActivities(
    uint32_t now
)
{
    if (!clients_)
    {
        return;
    }

    for (
        uint8_t i = 0;
        i < MAX_CLIENTS;
        ++i
    )
    {
        ClientRecord& client =
            clients_[i];

        if (!client.used)
        {
            continue;
        }

        const uint32_t age =
            now -
            client.lastActivityMs;

        if (
            client.activity ==
                BrokerActivity::Burst &&
            age >= 250
        )
        {
            client.activity =
                BrokerActivity::Active;
        }

        if (
            client.idleAfterMs != 0 &&
            age >=
                client.idleAfterMs
        )
        {
            client.activity =
                BrokerActivity::Idle;
        }
        else if (
            client.backgroundAfterMs != 0 &&
            age >=
                client.backgroundAfterMs
        )
        {
            client.activity =
                BrokerActivity::Background;
        }
    }
}

int MemoryBroker::findBestDonor(
    BrokerClientId requester,
    uint16_t attemptedMask,
    BrokerReclaimReason reason
) const
{
    const ClientRecord* requesterRecord =
        clientRecord(
            requester
        );

    const uint16_t requesterImportance =
        requesterRecord
            ? importance(
                  *requesterRecord
              )
            : UINT16_MAX;

    int candidate = -1;
    uint16_t candidateImportance =
        UINT16_MAX;
    bool candidateOverSoft = false;
    size_t candidateReclaimable = 0;

    for (
        uint8_t i = 0;
        i < MAX_CLIENTS;
        ++i
    )
    {
        if (
            (
                attemptedMask &
                (
                    static_cast<uint16_t>(
                        1U
                    ) << i
                )
            ) != 0
        )
        {
            continue;
        }

        const ClientRecord& donor =
            clients_[i];

        if (
            !donor.used ||
            donor.id == requester ||
            !donor.reclaim ||
            donor.priority ==
                BrokerPriority::Critical
        )
        {
            continue;
        }

        const size_t reclaimable =
            availableReclaim(
                donor
            );

        if (reclaimable == 0)
        {
            continue;
        }

        const uint16_t donorImportance =
            importance(
                donor
            );

        if (
            reason ==
                BrokerReclaimReason::Request &&
            requesterRecord &&
            donorImportance >=
                requesterImportance
        )
        {
            continue;
        }

        const size_t donorTotal =
            totalBytes(
                donor
            );

        const bool overSoft =
            donor.softLimitBytes != 0 &&
            donorTotal >
                donor.softLimitBytes;

        if (
            candidate < 0 ||
            donorImportance <
                candidateImportance ||
            (
                donorImportance ==
                    candidateImportance &&
                overSoft &&
                !candidateOverSoft
            ) ||
            (
                donorImportance ==
                    candidateImportance &&
                overSoft ==
                    candidateOverSoft &&
                reclaimable >
                    candidateReclaimable
            )
        )
        {
            candidate =
                static_cast<int>(i);

            candidateImportance =
                donorImportance;

            candidateOverSoft =
                overSoft;

            candidateReclaimable =
                reclaimable;
        }
    }

    return candidate;
}

size_t MemoryBroker::reclaimFor(
    BrokerClientId requester,
    size_t targetBytes,
    BrokerReclaimReason reason
)
{
    if (
        !isReady() ||
        targetBytes == 0
    )
    {
        return 0;
    }

    size_t totalFreed = 0;
    uint16_t attemptedMask = 0;

    ++reclaim_passes_;

    while (
        totalFreed <
        targetBytes
    )
    {
        BrokerReclaimCallback callback =
            nullptr;

        void* userData = nullptr;
        size_t available = 0;
        int donorIndex = -1;

        portENTER_CRITICAL(
            &mux_
        );

        donorIndex =
            findBestDonor(
                requester,
                attemptedMask,
                reason
            );

        if (donorIndex >= 0)
        {
            ClientRecord& donor =
                clients_[donorIndex];

            callback =
                donor.reclaim;

            userData =
                donor.userData;

            available =
                availableReclaim(
                    donor
                );
        }

        portEXIT_CRITICAL(
            &mux_
        );

        if (
            donorIndex < 0 ||
            !callback ||
            available == 0
        )
        {
            break;
        }

        attemptedMask |=
            static_cast<uint16_t>(
                1U
            ) <<
            donorIndex;

        const size_t remaining =
            targetBytes -
            totalFreed;

        size_t ask =
            remaining <
                available
                ? remaining
                : available;

        if (
            ask <
                config_.
                    minimumReclaimBytes &&
            available >=
                config_.
                    minimumReclaimBytes
        )
        {
            ask =
                config_.
                    minimumReclaimBytes;
        }

        if (ask > available)
        {
            ask = available;
        }

        const size_t freed =
            callback(
                userData,
                ask,
                reason
            );

        if (freed == 0)
        {
            continue;
        }

        const size_t credited =
            freed <= available
                ? freed
                : available;

        portENTER_CRITICAL(
            &mux_
        );

        ClientRecord& donor =
            clients_[donorIndex];

        if (donor.used)
        {
            if (
                donor.observedBytes >=
                credited
            )
            {
                donor.observedBytes -=
                    credited;
            }
            else
            {
                donor.observedBytes = 0;
            }

            if (
                donor.reclaimableBytes >=
                credited
            )
            {
                donor.reclaimableBytes -=
                    credited;
            }
            else
            {
                donor.reclaimableBytes = 0;
            }
        }

        portEXIT_CRITICAL(
            &mux_
        );

        totalFreed +=
            credited;
    }

    reclaimed_bytes_ +=
        totalFreed;

    return totalFreed;
}

void MemoryBroker::service()
{
    if (!isReady())
    {
        return;
    }

    const uint32_t now =
        millis();

    if (
        config_.serviceIntervalMs != 0 &&
        static_cast<uint32_t>(
            now -
            last_service_ms_
        ) <
            config_.serviceIntervalMs
    )
    {
        return;
    }

    last_service_ms_ =
        now;

    portENTER_CRITICAL(
        &mux_
    );

    refreshActivities(
        now
    );

    size_t reclaimable = 0;

    for (
        uint8_t i = 0;
        i < MAX_CLIENTS;
        ++i
    )
    {
        if (clients_[i].used)
        {
            const size_t value =
                availableReclaim(
                    clients_[i]
                );

            if (
                value >
                SIZE_MAX -
                    reclaimable
            )
            {
                reclaimable =
                    SIZE_MAX;
                break;
            }

            reclaimable +=
                value;
        }
    }

    portEXIT_CRITICAL(
        &mux_
    );

    if (reclaimable == 0)
    {
        return;
    }

    const MemoryPressure pressure =
        manager_->pressure();

    uint8_t percent = 0;
    BrokerReclaimReason reason =
        BrokerReclaimReason::WarningPressure;

    if (
        pressure ==
        MemoryPressure::Critical
    )
    {
        percent =
            config_.
                criticalReclaimPercent;

        reason =
            BrokerReclaimReason::CriticalPressure;
    }
    else if (
        pressure ==
        MemoryPressure::Warning
    )
    {
        percent =
            config_.
                warningReclaimPercent;
    }
    else
    {
        return;
    }

    if (percent > 100)
    {
        percent = 100;
    }

    size_t target =
        (
            reclaimable *
            percent
        ) /
        100;

    if (
        target <
            config_.
                minimumReclaimBytes
    )
    {
        target =
            reclaimable <
                config_.
                    minimumReclaimBytes
                ? reclaimable
                : config_.
                    minimumReclaimBytes;
    }

    reclaimFor(
        INVALID_BROKER_CLIENT,
        target,
        reason
    );
}

bool MemoryBroker::clientStats(
    BrokerClientId client,
    BrokerClientStats& stats
) const
{
    stats =
        BrokerClientStats();

    if (!isReady())
    {
        return false;
    }

    portENTER_CRITICAL(
        &mux_
    );

    const ClientRecord* record =
        clientRecord(
            client
        );

    if (!record)
    {
        portEXIT_CRITICAL(
            &mux_
        );

        return false;
    }

    stats.id =
        record->id;

    stats.registered = true;

    strncpy(
        stats.name,
        record->name,
        CLIENT_NAME_LENGTH - 1
    );

    stats.name[
        CLIENT_NAME_LENGTH - 1
    ] = '\0';

    stats.priority =
        record->priority;

    stats.activity =
        record->activity;

    stats.minimumBytes =
        record->minimumBytes;

    stats.softLimitBytes =
        record->softLimitBytes;

    stats.hardLimitBytes =
        record->hardLimitBytes;

    stats.managedBytes =
        record->managedBytes;

    stats.observedBytes =
        record->observedBytes;

    stats.totalBytes =
        totalBytes(
            *record
        );

    stats.reclaimableBytes =
        availableReclaim(
            *record
        );

    stats.peakBytes =
        record->peakBytes;

    stats.lastActivityMs =
        record->lastActivityMs;

    portEXIT_CRITICAL(
        &mux_
    );

    return true;
}

BrokerStats MemoryBroker::stats() const
{
    BrokerStats result;

    result.ready =
        isReady();

    result.permanentRequiredBytes =
        permanent_required_bytes_;

    result.permanentReservedBytes =
        permanent_reserved_bytes_;

    result.permanentUsedBytes =
        permanent_used_bytes_;

    result.permanentHeadroomBytes =
        permanent_reserved_bytes_ >=
                permanent_used_bytes_
            ? permanent_reserved_bytes_ -
                permanent_used_bytes_
            : 0;

    result.successfulRequests =
        successful_requests_;

    result.failedRequests =
        failed_requests_;

    result.reclaimPasses =
        reclaim_passes_;

    result.reclaimedBytes =
        reclaimed_bytes_;

    if (!result.ready)
    {
        return result;
    }

    portENTER_CRITICAL(
        &mux_
    );

    for (
        uint8_t i = 0;
        i < MAX_CLIENTS;
        ++i
    )
    {
        const ClientRecord& client =
            clients_[i];

        if (!client.used)
        {
            continue;
        }

        ++result.registeredClients;

        result.managedBytes +=
            client.managedBytes;

        result.observedBytes +=
            client.observedBytes;

        result.reclaimableBytes +=
            availableReclaim(
                client
            );
    }

    for (
        size_t i = 0;
        i < MAX_LEASES;
        ++i
    )
    {
        if (leases_[i].used)
        {
            ++result.activeLeases;
        }
    }

    portEXIT_CRITICAL(
        &mux_
    );

    return result;
}

const char* MemoryBroker::priorityName(
    BrokerPriority priority
)
{
    switch (priority)
    {
        case BrokerPriority::Critical:
            return "CRITICAL";

        case BrokerPriority::High:
            return "HIGH";

        case BrokerPriority::Normal:
            return "NORMAL";

        case BrokerPriority::Low:
            return "LOW";

        case BrokerPriority::Background:
        default:
            return "BACKGROUND";
    }
}

const char* MemoryBroker::activityName(
    BrokerActivity activity
)
{
    switch (activity)
    {
        case BrokerActivity::Burst:
            return "BURST";

        case BrokerActivity::Active:
            return "ACTIVE";

        case BrokerActivity::Background:
            return "BACKGROUND";

        case BrokerActivity::Idle:
        default:
            return "IDLE";
    }
}

void MemoryBroker::dump(
    Stream& output
) const
{
    const BrokerStats broker =
        stats();

    output.println();
    output.println(
        "=== NV3047 Memory Broker ==="
    );

    output.print(
        "Permanent control required/reserved: "
    );

    output.print(
        broker.permanentRequiredBytes
    );

    output.print('/');

    output.println(
        broker.permanentReservedBytes
    );

    output.print(
        "Permanent control used/headroom: "
    );

    output.print(
        broker.permanentUsedBytes
    );

    output.print('/');

    output.println(
        broker.permanentHeadroomBytes
    );

    output.print(
        "Clients/leases: "
    );

    output.print(
        broker.registeredClients
    );

    output.print('/');

    output.println(
        broker.activeLeases
    );

    output.print(
        "Broker managed/observed/reclaimable: "
    );

    output.print(
        broker.managedBytes
    );

    output.print('/');

    output.print(
        broker.observedBytes
    );

    output.print('/');

    output.println(
        broker.reclaimableBytes
    );

    output.print(
        "Requests success/fail: "
    );

    output.print(
        broker.successfulRequests
    );

    output.print('/');

    output.println(
        broker.failedRequests
    );

    output.print(
        "Reclaim passes/bytes: "
    );

    output.print(
        broker.reclaimPasses
    );

    output.print('/');

    output.println(
        broker.reclaimedBytes
    );

    if (!broker.ready)
    {
        output.println(
            "Broker not ready"
        );

        return;
    }

    output.println(
        "-- Broker clients --"
    );

    for (
        BrokerClientId id = 1;
        id <= MAX_CLIENTS;
        ++id
    )
    {
        BrokerClientStats client;

        if (
            !clientStats(
                id,
                client
            )
        )
        {
            continue;
        }

        output.print('#');
        output.print(client.id);
        output.print(' ');

        output.print(
            client.name[0]
                ? client.name
                : "unnamed"
        );

        output.print(' ');

        output.print(
            priorityName(
                client.priority
            )
        );

        output.print(' ');

        output.print(
            activityName(
                client.activity
            )
        );

        output.print(
            " total="
        );

        output.print(
            client.totalBytes
        );

        output.print(
            " reclaimable="
        );

        output.print(
            client.reclaimableBytes
        );

        output.print(
            " soft="
        );

        output.println(
            client.softLimitBytes
        );
    }
}

} // namespace NV3047Memory
