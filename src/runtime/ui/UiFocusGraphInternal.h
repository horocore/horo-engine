#pragma once

#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiFocusGraph.h"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::Runtime::Ui {
    namespace FocusGraphDetail {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] inline Result<void> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<void>::Failure(MakeError(descriptor));
        }

        template <typename Enum> [[nodiscard]] bool IsKnown(const Enum value, const Enum count) noexcept {
            return static_cast<std::underlying_type_t<Enum>>(value) < static_cast<std::underlying_type_t<Enum>>(count);
        }

        [[nodiscard]] inline std::optional<std::size_t> DirectionIndex(const UiNavigationDirection direction) noexcept {
            using enum UiNavigationDirection;
            switch (direction) {
                case Next:
                    return 0;
                case Previous:
                    return 1;
                case Up:
                    return 2;
                case Down:
                    return 3;
                case Left:
                    return 4;
                case Right:
                    return 5;
                case Submit:
                case Cancel:
                case Count:
                    return std::nullopt;
            }
            return std::nullopt;
        }

        [[nodiscard]] inline const UiFocusOwnerContext &InvalidOwner() noexcept {
            static const UiFocusOwnerContext invalid{};
            return invalid;
        }

        [[nodiscard]] inline bool SameStaticScope(const UiFocusOwnerContext &left, const UiFocusOwnerContext &right) noexcept {
            return left.instance == right.instance && left.canvas == right.canvas && left.document == right.document &&
                   left.scope == right.scope;
        }
    }  // namespace FocusGraphDetail

    struct UiFocusGraph::Storage final {
        static constexpr std::size_t InvalidIndex = std::numeric_limits<std::size_t>::max();

        struct Node final {
            UiFocusNodeDescriptor descriptor;
        };

        struct RestorationEntry final {
            UiElementId focused;
            std::array<UiElementId, MaximumUiFocusGraphDepth> ancestors{};
            std::uint32_t ancestorCount{};
        };

        struct ModalSlot final {
            UiElementId root;
            UiElementHandle rootHandle;
            UiElementId defaultFocus;
            UiFocusModalScopePolicy policy{UiFocusModalScopePolicy::InclusiveTrap};
            std::uint32_t generation{1};
        };

        explicit Storage(const UiFocusGraphDescriptor &value) : descriptor(value) {
            nodes.reserve(value.nodeCapacity);
            modalSlots.resize(value.modalCapacity);
            restorations.resize(value.restorationCapacity);
        }

        [[nodiscard]] std::size_t FindNode(const UiElementId id) const noexcept {
            if (!id.IsValid())
                return InvalidIndex;
            for (std::size_t index = 0; index < nodes.size(); ++index) {
                if (nodes[index].descriptor.id == id)
                    return index;
            }
            return InvalidIndex;
        }

        [[nodiscard]] std::size_t FindNode(const UiElementHandle handle) const noexcept {
            if (!handle.IsValid())
                return InvalidIndex;
            for (std::size_t index = 0; index < nodes.size(); ++index) {
                if (nodes[index].descriptor.element == handle)
                    return index;
            }
            return InvalidIndex;
        }

        [[nodiscard]] bool IsWithinModal(const std::size_t index) const noexcept {
            if (modalDepth == 0)
                return true;
            if (index >= nodes.size())
                return false;

            const UiElementId root = modalSlots[modalDepth - 1].root;
            std::size_t current = index;
            for (std::uint32_t depth = 0; depth < MaximumUiFocusGraphDepth; ++depth) {
                const UiFocusNodeDescriptor &node = nodes[current].descriptor;
                if (node.id == root)
                    return true;
                if (!node.parent.IsValid())
                    return false;
                current = FindNode(node.parent);
                if (current == InvalidIndex)
                    return false;
            }
            return false;
        }

        [[nodiscard]] bool IsAllowed(const std::size_t index) const noexcept {
            if (index >= nodes.size() || !IsWithinModal(index))
                return false;
            const UiFocusNodeDescriptor &node = nodes[index].descriptor;
            return node.focusable && node.enabled && node.visible;
        }

        [[nodiscard]] std::optional<UiFocusTarget> CurrentTarget() const noexcept {
            if (!focusedIndex.has_value() || *focusedIndex >= nodes.size())
                return std::nullopt;
            const UiFocusNodeDescriptor &node = nodes[*focusedIndex].descriptor;
            return UiFocusTarget{node.id, node.element};
        }

        [[nodiscard]] std::optional<std::size_t> ResolveAllowed(const UiElementId id) const noexcept {
            const std::size_t index = FindNode(id);
            if (index == InvalidIndex || !IsAllowed(index))
                return std::nullopt;
            return index;
        }

        [[nodiscard]] std::optional<std::size_t> FirstAllowed() const noexcept {
            for (std::size_t index = 0; index < nodes.size(); ++index) {
                if (IsAllowed(index))
                    return index;
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<std::size_t> ActiveModalDefault() const noexcept {
            if (modalDepth == 0)
                return std::nullopt;
            return ResolveAllowed(modalSlots[modalDepth - 1].defaultFocus);
        }

        [[nodiscard]] std::optional<std::size_t> ResolveInitial() const noexcept {
            if (const auto modalDefault = ActiveModalDefault(); modalDefault.has_value())
                return modalDefault;
            if (const auto graphDefault = ResolveAllowed(descriptor.defaultFocus); graphDefault.has_value())
                return graphDefault;
            return FirstAllowed();
        }

        [[nodiscard]] std::optional<std::size_t> ResolveRestoration(const RestorationEntry &entry) const noexcept {
            if (const auto saved = ResolveAllowed(entry.focused); saved.has_value())
                return saved;

            if (descriptor.recovery == UiFocusRecoveryPolicy::AncestorThenDefaultThenFirst) {
                for (std::uint32_t index = 0; index < entry.ancestorCount; ++index) {
                    if (const auto ancestor = ResolveAllowed(entry.ancestors[index]); ancestor.has_value())
                        return ancestor;
                }
            }

            if (descriptor.recovery != UiFocusRecoveryPolicy::FirstFocusable && descriptor.recovery != UiFocusRecoveryPolicy::Clear) {
                if (const auto modalDefault = ActiveModalDefault(); modalDefault.has_value())
                    return modalDefault;
                if (const auto graphDefault = ResolveAllowed(descriptor.defaultFocus); graphDefault.has_value())
                    return graphDefault;
            }
            if (descriptor.recovery != UiFocusRecoveryPolicy::Clear)
                return FirstAllowed();
            return std::nullopt;
        }

        [[nodiscard]] std::optional<std::size_t> ResolveReload(const RestorationEntry &entry) const noexcept {
            if (const auto saved = ResolveAllowed(entry.focused); saved.has_value())
                return saved;
            if (descriptor.recovery == UiFocusRecoveryPolicy::AncestorThenDefaultThenFirst) {
                for (std::uint32_t index = 0; index < entry.ancestorCount; ++index) {
                    if (const auto ancestor = ResolveAllowed(entry.ancestors[index]); ancestor.has_value())
                        return ancestor;
                }
            }
            return ResolveInitial();
        }

        void CollectPath(const std::optional<std::size_t> index, RestorationEntry &output) const noexcept {
            output = {};
            if (!index.has_value() || *index >= nodes.size())
                return;

            std::size_t current = *index;
            output.focused = nodes[current].descriptor.id;
            for (std::uint32_t depth = 0; depth < MaximumUiFocusGraphDepth; ++depth) {
                const UiElementId parent = nodes[current].descriptor.parent;
                if (!parent.IsValid() || output.ancestorCount >= output.ancestors.size())
                    return;
                output.ancestors[output.ancestorCount++] = parent;
                current = FindNode(parent);
                if (current == InvalidIndex)
                    return;
            }
        }

        [[nodiscard]] UiFocusChange BuildChange(const std::optional<UiFocusTarget> &previous, const UiFocusChangeReason reason) const {
            UiFocusChange result;
            result.reason = reason;
            result.previous = previous;
            result.current = CurrentTarget();

            if (!previous.has_value() && !result.current.has_value())
                result.kind = UiFocusChangeKind::NoTarget;
            else if (!result.current.has_value())
                result.kind = UiFocusChangeKind::FocusCleared;
            else if (!previous.has_value())
                result.kind = UiFocusChangeKind::FocusMoved;
            else if (*previous == *result.current)
                result.kind = UiFocusChangeKind::Unchanged;
            else if (reason == UiFocusChangeReason::InvalidTarget || reason == UiFocusChangeReason::ModalClosed ||
                     reason == UiFocusChangeReason::Reload)
                result.kind = UiFocusChangeKind::FocusRecovered;
            else
                result.kind = UiFocusChangeKind::FocusMoved;

            if (result.current.has_value() && result.kind != UiFocusChangeKind::Unchanged && result.kind != UiFocusChangeKind::NoTarget) {
                const std::size_t index = *focusedIndex;
                const UiFocusBringIntoViewPolicy policy = nodes[index].descriptor.bringIntoView;
                if (policy != UiFocusBringIntoViewPolicy::None)
                    result.bringIntoView = UiFocusBringIntoViewRequest{descriptor.owner, *result.current, policy};
            }
            return result;
        }

        [[nodiscard]] UiFocusChange NoTarget(const UiFocusChangeReason reason) const {
            const auto current = CurrentTarget();
            return UiFocusChange{UiFocusChangeKind::NoTarget, reason, current, current, std::nullopt};
        }

        [[nodiscard]] static std::size_t FindCandidateNode(const std::vector<Node> &candidate, const UiElementId id) noexcept {
            for (std::size_t index = 0; index < candidate.size(); ++index) {
                if (candidate[index].descriptor.id == id)
                    return index;
            }
            return InvalidIndex;
        }

        [[nodiscard]] static Result<std::vector<Node>> CopyNodes(const UiFocusGraphDescriptor &candidateDescriptor,
                                                                 const std::span<const UiFocusNodeDescriptor> input) {
            std::vector<Node> candidate;
            candidate.reserve(candidateDescriptor.nodeCapacity);
            for (const UiFocusNodeDescriptor &node : input) {
                if (!node.IsValid())
                    return FocusGraphDetail::Failure<std::vector<Node>>(UiErrors::FocusInvalid);
                if (!node.element.IsValid() || node.element.ownership != candidateDescriptor.owner.instance.ownership)
                    return FocusGraphDetail::Failure<std::vector<Node>>(UiErrors::FocusSourceStale);
                if (FindCandidateNode(candidate, node.id) != InvalidIndex || FindNodeByHandle(candidate, node.element) != InvalidIndex)
                    return FocusGraphDetail::Failure<std::vector<Node>>(UiErrors::FocusInvalid);
                candidate.push_back(Node{node});
            }
            return Result<std::vector<Node>>::Success(std::move(candidate));
        }

        [[nodiscard]] static std::size_t FindNodeByHandle(const std::vector<Node> &candidate, const UiElementHandle handle) noexcept {
            for (std::size_t index = 0; index < candidate.size(); ++index) {
                if (candidate[index].descriptor.element == handle)
                    return index;
            }
            return InvalidIndex;
        }

        [[nodiscard]] static bool HasValidParentChain(const std::vector<Node> &candidate, const std::size_t index) noexcept {
            std::size_t current = index;
            for (std::uint32_t depth = 0; depth < MaximumUiFocusGraphDepth; ++depth) {
                const UiElementId parent = candidate[current].descriptor.parent;
                if (!parent.IsValid())
                    return true;
                current = FindCandidateNode(candidate, parent);
                if (current == InvalidIndex)
                    return false;
            }
            return false;
        }

        [[nodiscard]] static Result<void> ValidateTopology(const std::vector<Node> &candidate) {
            std::size_t roots = 0;
            for (std::size_t index = 0; index < candidate.size(); ++index) {
                if (!candidate[index].descriptor.parent.IsValid())
                    ++roots;
                if (!HasValidParentChain(candidate, index))
                    return FocusGraphDetail::Failure(UiErrors::FocusInvalid);
            }
            return roots == 1 ? Result<void>::Success() : FocusGraphDetail::Failure(UiErrors::FocusInvalid);
        }

        [[nodiscard]] Result<std::vector<Node>> BuildCandidate(const UiFocusGraphDescriptor &candidateDescriptor,
                                                               const std::span<const UiFocusNodeDescriptor> input) const {
            if (input.empty() || input.size() > candidateDescriptor.nodeCapacity)
                return FocusGraphDetail::Failure<std::vector<Node>>(UiErrors::FocusCapacityExceeded);
            auto candidate = CopyNodes(candidateDescriptor, input);
            if (candidate.HasError())
                return Result<std::vector<Node>>::Failure(candidate.ErrorValue());
            const auto topology = ValidateTopology(candidate.Value());
            if (topology.HasError())
                return Result<std::vector<Node>>::Failure(topology.ErrorValue());
            return candidate;
        }

        UiFocusGraphDescriptor descriptor;
        std::vector<Node> nodes;
        std::vector<ModalSlot> modalSlots;
        std::vector<RestorationEntry> restorations;
        std::optional<std::size_t> focusedIndex;
        std::uint32_t modalDepth{};
        std::uint32_t restorationDepth{};
        UiFocusGraphState lifecycle{UiFocusGraphState::Active};
    };
}  // namespace Horo::Runtime::Ui
