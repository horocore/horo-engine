#include "AnimationGraphCompilerInternal.h"

#include <set>
#include <string_view>

namespace Horo::Animation::GraphCompileDetail {
    namespace {
        using namespace AnimationErrors;

        /** @brief Stable ASCII names remain bounded metadata; IDs remain binding
         * authority. */
        /** @brief Shared character predicate keeps accepted identifier names explicit. */
        bool Letter(const unsigned char c) noexcept {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        }

        bool Name(const std::string_view text) noexcept {
            if (text.empty() || text.size() > AnimationGraphHardLimits::ParameterNameBytes)
                return false;
            if (!Letter(static_cast<unsigned char>(text.front())))
                return false;
            for (const auto c : text)
                if (!Letter(static_cast<unsigned char>(c)) && !(c >= '0' && c <= '9'))
                    return false;
            return true;
        }

        /** @brief Checks finite action ceilings independently from ordinary graph node
         * counts. */
        bool Limits(const AnimationGraphLimits &limits) noexcept {
            return limits.actionSlots <= AnimationGraphHardLimits::ActionSlots && limits.actions <= AnimationGraphHardLimits::Actions &&
                   limits.actionSections <= AnimationGraphHardLimits::ActionSections &&
                   limits.actionLoops <= AnimationGraphHardLimits::ActionLoops;
        }

        /** @brief Validates one section's exact IDs/range/link without requiring
         * runtime dependencies. */
        bool Section(const GraphActionSection &section, const GraphActionDefinition &action, const AnimationGraphData &data) {
            if (!Name(section.name) || !section.clip.IsValid())
                return false;
            if (section.clip.Asset() == data.id.Asset() || section.clip.Asset() == data.skeleton.Asset())
                return false;
            if (section.begin.ticks < 0 || section.end.ticks <= section.begin.ticks ||
                section.end.ticks > AnimationClipHardLimits::DurationTicks)
                return false;
            return !section.next.IsValid() || Find(action.sequence.sections, section.next);
        }

        /** @brief Ordinary successor links are acyclic; only explicit loop regions may
         * authorize repetition. */
        bool Acyclic(const GraphActionDefinition &action, const GraphActionSectionId start) {
            auto current = start;
            for (std::size_t count = 0; count <= action.sequence.sections.size(); ++count) {
                if (!current.IsValid())
                    return true;
                const auto *section = Find(action.sequence.sections, current);
                if (!section)
                    return false;
                current = section->next;
            }
            return false;
        }

        /** @brief Finite repetitions and explicit release loops use disjoint closed count contracts. */
        bool LoopCount(const GraphActionLoopRegion &loop) noexcept {
            if (loop.mode == GraphActionLoopMode::Finite)
                return loop.repetitions > 0 && loop.repetitions <= AnimationGraphHardLimits::ActionTransitions;
            return loop.mode == GraphActionLoopMode::UntilReleased && loop.repetitions == 0;
        }

        /** @brief Proves one loop path and records membership to reject overlap and
         * nesting. */
        bool Loop(const GraphActionDefinition &action, const GraphActionLoopRegion &loop, std::set<GraphActionSectionId> &members) {
            if (!LoopCount(loop))
                return false;
            auto current = loop.first;
            for (std::size_t count = 0; count < action.sequence.sections.size(); ++count) {
                const auto *section = Find(action.sequence.sections, current);
                if (!section || !members.insert(current).second)
                    return false;
                if (current == loop.last)
                    return true;
                current = section->next;
            }
            return false;
        }

        /** @brief Catalog shape is admitted before section and path walks. */
        bool SequenceShape(const GraphActionSequence &sequence) {
            return !sequence.sections.empty() && UniqueIds(sequence.sections) && UniqueIds(sequence.loops) &&
                   Find(sequence.sections, sequence.entry);
        }

        /** @brief Checks section names/topology and loop regions once at compilation.
         */
        bool Sequence(const GraphActionDefinition &action, const AnimationGraphData &data) {
            const auto &sequence = action.sequence;
            if (!SequenceShape(sequence))
                return false;
            std::set<std::string, std::less<>> names;
            for (const auto &section : sequence.sections)
                if (!Section(section, action, data) || !names.insert(section.name).second || !Acyclic(action, section.id))
                    return false;
            std::set<GraphActionSectionId> members;
            for (const auto &loop : sequence.loops)
                if (!Loop(action, loop, members))
                    return false;
            return true;
        }

        /** @brief Action envelopes and policies have closed typed domains. */
        bool Policy(const GraphActionDefinition &action) noexcept {
            if (action.blendIn.ticks < 0 || action.blendOut.ticks < 0 || action.blendIn.ticks > AnimationClipHardLimits::DurationTicks ||
                action.blendOut.ticks > AnimationClipHardLimits::DurationTicks)
                return false;
            return action.curve <= GraphStateBlendCurve::EaseOut && action.interruption < GraphActionInterruption::Count &&
                   action.reverse < GraphActionReverse::Count && action.missingJoint < GraphMissingJointPolicy::Count;
        }

        /** @brief A slot must participate in actual compiled authoring, not an
         * unconsumed side catalog. */
        bool SlotReferenced(const AnimationGraphData &data, const GraphActionSlotId slot) {
            for (const auto &definition : data.definitions)
                for (const auto &node : definition.nodes)
                    if (const auto *value = std::get_if<GraphActionSlotNode>(&node.payload); value && value->slot == slot)
                        return true;
            return false;
        }
    }  // namespace

    namespace {
        struct ActionCounts final {
            std::uint64_t sections{};
            std::uint64_t loops{};
            std::uint64_t masks{};
        };

        /** @brief Whole detached catalog must fit before canonical copying. */
        template <typename Data> bool Structure(const Data &data, const AnimationGraphLimits &limits) noexcept {
            return Limits(limits) && data.actionSlots.size() <= limits.actionSlots && data.actions.size() <= limits.actions;
        }

        /** @brief Charges one immutable action's retained nested storage and metadata names. */
        bool CountAction(const GraphActionDefinition &action, ActionCounts &counts, const AnimationGraphLimits &limits) {
            counts.sections += action.sequence.sections.size();
            counts.loops += action.sequence.loops.size();
            counts.masks += action.mask.joints.size();
            return counts.sections <= limits.actionSections && counts.loops <= limits.actionLoops && counts.masks <= limits.poseTransforms;
        }

        /** @brief Identifier metadata failures are malformed data, not capacity failures. */
        bool ActionNames(const GraphActionDefinition &action) {
            return Name(action.name) && std::ranges::all_of(action.sequence.sections, [](const auto &section) {
                return Name(section.name);
            });
        }

        /** @brief Slot names/IDs must correspond to an actual graph consumer. */
        bool Slots(const AnimationGraphData &data) {
            std::set<std::string, std::less<>> names;
            for (const auto &slot : data.actionSlots)
                if (!names.insert(slot.name).second || !SlotReferenced(data, slot.id))
                    return false;
            return true;
        }

        /** @brief Separates immutable policy validation from namespace errors for stable diagnostics. */
        bool Definition(const GraphActionDefinition &action, const AnimationGraphData &data) {
            return Policy(action) && ValidGraphBoneMask(action.mask) && Sequence(action, data);
        }
    }  // namespace

    /** @brief Reuses one action storage admission for source and cooked declarations. */
    template <typename Data> Result<void> PreflightActionTables(const Data &data, const AnimationGraphCompileContext &context) {
        if (!Structure(data, context.limits))
            return Result<void>::Failure(Failure(GraphLimitExceeded));
        ActionCounts counts;
        for (const auto &action : data.actions) {
            if (const auto admission = Admission(context); admission.HasError())
                return admission;
            if (!CountAction(action, counts, context.limits))
                return Result<void>::Failure(Failure(GraphLimitExceeded));
            if (!ActionNames(action))
                return Result<void>::Failure(Failure(GraphMalformed));
        }
        for (const auto &slot : data.actionSlots)
            if (!Name(slot.name))
                return Result<void>::Failure(Failure(GraphMalformed));
        return Result<void>::Success();
    }

    /** @copydoc PreflightActions */
    Result<void> PreflightActions(const AnimationGraphData &data, const AnimationGraphCompileContext &context) {
        return PreflightActionTables(data, context);
    }

    /** @copydoc PreflightActions */
    Result<void> PreflightActions(const AnimationGraphCookedData &data, const AnimationGraphCompileContext &context) {
        return PreflightActionTables(data, context);
    }

    /** @copydoc ValidateActions */
    Result<void> ValidateActions(const AnimationGraphData &data) {
        if (!UniqueIds(data.actionSlots) || !UniqueIds(data.actions))
            return Result<void>::Failure(Failure(GraphMalformed));
        if (!Slots(data))
            return Result<void>::Failure(Failure(GraphBindingMismatch));
        std::set<std::string, std::less<>> names;
        for (const auto &action : data.actions) {
            if (!names.insert(action.name).second || !Find(data.actionSlots, action.slot))
                return Result<void>::Failure(Failure(GraphBindingMismatch));
            if (!Definition(action, data))
                return Result<void>::Failure(Failure(GraphMalformed));
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Animation::GraphCompileDetail
