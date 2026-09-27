#include "Horo/Destruction/StructuralGraphCook.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace Horo::Destruction::StructuralGraphErrors {
    namespace {
        const ErrorDomainId Domain{"horo.destruction"};
        constexpr auto Severity = ErrorSeverity::Error;
    }  // namespace

    const ErrorCodeDescriptor InvalidInput{Domain, ErrorCode{"destruction.graph.invalid_input"}, Severity,
                                           "Structural graph input or provenance is invalid.",
                                           "Recook against the exact chunk mesh and policy."};
    const ErrorCodeDescriptor InvalidIndex{Domain, ErrorCode{"destruction.graph.invalid_index"}, Severity,
                                           "A structural contact references an invalid chunk index.", "Repair the indexed contact table."};
    const ErrorCodeDescriptor UnstableOrder{Domain, ErrorCode{"destruction.graph.unstable_order"}, Severity,
                                            "Structural chunk or contact order is not canonical.",
                                            "Sort by stable chunk ID and contact index."};
    const ErrorCodeDescriptor HierarchyCycle{Domain, ErrorCode{"destruction.graph.hierarchy_cycle"}, Severity,
                                             "Structural parent hierarchy contains a cycle.", "Repair the authored parent chain."};
    const ErrorCodeDescriptor DisconnectedRequired{Domain, ErrorCode{"destruction.graph.disconnected_required"}, Severity,
                                                   "A required structural chunk has no path to an anchor.",
                                                   "Add an anchor or valid structural contact."};
    const ErrorCodeDescriptor LimitExceeded{Domain, ErrorCode{"destruction.graph.limit_exceeded"}, Severity,
                                            "Structural graph exceeds a finite chunk, byte, depth or work limit.",
                                            "Reduce topology or choose an explicitly supported limit."};
    const ErrorCodeDescriptor Unsupported{Domain, ErrorCode{"destruction.graph.unsupported"}, Severity,
                                          "The exact DFR tier does not provide required graph capabilities.",
                                          "Choose a tier that explicitly supports the required features."};
    const ErrorCodeDescriptor Cancelled{Domain, ErrorCode{"destruction.graph.cancelled"}, Severity,
                                        "Structural graph preparation was cancelled.", "Retry from the current generation."};
    const ErrorCodeDescriptor Stale{Domain, ErrorCode{"destruction.graph.stale"}, Severity, "Structural graph candidate is stale.",
                                    "Recook against the current mesh and policy."};
    const ErrorCodeDescriptor Shutdown{Domain, ErrorCode{"destruction.graph.shutdown"}, Severity,
                                       "Structural graph owner has closed admission.", "Do not publish after shutdown."};
}  // namespace Horo::Destruction::StructuralGraphErrors

namespace Horo::Destruction {
    namespace {
        using Output = Result<std::shared_ptr<const StructuralGraphArtifact>>;

        [[nodiscard]] bool Nonzero(const Sha256Digest &digest) {
            return std::ranges::any_of(digest.bytes, [](std::uint8_t byte) {
                return byte != 0;
            });
        }

        void HashByte(Sha256Builder &hash, std::uint8_t value) {
            const auto byte = static_cast<std::byte>(value);
            (void)hash.Update(std::span{&byte, 1});
        }

        void HashU64(Sha256Builder &hash, std::uint64_t value) {
            for (int shift = 56; shift >= 0; shift -= 8)
                HashByte(hash, static_cast<std::uint8_t>(value >> shift));
        }

        void HashDigest(Sha256Builder &hash, const Sha256Digest &digest) {
            for (const auto byte : digest.bytes)
                HashByte(hash, byte);
        }

        [[nodiscard]] Sha256Digest GraphDigest(const StructuralGraphArtifact &graph) {
            Sha256Builder hash;
            HashU64(hash, graph.schemaVersion);
            HashDigest(hash, graph.content.SemanticDigest());
            HashU64(hash, graph.content.Revision().Value());
            for (const auto byte : graph.content.Asset().Asset().Bytes())
                HashByte(hash, byte);
            HashDigest(hash, graph.meshIntegrityDigest);
            HashU64(hash, graph.ownerRevision.Value());
            HashU64(hash, graph.policyRevision.Value());
            HashU64(hash, static_cast<std::uint8_t>(graph.tier));
            HashU64(hash, graph.producedFeatures.bits);
            const auto &facts = graph.validation;
            HashU64(hash, facts.chunkCount);
            HashU64(hash, facts.contactCount);
            HashU64(hash, facts.islandCount);
            HashU64(hash, facts.anchorCount);
            HashU64(hash, facts.requiredCount);
            HashU64(hash, facts.maximumHierarchyDepth);
            HashU64(hash, facts.workItems);
            HashU64(hash, facts.estimatedBytes);
            for (const auto &chunk : graph.chunks) {
                HashU64(hash, chunk.id.Value());
                HashU64(hash, chunk.parent.Value());
                HashByte(hash, chunk.flags.anchor);
                HashByte(hash, chunk.flags.required);
                HashByte(hash, chunk.flags.initiallySupported);
                HashByte(hash, chunk.flags.hasParent);
                HashU64(hash, chunk.island);
                HashU64(hash, std::bit_cast<std::uint64_t>(chunk.supportWeight));
                HashU64(hash, chunk.adjacency.size());
                for (const auto neighbor : chunk.adjacency)
                    HashU64(hash, neighbor);
            }
            for (const auto &contact : graph.contacts) {
                HashU64(hash, contact.low);
                HashU64(hash, contact.high);
                HashU64(hash, std::bit_cast<std::uint64_t>(contact.weight));
            }
            return hash.Finalize();
        }

        [[nodiscard]] Error Context(const ErrorCodeDescriptor &code, std::string_view field, std::size_t index) {
            return MakeError(code, std::format("{}[{}]", field, index));
        }

        [[nodiscard]] bool ValidLimits(const DestructionLimits &limits, const DestructionLimits &tier) {
            return limits.maximumChunksPerDestructible > 0 && limits.maximumChunksPerDestructible <= tier.maximumChunksPerDestructible &&
                   limits.maximumHierarchyDepth > 0 && limits.maximumHierarchyDepth <= tier.maximumHierarchyDepth &&
                   limits.maximumArtifactBytes > 0 && limits.maximumArtifactBytes <= tier.maximumArtifactBytes &&
                   limits.maximumWorkItemsPerTransition > 0 && limits.maximumWorkItemsPerTransition <= tier.maximumWorkItemsPerTransition;
        }

        [[nodiscard]] Result<void> Charge(StructuralGraphValidation &facts, const DestructionLimits &limits, std::uint64_t work,
                                          std::uint64_t bytes = 0) {
            if (work > limits.maximumWorkItemsPerTransition - facts.workItems || bytes > limits.maximumArtifactBytes - facts.estimatedBytes)
                return Result<void>::Failure(MakeError(StructuralGraphErrors::LimitExceeded));
            facts.workItems += work;
            facts.estimatedBytes += bytes;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateRequest(const ChunkMeshArtifact &mesh, const StructuralGraphCookRequest &request,
                                                   const CancellationToken &cancellation) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(StructuralGraphErrors::Cancelled));
            if (!mesh.content.IsValid() || request.content != mesh.content || !Nonzero(mesh.integrityDigest) ||
                request.meshIntegrityDigest != mesh.integrityDigest || !request.ownerRevision.IsValid() ||
                !request.policyRevision.IsValid() || mesh.schemaVersion != ChunkMeshCookSchemaVersion || mesh.chunks.empty() ||
                request.chunks.size() != mesh.chunks.size())
                return Result<void>::Failure(MakeError(StructuralGraphErrors::Stale));
            const auto profile = GetDestructionTierProfile(mesh.tier);
            if (profile.HasError() || !request.requiredFeatures.IsValid() ||
                (request.requiredFeatures.bits & ~profile.Value().supportedFeatures.bits) != 0 ||
                !profile.Value().supportedFeatures.Contains(DestructionFeature::CookedSupport))
                return Result<void>::Failure(MakeError(StructuralGraphErrors::Unsupported));
            if (!ValidLimits(request.limits, profile.Value().limits) || mesh.chunks.size() > request.limits.maximumChunksPerDestructible ||
                mesh.chunks.size() * 2 - 1 > request.limits.maximumWorkItemsPerTransition ||
                request.contacts.size() > request.limits.maximumWorkItemsPerTransition)
                return Result<void>::Failure(MakeError(StructuralGraphErrors::LimitExceeded));
            for (std::size_t index = 1; index < request.chunks.size(); ++index) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(StructuralGraphErrors::Cancelled));
                if (request.chunks[index - 1].id >= request.chunks[index].id || mesh.chunks[index - 1].id >= mesh.chunks[index].id)
                    return Result<void>::Failure(Context(StructuralGraphErrors::UnstableOrder, "chunk", index));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> BuildChunks(StructuralGraphArtifact &graph, const ChunkMeshArtifact &mesh,
                                               const StructuralGraphCookRequest &request, const CancellationToken &cancellation) {
            if (auto charged = Charge(graph.validation, request.limits, mesh.chunks.size() * 2 - 1,
                                      sizeof(StructuralGraphArtifact) + mesh.chunks.size() * sizeof(StructuralGraphChunk));
                charged.HasError())
                return charged;
            graph.chunks.reserve(mesh.chunks.size());
            for (std::size_t index = 0; index < mesh.chunks.size(); ++index) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(StructuralGraphErrors::Cancelled));
                const auto &input = request.chunks[index];
                if (!input.id.IsValid() || input.id != mesh.chunks[index].id || input.parent == input.id)
                    return Result<void>::Failure(Context(StructuralGraphErrors::InvalidInput, "chunk", index));
                if (const auto parent = std::ranges::lower_bound(mesh.chunks, input.parent, {}, &ChunkMesh::id);
                    input.parent.IsValid() && (parent == mesh.chunks.end() || parent->id != input.parent))
                    return Result<void>::Failure(Context(StructuralGraphErrors::InvalidInput, "parent", index));
                StructuralGraphChunk chunk;
                chunk.id = input.id;
                chunk.parent = input.parent;
                chunk.flags = {input.anchor, input.required, input.anchor, input.parent.IsValid()};
                if (input.anchor)
                    ++graph.validation.anchorCount;
                if (input.required)
                    ++graph.validation.requiredCount;
                graph.chunks.push_back(std::move(chunk));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> BuildContacts(StructuralGraphArtifact &graph, const StructuralGraphCookRequest &request,
                                                 const CancellationToken &cancellation) {
            for (std::size_t index = 0; index < request.contacts.size(); ++index) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(StructuralGraphErrors::Cancelled));
                const auto &contact = request.contacts[index];
                if (contact.low >= graph.chunks.size() || contact.high >= graph.chunks.size())
                    return Result<void>::Failure(Context(StructuralGraphErrors::InvalidIndex, "contact", index));
                if (contact.low >= contact.high || !std::isfinite(contact.weight) || contact.weight <= 0.0)
                    return Result<void>::Failure(Context(StructuralGraphErrors::InvalidInput, "contact", index));
                if (index != 0 &&
                    std::pair{request.contacts[index - 1].low, request.contacts[index - 1].high} >= std::pair{contact.low, contact.high})
                    return Result<void>::Failure(Context(StructuralGraphErrors::UnstableOrder, "contact", index));
                auto &low = graph.chunks[contact.low];
                auto &high = graph.chunks[contact.high];
                if (!std::isfinite(low.supportWeight + contact.weight) || !std::isfinite(high.supportWeight + contact.weight))
                    return Result<void>::Failure(Context(StructuralGraphErrors::InvalidInput, "weight", index));
                if (const auto charged =
                        Charge(graph.validation, request.limits, 2, sizeof(StructuralContactInput) + 2 * sizeof(std::uint32_t));
                    charged.HasError())
                    return charged;
                low.supportWeight += contact.weight;
                high.supportWeight += contact.weight;
                low.adjacency.push_back(contact.high);
                high.adjacency.push_back(contact.low);
            }
            graph.contacts = request.contacts;
            for (auto &chunk : graph.chunks)
                std::ranges::sort(chunk.adjacency);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateHierarchy(StructuralGraphArtifact &graph, const StructuralGraphCookRequest &request,
                                                     DestructionFeatureSet supportedFeatures, const CancellationToken &cancellation) {
            for (std::size_t index = 0; index < graph.chunks.size(); ++index) {
                std::uint32_t depth = 1;
                auto parent = graph.chunks[index].parent;
                while (parent.IsValid()) {
                    if (cancellation.IsCancellationRequested())
                        return Result<void>::Failure(MakeError(StructuralGraphErrors::Cancelled));
                    if (const auto charged = Charge(graph.validation, request.limits, 1); charged.HasError())
                        return charged;
                    if (++depth > graph.chunks.size())
                        return Result<void>::Failure(Context(StructuralGraphErrors::HierarchyCycle, "chunk", index));
                    const auto it = std::ranges::lower_bound(graph.chunks, parent, {}, &StructuralGraphChunk::id);
                    if (it == graph.chunks.end() || it->id != parent)
                        return Result<void>::Failure(Context(StructuralGraphErrors::InvalidInput, "parent", index));
                    parent = it->parent;
                }
                if (depth > request.limits.maximumHierarchyDepth)
                    return Result<void>::Failure(Context(StructuralGraphErrors::LimitExceeded, "depth", index));
                graph.validation.maximumHierarchyDepth = std::max(graph.validation.maximumHierarchyDepth, depth);
            }
            if (graph.validation.maximumHierarchyDepth > 1)
                graph.producedFeatures.bits |= DestructionFeatureBit<DestructionFeature::HierarchicalFracture>;
            if ((graph.producedFeatures.bits & ~supportedFeatures.bits) != 0 ||
                (request.requiredFeatures.bits & ~graph.producedFeatures.bits) != 0)
                return Result<void>::Failure(MakeError(StructuralGraphErrors::Unsupported));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> MarkSupport(StructuralGraphArtifact &graph, const DestructionLimits &limits,
                                               const CancellationToken &cancellation) {
            std::vector<bool> supported(graph.chunks.size());
            std::vector<std::uint32_t> queue;
            queue.reserve(graph.chunks.size());
            for (std::uint32_t index = 0; index < graph.chunks.size(); ++index) {
                if (graph.chunks[index].flags.anchor) {
                    supported[index] = true;
                    queue.push_back(index);
                }
            }
            std::size_t head = 0;
            while (head < queue.size()) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(StructuralGraphErrors::Cancelled));
                for (const auto neighbor : graph.chunks[queue[head++]].adjacency) {
                    if (const auto charged = Charge(graph.validation, limits, 1); charged.HasError())
                        return charged;
                    if (!supported[neighbor]) {
                        supported[neighbor] = true;
                        queue.push_back(neighbor);
                    }
                }
            }
            for (std::uint32_t index = 0; index < graph.chunks.size(); ++index) {
                graph.chunks[index].flags.initiallySupported = supported[index];
                if (graph.chunks[index].flags.required && !supported[index])
                    return Result<void>::Failure(Context(StructuralGraphErrors::DisconnectedRequired, "chunk", index));
            }
            return Result<void>::Success();
        }

        /** @brief Expands one island from its seed, charging each visited contact and honoring cancellation. */
        [[nodiscard]] Result<void> TraverseIsland(StructuralGraphArtifact &graph, const DestructionLimits &limits,
                                                  const CancellationToken &cancellation, std::vector<bool> &visited,
                                                  std::vector<std::uint32_t> &queue) {
            std::size_t head = 0;
            while (head < queue.size()) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(StructuralGraphErrors::Cancelled));
                auto &chunk = graph.chunks[queue[head++]];
                chunk.island = graph.validation.islandCount;
                for (const auto neighbor : chunk.adjacency) {
                    if (const auto charged = Charge(graph.validation, limits, 1); charged.HasError())
                        return charged;
                    if (visited[neighbor])
                        continue;
                    visited[neighbor] = true;
                    queue.push_back(neighbor);
                }
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> AssignIslands(StructuralGraphArtifact &graph, const DestructionLimits &limits,
                                                 const CancellationToken &cancellation) {
            std::vector<bool> visited(graph.chunks.size());
            std::vector<std::uint32_t> queue;
            queue.reserve(graph.chunks.size());
            for (std::uint32_t index = 0; index < graph.chunks.size(); ++index) {
                if (visited[index])
                    continue;
                queue.clear();
                queue.push_back(index);
                visited[index] = true;
                if (const auto traversed = TraverseIsland(graph, limits, cancellation, visited, queue); traversed.HasError())
                    return traversed;
                ++graph.validation.islandCount;
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc CookStructuralGraph */
    Result<std::shared_ptr<const StructuralGraphArtifact>> CookStructuralGraph(const ChunkMeshArtifact &mesh,
                                                                               const StructuralGraphCookRequest &request,
                                                                               const CancellationToken &cancellation) {
        if (const auto valid = ValidateRequest(mesh, request, cancellation); valid.HasError())
            return Output::Failure(valid.ErrorValue());
        auto graph = std::make_shared<StructuralGraphArtifact>();
        graph->content = request.content;
        graph->meshIntegrityDigest = request.meshIntegrityDigest;
        graph->ownerRevision = request.ownerRevision;
        graph->policyRevision = request.policyRevision;
        graph->tier = mesh.tier;
        graph->producedFeatures.bits = mesh.producedFeatures.bits | DestructionFeatureBit<DestructionFeature::CookedSupport>;
        graph->validation.chunkCount = static_cast<std::uint32_t>(mesh.chunks.size());
        graph->validation.contactCount = static_cast<std::uint32_t>(request.contacts.size());
        auto stage = BuildChunks(*graph, mesh, request, cancellation);
        if (stage.HasError())
            return Output::Failure(stage.ErrorValue());
        stage = BuildContacts(*graph, request, cancellation);
        if (stage.HasError())
            return Output::Failure(stage.ErrorValue());
        const auto profile = GetDestructionTierProfile(mesh.tier);
        stage = ValidateHierarchy(*graph, request, profile.Value().supportedFeatures, cancellation);
        if (stage.HasError())
            return Output::Failure(stage.ErrorValue());
        stage = MarkSupport(*graph, request.limits, cancellation);
        if (stage.HasError())
            return Output::Failure(stage.ErrorValue());
        stage = AssignIslands(*graph, request.limits, cancellation);
        if (stage.HasError())
            return Output::Failure(stage.ErrorValue());
        graph->integrityDigest = GraphDigest(*graph);
        return Output::Success(std::move(graph));
    }

    /** @copydoc StructuralGraphCookOwner::Revision */
    StructuralGraphOwnerRevision StructuralGraphCookOwner::Revision() const noexcept {
        return revision_;
    }

    /** @copydoc StructuralGraphCookOwner::Token */
    CancellationToken StructuralGraphCookOwner::Token() const noexcept {
        return cancellation_.Token();
    }

    /** @copydoc StructuralGraphCookOwner::Snapshot */
    std::shared_ptr<const StructuralGraphArtifact> StructuralGraphCookOwner::Snapshot() const noexcept {
        return current_;
    }

    /** @copydoc StructuralGraphCookOwner::Accept */
    Result<void> StructuralGraphCookOwner::Accept(std::shared_ptr<const StructuralGraphArtifact> candidate,
                                                  StructuralGraphOwnerRevision expectedRevision,
                                                  const FractureArtifactContentIdentity &currentContent,
                                                  const Sha256Digest &currentMeshDigest, StructuralPolicyRevision currentPolicyRevision) {
        if (shutdown_)
            return Result<void>::Failure(MakeError(StructuralGraphErrors::Shutdown));
        if (revision_ != expectedRevision || cancellation_.Token().IsCancellationRequested() || !candidate ||
            candidate->ownerRevision != expectedRevision || candidate->content != currentContent ||
            candidate->meshIntegrityDigest != currentMeshDigest || candidate->policyRevision != currentPolicyRevision)
            return Result<void>::Failure(MakeError(StructuralGraphErrors::Stale));
        if (candidate->schemaVersion != StructuralGraphSchemaVersion || candidate->integrityDigest != GraphDigest(*candidate))
            return Result<void>::Failure(MakeError(StructuralGraphErrors::InvalidInput));
        current_ = std::move(candidate);
        return Result<void>::Success();
    }

    /** @copydoc StructuralGraphCookOwner::Invalidate */
    Result<void> StructuralGraphCookOwner::Invalidate() {
        if (shutdown_)
            return Result<void>::Failure(MakeError(StructuralGraphErrors::Shutdown));
        if (revision_.Value() == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(DestructionErrors::RevisionExhausted));
        cancellation_.RequestCancellation();
        revision_ = StructuralGraphOwnerRevision::Create(revision_.Value() + 1).Value();
        cancellation_ = CancellationSource{};
        return Result<void>::Success();
    }

    /** @copydoc StructuralGraphCookOwner::Shutdown */
    void StructuralGraphCookOwner::Shutdown() noexcept {
        shutdown_ = true;
        cancellation_.RequestCancellation();
    }
}  // namespace Horo::Destruction
