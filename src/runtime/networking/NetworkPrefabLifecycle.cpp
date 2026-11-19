#include "Horo/Network/NetworkPrefabLifecycle.h"

#include "NetworkAdmissionWire.h"

#include <algorithm>
#include <array>
#include <new>
#include <tuple>

namespace Horo::Network {
    namespace {
        using AdmissionWire::Reader;
        using AdmissionWire::Writer;
        constexpr std::uint16_t WireVersion = 1;
        constexpr std::uint16_t IntentWireVersion = 2;
        constexpr std::size_t IntentLengthBytes = 2;
        constexpr std::size_t HeaderBytes = 32;
        constexpr std::size_t RecipeBytes = 54;
        constexpr std::size_t MemberBytes = 28;
        static_assert(HeaderBytes + RecipeBytes + MemberBytes == MinimumNetworkPrefabSpawnBytes);
        static_assert(HeaderBytes + RecipeBytes + MaximumNetworkPrefabEntities * MemberBytes == MaximumNetworkPrefabLegacyBytes);

        enum class RecordKind : std::uint16_t {
            Spawn = 1,
            Despawn = 2
        };

        template <typename T> Result<T> Invalid() {
            return Result<T>::Failure(MakeError(NetworkErrors::MessageEnvelopeInvalid));
        }

        /** @brief Admits the cooked group size and its sparse replicated member count. */
        bool ValidSpawnLayout(const NetworkPrefabTemplateIdentity &recipe, std::size_t members) {
            return recipe.IsValid() && recipe.entityCount <= MaximumNetworkPrefabEntities && members > 0 && members <= recipe.entityCount;
        }

        /** @brief Checks one member within its exact epoch and cooked entity bounds. */
        bool ValidMember(const NetworkPrefabMemberRecord &member, ReplicationAuthorityEpoch epoch, std::uint32_t entityCount) {
            return member.object.IsValid() && member.object.Epoch() == epoch && member.schema.IsValid() && member.version.IsValid() &&
                   member.entitySlot < entityCount;
        }

        /** @brief Validates canonical prepared order, unique live slots and exact occurrence anchoring. */
        bool ValidMembers(const NetworkPrefabSpawnRecord &spawn, NetworkObjectId occurrence) {
            if (!ValidSpawnLayout(spawn.recipe, spawn.members.size()) || spawn.members.front().object != occurrence)
                return false;
            std::array<std::uint64_t, MaximumNetworkPrefabEntities> objectSlots;
            std::optional<std::uint32_t> previous;
            std::size_t count{};
            for (const auto &member : spawn.members) {
                if (!ValidMember(member, occurrence.Epoch(), spawn.recipe.entityCount) || (previous && member.entitySlot <= *previous))
                    return false;
                previous = member.entitySlot;
                objectSlots[count++] = member.object.Slot();
            }
            auto used = std::span(objectSlots).first(count);
            std::ranges::sort(used);
            return std::ranges::adjacent_find(used) == used.end();
        }

        /** @brief Writes common occurrence coordinates, with the authority epoch supplied by the header only. */
        bool WriteObject(Writer &writer, NetworkObjectId object) {
            return writer.Number(object.Slot(), 8) && writer.Number(object.Generation(), 4);
        }

        /** @brief Reads an object scoped to the already decoded authority epoch. */
        Result<NetworkObjectId> ReadObject(Reader &reader, ReplicationAuthorityEpoch epoch) {
            std::uint64_t slot{};
            std::uint32_t generation{};
            if (!reader.Number(slot, 8) || !reader.Number(generation, 4))
                return Invalid<NetworkObjectId>();
            return NetworkObjectId::Create(epoch, slot, generation);
        }

        /** @brief Writes the bounded spawn-specific body after common validation. */
        bool WriteSpawn(Writer &writer, const NetworkPrefabSpawnRecord &spawn) {
            if (!writer.Bytes(std::as_bytes(std::span(spawn.recipe.asset.Bytes()))) ||
                !writer.Bytes(std::as_bytes(std::span(spawn.recipe.artifactDigest.bytes))) || !writer.Number(spawn.recipe.entityCount, 4) ||
                !writer.Number(spawn.members.size(), 2))
                return false;
            for (const auto &member : spawn.members)
                if (!writer.Number(member.entitySlot, 4) || !WriteObject(writer, member.object) ||
                    !writer.Number(member.schema.Value(), 8) || !writer.Number(member.version.major, 2) ||
                    !writer.Number(member.version.minor, 2))
                    return false;
            return true;
        }

        /** @brief Reads one complete schema and object declaration. */
        bool ReadMember(Reader &reader, NetworkPrefabMemberRecord &member, ReplicationAuthorityEpoch epoch) {
            if (!reader.Number(member.entitySlot, 4))
                return false;
            const auto object = ReadObject(reader, epoch);
            std::uint64_t schemaValue{};
            if (object.HasError() || !reader.Number(schemaValue, 8) || !reader.Number(member.version.major, 2) ||
                !reader.Number(member.version.minor, 2))
                return false;
            const auto schema = ReplicationSchemaId::Create(schemaValue);
            if (schema.HasError())
                return false;
            member.object = object.Value();
            member.schema = schema.Value();
            return true;
        }

        /** @brief Checks framing and counts before reserving any member storage. */
        bool ReadSpawn(Reader &reader, NetworkPrefabSpawnRecord &spawn, NetworkObjectId occurrence, std::size_t bytes, bool hasIntent) {
            std::array<std::uint8_t, 16> asset;
            std::uint16_t count{};
            if (!reader.Bytes(std::as_writable_bytes(std::span(asset))) ||
                !reader.Bytes(std::as_writable_bytes(std::span(spawn.recipe.artifactDigest.bytes))) ||
                !reader.Number(spawn.recipe.entityCount, 4) || !reader.Number(count, 2))
                return false;
            spawn.recipe.asset = Assets::AssetId::FromBytes(asset);
            const auto minimum = HeaderBytes + RecipeBytes + count * MemberBytes;
            if (!ValidSpawnLayout(spawn.recipe, count) || (hasIntent ? bytes <= minimum + IntentLengthBytes : bytes != minimum))
                return false;
            spawn.members.resize(count);
            for (auto &member : spawn.members)
                if (!ReadMember(reader, member, occurrence.Epoch()))
                    return false;
            return ValidMembers(spawn, occurrence);
        }

        /** @brief Reads an explicitly present bounded intent payload without trusting its length. */
        bool ReadIntent(Reader &reader, NetworkPrefabSpawnRecord &spawn) {
            std::uint16_t length{};
            std::span<const std::byte> bytes;
            if (!reader.Number(length, IntentLengthBytes) || length == 0 || length > MaximumNetworkPrefabIntentBytes ||
                !reader.Borrow(length, bytes) || !reader.Done())
                return false;
            spawn.intent.assign(bytes.begin(), bytes.end());
            return true;
        }

        /** @brief Appends the one verified anchor payload to the captured group. */
        void CaptureIntent(NetworkPrefabSpawnRecord &spawn, const NetworkObjectMappingEntry &entry) {
            if (entry.prefabIntent)
                spawn.intent.assign(entry.prefabIntent->begin(), entry.prefabIntent->end());
        }

        /** @brief Copies matching committed rows and orders them by cooked member slot. */
        Result<NetworkPrefabSpawnRecord> CaptureMembers(const NetworkObjectMappingSnapshot &mapping, NetworkObjectId occurrence) {
            NetworkPrefabSpawnRecord spawn;
            for (const auto &entry : mapping.Entries()) {
                if (!entry.provenance.prefab || entry.provenance.prefab->occurrence != occurrence)
                    continue;
                const auto &source = entry.provenance.prefab->source;
                if (spawn.members.empty())
                    spawn.recipe = source.recipe;
                if (source.recipe != spawn.recipe || spawn.members.size() == MaximumNetworkPrefabEntities)
                    return Invalid<NetworkPrefabSpawnRecord>();
                spawn.members.push_back({source.entitySlot, entry.object, entry.provenance.schema, entry.provenance.schemaVersion});
                CaptureIntent(spawn, entry);
            }
            if (spawn.members.empty())
                return Result<NetworkPrefabSpawnRecord>::Failure(MakeError(NetworkErrors::NetworkObjectMappingUnknown));
            std::ranges::sort(spawn.members, {}, &NetworkPrefabMemberRecord::entitySlot);
            if (!ValidMembers(spawn, occurrence))
                return Invalid<NetworkPrefabSpawnRecord>();
            return Result<NetworkPrefabSpawnRecord>::Success(std::move(spawn));
        }
    }  // namespace

    /** @copydoc CaptureNetworkPrefabSpawn */
    Result<NetworkPrefabLifecycleRecord> CaptureNetworkPrefabSpawn(const ReplicationWorldReadLease &world, NetworkObjectId occurrence) {
        using Outcome = Result<NetworkPrefabLifecycleRecord>;
        if (!world.IsValid() || world.IsRevoked() || !world.Capabilities().publishAuthority)
            return Outcome::Failure(MakeError(NetworkErrors::ReplicationWorldUnavailable));
        if (!occurrence.IsValid() || occurrence.Epoch() != world.Descriptor().authority)
            return Invalid<NetworkPrefabLifecycleRecord>();
        try {
            auto spawn = CaptureMembers(world.Mapping(), occurrence);
            if (spawn.HasError())
                return Outcome::Failure(spawn.ErrorValue());
            if (world.IsRevoked())
                return Outcome::Failure(MakeError(NetworkErrors::ReplicationWorldStale));
            return Outcome::Success({world.Descriptor().session, occurrence, std::move(spawn).Value()});
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(NetworkErrors::MessageEnvelopeCapacityExceeded));
        }
    }

    /** @copydoc CaptureNetworkPrefabSpawns */
    Result<std::vector<NetworkPrefabLifecycleRecord>> CaptureNetworkPrefabSpawns(const ReplicationWorldReadLease &world) {
        using Outcome = Result<std::vector<NetworkPrefabLifecycleRecord>>;
        if (!world.IsValid() || world.IsRevoked() || !world.Capabilities().publishAuthority)
            return Outcome::Failure(MakeError(NetworkErrors::ReplicationWorldUnavailable));
        try {
            std::vector<const NetworkObjectMappingEntry *> members;
            for (const auto &entry : world.Mapping().Entries())
                if (entry.provenance.prefab)
                    members.push_back(&entry);
            std::ranges::sort(members, {}, [](const NetworkObjectMappingEntry *entry) {
                const auto &origin = *entry->provenance.prefab;
                return std::tuple{origin.occurrence, origin.source.entitySlot};
            });
            std::vector<NetworkPrefabLifecycleRecord> records;
            for (const auto *entry : members) {
                const auto &origin = *entry->provenance.prefab;
                if (records.empty() || records.back().occurrence != origin.occurrence)
                    records.push_back({world.Descriptor().session, origin.occurrence, NetworkPrefabSpawnRecord{origin.source.recipe, {}}});
                auto &spawn = *records.back().spawn;
                if (spawn.recipe != origin.source.recipe || spawn.members.size() == MaximumNetworkPrefabEntities)
                    return Invalid<std::vector<NetworkPrefabLifecycleRecord>>();
                spawn.members.push_back(
                    {origin.source.entitySlot, entry->object, entry->provenance.schema, entry->provenance.schemaVersion});
                CaptureIntent(spawn, *entry);
            }
            for (const auto &record : records)
                if (!ValidMembers(*record.spawn, record.occurrence))
                    return Invalid<std::vector<NetworkPrefabLifecycleRecord>>();
            if (world.IsRevoked())
                return Outcome::Failure(MakeError(NetworkErrors::ReplicationWorldStale));
            return Outcome::Success(std::move(records));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(NetworkErrors::MessageEnvelopeCapacityExceeded));
        }
    }

    /** @copydoc ValidateNetworkPrefabLifecycle */
    Result<void> ValidateNetworkPrefabLifecycle(const NetworkPrefabLifecycleRecord &record) {
        if (!record.session.IsValid() || !record.occurrence.IsValid() || (record.spawn && !ValidMembers(*record.spawn, record.occurrence)))
            return Invalid<void>();
        if (record.spawn && record.spawn->intent.size() > MaximumNetworkPrefabIntentBytes)
            return Result<void>::Failure(MakeError(NetworkErrors::MessageEnvelopeCapacityExceeded));
        return Result<void>::Success();
    }

    /** @copydoc EncodeNetworkPrefabLifecycle */
    Result<std::vector<std::byte>> EncodeNetworkPrefabLifecycle(const NetworkPrefabLifecycleRecord &record) {
        using Outcome = Result<std::vector<std::byte>>;
        if (auto valid = ValidateNetworkPrefabLifecycle(record); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        try {
            const auto kind = record.spawn ? RecordKind::Spawn : RecordKind::Despawn;
            const auto bodyBytes = record.spawn ? RecipeBytes + record.spawn->members.size() * MemberBytes : 0;
            const std::span<const std::byte> intent =
                record.spawn ? std::span<const std::byte>{record.spawn->intent} : std::span<const std::byte>{};
            const auto intentBytes = intent.empty() ? 0 : IntentLengthBytes + intent.size();
            std::vector<std::byte> bytes(HeaderBytes + bodyBytes + intentBytes);
            Writer writer{bytes};
            if (!writer.Number(intent.empty() ? WireVersion : IntentWireVersion, 2) ||
                !writer.Number(static_cast<std::uint16_t>(kind), 2) || !writer.Number(record.session.Value(), 8) ||
                !writer.Number(record.occurrence.Epoch().Value(), 8) || !WriteObject(writer, record.occurrence) ||
                (record.spawn && !WriteSpawn(writer, *record.spawn)))
                return Invalid<std::vector<std::byte>>();
            if (!intent.empty() && (!writer.Number(intent.size(), IntentLengthBytes) || !writer.Bytes(intent)))
                return Invalid<std::vector<std::byte>>();
            return Outcome::Success(std::move(bytes));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(NetworkErrors::MessageEnvelopeCapacityExceeded));
        }
    }

    /** @copydoc DecodeNetworkPrefabLifecycle */
    Result<NetworkPrefabLifecycleRecord> DecodeNetworkPrefabLifecycle(std::span<const std::byte> bytes) {
        using Outcome = Result<NetworkPrefabLifecycleRecord>;
        if (bytes.size() > MaximumNetworkPrefabLifecycleBytes)
            return Outcome::Failure(MakeError(NetworkErrors::MessageEnvelopeCapacityExceeded));
        Reader reader{bytes};
        std::uint16_t version{};
        RecordKind kind{};
        std::uint64_t sessionValue{}, epochValue{};
        if (!reader.Number(version, 2) || (version != WireVersion && version != IntentWireVersion) || !reader.Number(kind, 2) ||
            !reader.Number(sessionValue, 8) || !reader.Number(epochValue, 8))
            return Invalid<NetworkPrefabLifecycleRecord>();
        const auto session = NetworkSessionGeneration::Create(sessionValue);
        const auto epoch = ReplicationAuthorityEpoch::Create(epochValue);
        if (session.HasError() || epoch.HasError())
            return Invalid<NetworkPrefabLifecycleRecord>();
        const auto occurrence = ReadObject(reader, epoch.Value());
        if (occurrence.HasError())
            return Invalid<NetworkPrefabLifecycleRecord>();
        NetworkPrefabLifecycleRecord record{session.Value(), occurrence.Value(), std::nullopt};
        const bool hasIntent = version == IntentWireVersion;
        try {
            if (kind == RecordKind::Spawn && !ReadSpawn(reader, record.spawn.emplace(), record.occurrence, bytes.size(), hasIntent))
                return Invalid<NetworkPrefabLifecycleRecord>();
            if (hasIntent && (!record.spawn || !ReadIntent(reader, *record.spawn)))
                return Invalid<NetworkPrefabLifecycleRecord>();
            if ((kind != RecordKind::Spawn && kind != RecordKind::Despawn) || !reader.Done())
                return Invalid<NetworkPrefabLifecycleRecord>();
            return Outcome::Success(std::move(record));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(NetworkErrors::MessageEnvelopeCapacityExceeded));
        }
    }
}  // namespace Horo::Network
