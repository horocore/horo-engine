#include "DestructionReplicationFields.h"
#include "Horo/Destruction/DestructionErrors.h"
#include "Horo/Destruction/DestructionReplication.h"
#include "Horo/Network/NetworkErrors.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace Horo::Destruction {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Fail(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Fixed network-order writer; the caller has already checked the complete byte envelope. */
        class Writer final {
        public:
            void U8(const std::uint8_t value) {
                bytes_.push_back(static_cast<std::byte>(value));
            }

            void U32(const std::uint32_t value) {
                for (int shift = 24; shift >= 0; shift -= 8)
                    U8(static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
            }

            void U64(const std::uint64_t value) {
                for (int shift = 56; shift >= 0; shift -= 8)
                    U8(static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
            }

            void Bytes(const std::span<const std::byte> bytes) {
                bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
            }

            template <std::size_t N> void Octets(const std::array<std::uint8_t, N> &bytes) {
                for (const std::uint8_t value : bytes)
                    U8(value);
            }

            [[nodiscard]] std::vector<std::byte> Finish() && {
                return std::move(bytes_);
            }

        private:
            std::vector<std::byte> bytes_;
        };

        /** @brief No-allocation checked network-order reader for one complete field. */
        class Reader final {
        public:
            explicit Reader(const std::span<const std::byte> bytes) : bytes_(bytes) {}

            [[nodiscard]] bool U8(std::uint8_t &value) {
                if (position_ >= bytes_.size())
                    return false;
                value = std::to_integer<std::uint8_t>(bytes_[position_++]);
                return true;
            }

            [[nodiscard]] bool U32(std::uint32_t &value) {
                value = 0;
                for (int index = 0; index < 4; ++index) {
                    std::uint8_t octet{};
                    if (!U8(octet))
                        return false;
                    value = (value << 8U) | octet;
                }
                return true;
            }

            [[nodiscard]] bool U64(std::uint64_t &value) {
                value = 0;
                for (int index = 0; index < 8; ++index) {
                    std::uint8_t octet{};
                    if (!U8(octet))
                        return false;
                    value = (value << 8U) | octet;
                }
                return true;
            }

            template <std::size_t N> [[nodiscard]] bool Octets(std::array<std::uint8_t, N> &value) {
                for (auto &octet : value) {
                    if (!U8(octet))
                        return false;
                }
                return true;
            }

            [[nodiscard]] bool Bytes(const std::size_t count, std::vector<std::byte> &value) {
                if (count > bytes_.size() - position_)
                    return false;
                value.assign(bytes_.begin() + static_cast<std::ptrdiff_t>(position_),
                             bytes_.begin() + static_cast<std::ptrdiff_t>(position_ + count));
                position_ += count;
                return true;
            }

            [[nodiscard]] bool Complete() const noexcept {
                return position_ == bytes_.size();
            }

        private:
            std::span<const std::byte> bytes_;
            std::size_t position_{};
        };

        [[nodiscard]] bool ValidLimits(const DestructionReplicationLimits &limits) noexcept {
            return limits.maximumChunks > 0 && limits.maximumChunks <= DestructionHardLimits::ChunksPerDestructible &&
                   limits.maximumAnchors <= DestructionHardLimits::ChunksPerDestructible &&
                   limits.maximumBytes >= Detail::FixedFieldBytes && limits.maximumBytes <= Detail::MaximumPayloadBytes;
        }

        [[nodiscard]] bool AnyBits(const std::span<const std::byte> mask) noexcept {
            return std::ranges::any_of(mask, [](const std::byte value) {
                return value != std::byte{};
            });
        }

        [[nodiscard]] bool ValidMask(const std::span<const std::byte> mask, const std::uint32_t count) noexcept {
            if (const std::size_t bytes = (static_cast<std::size_t>(count) + 7U) / 8U; mask.size() != bytes)
                return false;
            if (count % 8U == 0)
                return true;
            const auto highBits = std::byte{0xFF} << (count % 8U);
            return (mask.back() & highBits) == std::byte{};
        }

        [[nodiscard]] Result<void> ValidateState(const DestructionReplicationState &state,
                                                 const DestructionReplicationArtifactView &artifact,
                                                 const DestructionReplicationLimits &limits) {
            if (!ValidLimits(limits))
                return Fail<void>(DestructionErrors::ReplicationLimitExceeded);
            if (!state.target.IsValid() || !state.content.IsValid() || !state.configuration.IsValid() ||
                !state.effectiveFeatures.IsValid() || !state.effectiveFeatures.Contains(DestructionFeature::AuthoritativeReplication) ||
                !state.authority.IsValid() || !state.revision.IsValid() || state.seed.version == 0 ||
                state.phase > DestructionStatePhase::Destroyed)
                return Fail<void>(DestructionErrors::ReplicationInvalid);
            if (state.masks.chunkCount == 0 || state.masks.chunkCount > limits.maximumChunks ||
                state.supportAnchors.size() > limits.maximumAnchors)
                return Fail<void>(DestructionErrors::ReplicationLimitExceeded);
            if (artifact.content != state.content || artifact.canonicalChunks.size() != state.masks.chunkCount)
                return Fail<void>(DestructionErrors::ReplicationIncompatible);
            const auto &masks = state.masks;
            if (!ValidMask(masks.broken, masks.chunkCount) || !ValidMask(masks.active, masks.chunkCount) ||
                !ValidMask(masks.supported, masks.chunkCount) || !ValidMask(masks.dormant, masks.chunkCount))
                return Fail<void>(DestructionErrors::ReplicationInvalidChunkMask);
            for (std::size_t index = 0; index < masks.broken.size(); ++index) {
                if ((masks.active[index] & masks.dormant[index]) != std::byte{} ||
                    ((masks.active[index] | masks.dormant[index]) & ~masks.broken[index]) != std::byte{})
                    return Fail<void>(DestructionErrors::ReplicationInvalidChunkMask);
            }
            if (state.phase == DestructionStatePhase::Intact && (AnyBits(masks.broken) || AnyBits(masks.active) || AnyBits(masks.dormant)))
                return Fail<void>(DestructionErrors::ReplicationInvalidChunkMask);
            if (!std::ranges::is_sorted(state.supportAnchors) ||
                std::ranges::adjacent_find(state.supportAnchors) != state.supportAnchors.end() ||
                std::ranges::any_of(state.supportAnchors, [](const DestructionChunkId id) {
                return !id.IsValid();
            }))
                return Fail<void>(DestructionErrors::ReplicationInvalid);
            for (const DestructionChunkId anchor : state.supportAnchors) {
                if (std::ranges::find(artifact.canonicalChunks, anchor) == artifact.canonicalChunks.end())
                    return Fail<void>(DestructionErrors::ReplicationIncompatible);
            }
            if (const std::size_t payloadBytes = Detail::FixedFieldBytes + 4U * masks.broken.size() + 8U * state.supportAnchors.size();
                payloadBytes > limits.maximumBytes)
                return Fail<void>(DestructionErrors::ReplicationLimitExceeded);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateWriter(const Network::ReplicationRoleBinding &writer, const DestructionReplicationState &state) {
            if (!writer.IsValid() || writer.schema != Detail::SchemaId() || writer.schemaVersion != DestructionReplicationVersion)
                return Fail<void>(DestructionErrors::ReplicationIncompatible);
            if (writer.object.Epoch() != state.authority)
                return Fail<void>(DestructionErrors::ReplicationStaleAuthority);
            if (writer.role != Network::ReplicationExecutionRole::AuthorityServer)
                return Fail<void>(Network::NetworkErrors::ReplicationAuthorityDenied);
            return Result<void>::Success();
        }

        /** @brief Builds fields in published FieldId order from a completely validated detached value. */
        [[nodiscard]] DestructionReplicationPayload WriteFields(const DestructionReplicationState &state) {
            DestructionReplicationPayload payload{.schema = Detail::SchemaId(), .version = DestructionReplicationVersion};
            payload.fields.reserve(Detail::FieldValues.size());
            const auto add = [&payload](const std::size_t index, Writer writer) {
                auto &field = payload.fields.emplace_back();
                field.id = Detail::FieldId(index);
                field.canonical = std::move(writer).Finish();
            };
            Writer target;
            target.Octets(SerializeDestructionHandle(state.target));
            add(0, std::move(target));
            Writer content;
            content.Octets(SerializeFractureArtifactContentIdentity(state.content));
            add(1, std::move(content));
            Writer configuration;
            configuration.U64(state.configuration.Value());
            add(2, std::move(configuration));
            Writer authority;
            authority.U64(state.authority.Value());
            add(3, std::move(authority));
            Writer revision;
            revision.U64(state.revision.Value());
            add(4, std::move(revision));
            Writer phaseHealth;
            phaseHealth.U8(static_cast<std::uint8_t>(state.phase));
            phaseHealth.U32(state.healthQ16);
            add(5, std::move(phaseHealth));
            Writer seed;
            seed.U32(state.seed.version);
            seed.U64(state.seed.value);
            seed.U64(state.seed.cursor);
            add(6, std::move(seed));
            Writer masks;
            masks.U32(state.masks.chunkCount);
            masks.Bytes(state.masks.broken);
            masks.Bytes(state.masks.active);
            masks.Bytes(state.masks.supported);
            masks.Bytes(state.masks.dormant);
            add(7, std::move(masks));
            Writer support;
            support.U32(static_cast<std::uint32_t>(state.supportAnchors.size()));
            for (const DestructionChunkId anchor : state.supportAnchors)
                support.U64(anchor.Value());
            support.U32(state.supportCursor);
            add(8, std::move(support));
            Writer features;
            features.U32(state.effectiveFeatures.bits);
            add(9, std::move(features));
            return payload;
        }

        /** @brief Rejects malformed field framing and excessive claimed work before any decode allocation. */
        [[nodiscard]] Result<void> ValidateFields(const DestructionReplicationPayload &payload,
                                                  const DestructionReplicationLimits &limits) {
            if (payload.schema != Detail::SchemaId() || payload.version != DestructionReplicationVersion)
                return Fail<void>(DestructionErrors::ReplicationIncompatible);
            if (!ValidLimits(limits) || payload.fields.size() != Detail::FieldValues.size())
                return Fail<void>(DestructionErrors::ReplicationInvalid);
            std::size_t total{};
            for (std::size_t index = 0; index < payload.fields.size(); ++index) {
                const auto &field = payload.fields[index];
                if (field.id != Detail::FieldId(index))
                    return Fail<void>(DestructionErrors::ReplicationInvalid);
                if (field.canonical.size() > Detail::MaximumFieldBytes[index])
                    return Fail<void>(DestructionErrors::ReplicationLimitExceeded);
                if (field.canonical.size() > limits.maximumBytes - total)
                    return Fail<void>(DestructionErrors::ReplicationLimitExceeded);
                total += field.canonical.size();
            }
            return Result<void>::Success();
        }

        /** @brief Reads exact DFR owner and artifact identity without retaining input views. */
        [[nodiscard]] Result<void> ReadIdentity(const DestructionReplicationPayload &payload, DestructionReplicationState &state) {
            Reader target{payload.fields[0].canonical};
            SerializedDestructionHandle targetBytes{};
            if (!target.Octets(targetBytes) || !target.Complete())
                return Fail<void>(DestructionErrors::ReplicationInvalid);
            auto decodedTarget = DeserializeDestructionHandle(targetBytes);
            if (decodedTarget.HasError())
                return Fail<void>(DestructionErrors::ReplicationInvalid);
            state.target = decodedTarget.Value();
            Reader content{payload.fields[1].canonical};
            SerializedFractureArtifactContentIdentity contentBytes{};
            if (!content.Octets(contentBytes) || !content.Complete())
                return Fail<void>(DestructionErrors::ReplicationInvalid);
            auto decodedContent = DeserializeFractureArtifactContentIdentity(contentBytes);
            if (decodedContent.HasError())
                return Fail<void>(DestructionErrors::ReplicationInvalid);
            state.content = decodedContent.Value();
            return Result<void>::Success();
        }

        /** @brief Reads authority, revision and deterministic scalar semantics. */
        [[nodiscard]] Result<void> ReadScalars(const DestructionReplicationPayload &payload, DestructionReplicationState &state) {
            Reader configuration{payload.fields[2].canonical};
            std::uint64_t configurationValue{};
            if (!configuration.U64(configurationValue) || !configuration.Complete())
                return Fail<void>(DestructionErrors::ReplicationInvalid);
            auto decodedConfiguration = DestructionConfigurationRevision::Create(configurationValue);
            if (decodedConfiguration.HasError())
                return Fail<void>(DestructionErrors::ReplicationInvalid);
            state.configuration = decodedConfiguration.Value();
            Reader authority{payload.fields[3].canonical};
            std::uint64_t authorityValue{};
            if (!authority.U64(authorityValue) || !authority.Complete())
                return Fail<void>(DestructionErrors::ReplicationInvalid);
            auto decodedAuthority = Network::ReplicationAuthorityEpoch::Create(authorityValue);
            if (decodedAuthority.HasError())
                return Fail<void>(DestructionErrors::ReplicationInvalid);
            state.authority = decodedAuthority.Value();
            Reader revision{payload.fields[4].canonical};
            std::uint64_t revisionValue{};
            if (!revision.U64(revisionValue) || !revision.Complete())
                return Fail<void>(DestructionErrors::ReplicationInvalid);
            auto decodedRevision = DestructionStateRevision::Create(revisionValue);
            if (decodedRevision.HasError())
                return Fail<void>(DestructionErrors::ReplicationInvalid);
            state.revision = decodedRevision.Value();
            Reader phaseHealth{payload.fields[5].canonical};
            std::uint8_t phase{};
            if (!phaseHealth.U8(phase) || !phaseHealth.U32(state.healthQ16) || !phaseHealth.Complete() ||
                phase > static_cast<std::uint8_t>(DestructionStatePhase::Destroyed))
                return Fail<void>(DestructionErrors::ReplicationInvalid);
            state.phase = static_cast<DestructionStatePhase>(phase);
            if (Reader seed{payload.fields[6].canonical};
                !seed.U32(state.seed.version) || !seed.U64(state.seed.value) || !seed.U64(state.seed.cursor) || !seed.Complete())
                return Fail<void>(DestructionErrors::ReplicationInvalid);
            if (Reader features{payload.fields[9].canonical}; !features.U32(state.effectiveFeatures.bits) || !features.Complete())
                return Fail<void>(DestructionErrors::ReplicationInvalid);
            return Result<void>::Success();
        }

        /** @brief Uses the bounded claimed count to derive exact mask length before copying payload bytes. */
        [[nodiscard]] Result<void> ReadMasks(const DestructionReplicationPayload &payload, DestructionReplicationState &state,
                                             const DestructionReplicationLimits &limits) {
            Reader reader{payload.fields[7].canonical};
            if (!reader.U32(state.masks.chunkCount) || state.masks.chunkCount == 0 || state.masks.chunkCount > limits.maximumChunks)
                return Fail<void>(DestructionErrors::ReplicationLimitExceeded);
            if (const std::size_t bytes = (static_cast<std::size_t>(state.masks.chunkCount) + 7U) / 8U;
                !reader.Bytes(bytes, state.masks.broken) || !reader.Bytes(bytes, state.masks.active) ||
                !reader.Bytes(bytes, state.masks.supported) || !reader.Bytes(bytes, state.masks.dormant) || !reader.Complete())
                return Fail<void>(DestructionErrors::ReplicationInvalidChunkMask);
            return Result<void>::Success();
        }

        /** @brief Admits only finite sorted stable anchor identities, never artifact-local ordinals. */
        [[nodiscard]] Result<void> ReadSupport(const DestructionReplicationPayload &payload, DestructionReplicationState &state,
                                               const DestructionReplicationLimits &limits) {
            Reader reader{payload.fields[8].canonical};
            std::uint32_t count{};
            if (!reader.U32(count) || count > limits.maximumAnchors)
                return Fail<void>(DestructionErrors::ReplicationLimitExceeded);
            if (payload.fields[8].canonical.size() != 8U + 8U * count)
                return Fail<void>(DestructionErrors::ReplicationInvalid);
            state.supportAnchors.reserve(count);
            for (std::uint32_t index = 0; index < count; ++index) {
                std::uint64_t value{};
                if (!reader.U64(value))
                    return Fail<void>(DestructionErrors::ReplicationInvalid);
                auto anchor = DestructionChunkId::Create(value);
                if (anchor.HasError())
                    return Fail<void>(DestructionErrors::ReplicationInvalid);
                state.supportAnchors.push_back(anchor.Value());
            }
            if (!reader.U32(state.supportCursor) || !reader.Complete())
                return Fail<void>(DestructionErrors::ReplicationInvalid);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateReceiver(const Network::ReplicationRoleBinding &receiver,
                                                    const DestructionReplicationFence &fence) {
            if (!receiver.IsValid() || !fence.session.IsValid() || !fence.object.IsValid() || !fence.target.IsValid() ||
                !fence.content.IsValid() || !fence.configuration.IsValid() || !fence.effectiveFeatures.IsValid() ||
                !fence.effectiveFeatures.Contains(DestructionFeature::AuthoritativeReplication) || !fence.authority.IsValid() ||
                receiver.session != fence.session || receiver.object != fence.object || receiver.schema != Detail::SchemaId() ||
                receiver.schemaVersion != DestructionReplicationVersion)
                return Fail<void>(DestructionErrors::ReplicationIncompatible);
            if (receiver.role != Network::ReplicationExecutionRole::AutonomousClient &&
                receiver.role != Network::ReplicationExecutionRole::SimulatedClient)
                return Fail<void>(Network::NetworkErrors::ReplicationAuthorityDenied);
            if (fence.object.Epoch() != fence.authority)
                return Fail<void>(DestructionErrors::ReplicationStaleAuthority);
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc EncodeDestructionReplication */
    Result<DestructionReplicationPayload> EncodeDestructionReplication(const DestructionReplicationState &state,
                                                                       const DestructionReplicationArtifactView &artifact,
                                                                       const Network::ReplicationRoleBinding &writer,
                                                                       const DestructionReplicationLimits &limits) {
        if (const auto valid = ValidateWriter(writer, state); valid.HasError())
            return Result<DestructionReplicationPayload>::Failure(valid.ErrorValue());
        if (const auto valid = ValidateState(state, artifact, limits); valid.HasError())
            return Result<DestructionReplicationPayload>::Failure(valid.ErrorValue());
        try {
            return Result<DestructionReplicationPayload>::Success(WriteFields(state));
        } catch (const std::bad_alloc &) {
            return Fail<DestructionReplicationPayload>(DestructionErrors::ReplicationLimitExceeded);
        }
    }

    /** @copydoc DecodeDestructionReplication */
    Result<DestructionReplicationState> DecodeDestructionReplication(const DestructionReplicationPayload &payload,
                                                                     const DestructionReplicationFence &fence,
                                                                     const DestructionReplicationArtifactView &artifact,
                                                                     const Network::ReplicationRoleBinding &receiver,
                                                                     const DestructionReplicationLimits &limits) {
        if (const auto valid = ValidateReceiver(receiver, fence); valid.HasError())
            return Result<DestructionReplicationState>::Failure(valid.ErrorValue());
        if (const auto valid = ValidateFields(payload, limits); valid.HasError())
            return Result<DestructionReplicationState>::Failure(valid.ErrorValue());
        try {
            DestructionReplicationState state;
            if (const auto valid = ReadIdentity(payload, state); valid.HasError())
                return Result<DestructionReplicationState>::Failure(valid.ErrorValue());
            if (const auto valid = ReadScalars(payload, state); valid.HasError())
                return Result<DestructionReplicationState>::Failure(valid.ErrorValue());
            if (const auto valid = ReadMasks(payload, state, limits); valid.HasError())
                return Result<DestructionReplicationState>::Failure(valid.ErrorValue());
            if (const auto valid = ReadSupport(payload, state, limits); valid.HasError())
                return Result<DestructionReplicationState>::Failure(valid.ErrorValue());
            if (const auto valid = ValidateState(state, artifact, limits); valid.HasError())
                return Result<DestructionReplicationState>::Failure(valid.ErrorValue());
            if (state.authority != fence.authority)
                return Fail<DestructionReplicationState>(DestructionErrors::ReplicationStaleAuthority);
            if (state.target != fence.target)
                return Fail<DestructionReplicationState>(DestructionErrors::StaleGeneration);
            if (state.content != fence.content)
                return Fail<DestructionReplicationState>(DestructionErrors::ReplicationIncompatible);
            if (state.configuration != fence.configuration)
                return Fail<DestructionReplicationState>(DestructionErrors::StaleConfiguration);
            if (state.effectiveFeatures != fence.effectiveFeatures)
                return Fail<DestructionReplicationState>(DestructionErrors::ReplicationIncompatible);
            if (fence.currentRevision.IsValid() && state.revision <= fence.currentRevision)
                return Fail<DestructionReplicationState>(DestructionErrors::StaleRevision);
            return Result<DestructionReplicationState>::Success(std::move(state));
        } catch (const std::bad_alloc &) {
            return Fail<DestructionReplicationState>(DestructionErrors::ReplicationLimitExceeded);
        }
    }
}  // namespace Horo::Destruction
