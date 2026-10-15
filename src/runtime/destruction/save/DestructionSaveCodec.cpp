#include "DestructionSaveInternal.h"

#include <algorithm>
#include <array>
#include <new>

namespace Horo::Destruction {
    namespace {
        /** @brief Reject malformed canonical Save values without publishing a partial record. */
        template <typename T> Result<T> Invalid() {
            return Result<T>::Failure(MakeError(DestructionErrors::StateInvalid));
        }

        /** @brief Preserve the first sticky codec failure after a bounded scalar field group. */
        Result<void> Writes(const std::initializer_list<Result<void>> values) {
            for (const auto &v : values)
                if (v.HasError())
                    return v;
            return Result<void>::Success();
        }

        /** @brief Decode a fixed canonical octet identity without native object-layout reads. */
        template <std::size_t N> Result<std::array<std::uint8_t, N>> Octets(Runtime::CanonicalValueReader &reader) {
            const auto bytes = reader.ReadBytes(N);
            if (bytes.HasError())
                return Result<std::array<std::uint8_t, N>>::Failure(bytes.ErrorValue());
            if (bytes.Value().size() != N)
                return Invalid<std::array<std::uint8_t, N>>();
            std::array<std::uint8_t, N> result{};
            std::ranges::transform(bytes.Value(), result.begin(), [](const std::byte b) {
                return std::to_integer<std::uint8_t>(b);
            });
            return Result<std::array<std::uint8_t, N>>::Success(result);
        }

        /** @brief Read one finite fixed-width scalar group; the reader owns wire/work admission. */
        template <std::size_t N> Result<std::array<std::uint64_t, N>> U64s(Runtime::CanonicalValueReader &reader) {
            std::array<std::uint64_t, N> result{};
            for (auto &v : result) {
                auto value = reader.ReadUInt64();
                if (value.HasError())
                    return Result<std::array<std::uint64_t, N>>::Failure(value.ErrorValue());
                v = value.Value();
            }
            return Result<std::array<std::uint64_t, N>>::Success(result);
        }

        /** @brief Capture-independent canonical scalar fields; identities are decoded through their existing contracts. */
        Result<CanonicalDestructionSnapshot> ReadStateHeader(Runtime::CanonicalValueReader &reader) {
            auto version = reader.ReadUInt32();
            if (version.HasError())
                return Result<CanonicalDestructionSnapshot>::Failure(version.ErrorValue());
            if (version.Value() != 1)
                return Invalid<CanonicalDestructionSnapshot>();
            auto targetBytes = Octets<24>(reader);
            auto contentBytes = Octets<56>(reader);
            if (targetBytes.HasError())
                return Result<CanonicalDestructionSnapshot>::Failure(targetBytes.ErrorValue());
            if (contentBytes.HasError())
                return Result<CanonicalDestructionSnapshot>::Failure(contentBytes.ErrorValue());
            const auto target = DeserializeDestructionHandle(targetBytes.Value());
            const auto content = DeserializeFractureArtifactContentIdentity(contentBytes.Value());
            if (target.HasError())
                return Result<CanonicalDestructionSnapshot>::Failure(target.ErrorValue());
            if (content.HasError())
                return Result<CanonicalDestructionSnapshot>::Failure(content.ErrorValue());
            auto scalar = U64s<5>(reader);
            if (scalar.HasError())
                return Result<CanonicalDestructionSnapshot>::Failure(scalar.ErrorValue());
            const auto configuration = DestructionConfigurationRevision::Create(scalar.Value()[0]);
            const auto authority = DestructionAuthorityEpoch::Create(scalar.Value()[1]);
            const auto revision = DestructionStateRevision::Create(scalar.Value()[2]);
            if (configuration.HasError() || authority.HasError() || revision.HasError())
                return Invalid<CanonicalDestructionSnapshot>();
            CanonicalDestructionSnapshot result{.target = target.Value(),
                                                .content = content.Value(),
                                                .configuration = configuration.Value(),
                                                .authority = authority.Value(),
                                                .revision = revision.Value()};
            result.seed.value = scalar.Value()[3];
            result.seed.cursor = scalar.Value()[4];
            std::array<std::uint32_t, 6> words{};
            for (auto &word : words) {
                auto v = reader.ReadUInt32();
                if (v.HasError())
                    return Result<CanonicalDestructionSnapshot>::Failure(v.ErrorValue());
                word = v.Value();
            }
            if (words[1] > static_cast<std::uint32_t>(DestructionStatePhase::Destroyed))
                return Invalid<CanonicalDestructionSnapshot>();
            result.effectiveFeatures.bits = words[0];
            result.phase = static_cast<DestructionStatePhase>(words[1]);
            result.healthQ16 = words[2];
            result.seed.version = words[3];
            result.masks.chunkCount = words[4];
            result.supportCursor = words[5];
            return Result<CanonicalDestructionSnapshot>::Success(std::move(result));
        }

        /** @brief Bound variable lengths before copying masks or reserving anchors. */
        Result<void> ReadCollections(Runtime::CanonicalValueReader &reader, CanonicalDestructionSnapshot &state,
                                     const DestructionCanonicalLimits &limits) {
            if (!state.masks.chunkCount || state.masks.chunkCount > limits.maximumChunks)
                return Invalid<void>();
            const auto bytes = (static_cast<std::size_t>(state.masks.chunkCount) + 7) / 8;
            for (auto *mask : {&state.masks.broken, &state.masks.active, &state.masks.supported, &state.masks.dormant}) {
                auto value = reader.ReadBytes(bytes);
                if (value.HasError())
                    return Result<void>::Failure(value.ErrorValue());
                if (value.Value().size() != bytes)
                    return Invalid<void>();
                *mask = std::move(value).Value();
            }
            auto count = reader.ReadUInt32();
            if (count.HasError())
                return Result<void>::Failure(count.ErrorValue());
            if (count.Value() > limits.maximumAnchors)
                return Invalid<void>();
            state.supportAnchors.reserve(count.Value());
            for (std::uint32_t i = 0; i < count.Value(); ++i) {
                auto value = reader.ReadUInt64();
                if (value.HasError())
                    return Result<void>::Failure(value.ErrorValue());
                auto id = DestructionChunkId::Create(value.Value());
                if (id.HasError())
                    return Result<void>::Failure(id.ErrorValue());
                state.supportAnchors.push_back(id.Value());
            }
            return Result<void>::Success();
        }

        /** @brief Restore only the explicitly named pairing receipt, deriving membership from semantic masks. */
        Result<void> ReadCapture(Runtime::CanonicalValueReader &reader, DecodedDestructionSaveState &record,
                                 const DestructionCanonicalArtifactView &artifact) {
            auto scalar = U64s<4>(reader);
            if (scalar.HasError())
                return Result<void>::Failure(scalar.ErrorValue());
            const auto capability = DestructionCapabilityRevision::Create(scalar.Value()[3]);
            if (!scalar.Value()[0] || !scalar.Value()[1] || !scalar.Value()[2] || capability.HasError())
                return Invalid<void>();
            record.fence = {scalar.Value()[0], scalar.Value()[1], {scalar.Value()[2]}, capability.Value(), record.state.authority, {}};
            auto hasPhysics = reader.ReadBool();
            if (hasPhysics.HasError())
                return Result<void>::Failure(hasPhysics.ErrorValue());
            if (!hasPhysics.Value())
                return Result<void>::Success();
            auto participantBytes = reader.ReadBytes(Runtime::MaximumSaveParticipantIdBytes);
            auto checkpointBytes = Octets<16>(reader);
            if (participantBytes.HasError())
                return Result<void>::Failure(participantBytes.ErrorValue());
            if (checkpointBytes.HasError())
                return Result<void>::Failure(checkpointBytes.ErrorValue());
            const auto &bytes = participantBytes.Value();
            std::string participantText(bytes.size(), '\0');
            std::ranges::transform(bytes, participantText.begin(), [](const std::byte byte) {
                return std::to_integer<char>(byte);
            });
            auto participant = Runtime::SaveParticipantId::Parse(participantText);
            auto checkpoint = Runtime::SaveRecordId::FromBytes(checkpointBytes.Value());
            if (participant.HasError() || checkpoint.HasError())
                return Invalid<void>();
            record.physics = DestructionPhysicsCaptureReceipt{participant.Value(), checkpoint.Value(),  record.fence.sceneIncarnation,
                                                              record.fence.epoch,  record.state.target, {}};
            for (std::size_t i = 0; i < artifact.canonicalChunks.size(); ++i)
                if ((record.state.masks.active[i / 8] & (std::byte{1} << (i % 8))) != std::byte{})
                    record.physics->activeChunks.push_back(artifact.canonicalChunks[i]);
            return Result<void>::Success();
        }
    }  // namespace

    namespace Detail {
        /** @copydoc DestructionSavePayloadBytes */
        std::size_t DestructionSavePayloadBytes(const CapturedDestructionState &captured) noexcept {
            const auto &s = captured.State();
            // Canonical byte strings have a uint32 length; other fields have fixed canonical widths.
            return 209 + 4 * s.masks.broken.size() + 8 * s.supportAnchors.size() +
                   (captured.Physics() ? 24 + captured.Physics()->participant.Value().size() : 0);
        }
    }  // namespace Detail

    /** @copydoc EncodeDestructionSaveState */
    Result<Runtime::CanonicalEncodedValue> EncodeDestructionSaveState(const CapturedDestructionState &captured,
                                                                      const Runtime::CanonicalCodecLimits &limits) {
        using Return = Result<Runtime::CanonicalEncodedValue>;
        if (captured.Fence().cancellation.IsCancellationRequested())
            return Return::Failure(MakeError(DestructionErrors::CancelledBeforeCommit));
        if (Detail::DestructionSavePayloadBytes(captured) > limits.maximumBytes)
            return Return::Failure(MakeError(DestructionErrors::LimitExceeded));
        const auto &s = captured.State();
        Runtime::CanonicalValueWriter writer{limits};
        const auto target = SerializeDestructionHandle(s.target);
        const auto content = SerializeFractureArtifactContentIdentity(s.content);
        if (auto v = Writes({writer.WriteUInt32(s.schemaVersion), writer.WriteBytes(std::as_bytes(std::span{target})),
                             writer.WriteBytes(std::as_bytes(std::span{content}))});
            v.HasError())
            return Return::Failure(v.ErrorValue());
        for (const auto value : {s.configuration.Value(), s.authority.Value(), s.revision.Value(), s.seed.value, s.seed.cursor})
            if (auto v = writer.WriteUInt64(value); v.HasError())
                return Return::Failure(v.ErrorValue());
        for (const auto value : {s.effectiveFeatures.bits, static_cast<std::uint32_t>(s.phase), s.healthQ16, s.seed.version,
                                 s.masks.chunkCount, s.supportCursor})
            if (auto v = writer.WriteUInt32(value); v.HasError())
                return Return::Failure(v.ErrorValue());
        for (const auto *mask : {&s.masks.broken, &s.masks.active, &s.masks.supported, &s.masks.dormant})
            if (auto v = writer.WriteBytes(*mask); v.HasError())
                return Return::Failure(v.ErrorValue());
        if (auto v = writer.WriteUInt32(static_cast<std::uint32_t>(s.supportAnchors.size())); v.HasError())
            return Return::Failure(v.ErrorValue());
        for (const auto id : s.supportAnchors)
            if (auto v = writer.WriteUInt64(id.Value()); v.HasError())
                return Return::Failure(v.ErrorValue());
        const auto &f = captured.Fence();
        for (const auto value : {f.sceneIncarnation, f.sceneRevision, f.epoch.value, f.capabilityRevision.Value()})
            if (auto v = writer.WriteUInt64(value); v.HasError())
                return Return::Failure(v.ErrorValue());
        if (auto v = writer.WriteBool(captured.Physics().has_value()); v.HasError())
            return Return::Failure(v.ErrorValue());
        if (captured.Physics()) {
            if (auto v = Writes({writer.WriteBytes(std::as_bytes(std::span{captured.Physics()->participant.Value().data(),
                                                                           captured.Physics()->participant.Value().size()})),
                                 writer.WriteBytes(std::as_bytes(std::span{captured.Physics()->checkpoint.Bytes()}))});
                v.HasError())
                return Return::Failure(v.ErrorValue());
        }
        return std::move(writer).Finalize();
    }

    /** @copydoc DecodeDestructionSaveState */
    Result<DecodedDestructionSaveState> DecodeDestructionSaveState(Runtime::CanonicalValueReader source,
                                                                   const DestructionCanonicalArtifactView &artifact,
                                                                   const DestructionCanonicalLimits &limits) {
        using Return = Result<DecodedDestructionSaveState>;
        if (!limits.maximumChunks || limits.maximumChunks > DestructionHardLimits::ChunksPerDestructible ||
            limits.maximumAnchors > DestructionHardLimits::ChunksPerDestructible ||
            limits.maximumBytes < Detail::MinimumDestructionSavePayloadBytes || limits.maximumBytes > 16384)
            return Return::Failure(MakeError(DestructionErrors::LimitExceeded));
        const auto start = source.ByteOffset();
        try {
            auto header = ReadStateHeader(source);
            if (header.HasError())
                return Return::Failure(header.ErrorValue());
            DecodedDestructionSaveState record{std::move(header).Value(), {}, {}};
            if (auto v = ReadCollections(source, record.state, limits); v.HasError())
                return Return::Failure(v.ErrorValue());
            if (auto v = ValidateCanonicalDestructionSnapshot(record.state, artifact, limits); v.HasError())
                return Return::Failure(v.ErrorValue());
            if (auto v = ReadCapture(source, record, artifact); v.HasError())
                return Return::Failure(v.ErrorValue());
            if (auto v = Detail::ValidateDestructionPhysicsCapture(record.state, artifact, record.fence, record.physics); v.HasError())
                return Return::Failure(v.ErrorValue());
            if (auto v = source.RequireFinished(); v.HasError())
                return Return::Failure(v.ErrorValue());
            if (source.ByteOffset() - start > limits.maximumBytes)
                return Return::Failure(MakeError(DestructionErrors::LimitExceeded));
            return Return::Success(std::move(record));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(DestructionErrors::LimitExceeded));
        }
    }
}  // namespace Horo::Destruction
