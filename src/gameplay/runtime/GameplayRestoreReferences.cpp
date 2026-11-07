#include "GameplayRestoreReferencesInternal.h"
#include "Horo/Gameplay/GameplayErrors.h"

#include <algorithm>
#include <map>
#include <set>

namespace Horo::Gameplay::Detail {
    namespace {
        using namespace Runtime;

        Error Invalid() {
            return MakeError(SaveErrors::ReferenceResolutionInvalid);
        }

        Result<RestoreReferenceTarget> Convert(const BehaviorEntityRestoreTarget &value) {
            return Result<RestoreReferenceTarget>::Success(RestoreEntityReference{value.entity, {value.generation}});
        }

        Result<RestoreReferenceTarget> Convert(const BehaviorComponentRestoreTarget &value) {
            return Result<RestoreReferenceTarget>::Success(
                RestoreComponentReference{{value.entity.entity, {value.entity.generation}}, value.type});
        }

        Result<RestoreReferenceTarget> Convert(const BehaviorAssetRestoreTarget &value) {
            const auto asset = Assets::AssetId::FromBytes(value.identity.Bytes());
            if (!asset.IsValid())
                return Result<RestoreReferenceTarget>::Failure(Invalid());
            auto type = Assets::AssetTypeId::Parse(value.type.Value());
            if (type.HasError())
                return Result<RestoreReferenceTarget>::Failure(type.ErrorValue());
            return Result<RestoreReferenceTarget>::Success(RestoreAssetReference{asset, std::move(type).Value()});
        }

        Result<RestoreReferenceTarget> Convert(const BehaviorServiceRestoreTarget &value) {
            return Result<RestoreReferenceTarget>::Success(RestoreServiceReference{value.service, value.generation});
        }

        Result<RestoreReferenceTarget> Convert(const SaveRestoreParticipantTarget &value) {
            return Result<RestoreReferenceTarget>::Success(RestoreParticipantReference{value.identity, value.schema, value.record});
        }

        using SourceMap = std::map<BehaviorRestoreAttachment, RestoreReferenceTarget>;

        class SourceVisitor final : public IBehaviorPersistenceVisitor {
        public:
            SourceVisitor(const RestoreReferenceAuthorities &authorities) : authorities_(authorities) {}

            Result<void> Visit(std::span<const BehaviorRestoreInstance> instances) override {
                if (instances.size() > 16'384)
                    return Result<void>::Failure(Invalid());
                for (const auto &instance : instances) {
                    const EntityRef actual{{instance.entity.scene}, {instance.entity.index, instance.entity.generation}};
                    auto resolved = authorities_.identities->Resolve(instance.persistentEntity, {instance.persistentGeneration});
                    if (resolved.HasError())
                        return Result<void>::Failure(resolved.ErrorValue());
                    if (resolved.Value() != actual)
                        return Result<void>::Failure(Invalid());
                    const BehaviorRestoreAttachment key{instance.persistentEntity, instance.persistentGeneration, instance.instance};
                    if (!sources.emplace(key, RestoreEntityReference{{authorities_.world, key.entity}, {key.generation}}).second)
                        return Result<void>::Failure(Invalid());
                }
                return Result<void>::Success();
            }

            SourceMap sources;

        private:
            const RestoreReferenceAuthorities &authorities_;
        };

        Result<void> Insert(std::set<RestoreReferenceTarget> &nodes, const RestoreReferenceTarget &node,
                            const RestoreReferenceGraphLimits &limits) {
            if (nodes.contains(node))
                return Result<void>::Success();
            if (nodes.size() >= limits.maximumNodes)
                return Result<void>::Failure(Invalid());
            nodes.insert(node);
            return Result<void>::Success();
        }

        Result<RestoreReferenceRequest> ConvertOne(const BehaviorRestoreReference &value, const SourceMap &sources,
                                                   const SaveParticipantId &owner) {
            if (value.owner != owner || value.identity.value == 0 || !value.source.entity.IsValid() || value.source.generation == 0 ||
                !value.source.instance.IsValid())
                return Result<RestoreReferenceRequest>::Failure(Invalid());
            const auto source = sources.find(value.source);
            if (source == sources.end())
                return Result<RestoreReferenceRequest>::Failure(Invalid());
            auto converted = std::visit([](const auto &input) {
                return Convert(input);
            }, value.target);
            if (converted.HasError())
                return Result<RestoreReferenceRequest>::Failure(converted.ErrorValue());
            return Result<RestoreReferenceRequest>::Success(
                {value.identity.value,
                 owner,
                 source->second,
                 std::move(converted).Value(),
                 value.allocationPrerequisite ? RestoreReferencePhase::AllocationPrerequisite : RestoreReferencePhase::DeferredFixup,
                 value.required ? RestoreReferencePresence::Required : RestoreReferencePresence::Optional,
                 {}});
        }
    }  // namespace

    Result<Runtime::RestoreReferenceContribution> ConvertBehaviorRestoreReferences(std::span<const BehaviorRestoreReference> inventory,
                                                                                   const BehaviorPersistenceTarget &target,
                                                                                   const Runtime::RestoreReferenceAuthorities &authorities,
                                                                                   const Runtime::SaveParticipantId &owner,
                                                                                   const Runtime::RestoreReferenceGraphLimits &limits) {
        using Outcome = Result<Runtime::RestoreReferenceContribution>;
        if (inventory.size() > limits.maximumReferences || !target.CanUse() || !target.InactiveRestore() || !authorities.identities)
            return Outcome::Failure(Invalid());
        SourceVisitor sources{authorities};
        if (auto visited = target.Visit(sources); visited.HasError())
            return Outcome::Failure(visited.ErrorValue());
        Runtime::RestoreReferenceContribution output;
        output.references.reserve(inventory.size());
        std::set<Runtime::RestoreReferenceTarget> nodes;
        for (const auto &entry : inventory) {
            auto converted = ConvertOne(entry, sources.sources, owner);
            if (converted.HasError())
                return Outcome::Failure(converted.ErrorValue());
            auto request = std::move(converted).Value();
            if (auto source = Insert(nodes, request.source, limits); source.HasError())
                return Outcome::Failure(source.ErrorValue());
            if (auto destination = Insert(nodes, request.target, limits); destination.HasError())
                return Outcome::Failure(destination.ErrorValue());
            output.references.push_back(std::move(request));
        }
        output.nodes.assign(nodes.begin(), nodes.end());
        return Outcome::Success(std::move(output));
    }
}  // namespace Horo::Gameplay::Detail
