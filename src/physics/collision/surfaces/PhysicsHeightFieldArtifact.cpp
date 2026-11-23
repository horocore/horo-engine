#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsHeightFieldCook.h"
#include "PhysicsHeightFieldCookInternal.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <new>
#include <ranges>
#include <utility>

namespace Horo::Physics {
    namespace HeightFieldDetail {
        class Reader final {
        public:
            explicit Reader(const std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

            [[nodiscard]] bool U8(std::uint8_t &value) noexcept {
                if (bytes_.empty())
                    return false;
                value = bytes_.front();
                bytes_ = bytes_.subspan(1);
                return true;
            }

            [[nodiscard]] bool U32(std::uint32_t &value) noexcept {
                return Integer(value);
            }

            [[nodiscard]] bool U64(std::uint64_t &value) noexcept {
                return Integer(value);
            }

            [[nodiscard]] bool Float(float &value) noexcept {
                std::uint32_t bits{};
                if (!U32(bits))
                    return false;
                value = std::bit_cast<float>(bits);
                return std::isfinite(value) && !(value == 0.0F && std::signbit(value));
            }

            [[nodiscard]] bool Bytes(const std::span<std::uint8_t> destination) noexcept {
                if (bytes_.size() < destination.size())
                    return false;
                std::ranges::copy(bytes_.first(destination.size()), destination.begin());
                bytes_ = bytes_.subspan(destination.size());
                return true;
            }

            [[nodiscard]] std::span<const std::uint8_t> Remaining() const noexcept {
                return bytes_;
            }

        private:
            template <typename IntegerType> [[nodiscard]] bool Integer(IntegerType &value) noexcept {
                if (bytes_.size() < sizeof(IntegerType))
                    return false;
                value = 0;
                for (std::size_t index = 0; index < sizeof(IntegerType); ++index)
                    value |= static_cast<IntegerType>(bytes_[index]) << (index * 8U);
                bytes_ = bytes_.subspan(sizeof(IntegerType));
                return true;
            }

            std::span<const std::uint8_t> bytes_;
        };

        [[nodiscard]] bool ReadLimits(Reader &reader, PhysicsHeightFieldCookLimits &limits) noexcept {
            return reader.U32(limits.maxDimension) && reader.U64(limits.maxSamples) && reader.U32(limits.maxMaterialSlots) &&
                   reader.U64(limits.maxPayloadBytes);
        }

        struct Header final {
            std::array<std::uint8_t, 4> magic{};
            std::uint32_t payloadVersion{};
            PhysicsHeightFieldCookSettings settings;
            std::array<std::uint8_t, 16> assetBytes{};
            std::uint64_t subresource{};
            Sha256Digest source;
            Sha256Digest key;
            PhysicsShapeCookTargetDigest target;
            Math::Aabb bounds;
            std::uint64_t payloadBytes{};
        };

        [[nodiscard]] bool ReadHeader(Reader &reader, Header &header) noexcept {
            if (!reader.Bytes(header.magic) || !reader.U32(header.payloadVersion) || !reader.U32(header.settings.schemaVersion) ||
                !reader.U32(header.settings.algorithmVersion) || !reader.Bytes(header.assetBytes) || !reader.U64(header.subresource) ||
                !reader.Bytes(header.source.bytes) || !reader.Bytes(header.key.bytes) || !reader.Bytes(header.target.digest.bytes) ||
                !ReadLimits(reader, header.settings.limits))
                return false;
            return reader.Float(header.bounds.minimum.x) && reader.Float(header.bounds.minimum.y) &&
                   reader.Float(header.bounds.minimum.z) && reader.Float(header.bounds.maximum.x) &&
                   reader.Float(header.bounds.maximum.y) && reader.Float(header.bounds.maximum.z) && reader.U64(header.payloadBytes);
        }

        [[nodiscard]] Result<void> ValidateHeader(const Header &header, const PhysicsCookedShapeDescriptor &descriptor,
                                                  const PhysicsShapeCookTargetDigest &target, const std::size_t payloadBytes,
                                                  const PhysicsHeightFieldCookLimits &limits) {
            if (header.magic != PayloadMagic || header.payloadVersion != PayloadVersion ||
                header.settings.schemaVersion != PhysicsHeightFieldCookSettings::CurrentSchemaVersion ||
                header.settings.algorithmVersion != PhysicsHeightFieldCookSettings::CurrentAlgorithmVersion ||
                !Bounded(header.settings.limits) || header.payloadBytes != payloadBytes || !header.bounds.IsValid() ||
                Assets::AssetId::FromBytes(header.assetBytes) != descriptor.asset ||
                PhysicsShapeSubresourceId::FromValue(header.subresource) != descriptor.subresource || header.target != target ||
                header.key != *descriptor.cacheKeyDigest || payloadBytes > limits.maxPayloadBytes ||
                payloadBytes > header.settings.limits.maxPayloadBytes)
                return Failure<void>(PhysicsErrors::ShapeArtifactInvalid, "Heightfield artifact envelope or exact identity is invalid.");
            if (CookKey(descriptor.asset, descriptor.subresource, header.source, header.settings, target) != header.key)
                return Failure<void>(PhysicsErrors::ShapeArtifactInvalid, "Heightfield artifact cook key is inconsistent.");
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ReadGridMetadata(Reader &reader, LoadedPhysicsHeightField &loaded, std::uint32_t &materialCount) {
            if (!reader.U32(loaded.width) || !reader.U32(loaded.height) || !reader.Float(loaded.origin.x) ||
                !reader.Float(loaded.origin.y) || !reader.Float(loaded.origin.z) || !reader.Float(loaded.spacingX) ||
                !reader.Float(loaded.spacingZ) || !reader.Float(loaded.sampleScaleY) || !reader.U32(materialCount))
                return Failure<void>(PhysicsErrors::ShapeArtifactInvalid, "Heightfield tile geometry header is truncated.");
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateGridExtent(const LoadedPhysicsHeightField &loaded,
                                                      const PhysicsHeightFieldCookLimits &cookLimits,
                                                      const PhysicsHeightFieldCookLimits &runtimeLimits, const std::uint32_t materialCount,
                                                      const std::size_t payloadBytes) {
            if (loaded.width < 2 || loaded.height < 2 || loaded.width > runtimeLimits.maxDimension ||
                loaded.height > runtimeLimits.maxDimension || loaded.width > cookLimits.maxDimension ||
                loaded.height > cookLimits.maxDimension || SampleCount(loaded.width, loaded.height) > runtimeLimits.maxSamples ||
                SampleCount(loaded.width, loaded.height) > cookLimits.maxSamples || materialCount == 0 ||
                materialCount > runtimeLimits.maxMaterialSlots || materialCount > cookLimits.maxMaterialSlots ||
                !ValidScale(loaded.origin, loaded.spacingX, loaded.spacingZ, loaded.sampleScaleY))
                return Failure<void>(PhysicsErrors::ShapeArtifactInvalid, "Heightfield tile dimensions or scale are invalid.");
            const auto samples = SampleCount(loaded.width, loaded.height);
            const auto cells = CellCount(loaded.width, loaded.height);
            if (const auto expectedBytes = HeaderBytes + 36ULL + samples * sizeof(float) +
                                           cells * (sizeof(std::uint8_t) + sizeof(std::uint64_t)) +
                                           static_cast<std::uint64_t>(materialCount) * sizeof(std::uint64_t);
                expectedBytes != payloadBytes)
                return Failure<void>(PhysicsErrors::ShapeArtifactInvalid, "Heightfield artifact table extent is inconsistent.");
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ReadGridSamplesAndHoles(Reader &reader, LoadedPhysicsHeightField &loaded) {
            const auto samples = SampleCount(loaded.width, loaded.height);
            const auto cells = CellCount(loaded.width, loaded.height);
            loaded.samples.resize(static_cast<std::size_t>(samples));
            loaded.cellHoles.resize(static_cast<std::size_t>(cells));
            for (auto &sample : loaded.samples) {
                if (!reader.Float(sample))
                    return Failure<void>(PhysicsErrors::ShapeArtifactInvalid, "Heightfield sample is invalid.");
            }
            for (auto &hole : loaded.cellHoles) {
                if (!reader.U8(hole))
                    return Failure<void>(PhysicsErrors::ShapeArtifactInvalid, "Heightfield hole table is truncated.");
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ReadGridMaterials(Reader &reader, LoadedPhysicsHeightField &loaded, const std::uint32_t materialCount) {
            loaded.cellMaterials.resize(static_cast<std::size_t>(CellCount(loaded.width, loaded.height)));
            loaded.materialSlots.resize(materialCount);
            for (auto &material : loaded.cellMaterials) {
                std::uint64_t value{};
                if (!reader.U64(value))
                    return Failure<void>(PhysicsErrors::ShapeArtifactInvalid, "Heightfield cell material table is truncated.");
                material = PhysicsMaterialSlotId::FromValue(value);
            }
            for (auto &material : loaded.materialSlots) {
                std::uint64_t value{};
                if (!reader.U64(value))
                    return Failure<void>(PhysicsErrors::ShapeArtifactInvalid, "Heightfield material table is truncated.");
                material = PhysicsMaterialSlotId::FromValue(value);
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateDecodedGrid(const LoadedPhysicsHeightField &loaded, const Math::Aabb &bounds,
                                                       const Reader &reader) {
            if (!reader.Remaining().empty())
                return Failure<void>(PhysicsErrors::ShapeArtifactInvalid, "Heightfield artifact has trailing bytes.");
            if (const auto mapping = ValidateMappings(loaded.cellHoles, loaded.cellMaterials, loaded.materialSlots); mapping.HasError())
                return Failure<void>(PhysicsErrors::ShapeArtifactInvalid, mapping.ErrorValue().message);
            if (const auto measured = ComputeBounds(
                    {loaded.width, loaded.height, loaded.origin, loaded.spacingX, loaded.spacingZ, loaded.sampleScaleY, loaded.samples});
                measured.HasError() || measured.Value().minimum != bounds.minimum || measured.Value().maximum != bounds.maximum)
                return Failure<void>(PhysicsErrors::ShapeArtifactInvalid, "Heightfield artifact bounds do not match its sample grid.");
            return Result<void>::Success();
        }

        [[nodiscard]] Result<LoadedPhysicsHeightField> Decode(const PhysicsCookedShapeDescriptor &descriptor,
                                                              const PhysicsShapeCookTargetDigest &target,
                                                              const std::span<const std::uint8_t> payload,
                                                              const PhysicsHeightFieldCookLimits &limits) {
            Reader reader{payload};
            Header header;
            if (!ReadHeader(reader, header))
                return Failure<LoadedPhysicsHeightField>(PhysicsErrors::ShapeArtifactInvalid, "Heightfield artifact header is truncated.");
            if (const auto valid = ValidateHeader(header, descriptor, target, payload.size(), limits); valid.HasError())
                return Result<LoadedPhysicsHeightField>::Failure(valid.ErrorValue());
            if (Digest(reader.Remaining()) != header.source)
                return Failure<LoadedPhysicsHeightField>(PhysicsErrors::ShapeArtifactInvalid,
                                                         "Heightfield artifact source table digest is inconsistent.");
            LoadedPhysicsHeightField loaded{.descriptor = descriptor, .sourceDigest = header.source, .bounds = header.bounds};
            std::uint32_t materialCount{};
            if (const auto metadata = ReadGridMetadata(reader, loaded, materialCount); metadata.HasError())
                return Result<LoadedPhysicsHeightField>::Failure(metadata.ErrorValue());
            if (const auto extent = ValidateGridExtent(loaded, header.settings.limits, limits, materialCount, payload.size());
                extent.HasError())
                return Result<LoadedPhysicsHeightField>::Failure(extent.ErrorValue());
            if (const auto geometry = ReadGridSamplesAndHoles(reader, loaded); geometry.HasError())
                return Result<LoadedPhysicsHeightField>::Failure(geometry.ErrorValue());
            if (const auto materials = ReadGridMaterials(reader, loaded, materialCount); materials.HasError())
                return Result<LoadedPhysicsHeightField>::Failure(materials.ErrorValue());
            if (const auto valid = ValidateDecodedGrid(loaded, header.bounds, reader); valid.HasError())
                return Result<LoadedPhysicsHeightField>::Failure(valid.ErrorValue());
            return Result<LoadedPhysicsHeightField>::Success(std::move(loaded));
        }
    }  // namespace HeightFieldDetail

    /** @copydoc LoadCookedPhysicsHeightField */
    Result<LoadedPhysicsHeightField> LoadCookedPhysicsHeightField(const PhysicsCookedShapeDescriptor &descriptor,
                                                                  const PhysicsShapeCookTargetDigest &expectedTarget,
                                                                  const std::span<const std::uint8_t> payload,
                                                                  const PhysicsHeightFieldCookLimits &limits) {
        if (!HeightFieldDetail::Bounded(limits))
            return HeightFieldDetail::Failure<LoadedPhysicsHeightField>(PhysicsErrors::ProfileUnsupported,
                                                                        "Heightfield runtime limits are unsupported.");
        if (const auto reference = ValidatePhysicsCookedShapeDescriptor(descriptor, expectedTarget); reference.HasError())
            return Result<LoadedPhysicsHeightField>::Failure(reference.ErrorValue());
        if (descriptor.kind != PhysicsCookedShapeKind::HeightField || payload.size() > limits.maxPayloadBytes ||
            HeightFieldDetail::Digest(payload) != *descriptor.payloadDigest)
            return HeightFieldDetail::Failure<
                LoadedPhysicsHeightField>(PhysicsErrors::ShapeArtifactInvalid,
                                          "Heightfield artifact kind, size or payload digest does not match its exact reference.");
        try {
            return HeightFieldDetail::Decode(descriptor, expectedTarget, payload, limits);
        } catch (const std::bad_alloc &) {
            return HeightFieldDetail::Failure<
                LoadedPhysicsHeightField>(PhysicsErrors::ShapeCookLimitExceeded,
                                          "Heightfield artifact storage could not be allocated within runtime limits.");
        }
    }
}  // namespace Horo::Physics
