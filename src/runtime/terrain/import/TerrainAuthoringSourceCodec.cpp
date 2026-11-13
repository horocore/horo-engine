#include "Horo/Terrain/TerrainAuthoringSourceCodec.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <new>

namespace Horo::Terrain {
    namespace {
        constexpr std::array<std::uint8_t, 8> SourceMagic{0, 0, 0xc0, 0x7f, 'H', 'T', 'A', 'S'};
        constexpr std::array<std::uint8_t, 8> LayerMagic{'H', 'T', 'L', 'A', 'Y', 'R', '0', '1'};
        constexpr std::uint32_t Version = 1;
        constexpr std::size_t MaximumBytes = 64 * 1024 * 1024;
        constexpr std::size_t LayerBytes = 4096;

        /** @brief Canonical little-endian writer; bulk capacity is reserved before sample traversal. */
        struct Writer final {
            std::vector<std::byte> bytes;

            void U(std::uint64_t value, unsigned count) {
                for (unsigned i = 0; i < count; ++i)
                    bytes.push_back(static_cast<std::byte>((value >> (8U * i)) & 255U));
            }

            template <std::size_t N> void Id(const std::array<std::uint8_t, N> &value) {
                for (auto byte : value)
                    bytes.push_back(static_cast<std::byte>(byte));
            }

            void D(double value) {
                U(std::bit_cast<std::uint64_t>(value == 0 ? 0.0 : value), 8);
            }

            void F(float value) {
                U(std::bit_cast<std::uint32_t>(value == 0 ? 0.0F : value), 4);
            }

            void Text(std::string_view value) {
                U(value.size(), 1);
                for (auto c : value)
                    U(static_cast<unsigned char>(c), 1);
            }
        };

        /** @brief Fixed-width checked reader; no malformed length can advance outside its bounded capture. */
        struct Reader final {
            std::span<const std::byte> bytes;
            std::size_t at{};
            bool valid{true};

            std::uint64_t U(unsigned count) {
                if (count > 8 || count > bytes.size() - at) {
                    valid = false;
                    return 0;
                }
                std::uint64_t value{};
                for (unsigned i = 0; i < count; ++i)
                    value |= std::uint64_t{std::to_integer<std::uint8_t>(bytes[at++])} << (8U * i);
                return value;
            }

            template <std::size_t N> std::array<std::uint8_t, N> Id() {
                std::array<std::uint8_t, N> result{};
                for (auto &byte : result)
                    byte = static_cast<std::uint8_t>(U(1));
                return result;
            }

            double D() {
                return std::bit_cast<double>(U(8));
            }

            float F() {
                return std::bit_cast<float>(static_cast<std::uint32_t>(U(4)));
            }

            std::string Text() {
                const auto count = U(1);
                if (count > 16 || count > bytes.size() - at) {
                    valid = false;
                    return {};
                }
                std::string result;
                result.reserve(static_cast<std::size_t>(count));
                for (std::size_t i = 0; i < count; ++i)
                    result.push_back(static_cast<char>(U(1)));
                return result;
            }
        };

        /** @brief Bounded cooperative integrity work, separate from sample/channel allocation admission. */
        Result<Sha256Digest> Hash(std::span<const std::byte> bytes, const CancellationToken &token) {
            Sha256Builder hash;
            for (std::size_t at = 0; at < bytes.size();) {
                if (token.IsCancellationRequested())
                    return Result<Sha256Digest>::Failure(MakeError(TerrainSourceErrors::Cancelled));
                const auto count = std::min<std::size_t>(4096, bytes.size() - at);
                if (!hash.Update(bytes.subspan(at, count)))
                    return Result<Sha256Digest>::Failure(MakeError(TerrainSourceErrors::InvalidBytes));
                at += count;
            }
            return Result<Sha256Digest>::Success(hash.Finalize());
        }

        Result<std::span<const std::byte>> Payload(std::span<const std::byte> bytes, std::size_t maximum, const CancellationToken &token) {
            using Return = Result<std::span<const std::byte>>;
            if (bytes.size() < 44 || bytes.size() > maximum)
                return Return::Failure(MakeError(TerrainSourceErrors::InvalidBytes));
            const auto body = bytes.first(bytes.size() - 32);
            auto digest = Hash(body, token);
            if (digest.HasError())
                return Return::Failure(std::move(digest).ErrorValue());
            for (std::size_t i = 0; i < 32; ++i)
                if (std::to_integer<std::uint8_t>(bytes[body.size() + i]) != digest.Value().bytes[i])
                    return Return::Failure(MakeError(TerrainSourceErrors::InvalidBytes));
            return Return::Success(body);
        }

        Result<std::vector<std::byte>> Finish(Writer writer, const CancellationToken &token) {
            auto digest = Hash(writer.bytes, token);
            if (digest.HasError())
                return Result<std::vector<std::byte>>::Failure(std::move(digest).ErrorValue());
            writer.Id(digest.Value().bytes);
            return Result<std::vector<std::byte>>::Success(std::move(writer.bytes));
        }

        bool DigestPresent(const Sha256Digest &value) noexcept {
            return std::ranges::any_of(value.bytes, [](auto byte) {
                return byte != 0;
            });
        }

        constexpr std::uint64_t SourceHeaderBytes = 156;
        constexpr std::uint64_t LayerBindingBytes = 72;
        constexpr std::uint64_t LayerIdBytes = 16;
        constexpr std::uint64_t DigestBytes = 32;
        constexpr std::uint64_t HashChunkBytes = 4096;

        bool ProjectedCrs(std::string_view crs) noexcept {
            if (!crs.starts_with("EPSG:") || crs.size() < 6 || crs.size() > 16)
                return false;
            return std::ranges::all_of(crs.substr(5), [](char ch) {
                return ch >= '0' && ch <= '9';
            });
        }

        bool CoordinateSpace(const TerrainSourceCoordinates &coordinates) noexcept {
            switch (coordinates.space) {
                case TerrainCoordinateSpace::LocalMeters:
                    return coordinates.projectedCrs.empty();
                case TerrainCoordinateSpace::ProjectedMeters:
                    return ProjectedCrs(coordinates.projectedCrs);
                default:
                    return false;
            }
        }

        bool FiniteCoordinates(const TerrainSourceCoordinates &coordinates) noexcept {
            const std::array values{coordinates.originX,
                                    coordinates.originZ,
                                    coordinates.spacingX,
                                    coordinates.spacingZ,
                                    coordinates.heightScale,
                                    coordinates.heightOffset,
                                    coordinates.maximumPrecisionError};
            return std::ranges::all_of(values, [](double value) {
                return std::isfinite(value);
            });
        }

        bool CoordinateScale(const TerrainSourceCoordinates &coordinates) noexcept {
            return coordinates.spacingX > 0 && coordinates.spacingZ > 0 && coordinates.heightScale > 0 &&
                   coordinates.maximumPrecisionError >= 0;
        }

        /** @brief Validate the transformed grid extent only after dimensions and scale have been admitted. */
        bool FiniteExtent(const TerrainCanonicalSource &source) noexcept {
            const auto &coordinates = source.coordinates;
            return std::isfinite(coordinates.originX + (source.width - 1U) * coordinates.spacingX) &&
                   std::isfinite(coordinates.originZ + (source.height - 1U) * coordinates.spacingZ);
        }

        /** @brief Matches canonical import rules without applying height scale/offset a second time. */
        bool Coordinates(const TerrainCanonicalSource &source) noexcept {
            return CoordinateSpace(source.coordinates) && FiniteCoordinates(source.coordinates) && CoordinateScale(source.coordinates) &&
                   FiniteExtent(source);
        }

        Result<void> ValidateCodecLimits(const TerrainAuthoringCodecLimits &limits) {
            if (limits.maximumBytes == 0 || limits.maximumBytes > MaximumBytes || limits.maximumSamples == 0 ||
                limits.maximumWorkItems == 0 || limits.maximumWorkItems > TerrainDescriptorHardLimits::WorkItems)
                return Result<void>::Failure(MakeError(TerrainSourceErrors::LimitExceeded));
            return Result<void>::Success();
        }

        bool SourceIdentities(const TerrainCanonicalSource &source) noexcept {
            return source.dataset.IsValid() && source.sourceAsset.IsValid() && source.revision.IsValid() && source.capability.IsValid();
        }

        bool GridDimensions(const TerrainCanonicalSource &source) noexcept {
            return source.width >= 2 && source.height >= 2 && source.width <= TerrainDescriptorHardLimits::SamplesPerAxis &&
                   source.height <= TerrainDescriptorHardLimits::SamplesPerAxis &&
                   source.layerCount <= TerrainDescriptorHardLimits::LayersPerTile;
        }

        Result<void> ValidateSourceHeader(const TerrainAuthoringSource &value) {
            if (const auto &source = value.source; !SourceIdentities(source) || !GridDimensions(source) || !Coordinates(source) ||
                                                   value.layers.has_value() != (source.layerCount != 0))
                return Result<void>::Failure(MakeError(TerrainSourceErrors::InvalidDimensions));
            return Result<void>::Success();
        }

        /** @brief Active channels have unique semantic identities; order remains authored rather than sorted. */
        bool ActiveLayerOrder(std::span<const TerrainLayerId> active) noexcept {
            for (std::size_t i = 0; i < active.size(); ++i) {
                const auto earlier = active.first(i);
                if (!active[i].IsValid() || std::ranges::find(earlier, active[i]) != earlier.end())
                    return false;
            }
            return true;
        }

        bool LayerOrder(const TerrainAuthoringLayerBinding &binding, std::uint8_t layerCount) noexcept {
            const std::span order{binding.order};
            const auto unused = order.subspan(layerCount);
            return ActiveLayerOrder(order.first(layerCount)) && std::ranges::none_of(unused, [](const auto &id) {
                return id.IsValid();
            });
        }

        Result<void> ValidateLayerBinding(const TerrainAuthoringSource &value) {
            if (!value.layers)
                return Result<void>::Success();
            const auto &binding = *value.layers;
            if (const bool identities = binding.asset.IsValid() && binding.id.IsValid() && binding.revision.IsValid();
                !identities || !DigestPresent(binding.digest) || !LayerOrder(binding, value.source.layerCount))
                return Result<void>::Failure(MakeError(TerrainSourceErrors::InvalidSample));
            return Result<void>::Success();
        }

        /** @brief Describes encoded storage and traversal cost after header validation; it allocates no channel data. */
        struct SourceLayout final {
            std::uint64_t samples;
            std::uint64_t sampleBytes;
            std::uint64_t headerBytes;
            std::uint64_t visitsPerSample;
            std::uint64_t foliageWork;

            std::uint64_t EncodedBytes() const noexcept {
                return headerBytes + samples * sampleBytes;
            }
        };

        SourceLayout Layout(const TerrainAuthoringSource &value, bool hasHoles, const FoliageAuthoringSourceFootprint &foliage) noexcept {
            const auto &source = value.source;
            const std::uint64_t holeBytes = hasHoles ? 1U : 0U;
            const auto bindingBytes = value.layers ? LayerBindingBytes + LayerIdBytes * source.layerCount : 0U;
            return {std::uint64_t{source.width} * source.height, 4U + 2U * source.layerCount + holeBytes,
                    SourceHeaderBytes + source.coordinates.projectedCrs.size() + bindingBytes + foliage.encodedBytes,
                    1U + source.layerCount + holeBytes, foliage.workItems};
        }

        Result<void> ValidateStorageBudget(const SourceLayout &layout, const TerrainAuthoringCodecLimits &limits) {
            if (layout.headerBytes > limits.maximumBytes || layout.samples > limits.maximumSamples ||
                layout.samples > (limits.maximumBytes - layout.headerBytes) / layout.sampleBytes)
                return Result<void>::Failure(MakeError(TerrainSourceErrors::LimitExceeded));
            return Result<void>::Success();
        }

        /** @brief Division admits sample work before multiplication; storage has already bounded the hash input. */
        Result<void> ValidateWorkBudget(const SourceLayout &layout, const TerrainAuthoringCodecLimits &limits) {
            if (layout.foliageWork > limits.maximumWorkItems)
                return Result<void>::Failure(MakeError(TerrainSourceErrors::LimitExceeded));
            const auto remaining = limits.maximumWorkItems - layout.foliageWork;
            if (layout.samples > remaining / layout.visitsPerSample)
                return Result<void>::Failure(MakeError(TerrainSourceErrors::LimitExceeded));
            const auto visits = layout.samples * layout.visitsPerSample;
            if (const auto hashChunks = (layout.EncodedBytes() - DigestBytes + HashChunkBytes - 1) / HashChunkBytes;
                hashChunks > remaining - visits)
                return Result<void>::Failure(MakeError(TerrainSourceErrors::LimitExceeded));
            return Result<void>::Success();
        }

        /** @brief Admit the header, layer meaning and complete output/work before any bulk allocation or sample read. */
        Result<std::size_t> Shape(const TerrainAuthoringSource &value, const TerrainAuthoringCodecLimits &limits, bool hasHoles,
                                  const FoliageAuthoringSourceFootprint &foliage) {
            using Return = Result<std::size_t>;
            if (auto valid = ValidateCodecLimits(limits); valid.HasError())
                return Return::Failure(std::move(valid).ErrorValue());
            if (auto valid = ValidateSourceHeader(value); valid.HasError())
                return Return::Failure(std::move(valid).ErrorValue());
            if (auto valid = ValidateLayerBinding(value); valid.HasError())
                return Return::Failure(std::move(valid).ErrorValue());
            const auto layout = Layout(value, hasHoles, foliage);
            if (auto valid = ValidateStorageBudget(layout, limits); valid.HasError())
                return Return::Failure(std::move(valid).ErrorValue());
            if (auto valid = ValidateWorkBudget(layout, limits); valid.HasError())
                return Return::Failure(std::move(valid).ErrorValue());
            return Return::Success(static_cast<std::size_t>(layout.samples));
        }

        void WriteCoordinates(Writer &writer, const TerrainSourceCoordinates &c) {
            writer.U(static_cast<std::uint8_t>(c.space), 1);
            writer.Text(c.projectedCrs);
            writer.D(c.originX);
            writer.D(c.originZ);
            writer.D(c.spacingX);
            writer.D(c.spacingZ);
            writer.D(c.heightScale);
            writer.D(c.heightOffset);
            writer.D(c.maximumPrecisionError);
        }

        TerrainSourceCoordinates ReadCoordinates(Reader &reader) {
            TerrainSourceCoordinates c;
            c.space = static_cast<TerrainCoordinateSpace>(reader.U(1));
            c.projectedCrs = reader.Text();
            c.originX = reader.D();
            c.originZ = reader.D();
            c.spacingX = reader.D();
            c.spacingZ = reader.D();
            c.heightScale = reader.D();
            c.heightOffset = reader.D();
            c.maximumPrecisionError = reader.D();
            return c;
        }

        Result<void> ValidateChannelSizes(const TerrainCanonicalSource &source, std::size_t samples) {
            if (source.heightsMeters.size() != samples || source.weights.size() != samples * source.layerCount ||
                (source.HasHoles() && source.holes.size() != samples))
                return Result<void>::Failure(MakeError(TerrainSourceErrors::InvalidDimensions));
            return Result<void>::Success();
        }

        void WriteLayerBinding(Writer &writer, const TerrainAuthoringSource &value) {
            if (!value.layers)
                return;
            const auto &binding = *value.layers;
            writer.Id(binding.asset.Bytes());
            writer.Id(binding.id.Bytes());
            writer.U(binding.revision.Value(), 8);
            writer.Id(binding.digest.bytes);
            for (std::size_t i = 0; i < value.source.layerCount; ++i)
                writer.Id(binding.order[i].Bytes());
        }

        void WriteSourceHeader(Writer &writer, const TerrainAuthoringSource &value, std::size_t foliageBytes) {
            const auto &source = value.source;
            writer.Id(SourceMagic);
            writer.U(Version, 4);
            writer.Id(source.sourceAsset.Bytes());
            writer.Id(source.dataset.Bytes());
            writer.U(source.revision.Value(), 8);
            writer.U(source.width, 4);
            writer.U(source.height, 4);
            WriteCoordinates(writer, source.coordinates);
            writer.U(source.layerCount, 1);
            writer.U(source.HasHoles() ? 1 : 0, 1);
            WriteLayerBinding(writer, value);
            writer.U(foliageBytes, 4);
        }

        bool ValidHeightAndHole(const TerrainCanonicalSource &source, std::size_t index) noexcept {
            return std::isfinite(source.heightsMeters[index]) && (!source.HasHoles() || source.holes[index] <= 1);
        }

        bool NormalizedWeights(std::uint8_t layerCount, std::uint32_t sum) noexcept {
            return layerCount == 0 || sum == 65535;
        }

        Result<void> WriteWeights(Writer &writer, const TerrainCanonicalSource &source, std::size_t index) {
            std::uint32_t sum{};
            for (std::size_t layer = 0; layer < source.layerCount; ++layer) {
                const auto weight = source.weights[index * source.layerCount + layer];
                sum += weight;
                writer.U(weight, 2);
            }
            if (!NormalizedWeights(source.layerCount, sum))
                return Result<void>::Failure(MakeError(TerrainSourceErrors::InvalidSample));
            return Result<void>::Success();
        }

        Result<void> WriteSamples(Writer &writer, const TerrainCanonicalSource &source, std::size_t count, const CancellationToken &token) {
            for (std::size_t i = 0; i < count; ++i) {
                if (i % source.width == 0 && token.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(TerrainSourceErrors::Cancelled));
                if (!ValidHeightAndHole(source, i))
                    return Result<void>::Failure(MakeError(TerrainSourceErrors::InvalidSample));
                writer.F(source.heightsMeters[i]);
                if (auto written = WriteWeights(writer, source, i); written.HasError())
                    return written;
                if (source.HasHoles())
                    writer.U(source.holes[i], 1);
            }
            return Result<void>::Success();
        }

        Result<std::span<const std::byte>> AdmitSourcePayload(std::span<const std::byte> bytes, const TerrainAuthoringCodecLimits &limits,
                                                              const CancellationToken &token) {
            using Return = Result<std::span<const std::byte>>;
            if (!IsTerrainAuthoringSource(bytes))
                return Return::Failure(MakeError(TerrainSourceErrors::UnsupportedFormat));
            if (limits.maximumBytes == 0 || limits.maximumBytes > MaximumBytes)
                return Return::Failure(MakeError(TerrainSourceErrors::LimitExceeded));
            if (const auto bodyBytes = bytes.size() - std::min<std::size_t>(bytes.size(), DigestBytes);
                (bodyBytes + HashChunkBytes - 1) / HashChunkBytes > limits.maximumWorkItems)
                return Return::Failure(MakeError(TerrainSourceErrors::LimitExceeded));
            return Payload(bytes, limits.maximumBytes, token);
        }

        Result<TerrainCanonicalSource> ReadSourceIdentity(Reader &reader, Assets::AssetId expectedAsset,
                                                          TerrainCapabilityRevision capability) {
            using Return = Result<TerrainCanonicalSource>;
            TerrainCanonicalSource source;
            source.sourceAsset = Assets::AssetId::FromBytes(reader.Id<16>());
            const auto dataset = TerrainDatasetId::Create(reader.Id<16>());
            const auto revision = TerrainSourceRevision::Create(reader.U(8));
            if (dataset.HasError() || revision.HasError() || !expectedAsset.IsValid() || source.sourceAsset != expectedAsset)
                return Return::Failure(MakeError(TerrainSourceErrors::InvalidBytes));
            source.dataset = dataset.Value();
            source.revision = revision.Value();
            source.capability = capability;
            return Return::Success(std::move(source));
        }

        Result<std::optional<TerrainAuthoringLayerBinding>> ReadLayerBinding(Reader &reader, std::uint8_t layerCount) {
            using Return = Result<std::optional<TerrainAuthoringLayerBinding>>;
            if (layerCount == 0)
                return Return::Success(std::nullopt);
            TerrainAuthoringLayerBinding binding;
            binding.asset = Assets::AssetId::FromBytes(reader.Id<16>());
            const auto id = TerrainLayerSetId::Create(reader.Id<16>());
            const auto revision = TerrainContentRevision::Create(reader.U(8));
            if (id.HasError() || revision.HasError())
                return Return::Failure(MakeError(TerrainSourceErrors::InvalidBytes));
            binding.id = id.Value();
            binding.revision = revision.Value();
            binding.digest.bytes = reader.Id<32>();
            for (std::size_t i = 0; i < layerCount; ++i) {
                const auto layer = TerrainLayerId::Create(reader.Id<16>());
                if (layer.HasError())
                    return Return::Failure(MakeError(TerrainSourceErrors::InvalidBytes));
                binding.order[i] = layer.Value();
            }
            return Return::Success(std::move(binding));
        }

        struct SourceHeader final {
            TerrainAuthoringSource value;
            bool hasHoles{};
            std::uint32_t foliageBytes{};
        };

        Result<SourceHeader> ReadSourceHeader(Reader &reader, Assets::AssetId expectedAsset, TerrainCapabilityRevision capability) {
            using Return = Result<SourceHeader>;
            auto identity = ReadSourceIdentity(reader, expectedAsset, capability);
            if (identity.HasError())
                return Return::Failure(std::move(identity).ErrorValue());
            SourceHeader header;
            auto &source = header.value.source;
            source = std::move(identity).Value();
            source.width = static_cast<std::uint32_t>(reader.U(4));
            source.height = static_cast<std::uint32_t>(reader.U(4));
            source.coordinates = ReadCoordinates(reader);
            source.layerCount = static_cast<std::uint8_t>(reader.U(1));
            const auto holes = reader.U(1);
            if (holes > 1 || source.layerCount > TerrainDescriptorHardLimits::LayersPerTile)
                return Return::Failure(MakeError(TerrainSourceErrors::InvalidBytes));
            auto binding = ReadLayerBinding(reader, source.layerCount);
            if (binding.HasError())
                return Return::Failure(std::move(binding).ErrorValue());
            header.value.layers = std::move(binding).Value();
            header.foliageBytes = static_cast<std::uint32_t>(reader.U(4));
            if (!reader.valid)
                return Return::Failure(MakeError(TerrainSourceErrors::InvalidBytes));
            header.hasHoles = holes != 0;
            return Return::Success(std::move(header));
        }

        Result<void> ValidateSamplePayload(const Reader &reader, const SourceLayout &layout) {
            if (const auto remaining = reader.bytes.size() - reader.at;
                layout.samples != remaining / layout.sampleBytes || remaining % layout.sampleBytes != 0)
                return Result<void>::Failure(MakeError(TerrainSourceErrors::InvalidBytes));
            return Result<void>::Success();
        }

        std::uint32_t ReadWeights(Reader &reader, TerrainCanonicalSource &source, std::size_t index) {
            std::uint32_t sum{};
            for (std::size_t layer = 0; layer < source.layerCount; ++layer) {
                auto &weight = source.weights[index * source.layerCount + layer];
                weight = static_cast<std::uint16_t>(reader.U(2));
                sum += weight;
            }
            return sum;
        }

        Result<void> ReadSamples(Reader &reader, TerrainCanonicalSource &source, std::size_t count, bool hasHoles,
                                 const CancellationToken &token) {
            source.heightsMeters.resize(count);
            source.weights.resize(count * source.layerCount);
            if (hasHoles)
                source.holes.resize(count);
            for (std::size_t i = 0; i < count; ++i) {
                if (i % source.width == 0 && token.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(TerrainSourceErrors::Cancelled));
                source.heightsMeters[i] = reader.F();
                const auto sum = ReadWeights(reader, source, i);
                if (hasHoles)
                    source.holes[i] = static_cast<std::uint8_t>(reader.U(1));
                if (!ValidHeightAndHole(source, i) || !NormalizedWeights(source.layerCount, sum))
                    return Result<void>::Failure(MakeError(TerrainSourceErrors::InvalidSample));
            }
            return Result<void>::Success();
        }

        /** @brief Admit aggregate raster/placement cost before either bulk payload is allocated. */
        Result<TerrainAuthoringSource> ReadSourceBody(Reader reader, SourceHeader header, const TerrainAuthoringCodecLimits &limits,
                                                      const CancellationToken &token) {
            using Return = Result<TerrainAuthoringSource>;
            if (header.foliageBytes > reader.bytes.size() - reader.at)
                return Return::Failure(MakeError(TerrainSourceErrors::InvalidBytes));
            const auto foliageBytes = reader.bytes.last(header.foliageBytes);
            auto footprint = MeasureEncodedFoliageAuthoringSource(foliageBytes, limits.foliage);
            if (footprint.HasError())
                return Return::Failure(std::move(footprint).ErrorValue());
            auto count = Shape(header.value, limits, header.hasHoles, footprint.Value());
            if (count.HasError())
                return Return::Failure(std::move(count).ErrorValue());
            reader.bytes = reader.bytes.first(reader.bytes.size() - header.foliageBytes);
            if (auto valid = ValidateSamplePayload(reader, Layout(header.value, header.hasHoles, footprint.Value())); valid.HasError())
                return Return::Failure(std::move(valid).ErrorValue());
            if (auto read = ReadSamples(reader, header.value.source, count.Value(), header.hasHoles, token); read.HasError())
                return Return::Failure(std::move(read).ErrorValue());
            auto foliage =
                DecodeFoliageAuthoringSource(foliageBytes, header.value.source.width, header.value.source.height, limits.foliage, token);
            if (foliage.HasError())
                return Return::Failure(std::move(foliage).ErrorValue());
            header.value.foliage = std::move(foliage).Value();
            return Return::Success(std::move(header.value));
        }

        Result<TerrainAuthoringLayerSet> ReadLayerSetIdentity(Reader &reader, Assets::AssetId expectedAsset) {
            using Return = Result<TerrainAuthoringLayerSet>;
            TerrainAuthoringLayerSet value;
            value.asset = Assets::AssetId::FromBytes(reader.Id<16>());
            const auto id = TerrainLayerSetId::Create(reader.Id<16>());
            const auto revision = TerrainContentRevision::Create(reader.U(8));
            if (!expectedAsset.IsValid() || value.asset != expectedAsset || id.HasError() || revision.HasError())
                return Return::Failure(MakeError(TerrainErrors::IdentityInvalid));
            value.layers.id = id.Value();
            value.layers.revision = revision.Value();
            return Return::Success(std::move(value));
        }

        Result<void> ReadMaterialLayers(Reader &reader, TerrainMaterialLayerSetData &layers) {
            layers.layerCount = static_cast<std::uint8_t>(reader.U(1));
            if (layers.layerCount == 0 || layers.layerCount > TerrainDescriptorHardLimits::LayersPerTile ||
                reader.bytes.size() - reader.at != std::size_t{layers.layerCount} * 36)
                return Result<void>::Failure(MakeError(TerrainSourceErrors::InvalidBytes));
            for (std::size_t i = 0; i < layers.layerCount; ++i) {
                const auto layer = TerrainLayerId::Create(reader.Id<16>());
                const auto material = TerrainMaterialAssetId::Create(reader.Id<16>());
                if (layer.HasError() || material.HasError())
                    return Result<void>::Failure(MakeError(TerrainErrors::IdentityInvalid));
                layers.layers[i] = {layer.Value(), material.Value(), static_cast<std::uint32_t>(reader.U(4))};
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc IsTerrainAuthoringSource */
    bool IsTerrainAuthoringSource(std::span<const std::byte> bytes) noexcept {
        if (bytes.size() < SourceMagic.size())
            return false;
        for (std::size_t i = 0; i < SourceMagic.size(); ++i)
            if (std::to_integer<std::uint8_t>(bytes[i]) != SourceMagic[i])
                return false;
        return true;
    }

    /** @copydoc EncodeTerrainAuthoringSource */
    Result<std::vector<std::byte>> EncodeTerrainAuthoringSource(const TerrainAuthoringSource &value,
                                                                const TerrainAuthoringCodecLimits &limits, const CancellationToken &token) {
        using Return = Result<std::vector<std::byte>>;
        try {
            if (token.IsCancellationRequested())
                return Return::Failure(MakeError(TerrainSourceErrors::Cancelled));
            const auto &source = value.source;
            auto footprint =
                MeasureFoliageAuthoringSource(value.foliage.placements.size(), value.foliage.definitions.size(), limits.foliage);
            if (footprint.HasError())
                return Return::Failure(std::move(footprint).ErrorValue());
            auto count = Shape(value, limits, source.HasHoles(), footprint.Value());
            if (count.HasError())
                return Return::Failure(std::move(count).ErrorValue());
            if (auto valid = ValidateChannelSizes(source, count.Value()); valid.HasError())
                return Return::Failure(std::move(valid).ErrorValue());
            auto foliage = EncodeFoliageAuthoringSource(value.foliage, source.width, source.height, limits.foliage, token);
            if (foliage.HasError())
                return Return::Failure(std::move(foliage).ErrorValue());
            Writer writer;
            writer.bytes.reserve(static_cast<std::size_t>(Layout(value, source.HasHoles(), footprint.Value()).EncodedBytes()));
            WriteSourceHeader(writer, value, foliage.Value().size());
            if (auto written = WriteSamples(writer, source, count.Value(), token); written.HasError())
                return Return::Failure(std::move(written).ErrorValue());
            writer.bytes.insert(writer.bytes.end(), foliage.Value().begin(), foliage.Value().end());
            return Finish(std::move(writer), token);
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(TerrainSourceErrors::LimitExceeded));
        }
    }

    /** @copydoc DecodeTerrainAuthoringSource */
    Result<TerrainAuthoringSource> DecodeTerrainAuthoringSource(std::span<const std::byte> bytes, Assets::AssetId expectedAsset,
                                                                TerrainCapabilityRevision capability,
                                                                const TerrainAuthoringCodecLimits &limits, const CancellationToken &token) {
        using Return = Result<TerrainAuthoringSource>;
        try {
            auto payload = AdmitSourcePayload(bytes, limits, token);
            if (payload.HasError())
                return Return::Failure(std::move(payload).ErrorValue());
            Reader reader{payload.Value(), SourceMagic.size()};
            if (reader.U(4) != Version)
                return Return::Failure(MakeError(TerrainSourceErrors::UnsupportedFormat));
            auto header = ReadSourceHeader(reader, expectedAsset, capability);
            if (header.HasError())
                return Return::Failure(std::move(header).ErrorValue());
            return ReadSourceBody(reader, std::move(header).Value(), limits, token);
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(TerrainSourceErrors::LimitExceeded));
        }
    }

    /** @copydoc EncodeTerrainAuthoringLayerSet */
    Result<std::vector<std::byte>> EncodeTerrainAuthoringLayerSet(const TerrainAuthoringLayerSet &value,
                                                                  const TerrainConfigurationSnapshot &configuration) {
        using Return = Result<std::vector<std::byte>>;
        try {
            if (!value.asset.IsValid())
                return Return::Failure(MakeError(TerrainErrors::IdentityInvalid));
            if (auto valid = TerrainMaterialLayerSet::Create(value.layers, configuration); valid.HasError())
                return Return::Failure(std::move(valid).ErrorValue());
            Writer writer;
            writer.bytes.reserve(LayerBytes);
            writer.Id(LayerMagic);
            writer.U(Version, 4);
            writer.Id(value.asset.Bytes());
            writer.Id(value.layers.id.Bytes());
            writer.U(value.layers.revision.Value(), 8);
            writer.U(value.layers.layerCount, 1);
            for (std::size_t i = 0; i < value.layers.layerCount; ++i) {
                const auto &layer = value.layers.layers[i];
                writer.Id(layer.id.Bytes());
                writer.Id(layer.material.Bytes());
                writer.U(layer.uvScaleMilli, 4);
            }
            return Finish(std::move(writer), {});
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(TerrainSourceErrors::LimitExceeded));
        }
    }

    /** @copydoc DecodeTerrainAuthoringLayerSet */
    Result<TerrainAuthoringLayerSet> DecodeTerrainAuthoringLayerSet(std::span<const std::byte> bytes, Assets::AssetId expectedAsset,
                                                                    const TerrainConfigurationSnapshot &configuration) {
        using Return = Result<TerrainAuthoringLayerSet>;
        try {
            auto payload = Payload(bytes, LayerBytes, {});
            if (payload.HasError())
                return Return::Failure(std::move(payload).ErrorValue());
            Reader reader{payload.Value()};
            if (reader.Id<8>() != LayerMagic || reader.U(4) != Version)
                return Return::Failure(MakeError(TerrainSourceErrors::UnsupportedFormat));
            auto identity = ReadLayerSetIdentity(reader, expectedAsset);
            if (identity.HasError())
                return Return::Failure(std::move(identity).ErrorValue());
            auto value = std::move(identity).Value();
            if (auto read = ReadMaterialLayers(reader, value.layers); read.HasError())
                return Return::Failure(std::move(read).ErrorValue());
            if (auto valid = TerrainMaterialLayerSet::Create(value.layers, configuration); valid.HasError())
                return Return::Failure(std::move(valid).ErrorValue());
            return Return::Success(std::move(value));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(TerrainSourceErrors::LimitExceeded));
        }
    }

    /** @copydoc ValidateTerrainAuthoringLayerBinding */
    Result<void> ValidateTerrainAuthoringLayerBinding(const TerrainAuthoringSource &source, const TerrainAuthoringLayerSet &layers,
                                                      const Sha256Digest &capturedDigest) {
        if (!source.layers || source.source.layerCount != layers.layers.layerCount || source.layers->asset != layers.asset ||
            source.layers->id != layers.layers.id || source.layers->revision != layers.layers.revision ||
            source.layers->digest != capturedDigest)
            return Result<void>::Failure(MakeError(TerrainSourceErrors::RevisionStale));
        for (std::size_t i = 0; i < source.source.layerCount; ++i)
            if (source.layers->order[i] != layers.layers.layers[i].id)
                return Result<void>::Failure(MakeError(TerrainSourceErrors::RevisionStale));
        return Result<void>::Success();
    }
}  // namespace Horo::Terrain
