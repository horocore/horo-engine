#include "Horo/Runtime/Ui/UiPointerInteraction.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <limits>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename Id> Id Identity() {
            return Id::Create(SerializedUiId{91}).Value();
        }

        template <typename Revision> Revision First() {
            return Revision::Create(1).Value();
        }

        class PointerLayout final : public UiLayoutEvaluator {
        public:
            Result<void> ResolveChildConstraints(const UiLayoutChildConstraintRequest &, std::span<UiLayoutConstraints>) const override {
                return Result<void>::Success();
            }

            Result<UiLayoutMeasurement> Measure(const UiLayoutMeasureRequest &) const override {
                return Result<UiLayoutMeasurement>::Success({{6400, 6400}, false, false});
            }

            Result<UiLayoutArrangement> Arrange(const UiLayoutArrangeRequest &request, std::span<UiLogicalRect>) const override {
                const auto rect = request.assignedContent;
                return Result<UiLayoutArrangement>::Success({rect, rect, rect, rect, rect, rect, NoUiBaseline});
            }
        };

        class GestureObserver final : public UiEventHandler {
        public:
            Result<UiEventResponse> Handle(UiElementHandle, const UiEventPhase phase, const UiRoutedEvent &event) override {
                if (phase != UiEventPhase::Target)
                    return Result<UiEventResponse>::Success({});
                const auto kind = event.gesture->kind;
                ++counts[static_cast<std::size_t>(kind)];
                if (interrupt == kind) {
                    if (shutdown)
                        interaction->Shutdown();
                    else if (retireTree)
                        tree->Shutdown();
                    else if (localCancel)
                        interaction->CancelTransient();
                    else {
                        const auto cancelled = captures->CancelContext(context, UiPointerCaptureCancellationReason::FocusLost);
                        REQUIRE(cancelled.HasValue());
                    }
                }
                return Result<UiEventResponse>::Success({false, false, prevented == kind});
            }

            Result<void> ApplyDefault(UiElementHandle, const UiRoutedEvent &) override {
                ++defaults;
                return Result<void>::Success();
            }

            std::array<std::uint32_t, static_cast<std::size_t>(UiGestureKind::Count)> counts{};
            std::uint32_t defaults{};
            UiGestureKind prevented{UiGestureKind::Count};
            UiGestureKind interrupt{UiGestureKind::Count};
            UiPointerInteraction *interaction{};
            UiPointerCaptureStore *captures{};
            UiElementTree *tree{};
            RuntimeUiInputContextId context;
            bool localCancel{};
            bool shutdown{};
            bool retireTree{};

            std::uint32_t Count(const UiGestureKind kind) const {
                return counts[static_cast<std::size_t>(kind)];
            }
        };

        struct PointerFixture {
            static UiLayoutEngine CreateLayout(const UiLayoutEngineDescriptor &descriptor) {
                auto created = UiLayoutEngine::Create(descriptor);
                if (created.HasError()) {
                    INFO("Pointer fixture layout admission: " << created.ErrorValue().code.Value() << ": " << created.ErrorValue().message);
                    REQUIRE(created.HasValue());
                }
                return std::move(created).Value();
            }

            UiOwnershipGeneration ownership{UiOwnershipGeneration::Create(91).Value()};
            UiElementTreeDescriptor descriptor{{ownership, 1, 1},
                                               {ownership, 2, 1},
                                               Identity<UiDocumentId>(),
                                               First<UiDocumentRevision>(),
                                               First<UiRuntimeTreeRevision>(),
                                               {1, 1, 1}};
            UiElementSlotAllocator slots{std::move(UiElementSlotAllocator::Create(ownership)).Value()};
            std::array<UiElementDescriptor, 1> elements{{{Identity<UiElementId>(), {}}}};
            UiElementTree tree{std::move(UiElementTree::Create(slots, descriptor, elements)).Value()};
            PointerLayout evaluator;
            UiLayoutEngine layoutOwner{
                CreateLayout({descriptor.instance, descriptor.canvas, descriptor.document, 1, 1, 2, First<UiInteractionRevision>()})};
            UiLayoutUpdateRequest request{{First<UiDocumentRevision>(), First<UiRuntimeTreeRevision>(), First<UiLayoutContentRevision>(),
                                           First<UiLayoutStyleRevision>(), First<UiLayoutIntrinsicRevision>(),
                                           First<UiLayoutCanvasRevision>(), First<UiLayoutPolicyRevision>()},
                                          {{0, 0}, {6400, 6400}},
                                          {{0, 0}, {6400, 6400}},
                                          &evaluator};
            UiLayoutSnapshot layout{std::move(layoutOwner.Update(tree, request)).Value()};
            UiElementHandle target{layout.Records().front().element};
            std::array<UiHitTestElement, 1> projection{{{target, {}, {}, {}, 0, false, true, true}}};
            UiHitTestStore hitOwner{std::move(UiHitTestStore::Create({descriptor.instance,
                                                                      descriptor.canvas,
                                                                      descriptor.document,
                                                                      UiRenderMode::ScreenSpaceOverlay,
                                                                      {6400, 6400},
                                                                      1,
                                                                      2}))
                                        .Value()};
            UiHitTestSnapshot hit{std::move(hitOwner.Publish(layout, projection)).Value()};
            UiRenderViewId view{ownership, 3, 1};
            RuntimeUiInputContextId context{ownership, 4, 1};
            UiPresentedInteractionState presented{std::move(UiPresentedInteractionState::Create(view, descriptor.canvas)).Value()};
            UiPointerCaptureStore captures{std::move(UiPointerCaptureStore::Create({ownership, MaximumUiInteractionPointers})).Value()};
            UiEventDispatcher dispatcher{
                std::move(UiEventDispatcher::Create({descriptor.instance, descriptor.canvas, descriptor.document, 1})).Value()};
            UiResolvedScreenCanvas canvas{{6400, 6400}, {100, 100}, {1, 1}};
            GestureObserver observer;
            std::optional<UiPointerInteraction> interaction;
            std::uint64_t sequence{1};

            explicit PointerFixture(const bool draggable = true) {
                REQUIRE(presented
                            .Apply({view, descriptor.canvas, layout.Descriptor().interaction, First<UiRenderSnapshotRevision>(),
                                    UiPresentationOutcome::Presented, UiPresentationReason::None})
                            .HasValue());
                const UiPointerCaptureContext owner{context,
                                                    view,
                                                    descriptor.instance,
                                                    descriptor.canvas,
                                                    descriptor.document,
                                                    tree.Revision(),
                                                    layout.Descriptor().interaction};
                const std::array policies{UiPointerTargetPolicy{target, draggable, true}};
                interaction.emplace(std::move(UiPointerInteraction::Create({owner}, policies)).Value());
                observer.interaction = &*interaction;
                observer.captures = &captures;
                observer.tree = &tree;
                observer.context = context;
            }

            Result<UiPointerInteractionResult> Pump(const UiPointerEdge edge, const float x, const std::uint64_t time) {
                const std::array samples{
                    UiPointerSample{UiPointerId::Create(1).Value(), edge, UiPointerModality::Touch, UiPointerButton::Primary, x, 10.0F}};
                return interaction->Pump({tree, hit, presented, captures, dispatcher, observer}, canvas, samples, time, sequence);
            }

            Result<UiPointerInteractionResult> Tick(const std::uint64_t time) {
                return interaction->Pump({tree, hit, presented, captures, dispatcher, observer}, canvas, {}, time, sequence);
            }
        };

        TEST_CASE("Prevented gesture begin retries without update or drop", "[runtime_ui][pointer][prevention]") {
            const bool draggable = GENERATE(false, true);
            PointerFixture fixture{draggable};
            const auto begin = draggable ? UiGestureKind::DragBegin : UiGestureKind::PanBegin;
            fixture.observer.prevented = begin;
            REQUIRE(fixture.Pump(UiPointerEdge::Press, 10, 1).HasValue());
            REQUIRE(fixture.Pump(UiPointerEdge::Move, 30, 2).HasValue());
            REQUIRE(fixture.Pump(UiPointerEdge::Move, 40, 3).HasValue());
            CHECK(fixture.observer.Count(begin) == 2);
            CHECK(fixture.observer.Count(UiGestureKind::PanUpdate) == 0);
            CHECK(fixture.observer.Count(UiGestureKind::DragUpdate) == 0);
            REQUIRE(fixture.Pump(UiPointerEdge::Release, 40, 4).HasValue());
            CHECK(fixture.observer.Count(UiGestureKind::Drop) == 0);
            CHECK(fixture.observer.Count(UiGestureKind::PanEnd) == 0);
            CHECK(fixture.observer.Count(UiGestureKind::Release) == 1);
            CHECK(fixture.observer.Count(UiGestureKind::Tap) == 0);
            CHECK(fixture.observer.Count(UiGestureKind::DoubleTap) == 0);
            CHECK(fixture.captures.IsDrained());
        }

        TEST_CASE("Prevented movement cannot become a tap or long press after returning", "[runtime_ui][pointer][prevention]") {
            const bool draggable = GENERATE(false, true);
            PointerFixture fixture{draggable};
            const auto begin = draggable ? UiGestureKind::DragBegin : UiGestureKind::PanBegin;
            fixture.observer.prevented = begin;
            REQUIRE(fixture.Pump(UiPointerEdge::Press, 10, 1).HasValue());
            REQUIRE(fixture.Pump(UiPointerEdge::Move, 30, 2).HasValue());
            REQUIRE(fixture.Pump(UiPointerEdge::Move, 10, 3).HasValue());
            REQUIRE(fixture.Tick(501).HasValue());
            CHECK(fixture.observer.Count(begin) == 1);
            CHECK(fixture.observer.Count(UiGestureKind::LongPress) == 0);
            REQUIRE(fixture.Pump(UiPointerEdge::Release, 10, 502).HasValue());
            CHECK(fixture.observer.Count(UiGestureKind::Tap) == 0);
            CHECK(fixture.observer.Count(UiGestureKind::DoubleTap) == 0);
            CHECK(fixture.observer.Count(UiGestureKind::Drop) == 0);
            CHECK(fixture.observer.Count(UiGestureKind::PanEnd) == 0);
            CHECK(fixture.observer.Count(UiGestureKind::Release) == 1);
            CHECK(fixture.captures.IsDrained());
        }

        TEST_CASE("Release beyond slop without a move sample cannot activate a tap", "[runtime_ui][pointer][boundary]") {
            PointerFixture fixture;
            REQUIRE(fixture.Pump(UiPointerEdge::Press, 10, 1).HasValue());
            REQUIRE(fixture.Pump(UiPointerEdge::Release, 30, 2).HasValue());
            CHECK(fixture.observer.Count(UiGestureKind::Tap) == 0);
            CHECK(fixture.observer.Count(UiGestureKind::DoubleTap) == 0);
            CHECK(fixture.observer.Count(UiGestureKind::DragBegin) == 0);
            CHECK(fixture.observer.Count(UiGestureKind::Release) == 1);
            CHECK(fixture.captures.IsDrained());
        }

        TEST_CASE("Callbacks cannot revive revoked gesture contacts", "[runtime_ui][pointer][lifetime]") {
            const auto interrupted = GENERATE(UiGestureKind::Press, UiGestureKind::DragBegin, UiGestureKind::LongPress);
            const auto mode = GENERATE(0, 1, 2, 3);
            PointerFixture fixture;
            fixture.observer.interrupt = interrupted;
            fixture.observer.localCancel = mode == 1;
            fixture.observer.shutdown = mode == 2;
            fixture.observer.retireTree = mode == 3;
            if (interrupted == UiGestureKind::Press)
                REQUIRE(fixture.Pump(UiPointerEdge::Press, 10, 1).HasError());
            else {
                REQUIRE(fixture.Pump(UiPointerEdge::Press, 10, 1).HasValue());
                if (interrupted == UiGestureKind::DragBegin)
                    REQUIRE(fixture.Pump(UiPointerEdge::Move, 30, 2).HasError());
                else
                    REQUIRE(fixture.Tick(501).HasError());
            }
            CHECK(fixture.captures.IsDrained());
            CHECK_FALSE(fixture.interaction->HasAccessibleDrag());
            CHECK(fixture.observer.Count(UiGestureKind::DragUpdate) == 0);
            CHECK(fixture.observer.Count(UiGestureKind::Drop) == 0);
            CHECK(fixture.observer.defaults == (interrupted == UiGestureKind::Press ? 0 : 1));
        }

        TEST_CASE("Prevented taps do not seed a double tap and release is not cancellation", "[runtime_ui][pointer][terminal]") {
            PointerFixture fixture;
            fixture.observer.prevented = UiGestureKind::Tap;
            REQUIRE(fixture.Pump(UiPointerEdge::Press, 10, 1).HasValue());
            REQUIRE(fixture.Pump(UiPointerEdge::Release, 10, 2).HasValue());
            fixture.observer.prevented = UiGestureKind::Count;
            REQUIRE(fixture.Pump(UiPointerEdge::Press, 10, 3).HasValue());
            REQUIRE(fixture.Pump(UiPointerEdge::Release, 10, 4).HasValue());
            CHECK(fixture.observer.Count(UiGestureKind::Tap) == 2);
            CHECK(fixture.observer.Count(UiGestureKind::DoubleTap) == 0);
            CHECK(fixture.observer.Count(UiGestureKind::Release) == 2);
            CHECK(fixture.observer.Count(UiGestureKind::Cancel) == 0);
        }

        TEST_CASE("Normal admitted movement and long press have distinct terminal semantics", "[runtime_ui][pointer][gesture]") {
            const bool drag = GENERATE(false, true);
            PointerFixture fixture{drag};
            REQUIRE(fixture.Pump(UiPointerEdge::Press, 10, 1).HasValue());
            REQUIRE(fixture.Pump(UiPointerEdge::Move, 30, 2).HasValue());
            REQUIRE(fixture.Pump(UiPointerEdge::Move, 40, 3).HasValue());
            REQUIRE(fixture.Pump(UiPointerEdge::Release, 40, 4).HasValue());
            CHECK(fixture.observer.Count(drag ? UiGestureKind::DragBegin : UiGestureKind::PanBegin) == 1);
            CHECK(fixture.observer.Count(drag ? UiGestureKind::DragUpdate : UiGestureKind::PanUpdate) == 1);
            CHECK(fixture.observer.Count(UiGestureKind::Drop) == (drag ? 1 : 0));
            CHECK(fixture.observer.Count(UiGestureKind::PanEnd) == 1);
            CHECK(fixture.observer.Count(UiGestureKind::Tap) == 0);
            REQUIRE(fixture.Pump(UiPointerEdge::Press, 10, 10).HasValue());
            REQUIRE(fixture.Tick(510).HasValue());
            REQUIRE(fixture.Tick(900).HasValue());
            REQUIRE(fixture.Pump(UiPointerEdge::Release, 10, 901).HasValue());
            CHECK(fixture.observer.Count(UiGestureKind::LongPress) == 1);
            CHECK(fixture.observer.Count(UiGestureKind::Tap) == 0);
            CHECK(fixture.captures.IsDrained());
        }

        TEST_CASE("Two physical touches produce pinch but never hover or tap", "[runtime_ui][pointer][touch]") {
            PointerFixture fixture;
            const auto pump = [&](const std::uint32_t pointer, const UiPointerEdge edge, const float x, const std::uint64_t time) {
                const std::array samples{
                    UiPointerSample{UiPointerId::Create(pointer).Value(), edge, UiPointerModality::Touch, UiPointerButton::Primary, x, 10}};
                return fixture.interaction->Pump({fixture.tree, fixture.hit, fixture.presented, fixture.captures, fixture.dispatcher,
                                                  fixture.observer},
                                                 fixture.canvas, samples, time, fixture.sequence);
            };
            REQUIRE(pump(1, UiPointerEdge::Press, 10, 1).HasValue());
            REQUIRE(pump(2, UiPointerEdge::Press, 30, 2).HasValue());
            REQUIRE(pump(2, UiPointerEdge::Move, 50, 3).HasValue());
            REQUIRE(fixture.Tick(900).HasValue());
            REQUIRE(pump(2, UiPointerEdge::Release, 50, 901).HasValue());
            REQUIRE(pump(1, UiPointerEdge::Release, 10, 902).HasValue());
            CHECK(fixture.observer.Count(UiGestureKind::PinchRotate) == 1);
            CHECK(fixture.observer.Count(UiGestureKind::LongPress) == 0);
            CHECK(fixture.observer.Count(UiGestureKind::Tap) == 0);
            CHECK(fixture.observer.Count(UiGestureKind::HoverEnter) == 0);
            CHECK(fixture.captures.IsDrained());
        }

        TEST_CASE("Mouse hover is routed without capture and prevention retries entry", "[runtime_ui][pointer][hover]") {
            PointerFixture fixture;
            const auto pump = [&](const float x, const std::uint64_t time) {
                const std::array samples{UiPointerSample{UiPointerId::Create(1).Value(), UiPointerEdge::Move, UiPointerModality::Mouse,
                                                         UiPointerButton::Primary, x, 10}};
                return fixture.interaction->Pump({fixture.tree, fixture.hit, fixture.presented, fixture.captures, fixture.dispatcher,
                                                  fixture.observer},
                                                 fixture.canvas, samples, time, fixture.sequence);
            };
            fixture.observer.prevented = UiGestureKind::HoverEnter;
            REQUIRE(pump(10, 1).HasValue());
            CHECK(fixture.observer.Count(UiGestureKind::HoverMove) == 0);
            fixture.observer.prevented = UiGestureKind::Count;
            REQUIRE(pump(10, 2).HasValue());
            REQUIRE(pump(20, 3).HasValue());
            REQUIRE(pump(120, 4).HasValue());
            CHECK(fixture.observer.Count(UiGestureKind::HoverEnter) == 2);
            CHECK(fixture.observer.Count(UiGestureKind::HoverMove) == 2);
            CHECK(fixture.observer.Count(UiGestureKind::HoverLeave) == 1);
            CHECK(fixture.captures.IsDrained());
        }

        TEST_CASE("Malformed batches and clocks fail before defaults while capacity failure drains the admitted prefix",
                  "[runtime_ui][pointer][bounds]") {
            PointerFixture fixture;
            std::array<UiPointerSample, MaximumUiInteractionPointers + 1> samples{};
            for (std::size_t index = 0; index < samples.size(); ++index)
                samples[index] = {UiPointerId::Create(index + 1).Value(),
                                  UiPointerEdge::Press,
                                  UiPointerModality::Touch,
                                  UiPointerButton::Primary,
                                  10,
                                  10};
            const auto environment = UiPointerInteractionEnvironment{fixture.tree,     fixture.hit,        fixture.presented,
                                                                     fixture.captures, fixture.dispatcher, fixture.observer};
            auto malformed = samples;
            malformed.back().pixelX = std::numeric_limits<float>::infinity();
            REQUIRE(fixture.interaction->Pump(environment, fixture.canvas, malformed, 1, fixture.sequence).HasError());
            CHECK(fixture.observer.defaults == 0);
            CHECK(fixture.captures.IsDrained());
            REQUIRE(fixture.interaction->Pump(environment, fixture.canvas, samples, 2, fixture.sequence).HasError());
            CHECK(fixture.captures.IsDrained());
            REQUIRE(fixture.Pump(UiPointerEdge::Press, 10, 3).HasValue());
            const auto before = fixture.observer.defaults;
            REQUIRE(fixture.Tick(2).HasError());
            CHECK(fixture.observer.defaults == before);
            REQUIRE(fixture.Pump(UiPointerEdge::Cancel, 10, 4).HasValue());
            CHECK(fixture.captures.IsDrained());
        }

        TEST_CASE("Quiescent moves rebind live gesture ownership and moved-from recognizers reject input", "[runtime_ui][pointer][move]") {
            PointerFixture fixture;
            REQUIRE(fixture.Pump(UiPointerEdge::Press, 10, 1).HasValue());
            UiPointerInteraction moved{std::move(*fixture.interaction)};
            REQUIRE(fixture.Tick(2).HasError());
            fixture.interaction.emplace(std::move(moved));
            fixture.observer.interaction = &*fixture.interaction;
            REQUIRE(fixture.Pump(UiPointerEdge::Release, 10, 3).HasValue());
            CHECK(fixture.observer.Count(UiGestureKind::Tap) == 1);
            CHECK(fixture.captures.IsDrained());
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui
