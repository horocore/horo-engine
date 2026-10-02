#include "Horo/Destruction/ChunkCollisionArtifact.h"

#include <algorithm>
#include <concepts>
#include <new>

namespace Horo::Destruction {
    namespace {
        constexpr std::array<std::uint8_t, 4> Magic{'D', 'F', 'C', '1'};
        constexpr std::uint32_t Schema = 1;
        constexpr std::uint64_t HeaderBytes = 165;
        constexpr std::uint64_t EntryBytes = 80;
        using Output = Result<std::shared_ptr<const ChunkCollisionArtifactSet>>;

        /** @brief Versioned little-endian bundle writer with admission performed before reserve. */
        class Writer final {
        public:
            explicit Writer(std::uint64_t capacity) {
                bytes_.reserve(static_cast<std::size_t>(capacity));
            }

            void Bytes(std::span<const std::uint8_t> bytes) {
                bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
            }

            void U64(std::uint64_t value) {
                for (unsigned shift = 0; shift < 64; shift += 8)
                    bytes_.push_back(static_cast<std::uint8_t>(value >> shift));
            }

            void U32(std::uint32_t value) {
                for (unsigned shift = 0; shift < 32; shift += 8)
                    bytes_.push_back(static_cast<std::uint8_t>(value >> shift));
            }

            void U8(std::uint8_t value) {
                bytes_.push_back(value);
            }

            [[nodiscard]] std::vector<std::uint8_t> Take() && {
                return std::move(bytes_);
            }

        private:
            std::vector<std::uint8_t> bytes_;
        };

        /** @brief Extent-checked bundle reader; no source or cook contract is consulted. */
        class Reader final {
        public:
            explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

            bool Bytes(std::span<std::uint8_t> output) {
                std::span<const std::uint8_t> encoded;
                if (!Payload(output.size(), encoded))
                    return false;
                std::ranges::copy(encoded, output.begin());
                return true;
            }

            bool U64(std::uint64_t &value) {
                return Unsigned(value);
            }

            bool U32(std::uint32_t &value) {
                return Unsigned(value);
            }

            bool U8(std::uint8_t &value) {
                return Unsigned(value);
            }

            bool Payload(std::uint64_t size, std::span<const std::uint8_t> &output) {
                if (size > bytes_.size())
                    return false;
                output = bytes_.first(static_cast<std::size_t>(size));
                bytes_ = bytes_.subspan(static_cast<std::size_t>(size));
                return true;
            }

            [[nodiscard]] bool Finished() const noexcept {
                return bytes_.empty();
            }

        private:
            /** @brief Reads one complete little-endian scalar without changing the output on truncation. */
            template <std::unsigned_integral Integer> bool Unsigned(Integer &value) {
                std::span<const std::uint8_t> encoded;
                if (!Payload(sizeof(Integer), encoded))
                    return false;
                value = 0;
                for (unsigned index = 0; index < sizeof(Integer); ++index)
                    value |= static_cast<Integer>(encoded[index]) << (index * 8U);
                return true;
            }

            std::span<const std::uint8_t> bytes_;
        };

        /** @brief Captured bounded header evidence, independent of platform-native layout. */
        struct Header final {
            FractureArtifactContentIdentity content;
            Sha256Digest mesh;
            Physics::PhysicsShapeCookTargetDigest target;
            DestructionFeatureTier tier{};
            Sha256Digest configuration;
            std::uint32_t count{};
        };

        /** @brief Validates exact owner reference and immutable bundle integrity before allocation. */
        [[nodiscard]] Result<void> ValidateInput(const ChunkCollisionBundleReference &reference, std::span<const std::uint8_t> payload,
                                                 const DestructionLimits &limits) {
            if (!reference.content.IsValid() || payload.size() < HeaderBytes)
                return Result<void>::Failure(MakeError(DestructionErrors::DescriptorInvalid));
            if (limits.maximumArtifactBytes == 0 || limits.maximumArtifactBytes > DestructionHardLimits::ArtifactBytes ||
                limits.maximumChunksPerDestructible == 0 ||
                limits.maximumChunksPerDestructible > DestructionHardLimits::ChunksPerDestructible ||
                limits.maximumWorkItemsPerTransition == 0 ||
                limits.maximumWorkItemsPerTransition > DestructionHardLimits::WorkItemsPerTransition ||
                payload.size() > limits.maximumArtifactBytes)
                return Result<void>::Failure(MakeError(DestructionErrors::LimitExceeded));
            if (ComputeSha256(std::as_bytes(payload)) != reference.payloadDigest)
                return Result<void>::Failure(MakeError(DestructionErrors::DescriptorInvalid, "Collision bundle digest mismatch."));
            return Result<void>::Success();
        }

        /** @brief Decodes version, content, target, tier and count with complete extent checks. */
        [[nodiscard]] Result<Header> ReadHeader(Reader &reader, const ChunkCollisionBundleReference &reference,
                                                const DestructionLimits &limits, std::size_t payloadBytes) {
            std::array<std::uint8_t, 4> magic{};
            std::uint32_t schema{};
            SerializedFractureArtifactContentIdentity content{};
            std::uint8_t tier{};
            Header header;
            if (!reader.Bytes(magic) || !reader.U32(schema) || !reader.Bytes(content) || !reader.Bytes(header.mesh.bytes) ||
                !reader.Bytes(header.target.digest.bytes) || !reader.U8(tier) || !reader.Bytes(header.configuration.bytes) ||
                !reader.U32(header.count))
                return Result<Header>::Failure(MakeError(DestructionErrors::DescriptorInvalid, "Truncated collision bundle header."));
            const auto decoded = DeserializeFractureArtifactContentIdentity(content);
            if (decoded.HasError())
                return Result<Header>::Failure(decoded.ErrorValue());
            header.content = decoded.Value();
            header.tier = static_cast<DestructionFeatureTier>(tier);
            const auto profile = GetDestructionTierProfile(header.tier);
            if (profile.HasError())
                return Result<Header>::Failure(profile.ErrorValue());
            if (magic != Magic || schema != Schema || header.content != reference.content || header.mesh != reference.meshDigest ||
                header.target != reference.target)
                return Result<Header>::Failure(
                    MakeError(DestructionErrors::DescriptorInvalid, "Collision bundle schema or exact revision mismatch."));
            if (header.count == 0 || header.count > limits.maximumChunksPerDestructible ||
                header.count > profile.Value().limits.maximumChunksPerDestructible ||
                header.count > (payloadBytes - HeaderBytes) / EntryBytes)
                return Result<Header>::Failure(MakeError(DestructionErrors::LimitExceeded));
            return Result<Header>::Success(header);
        }

        /** @brief Verifies one cooked compound with bounded work before returning owned package bytes. */
        [[nodiscard]] Result<ChunkCollisionArtifact> ReadShape(Reader &reader, const Header &header, const DestructionLimits &limits,
                                                               std::uint64_t &work) {
            std::uint64_t chunk{};
            std::uint64_t size{};
            Sha256Digest key;
            Sha256Digest digest;
            std::span<const std::uint8_t> payload;
            if (!reader.U64(chunk) || !reader.Bytes(key.bytes) || !reader.Bytes(digest.bytes) || !reader.U64(size) ||
                !reader.Payload(size, payload))
                return Result<ChunkCollisionArtifact>::Failure(
                    MakeError(DestructionErrors::DescriptorInvalid, "Invalid collision bundle entry extent."));
            auto identity = DestructionChunkId::Create(chunk);
            if (identity.HasError())
                return Result<ChunkCollisionArtifact>::Failure(identity.ErrorValue());
            // Qualified hull verification examines at most 256 points per encoded triangle.
            constexpr auto amplification = Physics::PhysicsConvexHullCookLimits::MaximumHullVertices;
            if (size > (limits.maximumWorkItemsPerTransition - work) / amplification)
                return Result<ChunkCollisionArtifact>::Failure(MakeError(DestructionErrors::LimitExceeded));
            work += size * amplification;
            Physics::PhysicsCookedShapeDescriptor descriptor{.asset = header.content.Asset().Asset(),
                                                             .subresource = Physics::PhysicsShapeSubresourceId::FromValue(chunk),
                                                             .kind = Physics::PhysicsCookedShapeKind::Compound,
                                                             .cacheKeyDigest = key,
                                                             .payloadDigest = digest,
                                                             .target = header.target};
            const auto loaded = Physics::LoadCookedPhysicsCompound(descriptor, header.target, payload);
            if (loaded.HasError())
                return Result<ChunkCollisionArtifact>::Failure(loaded.ErrorValue());
            if (loaded.Value().dependencyDigest != header.configuration)
                return Result<ChunkCollisionArtifact>::Failure(
                    MakeError(DestructionErrors::DescriptorInvalid, "Mixed collision configuration generations."));
            return Result<ChunkCollisionArtifact>::Success({identity.Value(), {descriptor, {payload.begin(), payload.end()}}});
        }
    }  // namespace

    /** @copydoc EncodeChunkCollisionArtifacts */
    Result<std::vector<std::uint8_t>> EncodeChunkCollisionArtifacts(const ChunkCollisionArtifactSet &artifacts,
                                                                    std::uint64_t maximumBytes) {
        using Encoded = Result<std::vector<std::uint8_t>>;
        if (maximumBytes < HeaderBytes || maximumBytes > DestructionHardLimits::ArtifactBytes)
            return Encoded::Failure(MakeError(DestructionErrors::LimitExceeded));
        std::uint64_t bytes = HeaderBytes;
        for (const auto &artifact : artifacts.Shapes()) {
            if (EntryBytes > maximumBytes - bytes || artifact.shape.payload.size() > maximumBytes - bytes - EntryBytes)
                return Encoded::Failure(MakeError(DestructionErrors::LimitExceeded));
            bytes += EntryBytes + artifact.shape.payload.size();
        }
        try {
            Writer writer{bytes};
            writer.Bytes(Magic);
            writer.U32(Schema);
            writer.Bytes(SerializeFractureArtifactContentIdentity(artifacts.Content()));
            writer.Bytes(artifacts.MeshDigest().bytes);
            writer.Bytes(artifacts.Target().digest.bytes);
            writer.U8(static_cast<std::uint8_t>(artifacts.Tier()));
            writer.Bytes(artifacts.ConfigurationDigest().bytes);
            writer.U32(static_cast<std::uint32_t>(artifacts.Shapes().size()));
            for (const auto &artifact : artifacts.Shapes()) {
                writer.U64(artifact.chunk.Value());
                writer.Bytes(artifact.shape.descriptor.cacheKeyDigest->bytes);
                writer.Bytes(artifact.shape.descriptor.payloadDigest->bytes);
                writer.U64(artifact.shape.payload.size());
                writer.Bytes(artifact.shape.payload);
            }
            return Encoded::Success(std::move(writer).Take());
        } catch (const std::bad_alloc &) {
            return Encoded::Failure(MakeError(DestructionErrors::LimitExceeded));
        }
    }

    /** @copydoc LoadChunkCollisionArtifacts */
    Result<std::shared_ptr<const ChunkCollisionArtifactSet>> LoadChunkCollisionArtifacts(const ChunkCollisionBundleReference &reference,
                                                                                         std::span<const std::uint8_t> payload,
                                                                                         const DestructionLimits &limits) {
        if (auto valid = ValidateInput(reference, payload, limits); valid.HasError())
            return Output::Failure(valid.ErrorValue());
        try {
            Reader reader{payload};
            auto header = ReadHeader(reader, reference, limits, payload.size());
            if (header.HasError())
                return Output::Failure(header.ErrorValue());
            auto candidate = std::make_shared<ChunkCollisionArtifactSet>(ChunkCollisionArtifactSet::ConstructionKey());
            candidate->content_ = header.Value().content;
            candidate->meshDigest_ = header.Value().mesh;
            candidate->target_ = header.Value().target;
            candidate->tier_ = header.Value().tier;
            candidate->requestDigest_ = header.Value().configuration;
            candidate->shapes_.reserve(header.Value().count);
            std::uint64_t work = 0;
            DestructionChunkId previous;
            for (std::uint32_t i = 0; i < header.Value().count; ++i) {
                auto shape = ReadShape(reader, header.Value(), limits, work);
                if (shape.HasError())
                    return Output::Failure(shape.ErrorValue());
                if (shape.Value().chunk <= previous)
                    return Output::Failure(MakeError(DestructionErrors::DescriptorInvalid, "Noncanonical collision chunk order."));
                previous = shape.Value().chunk;
                candidate->shapes_.push_back(std::move(shape).Value());
            }
            if (!reader.Finished())
                return Output::Failure(MakeError(DestructionErrors::DescriptorInvalid, "Trailing collision bundle bytes."));
            candidate->bytes_ = payload.size();
            return Output::Success(std::move(candidate));
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(DestructionErrors::LimitExceeded));
        }
    }
}  // namespace Horo::Destruction
