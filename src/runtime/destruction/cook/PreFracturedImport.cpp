#include "Horo/Destruction/PreFracturedImport.h"

#include "PreFracturedIntersection.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>

namespace Horo::Destruction::PreFracturedImportErrors {
    namespace {
        const ErrorDomainId kDomain{"horo.destruction"};
        constexpr auto kError = ErrorSeverity::Error;
    }  // namespace

    const ErrorCodeDescriptor InvalidSchema{kDomain, ErrorCode{"destruction.import.invalid_schema"}, kError,
                                            "Pre-fractured normalized source schema is unsupported.",
                                            "Re-import the source with a compatible Assets normalization schema."};
    const ErrorCodeDescriptor MissingChunk{kDomain, ErrorCode{"destruction.import.missing_chunk"}, kError,
                                           "A pre-fractured mesh has no canonical chunk token.",
                                           "Name each mesh HoroChunk_<nonzero decimal ID>, optionally followed by __label."};
    const ErrorCodeDescriptor DuplicateChunk{kDomain, ErrorCode{"destruction.import.duplicate_chunk"}, kError,
                                             "Two pre-fractured meshes claim one chunk ID.", "Give each chunk a distinct authored token."};
    const ErrorCodeDescriptor NonFinite{kDomain, ErrorCode{"destruction.import.non_finite"}, kError,
                                        "A pre-fractured mesh has a non-finite transform or vertex.",
                                        "Correct the source transform and positions."};
    const ErrorCodeDescriptor InvalidTopology{kDomain, ErrorCode{"destruction.import.invalid_topology"}, kError,
                                              "A pre-fractured mesh is open, degenerate, non-manifold, or inconsistently wound.",
                                              "Repair and triangulate the closed chunk surface."};
    const ErrorCodeDescriptor InvalidHierarchy{kDomain, ErrorCode{"destruction.import.invalid_hierarchy"}, kError,
                                               "Chunk hierarchy has an invalid parent, cycle, or excessive depth.",
                                               "Repair the source hierarchy or select an explicit higher limit."};
    const ErrorCodeDescriptor InvalidMaterial{kDomain, ErrorCode{"destruction.import.invalid_material"}, kError,
                                              "A pre-fractured triangle has no material assignment.",
                                              "Assign a named material to every source face."};
    const ErrorCodeDescriptor LimitExceeded{kDomain, ErrorCode{"destruction.import.limit_exceeded"}, kError,
                                            "Pre-fractured import exceeds a finite product or engine limit.",
                                            "Reduce chunks, geometry, or material data, or select an explicit supported profile."};
    const ErrorCodeDescriptor Cancelled{kDomain, ErrorCode{"destruction.import.cancelled"}, kError,
                                        "Pre-fractured preparation was cancelled.", "Retry while the owning operation remains active."};
    const ErrorCodeDescriptor StaleCandidate{kDomain, ErrorCode{"destruction.import.stale_candidate"}, kError,
                                             "Candidate was prepared for a replaced owner revision.",
                                             "Prepare again from the current owner revision."};
    const ErrorCodeDescriptor Shutdown{kDomain, ErrorCode{"destruction.import.shutdown"}, kError,
                                       "Pre-fractured owner has closed acceptance.", "Do not publish after shutdown."};
}  // namespace Horo::Destruction::PreFracturedImportErrors

namespace Horo::Destruction {
    namespace {
        [[nodiscard]] Error ImportError(const ErrorCodeDescriptor &code, const Assets::PreFracturedSource &source, std::string_view path,
                                        std::string message) {
            auto error = MakeError(code, message);
            error.diagnostics.push_back({DiagnosticCode{code.code.Value()}, DiagnosticSeverity::Error, std::move(message),
                                         SourceLocation{source.sourceName}, std::string{path}});
            return error;
        }

        [[nodiscard]] Result<DestructionChunkId> ParseChunkId(const std::string_view name) {
            constexpr std::string_view prefix = "HoroChunk_";
            if (!name.starts_with(prefix))
                return Result<DestructionChunkId>::Failure(MakeError(PreFracturedImportErrors::MissingChunk));
            const auto suffix = name.substr(prefix.size());
            const auto labelOffset = suffix.find("__");
            const auto token = suffix.substr(0, labelOffset);
            if (token.empty() || token.front() == '0' || (labelOffset != std::string_view::npos && labelOffset + 2 == suffix.size()))
                return Result<DestructionChunkId>::Failure(MakeError(PreFracturedImportErrors::MissingChunk));
            std::uint64_t value{};
            const auto [end, status] = std::from_chars(token.data(), token.data() + token.size(), value);
            if (status != std::errc{} || end != token.data() + token.size() ||
                (labelOffset == std::string_view::npos && token.size() != suffix.size()))
                return Result<DestructionChunkId>::Failure(MakeError(PreFracturedImportErrors::MissingChunk));
            return DestructionChunkId::Create(value);
        }

        [[nodiscard]] bool ValidLimits(const DestructionLimits &limits) {
            const std::array<std::uint64_t, 10> actual{
                limits.maximumChunksPerDestructible,  limits.maximumHierarchyDepth,      limits.maximumActiveChunkBodies,
                limits.maximumEventsPerTransition,    limits.maximumEventJournalEntries, limits.maximumCosmeticDebrisParticles,
                limits.maximumArtifactBytes,          limits.maximumTransitionBytes,     limits.maximumResidentBytes,
                limits.maximumWorkItemsPerTransition,
            };
            constexpr std::array<std::uint64_t, 10> hard{
                DestructionHardLimits::ChunksPerDestructible, DestructionHardLimits::HierarchyDepth,
                DestructionHardLimits::ActiveChunkBodies,     DestructionHardLimits::EventsPerTransition,
                DestructionHardLimits::EventJournalEntries,   DestructionHardLimits::CosmeticDebrisParticles,
                DestructionHardLimits::ArtifactBytes,         DestructionHardLimits::TransitionBytes,
                DestructionHardLimits::ResidentBytes,         DestructionHardLimits::WorkItemsPerTransition,
            };
            for (std::size_t index = 0; index < actual.size(); ++index) {
                if (actual[index] == 0 || actual[index] > hard[index])
                    return false;
            }
            return limits.maximumActiveChunkBodies <= limits.maximumChunksPerDestructible &&
                   limits.maximumEventsPerTransition <= limits.maximumEventJournalEntries &&
                   limits.maximumArtifactBytes <= limits.maximumTransitionBytes &&
                   limits.maximumTransitionBytes <= limits.maximumResidentBytes &&
                   limits.maximumChunksPerDestructible <= limits.maximumWorkItemsPerTransition;
        }

        [[nodiscard]] bool FiniteTransform(const std::array<double, 12> &matrix) {
            for (const double value : matrix) {
                if (!std::isfinite(value))
                    return false;
            }
            const double determinant = matrix[0] * (matrix[4] * matrix[8] - matrix[7] * matrix[5]) -
                                       matrix[3] * (matrix[1] * matrix[8] - matrix[7] * matrix[2]) +
                                       matrix[6] * (matrix[1] * matrix[5] - matrix[4] * matrix[2]);
            return std::isfinite(determinant) && std::abs(determinant) > 1.0e-18;
        }

        struct EdgeIncidence {
            std::uint32_t count{};
            int orientation{};
            std::size_t firstTriangle{};
        };

        [[nodiscard]] Result<void> ValidateTopology(const Assets::PreFracturedSource &source, const Assets::PreFracturedSourceNode &node,
                                                    const CancellationToken &cancellation) {
            if (node.positions.size() < 4 || node.triangleIndices.empty() || node.triangleIndices.size() % 3 != 0)
                return Result<void>::Failure(ImportError(PreFracturedImportErrors::InvalidTopology, source, node.sourcePath,
                                                         "Chunk has no closed triangle surface."));
            if (node.triangleMaterials.size() != node.triangleIndices.size() / 3U)
                return Result<void>::Failure(ImportError(PreFracturedImportErrors::InvalidMaterial, source, node.sourcePath,
                                                         "Triangle and material counts differ."));
            std::map<std::pair<std::uint32_t, std::uint32_t>, EdgeIncidence> edges;
            std::vector<std::size_t> parents(node.triangleIndices.size() / 3U);
            std::iota(parents.begin(), parents.end(), 0U);
            const auto root = [&parents](std::size_t triangle) {
                while (parents[triangle] != triangle)
                    triangle = parents[triangle];
                return triangle;
            };
            for (std::size_t triangle = 0; triangle < node.triangleIndices.size(); triangle += 3U) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(ImportError(PreFracturedImportErrors::Cancelled, source, node.sourcePath,
                                                             "Pre-fractured preparation was cancelled."));
                const auto a = node.triangleIndices[triangle];
                const auto b = node.triangleIndices[triangle + 1U];
                const auto c = node.triangleIndices[triangle + 2U];
                if (a >= node.positions.size() || b >= node.positions.size() || c >= node.positions.size() || a == b || b == c || c == a)
                    return Result<void>::Failure(ImportError(PreFracturedImportErrors::InvalidTopology, source, node.sourcePath,
                                                             "Triangle references an invalid or repeated vertex."));
                const auto &pa = node.positions[a];
                const auto &pb = node.positions[b];
                const auto &pc = node.positions[c];
                const double ux = static_cast<double>(pb[0]) - pa[0];
                const double uy = static_cast<double>(pb[1]) - pa[1];
                const double uz = static_cast<double>(pb[2]) - pa[2];
                const double vx = static_cast<double>(pc[0]) - pa[0];
                const double vy = static_cast<double>(pc[1]) - pa[1];
                const double vz = static_cast<double>(pc[2]) - pa[2];
                const double cx = uy * vz - uz * vy;
                const double cy = uz * vx - ux * vz;
                const double cz = ux * vy - uy * vx;
                if (!std::isfinite(cx * cx + cy * cy + cz * cz) || cx * cx + cy * cy + cz * cz <= 1.0e-24)
                    return Result<void>::Failure(ImportError(PreFracturedImportErrors::InvalidTopology, source, node.sourcePath,
                                                             "Chunk contains a zero-area or invalid triangle."));
                if (node.triangleMaterials[triangle / 3U].empty())
                    return Result<void>::Failure(ImportError(PreFracturedImportErrors::InvalidMaterial, source, node.sourcePath,
                                                             "Chunk triangle has no named material."));
                for (const auto [from, to] : {std::pair{a, b}, std::pair{b, c}, std::pair{c, a}}) {
                    auto &edge = edges[std::minmax(from, to)];
                    ++edge.count;
                    edge.orientation += from < to ? 1 : -1;
                    if (edge.count == 1)
                        edge.firstTriangle = triangle / 3U;
                    else if (edge.count == 2)
                        parents[root(triangle / 3U)] = root(edge.firstTriangle);
                    if (edge.count > 2)
                        return Result<void>::Failure(ImportError(PreFracturedImportErrors::InvalidTopology, source, node.sourcePath,
                                                                 "Chunk has a non-manifold edge."));
                }
            }
            for (const auto &[edge, incidence] : edges) {
                (void)edge;
                if (incidence.count != 2 || incidence.orientation != 0)
                    return Result<void>::Failure(ImportError(PreFracturedImportErrors::InvalidTopology, source, node.sourcePath,
                                                             "Chunk has an open or inconsistently wound edge."));
            }
            const auto firstRoot = root(0);
            for (std::size_t triangle = 1; triangle < parents.size(); ++triangle) {
                if (root(triangle) != firstRoot)
                    return Result<void>::Failure(ImportError(PreFracturedImportErrors::InvalidTopology, source, node.sourcePath,
                                                             "Chunk has disconnected closed surface components."));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ValidatePreFracturedSource */
    Result<PreFracturedCandidate> ValidatePreFracturedSource(const Assets::PreFracturedSource &source, const DestructionLimits &limits,
                                                             const CancellationToken &cancellation) {
        if (source.schemaVersion != Assets::CurrentPreFracturedSourceSchemaVersion)
            return Result<PreFracturedCandidate>::Failure(
                ImportError(PreFracturedImportErrors::InvalidSchema, source, {}, "Normalized pre-fractured source schema is unsupported."));
        if (!ValidLimits(limits) || source.nodes.empty())
            return Result<PreFracturedCandidate>::Failure(ImportError(PreFracturedImportErrors::LimitExceeded, source, {},
                                                                      "Import limits are invalid or source has no mesh chunks."));
        if (source.nodes.size() > limits.maximumChunksPerDestructible)
            return Result<PreFracturedCandidate>::Failure(ImportError(PreFracturedImportErrors::LimitExceeded, source,
                                                                      source.nodes[limits.maximumChunksPerDestructible].sourcePath,
                                                                      "Chunk count exceeds the selected profile."));
        PreFracturedCandidate candidate;
        candidate.sourceName_ = source.sourceName;
        candidate.estimatedBytes_ = sizeof(PreFracturedCandidate) + candidate.sourceName_.size();
        if (candidate.estimatedBytes_ > limits.maximumArtifactBytes)
            return Result<PreFracturedCandidate>::Failure(
                ImportError(PreFracturedImportErrors::LimitExceeded, source, {}, "Candidate source label exceeds byte budget."));
        candidate.chunks_.reserve(source.nodes.size());
        std::vector<DestructionChunkId> identities;
        identities.reserve(source.nodes.size());
        std::map<std::uint64_t, std::string> seen;
        for (const auto &node : source.nodes) {
            if (cancellation.IsCancellationRequested())
                return Result<PreFracturedCandidate>::Failure(
                    ImportError(PreFracturedImportErrors::Cancelled, source, node.sourcePath, "Preparation was cancelled."));
            const auto id = ParseChunkId(node.name);
            if (id.HasError())
                return Result<PreFracturedCandidate>::Failure(
                    ImportError(PreFracturedImportErrors::MissingChunk, source, node.sourcePath,
                                "Mesh node '" + node.name + "' lacks a canonical HoroChunk_<id> token."));
            if (const auto [it, inserted] = seen.emplace(id.Value().Value(), node.sourcePath); !inserted)
                return Result<PreFracturedCandidate>::Failure(ImportError(PreFracturedImportErrors::DuplicateChunk, source, node.sourcePath,
                                                                          "Duplicate chunk ID also appears at " + it->second + '.'));
            identities.push_back(id.Value());
        }
        std::uint64_t workItems{};
        for (std::size_t index = 0; index < source.nodes.size(); ++index) {
            const auto &node = source.nodes[index];
            if (!FiniteTransform(node.geometryToWorld))
                return Result<PreFracturedCandidate>::Failure(ImportError(PreFracturedImportErrors::NonFinite, source, node.sourcePath,
                                                                          "Chunk transform is non-finite or singular."));
            for (const auto &position : node.positions) {
                if (!std::isfinite(position[0]) || !std::isfinite(position[1]) || !std::isfinite(position[2]))
                    return Result<PreFracturedCandidate>::Failure(
                        ImportError(PreFracturedImportErrors::NonFinite, source, node.sourcePath, "Chunk has a non-finite position."));
            }
            std::uint32_t depth = 1;
            std::optional<std::uint32_t> ancestor = node.parent;
            while (ancestor) {
                if (*ancestor >= source.nodes.size() || ++depth > limits.maximumHierarchyDepth)
                    return Result<PreFracturedCandidate>::Failure(ImportError(PreFracturedImportErrors::InvalidHierarchy, source,
                                                                              node.sourcePath,
                                                                              "Chunk parent chain is invalid or too deep."));
                ancestor = source.nodes[*ancestor].parent;
            }
            const std::uint64_t nodeWork = static_cast<std::uint64_t>(node.positions.size()) + node.triangleIndices.size();
            if (nodeWork > limits.maximumWorkItemsPerTransition - workItems)
                return Result<PreFracturedCandidate>::Failure(
                    ImportError(PreFracturedImportErrors::LimitExceeded, source, node.sourcePath, "Chunk geometry exceeds work budget."));
            workItems += nodeWork;
            std::uint64_t bytes = sizeof(PreFracturedChunk) + node.sourcePath.size() +
                                  node.positions.size() * sizeof(std::array<float, 3>) +
                                  node.triangleIndices.size() * sizeof(std::uint32_t) + node.triangleMaterials.size() * sizeof(std::string);
            for (const auto &material : node.triangleMaterials)
                bytes += material.size();
            if (bytes > limits.maximumArtifactBytes - candidate.estimatedBytes_ ||
                bytes > limits.maximumTransitionBytes - candidate.estimatedBytes_)
                return Result<PreFracturedCandidate>::Failure(
                    ImportError(PreFracturedImportErrors::LimitExceeded, source, node.sourcePath, "Candidate exceeds byte budget."));
            candidate.estimatedBytes_ += bytes;
            if (auto topology = ValidateTopology(source, node, cancellation); topology.HasError())
                return Result<PreFracturedCandidate>::Failure(topology.ErrorValue());
            std::uint64_t remainingWork = limits.maximumWorkItemsPerTransition - workItems;
            switch (Detail::CheckSelfIntersection(node, remainingWork, cancellation)) {
                case Detail::IntersectionCheck::Intersecting:
                    return Result<PreFracturedCandidate>::Failure(ImportError(PreFracturedImportErrors::InvalidTopology, source,
                                                                              node.sourcePath, "Chunk surface intersects itself."));
                case Detail::IntersectionCheck::TooMuchWork:
                    return Result<PreFracturedCandidate>::Failure(ImportError(PreFracturedImportErrors::LimitExceeded, source,
                                                                              node.sourcePath, "Topology check exceeds work budget."));
                case Detail::IntersectionCheck::Cancelled:
                    return Result<PreFracturedCandidate>::Failure(
                        ImportError(PreFracturedImportErrors::Cancelled, source, node.sourcePath, "Preparation was cancelled."));
                case Detail::IntersectionCheck::Clear:
                    break;
            }
            workItems = limits.maximumWorkItemsPerTransition - remainingWork;
            PreFracturedChunk chunk{.id = identities[index],
                                    .parent = node.parent ? identities[*node.parent] : DestructionChunkId{},
                                    .sourcePath = node.sourcePath,
                                    .geometryToWorld = node.geometryToWorld,
                                    .positions = node.positions,
                                    .triangleIndices = node.triangleIndices,
                                    .triangleMaterials = node.triangleMaterials};
            candidate.chunks_.push_back(std::move(chunk));
        }
        std::sort(candidate.chunks_.begin(), candidate.chunks_.end(), [](const PreFracturedChunk &a, const PreFracturedChunk &b) {
            return a.id.Value() < b.id.Value();
        });
        return Result<PreFracturedCandidate>::Success(std::move(candidate));
    }

    /** @copydoc PreparePreFracturedFbx */
    Result<PreFracturedCandidate> PreparePreFracturedFbx(const std::span<const std::uint8_t> bytes, const std::string_view sourceName,
                                                         const DestructionLimits &limits, const CancellationToken &cancellation) {
        auto parsed = Assets::ParsePreFracturedFbx(bytes, sourceName, cancellation);
        if (parsed.HasError())
            return Result<PreFracturedCandidate>::Failure(parsed.ErrorValue());
        return ValidatePreFracturedSource(parsed.Value(), limits, cancellation);
    }

    /** @copydoc PreFracturedImportOwner::PreFracturedImportOwner */
    PreFracturedImportOwner::PreFracturedImportOwner() : revision_(PreFracturedOwnerRevision::Create(1).Value()) {}

    /** @copydoc PreFracturedImportOwner::Accept */
    Result<void> PreFracturedImportOwner::Accept(PreFracturedCandidate candidate, const PreFracturedOwnerRevision expectedRevision) {
        if (shutdown_)
            return Result<void>::Failure(MakeError(PreFracturedImportErrors::Shutdown));
        if (expectedRevision != revision_)
            return Result<void>::Failure(MakeError(PreFracturedImportErrors::StaleCandidate));
        if (revision_.Value() == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(DestructionErrors::RevisionExhausted));
        CancellationSource nextCancellation;
        auto next = std::make_shared<const PreFracturedCandidate>(std::move(candidate));
        cancellation_.RequestCancellation();
        current_ = std::move(next);
        cancellation_ = std::move(nextCancellation);
        revision_ = PreFracturedOwnerRevision::Create(revision_.Value() + 1U).Value();
        return Result<void>::Success();
    }

    /** @copydoc PreFracturedImportOwner::Invalidate */
    Result<void> PreFracturedImportOwner::Invalidate() {
        if (shutdown_)
            return Result<void>::Failure(MakeError(PreFracturedImportErrors::Shutdown));
        if (revision_.Value() == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(DestructionErrors::RevisionExhausted));
        CancellationSource nextCancellation;
        cancellation_.RequestCancellation();
        cancellation_ = std::move(nextCancellation);
        revision_ = PreFracturedOwnerRevision::Create(revision_.Value() + 1U).Value();
        return Result<void>::Success();
    }

    /** @copydoc PreFracturedImportOwner::Shutdown */
    void PreFracturedImportOwner::Shutdown() noexcept {
        shutdown_ = true;
        cancellation_.RequestCancellation();
    }
}  // namespace Horo::Destruction
