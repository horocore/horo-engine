#include "Horo/AI/AIErrors.h"
#include "Horo/AI/BlackboardUsageInspection.h"
#include "Horo/AI/DecisionGraphCompiler.h"

#include <algorithm>
#include <type_traits>

namespace Horo::AI {
    namespace {
        /** @brief Finds a key in the admitted schema's stable identity order. */
        const BlackboardKeyDescriptor *FindKey(const BlackboardSchema &schema, BlackboardKeyId key) {
            const auto keys = schema.Keys();
            const auto found = std::ranges::lower_bound(keys, key, {}, &BlackboardKeyDescriptor::key);
            return found != keys.end() && found->key == key ? &*found : nullptr;
        }

        /** @brief Checks the key contract independently of identity and the value of a provided default. */
        bool SameKeyContract(const BlackboardKeyDescriptor &left, const BlackboardKeyDescriptor &right) {
            return left.kind == right.kind && left.access == right.access && left.presence == right.presence &&
                   left.cardinality == right.cardinality && left.maximumCollectionElements == right.maximumCollectionElements &&
                   left.defaultValue.has_value() == right.defaultValue.has_value();
        }

        /** @brief Rewrites bounded authored records in the isolated source candidate. */
        class KeyRetarget final {
        public:
            KeyRetarget(BlackboardKeyId from, BlackboardKeyId to) : from_(from), to_(to) {}

            template <class Entry> void Keys(std::vector<Entry> &entries) {
                for (auto &entry : entries)
                    Key(entry.key);
            }

            Result<void> Requirements(std::vector<DecisionBlackboardRequirement> &requirements) {
                if (std::ranges::find(requirements, from_, &DecisionBlackboardRequirement::key) == requirements.end())
                    return Result<void>::Success();
                if (std::ranges::find(requirements, to_, &DecisionBlackboardRequirement::key) != requirements.end())
                    return Result<void>::Failure(MakeError(AIErrors::DecisionAssetBindingAmbiguous));
                Keys(requirements);
                return Result<void>::Success();
            }

            Result<void> Properties(std::vector<BehaviorTreeProperty> &properties) {
                for (auto &property : properties) {
                    if (property.kind == BehaviorTreePropertyKind::Opaque)
                        return Result<void>::Failure(MakeError(AIErrors::DecisionAssetDescriptorIncompatible));
                    if (auto *output = std::get_if<BehaviorTreeQueryOutput>(&property.value))
                        Key(output->key);
                }
                return Result<void>::Success();
            }

            bool Changed() const noexcept {
                return changed_;
            }

        private:
            void Key(BlackboardKeyId &key) noexcept {
                if (key == from_) {
                    key = to_;
                    changed_ = true;
                }
            }

            BlackboardKeyId from_;
            BlackboardKeyId to_;
            bool changed_{};
        };

        /** @brief Preserves Behavior Tree topology and recaptures canonical immutable source. */
        Result<void> RetargetTree(std::shared_ptr<const BehaviorTreeAsset> &tree, KeyRetarget &retarget) {
            auto body = tree->Data();
            for (auto &node : body.nodes) {
                if (auto uses = retarget.Requirements(node.requirements); uses.HasError())
                    return uses;
                if (auto uses = retarget.Properties(node.properties); uses.HasError())
                    return uses;
            }
            if (!retarget.Changed())
                return Result<void>::Success();
            auto capture = BehaviorTreeAsset::Capture(std::move(body));
            if (capture.HasError())
                return Result<void>::Failure(capture.ErrorValue());
            tree = std::make_shared<const BehaviorTreeAsset>(std::move(capture).Value());
            return Result<void>::Success();
        }

        /** @brief Retargets explicitly authored action declarations, including typed output bindings. */
        Result<void> RetargetAction(StateMachineProviderAction &action, KeyRetarget &retarget) {
            if (auto uses = retarget.Requirements(action.binding.requirements); uses.HasError())
                return uses;
            return retarget.Properties(action.properties);
        }

        /** @brief Visits every authored action phase while allowing states with no provider. */
        Result<void> RetargetStateActions(StateMachineState &state, KeyRetarget &retarget) {
            for (auto *action : {&state.entryProvider, &state.updateProvider, &state.exitProvider}) {
                if (!*action)
                    continue;
                if (auto uses = RetargetAction(**action, retarget); uses.HasError())
                    return uses;
            }
            return Result<void>::Success();
        }

        /** @brief Preserves State Machine control topology while rewriting guards and authored actions. */
        Result<void> RetargetMachine(StateMachineAssetDescriptor &machine, KeyRetarget &retarget) {
            for (auto &transition : machine.transitions) {
                if (auto uses = retarget.Requirements(transition.requirements); uses.HasError())
                    return uses;
                retarget.Keys(transition.guards);
            }
            for (auto &state : machine.states)
                if (auto uses = RetargetStateActions(state, retarget); uses.HasError())
                    return uses;
            return Result<void>::Success();
        }

        /** @brief Preserves option scoring policy and retargets the declared key used by each consideration. */
        Result<void> RetargetUtility(UtilityDecisionSource &utility, KeyRetarget &retarget) {
            if (auto uses = retarget.Requirements(utility.requirements); uses.HasError())
                return uses;
            for (auto &option : utility.options) {
                retarget.Keys(option.option.considerations);
                if (auto uses = retarget.Requirements(option.provider.requirements); uses.HasError())
                    return uses;
                if (auto uses = retarget.Properties(option.properties); uses.HasError())
                    return uses;
            }
            return Result<void>::Success();
        }

        /** @brief Refuses references that require a new provider declaration instead of a source edit. */
        Result<void> CheckProviderUses(const DecisionBlackboardSourceUses &uses) {
            if (!uses.unresolvedProviders.empty())
                return Result<void>::Failure(MakeError(AIErrors::DecisionAssetDescriptorMissing));
            if (std::ranges::any_of(uses.uses, [](const auto &use) {
                return use.provider.has_value();
            }))
                return Result<void>::Failure(MakeError(AIErrors::DecisionAssetDescriptorIncompatible));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc RetargetDecisionBlackboardKey */
    Result<bool> RetargetDecisionBlackboardKey(DecisionGraphSource &source, const BlackboardSchema &schema, BlackboardKeyId from,
                                               BlackboardKeyId to, const DecisionBlackboardProviderMetadata *providers) {
        using Retargeted = Result<bool>;
        if (from == to || source.blackboard != schema.Identity() || !source.blackboardVersion.Contains(schema.Version()))
            return Retargeted::Failure(MakeError(AIErrors::DecisionAssetSchemaInvalid));
        const auto *original = FindKey(schema, from);
        const auto *replacement = FindKey(schema, to);
        if (!original || !replacement || !SameKeyContract(*original, *replacement))
            return Retargeted::Failure(MakeError(AIErrors::BlackboardValueTypeMismatch));
        const auto evidence = InspectDecisionBlackboardUses(source, from, providers);
        if (evidence.HasError())
            return Retargeted::Failure(evidence.ErrorValue());
        if (auto writable = CheckProviderUses(evidence.Value()); writable.HasError())
            return Retargeted::Failure(writable.ErrorValue());

        auto candidate = source;
        KeyRetarget retarget{from, to};
        const auto result = std::visit([&]<class Body>(Body &body) {
            if constexpr (std::is_same_v<Body, std::shared_ptr<const BehaviorTreeAsset>>)
                return RetargetTree(body, retarget);
            else if constexpr (std::is_same_v<Body, StateMachineAssetDescriptor>)
                return RetargetMachine(body, retarget);
            else
                return RetargetUtility(body, retarget);
        }, candidate.body);
        if (result.HasError())
            return Retargeted::Failure(result.ErrorValue());
        if (retarget.Changed())
            source = std::move(candidate);
        return Retargeted::Success(retarget.Changed());
    }
}  // namespace Horo::AI
