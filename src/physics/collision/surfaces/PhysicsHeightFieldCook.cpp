#include "Horo/Physics/PhysicsHeightFieldCook.h"

#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsHeightFieldCookInternal.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <format>
#include <limits>
#include <new>
#include <ranges>
#include <string>
#include <utility>

namespace Horo::Physics {
    namespace HeightFieldDetail {
        constexpr std::size_t MaximumContextBytes = 256;

        class Writer final {
        public:
            void Reserve(const std::size_t count) {
                bytes_.reserve(count);
            }

            void U8(const std::uint8_t value) {
                bytes_.push_back(value);
            }

            void U32(const std::uint32_t value) {
                Integer(value);
            }

            void U64(const std::uint64_t value) {
                Integer(value);
            }

            void Float(const float value) {
                U32(std::bit_cast<std::uint32_t>(value == 0.0F ? 0.0F : value));
            }

            void Bytes(const std::span<const std::uint8_t> bytes) {
                bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
            }

            [[nodiscard]] std::span<const std::uint8_t> View() const noexcept {
                return bytes_;
            }

            [[nodiscard]] std::vector<std::uint8_t> Take() && {
                return std::move(bytes_);
            }

        private:
            template <typename IntegerType> void Integer(const IntegerType value) {
                for (std::size_t index = 0; index < sizeof(IntegerType); ++index)
                    U8(static_cast<std::uint8_t>(value >> (index * 8U)));
            }

            std::vector<std::uint8_t> bytes_;
        };

        [[nodiscard]] Sha256Digest Digest(const std::span<const std::uint8_t> bytes) noexcept {
            return ComputeSha256(std::as_bytes(bytes));
        }

        [[nodiscard]] bool Bounded(const PhysicsHeightFieldCookLimits &limits) noexcept {
            return limits.maxDimension >= 2 && limits.maxDimension <= PhysicsHeightFieldCookLimits::MaximumDimension &&
                   limits.maxSamples >= 4 && limits.maxSamples <= PhysicsHeightFieldCookLimits::MaximumSamples &&
                   limits.maxMaterialSlots > 0 && limits.maxMaterialSlots <= PhysicsHeightFieldCookLimits::MaximumMaterialSlots &&
                   limits.maxPayloadBytes >= HeaderBytes && limits.maxPayloadBytes <= PhysicsHeightFieldCookLimits::MaximumPayloadBytes;
        }

        [[nodiscard]] std::string Context(const PhysicsHeightFieldCookRequest &request, const std::string_view message) {
            const auto name = request.sourceContext.empty() ? std::string{"<unnamed>"} : std::string{request.sourceContext};
            return std::format("Physics heightfield tile '{}' (asset {}, subresource {}): {}", name, request.asset.ToString(),
                               request.subresource.Value(), message);
        }

        [[nodiscard]] std::uint64_t SampleCount(const std::uint32_t width, const std::uint32_t height) noexcept {
            return static_cast<std::uint64_t>(width) * height;
        }

        [[nodiscard]] std::uint64_t CellCount(const std::uint32_t width, const std::uint32_t height) noexcept {
            return static_cast<std::uint64_t>(width - 1U) * (height - 1U);
        }

        [[nodiscard]] bool ValidScale(const Math::Vec3 origin, const float spacingX, const float spacingZ,
                                      const float sampleScaleY) noexcept {
            return Math::IsFinite(origin) && std::isfinite(spacingX) && spacingX > 0.0F && std::isfinite(spacingZ) && spacingZ > 0.0F &&
                   std::isfinite(sampleScaleY) && sampleScaleY > 0.0F &&
                   std::isfinite(static_cast<float>(static_cast<double>(origin.x) + spacingX)) &&
                   std::isfinite(static_cast<float>(static_cast<double>(origin.z) + spacingZ));
        }

        Result<Math::Aabb> ComputeBounds(const HeightFieldGridView &grid, const CancellationToken *cancellation) {
            const auto maximumX =
                static_cast<float>(static_cast<double>(grid.origin.x) + static_cast<double>(grid.width - 1U) * grid.spacingX);
            const auto maximumZ =
                static_cast<float>(static_cast<double>(grid.origin.z) + static_cast<double>(grid.height - 1U) * grid.spacingZ);
            if (!std::isfinite(maximumX) || !std::isfinite(maximumZ) || maximumX <= grid.origin.x || maximumZ <= grid.origin.z)
                return Failure<Math::Aabb>(PhysicsErrors::ShapeCookSourceInvalid,
                                           "Heightfield tile horizontal extent is not representable.");
            float minimumY = std::numeric_limits<float>::infinity();
            float maximumY = -std::numeric_limits<float>::infinity();
            for (std::size_t index = 0; index < grid.samples.size(); ++index) {
                if (cancellation != nullptr && index % 4096U == 0 && cancellation->IsCancellationRequested())
                    return Failure<Math::Aabb>(PhysicsErrors::ShapeCookCancelled,
                                               "Heightfield cook was cancelled during sample validation.");
                const float sample = grid.samples[index];
                if (!std::isfinite(sample))
                    return Failure<Math::Aabb>(PhysicsErrors::ShapeCookSourceInvalid, "Heightfield tile contains a non-finite sample.");
                const auto y = static_cast<float>(static_cast<double>(grid.origin.y) + static_cast<double>(sample) * grid.sampleScaleY);
                if (!std::isfinite(y))
                    return Failure<Math::Aabb>(PhysicsErrors::ShapeCookSourceInvalid, "Heightfield tile height is not representable.");
                minimumY = std::min(minimumY, y);
                maximumY = std::max(maximumY, y);
            }
            return Result<Math::Aabb>::Success(
                {.minimum = {grid.origin.x, minimumY, grid.origin.z}, .maximum = {maximumX, maximumY, maximumZ}});
        }

        Result<void> ValidateMappings(const std::span<const std::uint8_t> holes, const std::span<const PhysicsMaterialSlotId> materials,
                                      const std::span<const PhysicsMaterialSlotId> slots, const CancellationToken *cancellation) {
            if (slots.empty() ||
                std::ranges::any_of(slots,
                                    [](const auto slot) {
                return !slot.IsValid();
            }) ||
                !std::ranges::is_sorted(slots) || std::ranges::adjacent_find(slots) != slots.end())
                return Failure<void>(PhysicsErrors::ShapeCookSourceInvalid,
                                     "Heightfield material slots must be non-zero, sorted and unique.");
            bool hasSolid = false;
            for (std::size_t index = 0; index < holes.size(); ++index) {
                if (cancellation != nullptr && index % 4096U == 0 && cancellation->IsCancellationRequested())
                    return Failure<void>(PhysicsErrors::ShapeCookCancelled, "Heightfield cook was cancelled during cell validation.");
                if (holes[index] > 1 || (holes[index] == 1 && materials[index].IsValid()) ||
                    (holes[index] == 0 && !std::ranges::binary_search(slots, materials[index])))
                    return Failure<void>(PhysicsErrors::ShapeCookSourceInvalid,
                                         std::format("Heightfield cell {} has an invalid hole or material mapping.", index));
                hasSolid |= holes[index] == 0;
            }
            if (!hasSolid)
                return Failure<void>(PhysicsErrors::ShapeCookSourceInvalid, "Heightfield tile has no solid collision cell.");
            return Result<void>::Success();
        }

        [[nodiscard]] Result<Math::Aabb> ValidateSource(const PhysicsHeightFieldCookRequest &request,
                                                        const CancellationToken &cancellation) {
            if (cancellation.IsCancellationRequested())
                return Failure<Math::Aabb>(PhysicsErrors::ShapeCookCancelled, Context(request, "cook was cancelled."));
            if (request.sourceContext.size() > MaximumContextBytes || !request.asset.IsValid() || !request.subresource.IsValid())
                return Failure<Math::Aabb>(PhysicsErrors::ShapeCookSourceInvalid,
                                           Context(request, "source context or persistent identity is invalid."));
            if (request.settings.schemaVersion != PhysicsHeightFieldCookSettings::CurrentSchemaVersion ||
                request.settings.algorithmVersion != PhysicsHeightFieldCookSettings::CurrentAlgorithmVersion ||
                !Bounded(request.settings.limits))
                return Failure<Math::Aabb>(PhysicsErrors::ProfileUnsupported, Context(request, "cook settings are unsupported."));
            if (const auto &limits = request.settings.limits;
                request.width < 2 || request.height < 2 || request.width > limits.maxDimension || request.height > limits.maxDimension ||
                SampleCount(request.width, request.height) > limits.maxSamples || request.materialSlots.size() > limits.maxMaterialSlots)
                return Failure<Math::Aabb>(PhysicsErrors::ShapeCookLimitExceeded,
                                           Context(request, "tile dimensions, sample count or material count exceed qualified limits."));
            if (request.samples.size() != SampleCount(request.width, request.height) ||
                request.cellHoles.size() != CellCount(request.width, request.height) ||
                request.cellMaterials.size() != request.cellHoles.size() ||
                !ValidScale(request.origin, request.spacingX, request.spacingZ, request.sampleScaleY))
                return Failure<Math::Aabb>(PhysicsErrors::ShapeCookSourceInvalid,
                                           Context(request, "tile table dimensions or scale/origin are invalid."));
            if (const auto mapping = ValidateMappings(request.cellHoles, request.cellMaterials, request.materialSlots, &cancellation);
                mapping.HasError())
                return Result<Math::Aabb>::Failure(mapping.ErrorValue());
            auto bounds = ComputeBounds({request.width, request.height, request.origin, request.spacingX, request.spacingZ,
                                         request.sampleScaleY, request.samples},
                                        &cancellation);
            if (bounds.HasError())
                return Result<Math::Aabb>::Failure(bounds.ErrorValue());
            return bounds;
        }

        void WriteLimits(Writer &writer, const PhysicsHeightFieldCookLimits &limits) {
            writer.U32(limits.maxDimension);
            writer.U64(limits.maxSamples);
            writer.U32(limits.maxMaterialSlots);
            writer.U64(limits.maxPayloadBytes);
        }

        [[nodiscard]] Sha256Digest CookKey(const Assets::AssetId &asset, const PhysicsShapeSubresourceId subresource,
                                           const Sha256Digest &source, const PhysicsHeightFieldCookSettings &settings,
                                           const PhysicsShapeCookTargetDigest &target) {
            Writer writer;
            writer.Bytes(KeyMagic);
            writer.U32(PayloadVersion);
            writer.Bytes(asset.Bytes());
            writer.U64(subresource.Value());
            writer.Bytes(source.bytes);
            writer.U32(settings.schemaVersion);
            writer.U32(settings.algorithmVersion);
            WriteLimits(writer, settings.limits);
            writer.Bytes(target.digest.bytes);
            return Digest(writer.View());
        }

        [[nodiscard]] bool WriteSource(Writer &writer, const PhysicsHeightFieldCookRequest &request,
                                       const CancellationToken &cancellation) {
            writer.U32(request.width);
            writer.U32(request.height);
            writer.Float(request.origin.x);
            writer.Float(request.origin.y);
            writer.Float(request.origin.z);
            writer.Float(request.spacingX);
            writer.Float(request.spacingZ);
            writer.Float(request.sampleScaleY);
            writer.U32(static_cast<std::uint32_t>(request.materialSlots.size()));
            for (std::size_t index = 0; index < request.samples.size(); ++index) {
                if (index % 4096U == 0 && cancellation.IsCancellationRequested())
                    return false;
                writer.Float(request.samples[index]);
            }
            for (std::size_t index = 0; index < request.cellHoles.size(); ++index) {
                if (index % 4096U == 0 && cancellation.IsCancellationRequested())
                    return false;
                writer.U8(request.cellHoles[index]);
            }
            for (std::size_t index = 0; index < request.cellMaterials.size(); ++index) {
                if (index % 4096U == 0 && cancellation.IsCancellationRequested())
                    return false;
                writer.U64(request.cellMaterials[index].Value());
            }
            for (const auto material : request.materialSlots)
                writer.U64(material.Value());
            return !cancellation.IsCancellationRequested();
        }

        void WriteHeader(Writer &writer, const PhysicsHeightFieldCookRequest &request, const Sha256Digest &source, const Sha256Digest &key,
                         const Math::Aabb &bounds, const std::uint64_t payloadBytes) {
            writer.Bytes(PayloadMagic);
            writer.U32(PayloadVersion);
            writer.U32(request.settings.schemaVersion);
            writer.U32(request.settings.algorithmVersion);
            writer.Bytes(request.asset.Bytes());
            writer.U64(request.subresource.Value());
            writer.Bytes(source.bytes);
            writer.Bytes(key.bytes);
            writer.Bytes(request.target.digest.bytes);
            WriteLimits(writer, request.settings.limits);
            for (const float value :
                 {bounds.minimum.x, bounds.minimum.y, bounds.minimum.z, bounds.maximum.x, bounds.maximum.y, bounds.maximum.z})
                writer.Float(value);
            writer.U64(payloadBytes);
        }

        [[nodiscard]] Result<PhysicsHeightFieldCookResult> Cook(const PhysicsHeightFieldCookRequest &request, const Math::Aabb &bounds,
                                                                const CancellationToken &cancellation) {
            const auto sourceBytes = 36ULL + SampleCount(request.width, request.height) * sizeof(float) +
                                     CellCount(request.width, request.height) * (sizeof(std::uint8_t) + sizeof(std::uint64_t)) +
                                     request.materialSlots.size() * sizeof(std::uint64_t);
            const auto payloadBytes = HeaderBytes + sourceBytes;
            if (payloadBytes > request.settings.limits.maxPayloadBytes)
                return Failure<PhysicsHeightFieldCookResult>(PhysicsErrors::ShapeCookLimitExceeded,
                                                             Context(request, "tile artifact exceeds configured byte limit."));
            Writer sourceWriter;
            sourceWriter.Reserve(static_cast<std::size_t>(sourceBytes));
            if (!WriteSource(sourceWriter, request, cancellation))
                return Failure<PhysicsHeightFieldCookResult>(PhysicsErrors::ShapeCookCancelled,
                                                             Context(request, "cook was cancelled during table encoding."));
            const auto sourceDigest = Digest(sourceWriter.View());
            const auto key = CookKey(request.asset, request.subresource, sourceDigest, request.settings, request.target);
            Writer payloadWriter;
            payloadWriter.Reserve(static_cast<std::size_t>(payloadBytes));
            WriteHeader(payloadWriter, request, sourceDigest, key, bounds, payloadBytes);
            payloadWriter.Bytes(sourceWriter.View());
            auto payload = std::move(payloadWriter).Take();
            const auto payloadDigest = Digest(payload);
            return Result<PhysicsHeightFieldCookResult>::Success({.descriptor = {.asset = request.asset,
                                                                                 .subresource = request.subresource,
                                                                                 .kind = PhysicsCookedShapeKind::HeightField,
                                                                                 .cacheKeyDigest = key,
                                                                                 .payloadDigest = payloadDigest,
                                                                                 .target = request.target},
                                                                  .sourceDigest = sourceDigest,
                                                                  .bounds = bounds,
                                                                  .width = request.width,
                                                                  .height = request.height,
                                                                  .payload = std::move(payload)});
        }

    }  // namespace HeightFieldDetail

    /** @copydoc CookPhysicsHeightField */
    Result<PhysicsHeightFieldCookResult> CookPhysicsHeightField(const PhysicsHeightFieldCookRequest &request,
                                                                const CancellationToken &cancellation) {
        const auto bounds = HeightFieldDetail::ValidateSource(request, cancellation);
        if (bounds.HasError())
            return Result<PhysicsHeightFieldCookResult>::Failure(bounds.ErrorValue());
        try {
            return HeightFieldDetail::Cook(request, bounds.Value(), cancellation);
        } catch (const std::bad_alloc &) {
            return HeightFieldDetail::Failure<
                PhysicsHeightFieldCookResult>(PhysicsErrors::ShapeCookLimitExceeded,
                                              HeightFieldDetail::Context(request, "bounded cook storage could not be allocated."));
        }
    }

    /** @copydoc ValidatePhysicsHeightFieldMotion */
    Result<void> ValidatePhysicsHeightFieldMotion(const PhysicsMotionType motion) {
        using enum PhysicsMotionType;
        if (motion == Static)
            return Result<void>::Success();
        if (motion == Kinematic || motion == Dynamic)
            return HeightFieldDetail::Failure<void>(PhysicsErrors::ShapeMotionUnsupported,
                                                    "Heightfield collision supports static bodies only.");
        return HeightFieldDetail::Failure<void>(PhysicsErrors::OperationUnsupported, "Unknown body motion mode.");
    }
}  // namespace Horo::Physics
