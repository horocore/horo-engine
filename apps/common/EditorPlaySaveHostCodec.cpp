#include "EditorPlaySaveHostCapture.h"

#include <algorithm>
#include <array>
#include <set>

namespace Horo::Application::Internal {
    namespace {
        using namespace Runtime;
        constexpr std::size_t MaximumEntities = 16'384;
        constexpr std::size_t MaximumBytes = 8U << 20U;
        constexpr SceneCook::CookedSceneLimits SceneLimits{MaximumEntities, MaximumEntities, MaximumBytes};

        /** @brief Reports foreign sandbox metadata without admitting live Scene mutation. */
        Error InvalidRecord() {
            return MakeError(SaveErrors::CaptureRecordInvalid,
                             "The Play save record does not match this activation's Scene and identities.");
        }

        /** @brief Preserves sticky codec failure while writing one exact host-issued identity record. */
        Result<void> WriteIdentity(CanonicalValueWriter &writer, const PersistentEntityRecord &record) {
            const auto *authored = std::get_if<AuthoredPersistentEntityProvenance>(&record.provenance);
            if (!authored || !record.identity.IsValid() || !record.generation.IsValid())
                return Result<void>::Failure(InvalidRecord());
            if (record.disposition > PersistentEntityDisposition::Tombstone)
                return Result<void>::Failure(InvalidRecord());
            const std::array writes{writer.WriteBytes(std::as_bytes(std::span{record.identity.Bytes()})),
                                    writer.WriteUInt64(record.generation.value), writer.WriteUInt64(authored->object.value),
                                    writer.WriteBool(record.disposition == PersistentEntityDisposition::Tombstone)};
            for (const auto &write : writes)
                if (write.HasError())
                    return Result<void>::Failure(write.ErrorValue());
            return Result<void>::Success();
        }

        /** @brief Writes the closed versioned envelope around the existing source-free runtime Scene encoding. */
        Result<void> WriteHeader(CanonicalValueWriter &writer, const PlaySaveSceneState &state, std::span<const std::uint8_t> definition) {
            const std::array writes{writer.WriteUInt32(1), writer.WriteUInt64(state.definition.Id().value),
                                    writer.WriteUInt64(state.definition.Revision().value), writer.WriteBytes(std::as_bytes(definition)),
                                    writer.WriteUInt32(static_cast<std::uint32_t>(state.identities.size()))};
            for (const auto &write : writes)
                if (write.HasError())
                    return Result<void>::Failure(write.ErrorValue());
            return Result<void>::Success();
        }

        /** @brief Checks live/tombstone coverage before any restored identity is consumed by SceneRestoreBundle. */
        bool IdentityCoverage(const PlaySaveSceneState &state) {
            std::set<SceneObjectId> live;
            for (const auto &entity : state.definition.Entities())
                live.insert(entity.object);
            PersistentEntityId prior;
            for (const auto &record : state.identities) {
                const auto *authored = std::get_if<AuthoredPersistentEntityProvenance>(&record.provenance);
                if (!authored || authored->scene != state.definition.Id() || !record.identity.IsValid() || record.identity <= prior)
                    return false;
                prior = record.identity;
                const auto removed = live.erase(authored->object);
                if ((record.disposition == PersistentEntityDisposition::Live) != (removed == 1))
                    return false;
            }
            return live.empty();
        }

        /** @brief Reopens only a payload naming the exact current immutable activation baseline. */
        Result<RuntimeSceneDefinition> ReadDefinition(CanonicalValueReader &reader, const RuntimeSceneDefinition &baseline) {
            using Outcome = Result<RuntimeSceneDefinition>;
            const auto version = reader.ReadUInt32();
            const auto identity = reader.ReadUInt64();
            const auto revision = reader.ReadUInt64();
            if (version.HasError())
                return Outcome::Failure(version.ErrorValue());
            if (identity.HasError())
                return Outcome::Failure(identity.ErrorValue());
            if (revision.HasError())
                return Outcome::Failure(revision.ErrorValue());
            if (version.Value() != 1 || identity.Value() != baseline.Id().value || revision.Value() != baseline.Revision().value)
                return Outcome::Failure(InvalidRecord());
            const auto bytes = reader.ReadBytes(MaximumBytes);
            if (bytes.HasError())
                return Outcome::Failure(bytes.ErrorValue());
            return SceneCook::DecodeCookedSceneDefinition({reinterpret_cast<const std::uint8_t *>(bytes.Value().data()),
                                                           bytes.Value().size()},
                                                          baseline.Id(), baseline.Revision(), SceneLimits);
        }

        /** @brief Keeps host identity/schema authority independent of hostile record bytes. */
        Result<PersistentEntityRecord> ReadIdentity(CanonicalValueReader &reader, const PersistentEntityRecord &expected) {
            using Outcome = Result<PersistentEntityRecord>;
            const auto identity = reader.ReadBytes(16);
            const auto generation = reader.ReadUInt64();
            const auto object = reader.ReadUInt64();
            const auto deleted = reader.ReadBool();
            if (identity.HasError())
                return Outcome::Failure(identity.ErrorValue());
            if (generation.HasError())
                return Outcome::Failure(generation.ErrorValue());
            if (object.HasError())
                return Outcome::Failure(object.ErrorValue());
            if (deleted.HasError())
                return Outcome::Failure(deleted.ErrorValue());
            const auto *authored = std::get_if<AuthoredPersistentEntityProvenance>(&expected.provenance);
            if (!authored || !std::ranges::equal(identity.Value(), std::as_bytes(std::span{expected.identity.Bytes()})) ||
                generation.Value() != expected.generation.value || object.Value() != authored->object.value)
                return Outcome::Failure(InvalidRecord());
            auto result = expected;
            result.disposition = deleted.Value() ? PersistentEntityDisposition::Tombstone : PersistentEntityDisposition::Live;
            return Outcome::Success(std::move(result));
        }

        /** @brief Reads the fixed authority-sized identity set without growing it from untrusted counts. */
        Result<std::vector<PersistentEntityRecord>> ReadIdentities(CanonicalValueReader &reader,
                                                                   std::span<const PersistentEntityRecord> expected) {
            using Outcome = Result<std::vector<PersistentEntityRecord>>;
            const auto count = reader.ReadUInt32();
            if (count.HasError())
                return Outcome::Failure(count.ErrorValue());
            if (count.Value() != expected.size() || count.Value() > MaximumEntities)
                return Outcome::Failure(InvalidRecord());
            std::vector<PersistentEntityRecord> authority(expected.begin(), expected.end());
            std::ranges::sort(authority, {}, &PersistentEntityRecord::identity);
            std::vector<PersistentEntityRecord> records;
            records.reserve(authority.size());
            for (const auto &record : authority) {
                auto decoded = ReadIdentity(reader, record);
                if (decoded.HasError())
                    return Outcome::Failure(decoded.ErrorValue());
                records.push_back(std::move(decoded).Value());
            }
            return Outcome::Success(std::move(records));
        }
    }  // namespace

    /** @copydoc EncodePlaySaveScene */
    Result<Runtime::CanonicalEncodedValue> EncodePlaySaveScene(const PlaySaveSceneState &state) {
        using Outcome = Result<Runtime::CanonicalEncodedValue>;
        if (state.identities.size() > MaximumEntities || !IdentityCoverage(state))
            return Outcome::Failure(InvalidRecord());
        const auto encoded = SceneCook::EncodeCookedSceneDefinition(state.definition, SceneLimits);
        if (encoded.HasError())
            return Outcome::Failure(encoded.ErrorValue());
        Runtime::CanonicalValueWriter writer({.maximumBytes = MaximumBytes});
        if (const auto header = WriteHeader(writer, state, encoded.Value()); header.HasError())
            return Outcome::Failure(header.ErrorValue());
        for (const auto &record : state.identities)
            if (auto written = WriteIdentity(writer, record); written.HasError())
                return Outcome::Failure(written.ErrorValue());
        return std::move(writer).Finalize();
    }

    /** @copydoc DecodePlaySaveSceneStructure */
    Result<PlaySaveSceneState> DecodePlaySaveSceneStructure(std::span<const std::byte> bytes,
                                                            const Runtime::RuntimeSceneDefinition &baseline,
                                                            std::span<const Runtime::PersistentEntityRecord> identities) {
        using Outcome = Result<PlaySaveSceneState>;
        auto opened = Runtime::CanonicalValueReader::Create(bytes, {.maximumBytes = MaximumBytes});
        if (opened.HasError())
            return Outcome::Failure(opened.ErrorValue());
        auto reader = std::move(opened).Value();
        auto definition = ReadDefinition(reader, baseline);
        if (definition.HasError())
            return Outcome::Failure(definition.ErrorValue());
        auto records = ReadIdentities(reader, identities);
        if (records.HasError())
            return Outcome::Failure(records.ErrorValue());
        if (const auto end = reader.RequireFinished(); end.HasError())
            return Outcome::Failure(end.ErrorValue());
        if (!std::ranges::equal(definition.Value().AssetDependencies(), baseline.AssetDependencies()) ||
            !std::ranges::equal(definition.Value().TriggerVolumeEvidence(), baseline.TriggerVolumeEvidence()))
            return Outcome::Failure(InvalidRecord());
        if (const auto core = ValidatePlaySaveCoreStructure(definition.Value()); core.HasError())
            return Outcome::Failure(core.ErrorValue());
        PlaySaveSceneState state{std::move(definition).Value(), std::move(records).Value()};
        if (!IdentityCoverage(state))
            return Outcome::Failure(InvalidRecord());
        return Outcome::Success(std::move(state));
    }

    /** @copydoc DecodePlaySaveScene */
    Result<PlaySaveSceneState> DecodePlaySaveScene(std::span<const std::byte> bytes, const Runtime::RuntimeSceneDefinition &baseline,
                                                   std::span<const Runtime::PersistentEntityRecord> identities,
                                                   std::span<const Editor::PlaySaveGameplayBinding> gameplay) {
        auto decoded = DecodePlaySaveSceneStructure(bytes, baseline, identities);
        if (decoded.HasError())
            return decoded;
        if (const auto covered = ValidatePlaySaveCoreState(decoded.Value().definition, gameplay); covered.HasError())
            return Result<PlaySaveSceneState>::Failure(covered.ErrorValue());
        return decoded;
    }
}  // namespace Horo::Application::Internal
