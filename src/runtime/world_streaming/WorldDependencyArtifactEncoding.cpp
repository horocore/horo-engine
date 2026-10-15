#include "WorldDependencyArtifactWire.h"

namespace Horo::WorldStreaming {
    namespace {
        using namespace Detail;

        /** @brief Adds one source-table count without narrowing or overflowing the version-one range. */
        bool AddCount(std::size_t &count, const std::size_t added) noexcept {
            if (added > std::numeric_limits<std::uint32_t>::max() - count)
                return false;
            count += added;
            return true;
        }

        /** @brief Admits owner-plan storage before copying it; complete derived edges are admitted again before encoding. */
        Result<DependencyLayout> OwnerLayout(const DecodedWorldIndex &index, const WorldSpatialAssignment &assignments,
                                             const WorldSpanningObjectPlan &placements, const WorldDependencyPlan &dependencies,
                                             const WorldDependencyArtifactLimits &limits) {
            std::array<std::size_t, 8>
                counts{assignments.Objects().size(), 0, 0, dependencies.Bundles().size(), 0, dependencies.SoftReferences().size(),
                       index.cells.size(),           0};
            for (std::size_t position = 0; position < assignments.Objects().size(); ++position)
                if (!AddCount(counts[1], assignments.CellsForObject(position).size()) ||
                    !AddCount(counts[2], placements.CellsForObject(position).size()))
                    return WorldIndexReject<DependencyLayout>(WorldArtifactErrors::CapacityExceeded);
            for (std::size_t position = 0; position < dependencies.Bundles().size(); ++position)
                if (!AddCount(counts[4], dependencies.MembersForBundle(position).size()))
                    return WorldIndexReject<DependencyLayout>(WorldArtifactErrors::CapacityExceeded);
            return AdmitDependencyLayout(counts, limits);
        }

        /** @brief Fills exact binding and packed section metadata into the already zero-filled header. */
        void WriteHeader(std::vector<std::byte> &bytes, const DecodedWorldIndex &index, const DependencyLayout &layout) {
            constexpr std::string_view magic{"HORODEPS"};
            for (std::size_t position = 0; position < magic.size(); ++position)
                bytes[position] = static_cast<std::byte>(magic[position]);
            WorldIndexWrite(bytes, 8, std::uint16_t{1});
            WorldIndexWrite(bytes, 32, std::uint16_t{1});
            for (std::size_t position = 0; position < 16; ++position)
                bytes[16 + position] = static_cast<std::byte>(index.descriptor.Partition().Bytes()[position]);
            for (std::size_t position = 0; position < 32; ++position)
                bytes[40 + position] = static_cast<std::byte>(index.artifactHash.bytes[position]);
            for (std::size_t table = 0; table < layout.counts.size(); ++table) {
                WorldIndexWrite(bytes, 72 + table * 4, static_cast<std::uint32_t>(layout.counts[table]));
                WorldIndexWrite(bytes, 104 + table * 4, static_cast<std::uint32_t>(layout.offsets[table]));
            }
            WorldIndexWrite(bytes, 136, static_cast<std::uint64_t>(layout.total));
        }

        /** @brief Copies one canonical cell slice with cancellation inside large single-object coverage sets. */
        Result<std::size_t> WriteCells(std::vector<std::byte> &bytes, const std::size_t tableOffset, std::size_t cursor,
                                       const std::span<const StreamingCellId> cells, const CancellationToken &cancellation) {
            for (const auto &cell : cells) {
                if (cancellation.IsCancellationRequested())
                    return WorldIndexReject<std::size_t>(WorldArtifactErrors::Cancelled);
                WriteDependencyCell(bytes, tableOffset + cursor * 16, cell);
                ++cursor;
            }
            return Result<std::size_t>::Success(cursor);
        }

        /** @brief Encodes exact object endpoints and their two independently packed cell slices. */
        Result<void> WriteObjects(std::vector<std::byte> &bytes, const DecodedWorldDependencies &model, const DependencyLayout &layout,
                                  const CancellationToken &cancellation) {
            std::size_t coverage{};
            std::size_t placements{};
            for (std::size_t position = 0; position < model.objects.size(); ++position) {
                const auto &object = model.objects[position];
                const auto offset = layout.offsets[0] + position * 64;
                WriteDependencyEndpoint(bytes, offset, object.object);
                WorldIndexWrite(bytes, offset + 32, static_cast<std::uint32_t>(object.placement));
                WorldIndexWrite(bytes, offset + 36, static_cast<std::uint32_t>(coverage));
                WorldIndexWrite(bytes, offset + 40, static_cast<std::uint32_t>(object.coverage.size()));
                WorldIndexWrite(bytes, offset + 44, static_cast<std::uint32_t>(placements));
                WorldIndexWrite(bytes, offset + 48, static_cast<std::uint32_t>(object.cells.size()));
                auto covered = WriteCells(bytes, layout.offsets[1], coverage, object.coverage, cancellation);
                if (covered.HasError())
                    return Result<void>::Failure(covered.ErrorValue());
                coverage = covered.Value();
                auto placed = WriteCells(bytes, layout.offsets[2], placements, object.cells, cancellation);
                if (placed.HasError())
                    return Result<void>::Failure(placed.ErrorValue());
                placements = placed.Value();
            }
            return Result<void>::Success();
        }

        /** @brief Serializes hard components and explicit soft edges without retaining authoring pointers. */
        Result<void> WriteReferences(std::vector<std::byte> &bytes, const DecodedWorldDependencies &model, const DependencyLayout &layout,
                                     const CancellationToken &cancellation) {
            std::size_t members{};
            for (std::size_t position = 0; position < model.bundles.size(); ++position) {
                const auto &bundle = model.bundles[position];
                const auto offset = layout.offsets[3] + position * 8;
                WorldIndexWrite(bytes, offset, static_cast<std::uint32_t>(members));
                WorldIndexWrite(bytes, offset + 4, static_cast<std::uint32_t>(bundle.size()));
                for (const auto &endpoint : bundle) {
                    if (cancellation.IsCancellationRequested())
                        return WorldIndexReject<void>(WorldArtifactErrors::Cancelled);
                    WriteDependencyEndpoint(bytes, layout.offsets[4] + members * 32, endpoint);
                    ++members;
                }
            }
            for (std::size_t position = 0; position < model.softReferences.size(); ++position) {
                if (cancellation.IsCancellationRequested())
                    return WorldIndexReject<void>(WorldArtifactErrors::Cancelled);
                const auto offset = layout.offsets[5] + position * 64;
                WriteDependencyEndpoint(bytes, offset, model.softReferences[position].source);
                WriteDependencyEndpoint(bytes, offset + 32, model.softReferences[position].target);
            }
            return Result<void>::Success();
        }

        /** @brief Encodes every index cell and its complete derived hard-target slice, including zero-count rows. */
        Result<void> WriteCellEdges(std::vector<std::byte> &bytes, const DecodedWorldDependencies &model, const DependencyLayout &layout,
                                    const CancellationToken &cancellation) {
            std::size_t targets{};
            for (std::size_t position = 0; position < model.cells.size(); ++position) {
                const auto &cell = model.cells[position];
                const auto offset = layout.offsets[6] + position * 24;
                WriteDependencyCell(bytes, offset, cell.cell);
                WorldIndexWrite(bytes, offset + 16, static_cast<std::uint32_t>(targets));
                WorldIndexWrite(bytes, offset + 20, static_cast<std::uint32_t>(cell.hardTargets.size()));
                auto written = WriteCells(bytes, layout.offsets[7], targets, cell.hardTargets, cancellation);
                if (written.HasError())
                    return Result<void>::Failure(written.ErrorValue());
                targets = written.Value();
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc EncodeWorldDependencyArtifact */
    Result<std::vector<std::byte>> EncodeWorldDependencyArtifact(const DecodedWorldIndex &index, const WorldSpatialAssignment &assignments,
                                                                 const WorldSpanningObjectPlan &placements,
                                                                 const WorldDependencyPlan &dependencies,
                                                                 const WorldDependencyArtifactLimits &limits,
                                                                 const CancellationToken &cancellation) {
        if (cancellation.IsCancellationRequested())
            return WorldIndexReject<std::vector<std::byte>>(WorldArtifactErrors::Cancelled);
        auto admitted = OwnerLayout(index, assignments, placements, dependencies, limits);
        if (admitted.HasError())
            return Result<std::vector<std::byte>>::Failure(admitted.ErrorValue());
        auto captured = CaptureWorldDependencyProjection(index, assignments, placements, dependencies, limits, cancellation);
        if (captured.HasError())
            return Result<std::vector<std::byte>>::Failure(captured.ErrorValue());
        auto model = std::move(captured).Value();
        auto counts = admitted.Value().counts;
        for (const auto &cell : model.cells)
            if (!AddCount(counts[7], cell.hardTargets.size()))
                return WorldIndexReject<std::vector<std::byte>>(WorldArtifactErrors::CapacityExceeded);
        auto checked = AdmitDependencyLayout(counts, limits);
        if (checked.HasError())
            return Result<std::vector<std::byte>>::Failure(checked.ErrorValue());
        const auto &layout = checked.Value();
        std::vector<std::byte> bytes(layout.total);
        WriteHeader(bytes, index, layout);
        if (auto objects = WriteObjects(bytes, model, layout, cancellation); objects.HasError())
            return Result<std::vector<std::byte>>::Failure(objects.ErrorValue());
        if (auto references = WriteReferences(bytes, model, layout, cancellation); references.HasError())
            return Result<std::vector<std::byte>>::Failure(references.ErrorValue());
        if (auto cells = WriteCellEdges(bytes, model, layout, cancellation); cells.HasError())
            return Result<std::vector<std::byte>>::Failure(cells.ErrorValue());
        auto digest = DependencyDigest(bytes, cancellation);
        if (digest.HasError())
            return Result<std::vector<std::byte>>::Failure(digest.ErrorValue());
        for (std::size_t position = 0; position < 32; ++position)
            bytes[DependencyHashOffset + position] = static_cast<std::byte>(digest.Value().bytes[position]);
        return Result<std::vector<std::byte>>::Success(std::move(bytes));
    }
}  // namespace Horo::WorldStreaming
