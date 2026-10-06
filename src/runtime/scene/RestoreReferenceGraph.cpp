#include "Horo/Runtime/Scene/RestoreReferenceGraph.h"

#include "Horo/Runtime/Save/SaveErrors.h"
#include "RuntimeSceneErrors.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <new>
#include <queue>
#include <tuple>
#include <type_traits>

namespace Horo::Runtime {
    namespace {
        /** @brief Availability is distinct from malformed or stale evidence, which always remains an error. */
        struct TargetResolution final {
            bool available{};
            std::optional<EntityRef> entity;
        };

        /** @brief Resolves an exact incarnation without permitting stale references to become optional absence. */
        [[nodiscard]] Result<TargetResolution> ResolveEntity(const RestoreEntityReference &target,
                                                             const RestoreReferenceAuthorities &authorities) {
            if (!target.target.world.IsValid() || target.target.world != authorities.world || !target.target.entity.IsValid() ||
                !target.generation.IsValid())
                return Result<TargetResolution>::Failure(MakeError(SaveErrors::ReferenceInvalid));
            auto entity = authorities.identities->Resolve(target.target.entity, target.generation);
            if (entity.HasError()) {
                const auto &error = entity.ErrorValue();
                if (const auto absent = [&](const ErrorCodeDescriptor &descriptor) {
                    return error.domain.Value() == descriptor.domain.Value() && error.code.Value() == descriptor.code.Value();
                }; absent(SceneErrors::PersistentIdentityUnknown) || absent(SceneErrors::PersistentIdentityTombstoned))
                    return Result<TargetResolution>::Success({});
                return Result<TargetResolution>::Failure(entity.ErrorValue());
            }
            if (entity.Value().runtime != authorities.scene.RuntimeId() || authorities.scene.Get(entity.Value()).HasError())
                return Result<TargetResolution>::Failure(MakeError(SaveErrors::ReferenceResolutionInvalid));
            return Result<TargetResolution>::Success({true, entity.Value()});
        }

        /** @brief Checks exact component ownership through the candidate entity authority. */
        [[nodiscard]] Result<TargetResolution> ResolveComponent(const RestoreComponentReference &target,
                                                                const RestoreReferenceAuthorities &authorities) {
            if (!target.type.IsValid() || !authorities.components)
                return Result<TargetResolution>::Failure(MakeError(SaveErrors::ReferenceInvalid));
            auto entity = ResolveEntity(target.entity, authorities);
            if (entity.HasError() || !entity.Value().available)
                return entity;
            if (!authorities.components->Contains(*entity.Value().entity, target.type))
                return Result<TargetResolution>::Success({});
            return entity;
        }

        /** @brief Checks a pinned cooked payload and its exact semantic asset type. */
        [[nodiscard]] Result<TargetResolution> ResolveAsset(const RestoreAssetReference &target,
                                                            const RestoreReferenceAuthorities &authorities) {
            if (!target.asset.IsValid() || target.type.Value().empty())
                return Result<TargetResolution>::Failure(MakeError(SaveErrors::ReferenceInvalid));
            const auto asset = authorities.scene.FindAsset(target.asset);
            if (!asset)
                return Result<TargetResolution>::Success({});
            if (!asset->type || *asset->type != target.type)
                return Result<TargetResolution>::Failure(MakeError(SaveErrors::ReferenceResolutionInvalid));
            return Result<TargetResolution>::Success({true, {}});
        }

        /** @brief Checks a frozen, explicitly active service generation with a module lifetime pin. */
        [[nodiscard]] Result<TargetResolution> ResolveService(const RestoreServiceReference &target,
                                                              const RestoreReferenceAuthorities &authorities) {
            if (target.service.Value().empty() || target.generation == 0 || !authorities.services || !authorities.services->IsFrozen() ||
                !authorities.serviceLease)
                return Result<TargetResolution>::Failure(MakeError(SaveErrors::ReferenceInvalid));
            if (target.generation != authorities.serviceGeneration)
                return Result<TargetResolution>::Failure(MakeError(SaveErrors::RestoreActivationStale));
            const bool active = authorities.services->Find(target.service) &&
                                std::ranges::find(authorities.activeServices, target.service) != authorities.activeServices.end();
            return Result<TargetResolution>::Success({active, {}});
        }

        /** @brief Checks an actual prepared participant and its exact declared schema/record. */
        [[nodiscard]] Result<TargetResolution> ResolveParticipant(const RestoreParticipantReference &target,
                                                                  const RestoreReferenceAuthorities &authorities) {
            if (!target.participant.IsValid() || !target.schema.IsValid() || (target.record && !target.record->IsValid()))
                return Result<TargetResolution>::Failure(MakeError(SaveErrors::ReferenceInvalid));
            const auto *binding = authorities.participants.Find(target.participant);
            if (!binding)
                return Result<TargetResolution>::Success({});
            const auto &descriptor = binding->Descriptor();
            if (descriptor.schemaVersion != target.schema || !HasSaveParticipantRole(descriptor.roles, SaveParticipantRole::Restore))
                return Result<TargetResolution>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid));
            const bool prepared = std::ranges::find(authorities.preparedOwners, target.participant) != authorities.preparedOwners.end();
            const bool present =
                prepared && (!target.record || std::ranges::find(descriptor.ownedRecords, *target.record) != descriptor.ownedRecords.end());
            return Result<TargetResolution>::Success({present, {}});
        }

        /** @brief Dispatches the closed target kind to its existing semantic authority. */
        [[nodiscard]] Result<TargetResolution> ResolveTarget(const RestoreReferenceTarget &target,
                                                             const RestoreReferenceAuthorities &authorities) {
            return std::visit([&]<typename Type>(const Type &value) -> Result<TargetResolution> {
                if constexpr (std::is_same_v<Type, RestoreEntityReference>)
                    return ResolveEntity(value, authorities);
                else if constexpr (std::is_same_v<Type, RestoreComponentReference>)
                    return ResolveComponent(value, authorities);
                else if constexpr (std::is_same_v<Type, RestoreAssetReference>)
                    return ResolveAsset(value, authorities);
                else if constexpr (std::is_same_v<Type, RestoreServiceReference>)
                    return ResolveService(value, authorities);
                else
                    return ResolveParticipant(value, authorities);
            }, target);
        }

        /** @brief Validates explicit fallback kind and resolves it without weakening exact-generation checks. */
        [[nodiscard]] Result<ResolvedRestoreReference> ResolveRequest(const RestoreReferenceRequest &request,
                                                                      const RestoreReferenceAuthorities &authorities) {
            if (request.identity == 0 || !authorities.participants.Find(request.owner) ||
                std::ranges::find(authorities.preparedOwners, request.owner) == authorities.preparedOwners.end() ||
                (request.phase != RestoreReferencePhase::AllocationPrerequisite && request.phase != RestoreReferencePhase::DeferredFixup) ||
                (request.presence != RestoreReferencePresence::Required && request.presence != RestoreReferencePresence::Optional) ||
                (request.permittedReplacement &&
                 (request.permittedReplacement->index() != request.target.index() || *request.permittedReplacement == request.target)))
                return Result<ResolvedRestoreReference>::Failure(MakeError(SaveErrors::ReferenceInvalid));
            // Component/service identities stay in their typed project payloads. This context may
            // remap an entity owner, but cannot silently change a component type or service identity.
            if (request.permittedReplacement) {
                if (const auto *component = std::get_if<RestoreComponentReference>(&request.target);
                    component && component->type != std::get<RestoreComponentReference>(*request.permittedReplacement).type)
                    return Result<ResolvedRestoreReference>::Failure(MakeError(SaveErrors::ReferenceInvalid));
                if (const auto *service = std::get_if<RestoreServiceReference>(&request.target);
                    service && service->service != std::get<RestoreServiceReference>(*request.permittedReplacement).service)
                    return Result<ResolvedRestoreReference>::Failure(MakeError(SaveErrors::ReferenceInvalid));
            }
            auto resolved = ResolveTarget(request.target, authorities);
            if (resolved.HasError())
                return Result<ResolvedRestoreReference>::Failure(resolved.ErrorValue());
            if (resolved.Value().available)
                return Result<ResolvedRestoreReference>::Success({request, SaveReferenceDisposition::Resolved, resolved.Value().entity});
            if (request.permittedReplacement) {
                auto replacement = ResolveTarget(*request.permittedReplacement, authorities);
                if (replacement.HasError())
                    return Result<ResolvedRestoreReference>::Failure(replacement.ErrorValue());
                if (replacement.Value().available)
                    return Result<ResolvedRestoreReference>::Success(
                        {request, SaveReferenceDisposition::Remapped, replacement.Value().entity});
            }
            if (request.presence == RestoreReferencePresence::Required)
                return Result<ResolvedRestoreReference>::Failure(MakeError(SaveErrors::ReferenceResolutionInvalid));
            return Result<ResolvedRestoreReference>::Success({request, SaveReferenceDisposition::Missing, {}});
        }

        /** @brief Finds a canonical node index without traversing payload graphs recursively. */
        [[nodiscard]] std::optional<std::size_t> FindNode(const std::vector<RestoreReferenceTarget> &nodes,
                                                          const RestoreReferenceTarget &target) {
            const auto found = std::ranges::lower_bound(nodes, target);
            if (found == nodes.end() || *found != target)
                return std::nullopt;
            return static_cast<std::size_t>(found - nodes.begin());
        }

        /** @brief Computes stable provider-first order; only allocation prerequisites participate in cycle rejection. */
        [[nodiscard]] Result<std::vector<RestoreReferenceTarget>> BuildOrder(const std::vector<RestoreReferenceTarget> &nodes,
                                                                             const std::vector<ResolvedRestoreReference> &references) {
            std::vector<std::pair<std::size_t, std::size_t>> edges;
            std::vector<std::size_t> pending(nodes.size());
            edges.reserve(references.size());
            for (const auto &reference : references) {
                const auto source = FindNode(nodes, reference.request.source);
                if (!source.has_value())
                    return Result<std::vector<RestoreReferenceTarget>>::Failure(MakeError(SaveErrors::ReferenceResolutionInvalid));
                if (reference.request.phase != RestoreReferencePhase::AllocationPrerequisite ||
                    reference.disposition == SaveReferenceDisposition::Missing)
                    continue;
                const auto &target = reference.disposition == SaveReferenceDisposition::Remapped ? *reference.request.permittedReplacement
                                                                                                 : reference.request.target;
                const auto dependency = FindNode(nodes, target);
                if (!dependency.has_value())
                    return Result<std::vector<RestoreReferenceTarget>>::Failure(MakeError(SaveErrors::ReferenceResolutionInvalid));
                edges.emplace_back(*dependency, *source);
            }
            std::ranges::sort(edges);
            const auto duplicates = std::ranges::unique(edges);
            edges.erase(duplicates.begin(), duplicates.end());
            for (const auto &[dependency, source] : edges) {
                static_cast<void>(dependency);
                ++pending[source];
            }
            std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<>> ready;
            for (std::size_t index = 0; index < nodes.size(); ++index)
                if (pending[index] == 0)
                    ready.push(index);
            std::vector<RestoreReferenceTarget> order;
            order.reserve(nodes.size());
            while (!ready.empty()) {
                const std::size_t next = ready.top();
                ready.pop();
                order.push_back(nodes[next]);
                auto edge = std::ranges::lower_bound(edges, std::pair{next, std::size_t{0}});
                while (edge != edges.end() && edge->first == next) {
                    if (--pending[edge->second] == 0)
                        ready.push(edge->second);
                    ++edge;
                }
            }
            if (order.size() != nodes.size())
                return Result<std::vector<RestoreReferenceTarget>>::Failure(MakeError(SaveErrors::ParticipantDependencyCycle));
            return Result<std::vector<RestoreReferenceTarget>>::Success(std::move(order));
        }
    }  // namespace

    /** @copydoc PreparedRestoreReferenceGraph::Create */
    Result<PreparedRestoreReferenceGraph> PreparedRestoreReferenceGraph::Create(
        const RestoreReferenceAuthorities &authorities, const std::span<const RestoreReferenceTarget> inputNodes,
        const std::span<const RestoreReferenceRequest> inputReferences, const RestoreReferenceGraphLimits limits) {
        if (!authorities.world.IsValid() || !authorities.scene.IsCurrent() || !authorities.identities ||
            !authorities.participants.IsValid() || limits.maximumNodes == 0 || limits.maximumNodes > 16'384 ||
            authorities.activeServices.size() > Gameplay::MaximumGameplayServices ||
            authorities.preparedOwners.size() > MaximumSaveParticipantCount || limits.maximumReferences == 0 ||
            limits.maximumReferences > 65'536 || inputNodes.size() > limits.maximumNodes ||
            inputReferences.size() > limits.maximumReferences)
            return Result<PreparedRestoreReferenceGraph>::Failure(MakeError(SaveErrors::RestoreContextInvalid));
        try {
            std::vector<RestoreReferenceTarget> nodes(inputNodes.begin(), inputNodes.end());
            std::ranges::sort(nodes);
            if (std::ranges::adjacent_find(nodes) != nodes.end())
                return Result<PreparedRestoreReferenceGraph>::Failure(MakeError(SaveErrors::ReferenceInvalid));
            for (const auto &node : nodes) {
                auto resolved = ResolveTarget(node, authorities);
                if (resolved.HasError())
                    return Result<PreparedRestoreReferenceGraph>::Failure(resolved.ErrorValue());
                if (!resolved.Value().available)
                    return Result<PreparedRestoreReferenceGraph>::Failure(MakeError(SaveErrors::ReferenceResolutionInvalid));
            }
            std::vector<ResolvedRestoreReference> references;
            references.reserve(inputReferences.size());
            for (const auto &request : inputReferences) {
                auto resolved = ResolveRequest(request, authorities);
                if (resolved.HasError())
                    return Result<PreparedRestoreReferenceGraph>::Failure(resolved.ErrorValue());
                references.push_back(std::move(resolved).Value());
            }
            std::ranges::sort(references, [](const auto &left, const auto &right) {
                return left.request.owner < right.request.owner ||
                       (left.request.owner == right.request.owner && left.request.identity < right.request.identity);
            });
            if (std::ranges::adjacent_find(references, [](const auto &left, const auto &right) {
                return left.request.owner == right.request.owner && left.request.identity == right.request.identity;
            }) != references.end())
                return Result<PreparedRestoreReferenceGraph>::Failure(MakeError(SaveErrors::ReferenceInvalid));
            auto order = BuildOrder(nodes, references);
            if (order.HasError())
                return Result<PreparedRestoreReferenceGraph>::Failure(order.ErrorValue());
            return Result<PreparedRestoreReferenceGraph>::Success(
                PreparedRestoreReferenceGraph{std::move(order).Value(), std::move(references), authorities.participants,
                                              authorities.serviceLease, authorities.scene.RuntimeId()});
        } catch (const std::bad_alloc &) {
            return Result<PreparedRestoreReferenceGraph>::Failure(MakeError(SaveErrors::RestoreAllocationFailed));
        }
    }

    /** @copydoc PreparedRestoreReferenceGraph::AllocationOrder */
    std::span<const RestoreReferenceTarget> PreparedRestoreReferenceGraph::AllocationOrder() const noexcept {
        return order_;
    }

    /** @copydoc PreparedRestoreReferenceGraph::References */
    std::span<const ResolvedRestoreReference> PreparedRestoreReferenceGraph::References() const noexcept {
        return references_;
    }

    /** @copydoc PreparedRestoreReferenceGraph::Find */
    const ResolvedRestoreReference *PreparedRestoreReferenceGraph::Find(const SaveParticipantId &owner,
                                                                        const std::uint64_t identity) const noexcept {
        const auto found =
            std::ranges::lower_bound(references_, std::tie(owner, identity), {}, [](const ResolvedRestoreReference &reference) {
            return std::tie(reference.request.owner, reference.request.identity);
        });
        return found == references_.end() || found->request.owner != owner || found->request.identity != identity ? nullptr
                                                                                                                  : std::to_address(found);
    }

    /** @copydoc PreparedRestoreReferenceGraph::MakeContext */
    Result<SaveRestoreReferenceContext> PreparedRestoreReferenceGraph::MakeContext(const SaveRestoreReferenceGeneration generation) const {
        if (generation.registry != participants_.Generation() || generation.candidateScene != candidate_.value)
            return Result<SaveRestoreReferenceContext>::Failure(MakeError(SaveErrors::RestoreActivationStale));
        try {
            std::vector<SaveRestoreReferenceResult> results;
            results.reserve(references_.size());
            for (const auto &reference : references_) {
                const auto &target = reference.disposition == SaveReferenceDisposition::Remapped ? *reference.request.permittedReplacement
                                                                                                 : reference.request.target;
                const auto projected = std::visit([]<typename Type>(const Type &value) -> SaveRestoreReferenceTarget {
                    if constexpr (std::is_same_v<Type, RestoreEntityReference>)
                        return SaveRestoreEntityTarget{value.target.entity, value.generation.value};
                    else if constexpr (std::is_same_v<Type, RestoreComponentReference>)
                        return SaveRestoreComponentTarget{{value.entity.target.entity, value.entity.generation.value}};
                    else if constexpr (std::is_same_v<Type, RestoreAssetReference>)
                        return SaveRestoreAssetTarget{SaveAssetId::FromBytes(value.asset.Bytes()).Value()};
                    else if constexpr (std::is_same_v<Type, RestoreServiceReference>)
                        return SaveRestoreServiceTarget{value.generation};
                    else
                        return SaveRestoreParticipantTarget{value.participant, value.schema, value.record};
                }, target);
                auto disposition = SaveRestoreReferenceDisposition::Resolved;
                if (reference.disposition == SaveReferenceDisposition::Missing)
                    disposition = SaveRestoreReferenceDisposition::OptionalAbsent;
                else if (reference.disposition == SaveReferenceDisposition::Remapped)
                    disposition = SaveRestoreReferenceDisposition::Remapped;
                results.push_back({reference.request.owner,
                                   {reference.request.identity},
                                   disposition,
                                   reference.request.presence == RestoreReferencePresence::Required,
                                   projected});
            }
            return SaveRestoreReferenceContext::Create(generation, results);
        } catch (const std::bad_alloc &) {
            return Result<SaveRestoreReferenceContext>::Failure(MakeError(SaveErrors::RestoreAllocationFailed));
        }
    }

    PreparedRestoreReferenceGraph::PreparedRestoreReferenceGraph(std::vector<RestoreReferenceTarget> order,
                                                                 std::vector<ResolvedRestoreReference> references,
                                                                 SaveParticipantRegistrySnapshot participants,
                                                                 std::shared_ptr<void> serviceLease,
                                                                 const SceneRuntimeId candidate) noexcept
        : order_(std::move(order)), references_(std::move(references)), participants_(std::move(participants)),
          serviceLease_(std::move(serviceLease)), candidate_(candidate) {}
}  // namespace Horo::Runtime
