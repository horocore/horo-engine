#include "NavigationRegionClipInternal.h"

#include <algorithm>

namespace Horo::Application::NavigationBakeDetail {
    namespace {
        namespace Clip = RegionClip;
        namespace Errors = Navigation::NavigationErrors;

        /** @brief Three independently bounded buffers; pointer swaps never copy the fixed fragment tables. */
        struct Scratch final {
            std::array<Clip::Fragments, 3> buffers;
        };

        struct Counts final {
            std::uint64_t vertices{};
            std::uint64_t triangles{};
        };

        /** @brief Claims only previously unclaimed geometry, so overlapping includes form a disjoint union. */
        Result<void> IncludeRegions(const SourceRegions &regions, Clip::Fragments *&pending, Clip::Fragments *&next,
                                    Clip::Fragments &accepted, std::uint32_t maximumFragments, Clip::Work &work) {
            for (const auto &region : regions.values) {
                if (auto admitted = work.Spend(1); admitted.HasError())
                    return admitted;
                if (region.authored.mode != Runtime::NavigationRegionMode::Include)
                    continue;
                next->count = 0;
                for (std::size_t i = 0; i < pending->count; ++i)
                    if (auto split = Clip::Split(pending->values[i], region, &accepted, *next, maximumFragments, work); split.HasError())
                        return split;
                std::swap(pending, next);
                if (pending->count == 0)
                    break;
            }
            return Result<void>::Success();
        }

        /** @brief Subtracts exclusions after the complete include union, retaining no duplicate coplanar boundary faces. */
        Result<void> ExcludeRegions(const SourceRegions &regions, Clip::Fragments *&accepted, Clip::Fragments *&next,
                                    std::uint32_t maximumFragments, Clip::Work &work) {
            for (const auto &region : regions.values) {
                if (auto admitted = work.Spend(1); admitted.HasError())
                    return admitted;
                if (region.authored.mode != Runtime::NavigationRegionMode::Exclude)
                    continue;
                next->count = 0;
                for (std::size_t i = 0; i < accepted->count; ++i)
                    if (auto split = Clip::Split(accepted->values[i], region, nullptr, *next, maximumFragments, work); split.HasError())
                        return split;
                std::swap(accepted, next);
                if (accepted->count == 0)
                    break;
            }
            return Result<void>::Success();
        }

        /** @brief Selects one original triangle under exact affine region semantics without heap scratch growth. */
        Result<std::span<const Clip::Triangle>> Select(const Clip::Triangle &triangle, const SourceRegions &regions,
                                                       std::uint32_t maximumFragments, Scratch &scratch, Clip::Work &work) {
            for (auto &buffer : scratch.buffers)
                buffer.count = 0;
            auto *accepted = &scratch.buffers[0];
            auto *pending = &scratch.buffers[1];
            auto *next = &scratch.buffers[2];
            if (regions.hasIncludes) {
                pending->values[pending->count++] = triangle;
                if (auto included = IncludeRegions(regions, pending, next, *accepted, maximumFragments, work); included.HasError())
                    return Result<std::span<const Clip::Triangle>>::Failure(included.ErrorValue());
            } else {
                accepted->values[accepted->count++] = triangle;
            }
            if (regions.hasExcludes) {
                if (auto excluded = ExcludeRegions(regions, accepted, next, maximumFragments, work); excluded.HasError())
                    return Result<std::span<const Clip::Triangle>>::Failure(excluded.ErrorValue());
            }
            return Result<std::span<const Clip::Triangle>>::Success(std::span{accepted->values}.first(accepted->count));
        }

        /** @brief Counts or writes identical disjoint fragments, charging both passes to the shared project work budget. */
        Result<Counts> Pass(const NavigationPrimitiveGeometry &input, const SourceRegions &regions, const RegionSelectionPolicy &policy,
                            NavigationPrimitiveGeometry *output, Clip::Work &work) {
            Scratch scratch;
            Counts count;
            for (const auto &source : input.triangles) {
                if (auto admitted = work.Spend(1); admitted.HasError())
                    return Result<Counts>::Failure(admitted.ErrorValue());
                const Clip::Triangle triangle{input.vertices[source.vertexIndices[0]], input.vertices[source.vertexIndices[1]],
                                              input.vertices[source.vertexIndices[2]]};
                auto selected = Select(triangle, regions, policy.maximumFragments, scratch, work);
                if (selected.HasError())
                    return Result<Counts>::Failure(selected.ErrorValue());
                for (const auto &fragment : selected.Value()) {
                    count.vertices += 3;
                    ++count.triangles;
                    if (count.vertices > policy.geometry.storage.maxVertices || count.triangles > policy.geometry.storage.maxTriangles)
                        return Result<Counts>::Failure(
                            MakeError(Errors::CapacityExceeded, "Navigation region geometry exceeds source counts"));
                    if (!output)
                        continue;
                    const auto first = static_cast<std::uint32_t>(output->vertices.size());
                    output->vertices.insert(output->vertices.end(), fragment.begin(), fragment.end());
                    output->triangles.push_back({{first, first + 1, first + 2}, source.area, source.materialSlot});
                }
            }
            return Result<Counts>::Success(count);
        }

        /** @brief Retains exact region intent even when two authored sets happen to produce identical clipped geometry. */
        Sha256Digest ContributionDigest(const Sha256Digest &original, const Sha256Digest &regions, const Sha256Digest &selected) {
            constexpr std::string_view domain{"horo.navigation.region-contribution.v1"};
            const std::array<std::span<const std::byte>, 4> fragments{std::as_bytes(std::span{domain.data(), domain.size()}),
                                                                      std::as_bytes(std::span{original.bytes}),
                                                                      std::as_bytes(std::span{regions.bytes}),
                                                                      std::as_bytes(std::span{selected.bytes})};
            return ComputeSha256Fragments(fragments);
        }
    }  // namespace

    /** @copydoc SelectRegionGeometry */
    Result<RegionSelection> SelectRegionGeometry(NavigationPrimitiveGeometry input, const SourceRegions &regions,
                                                 const RegionSelectionPolicy &policy, std::uint64_t &remainingWork,
                                                 CancellationToken cancellation) {
        using Return = Result<RegionSelection>;
        if (regions.values.empty()) {
            const auto digest = input.digest;
            return Return::Success({std::move(input), digest});
        }
        Clip::Work work{remainingWork, cancellation};
        auto counted = Pass(input, regions, policy, nullptr, work);
        if (counted.HasError())
            return Return::Failure(counted.ErrorValue());
        if (const auto peakBytes = input.vertices.capacity() * sizeof(Math::Vec3) +
                                   input.triangles.capacity() * sizeof(Navigation::NavigationSourceTriangleInput) +
                                   counted.Value().vertices * sizeof(Math::Vec3) +
                                   counted.Value().triangles * sizeof(Navigation::NavigationSourceTriangleInput);
            peakBytes > policy.geometry.storage.maxOwnedBytes)
            return Return::Failure(MakeError(Errors::CapacityExceeded, "Navigation region selection exceeds peak geometry bytes"));
        NavigationPrimitiveGeometry output;
        output.vertices.reserve(static_cast<std::size_t>(counted.Value().vertices));
        output.triangles.reserve(static_cast<std::size_t>(counted.Value().triangles));
        if (auto written = Pass(input, regions, policy, &output, work); written.HasError())
            return Return::Failure(written.ErrorValue());
        auto finalized = FinalizeGeometry(std::move(output), {}, {}, policy.area, policy.material, cancellation);
        if (finalized.HasError())
            return Return::Failure(finalized.ErrorValue());
        const auto digest = ContributionDigest(input.digest, regions.digest, finalized.Value().digest);
        return Return::Success({std::move(finalized).Value(), digest});
    }
}  // namespace Horo::Application::NavigationBakeDetail
