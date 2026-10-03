#include "Horo/Navigation/NavMeshAssetLoading.h"

#include "Horo/Navigation/NavigationErrors.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <tuple>

namespace Horo::Navigation {
    namespace {
        constexpr std::string_view BundleSignature = "HNAVASSET1";

        /** @brief Qualified bundle limits are fixed before manifest parsing or producer allocation. */
        [[nodiscard]] bool ValidLimits(const NavMeshAssetLimits &limits) noexcept {
            return limits.maximumPartitions > 0 && limits.maximumPartitions <= 64 && limits.maximumDependencies <= 1024 &&
                   limits.cook.maximumArtifactBytes > 0 && limits.cook.maximumArtifactBytes <= 256U * 1024U * 1024U;
        }

        /** @brief Content evidence must be present; identity/type verification remains canonical AssetPipeline work. */
        [[nodiscard]] bool Present(const Sha256Digest &digest) noexcept {
            return std::ranges::any_of(digest.bytes, [](const auto byte) {
                return byte != 0;
            });
        }

        /** @brief Bounded line metadata followed by length-delimited opaque neutral partitions. */
        class BundleWriter final {
        public:
            explicit BundleWriter(const std::size_t maximum) : maximum_(maximum) {}

            void Line(const std::string_view text) {
                if (!valid || text.size() >= maximum_ - bytes.size()) {
                    valid = false;
                    return;
                }
                bytes.insert(bytes.end(), text.begin(), text.end());
                bytes.push_back('\n');
            }

            void Number(const std::uint64_t value) {
                std::array<char, 24> buffer{};
                const auto encoded = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
                Line({buffer.data(), static_cast<std::size_t>(encoded.ptr - buffer.data())});
            }

            void Mesh(const std::uint64_t surface, const std::uint64_t generation, const std::span<const std::byte> mesh) {
                Number(surface);
                Number(generation);
                Number(mesh.size());
                if (!valid || mesh.size() >= maximum_ - bytes.size()) {
                    valid = false;
                    return;
                }
                for (const auto byte : mesh)
                    bytes.push_back(std::to_integer<std::uint8_t>(byte));
                bytes.push_back('\n');
            }

            std::vector<std::uint8_t> bytes;
            bool valid{true};

            std::size_t maximum_{};
        };

        /** @brief No unbounded delimiter search or string allocation occurs while reading manifest fields. */
        class BundleReader final {
        public:
            explicit BundleReader(const std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

            [[nodiscard]] std::string_view Line() noexcept {
                const auto start = position;
                const auto end = std::min(bytes_.size(), start + 129);
                while (position < end) {
                    if (bytes_[position++] == '\n') {
                        return {reinterpret_cast<const char *>(bytes_.data() + start), position - start - 1};
                    }
                }
                valid = false;
                return {};
            }

            [[nodiscard]] std::uint64_t Number() noexcept {
                const auto text = Line();
                std::uint64_t number{};
                if (const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
                    text.empty() || (text.size() > 1 && text.front() == '0') || parsed.ec != std::errc{} ||
                    parsed.ptr != text.data() + text.size())
                    valid = false;
                return number;
            }

            [[nodiscard]] std::span<const std::uint8_t> Raw(const std::uint64_t size) noexcept {
                if (size > bytes_.size() - position) {
                    valid = false;
                    return {};
                }
                const auto bytes = bytes_.subspan(position, static_cast<std::size_t>(size));
                position += static_cast<std::size_t>(size);
                return bytes;
            }

            [[nodiscard]] bool Finished() const noexcept {
                return valid && position == bytes_.size();
            }

            std::size_t position{};
            bool valid{true};

            std::span<const std::uint8_t> bytes_;
        };

        /** @brief Asset dependency rows are canonical and unique by AssetId, regardless of declared type. */
        [[nodiscard]] bool ValidDependencies(const std::span<const NavMeshAssetDependency> dependencies) {
            const NavMeshAssetDependency *previous{};
            for (const auto &dependency : dependencies) {
                if (!dependency.asset.id.IsValid() || dependency.asset.expectedType.Value().empty() ||
                    !Present(dependency.cookedContentDigest) || (previous && previous->asset.id >= dependency.asset.id))
                    return false;
                previous = &dependency;
            }
            return true;
        }

        /** @brief Surface/profile tuple order is canonical; all partitions for a surface share its generation. */
        template <class Partition> [[nodiscard]] bool OrderedPartition(const Partition &partition, const Partition *previous) {
            if (!partition.surface.IsValid() || partition.surfaceGeneration == 0)
                return false;
            if (!previous)
                return true;
            const auto &profile = [&]() -> const NavMeshArtifactHeader & {
                if constexpr (std::is_same_v<Partition, NavMeshAssetPartitionInput>)
                    return partition.artifact.header;
                else
                    return partition.data.Header();
            }();
            const auto &previousProfile = [&]() -> const NavMeshArtifactHeader & {
                if constexpr (std::is_same_v<Partition, NavMeshAssetPartitionInput>)
                    return previous->artifact.header;
                else
                    return previous->data.Header();
            }();
            return std::tuple{previous->surface, previousProfile.profile.id} < std::tuple{partition.surface, profile.profile.id} &&
                   (previous->surface != partition.surface || previous->surfaceGeneration == partition.surfaceGeneration);
        }

        /** @brief Read bounded dependency metadata before decoding any mesh partition. */
        [[nodiscard]] Result<std::vector<NavMeshAssetDependency>> ReadDependencies(BundleReader &reader, const std::size_t count) {
            std::vector<NavMeshAssetDependency> dependencies;
            dependencies.reserve(count);
            for (std::size_t index = 0; index < count; ++index) {
                const auto id = Assets::AssetId::Parse(reader.Line());
                const auto type = Assets::AssetTypeId::Parse(reader.Line());
                const auto digest = ParseSha256(reader.Line());
                if (!reader.valid || id.HasError() || type.HasError() || digest.HasError()) {
                    return Result<std::vector<NavMeshAssetDependency>>::Failure(MakeError(NavigationErrors::NavMeshArtifactCorrupt));
                }
                dependencies.push_back({{id.Value(), type.Value()}, digest.Value()});
            }
            if (!ValidDependencies(dependencies)) {
                return Result<std::vector<NavMeshAssetDependency>>::Failure(MakeError(NavigationErrors::NavMeshArtifactCorrupt));
            }
            return Result<std::vector<NavMeshAssetDependency>>::Success(std::move(dependencies));
        }

        /** @brief Decode one complete generated partition before adding its borrowed immutable byte ranges. */
        [[nodiscard]] Result<void> ReadPartition(BundleReader &reader, const NavMeshAssetLimits &limits, LoadedNavMeshAsset &loaded,
                                                 std::vector<std::span<const std::byte>> &tiles) {
            const auto surface = SurfaceId::Create(reader.Number());
            const auto generation = reader.Number();
            const auto encoded = reader.Raw(reader.Number());
            if (const auto delimiter = reader.Line(); !reader.valid || !delimiter.empty() || surface.HasError() || generation == 0) {
                return Result<void>::Failure(MakeError(NavigationErrors::NavMeshArtifactCorrupt));
            }
            auto decoded = DecodeNavMeshArtifact(std::as_bytes(encoded), limits.mesh);
            if (decoded.HasError())
                return Result<void>::Failure(decoded.ErrorValue());
            auto decodedArtifact = std::move(decoded).Value();
            LoadedNavMeshPartition partition{surface.Value(), generation, std::move(decodedArtifact.data)};
            if (!OrderedPartition(partition, loaded.partitions.empty() ? nullptr : &loaded.partitions.back())) {
                return Result<void>::Failure(MakeError(NavigationErrors::NavMeshArtifactCorrupt));
            }
            for (const auto &tile : decodedArtifact.encodedTiles) {
                tiles.push_back(std::as_bytes(encoded).subspan(tile.offset, tile.bytes));
            }
            loaded.partitions.push_back(std::move(partition));
            return Result<void>::Success();
        }

        /** @brief Decode all bounded partitions transactionally before cache admission. */
        [[nodiscard]] Result<void> ReadPartitions(BundleReader &reader, const std::size_t count, const NavMeshAssetLimits &limits,
                                                  LoadedNavMeshAsset &loaded, std::vector<std::span<const std::byte>> &tiles) {
            loaded.partitions.reserve(count);
            for (std::size_t index = 0; index < count; ++index) {
                if (const auto parsed = ReadPartition(reader, limits, loaded, tiles); parsed.HasError())
                    return parsed;
            }
            if (!reader.Finished())
                return Result<void>::Failure(MakeError(NavigationErrors::NavMeshArtifactCorrupt));
            return Result<void>::Success();
        }

        /** @brief Producer manifest bounds are independent of the per-partition geometry schema. */
        [[nodiscard]] bool ValidBundleShape(const std::span<const NavMeshAssetPartitionInput> partitions,
                                            const std::span<const NavMeshAssetDependency> dependencies, const NavMeshAssetLimits &limits) {
            return ValidLimits(limits) && !partitions.empty() && partitions.size() <= limits.maximumPartitions &&
                   dependencies.size() <= limits.maximumDependencies && ValidDependencies(dependencies);
        }

        /** @brief Require an exact canonical registry/Scene metadata incarnation before envelope decoding. */
        [[nodiscard]] bool ValidMetadata(const Assets::AssetDependency &metadata, const Assets::AssetRegistryRevision revision,
                                         const NavMeshAssetLimits &limits) {
            return ValidLimits(limits) && metadata.id.IsValid() && metadata.expectedType.Value() == Assets::NavMeshAssetTypeName &&
                   revision.value != 0;
        }

        /** @brief Canonical envelopes cannot substitute another asset identity, type or host-selected target. */
        [[nodiscard]] bool MatchesEnvelope(const Assets::AssetCookArtifact &envelope, const Assets::AssetDependency &metadata,
                                           const AssetCookTargetId &target) {
            return envelope.id == metadata.id && envelope.type == metadata.expectedType && envelope.target == target;
        }

        /** @brief Verify manifest, closure evidence and every partition before any cache admission. */
        [[nodiscard]] Result<void> ReadBundle(const Assets::AssetCookArtifact &envelope, const NavMeshAssetLimits &limits,
                                              LoadedNavMeshAsset &loaded, std::vector<std::span<const std::byte>> &tiles) {
            BundleReader reader{envelope.payload};
            const auto signature = reader.Line();
            const auto partitionCount = reader.Number();
            const auto dependencyCount = reader.Number();
            if (!reader.valid || signature != BundleSignature || partitionCount == 0 || partitionCount > limits.maximumPartitions ||
                dependencyCount > limits.maximumDependencies)
                return Result<void>::Failure(MakeError(NavigationErrors::NavMeshArtifactCorrupt));
            auto dependencies = ReadDependencies(reader, static_cast<std::size_t>(dependencyCount));
            if (dependencies.HasError())
                return Result<void>::Failure(dependencies.ErrorValue());
            if (std::ranges::any_of(dependencies.Value(), [&](const auto &dependency) {
                return dependency.asset.id == loaded.id;
            }))
                return Result<void>::Failure(MakeError(NavigationErrors::NavMeshArtifactCorrupt));
            loaded.dependencies = std::move(dependencies).Value();
            return ReadPartitions(reader, static_cast<std::size_t>(partitionCount), limits, loaded, tiles);
        }

        /** @brief Pins are admitted only after whole-bundle validation; no live-world authority is modified. */
        [[nodiscard]] Result<void> PinTiles(LoadedNavMeshAsset &loaded, const std::span<const std::span<const std::byte>> tiles,
                                            Assets::AssetPayloadCache &cache) {
            loaded.tileBytes.reserve(tiles.size());
            for (const auto bytes : tiles) {
                auto admitted = cache.Admit(bytes);
                if (admitted.HasError())
                    return Result<void>::Failure(admitted.ErrorValue());
                loaded.tileBytes.push_back(std::move(admitted).Value());
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc EncodeNavMeshAssetPayload */
    Result<std::vector<std::uint8_t>> EncodeNavMeshAssetPayload(const std::span<const NavMeshAssetPartitionInput> partitions,
                                                                const std::span<const NavMeshAssetDependency> dependencies,
                                                                const NavMeshAssetLimits &limits) {
        if (!ValidBundleShape(partitions, dependencies, limits)) {
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(NavigationErrors::NavMeshArtifactInvalid));
        }
        BundleWriter writer{limits.cook.maximumArtifactBytes};
        writer.Line(BundleSignature);
        writer.Number(partitions.size());
        writer.Number(dependencies.size());
        for (const auto &dependency : dependencies) {
            writer.Line(dependency.asset.id.ToString());
            writer.Line(dependency.asset.expectedType.Value());
            writer.Line(FormatSha256(dependency.cookedContentDigest));
        }
        const NavMeshAssetPartitionInput *previous{};
        for (const auto &partition : partitions) {
            if (!OrderedPartition(partition, previous)) {
                return Result<std::vector<std::uint8_t>>::Failure(MakeError(NavigationErrors::NavMeshArtifactInvalid));
            }
            auto mesh = EncodeNavMeshArtifact(partition.artifact, limits.mesh);
            if (mesh.HasError())
                return Result<std::vector<std::uint8_t>>::Failure(mesh.ErrorValue());
            writer.Mesh(partition.surface.Value(), partition.surfaceGeneration, mesh.Value());
            previous = &partition;
        }
        if (!writer.valid)
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(NavigationErrors::NavMeshArtifactCapacityExceeded));
        return Result<std::vector<std::uint8_t>>::Success(std::move(writer.bytes));
    }

    /** @copydoc LoadNavMeshAsset */
    Result<LoadedNavMeshAsset> LoadNavMeshAsset(const Assets::AssetDependency &metadata,
                                                const Assets::AssetRegistryRevision registryRevision,
                                                const std::span<const std::uint8_t> encoded, const AssetCookTargetId &target,
                                                Assets::AssetPayloadCache &cache, const NavMeshAssetLimits &limits) {
        if (!ValidMetadata(metadata, registryRevision, limits))
            return Result<LoadedNavMeshAsset>::Failure(MakeError(NavigationErrors::NavMeshArtifactInvalid));
        const auto envelope = Assets::DecodeCookedArtifact(encoded, limits.cook);
        if (envelope.HasError())
            return Result<LoadedNavMeshAsset>::Failure(envelope.ErrorValue());
        if (!MatchesEnvelope(envelope.Value(), metadata, target))
            return Result<LoadedNavMeshAsset>::Failure(MakeError(NavigationErrors::NavMeshArtifactCorrupt));
        LoadedNavMeshAsset loaded{metadata.id, registryRevision, ComputeSha256(std::as_bytes(encoded)), envelope.Value().sourceDigest,
                                  envelope.Value().cacheKeyDigest};
        std::vector<std::span<const std::byte>> tiles;
        if (const auto parsed = ReadBundle(envelope.Value(), limits, loaded, tiles); parsed.HasError())
            return Result<LoadedNavMeshAsset>::Failure(parsed.ErrorValue());
        if (const auto pinned = PinTiles(loaded, tiles, cache); pinned.HasError())
            return Result<LoadedNavMeshAsset>::Failure(pinned.ErrorValue());
        return Result<LoadedNavMeshAsset>::Success(std::move(loaded));
    }

    /** @copydoc LoadNavMeshAsset */
    Result<LoadedNavMeshAsset> LoadNavMeshAsset(const Assets::AssetRegistrySnapshot &registry, const Assets::IAssetProvider &provider,
                                                const Assets::AssetId id, const AssetCookTargetId &target, Assets::AssetPayloadCache &cache,
                                                const CancellationToken &cancellation, const NavMeshAssetLimits &limits) {
        const auto *record = registry.Find(id);
        if (!record)
            return Result<LoadedNavMeshAsset>::Failure(MakeError(NavigationErrors::NoNavigationData));
        const auto bytes = provider.Load(id, cancellation);
        if (bytes.HasError())
            return Result<LoadedNavMeshAsset>::Failure(bytes.ErrorValue());
        return LoadNavMeshAsset(Assets::AssetDependency{record->id, record->type}, registry.Revision(), bytes.Value(), target, cache,
                                limits);
    }
}  // namespace Horo::Navigation
