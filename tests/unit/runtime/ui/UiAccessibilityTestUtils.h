#pragma once
#include "Horo/Runtime/Ui/UiAccessibilityChanges.h"
#include "Horo/Runtime/Ui/UiErrors.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <utility>

// The executable's allocation override and both translation units share this counter.
inline std::atomic<std::size_t> accessibilityAllocations{};

namespace Horo::Runtime::Ui {
    namespace {
        template <typename Identity> Identity Stable(const std::uint8_t marker) {
            SerializedUiId bytes{};
            bytes.back() = marker;
            return Identity::Create(bytes).Value();
        }

        UiOwnershipGeneration Owner(const std::uint64_t value = 17) {
            return UiOwnershipGeneration::Create(value).Value();
        }

        UiElementTree MakeTree(const UiOwnershipGeneration owner = Owner(), const std::uint64_t documentRevision = 3,
                               UiElementSlotAllocator *ownerSlots = nullptr) {
            std::optional<UiElementSlotAllocator> allocator;
            if (!ownerSlots) {
                auto allocatorResult = UiElementSlotAllocator::Create(owner);
                REQUIRE(allocatorResult.HasValue());
                allocator.emplace(std::move(allocatorResult).Value());
                ownerSlots = &*allocator;
            }
            const UiElementTreeDescriptor descriptor{.instance = {owner, 1, 1},
                                                     .canvas = {owner, 2, 1},
                                                     .document = Stable<UiDocumentId>(1),
                                                     .documentRevision = UiDocumentRevision::Create(documentRevision).Value(),
                                                     .treeRevision = UiRuntimeTreeRevision::Create(4).Value(),
                                                     .limits = {8, 8, 8}};
            const std::array elements{UiElementDescriptor{Stable<UiElementId>(1), {}},
                                      UiElementDescriptor{Stable<UiElementId>(2), Stable<UiElementId>(1)},
                                      UiElementDescriptor{Stable<UiElementId>(3), Stable<UiElementId>(1)},
                                      UiElementDescriptor{Stable<UiElementId>(5), Stable<UiElementId>(1)},
                                      UiElementDescriptor{Stable<UiElementId>(4), Stable<UiElementId>(5)}};
            auto treeResult = UiElementTree::Create(*ownerSlots, descriptor, elements);
            REQUIRE(treeResult.HasValue());
            return std::move(treeResult).Value();
        }

        UiAccessibilityLimits Limits() {
            return {8, 16, 16, 4096};
        }

        UiAccessibilityExtractor MakeExtractor(const UiElementTree &tree, const std::uint32_t snapshots = 2) {
            auto result = UiAccessibilityExtractor::Create({tree.Instance(), tree.Canvas(), tree.SourceDocument(), Limits(), snapshots});
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        UiAccessibilitySnapshotDescriptor Descriptor(const UiElementTree &tree, const std::uint64_t semanticRevision = 1) {
            return {.instance = tree.Instance(),
                    .canvas = tree.Canvas(),
                    .document = tree.SourceDocument(),
                    .documentRevision = tree.SourceDocumentRevision(),
                    .treeRevision = tree.Revision(),
                    .interactionRevision = UiInteractionRevision::Create(7).Value(),
                    .semanticRevision = UiAccessibilitySemanticRevision::Create(semanticRevision).Value(),
                    .limits = Limits()};
        }

        UiAccessibilityActionId Action(const std::uint32_t value) {
            return UiAccessibilityActionId::Create(value).Value();
        }

        UiAccessibilityContributorId Contributor(const std::uint64_t value) {
            return UiAccessibilityContributorId::Create(value).Value();
        }

        struct ProjectionFixture final {
            std::array<UiAccessibilityRelationInput, 1> sliderRelations{
                UiAccessibilityRelationInput{UiAccessibilityRelationKind::LabelledBy, Stable<UiElementId>(2)}};
            std::array<UiAccessibilityActionInput, 3> sliderActions{UiAccessibilityActionInput{Action(1),
                                                                                               UiAccessibilityActionKind::Increment,
                                                                                               UiAccessibilityActionValueKind::None,
                                                                                               {"Increase", {}}},
                                                                    UiAccessibilityActionInput{Action(2),
                                                                                               UiAccessibilityActionKind::Decrement,
                                                                                               UiAccessibilityActionValueKind::None,
                                                                                               {"Decrease", {}}},
                                                                    UiAccessibilityActionInput{Action(3),
                                                                                               UiAccessibilityActionKind::SetValue,
                                                                                               UiAccessibilityActionValueKind::Number,
                                                                                               {"Set", {}}}};
            std::array<UiAccessibilityActionInput, 1> contributedActions{UiAccessibilityActionInput{Action(4),
                                                                                                    UiAccessibilityActionKind::Activate,
                                                                                                    UiAccessibilityActionValueKind::None,
                                                                                                    {"Open", {}}}};
            std::array<UiAccessibilityNodeInput, 4> nodes{};

            ProjectionFixture() {
                nodes[0].element = Stable<UiElementId>(1);
                nodes[0].role = UiAccessibilityRole::Screen;
                nodes[0].name = {"Main menu", UiAccessibilityTextSource::ResolvedMessage};
                nodes[0].state = UiAccessibilityState{UiAccessibilityStateFlag::Modal};
                nodes[0].bounds = UiLogicalRect{{0, 0}, {640, 360}};

                nodes[1].element = Stable<UiElementId>(2);
                nodes[1].role = UiAccessibilityRole::StaticText;
                nodes[1].name = {"Volume", UiAccessibilityTextSource::ResolvedMessage};
                nodes[1].bounds = UiLogicalRect{{0, 0}, {100, 32}};

                nodes[2].element = Stable<UiElementId>(3);
                nodes[2].role = UiAccessibilityRole::Slider;
                nodes[2].name = {"Volume", UiAccessibilityTextSource::ResolvedMessage};
                nodes[2].value = {UiAccessibilityValueKind::Number, false, 0, 0.5, {}};
                nodes[2].state = UiAccessibilityState{UiAccessibilityStateFlag::Focusable};
                nodes[2].hasRange = true;
                nodes[2].range = {0.0, 1.0, 0.5, 0.1};
                nodes[2].relations = sliderRelations;
                nodes[2].actions = sliderActions;
                nodes[2].bounds = UiLogicalRect{{0, 40}, {320, 32}};

                nodes[3].element = Stable<UiElementId>(4);
                nodes[3].role = UiAccessibilityRole::Button;
                nodes[3].source = UiAccessibilityControlSource::Contributed;
                nodes[3].contributor = Contributor(9);
                nodes[3].name = {"Open inventory", UiAccessibilityTextSource::UserContent};
                nodes[3].actions = contributedActions;
                nodes[3].bounds = UiLogicalRect{{0, 80}, {160, 32}};
            }

            UiAccessibilityProjection View() const noexcept {
                return {nodes};
            }
        };

        template <typename Value> void RequireError(const Result<Value> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == expected.code.Value());
        }

        UiAccessibilityChangePublisher MakePublisher(const UiElementTree &tree, const UiAccessibilityChangeLimits limits = {}) {
            auto result =
                UiAccessibilityChangePublisher::Create({tree.Instance(), tree.Canvas(), tree.SourceDocument(), Limits(), 2}, limits);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

    }  // namespace
}  // namespace Horo::Runtime::Ui
