#include "Horo/Foundation/Sha256.h"
#include "Horo/Runtime/Ui/UiAsyncActions.h"
#include "Horo/Runtime/Ui/UiDiagnostics.h"
#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiScreenTransition.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <limits>
#include <thread>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T> T Stable(std::uint8_t marker) {
            SerializedUiId bytes{};
            bytes.back() = marker;
            return T::Create(bytes).Value();
        }

        Assets::AssetId Asset(std::uint8_t marker = 7) {
            SerializedUiId bytes{};
            bytes.back() = marker;
            return Assets::AssetId::FromBytes(bytes);
        }

        UiOwnershipGeneration Owner() {
            return UiOwnershipGeneration::Create(771).Value();
        }

        struct Fixture final {
            Assets::AssetRegistry registry;
            Assets::MemoryAssetProvider provider;
            JobSystem jobs{JobSystemConfig{1, 16}};
            Assets::AssetLoadService assets{jobs, provider};
            UiRuntimeAssetLoadService loader{registry, assets};
            UiElementSlotAllocator allocator{std::move(UiElementSlotAllocator::Create(Owner())).Value()};
            UiScreenTransition screen;

            explicit Fixture(std::uint32_t retention = 4)
                : screen(std::move(UiScreenTransition::Create(loader, Owner(), {retention, {}})).Value()) {
                Publish();
            }

            void Publish(std::uint64_t revision = 1, bool missingDependency = false) {
                const auto type = Assets::AssetTypeId::Parse("runtime.ui.document").Value();
                UiDocumentBuilder builder{Stable<UiDocumentId>(1), UiDocumentRevision::Create(revision).Value()};
                REQUIRE(builder.AddCanvas({Stable<UiCanvasId>(2), Stable<UiElementId>(3)}).HasValue());
                REQUIRE(
                    builder.AddElement({Stable<UiElementId>(3), {}, Assets::AssetTypeId::Parse("core.panel").Value(), {}, {}}).HasValue());
                REQUIRE(builder.AddRoute({Stable<UiRouteId>(9), UiPresentationBand::Screen, 1, false}).HasValue());
                REQUIRE(builder.AddRoute({Stable<UiRouteId>(10), UiPresentationBand::Screen, 2, false}).HasValue());
                if (missingDependency)
                    REQUIRE(builder.RequireAsset({Asset(8), Assets::AssetTypeId::Parse("core.texture").Value(), true}).HasValue());
                auto document = std::move(builder).Build();
                REQUIRE(document.HasValue());
                auto cooked = CookedUiDocument::Cook(document.Value());
                REQUIRE(cooked.HasValue());
                Assets::AssetCookArtifact artifact;
                artifact.id = Asset();
                artifact.type = type;
                artifact.target = AssetCookTargetId::Parse("linux-x64").Value();
                artifact.payload.assign(cooked.Value().Payload().begin(), cooked.Value().Payload().end());
                artifact.payloadDigest = ComputeSha256(std::as_bytes(std::span{artifact.payload}));
                provider.Insert(Asset(), Assets::EncodeCookedArtifact(artifact).Value());
                REQUIRE(registry
                            .Publish({{Asset(), type, ProjectPath::Parse("assets/screen.horoasset").Value(),
                                       ProjectPath::Parse("assets/screen.horoasset.horo").Value()}})
                            .status == Assets::AssetRegistryBuildStatus::Complete);
            }

            UiScreenTransitionRequest Request(std::uint32_t slot, std::uint64_t timeout = 100) {
                return {{{Asset(), Stable<UiDocumentId>(1), Stable<UiCanvasId>(2), UiDocumentRevision::Create(1).Value()},
                         Assets::AssetTypeId::Parse("runtime.ui.document").Value(),
                         AssetCookTargetId::Parse("linux-x64").Value()},
                        {Owner(), slot, 1},
                        Stable<UiRouteId>(9),
                        timeout};
            }

            std::vector<UiReloadCanvas> Canvases(const CookedUiDocument &document, RuntimeUiInstanceId instance) {
                const std::array elements{UiElementDescriptor{document.Canvases().front().rootElement, {}}};
                auto tree = UiElementTree::Create(allocator,
                                                  {instance,
                                                   {Owner(), instance.slot, 1},
                                                   document.Id(),
                                                   document.SourceRevision(),
                                                   UiRuntimeTreeRevision::Create(document.SourceRevision().Value()).Value(),
                                                   {8, 8, 8}},
                                                  elements);
                REQUIRE(tree.HasValue());
                std::vector<UiReloadCanvas> canvases;
                canvases.push_back({document.Canvases().front().id, std::move(tree).Value()});
                const auto root = canvases.front().tree.Root().Value().handle;
                canvases.front().routes.emplace(
                    std::move(UiScreenStack::Create({Owner(), {Owner(), root.slot, 1}, document.Routes(), 4})).Value());
                return canvases;
            }

            void Pump(std::uint64_t tick = 1) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
                while (screen.Poll(tick).state != UiScreenTransitionState::LoadCompleted) {
                    REQUIRE(std::chrono::steady_clock::now() < deadline);
                    REQUIRE(loader.Advance().HasValue());
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
            }

            void Ready(std::uint32_t slot, std::uint64_t tick = 1) {
                REQUIRE(screen.Begin(Request(slot), tick).HasValue());
                Pump(tick);
                REQUIRE(screen.PrepareAssets(screen.Progress().operation, tick).HasValue());
                REQUIRE(screen.LoadedDocument());
                REQUIRE(
                    screen.Prepare(screen.Progress().operation, Canvases(*screen.LoadedDocument(), {Owner(), slot, 1}), tick).HasValue());
                CHECK(screen.Progress().state == UiScreenTransitionState::Ready);
            }

            void Boot() {
                Ready(1);
                REQUIRE(screen.Commit(screen.Progress().operation, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands, 1) ==
                        UiScreenTransitionCommitResult::Committed);
                REQUIRE(screen.CollectRetired().HasValue());
            }

            UiHotReload::Prepared Reload(std::uint64_t version) {
                Publish(version);
                auto handle = loader.LoadAsync(Request(1).asset);
                REQUIRE(handle.HasValue());
                auto loading = std::move(handle).Value();
                REQUIRE(loading.Wait().HasValue());
                auto loaded = loading.TakeResult();
                REQUIRE(loaded.HasValue());
                auto canvases = Canvases(loaded.Value().document, {Owner(), 1, 1});
                auto generation = UiReloadGeneration::Create(std::move(loaded).Value(), {Owner(), 1, 1}, std::move(canvases));
                REQUIRE(generation.HasValue());
                auto prepared = screen.Current()->Prepare(std::move(generation).Value());
                REQUIRE(prepared.HasValue());
                return std::move(prepared).Value();
            }
        };

        struct RetirementObservation final {
            std::size_t destroyed{};
            bool borrowsClosed{};
            bool mutationsClosed{};
            bool shutdownDeferred{};
        };

        /** @brief Leaves the real control as sole failure owner after external store and producer destruction. */
        std::weak_ptr<const Error> RetainFailedControl(Fixture &f, UiReloadCanvas &canvas, RetirementObservation &observed) {
            const UiActionOwnerContext context{canvas.tree.Instance(),       canvas.tree.Canvas(),
                                               canvas.tree.SourceDocument(), canvas.tree.SourceDocumentRevision(),
                                               canvas.tree.Revision(),       UiInteractionRevision::Create(1).Value()};
            const auto root = canvas.tree.Root().Value().handle;
            auto control = UiControlStateMachine::Create(UiButtonControlDescriptor{{context, root, Stable<UiActionId>(12), {}}});
            REQUIRE(control.HasValue());
            canvas.controls.push_back({Stable<UiElementId>(3), std::move(control).Value()});
            std::weak_ptr<const Error> weak;
            auto store = std::move(UiAsyncActionStore::Create(context, 2)).Value();
            const UiActionRequest request{{Owner(), UiActionSequence::Create(1).Value()},
                                          {context, root},
                                          UiActionOrigin::Button,
                                          UiButtonActionCommand{Stable<UiActionId>(12), {}}};
            auto producer = std::move(store.Start(request)).Value();
            std::shared_ptr<const Error> error{new Error{MakeError(UiErrors::ActionHandlerFailed, std::string(200, 'x'))},
                                               [&f, &observed](const Error *value) noexcept {
                ++observed.destroyed;
                observed.borrowsClosed = !f.screen.Current() && f.screen.Acquire().HasError();
                observed.mutationsClosed =
                    f.screen.Commit(f.screen.Progress().operation, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands, 2) ==
                        UiScreenTransitionCommitResult::Collecting &&
                    f.screen.CollectRetired().HasError() && f.screen.Begin(f.Request(3), 2).HasError();
                f.screen.Shutdown();
                observed.shutdownDeferred = f.screen.Progress().state != UiScreenTransitionState::Stopped;
                delete value;
            }};
            weak = error;
            REQUIRE(producer.Fail(std::move(error)).HasValue());
            REQUIRE(canvas.controls.front().control.ObserveAsyncActions(store).HasValue());
            return weak;
        }

        TEST_CASE("Screen loading privately composes a whole generation and publishes once", "[runtime_ui][screen_transition]") {
            Fixture f;
            CHECK_FALSE(f.screen.Acquire().HasValue());
            f.Ready(1);
            CHECK_FALSE(f.screen.Current());
            CHECK_FALSE(f.screen.Acquire().HasValue());
            CHECK(f.screen.Commit(f.screen.Progress().operation, static_cast<UiStructuralCommitPoint>(99), 1) ==
                  UiScreenTransitionCommitResult::InvalidPoint);
            REQUIRE(f.screen.Commit(f.screen.Progress().operation, UiStructuralCommitPoint::CommitDeferredLifecycleChanges, 1) ==
                    UiScreenTransitionCommitResult::Committed);
            auto lease = f.screen.Acquire().Value();
            REQUIRE(lease.Get());
            CHECK(f.screen.IsCurrent(lease));
            CHECK(lease.Get()->Instance().State() == UiRuntimeInstanceState::Active);
            CHECK(lease.Get()->Canvases().front().routes->Top()->metadata.id == Stable<UiRouteId>(9));
            CHECK_FALSE(f.screen.Current()->InputEligible(Stable<UiCanvasId>(2), {Owner(), 1, 1}));
            CHECK(f.screen.Commit(f.screen.Progress().operation, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands, 1) ==
                  UiScreenTransitionCommitResult::NotReady);
        }

        TEST_CASE("Screen transition failures retain canonical typed diagnostic identity", "[runtime_ui][screen_transition][diagnostics]") {
            for (const auto *descriptor :
                 std::array{&UiScreenTransitionErrors::Invalid, &UiScreenTransitionErrors::Busy, &UiScreenTransitionErrors::Timeout}) {
                const auto record = MakeUiDiagnosticRecord(UiDiagnosticCategory::Lifecycle, MakeError(*descriptor));
                REQUIRE(record.HasValue());
                CHECK(record.Value().code.Value() == descriptor->code.Value());
            }
        }

        TEST_CASE("Screen replacement closes old admission and retains a whole-generation lease",
                  "[runtime_ui][screen_transition][lifetime]") {
            Fixture f;
            f.Boot();
            auto old = f.screen.Acquire().Value();
            f.Ready(2);
            CHECK(f.screen.IsCurrent(old));
            REQUIRE(f.screen.Commit(f.screen.Progress().operation, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands, 2) ==
                    UiScreenTransitionCommitResult::Committed);
            CHECK_FALSE(f.screen.IsCurrent(old));
            CHECK(old.Get()->Instance().State() == UiRuntimeInstanceState::Retiring);
            CHECK(old.Get()->Canvases().front().routes->State() == UiScreenStackState::Retiring);
            CHECK(f.screen.CollectRetired().Value() == 0);
            old = {};
            CHECK(f.screen.CollectRetired().Value() == 1);
        }

        TEST_CASE("Screen failure cancellation timeout and malformed composition retain the last-good screen",
                  "[runtime_ui][screen_transition][recovery]") {
            Fixture f;
            f.Boot();
            auto old = f.screen.Acquire().Value();
            SECTION("missing dependency preserves the original loader error") {
                f.Publish(2, true);
                REQUIRE(f.screen.Begin(f.Request(2), 1).HasValue());
                f.Pump();
                CHECK(f.screen.PrepareAssets(f.screen.Progress().operation, 1).HasError());
                REQUIRE(f.screen.Failure());
                CHECK(f.screen.Failure()->code.Value() == UiErrors::AssetMissing.code.Value());
            }
            SECTION("cancel before completion rejects late readiness") {
                REQUIRE(f.screen.Begin(f.Request(2), 1).HasValue());
                REQUIRE(f.screen.Cancel(f.screen.Progress().operation).HasValue());
                REQUIRE(f.loader.Advance().HasValue());
                CHECK(f.screen.Poll(2).state == UiScreenTransitionState::Cancelled);
                CHECK(f.screen.PrepareAssets(f.screen.Progress().operation, 2).HasError());
            }
            SECTION("ready candidate reaches the exact deadline") {
                f.Ready(2);
                CHECK(f.screen.Poll(101).state == UiScreenTransitionState::TimedOut);
                REQUIRE(f.screen.Failure());
                CHECK(f.screen.Failure()->code.Value() == UiScreenTransitionErrors::Timeout.code.Value());
            }
            SECTION("composition missing the actual canvas is refused") {
                REQUIRE(f.screen.Begin(f.Request(2), 1).HasValue());
                f.Pump();
                REQUIRE(f.screen.PrepareAssets(f.screen.Progress().operation, 1).HasValue());
                CHECK(f.screen.Prepare(f.screen.Progress().operation, {}, 1).HasError());
            }
            CHECK(f.screen.Commit(f.screen.Progress().operation, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands, 101) ==
                  UiScreenTransitionCommitResult::NotReady);
            CHECK(f.screen.IsCurrent(old));
            CHECK(old.Get()->Instance().State() == UiRuntimeInstanceState::Active);
            REQUIRE(f.screen.CollectRetired().HasValue());
            f.Publish(3);
            f.Ready(3, 101);
            CHECK(f.screen.Progress().operation.sequence == 3);
        }

        TEST_CASE("Parent cancellation after screen composition preserves the last-good screen",
                  "[runtime_ui][screen_transition][recovery]") {
            Fixture f;
            f.Boot();
            auto old = f.screen.Acquire().Value();
            CancellationSource parent;
            REQUIRE(f.screen.Begin(f.Request(2), 1, parent.Token()).HasValue());
            f.Pump();
            REQUIRE(f.screen.PrepareAssets(f.screen.Progress().operation, 1).HasValue());
            REQUIRE(f.screen.Prepare(f.screen.Progress().operation, f.Canvases(*f.screen.LoadedDocument(), {Owner(), 2, 1}), 1).HasValue());
            parent.RequestCancellation();
            CHECK(f.screen.Poll(2).state == UiScreenTransitionState::Cancelled);
            CHECK(f.screen.Commit(f.screen.Progress().operation, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands, 2) ==
                  UiScreenTransitionCommitResult::NotReady);
            CHECK(f.screen.IsCurrent(old));
            REQUIRE(f.screen.CollectRetired().HasValue());
            f.Ready(3, 2);
            CHECK(f.screen.Progress().operation.sequence == 3);
        }

        TEST_CASE("Screen commit refuses source navigation reload and in-flight route transactions",
                  "[runtime_ui][screen_transition][stale]") {
            Fixture f;
            f.Boot();
            auto old = f.screen.Acquire().Value();
            f.Ready(2);
            auto *canvas = f.screen.Current()->Current()->Canvas(Stable<UiCanvasId>(2));
            SECTION("newer route revision") {
                REQUIRE(canvas->routes->Push(Stable<UiRouteId>(10)).Value().IsCommitted());
                CHECK(f.screen.Commit(f.screen.Progress().operation, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands, 2) ==
                      UiScreenTransitionCommitResult::SourceStale);
            }
            SECTION("held prepared route transaction") {
                auto transaction = canvas->routes->Prepare(UiRouteOperationRequest::Pop());
                REQUIRE(transaction.HasValue());
                auto prepared = std::move(transaction).Value();
                CHECK(f.screen.Commit(f.screen.Progress().operation, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands, 2) ==
                      UiScreenTransitionCommitResult::RouteBusy);
                REQUIRE(prepared.Cancel().HasValue());
                CHECK(f.screen.Commit(f.screen.Progress().operation, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands, 2) ==
                      UiScreenTransitionCommitResult::Committed);
            }
            SECTION("same-screen reload supersedes the pinned source") {
                auto prepared = f.Reload(2);
                REQUIRE(f.screen.Current()->Commit(prepared, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands).HasValue());
                CHECK(f.screen.Commit(f.screen.Progress().operation, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands, 2) ==
                      UiScreenTransitionCommitResult::SourceStale);
            }
        }

        TEST_CASE("Screen retirement capacity backpressures publication without losing either generation",
                  "[runtime_ui][screen_transition][capacity]") {
            Fixture f{1};
            f.Boot();
            auto first = f.screen.Acquire().Value();
            f.Ready(2);
            REQUIRE(f.screen.Commit(f.screen.Progress().operation, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands, 2) ==
                    UiScreenTransitionCommitResult::Committed);
            REQUIRE(f.screen.CollectRetired().HasValue());
            auto second = f.screen.Acquire().Value();
            f.Ready(3, 2);
            CHECK(f.screen.Commit(f.screen.Progress().operation, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands, 2) ==
                  UiScreenTransitionCommitResult::RetentionFull);
            CHECK(f.screen.IsCurrent(second));
            first = {};
            CHECK(f.screen.CollectRetired().Value() == 1);
            CHECK(f.screen.Commit(f.screen.Progress().operation, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands, 2) ==
                  UiScreenTransitionCommitResult::Committed);
        }

        TEST_CASE("Screen polling publication refusals and shutdown allocate no owner-thread storage",
                  "[runtime_ui][screen_transition][allocation]") {
            Fixture f;
            f.Boot();
            auto old = f.screen.Acquire().Value();
            f.Ready(2);
            UiScreenTransitionCommitResult invalid;
            UiScreenTransitionCommitResult committed;
            UiScreenTransitionProgress progress;
            Tests::AllocationProbe::Measurement measured;
            {
                Tests::AllocationProbe::ScopedMeasurement allocations;
                progress = f.screen.Poll(2);
                invalid = f.screen.Commit(f.screen.Progress().operation, static_cast<UiStructuralCommitPoint>(99), 2);
                committed = f.screen.Commit(f.screen.Progress().operation, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands, 2);
                f.screen.Shutdown();
                measured = allocations.Snapshot();
            }
            CHECK(progress.state == UiScreenTransitionState::Ready);
            CHECK(invalid == UiScreenTransitionCommitResult::InvalidPoint);
            CHECK(committed == UiScreenTransitionCommitResult::Committed);
            CHECK(measured.requests == 0);
            CHECK_FALSE(f.screen.CanReclaim());
            REQUIRE(f.screen.CollectRetired().HasValue());
            old = {};
            REQUIRE(f.screen.CollectRetired().HasValue());
            CHECK(f.screen.CanReclaim());
        }

        TEST_CASE("Screen shutdown during load or readiness cannot activate late work", "[runtime_ui][screen_transition][shutdown]") {
            Fixture f;
            SECTION("loading") {
                REQUIRE(f.screen.Begin(f.Request(1), 1).HasValue());
            }
            SECTION("ready") {
                f.Ready(1);
            }
            f.screen.Shutdown();
            f.screen.Shutdown();
            REQUIRE(f.loader.Advance().HasValue());
            CHECK(f.screen.Commit(f.screen.Progress().operation, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands, 2) ==
                  UiScreenTransitionCommitResult::Stopped);
            CHECK_FALSE(f.screen.Current());
            CHECK(f.screen.Begin(f.Request(2), 2).HasError());
            REQUIRE(f.screen.CollectRetired().HasValue());
            CHECK(f.screen.CanReclaim());
        }

        TEST_CASE("Screen commit retains final failed control projections until fenced collection",
                  "[runtime_ui][screen_transition][lifetime][reentry]") {
            RetirementObservation observed;

            Fixture f;
            f.Boot();
            auto old = f.screen.Acquire().Value();
            auto *canvas = f.screen.Current()->Current()->Canvas(Stable<UiCanvasId>(2));
            const auto weak = RetainFailedControl(f, *canvas, observed);
            REQUIRE_FALSE(weak.expired());
            f.Ready(2);
            const auto frees = Tests::AllocationProbe::FreeCount();
            const auto committed =
                f.screen.Commit(f.screen.Progress().operation, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands, 2);
            const auto freedDuringCommit = Tests::AllocationProbe::FreeCount() - frees;
            CHECK(committed == UiScreenTransitionCommitResult::Committed);
            CHECK(freedDuringCommit == 0);
            CHECK(observed.destroyed == 0);
            CHECK_FALSE(weak.expired());
            CHECK(canvas->controls.front().control.LifecycleState() == UiControlLifecycleState::Retiring);
            CHECK(canvas->controls.front().control.AsyncAction().Value()->state == UiAsyncActionState::Failed);
            REQUIRE(f.screen.CollectRetired().HasValue());
            CHECK(observed.destroyed == 0);
            old = {};
            REQUIRE(f.screen.CollectRetired().HasValue());
            CHECK(observed.destroyed == 1);
            CHECK(weak.expired());
            CHECK(observed.borrowsClosed);
            CHECK(observed.mutationsClosed);
            CHECK(observed.shutdownDeferred);
            CHECK(f.screen.Progress().state == UiScreenTransitionState::Stopped);
            REQUIRE(f.screen.CollectRetired().HasValue());
            CHECK(f.screen.CanReclaim());
        }

        TEST_CASE("Late transition commands cannot cancel prepare or publish a retry", "[runtime_ui][screen_transition][stale]") {
            Fixture f;
            const auto previous = f.screen.Begin(f.Request(1), 1).Value();
            REQUIRE(f.screen.Cancel(previous).HasValue());
            REQUIRE(f.screen.CollectRetired().HasValue());
            f.Ready(2);
            CHECK(f.screen.Cancel(previous).HasError());
            CHECK(f.screen.PrepareAssets(previous, 1).HasError());
            CHECK(f.screen.Prepare(previous, {}, 1).HasError());
            CHECK(f.screen.Commit(previous, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands, 1) ==
                  UiScreenTransitionCommitResult::SourceStale);
            CHECK(f.screen.Progress().state == UiScreenTransitionState::Ready);
            CHECK(f.screen.Commit(f.screen.Progress().operation, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands, 1) ==
                  UiScreenTransitionCommitResult::Committed);
        }

        TEST_CASE("Screen admission burns instance slots and checks monotonic overflow-safe deadlines",
                  "[runtime_ui][screen_transition][identity]") {
            Fixture f;
            CHECK(f.screen.Begin(f.Request(0), 1).HasError());
            CHECK(f.screen.Begin(f.Request(1, 0), 1).HasError());
            auto wrongOwner = f.Request(1);
            wrongOwner.instance.ownership = UiOwnershipGeneration::Create(772).Value();
            CHECK(f.screen.Begin(wrongOwner, 1).HasError());
            const auto start = std::numeric_limits<std::uint64_t>::max() - 10;
            REQUIRE(f.screen.Begin(f.Request(1, 10), start).HasValue());
            CHECK(f.screen.Begin(f.Request(2), start).HasError());
            CHECK(f.screen.Poll(start + 9).state != UiScreenTransitionState::TimedOut);
            CHECK(f.screen.Poll(start + 10).state == UiScreenTransitionState::TimedOut);
            REQUIRE(f.screen.CollectRetired().HasValue());
            CHECK(f.screen.Begin(f.Request(1), start + 10).HasError());
            REQUIRE(f.screen.Begin(f.Request(2), start + 10).HasValue());
            CHECK(f.screen.Poll(start).state == UiScreenTransitionState::Failed);
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui
