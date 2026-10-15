#include "WorldDependencyArtifactWire.h"

namespace Horo::WorldStreaming {
    namespace {
        using namespace Detail;

        /** @brief Tests only already range-admitted reserved fields. */
        bool Zero(const std::span<const std::byte> bytes) {
            return std::ranges::all_of(bytes, [](const auto value) {
                return value == std::byte{};
            });
        }

        /** @brief Requires the exact index incarnation and schema before exposing any dependent content. */
        Result<void> CheckBinding(const std::span<const std::byte> bytes, const DecodedWorldIndex &index) {
            if (WorldIndexRead<std::uint16_t>(bytes, 32) != 1 || WorldIndexRead<std::uint16_t>(bytes, 34) != 0)
                return WorldIndexReject<void>(WorldArtifactErrors::Unsupported);
            if (index.artifactHash == Sha256Digest{} || !index.descriptor.Partition().IsValid())
                return WorldIndexReject<void>(WorldArtifactErrors::Invalid);
            for (std::size_t position = 0; position < 16; ++position)
                if (bytes[16 + position] != static_cast<std::byte>(index.descriptor.Partition().Bytes()[position]))
                    return WorldIndexReject<void>(WorldArtifactErrors::Stale);
            for (std::size_t position = 0; position < 32; ++position)
                if (bytes[40 + position] != static_cast<std::byte>(index.artifactHash.bytes[position]))
                    return WorldIndexReject<void>(WorldArtifactErrors::Stale);
            return Result<void>::Success();
        }

        /** @brief Validates fixed bytes, exact table packing, both count budgets and the complete sibling digest. */
        Result<DependencyLayout> ReadHeader(const std::span<const std::byte> bytes, const DecodedWorldIndex &index,
                                            const WorldDependencyArtifactLimits &limits, const CancellationToken &cancellation) {
            if (bytes.size() > limits.maximumArtifactBytes)
                return WorldIndexReject<DependencyLayout>(WorldArtifactErrors::CapacityExceeded);
            if (bytes.size() < DependencyHeaderBytes)
                return WorldIndexReject<DependencyLayout>(WorldArtifactErrors::Invalid);
            constexpr std::string_view magic{"HORODEPS"};
            for (std::size_t position = 0; position < magic.size(); ++position)
                if (bytes[position] != static_cast<std::byte>(magic[position]))
                    return WorldIndexReject<DependencyLayout>(WorldArtifactErrors::Invalid);
            if (WorldIndexRead<std::uint16_t>(bytes, 8) != 1 || WorldIndexRead<std::uint16_t>(bytes, 10) != 0)
                return WorldIndexReject<DependencyLayout>(WorldArtifactErrors::Unsupported);
            if (!Zero(bytes.subspan(12, 4)) || !Zero(bytes.subspan(36, 4)) || !Zero(bytes.subspan(176, 16)))
                return WorldIndexReject<DependencyLayout>(WorldArtifactErrors::Invalid);
            if (const auto binding = CheckBinding(bytes, index); binding.HasError())
                return Result<DependencyLayout>::Failure(binding.ErrorValue());
            std::array<std::size_t, 8> counts{};
            for (std::size_t table = 0; table < counts.size(); ++table)
                counts[table] = WorldIndexRead<std::uint32_t>(bytes, 72 + table * 4);
            auto admitted = AdmitDependencyLayout(counts, limits);
            if (admitted.HasError())
                return admitted;
            const auto &layout = admitted.Value();
            if (layout.total != bytes.size() || WorldIndexRead<std::uint64_t>(bytes, 136) != bytes.size() ||
                counts[6] != index.cells.size())
                return WorldIndexReject<DependencyLayout>(WorldArtifactErrors::Invalid);
            for (std::size_t table = 0; table < counts.size(); ++table)
                if (WorldIndexRead<std::uint32_t>(bytes, 104 + table * 4) != layout.offsets[table])
                    return WorldIndexReject<DependencyLayout>(WorldArtifactErrors::Invalid);
            auto digest = DependencyDigest(bytes, cancellation);
            if (digest.HasError())
                return Result<DependencyLayout>::Failure(digest.ErrorValue());
            for (std::size_t position = 0; position < 32; ++position)
                if (bytes[DependencyHashOffset + position] != static_cast<std::byte>(digest.Value().bytes[position]))
                    return WorldIndexReject<DependencyLayout>(WorldArtifactErrors::Invalid);
            return admitted;
        }

        /** @brief Copies a previously admitted record slice with cancellation inside large individual ranges. */
        Result<std::vector<StreamingCellId>> ReadCells(const std::span<const std::byte> bytes, const std::size_t tableOffset,
                                                       const std::size_t first, const std::size_t count,
                                                       const CancellationToken &cancellation) {
            std::vector<StreamingCellId> result;
            result.reserve(count);
            for (std::size_t position = 0; position < count; ++position) {
                if (cancellation.IsCancellationRequested())
                    return WorldIndexReject<std::vector<StreamingCellId>>(WorldArtifactErrors::Cancelled);
                auto cell = ReadDependencyCell(bytes, tableOffset + (first + position) * 16);
                if (cell.HasError())
                    return Result<std::vector<StreamingCellId>>::Failure(cell.ErrorValue());
                result.push_back(cell.Value());
            }
            return Result<std::vector<StreamingCellId>>::Success(std::move(result));
        }

        /** @brief Decodes complete object records only after both slices pass exact prefix-range validation. */
        Result<void> ReadObjects(DecodedWorldDependencies &model, const std::span<const std::byte> bytes, const DependencyLayout &layout,
                                 const CancellationToken &cancellation) {
            std::size_t nextCoverage{};
            std::size_t nextPlacement{};
            model.objects.reserve(layout.counts[0]);
            for (std::size_t position = 0; position < layout.counts[0]; ++position) {
                if (cancellation.IsCancellationRequested())
                    return WorldIndexReject<void>(WorldArtifactErrors::Cancelled);
                const auto offset = layout.offsets[0] + position * 64;
                const auto placement = WorldIndexRead<std::uint32_t>(bytes, offset + 32);
                const auto coverageOffset = WorldIndexRead<std::uint32_t>(bytes, offset + 36);
                const auto coverageCount = WorldIndexRead<std::uint32_t>(bytes, offset + 40);
                const auto placementOffset = WorldIndexRead<std::uint32_t>(bytes, offset + 44);
                const auto placementCount = WorldIndexRead<std::uint32_t>(bytes, offset + 48);
                if (placement > static_cast<std::uint32_t>(WorldSpanningObjectPlacement::NonSpatial) ||
                    !Zero(bytes.subspan(offset + 52, 12)) ||
                    !TakeDependencySlice(coverageOffset, coverageCount, layout.counts[1], nextCoverage) ||
                    !TakeDependencySlice(placementOffset, placementCount, layout.counts[2], nextPlacement))
                    return WorldIndexReject<void>(WorldArtifactErrors::Invalid);
                auto endpoint = ReadDependencyEndpoint(bytes, offset);
                if (endpoint.HasError())
                    return Result<void>::Failure(endpoint.ErrorValue());
                auto covered = ReadCells(bytes, layout.offsets[1], coverageOffset, coverageCount, cancellation);
                if (covered.HasError())
                    return Result<void>::Failure(covered.ErrorValue());
                auto placed = ReadCells(bytes, layout.offsets[2], placementOffset, placementCount, cancellation);
                if (placed.HasError())
                    return Result<void>::Failure(placed.ErrorValue());
                model.objects.push_back({endpoint.Value(), static_cast<WorldSpanningObjectPlacement>(placement), std::move(covered).Value(),
                                         std::move(placed).Value()});
            }
            if (nextCoverage != layout.counts[1] || nextPlacement != layout.counts[2])
                return WorldIndexReject<void>(WorldArtifactErrors::Invalid);
            return Result<void>::Success();
        }

        /** @brief Restores exact disjoint-component members with no aliased or unused member slice. */
        Result<void> ReadBundles(DecodedWorldDependencies &model, const std::span<const std::byte> bytes, const DependencyLayout &layout,
                                 const CancellationToken &cancellation) {
            std::size_t nextMember{};
            model.bundles.reserve(layout.counts[3]);
            for (std::size_t position = 0; position < layout.counts[3]; ++position) {
                const auto offset = layout.offsets[3] + position * 8;
                const auto first = WorldIndexRead<std::uint32_t>(bytes, offset);
                const auto count = WorldIndexRead<std::uint32_t>(bytes, offset + 4);
                if (count < 2 || !TakeDependencySlice(first, count, layout.counts[4], nextMember))
                    return WorldIndexReject<void>(WorldArtifactErrors::Invalid);
                std::vector<WorldDependencyEndpoint> members;
                members.reserve(count);
                for (std::size_t member = 0; member < count; ++member) {
                    if (cancellation.IsCancellationRequested())
                        return WorldIndexReject<void>(WorldArtifactErrors::Cancelled);
                    auto endpoint = ReadDependencyEndpoint(bytes, layout.offsets[4] + (first + member) * 32);
                    if (endpoint.HasError())
                        return Result<void>::Failure(endpoint.ErrorValue());
                    members.push_back(endpoint.Value());
                }
                model.bundles.push_back(std::move(members));
            }
            if (nextMember != layout.counts[4])
                return WorldIndexReject<void>(WorldArtifactErrors::Invalid);
            return Result<void>::Success();
        }

        /** @brief Preserves all explicitly deferred references, including absent soft targets. */
        Result<void> ReadSoftReferences(DecodedWorldDependencies &model, const std::span<const std::byte> bytes,
                                        const DependencyLayout &layout, const CancellationToken &cancellation) {
            model.softReferences.reserve(layout.counts[5]);
            for (std::size_t position = 0; position < layout.counts[5]; ++position) {
                if (cancellation.IsCancellationRequested())
                    return WorldIndexReject<void>(WorldArtifactErrors::Cancelled);
                const auto offset = layout.offsets[5] + position * 64;
                auto source = ReadDependencyEndpoint(bytes, offset);
                if (source.HasError())
                    return Result<void>::Failure(source.ErrorValue());
                auto target = ReadDependencyEndpoint(bytes, offset + 32);
                if (target.HasError())
                    return Result<void>::Failure(target.ErrorValue());
                model.softReferences.push_back({source.Value(), target.Value(), WorldDependencyKind::Soft});
            }
            return Result<void>::Success();
        }

        /** @brief Restores every index cell and its strictly packed target slice before independent derivation. */
        Result<void> ReadCellEdges(DecodedWorldDependencies &model, const std::span<const std::byte> bytes, const DecodedWorldIndex &index,
                                   const DependencyLayout &layout, const CancellationToken &cancellation) {
            std::size_t nextTarget{};
            model.cells.reserve(layout.counts[6]);
            for (std::size_t position = 0; position < layout.counts[6]; ++position) {
                const auto offset = layout.offsets[6] + position * 24;
                auto cell = ReadDependencyCell(bytes, offset);
                if (cell.HasError())
                    return Result<void>::Failure(cell.ErrorValue());
                const auto first = WorldIndexRead<std::uint32_t>(bytes, offset + 16);
                const auto count = WorldIndexRead<std::uint32_t>(bytes, offset + 20);
                if (cell.Value() != index.cells[position].cell || !TakeDependencySlice(first, count, layout.counts[7], nextTarget))
                    return WorldIndexReject<void>(WorldArtifactErrors::Invalid);
                auto targets = ReadCells(bytes, layout.offsets[7], first, count, cancellation);
                if (targets.HasError())
                    return Result<void>::Failure(targets.ErrorValue());
                model.cells.push_back({cell.Value(), std::move(targets).Value()});
            }
            if (nextTarget != layout.counts[7])
                return WorldIndexReject<void>(WorldArtifactErrors::Invalid);
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc DecodeWorldDependencyArtifact */
    Result<DecodedWorldDependencies> DecodeWorldDependencyArtifact(const std::span<const std::byte> bytes, const DecodedWorldIndex &index,
                                                                   const WorldDependencyArtifactLimits &limits,
                                                                   const CancellationToken &cancellation) {
        if (cancellation.IsCancellationRequested())
            return WorldIndexReject<DecodedWorldDependencies>(WorldArtifactErrors::Cancelled);
        auto admitted = ReadHeader(bytes, index, limits, cancellation);
        if (admitted.HasError())
            return Result<DecodedWorldDependencies>::Failure(admitted.ErrorValue());
        const auto layout = admitted.Value();
        DecodedWorldDependencies model{.partition = index.descriptor.Partition(), .indexArtifactHash = index.artifactHash};
        if (auto objects = ReadObjects(model, bytes, layout, cancellation); objects.HasError())
            return Result<DecodedWorldDependencies>::Failure(objects.ErrorValue());
        if (auto bundles = ReadBundles(model, bytes, layout, cancellation); bundles.HasError())
            return Result<DecodedWorldDependencies>::Failure(bundles.ErrorValue());
        if (auto soft = ReadSoftReferences(model, bytes, layout, cancellation); soft.HasError())
            return Result<DecodedWorldDependencies>::Failure(soft.ErrorValue());
        if (auto cells = ReadCellEdges(model, bytes, index, layout, cancellation); cells.HasError())
            return Result<DecodedWorldDependencies>::Failure(cells.ErrorValue());
        auto derived = DeriveWorldDependencyCells(model, index, limits, cancellation);
        if (derived.HasError())
            return Result<DecodedWorldDependencies>::Failure(derived.ErrorValue());
        if (!std::ranges::equal(model.cells, derived.Value(), [](const auto &stored, const auto &expected) {
            return stored.cell == expected.cell && stored.hardTargets == expected.hardTargets;
        }))
            return WorldIndexReject<DecodedWorldDependencies>(WorldArtifactErrors::Invalid);
        return Result<DecodedWorldDependencies>::Success(std::move(model));
    }
}  // namespace Horo::WorldStreaming
