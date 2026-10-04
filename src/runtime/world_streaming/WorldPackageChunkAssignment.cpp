#include "Horo/WorldStreaming/WorldPackageChunkAssignment.h"

#include "WorldStreamingInternal.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace Horo::WorldStreaming {
    namespace {
        /** @brief Checks the complete host-owned release publication fence. */
        [[nodiscard]] bool IsValid(const WorldPackageAssignmentBinding &binding) noexcept {
            return binding.owner.IsValid() && binding.revision.IsValid() && binding.epoch.IsValid();
        }

        /** @brief Finds a canonical cell without consulting mutable runtime state. */
        [[nodiscard]] std::size_t CellIndex(const CookedWorldIndexManifest &manifest, const StreamingCellId cell) {
            const auto cells = manifest.Cells();
            return static_cast<std::size_t>(
                std::ranges::lower_bound(cells, cell, StreamingCellCanonicalLess{}, &CookedWorldCellManifestEntry::cell) - cells.begin());
        }

        /** @brief Appends each hard-cell dependency once, including cyclic co-load components. */
        void GatherCells(const CookedWorldIndexManifest &manifest, const std::size_t root, std::vector<std::uint8_t> &visited,
                         std::vector<std::size_t> &pending) {
            pending.clear();
            std::ranges::fill(visited, std::uint8_t{});
            visited[root] = true;
            pending.push_back(root);
            std::size_t cursor{};
            while (cursor < pending.size()) {
                for (const auto dependency : manifest.HardDependencies(pending[cursor++])) {
                    const auto index = CellIndex(manifest, dependency);
                    if (!visited[index]) {
                        visited[index] = true;
                        pending.push_back(index);
                    }
                }
            }
        }

        /** @brief Expands chunk prerequisites once; the validated Assets graph is acyclic and complete. */
        void GatherChunks(const std::span<const Assets::AssetChunkDefinition> chunks, std::vector<std::uint8_t> &required,
                          std::vector<std::size_t> &pending) {
            std::size_t cursor{};
            while (cursor < pending.size()) {
                for (const auto &dependency : chunks[pending[cursor++]].dependencies) {
                    const auto index = static_cast<std::size_t>(
                        std::ranges::lower_bound(chunks, dependency, {}, &Assets::AssetChunkDefinition::id) - chunks.begin());
                    if (!required[index]) {
                        required[index] = true;
                        pending.push_back(index);
                    }
                }
            }
        }

        /** @brief Hashes fixed-width canonical integers without native padding or endianness. */
        void AppendInteger(Sha256Builder &builder, const std::uint64_t value) noexcept {
            std::array<std::byte, 8> bytes{};
            for (std::size_t index{}; index < bytes.size(); ++index)
                bytes[index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
            static_cast<void>(builder.Update(bytes));
        }

        /** @brief Binds exact cell artifact integrity/size and direct dependency facts without retaining manifest storage. */
        [[nodiscard]] Sha256Digest MetadataHash(const CookedWorldIndexManifest &manifest, const std::size_t index) noexcept {
            Sha256Builder builder;
            const auto &entry = manifest.Cells()[index];
            const auto &artifact = manifest.Descriptor().Cells()[index].package.chunkAsset;
            static_cast<void>(builder.Update(std::as_bytes(std::span{artifact.Bytes()})));
            static_cast<void>(builder.Update(std::as_bytes(std::span{entry.artifactHash.bytes})));
            AppendInteger(builder, entry.uncompressedSize);
            AppendInteger(builder, entry.compressedSize);
            AppendInteger(builder, entry.payloadCrc32);
            const auto dependencies = manifest.HardDependencies(index);
            AppendInteger(builder, dependencies.size());
            for (const auto dependency : dependencies) {
                AppendInteger(builder, static_cast<std::uint32_t>(dependency.x));
                AppendInteger(builder, static_cast<std::uint32_t>(dependency.y));
                AppendInteger(builder, static_cast<std::uint32_t>(dependency.z));
                AppendInteger(builder, dependency.lod);
                AppendInteger(builder, dependency.layer.Value());
            }
            return builder.Finalize();
        }

        struct Membership final {
            Assets::AssetId asset{};
            std::size_t chunk{};
        };

        /** @brief Builds a bounded sorted membership index from the existing Assets authority. */
        [[nodiscard]] Result<std::vector<Membership>> Memberships(const Assets::AssetChunkPlan &plan, const std::size_t maximumAssets) {
            std::size_t count{};
            for (const auto &chunk : plan.Chunks()) {
                if (chunk.assets.size() > maximumAssets - count)
                    return Internal::Failure<std::vector<Membership>>(WorldStreamingErrors::PackageChunkCapacityExceeded);
                count += chunk.assets.size();
            }
            std::vector<Membership> result;
            result.reserve(count);
            for (std::size_t index{}; index < plan.Chunks().size(); ++index)
                for (const auto asset : plan.Chunks()[index].assets)
                    result.emplace_back(asset, index);
            std::ranges::sort(result, {}, &Membership::asset);
            return Result<std::vector<Membership>>::Success(std::move(result));
        }

        /** @brief Rejects malformed admission ceilings and binding before construction storage is allocated. */
        [[nodiscard]] Result<void> ValidateAssignmentInputs(const CookedWorldIndexManifest &manifest, const Assets::AssetChunkPlan &plan,
                                                            const WorldPackageAssignmentBinding binding,
                                                            const WorldPackageChunkLimits &limits) {
            if (!IsValid(binding) || limits.maximumCells == 0 || limits.maximumChunks == 0 || limits.maximumReleaseAssets == 0 ||
                limits.maximumTotalRequirements == 0 || manifest.Cells().empty() || plan.Chunks().empty())
                return Internal::Failure<void>(WorldStreamingErrors::PackageChunkInvalid);
            if (manifest.Cells().size() > limits.maximumCells || plan.Chunks().size() > limits.maximumChunks)
                return Internal::Failure<void>(WorldStreamingErrors::PackageChunkCapacityExceeded);
            return Result<void>::Success();
        }

        /** @brief Resolves every declared cell artifact against the sole Assets membership index. */
        [[nodiscard]] Result<std::vector<std::size_t>> ResolveCellChunks(const CookedWorldIndexManifest &manifest,
                                                                         const std::span<const Membership> membership) {
            std::vector<std::size_t> cellChunks;
            cellChunks.reserve(manifest.Cells().size());
            for (const auto &cell : manifest.Descriptor().Cells()) {
                const auto index = membership;
                const auto found = std::ranges::lower_bound(index, cell.package.chunkAsset, {}, &Membership::asset);
                if (found == index.end() || found->asset != cell.package.chunkAsset)
                    return Internal::Failure<std::vector<std::size_t>>(WorldStreamingErrors::PackageChunkUnassigned);
                cellChunks.push_back(found->chunk);
            }
            return Result<std::vector<std::size_t>>::Success(std::move(cellChunks));
        }

        /** @brief Complete private storage published only after every cell closure satisfies its ceiling. */
        struct OwnedRequirements final {
            std::vector<WorldCellChunkAssignment> cells;
            std::vector<Assets::AssetChunkId> requirements;
        };

        /** @brief Materializes canonical transitive requirements from validated membership and dependency authorities. */
        [[nodiscard]] Result<OwnedRequirements> BuildRequirements(const CookedWorldIndexManifest &manifest,
                                                                  const std::span<const Assets::AssetChunkDefinition> chunks,
                                                                  const std::span<const std::size_t> cellChunks,
                                                                  const std::size_t maximumRequirements) {
            const auto base = static_cast<std::size_t>(
                std::ranges::find(chunks, Assets::AssetChunkKind::Base, &Assets::AssetChunkDefinition::kind) - chunks.begin());
            OwnedRequirements owned;
            auto &cells = owned.cells;
            auto &requirements = owned.requirements;
            std::vector<std::uint8_t> visited(manifest.Cells().size());
            std::vector<std::uint8_t> required(chunks.size());
            std::vector<std::size_t> pendingCells;
            std::vector<std::size_t> pendingChunks;
            for (std::size_t root{}; root < manifest.Cells().size(); ++root) {
                GatherCells(manifest, root, visited, pendingCells);
                std::ranges::fill(required, std::uint8_t{});
                pendingChunks.clear();
                required[base] = true;
                pendingChunks.push_back(base);
                for (const auto index : pendingCells) {
                    const auto chunk = cellChunks[index];
                    if (!required[chunk]) {
                        required[chunk] = true;
                        pendingChunks.push_back(chunk);
                    }
                }
                GatherChunks(chunks, required, pendingChunks);
                if (pendingChunks.size() > maximumRequirements - requirements.size())
                    return Internal::Failure<OwnedRequirements>(WorldStreamingErrors::PackageChunkCapacityExceeded);
                const auto offset = requirements.size();
                for (std::size_t index{}; index < chunks.size(); ++index)
                    if (required[index])
                        requirements.push_back(chunks[index].id);
                cells.emplace_back(manifest.Cells()[root].cell, manifest.Descriptor().Cells()[root].package.chunkAsset,
                                   manifest.Cells()[root].artifactHash, MetadataHash(manifest, root), offset, requirements.size() - offset);
            }
            return Result<OwnedRequirements>::Success(std::move(owned));
        }

        /** @brief Compares the exact release and availability publication evidence without ambient state. */
        [[nodiscard]] bool MatchesPublication(const WorldPackageChunkAssignment &assignment,
                                              const WorldPackageAvailabilitySnapshot &snapshot, const WorldPackageContentContext &context) {
            return assignment.Binding() == context.assignment && snapshot.Binding() == context.assignment &&
                   snapshot.Revision() == context.availability && snapshot.Partition() == assignment.Partition() &&
                   std::ranges::equal(assignment.Chunks(), snapshot.Chunks(), {}, {}, &WorldPackageChunkAvailability::chunk);
        }

        /** @brief Validates current content admission separately from resolving one cell's missing chunk rows. */
        [[nodiscard]] Result<void> ValidateContentAdmission(const WorldPackageChunkAssignment &assignment,
                                                            const WorldPackageAvailabilitySnapshot &snapshot, const StreamingCellId cell,
                                                            const WorldPackageContentContext &context) {
            if (context.lifecycle >= WorldPackageContentLifecycle::Count)
                return Internal::Failure<void>(WorldStreamingErrors::PackageChunkUnsupported);
            if (!IsValid(context.assignment) || !context.availability.IsValid() || !cell.IsValid())
                return Internal::Failure<void>(WorldStreamingErrors::PackageChunkInvalid);
            if (context.lifecycle != WorldPackageContentLifecycle::Active)
                return Internal::Failure<void>(WorldStreamingErrors::PackageChunkLifecycleUnavailable);
            if (!MatchesPublication(assignment, snapshot, context))
                return Internal::Failure<void>(WorldStreamingErrors::PackageChunkStale);
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc WorldPackageChunkAssignment::WorldPackageChunkAssignment */
    WorldPackageChunkAssignment::WorldPackageChunkAssignment(WorldPartitionId partition, WorldPackageAssignmentBinding binding,
                                                             std::vector<Assets::AssetChunkId> chunks,
                                                             std::vector<WorldCellChunkAssignment> cells,
                                                             std::vector<Assets::AssetChunkId> requirements) noexcept
        : partition_(partition), binding_(binding), chunks_(std::move(chunks)), cells_(std::move(cells)),
          requirements_(std::move(requirements)) {}

    /** @copydoc WorldPackageChunkAssignment::Create */
    Result<WorldPackageChunkAssignment> WorldPackageChunkAssignment::Create(const CookedWorldIndexManifest &manifest,
                                                                            const Assets::AssetChunkPlan &plan,
                                                                            const WorldPackageAssignmentBinding binding,
                                                                            const Sha256Digest &baseManifest,
                                                                            const WorldPackageChunkLimits &limits) {
        using Return = Result<WorldPackageChunkAssignment>;
        if (const auto valid = ValidateAssignmentInputs(manifest, plan, binding, limits); valid.HasError())
            return Return::Failure(valid.ErrorValue());
        auto membership = Memberships(plan, limits.maximumReleaseAssets);
        if (membership.HasError())
            return Return::Failure(membership.ErrorValue());
        std::vector<Assets::AssetChunkId> chunkIds;
        for (const auto &chunk : plan.Chunks())
            chunkIds.push_back(chunk.id);
        if (const auto mount = Assets::ResolveAssetChunkMountOrder(plan, chunkIds, baseManifest); mount.HasError())
            return Return::Failure(mount.ErrorValue());
        auto cellChunks = ResolveCellChunks(manifest, membership.Value());
        if (cellChunks.HasError())
            return Return::Failure(cellChunks.ErrorValue());
        auto built = BuildRequirements(manifest, plan.Chunks(), cellChunks.Value(), limits.maximumTotalRequirements);
        if (built.HasError())
            return Return::Failure(built.ErrorValue());
        auto owned = std::move(built).Value();
        return Return::Success(WorldPackageChunkAssignment{manifest.Descriptor().Partition(), binding, std::move(chunkIds),
                                                           std::move(owned.cells), std::move(owned.requirements)});
    }

    /** @copydoc WorldPackageChunkAssignment::ValidateCell */
    Result<void> WorldPackageChunkAssignment::ValidateCell(const CookedWorldIndexManifest &manifest, const StreamingCellId cell) const {
        if (manifest.Descriptor().Partition() != partition_)
            return Internal::Failure<void>(WorldStreamingErrors::PackageChunkStale);
        std::vector<StreamingCellId> pending{cell};
        std::size_t cursor{};
        while (cursor < pending.size()) {
            const auto requested = pending[cursor++];
            const auto index = CellIndex(manifest, requested);
            if (const auto found =
                    std::ranges::lower_bound(cells_, requested, StreamingCellCanonicalLess{}, &WorldCellChunkAssignment::cell);
                index == manifest.Cells().size() || manifest.Cells()[index].cell != requested || found == cells_.end() ||
                found->cell != requested || found->metadataHash != MetadataHash(manifest, index))
                return Internal::Failure<void>(WorldStreamingErrors::PackageChunkStale);
            for (const auto dependency : manifest.HardDependencies(index))
                if (std::ranges::find(pending, dependency) == pending.end())
                    pending.push_back(dependency);
        }
        return Result<void>::Success();
    }

    /** @copydoc WorldPackageChunkAssignment::Requirements */
    std::span<const Assets::AssetChunkId> WorldPackageChunkAssignment::Requirements(const std::size_t index) const noexcept {
        if (index >= cells_.size())
            return {};
        const auto &cell = cells_[index];
        return std::span<const Assets::AssetChunkId>{requirements_}.subspan(cell.requirementOffset, cell.requirementCount);
    }

    /** @copydoc WorldPackageAvailabilitySnapshot::WorldPackageAvailabilitySnapshot */
    WorldPackageAvailabilitySnapshot::WorldPackageAvailabilitySnapshot(WorldPartitionId partition, WorldPackageAssignmentBinding binding,
                                                                       WorldPackageAvailabilityRevision revision,
                                                                       std::vector<WorldPackageChunkAvailability> chunks) noexcept
        : partition_(partition), binding_(binding), revision_(revision), chunks_(std::move(chunks)) {}

    /** @copydoc WorldPackageAvailabilitySnapshot::Create */
    Result<WorldPackageAvailabilitySnapshot> WorldPackageAvailabilitySnapshot::Create(
        const WorldPackageChunkAssignment &assignment, const WorldPackageAvailabilityRevision revision,
        const std::span<const WorldPackageChunkAvailability> chunks) {
        if (!revision.IsValid() || assignment.Chunks().empty() || chunks.size() != assignment.Chunks().size())
            return Internal::Failure<WorldPackageAvailabilitySnapshot>(WorldStreamingErrors::PackageChunkInvalid);
        if (std::ranges::any_of(chunks, [](const auto &chunk) {
            return chunk.state >= WorldPackageChunkState::Count;
        }))
            return Internal::Failure<WorldPackageAvailabilitySnapshot>(WorldStreamingErrors::PackageChunkUnsupported);
        std::vector<WorldPackageChunkAvailability> owned(chunks.begin(), chunks.end());
        std::ranges::sort(owned, {}, &WorldPackageChunkAvailability::chunk);
        for (std::size_t index{}; index < owned.size(); ++index)
            if (owned[index].chunk != assignment.Chunks()[index])
                return Internal::Failure<WorldPackageAvailabilitySnapshot>(WorldStreamingErrors::PackageChunkInvalid);
        return Result<WorldPackageAvailabilitySnapshot>::Success(
            WorldPackageAvailabilitySnapshot{assignment.Partition(), assignment.Binding(), revision, std::move(owned)});
    }

    /** @copydoc EvaluateWorldCellContent */
    Result<WorldCellContentAvailability> EvaluateWorldCellContent(const WorldPackageChunkAssignment &assignment,
                                                                  const WorldPackageAvailabilitySnapshot &snapshot,
                                                                  const StreamingCellId cell, const WorldPackageContentContext &context) {
        if (const auto valid = ValidateContentAdmission(assignment, snapshot, cell, context); valid.HasError())
            return Result<WorldCellContentAvailability>::Failure(valid.ErrorValue());
        const auto cells = assignment.Cells();
        const auto found = std::ranges::lower_bound(cells, cell, StreamingCellCanonicalLess{}, &WorldCellChunkAssignment::cell);
        if (found == cells.end() || found->cell != cell)
            return Internal::Failure<WorldCellContentAvailability>(WorldStreamingErrors::PackageChunkUnassigned);
        WorldCellContentAvailability result{context.assignment, context.availability, cell, {}};
        for (const auto &chunk : assignment.Requirements(static_cast<std::size_t>(found - cells.begin()))) {
            const auto state = std::ranges::lower_bound(snapshot.Chunks(), chunk, {}, &WorldPackageChunkAvailability::chunk);
            if (state->state != WorldPackageChunkState::Installed)
                result.missing.push_back(*state);
        }
        return Result<WorldCellContentAvailability>::Success(std::move(result));
    }
}  // namespace Horo::WorldStreaming
