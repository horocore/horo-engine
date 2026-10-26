#pragma once

#include "Horo/Runtime/Ui/RuntimeUiService.h"
#include "UiHotReloadTestFixture.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Tests::UiAccess {
    using namespace Runtime::Ui;
    using namespace Runtime::Ui::ReloadTests;

    template <class T> T Take(Result<T> result) {
        INFO((result.HasError() ? result.ErrorValue().message : ""));
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    inline constexpr auto Lifecycle = UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands;
    inline constexpr auto All = RuntimeUiPermission::Elements | RuntimeUiPermission::Screens | RuntimeUiPermission::ReadBindings |
                                RuntimeUiPermission::PublishBindings | RuntimeUiPermission::Actions | RuntimeUiPermission::Events;

    /** @brief Real cooked/Assets-loaded canvas, canonical semantic registry and actual action/tree/route owners. */
    struct Fixture final {
        UiElementSlotAllocator slots{Take(UiElementSlotAllocator::Create(Owner()))};
        RuntimeUiService service{Take(RuntimeUiService::Create(Take(UiSceneReconciliation::Create({Owner()}))))};
        UiActionOwnerContext actionOwner;
        bool bindingsEnabled{};
        bool canonicalElements{};
        UiBindingProviderSchema schema{MakeSchema()};

        static UiBindingProviderInstanceId Provider() {
            return {Owner(), 7, 1};
        }

        static UiBindingProviderSchema MakeSchema() {
            const std::array properties{UiBindingPropertyDescriptor{.id = Take(UiBindingPropertyId::Parse("label")),
                                                                    .type = UiBindingValueType::BoundedText,
                                                                    .limits = {.maximumBytes = 128, .maximumElements = 1}}};
            UiBindingProviderDescriptor descriptor{Take(UiBindingProviderTypeId::Parse("game.tests.ui")),
                                                   ModuleId{"game.tests"},
                                                   {1, 0, 0},
                                                   properties,
                                                   UiBindingProviderScopeBit(UiBindingProviderScopeKind::GameInstance)};
            descriptor.schema.fingerprint = ComputeUiBindingSchemaFingerprint(descriptor);
            return Take(UiBindingProviderSchema::Create(descriptor));
        }

        /** @brief Admits the actual initial owner, optionally starting its real scroll state before publication. */
        explicit Fixture(bool sceneOwned = false, bool bindings = false, bool catalogElements = false, UiLogicalPoint initialScroll = {})
            : bindingsEnabled(bindings), canonicalElements(catalogElements) {
            auto generation = Make(1);
            if (initialScroll != UiLogicalPoint{}) {
                auto &canvas = *generation.Canvas(Stable<UiCanvasId>(2));
                const auto root = Take(canvas.tree.Root()).handle;
                for (auto &policy : canvas.clipPolicies)
                    if (policy.element == root && policy.overflow == UiLayoutOverflowPolicy::Scroll)
                        policy.scrollOffset = initialScroll;
                canvas.clipped = Take(canvas.clipping->Update(canvas.tree, *canvas.layout, {canvas.clipPolicies, {}}));
            }
            auto publisher = Take(UiHotReload::Create(std::move(generation)));
            UiSceneInstanceDescriptor descriptor{Instance(), {}};
            if (sceneOwned) {
                descriptor.owner = {.kind = UiOwnerScopeKind::Scene, .scene = Runtime::SceneRuntimeId{101}};
            }
            REQUIRE(service.Admit(descriptor, std::move(publisher), Lifecycle).HasValue());
        }

        UiReloadGeneration Make(std::uint64_t version, std::uint64_t runtimeRevision = 0) {
            LayerOptions options;
            options.canonicalElements = canonicalElements;
            options.runtimeRevision = runtimeRevision;
            auto generation = Generation(slots, version, 32, false, 0, 0, canonicalElements ? 4 : 3, options);
            auto &canvas = *generation.Canvas(Stable<UiCanvasId>(2));
            REQUIRE(canvas.layout.has_value());
            actionOwner = {canvas.tree.Instance(),       canvas.tree.Canvas(),
                           canvas.tree.SourceDocument(), canvas.tree.SourceDocumentRevision(),
                           canvas.tree.Revision(),       canvas.layout->Descriptor().interaction};
            canvas.actions.emplace(Take(UiActionRouter::Create({actionOwner, 128, {}})));
            REQUIRE(canvas.actions->Owner() == actionOwner);
            for (const auto &control : canvas.controls)
                REQUIRE(control.control.Owner() == actionOwner);
            if (bindingsEnabled) {
                const std::array initial{UiBindingPropertyUpdate{0, std::string{"initial"}}};
                const std::array registrations{UiBindingProviderRegistration{Provider(), UiBindingProviderScopeKind::GameInstance, &schema,
                                                                             Revision<UiBindingSnapshotRevision>(1), initial}};
                const auto &property = schema.Properties()[0];
                const std::array bindings{
                    UiResolvedBindingDescriptor{Provider(),
                                                {.id = Stable<UiBindingId>(30),
                                                 .source = {schema.Type(), property.id, {1, 0}, property.signatureFingerprint},
                                                 .target = {Stable<UiElementId>(11),
                                                            UiBindingTargetProperty::Text,
                                                            {.maximumBytes = 128}}}}};
                canvas.bindings.emplace(Take(UiBindingStore::Create(canvas.tree, registrations, bindings)));
            }
            return generation;
        }

        RuntimeUiGrant Grant(std::uint64_t scene = 101, RuntimeUiPermission permission = All) const {
            return {"game.tests",
                    scene,
                    1,
                    Instance(),
                    Stable<UiCanvasId>(2),
                    permission,
                    bindingsEnabled ? std::vector{Provider()} : std::vector<UiBindingProviderInstanceId>{}};
        }

        std::shared_ptr<const RuntimeUiClient> Client(RuntimeUiPermission permission = All) {
            return Take(service.Issue(Grant(101, permission)));
        }
    };

    struct Actions final : UiActionHandler {
        std::size_t calls{};

        Result<UiActionResult> Handle(const UiActionRequest &request) override {
            ++calls;
            return UiActionResult::Handled(request.id);
        }
    };

    struct Events final : UiEventHandler {
        std::size_t calls{};

        Result<UiEventResponse> Handle(UiElementHandle, UiEventPhase, const UiRoutedEvent &) override {
            ++calls;
            return Result<UiEventResponse>::Success({true, false, false});
        }

        Result<void> ApplyDefault(UiElementHandle, const UiRoutedEvent &) override {
            return Result<void>::Success();
        }
    };
}  // namespace Horo::Tests::UiAccess
