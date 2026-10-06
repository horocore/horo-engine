#include "Horo/Runtime/RuntimeHost.h"
#include "Horo/Runtime/UiAnimationRuntimeParticipant.h"
#include "UiBindingWriteTestFixture.h"
#include "UiHotReloadTestFixture.h"
#include "support/AllocationProbe.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;
    using namespace Horo::Runtime::Ui;
    using namespace Horo::Runtime::Ui::ReloadTests;

    struct WriteAttachment final {
        UiBindingStore *store{};
        const UiElementTree *tree{};
        std::shared_ptr<BindingWriteTests::ProviderState> state;
        std::shared_ptr<BindingWriteTests::TypedAuthority> authority;
    };

    void AttachWrites(UiReloadGeneration &generation, WriteAttachment &result) {
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

    AnimationResources Resources(const UiReloadCanvas &canvas, const bool routeMotion = false, const std::int64_t deadline = 1000000000) {
        UiStyleRegistryDefinition schema;
        schema.properties.push_back({.id = Stable<UiStylePropertyId>(1),
                                     .category = UiStyleValueCategory::Dimension,
                                     .defaultValue = UiStyleDimension{300},
                                     .effects = {.measure = true, .paint = true},
                                     .maximumScalar = 1000});
        schema.assets.push_back({.id = Stable<RuntimeStyleAssetId>(1)});
        auto registry = RuntimeStyleRegistry::Create(std::move(schema), Revision<RuntimeStyleGeneration>(1));
        REQUIRE(registry.HasValue());
        auto styles = UiStyleResolver::Create({Instance(), canvas.tree.Canvas(), canvas.tree.SourceDocument(), 8, 1, 8, 3,
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
        if (routeMotion) {
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
        return {std::move(registry).Value(), std::move(styles).Value(), std::move(definition)};
    }

    UiAnimationOwner OwnerAggregate(UiElementSlotAllocator &allocator, const bool routeMotion = false,
                                    const std::int64_t deadline = 1000000000, UiActionRouter **attachedActions = nullptr,
                                    WriteAttachment *writes = nullptr, UiActionRouter **routeActions = nullptr,
                                    const std::uint64_t version = 1) {
        auto initial = Generation(allocator, version);
        const auto &canvas = initial.Canvases().front();
        if (writes)
            AttachWrites(initial, *writes);
        if (attachedActions) {
            auto *owned = initial.Canvas(canvas.id);
            auto actions = UiActionRouter::Create({owned->controls.front().control.Owner(), 8});
            REQUIRE(actions.HasValue());
            owned->actions.emplace(std::move(actions).Value());
            *attachedActions = &*owned->actions;
        }
        if (routeActions) {
            auto *owned = initial.Canvas(canvas.id);
            auto pushed = owned->routes->Push(Stable<UiRouteId>(9));
            REQUIRE(pushed.HasValue());
            REQUIRE(pushed.Value().IsCommitted());
            auto router = UiActionRouter::Create({owned->controls.front().control.Owner(), 8});
            REQUIRE(router.HasValue());
            REQUIRE(owned->routes->AttachActions(*pushed.Value().route, std::move(router).Value()).HasValue());
            *routeActions = owned->routes->Actions(*pushed.Value().route);
        }
        auto resources = Resources(canvas, routeMotion, deadline);
        auto owner = UiAnimationOwner::Create(std::move(initial), allocator, std::move(resources.registry), std::move(resources.styles),
                                              std::move(resources.definition));
        REQUIRE(owner.HasValue());
        return std::move(owner).Value();
    }

    UiAnimationRuntimeConfig Config() {
        UiAnimationRuntimeConfig config;
        config.viewport = {{{0, 0}, {100, 100}},
                           {{0, 0}, {100, 100}},
                           {},
                           Revision<UiLayoutIntrinsicRevision>(1),
                           Revision<UiLayoutCanvasRevision>(1),
                           Revision<UiLayoutPolicyRevision>(1)};
        return config;
    }

    class HostFixture final {
    public:
        explicit HostFixture(const bool routeMotion = false, const std::int64_t deadline = 1000000000, const bool actions = false,
                             const bool writes = false, const bool routeActions = false, UiElementSlotAllocator *externalIssuer = nullptr,
                             const std::uint64_t version = 1, const UiAnimationApplication application = UiAnimationApplication::Runtime) {
            auto created = RuntimeHost::Create(clock);
            REQUIRE(created.HasValue());
            host = std::move(created).Value();
            std::optional<UiElementSlotAllocator> localIssuer;
            if (!externalIssuer) {
                auto issued = UiElementSlotAllocator::Create(ReloadTests::Owner());
                REQUIRE(issued.HasValue());
                localIssuer.emplace(std::move(issued).Value());
                externalIssuer = &*localIssuer;
            }
            auto config = Config();
            config.application = application;
            auto composed = UiAnimationRuntimeParticipant::Compose(OwnerAggregate(*externalIssuer, routeMotion, deadline,
                                                                                  actions ? &attachedActions : nullptr,
                                                                                  writes ? &attachedWrites : nullptr,
                                                                                  routeActions ? &attachedRouteActions : nullptr, version),
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
    };

    UiControlInput Edge(const UiActionSource &source, const UiControlInputKind kind, const std::uint64_t sequence,
                        const std::string_view text = {}) {
        return {source, kind, UiControlActivationSource::Keyboard, sequence, 0, UiControlAdjustment::Count, Text(text)};
    }

    UiAnimationFrameLease Frame(UiAnimationRuntimeParticipant &participant) {
        auto acquired = participant.Acquire();
        REQUIRE(acquired.HasValue());
        return std::move(acquired).Value();
    }

    void CheckGeometry(const UiAnimationFrameLease &frame, const std::int32_t maximum) {
        REQUIRE(frame.Clipping() != nullptr);
        CHECK(frame.Clipping()->Descriptor().interaction == frame.Layout().Descriptor().interaction);
        CHECK(frame.Clipping()->Descriptor().sources == frame.Layout().Descriptor().sources);
        REQUIRE(frame.Clipping()->Scrolls().size() == 1);
        CHECK(frame.Clipping()->Scrolls().front().maximumOffset.y == maximum);
    }

    UiPresentationReceipt Receipt(const UiAnimationFrameLease &frame, const std::uint64_t snapshot) {
        return {{ReloadTests::Owner(), 10, 1},           frame.Layout().Descriptor().canvas,
                frame.Layout().Descriptor().interaction, Revision<UiRenderSnapshotRevision>(snapshot),
                UiPresentationOutcome::Presented,        UiPresentationReason::None};
    }
}  // namespace

TEST_CASE("Real host animation publishes matching style layout and clipping while retained geometry remains immutable",
          "[runtime_ui][animation][integration]") {
    HostFixture fixture;
    auto &clock = fixture.clock;
    auto &host = fixture.host;
    auto *participant = fixture.participant;
    REQUIRE(participant->Start(Stable<UiAnimationId>(1)).HasValue());
    REQUIRE(host->RunFrame().HasValue());
    auto first = Frame(*participant);
    CheckGeometry(first, 200);
    auto receipt = Receipt(first, 1);
    CHECK_FALSE(participant->InputEligible(receipt.view));
    REQUIRE(participant->ApplyPresentation(receipt).HasValue());
    CHECK(participant->InputEligible(receipt.view));
    clock.Advance(Duration::FromMilliseconds(100));
    REQUIRE(host->RunFrame().HasValue());
    auto next = Frame(*participant);
    CheckGeometry(next, 50);
    CHECK_FALSE(participant->InputEligible(receipt.view));
    CHECK(participant->ApplyPresentation(receipt).HasError());
    CHECK_FALSE(participant->InputEligible(receipt.view));
    receipt = Receipt(next, 2);
    REQUIRE(participant->ApplyPresentation(receipt).HasValue());
    CHECK(participant->InputEligible(receipt.view));
    CheckGeometry(first, 200);
    CHECK(first.Layout().Descriptor().interaction != next.Layout().Descriptor().interaction);
    host->Shutdown();
    CHECK(first.Clipping()->Scrolls().front().maximumOffset.y == 200);
    CHECK(next.Clipping()->Scrolls().front().maximumOffset.y == 50);
    CHECK(participant->Acquire().HasError());
}

TEST_CASE("Actual presented control replacement preserves draft Cancel baseline and rejects previous frame sources",
          "[runtime_ui][animation][integration][controls]") {
    HostFixture fixture;
    auto *participant = fixture.participant;
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto old = Frame(*participant);
    REQUIRE(old.Controls().size() == 1);
    const auto oldSource = old.Controls().front().source;
    auto receipt = Receipt(old, 1);
    CHECK(participant->HandleControl(receipt.view, Edge(oldSource, UiControlInputKind::FocusGained, 1)).HasError());
    REQUIRE(participant->ApplyPresentation(receipt).HasValue());
    REQUIRE(participant->HandleControl(receipt.view, Edge(oldSource, UiControlInputKind::FocusGained, 1)).HasValue());
    REQUIRE(participant->HandleControl(receipt.view, Edge(oldSource, UiControlInputKind::TextInput, 2, "draft")).HasValue());
    CHECK(std::get<UiTextInputControlState>(old.Controls().front().state).text.View() == "base");
    fixture.clock.Advance(Duration::FromMilliseconds(10));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto current = Frame(*participant);
    REQUIRE(current.Controls().size() == 1);
    const auto currentSource = current.Controls().front().source;
    CHECK(currentSource.owner.interaction == current.Layout().Descriptor().interaction);
    CHECK(currentSource.owner != oldSource.owner);
    CHECK(std::get<UiTextInputControlState>(current.Controls().front().state).text.View() == "basedraft");
    CHECK(participant->HandleControl(receipt.view, Edge(currentSource, UiControlInputKind::Cancel, 3)).HasError());
    receipt = Receipt(current, 2);
    REQUIRE(participant->ApplyPresentation(receipt).HasValue());
    CHECK(participant->HandleControl(receipt.view, Edge(oldSource, UiControlInputKind::Cancel, 3)).HasError());
    const auto cancelled = participant->HandleControl(receipt.view, Edge(currentSource, UiControlInputKind::Cancel, 3));
    REQUIRE(cancelled.HasValue());
    CHECK(std::get<UiTextInputControlState>(cancelled.Value().state).text.View() == "base");
    CHECK(std::get<UiTextInputControlState>(current.Controls().front().state).text.View() == "basedraft");
}

TEST_CASE("Real host animation preserves capture on stable geometry and cancels it before changed geometry becomes eligible",
          "[runtime_ui][animation][integration][capture]") {
    HostFixture fixture;
    auto &participant = *fixture.participant;
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto first = Frame(participant);
    REQUIRE_FALSE(first.Controls().empty());
    const auto receipt = Receipt(first, 1);
    const auto &layout = first.Layout().Descriptor();
    UiPointerCaptureRequest request{{ReloadTests::Owner(), 11, 1},
                                    UiPointerId::Create(1).Value(),
                                    UiPointerButton::Primary,
                                    receipt.view,
                                    {layout.instance,
                                     layout.canvas,
                                     layout.document,
                                     layout.sources.tree,
                                     layout.interaction,
                                     first.Controls().front().source.element,
                                     {}}};
    CHECK(participant.CapturePointer(request).HasError());
    REQUIRE(participant.ApplyPresentation(receipt).HasValue());
    auto admitted = participant.CapturePointer(request);
    REQUIRE(admitted.HasValue());
    auto token = std::move(admitted).Value();
    REQUIRE(token.IsActive());
    fixture.clock.Advance(Duration::FromMilliseconds(10));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto stable = Frame(participant);
    CHECK(stable.Styles().Descriptor().geometry == first.Styles().Descriptor().geometry);
    CHECK(stable.Layout().Descriptor().interaction == layout.interaction);
    CHECK(participant.InputEligible(receipt.view));
    CHECK(token.IsActive());
    REQUIRE(participant.Start(Stable<UiAnimationId>(1)).HasValue());
    fixture.clock.Advance(Duration::FromMilliseconds(10));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto changed = Frame(participant);
    CHECK(changed.Layout().Descriptor().interaction != layout.interaction);
    CHECK_FALSE(token.IsActive());
    CHECK(token.CancellationReason() == UiPointerCaptureCancellationReason::InteractionRevisionLost);
    CHECK_FALSE(participant.InputEligible(receipt.view));
    CHECK(participant.CapturePointer(request).HasError());
    REQUIRE(participant.ApplyPresentation(Receipt(changed, 2)).HasValue());
    CHECK(participant.InputEligible(receipt.view));
    CHECK(participant.CapturePointer(request).HasError());
}

TEST_CASE("Stable host animation frames preserve an active press and its pending default source",
          "[runtime_ui][animation][integration][input]") {
    HostFixture fixture;
    auto &participant = *fixture.participant;
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto first = Frame(participant);
    REQUIRE_FALSE(first.Controls().empty());
    const auto source = first.Controls().front().source;
    const auto receipt = Receipt(first, 1);
    REQUIRE(participant.ApplyPresentation(receipt).HasValue());
    REQUIRE(participant.HandleControl(receipt.view, Edge(source, UiControlInputKind::FocusGained, 1)).HasValue());
    auto pressed = participant.HandleControl(receipt.view, Edge(source, UiControlInputKind::SubmitPress, 2));
    REQUIRE(pressed.HasValue());
    REQUIRE(std::get<UiTextInputControlState>(pressed.Value().state).pressed);
    fixture.clock.Advance(Duration::FromMilliseconds(10));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto held = Frame(participant);
    REQUIRE(held.Controls().front().source == source);
    REQUIRE(std::get<UiTextInputControlState>(held.Controls().front().state).pressed);
    REQUIRE(held.Layout().Descriptor().interaction == first.Layout().Descriptor().interaction);
    REQUIRE(participant.InputEligible(receipt.view));
    auto released = participant.HandleControl(receipt.view, Edge(source, UiControlInputKind::SubmitRelease, 3));
    REQUIRE(released.HasValue());
    REQUIRE(released.Value().defaultActionPending);
    fixture.clock.Advance(Duration::FromMilliseconds(10));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto pending = Frame(participant);
    REQUIRE(pending.Controls().front().source == source);
    REQUIRE(pending.Layout().Descriptor().interaction == first.Layout().Descriptor().interaction);
    auto applied = participant.ApplyControlDefault(receipt.view, source);
    REQUIRE(applied.HasValue());
    REQUIRE(applied.Value().has_value());
    CHECK(applied.Value()->source == source);
    CHECK(applied.Value()->eventSequence == 3);
    CHECK(applied.Value()->kind == UiControlActionKind::Submit);
    CHECK(std::get<UiTextInputControlState>(held.Controls().front().state).pressed);
    CHECK_FALSE(participant.ApplyControlDefault(receipt.view, source).Value().has_value());
}

TEST_CASE("Real host navigation gates stack publication on its privately scoped enter and exit timelines",
          "[runtime_ui][animation][integration][routes]") {
    HostFixture fixture{true};
    auto &participant = *fixture.participant;
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto initial = Frame(participant);
    REQUIRE(initial.Routes().empty());
    CHECK(participant.Start(Stable<UiAnimationId>(2)).HasError());
    auto admitted = participant.Navigate(UiRouteOperationRequest::Push(Stable<UiRouteId>(9)));
    REQUIRE(admitted.HasValue());
    const auto operation = admitted.Value();
    CHECK(participant.Navigate(UiRouteOperationRequest::Push(Stable<UiRouteId>(9))).HasError());
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto entering = Frame(participant);
    REQUIRE(entering.RouteOperation().has_value());
    CHECK(entering.RouteOperation()->operation == operation);
    CHECK(entering.RouteOperation()->phase == UiAnimationRoutePhase::Entering);
    CHECK_FALSE(entering.RouteOperation()->terminal.has_value());
    CHECK(entering.Routes().empty());
    const auto child = entering.Clocks().domains[static_cast<std::size_t>(UiTimeDomain::ScreenTransition)];
    CHECK(child.available);
    CHECK(child.route.IsValid());
    CHECK(child.routeOperation == operation.sequence);
    REQUIRE(participant.ApplyPresentation(Receipt(entering, 1)).HasValue());
    CHECK_FALSE(participant.InputEligible(Receipt(entering, 1).view));
    entering = {};
    fixture.clock.Advance(Duration::FromMilliseconds(100));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto entered = Frame(participant);
    REQUIRE(entered.RouteOperation()->terminal.has_value());
    CHECK(entered.RouteOperation()->terminal->outcome == UiRouteOperationOutcome::Committed);
    REQUIRE(entered.Routes().size() == 1);
    const auto route = entered.Routes().front().id;
    CHECK(route == child.route);
    CHECK(initial.Routes().empty());
    CHECK_FALSE(participant.InputEligible(Receipt(entered, 2).view));
    REQUIRE(participant.ApplyPresentation(Receipt(entered, 2)).HasValue());
    CHECK(participant.InputEligible(Receipt(entered, 2).view));
    initial = {};
    auto exiting = participant.Navigate(UiRouteOperationRequest::Pop());
    REQUIRE(exiting.HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto pending = Frame(participant);
    CHECK(pending.RouteOperation()->phase == UiAnimationRoutePhase::Exiting);
    CHECK(pending.Routes().front().id == route);
    CHECK(pending.Clocks().domains[static_cast<std::size_t>(UiTimeDomain::ScreenTransition)].clock != child.clock);
    CHECK(pending.Clocks().domains[static_cast<std::size_t>(UiTimeDomain::ScreenTransition)].route == route);
    pending = {};
    fixture.clock.Advance(Duration::FromMilliseconds(100));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto removed = Frame(participant);
    CHECK(removed.Routes().empty());
    CHECK(removed.RouteOperation()->phase == UiAnimationRoutePhase::Completed);
    CHECK(entered.Routes().front().id == route);
    CHECK(participant.CancelNavigation(operation, UiAnimationCancellation::Explicit).HasError());
}

TEST_CASE("Actual route cancellation keeps the prior stack and rejects late or foreign operation identities",
          "[runtime_ui][animation][integration][routes][cancel]") {
    HostFixture fixture{true};
    auto &participant = *fixture.participant;
    REQUIRE(fixture.host->RunFrame().HasValue());
    const auto operation = participant.Navigate(UiRouteOperationRequest::Push(Stable<UiRouteId>(9)));
    REQUIRE(operation.HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto pending = Frame(participant);
    auto foreign = operation.Value();
    foreign.ownership = UiOwnershipGeneration::Create(ReloadTests::Owner().Value() + 1).Value();
    CHECK(participant.CancelNavigation(foreign, UiAnimationCancellation::Explicit).HasError());
    REQUIRE(participant.CancelNavigation(operation.Value(), UiAnimationCancellation::Explicit).HasValue());
    REQUIRE(participant.CancelNavigation(operation.Value(), UiAnimationCancellation::Explicit).HasValue());
    fixture.clock.Advance(Duration::FromMilliseconds(10));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto cancelled = Frame(participant);
    REQUIRE(cancelled.RouteOperation()->terminal.has_value());
    CHECK(cancelled.RouteOperation()->phase == UiAnimationRoutePhase::Cancelled);
    CHECK(cancelled.RouteOperation()->cancellation == UiAnimationCancellation::Explicit);
    CHECK(cancelled.RouteOperation()->terminal->rejection == UiRouteOperationRejection::Cancelled);
    CHECK(cancelled.Routes().empty());
    CHECK_FALSE(pending.RouteOperation()->terminal.has_value());
    CHECK(participant.CancelNavigation(operation.Value(), UiAnimationCancellation::Explicit).HasError());
    pending = {};
    CHECK(participant.Navigate(UiRouteOperationRequest::Push(Stable<UiRouteId>(9))).HasValue());
}

TEST_CASE("The real child-clock deadline cancels an incomplete required route without publishing its instance",
          "[runtime_ui][animation][integration][routes][deadline]") {
    HostFixture fixture{true, 20000000};
    auto &participant = *fixture.participant;
    REQUIRE(fixture.host->RunFrame().HasValue());
    REQUIRE(participant.Navigate(UiRouteOperationRequest::Push(Stable<UiRouteId>(9))).HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    fixture.clock.Advance(Duration::FromMilliseconds(20));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto cancelled = Frame(participant);
    REQUIRE(cancelled.RouteOperation()->terminal.has_value());
    CHECK(cancelled.RouteOperation()->cancellation == UiAnimationCancellation::Deadline);
    CHECK(cancelled.RouteOperation()->phase == UiAnimationRoutePhase::Cancelled);
    CHECK(cancelled.Routes().empty());
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto stable = Frame(participant);
    CHECK_FALSE(stable.Clocks().domains[static_cast<std::size_t>(UiTimeDomain::ScreenTransition)].available);
    CHECK(stable.RouteOperation()->terminal->operation == cancelled.RouteOperation()->terminal->operation);
    CHECK(stable.RouteOperation()->terminal->revision == cancelled.RouteOperation()->terminal->revision);
    CHECK(stable.RouteOperation()->terminal->rejection == cancelled.RouteOperation()->terminal->rejection);
}

TEST_CASE("Shutdown revokes a real pending gate while its immutable frame and scoped child evidence stay pinned",
          "[runtime_ui][animation][integration][routes][lifecycle]") {
    HostFixture fixture{true};
    auto &participant = *fixture.participant;
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto operation = participant.Navigate(UiRouteOperationRequest::Push(Stable<UiRouteId>(9)));
    REQUIRE(operation.HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto retained = Frame(participant);
    const auto child = retained.Clocks().domains[static_cast<std::size_t>(UiTimeDomain::ScreenTransition)];
    fixture.host->Shutdown();
    CHECK(participant.Acquire().HasError());
    CHECK(participant.Navigate(UiRouteOperationRequest::Push(Stable<UiRouteId>(9))).HasError());
    CHECK(participant.CancelNavigation(operation.Value(), UiAnimationCancellation::Explicit).HasError());
    CHECK(retained.Clocks().domains[static_cast<std::size_t>(UiTimeDomain::ScreenTransition)].clock == child.clock);
    CHECK(retained.RouteOperation()->phase == UiAnimationRoutePhase::Entering);
    CHECK_FALSE(retained.RouteOperation()->terminal.has_value());
}

namespace {
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
}  // namespace

TEST_CASE("A real attached async router remains live on stable geometry and blocks replacement until its producer drains",
          "[runtime_ui][animation][integration][async][rollback]") {
    HostFixture fixture{false, 1000000000, true};
    auto &participant = *fixture.participant;
    REQUIRE(fixture.attachedActions);
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto initial = Frame(participant);
    const auto source = initial.Controls().front().source;
    REQUIRE(participant.ApplyPresentation(Receipt(initial, 1)).HasValue());
    auto &router = *fixture.attachedActions;
    REQUIRE(router.Enqueue(source, UiButtonActionCommand{Stable<UiActionId>(8), {}}).HasValue());
    RetainedAnimationHandler handler;
    auto dispatched = router.DispatchNext(handler);
    REQUIRE(dispatched.HasValue());
    REQUIRE(dispatched.Value().has_value());
    REQUIRE(handler.producer.has_value());
    CHECK(handler.actual.source == source);
    fixture.clock.Advance(Duration::FromMilliseconds(10));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto stable = Frame(participant);
    CHECK(stable.Controls().front().source == source);
    CHECK(router.Owner() == source.owner);
    CHECK(participant.InputEligible(Receipt(initial, 1).view));
    REQUIRE(participant.Start(Stable<UiAnimationId>(1)).HasValue());
    fixture.clock.Advance(Duration::FromMilliseconds(10));
    const auto failed = fixture.host->RunFrame();
    REQUIRE(failed.HasError());
    // RuntimeHost's real fatal policy shuts down; the last immutable publication is nevertheless unchanged and readable.
    CHECK(stable.Styles().Descriptor().geometry == initial.Styles().Descriptor().geometry);
    CHECK(stable.Layout().Descriptor().interaction == initial.Layout().Descriptor().interaction);
    CHECK(handler.producer->IsCancellationRequested());
    CHECK(participant.Acquire().HasError());
    handler.producer.reset();
}

TEST_CASE("Actual frame and route publication use preallocated owners without C++ allocation or reclamation",
          "[runtime_ui][animation][integration][allocation][routes]") {
    HostFixture fixture{true};
    auto &participant = *fixture.participant;
    REQUIRE(fixture.host->RunFrame().HasValue());
    REQUIRE(participant.Navigate(UiRouteOperationRequest::Push(Stable<UiRouteId>(9))).HasValue());
    const auto allocations = Horo::Tests::AllocationProbe::Count();
    const auto frees = Horo::Tests::AllocationProbe::FreeCount();
    const auto prepared = fixture.host->RunFrame();
    const auto afterPrepare = Horo::Tests::AllocationProbe::Count();
    const auto afterPrepareFree = Horo::Tests::AllocationProbe::FreeCount();
    REQUIRE(prepared.HasValue());
    CHECK(afterPrepare == allocations);
    CHECK(afterPrepareFree == frees);
    fixture.clock.Advance(Duration::FromMilliseconds(100));
    const auto completionAllocations = Horo::Tests::AllocationProbe::Count();
    const auto completionFrees = Horo::Tests::AllocationProbe::FreeCount();
    const auto completed = fixture.host->RunFrame();
    const auto afterCompletion = Horo::Tests::AllocationProbe::Count();
    const auto afterCompletionFree = Horo::Tests::AllocationProbe::FreeCount();
    REQUIRE(completed.HasValue());
    CHECK(afterCompletion == completionAllocations);
    CHECK(afterCompletionFree == completionFrees);
    const auto frame = Frame(participant);
    REQUIRE(frame.RouteOperation()->terminal.has_value());
    CHECK(frame.RouteOperation()->phase == UiAnimationRoutePhase::Completed);
}

TEST_CASE("Actual admitted pending writes survive stable frames and refuse an animated source replacement without provider callbacks",
          "[runtime_ui][animation][integration][write][rollback]") {
    HostFixture fixture{false, 1000000000, true, true};
    auto &participant = *fixture.participant;
    auto &attachment = fixture.attachedWrites;
    REQUIRE(attachment.store);
    REQUIRE(attachment.tree);
    REQUIRE(fixture.attachedActions);
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto initial = Frame(participant);
    const auto source = initial.Controls().front().source;
    REQUIRE(participant.ApplyPresentation(Receipt(initial, 1)).HasValue());
    auto edit = attachment.store->BeginEdit(*attachment.tree, Stable<UiBindingId>(11), source);
    REQUIRE(edit.HasValue());
    UiActionPayload payload;
    REQUIRE(payload.Add(Text("draft")).HasValue());
    REQUIRE(fixture.attachedActions->Enqueue(source, UiGameplayActionCommand{Stable<UiActionId>(8), payload}).HasValue());
    auto routed = fixture.attachedActions->TryDequeue();
    REQUIRE(routed.HasValue());
    REQUIRE(routed.Value().has_value());
    auto queued = attachment.store->QueueWrite(*attachment.tree, edit.Value(), *routed.Value(), UiBindingCommitTrigger::Submit);
    REQUIRE(queued.HasValue());
    REQUIRE(attachment.state->prepares == 0);
    fixture.clock.Advance(Duration::FromMilliseconds(10));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto stable = Frame(participant);
    REQUIRE(stable.Controls().front().source == source);
    CHECK(attachment.state->prepares == 0);
    CHECK(attachment.state->commits == 0);
    CHECK(attachment.state->abandons == 0);
    REQUIRE(participant.Start(Stable<UiAnimationId>(1)).HasValue());
    fixture.clock.Advance(Duration::FromMilliseconds(10));
    REQUIRE(fixture.host->RunFrame().HasError());
    CHECK(stable.Layout().Descriptor().interaction == initial.Layout().Descriptor().interaction);
    CHECK(stable.Styles().Descriptor().geometry == initial.Styles().Descriptor().geometry);
    CHECK(attachment.state->prepares == 0);
    CHECK(attachment.state->commits == 0);
    CHECK_FALSE(participant.InputEligible(Receipt(stable, 2).view));
}

TEST_CASE("Actual replacement progresses exit then enter with fresh child and route incarnations and no old-terminal replay",
          "[runtime_ui][animation][integration][routes][replacement]") {
    HostFixture fixture{true};
    auto &participant = *fixture.participant;
    REQUIRE(fixture.host->RunFrame().HasValue());
    REQUIRE(participant.Navigate(UiRouteOperationRequest::Push(Stable<UiRouteId>(9))).HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    fixture.clock.Advance(Duration::FromMilliseconds(100));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto original = Frame(participant);
    REQUIRE(original.Routes().size() == 1);
    const auto originalRoute = original.Routes().front().id;
    const auto originalChild = original.Clocks().domains[static_cast<std::size_t>(UiTimeDomain::ScreenTransition)].clock;
    REQUIRE_FALSE(original.Timelines().empty());
    const auto terminal = original.Timelines().front().timeline;
    auto replacement = participant.Navigate(UiRouteOperationRequest::Replace(Stable<UiRouteId>(9)));
    REQUIRE(replacement.HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto exiting = Frame(participant);
    REQUIRE(exiting.RouteOperation()->phase == UiAnimationRoutePhase::Exiting);
    CHECK(exiting.RouteOperation()->scope == originalRoute);
    CHECK(exiting.Clocks().domains[static_cast<std::size_t>(UiTimeDomain::ScreenTransition)].clock != originalChild);
    CHECK(participant.Cancel(terminal, UiAnimationCancellation::Explicit).HasError());
    exiting = {};
    fixture.clock.Advance(Duration::FromMilliseconds(100));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto staged = Frame(participant);
    CHECK(staged.RouteOperation()->phase == UiAnimationRoutePhase::Waiting);
    CHECK_FALSE(staged.Clocks().domains[static_cast<std::size_t>(UiTimeDomain::ScreenTransition)].available);
    CHECK(staged.Routes().front().id == originalRoute);
    CHECK(staged.RouteOperation()->scope != originalRoute);
    staged = {};
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto entering = Frame(participant);
    const auto enteringClock = entering.Clocks().domains[static_cast<std::size_t>(UiTimeDomain::ScreenTransition)].clock;
    CHECK(enteringClock != originalChild);
    CHECK(entering.Clocks().domains[static_cast<std::size_t>(UiTimeDomain::ScreenTransition)].route != originalRoute);
    entering = {};
    fixture.clock.Advance(Duration::FromMilliseconds(100));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto replaced = Frame(participant);
    CHECK(replaced.RouteOperation()->phase == UiAnimationRoutePhase::Completed);
    REQUIRE(replaced.Routes().size() == 1);
    CHECK(replaced.Routes().front().id != originalRoute);
    CHECK(original.Routes().front().id == originalRoute);
    CHECK(replaced.RouteOperation()->terminal->operation == replacement.Value());
}

TEST_CASE("The owner preserves actual stack admission rejection and never enables a child for an unknown catalog route",
          "[runtime_ui][animation][integration][routes][admission]") {
    HostFixture fixture{true};
    auto &participant = *fixture.participant;
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto refused = participant.Navigate(UiRouteOperationRequest::Push(Stable<UiRouteId>(99)));
    REQUIRE(refused.HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto rejected = Frame(participant);
    REQUIRE(rejected.RouteOperation()->terminal.has_value());
    CHECK(rejected.RouteOperation()->phase == UiAnimationRoutePhase::Rejected);
    CHECK(rejected.RouteOperation()->terminal->operation == refused.Value());
    CHECK(rejected.RouteOperation()->terminal->rejection == UiRouteOperationRejection::NotFound);
    CHECK(rejected.Routes().empty());
    CHECK_FALSE(rejected.Clocks().domains[static_cast<std::size_t>(UiTimeDomain::ScreenTransition)].available);
}

TEST_CASE("Actual drained route action owners retire without frame reclamation and reclaim only at explicit quiescence",
          "[runtime_ui][animation][integration][routes][actions][allocation]") {
    HostFixture fixture{true, 1000000000, false, false, true};
    auto &participant = *fixture.participant;
    REQUIRE(fixture.attachedRouteActions);
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto first = Frame(participant);
    const auto source = first.Controls().front().source;
    auto &router = *fixture.attachedRouteActions;
    REQUIRE(router.Enqueue(source, UiButtonActionCommand{Stable<UiActionId>(8), {}}).HasValue());
    RetainedAnimationHandler handler;
    REQUIRE(router.DispatchNext(handler).HasValue());
    REQUIRE(handler.producer.has_value());
    const auto key = handler.producer->Key();
    REQUIRE(handler.producer->Complete().HasValue());
    REQUIRE(router.AsyncActions()->Release(key).HasValue());
    handler.producer.reset();
    REQUIRE(router.LastIssuedSequence() == key.request.sequence);
    REQUIRE(participant.Navigate(UiRouteOperationRequest::Pop()).HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    first = {};
    fixture.clock.Advance(Duration::FromMilliseconds(100));
    const auto allocations = Horo::Tests::AllocationProbe::Count();
    const auto frees = Horo::Tests::AllocationProbe::FreeCount();
    const auto completed = fixture.host->RunFrame();
    const auto after = Horo::Tests::AllocationProbe::Count();
    const auto freed = Horo::Tests::AllocationProbe::FreeCount();
    REQUIRE(completed.HasValue());
    CHECK(after == allocations);
    CHECK(freed == frees);
    auto removed = Frame(participant);
    CHECK(removed.Routes().empty());
    CHECK(removed.RouteOperation()->phase == UiAnimationRoutePhase::Completed);
    auto reclaimed = participant.DrainRetired();
    REQUIRE(reclaimed.HasValue());
    CHECK(reclaimed.Value() >= 1);
    CHECK(removed.Routes().empty());
}

TEST_CASE("Real asset generation re-admission uses the same canvas issuer and rejects old animation and input incarnations",
          "[runtime_ui][animation][integration][reload][identity]") {
    auto issued = UiElementSlotAllocator::Create(ReloadTests::Owner());
    REQUIRE(issued.HasValue());
    auto allocator = std::move(issued).Value();
    HostFixture firstHost{false, 1000000000, false, false, false, &allocator};
    auto &original = *firstHost.participant;
    REQUIRE(firstHost.host->RunFrame().HasValue());
    auto retained = Frame(original);
    const auto oldSource = retained.Controls().front().source;
    auto oldTimeline = original.Start(Stable<UiAnimationId>(1));
    REQUIRE(oldTimeline.HasValue());
    const auto oldClock = retained.Clocks().domains[static_cast<std::size_t>(UiTimeDomain::PresentationUnscaled)].clock;
    firstHost.host->Shutdown();
    // Reload is an explicit application retirement/re-admission; this does not pretend a live host resumes after teardown.
    HostFixture replacement{false, 1000000000, false, false, false, &allocator, 2};
    auto &current = *replacement.participant;
    REQUIRE(replacement.host->RunFrame().HasValue());
    auto admitted = Frame(current);
    CHECK(admitted.Controls().front().source.element != oldSource.element);
    CHECK(admitted.Controls().front().source.owner.documentRevision == Revision<UiDocumentRevision>(2));
    CHECK(admitted.Clocks().domains[static_cast<std::size_t>(UiTimeDomain::PresentationUnscaled)].clock != oldClock);
    REQUIRE(current.ApplyPresentation(Receipt(admitted, 1)).HasValue());
    CHECK(current.HandleControl(Receipt(admitted, 1).view, Edge(oldSource, UiControlInputKind::FocusGained, 1)).HasError());
    CHECK(current.Cancel(oldTimeline.Value(), UiAnimationCancellation::Explicit).HasError());
    CHECK(retained.Controls().front().source == oldSource);
    CHECK(retained.Clocks().domains[static_cast<std::size_t>(UiTimeDomain::PresentationUnscaled)].clock == oldClock);
    auto fresh = current.Start(Stable<UiAnimationId>(1));
    REQUIRE(fresh.HasValue());
    CHECK(fresh.Value() != oldTimeline.Value());
    replacement.clock.Advance(Duration::FromMilliseconds(10));
    REQUIRE(replacement.host->RunFrame().HasValue());
}

TEST_CASE("Real animation reload reconciles compatible draft and invalidates source and controller handles",
          "[runtime_ui][animation][integration][reload]") {
    auto issued = UiElementSlotAllocator::Create(ReloadTests::Owner());
    REQUIRE(issued.HasValue());
    auto allocator = std::move(issued).Value();
    HostFixture fixture{false, 1000000000, false, false, false, &allocator, 1, UiAnimationApplication::Manual};
    auto &participant = *fixture.participant;
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto retained = Frame(participant);
    auto receipt = Receipt(retained, 1);
    REQUIRE(participant.ApplyPresentation(receipt).HasValue());
    const auto source = retained.Controls().front().source;
    REQUIRE(participant.HandleControl(receipt.view, Edge(source, UiControlInputKind::FocusGained, 1)).HasValue());
    REQUIRE(participant.HandleControl(receipt.view, Edge(source, UiControlInputKind::TextInput, 2, "draft")).HasValue());
    const auto oldClock = fixture.controller.Clock(UiTimeDomain::Manual);
    REQUIRE(oldClock.HasValue());
    auto replacement = Generation(allocator, 100);
    auto resources = Resources(replacement.Canvases().front());
    auto reloaded = participant.Reload(std::move(replacement), allocator, std::move(resources.registry), std::move(resources.styles),
                                       std::move(resources.definition), UiAnimationReloadPolicy::Cancel,
                                       UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands);
    REQUIRE(reloaded.HasValue());
    CHECK(reloaded.Value().result.state.preservedControls == 1);
    CHECK(fixture.controller.Step(oldClock.Value(), UiDuration{1}).HasError());
    CHECK(participant.Acquire().HasError());
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto current = Frame(participant);
    CHECK(current.Controls().front().source != source);
    CHECK(std::get<UiTextInputControlState>(current.Controls().front().state).text.View() == "basedraft");
    CHECK(std::get<UiTextInputControlState>(retained.Controls().front().state).text.View() == "base");
    REQUIRE(participant.ApplyPresentation(Receipt(current, 2)).HasValue());
    CHECK(participant.HandleControl(receipt.view, Edge(source, UiControlInputKind::Cancel, 3)).HasError());
    const auto cancelled = participant.HandleControl(receipt.view, Edge(current.Controls().front().source, UiControlInputKind::Cancel, 3));
    REQUIRE(cancelled.HasValue());
    CHECK(std::get<UiTextInputControlState>(cancelled.Value().state).text.View() == "base");
    const auto freshClock = reloaded.Value().controller.Clock(UiTimeDomain::Manual);
    REQUIRE(freshClock.HasValue());
    CHECK(freshClock.Value() != oldClock.Value());
}

TEST_CASE("Rejected and cancelled real animation reloads preserve the existing frame and cursor",
          "[runtime_ui][animation][integration][reload][rollback]") {
    auto issued = UiElementSlotAllocator::Create(ReloadTests::Owner());
    REQUIRE(issued.HasValue());
    auto allocator = std::move(issued).Value();
    HostFixture fixture{false, 1000000000, false, false, false, &allocator};
    auto &participant = *fixture.participant;
    auto timeline = participant.Start(Stable<UiAnimationId>(1));
    REQUIRE(timeline.HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto retained = Frame(participant);
    auto replacement = Generation(allocator, 100);
    auto resources = Resources(replacement.Canvases().front());
    CancellationSource cancelled;
    SECTION("actual load ancestry cancelled") {
        cancelled.RequestCancellation();
    }
    SECTION("authored replacement missing actual element") {
        resources.definition.elements.pop_back();
    }
    auto result = participant.Reload(std::move(replacement), allocator, std::move(resources.registry), std::move(resources.styles),
                                     std::move(resources.definition), UiAnimationReloadPolicy::Cancel,
                                     UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands, cancelled.Token());
    REQUIRE(result.HasError());
    auto unchanged = Frame(participant);
    CHECK(unchanged.Timelines().front().timeline == timeline.Value());
    CHECK(unchanged.Clocks().updateSequence == retained.Clocks().updateSequence);
    CHECK(unchanged.Layout().Descriptor().interaction == retained.Layout().Descriptor().interaction);
    fixture.clock.Advance(Duration::FromMilliseconds(100));
    REQUIRE(fixture.host->RunFrame().HasValue());
    CHECK(Frame(participant).Timelines().front().playback.outcome == UiAnimationOutcome::Completed);
}

TEST_CASE("Explicit real reload restart closes the old instance once and starts a fresh cursor",
          "[runtime_ui][animation][integration][reload][restart]") {
    auto issued = UiElementSlotAllocator::Create(ReloadTests::Owner());
    REQUIRE(issued.HasValue());
    auto allocator = std::move(issued).Value();
    HostFixture fixture{false, 1000000000, false, false, false, &allocator};
    auto &participant = *fixture.participant;
    auto timeline = participant.Start(Stable<UiAnimationId>(1));
    REQUIRE(timeline.HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    fixture.clock.Advance(Duration::FromMilliseconds(40));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto retained = Frame(participant);
    auto replacement = Generation(allocator, 100);
    auto resources = Resources(replacement.Canvases().front());
    auto result = participant.Reload(std::move(replacement), allocator, std::move(resources.registry), std::move(resources.styles),
                                     std::move(resources.definition), UiAnimationReloadPolicy::Restart,
                                     UiStructuralCommitPoint::CommitDeferredLifecycleChanges);
    REQUIRE(result.HasValue());
    REQUIRE(result.Value().result.terminal.size() == 1);
    CHECK(result.Value().result.terminal.front().timeline == timeline.Value());
    CHECK(result.Value().result.terminal.front().playback.cancellation == UiAnimationCancellation::Reload);
    CHECK(result.Value().result.terminal.front().playback.newTerminalOutcome);
    CHECK(result.Value().result.restartedTimelines == 1);
    CHECK(participant.Cancel(timeline.Value(), UiAnimationCancellation::Explicit).HasError());
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto fresh = Frame(participant);
    REQUIRE(fresh.Timelines().size() == 1);
    CHECK(fresh.Timelines().front().timeline != timeline.Value());
    CHECK(fresh.Timelines().front().playback.elapsed == UiDuration{});
    CHECK(retained.Timelines().front().playback.elapsed == UiDuration{40000000});
    fixture.clock.Advance(Duration::FromMilliseconds(100));
    REQUIRE(fixture.host->RunFrame().HasValue());
    CHECK(Frame(participant).Timelines().front().playback.outcome == UiAnimationOutcome::Completed);
}

TEST_CASE("A pending actual route gate rejects reload without cancelling its admitted transition",
          "[runtime_ui][animation][integration][reload][routes]") {
    auto issued = UiElementSlotAllocator::Create(ReloadTests::Owner());
    REQUIRE(issued.HasValue());
    auto allocator = std::move(issued).Value();
    HostFixture fixture{true, 1000000000, false, false, false, &allocator};
    auto &participant = *fixture.participant;
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto operation = participant.Navigate(UiRouteOperationRequest::Push(Stable<UiRouteId>(9)));
    REQUIRE(operation.HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto retained = Frame(participant);
    auto replacement = Generation(allocator, 100);
    auto resources = Resources(replacement.Canvases().front(), true);
    CHECK(participant
              .Reload(std::move(replacement), allocator, std::move(resources.registry), std::move(resources.styles),
                      std::move(resources.definition), UiAnimationReloadPolicy::Cancel,
                      UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands)
              .HasError());
    fixture.clock.Advance(Duration::FromMilliseconds(100));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto completed = Frame(participant);
    REQUIRE(completed.RouteOperation());
    CHECK(completed.RouteOperation()->operation == operation.Value());
    CHECK(completed.RouteOperation()->phase == UiAnimationRoutePhase::Completed);
    REQUIRE(retained.RouteOperation());
    CHECK(retained.RouteOperation()->phase != UiAnimationRoutePhase::Completed);
}
