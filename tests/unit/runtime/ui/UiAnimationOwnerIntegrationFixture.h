#pragma once

/** @file UiAnimationOwnerIntegrationFixture.h
 * @brief Shared real host, provider and immutable-frame fixtures for animation integration tests.
 */
#include "Horo/Runtime/RuntimeHost.h"
#include "Horo/Runtime/UiAnimationRuntimeParticipant.h"
#include "UiBindingWriteTestFixture.h"
#include "UiHotReloadTestFixture.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <memory>
#include <optional>
#include <string>

namespace Horo::Runtime::Ui::AnimationTests {
    using ReloadTests::Generation;
    using ReloadTests::Instance;
    using ReloadTests::Revision;
    using ReloadTests::Stable;
    using ReloadTests::Text;

    struct WriteAttachment final {
        UiBindingStore *store{};
        const UiElementTree *tree{};
        std::shared_ptr<BindingWriteTests::ProviderState> state;
        std::shared_ptr<BindingWriteTests::TypedAuthority> authority;
    };

    inline void AttachWrites(UiReloadGeneration &generation, WriteAttachment &result) {
        auto *canvas = generation.Canvas(Stable<UiCanvasId>(2));
        auto schema = BindingWriteTests::Fixture::MakeSchema();
        const UiBindingProviderInstanceId provider{ReloadTests::Owner(), 9, 1};
        const std::array initial{UiBindingPropertyUpdate{1, std::string{"base"}}};
        const std::array registrations{UiBindingProviderRegistration{provider, UiBindingProviderScopeKind::Player, &schema,
                                                                     Revision<UiBindingSnapshotRevision>(1), initial}};
        const auto &property = schema.Properties()[1];
        const std::array bindings{
            UiResolvedBindingDescriptor{provider,
                                        {.id = Stable<UiBindingId>(11),
                                         .source = {schema.Type(), property.id, {1, 0}, property.signatureFingerprint},
                                         .target = {Stable<UiElementId>(11), UiBindingTargetProperty::Text, {.maximumBytes = 64}},
                                         .direction = UiBindingDirection::TwoWay},
                                        {}}};
        auto store = UiBindingStore::Create(canvas->tree, registrations, bindings);
        REQUIRE(store.HasValue());
        canvas->bindings.emplace(std::move(store).Value());
        result.state = std::make_shared<BindingWriteTests::ProviderState>();
        result.state->values[1] = Text("base");
        result.authority =
            std::make_shared<BindingWriteTests::TypedAuthority>(UiBindingWriteFence{provider, UiBindingProviderScopeKind::Player,
                                                                                    schema.Version(), 1, property.signatureFingerprint,
                                                                                    101},
                                                                result.state);
        const std::array admissions{UiBindingWriteAdmission{Stable<UiBindingId>(11), canvas->controls.front().control.Owner(),
                                                            Stable<UiActionId>(8), UiBindingCommitTrigger::Submit,
                                                            UiBindingConflictPolicy::RejectStale, result.authority}};
        REQUIRE(canvas->bindings->AdmitWrites(canvas->tree, admissions).HasValue());
        result.store = &*canvas->bindings;
        result.tree = &canvas->tree;
    }

    struct AnimationResources final {
        RuntimeStyleRegistry registry;
        UiStyleResolver styles;
        UiAnimationCanvasDefinition definition;
    };

    /** @brief Binds finite enter and exit definitions to the actual fixture route subtree. */
    inline void AppendRouteAnimations(UiAnimationCanvasDefinition &definition, const std::int64_t deadline) {
        auto enter = definition.animations.front();
        enter.id = Stable<UiAnimationId>(2);
        enter.time.domain = UiTimeDomain::ScreenTransition;
        enter.time.lifecycle = UiAnimationLifecycle::RequiredEnter;
        auto exit = enter;
        exit.id = Stable<UiAnimationId>(3);
        exit.time.lifecycle = UiAnimationLifecycle::RequiredExit;
        definition.animations.push_back(std::move(enter));
        definition.animations.push_back(std::move(exit));
        definition.routes.push_back(
            {Stable<UiRouteId>(9), Stable<UiElementId>(10), Stable<UiAnimationId>(2), Stable<UiAnimationId>(3), UiDuration{deadline}});
    }

    inline AnimationResources Resources(const UiReloadCanvas &canvas, const bool routeMotion = false,
                                        const std::int64_t deadline = 1000000000) {
        UiStyleRegistryDefinition schema;
        schema.properties.push_back({.id = Stable<UiStylePropertyId>(1),
                                     .category = UiStyleValueCategory::Dimension,
                                     .defaultValue = UiStyleDimension{300},
                                     .effects = {.measure = true, .paint = true},
                                     .maximumScalar = 1000});
        schema.assets.push_back({.id = Stable<RuntimeStyleAssetId>(1)});
        auto registry = RuntimeStyleRegistry::Create(std::move(schema), Revision<RuntimeStyleGeneration>(1));
        REQUIRE(registry.HasValue());
        // Three retained owner frames need one additional style slot for a prepared candidate.
        auto styles = UiStyleResolver::Create({Instance(), canvas.tree.Canvas(), canvas.tree.SourceDocument(), 8, 1, 8, 4,
                                               Revision<RuntimeStyleGeneration>(1), Revision<UiStylePublicationRevision>(1)});
        REQUIRE(styles.HasValue());
        UiAnimationCanvasDefinition definition;
        definition.canvas = canvas.id;
        definition.content = Revision<UiStyleContentRevision>(1);
        definition.resolvedPolicy = Revision<UiStylePolicyRevision>(1);
        for (const auto id : {Stable<UiElementId>(10), Stable<UiElementId>(11)}) {
            UiAnimationElementDefinition element;
            element.element = id;
            element.asset = Stable<RuntimeStyleAssetId>(1);
            element.layout.width = UiLength::Dip(100);
            element.layout.height = UiLength::Dip(id == Stable<UiElementId>(10) ? 100 : 300);
            definition.elements.push_back(std::move(element));
        }
        definition.layoutBindings.push_back({Stable<UiElementId>(11), Stable<UiStylePropertyId>(1), UiAnimationLayoutField::Height});
        UiAnimationDefinition motion;
        motion.id = Stable<UiAnimationId>(1);
        motion.time.duration = UiDuration{100000000};
        motion.time.domain = UiTimeDomain::PresentationUnscaled;
        motion.resolvedMotionPolicy = definition.resolvedPolicy;
        motion.tracks.push_back({Stable<UiElementId>(11),
                                 Stable<UiStylePropertyId>(1),
                                 {{0, UiStyleDimension{300}}, {std::numeric_limits<std::uint32_t>::max(), UiStyleDimension{150}}}});
        definition.animations.push_back(std::move(motion));
        if (routeMotion)
            AppendRouteAnimations(definition, deadline);
        return {std::move(registry).Value(), std::move(styles).Value(), std::move(definition)};
    }

    inline UiAnimationOwner OwnerAggregate(UiElementSlotAllocator &allocator, const bool routeMotion = false,
                                           const std::int64_t deadline = 1000000000, UiActionRouter **attachedActions = nullptr,
                                           WriteAttachment *writes = nullptr, UiActionRouter **routeActions = nullptr,
                                           const std::uint64_t version = 1, UiFocusGraph **attachedFocus = nullptr) {
        auto initial = Generation(allocator, version, 32, false, 0, 0, 4);
        const auto &canvas = initial.Canvases().front();
        if (attachedFocus)
            *attachedFocus = initial.Canvas(canvas.id)->focus ? &*initial.Canvas(canvas.id)->focus : nullptr;
        if (writes)
            AttachWrites(initial, *writes);
        if (attachedActions) {
            auto *owned = initial.Canvas(canvas.id);
            auto actions = UiActionRouter::Create({owned->controls.front().control.Owner(), 8});
            REQUIRE(actions.HasValue());
            owned->actions.emplace(std::move(actions).Value());
            *attachedActions = &*owned->actions;
        }
        auto *routeStack = routeActions ? &*initial.Canvas(canvas.id)->routes : nullptr;
        const auto routeOwner = canvas.controls.front().control.Owner();
        auto resources = Resources(canvas, routeMotion, deadline);
        auto owner = UiAnimationOwner::Create(std::move(initial), allocator, std::move(resources.registry), std::move(resources.styles),
                                              std::move(resources.definition));
        REQUIRE(owner.HasValue());
        if (routeStack) {
            // Initial-generation admission requires an empty stack; attach the real route owner after admission.
            auto pushed = routeStack->Push(Stable<UiRouteId>(9));
            REQUIRE(pushed.HasValue());
            REQUIRE(pushed.Value().IsCommitted());
            auto router = UiActionRouter::Create({routeOwner, 8});
            REQUIRE(router.HasValue());
            REQUIRE(routeStack->AttachActions(*pushed.Value().route, std::move(router).Value()).HasValue());
            *routeActions = routeStack->Actions(*pushed.Value().route);
        }
        return std::move(owner).Value();
    }

    inline UiAnimationRuntimeConfig Config() {
        UiAnimationRuntimeConfig config;
        config.viewport = {{{0, 0}, {100, 100}},
                           {{0, 0}, {100, 100}},
                           {},
                           Revision<UiLayoutIntrinsicRevision>(1),
                           Revision<UiLayoutCanvasRevision>(1),
                           Revision<UiLayoutPolicyRevision>(1)};
        return config;
    }

    /** @brief Explicit load-time choices for the real host and owned UI integration fixture. */
    struct HostOptions final {
        bool routeMotion{};
        std::int64_t deadline{1000000000};
        bool actions{};
        bool writes{};
        bool routeActions{};
        UiElementSlotAllocator *allocator{};
        std::uint64_t version{1};
        UiAnimationApplication application{UiAnimationApplication::Runtime};
        bool focusAccess{}; /**< Test-only quiescent borrow; never exposed by a production owner contract. */
    };

    /** @brief Owns the actual lifecycle host, animation participant and source issuer. */
    class HostFixture final {
    public:
        explicit HostFixture(const HostOptions &options = {}) {
            auto created = RuntimeHost::Create(clock);
            REQUIRE(created.HasValue());
            host = std::move(created).Value();
            auto *externalIssuer = options.allocator;
            std::optional<UiElementSlotAllocator> localIssuer;
            if (!externalIssuer) {
                auto issued = UiElementSlotAllocator::Create(ReloadTests::Owner());
                REQUIRE(issued.HasValue());
                localIssuer.emplace(std::move(issued).Value());
                externalIssuer = &*localIssuer;
            }
            auto config = Config();
            config.application = options.application;
            auto composed =
                UiAnimationRuntimeParticipant::Compose(OwnerAggregate(*externalIssuer, options.routeMotion, options.deadline,
                                                                      options.actions ? &attachedActions : nullptr,
                                                                      options.writes ? &attachedWrites : nullptr,
                                                                      options.routeActions ? &attachedRouteActions : nullptr,
                                                                      options.version, options.focusAccess ? &attachedFocus : nullptr),
                                                       host->DispatchSource(), config);
            REQUIRE(composed.HasValue());
            auto composition = std::move(composed).Value();
            participant = composition.participant.get();
            controller = std::move(composition.controller);
            REQUIRE(host->AddParticipant(std::move(composition.participant)).HasValue());
            REQUIRE(host->Startup().HasValue());
        }

        DeterministicClock clock;
        std::unique_ptr<RuntimeHost> host;
        UiAnimationRuntimeParticipant *participant{};
        UiAnimationClockController controller;
        UiActionRouter *attachedActions{};
        UiActionRouter *attachedRouteActions{};
        WriteAttachment attachedWrites;
        UiFocusGraph *attachedFocus{};
    };

    inline UiControlInput Edge(const UiActionSource &source, const UiControlInputKind kind, const std::uint64_t sequence,
                               const std::string_view text = {}) {
        return {source, kind, UiControlActivationSource::Keyboard, sequence, 0, UiControlAdjustment::Count, Text(text)};
    }

    inline UiAnimationFrameLease Frame(UiAnimationRuntimeParticipant &participant) {
        auto acquired = participant.Acquire();
        REQUIRE(acquired.HasValue());
        return std::move(acquired).Value();
    }

    inline void CheckGeometry(const UiAnimationFrameLease &frame, const std::int32_t maximum) {
        REQUIRE(frame.Clipping() != nullptr);
        CHECK(frame.Clipping()->Descriptor().interaction == frame.Layout().Descriptor().interaction);
        CHECK(frame.Clipping()->Descriptor().sources == frame.Layout().Descriptor().sources);
        REQUIRE(frame.Clipping()->Scrolls().size() == 1);
        CHECK(frame.Clipping()->Scrolls().front().maximumOffset.y == maximum);
    }

    inline UiPresentationReceipt Receipt(const UiAnimationFrameLease &frame, const std::uint64_t snapshot) {
        return {{ReloadTests::Owner(), 10, 1},           frame.Layout().Descriptor().canvas,
                frame.Layout().Descriptor().interaction, Revision<UiRenderSnapshotRevision>(snapshot),
                UiPresentationOutcome::Presented,        UiPresentationReason::None};
    }

    class RetainedAnimationHandler final : public UiAsyncActionHandler {
    public:
        Result<void> Start(const UiActionRequest &request, UiAsyncActionProducer pending) override {
            actual = request;
            producer.emplace(std::move(pending));
            return Result<void>::Success();
        }

        UiActionRequest actual;
        std::optional<UiAsyncActionProducer> producer;
    };
}  // namespace Horo::Runtime::Ui::AnimationTests
