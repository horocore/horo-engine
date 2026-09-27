#include "Horo/Physics/PhysicsHeightFieldCook.h"

#include "Horo/Physics/PhysicsErrors.h"

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
    namespace {
        constexpr std::array<std::uint8_t, 4> PayloadMagic{'P', 'H', 'H', 'F'};
        constexpr std::array<std::uint8_t, 4> KeyMagic{'P', 'H', 'H', 'K'};
        constexpr std::uint32_t PayloadVersion = 1;
        constexpr std::size_t HeaderBytes = 192;
        constexpr std::size_t MaximumContextBytes = 256;

        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &code, std::string message) {
            return Result<T>::Failure(MakeError(code, std::move(message)));
        }

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

        [[nodiscard]] Result<Math::Aabb> ComputeBounds(const std::uint32_t width, const std::uint32_t height, const Math::Vec3 origin,
                                                       const float spacingX, const float spacingZ, const float sampleScaleY,
                                                       const std::span<const float> samples,
                                                       const CancellationToken *cancellation = nullptr) {
            const auto maximumX = static_cast<float>(static_cast<double>(origin.x) + static_cast<double>(width - 1U) * spacingX);
            const auto maximumZ = static_cast<float>(static_cast<double>(origin.z) + static_cast<double>(height - 1U) * spacingZ);
            if (!std::isfinite(maximumX) || !std::isfinite(maximumZ) || maximumX <= origin.x || maximumZ <= origin.z)
                return Failure<Math::Aabb>(PhysicsErrors::ShapeCookSourceInvalid,
                                           "Heightfield tile horizontal extent is not representable.");
            float minimumY = std::numeric_limits<float>::infinity();
            float maximumY = -std::numeric_limits<float>::infinity();
            for (std::size_t index = 0; index < samples.size(); ++index) {
                if (cancellation != nullptr && index % 4096U == 0 && cancellation->IsCancellationRequested())
                    return Failure<Math::Aabb>(PhysicsErrors::ShapeCookCancelled,
                                               "Heightfield cook was cancelled during sample validation.");
                const float sample = samples[index];
                if (!std::isfinite(sample))
                    return Failure<Math::Aabb>(PhysicsErrors::ShapeCookSourceInvalid, "Heightfield tile contains a non-finite sample.");
                const auto y = static_cast<float>(static_cast<double>(origin.y) + static_cast<double>(sample) * sampleScaleY);
                if (!std::isfinite(y))
                    return Failure<Math::Aabb>(PhysicsErrors::ShapeCookSourceInvalid, "Heightfield tile height is not representable.");
                minimumY = std::min(minimumY, y);
                maximumY = std::max(maximumY, y);
            }
            return Result<Math::Aabb>::Success({.minimum = {origin.x, minimumY, origin.z}, .maximum = {maximumX, maximumY, maximumZ}});
        }

        [[nodiscard]] Result<void> ValidateMappings(const std::span<const std::uint8_t> holes,
                                                    const std::span<const PhysicsMaterialSlotId> materials,
                                                    const std::span<const PhysicsMaterialSlotId> slots,
                                                    const CancellationToken *cancellation = nullptr) {
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
            const auto &limits = request.settings.limits;
            if (request.width < 2 || request.height < 2 || request.width > limits.maxDimension || request.height > limits.maxDimension ||
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
            auto bounds = ComputeBounds(request.width, request.height, request.origin, request.spacingX, request.spacingZ,
                                        request.sampleScaleY, request.samples, &cancellation);
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

        [[nodiscard]] bool ReadLimits(Reader &reader, PhysicsHeightFieldCookLimits &limits) noexcept {
            return reader.U32(limits.maxDimension) && reader.U64(limits.maxSamples) && reader.U32(limits.maxMaterialSlots) &&
                   reader.U64(limits.maxPayloadBytes);
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
            if (!reader.U32(loaded.width) || !reader.U32(loaded.height) || !reader.Float(loaded.origin.x) ||
                !reader.Float(loaded.origin.y) || !reader.Float(loaded.origin.z) || !reader.Float(loaded.spacingX) ||
                !reader.Float(loaded.spacingZ) || !reader.Float(loaded.sampleScaleY) || !reader.U32(materialCount))
                return Failure<LoadedPhysicsHeightField>(PhysicsErrors::ShapeArtifactInvalid,
                                                         "Heightfield tile geometry header is truncated.");
            if (loaded.width < 2 || loaded.height < 2 || loaded.width > limits.maxDimension || loaded.height > limits.maxDimension ||
                loaded.width > header.settings.limits.maxDimension || loaded.height > header.settings.limits.maxDimension ||
                SampleCount(loaded.width, loaded.height) > limits.maxSamples ||
                SampleCount(loaded.width, loaded.height) > header.settings.limits.maxSamples || materialCount == 0 ||
                materialCount > limits.maxMaterialSlots || materialCount > header.settings.limits.maxMaterialSlots ||
                !ValidScale(loaded.origin, loaded.spacingX, loaded.spacingZ, loaded.sampleScaleY))
                return Failure<LoadedPhysicsHeightField>(PhysicsErrors::ShapeArtifactInvalid,
                                                         "Heightfield tile dimensions or scale are invalid.");
            const auto samples = SampleCount(loaded.width, loaded.height);
            const auto cells = CellCount(loaded.width, loaded.height);
            const auto expectedBytes = HeaderBytes + 36ULL + samples * sizeof(float) +
                                       cells * (sizeof(std::uint8_t) + sizeof(std::uint64_t)) +
                                       static_cast<std::uint64_t>(materialCount) * sizeof(std::uint64_t);
            if (expectedBytes != payload.size())
                return Failure<LoadedPhysicsHeightField>(PhysicsErrors::ShapeArtifactInvalid,
                                                         "Heightfield artifact table extent is inconsistent.");
            loaded.samples.resize(static_cast<std::size_t>(samples));
            loaded.cellHoles.resize(static_cast<std::size_t>(cells));
            loaded.cellMaterials.resize(static_cast<std::size_t>(cells));
            loaded.materialSlots.resize(materialCount);
            for (auto &sample : loaded.samples) {
                if (!reader.Float(sample))
                    return Failure<LoadedPhysicsHeightField>(PhysicsErrors::ShapeArtifactInvalid, "Heightfield sample is invalid.");
            }
            for (auto &hole : loaded.cellHoles) {
                if (!reader.U8(hole))
                    return Failure<LoadedPhysicsHeightField>(PhysicsErrors::ShapeArtifactInvalid, "Heightfield hole table is truncated.");
            }
            for (auto &material : loaded.cellMaterials) {
                std::uint64_t value{};
                if (!reader.U64(value))
                    return Failure<LoadedPhysicsHeightField>(PhysicsErrors::ShapeArtifactInvalid,
                                                             "Heightfield cell material table is truncated.");
                material = PhysicsMaterialSlotId::FromValue(value);
            }
            for (auto &material : loaded.materialSlots) {
                std::uint64_t value{};
                if (!reader.U64(value))
                    return Failure<LoadedPhysicsHeightField>(PhysicsErrors::ShapeArtifactInvalid,
                                                             "Heightfield material table is truncated.");
                material = PhysicsMaterialSlotId::FromValue(value);
            }
            if (!reader.Remaining().empty())
                return Failure<LoadedPhysicsHeightField>(PhysicsErrors::ShapeArtifactInvalid, "Heightfield artifact has trailing bytes.");
            if (const auto mapping = ValidateMappings(loaded.cellHoles, loaded.cellMaterials, loaded.materialSlots); mapping.HasError())
                return Failure<LoadedPhysicsHeightField>(PhysicsErrors::ShapeArtifactInvalid, mapping.ErrorValue().message);
            const auto bounds = ComputeBounds(loaded.width, loaded.height, loaded.origin, loaded.spacingX, loaded.spacingZ,
                                              loaded.sampleScaleY, loaded.samples);
            if (bounds.HasError() || bounds.Value().minimum != header.bounds.minimum || bounds.Value().maximum != header.bounds.maximum)
                return Failure<LoadedPhysicsHeightField>(PhysicsErrors::ShapeArtifactInvalid,
                                                         "Heightfield artifact bounds do not match its sample grid.");
            return Result<LoadedPhysicsHeightField>::Success(std::move(loaded));
        }
    }  // namespace

    /** @copydoc CookPhysicsHeightField */
    Result<PhysicsHeightFieldCookResult> CookPhysicsHeightField(const PhysicsHeightFieldCookRequest &request,
                                                                const CancellationToken &cancellation) {
        const auto bounds = ValidateSource(request, cancellation);
        if (bounds.HasError())
            return Result<PhysicsHeightFieldCookResult>::Failure(bounds.ErrorValue());
        try {
            return Cook(request, bounds.Value(), cancellation);
        } catch (const std::bad_alloc &) {
            return Failure<PhysicsHeightFieldCookResult>(PhysicsErrors::ShapeCookLimitExceeded,
                                                         Context(request, "bounded cook storage could not be allocated."));
        }
    }

    /** @copydoc LoadCookedPhysicsHeightField */
    Result<LoadedPhysicsHeightField> LoadCookedPhysicsHeightField(const PhysicsCookedShapeDescriptor &descriptor,
                                                                  const PhysicsShapeCookTargetDigest &expectedTarget,
                                                                  const std::span<const std::uint8_t> payload,
                                                                  const PhysicsHeightFieldCookLimits &limits) {
        if (!Bounded(limits))
            return Failure<LoadedPhysicsHeightField>(PhysicsErrors::ProfileUnsupported, "Heightfield runtime limits are unsupported.");
        if (const auto reference = ValidatePhysicsCookedShapeDescriptor(descriptor, expectedTarget); reference.HasError())
            return Result<LoadedPhysicsHeightField>::Failure(reference.ErrorValue());
        if (descriptor.kind != PhysicsCookedShapeKind::HeightField || payload.size() > limits.maxPayloadBytes ||
            Digest(payload) != *descriptor.payloadDigest)
            return Failure<
                LoadedPhysicsHeightField>(PhysicsErrors::ShapeArtifactInvalid,
                                          "Heightfield artifact kind, size or payload digest does not match its exact reference.");
        try {
            return Decode(descriptor, expectedTarget, payload, limits);
        } catch (const std::bad_alloc &) {
            return Failure<LoadedPhysicsHeightField>(PhysicsErrors::ShapeCookLimitExceeded,
                                                     "Heightfield artifact storage could not be allocated within runtime limits.");
        }
    }

    /** @copydoc ValidatePhysicsHeightFieldMotion */
    Result<void> ValidatePhysicsHeightFieldMotion(const PhysicsMotionType motion) {
        if (motion == PhysicsMotionType::Static)
            return Result<void>::Success();
        if (motion == PhysicsMotionType::Kinematic || motion == PhysicsMotionType::Dynamic)
            return Failure<void>(PhysicsErrors::ShapeMotionUnsupported, "Heightfield collision supports static bodies only.");
        return Failure<void>(PhysicsErrors::OperationUnsupported, "Unknown body motion mode.");
    }
}  // namespace Horo::Physics
