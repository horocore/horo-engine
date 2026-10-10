#include "UiHudAssociationTestFixture.h"

namespace Horo::Runtime::Ui::ReloadTests {
    namespace {
        TEST_CASE("Cooked HUD presentation policy survives the actual Assets load boundary", "[runtime_ui][hud][cook][asset_loading]") {
            auto authored = Authored();
            authored.presentation = {UiSafeAreaMode::Inset, {3, 2}, {5, 4}, UiPixelSnapMode::Edges};
            const auto loaded = Load(1, authored, false);
            REQUIRE(loaded.document.Canvases().size() == 1);
            CHECK(loaded.document.Canvases().front() == authored);
            CHECK(loaded.document.Payload()[8] == CurrentCookedUiDocumentFormatVersion);
        }

        TEST_CASE("Legacy v1 cooked HUD canvas decodes with neutral presentation defaults", "[runtime_ui][hud][cook][migration]") {
            // Independent little-endian v1 golden: header 56 bytes, one 42-byte canvas, no elements/dependencies/routes.
            std::array<std::uint8_t, 98> payload{};
            const std::array<std::uint8_t, 8> magic{'H', 'O', 'R', 'O', 'U', 'I', 'C', 0};
            std::ranges::copy(magic, payload.begin());
            payload[8] = 1;
            payload[12] = 1;
            payload[14] = 1;
            payload[31] = 1;
            payload[32] = 1;
            payload[40] = 1;
            payload[71] = 2;
            payload[87] = 10;
            payload[89] = 0x80;
            payload[90] = 0x07;
            payload[93] = 0x38;
            payload[94] = 0x04;
            payload[97] = 1;
            const auto decoded = Take(CookedUiDocument::Decode(payload));
            auto expected = Authored();
            expected.presentation = {};
            REQUIRE(decoded.Canvases().size() == 1);
            CHECK(decoded.Canvases().front() == expected);
            CHECK(std::ranges::equal(decoded.Payload(), payload));
        }

        TEST_CASE("Cooked HUD policy rejects malformed truncated and unsupported payloads", "[runtime_ui][hud][cook][malformed]") {
            UiDocumentBuilder builder{Stable<UiDocumentId>(1), Revision<UiDocumentRevision>(1)};
            REQUIRE(builder.AddCanvas(Authored()).HasValue());
            const auto cooked = Take(CookedUiDocument::Cook(Take(std::move(builder).Build())));
            const std::vector<std::uint8_t> original{cooked.Payload().begin(), cooked.Payload().end()};
            REQUIRE(original.size() == 116);
            for (const std::size_t offset : {98, 99, 103, 107, 111, 115}) {
                auto malformed = original;
                malformed[offset] = offset == 98 || offset == 115 ? 0xFF : 0;
                const auto rejected = CookedUiDocument::Decode(malformed);
                REQUIRE(rejected.HasError());
                CHECK(rejected.ErrorValue().code.Value() == UiErrors::CookedPayloadMalformed.code.Value());
            }
            auto oversized = original;
            oversized[99] = 1;
            oversized[100] = 0x10;
            CHECK(CookedUiDocument::Decode(oversized).HasError());
            for (std::size_t size = 98; size < original.size(); ++size) {
                const auto rejected = CookedUiDocument::Decode(std::span{original}.first(size));
                REQUIRE(rejected.HasError());
                CHECK(rejected.ErrorValue().code.Value() == UiErrors::CookedPayloadMalformed.code.Value());
            }
            for (const std::uint8_t version : {0, 3}) {
                auto unsupported = original;
                unsupported[8] = version;
                const auto rejected = CookedUiDocument::Decode(unsupported);
                REQUIRE(rejected.HasError());
                CHECK(rejected.ErrorValue().code.Value() == UiErrors::CookedFormatUnsupported.code.Value());
            }
        }

        TEST_CASE("Deferred real binding abandonment cannot partially shut down or dereference a collecting HUD publisher",
                  "[runtime_ui][hud][reload][capture][reentry]") {
            UiElementSlotAllocator allocator{Take(UiElementSlotAllocator::Create(Owner()))};
            auto publisher = Publish(HudGeneration(allocator, 1, Authored(), Binding(), Instance(), true));
            auto hud = Take(UiHudAssociation::Create(Binding(), publisher));
            auto authority = std::make_shared<RetirementAuthority>();
            authority->publisher = &publisher;
            authority->hud = &hud;
            auto old = Take(hud.PrepareFrame(publisher, Revision<UiRenderSnapshotRevision>(1)));
            const UiPresentationReceipt first{old.Association().view,
                                              old.Layout().Descriptor().canvas,
                                              old.Layout().Descriptor().interaction,
                                              old.Snapshot(),
                                              UiPresentationOutcome::Presented,
                                              UiPresentationReason::None};
            REQUIRE(hud.ApplyPresentation(publisher, old, first).HasValue());
            QueuePending(publisher, authority);
            auto replacement = Take(publisher.Prepare(HudGeneration(allocator, 2)));
            REQUIRE(publisher.Commit(replacement, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands).HasValue());
            REQUIRE(hud.Refresh(publisher).HasValue());
            auto frame = Take(hud.PrepareFrame(publisher, Revision<UiRenderSnapshotRevision>(2)));
            const UiPresentationReceipt receipt{frame.Association().view,
                                                frame.Layout().Descriptor().canvas,
                                                frame.Layout().Descriptor().interaction,
                                                frame.Snapshot(),
                                                UiPresentationOutcome::Presented,
                                                UiPresentationReason::None};
            REQUIRE(hud.ApplyPresentation(publisher, frame, receipt).HasValue());
            auto capture = CaptureFor(publisher, hud.Descriptor());
            authority->capture = &capture;
            REQUIRE(publisher.CollectRetired().HasValue());
            CHECK(authority->abandons == 1);
            CHECK(authority->busy);
            CHECK(authority->capturePreserved);
            CHECK(authority->policyPreserved);
            CHECK(authority->reentryRejected);
            CHECK(capture.IsActive());
            CHECK(hud.InputEligible(publisher));
            CHECK(publisher.IsCurrent(frame.Generation()));
            REQUIRE(hud.Shutdown(publisher).HasValue());
            CHECK_FALSE(capture.IsActive());
            CHECK(capture.CancellationReason() == UiPointerCaptureCancellationReason::ContextRemoved);
            CHECK(Canvas(publisher).captures->ActiveCount() == 0);
            CHECK_FALSE(hud.InputEligible(publisher));
            REQUIRE(hud.Shutdown(publisher).HasValue());
            CHECK(authority->abandons == 1);
        }

        TEST_CASE("Two players share a logical viewport without sharing HUD view input or lifetime", "[runtime_ui][hud][players]") {
            Rig rig;
            auto second = Binding();
            second.id.slot += 10;
            second.player.slot += 10;
            second.context.slot += 10;
            second.view.slot += 10;
            second.providers[0].instance.slot += 10;
            auto publisher = Publish(HudGeneration(rig.allocator, 1, Authored(), second, {Owner(), 5, 1}));
            auto hud = Take(UiHudAssociation::Create(second, publisher));
            auto firstFrame = rig.Frame();
            auto secondFrame = Take(hud.PrepareFrame(publisher, Revision<UiRenderSnapshotRevision>(1)));
            REQUIRE(rig.hud.ApplyPresentation(rig.publisher, firstFrame, rig.Receipt(firstFrame)).HasValue());
            REQUIRE(hud.ApplyPresentation(publisher, secondFrame, rig.Receipt(secondFrame)).HasValue());
            CHECK(hud.Descriptor().viewport == rig.hud.Descriptor().viewport);
            CHECK(hud.Descriptor().player != rig.hud.Descriptor().player);
            CHECK(hud.ApplyPresentation(publisher, firstFrame, rig.Receipt(firstFrame)).HasError());
            REQUIRE(hud.Shutdown(publisher).HasValue());
            CHECK_FALSE(hud.InputEligible(publisher));
            CHECK(rig.hud.InputEligible(rig.publisher));
            CHECK(publisher.Current() != nullptr);
        }

        TEST_CASE("HUD explicitly admits game-instance gameplay and authored DPI font policies", "[runtime_ui][hud][provider][geometry]") {
            UiElementSlotAllocator allocator{Take(UiElementSlotAllocator::Create(Owner()))};
            auto binding = Binding();
            binding.providers[0].scope = UiBindingProviderScopeKind::GameInstance;
            binding.viewportEvidence.dpiScale = {2, 1};
            auto authored = Authored();
            authored.scaleMode = UiScaleMode::ConstantPhysicalSize;
            authored.presentation.fontScale = {3, 2};
            auto publisher = Publish(HudGeneration(allocator, 1, authored, binding));
            auto hud = Take(UiHudAssociation::Create(binding, publisher));
            auto frame = Take(hud.PrepareFrame(publisher, Revision<UiRenderSnapshotRevision>(1)));
            CHECK(frame.Metrics().pixelsPerDip == UiCanvasDeviceScale{2, 1});
            CHECK(frame.Metrics().logicalExtent == UiCanvasLogicalExtent{9'600, 6'400});
            CHECK(frame.Layout().Descriptor().fontScale == UiCanvasScaleFactor{3, 2});
            CHECK(Canvas(publisher).bindings->HasProvider(binding.providers[0].instance, UiBindingProviderScopeKind::GameInstance));
        }

        TEST_CASE("HUD observes real route retirement and foreign publisher shutdown", "[runtime_ui][hud][lifecycle]") {
            Rig rig;
            auto frame = rig.Frame();
            REQUIRE(rig.hud.ApplyPresentation(rig.publisher, frame, rig.Receipt(frame)).HasValue());
            SECTION("Actual route popped") {
                REQUIRE(Canvas(rig.publisher).routes->Pop().Value().IsCommitted());
                CHECK(rig.hud.ResolveCanvas(rig.publisher).HasError());
                CHECK_FALSE(rig.hud.InputEligible(rig.publisher));
            }
            SECTION("External publisher shuts down first") {
                rig.publisher.Shutdown();
                CHECK(rig.hud.Refresh(rig.publisher).HasError());
                CHECK(rig.hud.PrepareFrame(rig.publisher, Revision<UiRenderSnapshotRevision>(2)).HasError());
                CHECK_FALSE(rig.hud.InputEligible(rig.publisher));
                REQUIRE(rig.hud.Shutdown(rig.publisher).HasValue());
                CHECK(frame.Layout().Records().size() == 1);
            }
        }

        TEST_CASE("HUD policy attachment and shutdown cancel only the actual admitted context captures", "[runtime_ui][hud][capture]") {
            Rig rig;
            auto frame = rig.Frame();
            REQUIRE(rig.hud.ApplyPresentation(rig.publisher, frame, rig.Receipt(frame)).HasValue());
            auto capture = rig.Capture();
            REQUIRE(capture.IsActive());
            auto invalid = Binding();
            invalid.player.slot += 1;
            CHECK(rig.hud.Reassociate(invalid, rig.publisher).HasError());
            CHECK(capture.IsActive());
            REQUIRE(rig.hud.SetPolicy(rig.publisher, true, false).HasValue());
            CHECK_FALSE(capture.IsActive());
            CHECK(capture.CancellationReason() == UiPointerCaptureCancellationReason::ContextRemoved);
            CHECK(Canvas(rig.publisher).captures->ActiveCount() == 0);
            REQUIRE(rig.hud.SetPolicy(rig.publisher, true, true).HasValue());
            auto fresh = rig.Frame(2);
            REQUIRE(rig.hud.ApplyPresentation(rig.publisher, fresh, rig.Receipt(fresh)).HasValue());
            auto replacement = rig.Capture();
            REQUIRE(rig.hud.Shutdown(rig.publisher).HasValue());
            CHECK_FALSE(replacement.IsActive());
            CHECK(replacement.CancellationReason() == UiPointerCaptureCancellationReason::ContextRemoved);
        }

        TEST_CASE("Foreign publisher shutdown preserves the real HUD association and outstanding capture",
                  "[runtime_ui][hud][provenance]") {
            Rig rig;
            auto frame = rig.Frame();
            REQUIRE(rig.hud.ApplyPresentation(rig.publisher, frame, rig.Receipt(frame)).HasValue());
            auto capture = rig.Capture();
            auto other = Publish(HudGeneration(rig.allocator, 2));
            const auto refused = rig.hud.Shutdown(other);
            REQUIRE(refused.HasError());
            CHECK(refused.ErrorValue().code.Value() == UiErrors::HandleOwnerMismatch.code.Value());
            CHECK(capture.IsActive());
            CHECK(rig.hud.InputEligible(rig.publisher));
            REQUIRE(rig.hud.Shutdown(rig.publisher).HasValue());
            CHECK_FALSE(capture.IsActive());
        }

        TEST_CASE("Cancelled reload and rejected foreign audience preserve HUD admission while committed camera policy fails closed",
                  "[runtime_ui][hud][reload]") {
            Rig rig;
            auto frame = rig.Frame();
            REQUIRE(rig.hud.ApplyPresentation(rig.publisher, frame, rig.Receipt(frame)).HasValue());
            const auto original = rig.hud.Descriptor();
            auto capture = rig.Capture();
            CancellationSource cancellation;
            auto cancelled = Take(rig.publisher.Prepare(HudGeneration(rig.allocator, 2), cancellation.Token()));
            cancellation.RequestCancellation();
            CHECK(rig.publisher.Commit(cancelled, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands).HasError());
            CHECK(rig.hud.InputEligible(rig.publisher));
            CHECK(capture.IsActive());
            CHECK(rig.publisher.IsCurrent(frame.Generation()));
            auto otherPlayer = Binding();
            otherPlayer.player.slot += 1;
            const auto foreign = rig.publisher.Prepare(HudGeneration(rig.allocator, 3, Authored(), otherPlayer));
            REQUIRE(foreign.HasError());
            CHECK(foreign.ErrorValue().code.Value() == UiErrors::HandleOwnerMismatch.code.Value());
            CHECK(rig.hud.InputEligible(rig.publisher));
            CHECK(rig.hud.Descriptor() == original);
            CHECK(rig.publisher.IsCurrent(frame.Generation()));
            CHECK(capture.IsActive());
            CHECK(rig.hud.PrepareFrame(rig.publisher, Revision<UiRenderSnapshotRevision>(2)).HasValue());
            auto prepared = Take(rig.publisher.Prepare(HudGeneration(rig.allocator, 4, Authored(UiRenderMode::ScreenSpaceCamera))));
            REQUIRE(rig.publisher.Commit(prepared, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands).HasValue());
            CHECK_FALSE(rig.publisher.IsCurrent(frame.Generation()));
            const auto refreshed = rig.hud.Refresh(rig.publisher);
            REQUIRE(refreshed.HasError());
            CHECK(refreshed.ErrorValue().code.Value() == UiErrors::CanvasSpaceModeMismatch.code.Value());
            CHECK_FALSE(rig.hud.InputEligible(rig.publisher));
            CHECK(rig.hud.ApplyPresentation(rig.publisher, frame, rig.Receipt(frame)).HasError());
        }

        TEST_CASE("Player HUD uses actual gameplay bindings safe geometry and renderer extraction", "[runtime_ui][hud][integration]") {
            Rig rig;
            CHECK_FALSE(rig.hud.InputEligible(rig.publisher));
            auto frame = rig.Frame();
            CHECK(frame.Metrics().ContentPixelRect() == UiCanvasPixelRect{10, 20, 300, 200});
            CHECK(frame.Layout().Records().front().arrangement.contentBox.extent == UiLogicalExtent{19'200, 12'800});
            const auto &canvas = Canvas(rig.publisher);
            REQUIRE(canvas.bindings->Find(canvas.tree, Stable<UiBindingId>(15)) != nullptr);
            CHECK(std::get<bool>(canvas.bindings->Find(canvas.tree, Stable<UiBindingId>(15))->value));
            const UiRenderSnapshotLimits limits{4, 1, 1, 4, 1, 1, 1};
            auto extractor = Take(UiRenderExtractor::Create({frame.Association().view, limits, 2}));
            const std::array transforms{UiLogicalTransform{}};
            const std::array commands{UiDrawCommand{canvas.tree.Root().Value().handle,
                                                    frame.Layout().Records().front().arrangement.contentBox, 0, NoUiRenderIndex,
                                                    NoUiRenderIndex, 1.0F, UiSolidDraw{{1.0F, 1.0F, 1.0F, 1.0F}}}};
            const auto &layout = frame.Layout().Descriptor();
            auto extracted =
                Take(extractor.Extract(canvas.tree,
                                       {layout.instance, layout.canvas, layout.document, layout.sources.document, layout.sources.tree,
                                        layout.interaction, frame.Snapshot(), frame.Association().view, limits},
                                       {commands, {}, {}, {}, {}, transforms, {}}));
            CHECK(extracted.Descriptor().snapshotRevision == frame.Snapshot());
            REQUIRE(rig.hud.ApplyPresentation(rig.publisher, frame, rig.Receipt(frame)).HasValue());
            CHECK(rig.hud.InputEligible(rig.publisher));
        }

        TEST_CASE("HUD enable visibility and passive policy are independent and fence old tickets", "[runtime_ui][hud][policy]") {
            Rig rig;
            auto original = rig.Frame();
            REQUIRE(rig.hud.SetPolicy(rig.publisher, true, false).HasValue());
            CHECK(rig.hud.Descriptor().enabled);
            CHECK_FALSE(rig.hud.Descriptor().visible);
            CHECK(rig.hud.ResolveCanvas(rig.publisher).HasValue());
            CHECK(rig.hud.PrepareFrame(rig.publisher, original.Snapshot()).HasError());
            REQUIRE(rig.hud.SetPolicy(rig.publisher, false, true).HasValue());
            CHECK_FALSE(rig.hud.Descriptor().enabled);
            CHECK(rig.hud.Descriptor().visible);
            REQUIRE(rig.hud.SetPolicy(rig.publisher, true, true).HasValue());
            CHECK(rig.hud.ApplyPresentation(rig.publisher, original, rig.Receipt(original)).HasError());
            auto passive = rig.hud.Descriptor();
            passive.interactive = false;
            REQUIRE(rig.hud.Reassociate(passive, rig.publisher).HasValue());
            auto frame = rig.Frame(2);
            REQUIRE(rig.hud.ApplyPresentation(rig.publisher, frame, rig.Receipt(frame)).HasValue());
            CHECK_FALSE(rig.hud.InputEligible(rig.publisher));
        }

        TEST_CASE("HUD reassociation validates exact audience provider and admitted view atomically", "[runtime_ui][hud][validation]") {
            Rig rig;
            const auto original = rig.hud.Descriptor();
            auto invalid = original;
            SECTION("Other player") {
                invalid.player.slot += 1;
            }
            SECTION("Other instance ownership") {
                invalid.viewport.ownership = Take(UiOwnershipGeneration::Create(705));
            }
            SECTION("Unadmitted view") {
                invalid.view.slot += 1;
            }
            SECTION("Unknown provider incarnation") {
                invalid.providers[0].instance.generation += 1;
            }
            SECTION("Scene scope does not become player ownership") {
                invalid.providers[0].scope = UiBindingProviderScopeKind::Scene;
            }
            SECTION("Provider count overflow") {
                invalid.providerCount = MaximumUiBindingProviders + 1;
            }
            SECTION("Duplicate provider") {
                invalid.providers[1] = invalid.providers[0];
                invalid.providerCount = 2;
            }
            SECTION("Empty drawable output") {
                invalid.viewportEvidence.pixelExtent.width = 0;
            }
            SECTION("Invalid safe area") {
                invalid.viewportEvidence.safeAreaInsets.left = 400;
            }
            CHECK(rig.hud.Reassociate(invalid, rig.publisher).HasError());
            CHECK(rig.hud.Descriptor() == original);
            CHECK(rig.hud.PrepareFrame(rig.publisher, Revision<UiRenderSnapshotRevision>(1)).HasValue());
        }

        TEST_CASE("HUD requires camera evidence only for authored camera projection and rejects world mode", "[runtime_ui][hud][camera]") {
            UiElementSlotAllocator allocator{Take(UiElementSlotAllocator::Create(Owner()))};
            auto publisher = Publish(HudGeneration(allocator, 1, Authored(UiRenderMode::ScreenSpaceCamera)));
            auto binding = Binding();
            CHECK(UiHudAssociation::Create(binding, publisher).HasError());
            binding.camera = UiHudCameraId{Owner(), 44, 1};
            auto hud = Take(UiHudAssociation::Create(binding, publisher));
            auto old = Take(hud.PrepareFrame(publisher, Revision<UiRenderSnapshotRevision>(1)));
            binding.camera->generation += 1;
            REQUIRE(hud.Reassociate(binding, publisher).HasValue());
            const UiPresentationReceipt receipt{binding.view,   old.Layout().Descriptor().canvas, old.Layout().Descriptor().interaction,
                                                old.Snapshot(), UiPresentationOutcome::Presented, UiPresentationReason::None};
            CHECK(hud.ApplyPresentation(publisher, old, receipt).HasError());
            auto world = Publish(HudGeneration(allocator, 2, Authored(UiRenderMode::WorldSpace)));
            CHECK(UiHudAssociation::Create(binding, world).HasError());
        }

        TEST_CASE("HUD resize changes resolved metrics and rejects stale published geometry", "[runtime_ui][hud][geometry]") {
            Rig rig;
            auto binding = rig.hud.Descriptor();
            binding.viewportEvidence.pixelExtent.width = 640;
            REQUIRE(rig.hud.Reassociate(binding, rig.publisher).HasValue());
            CHECK(rig.hud.ResolveCanvas(rig.publisher).Value().ContentPixelRect().width == 620);
            CHECK(rig.hud.PrepareFrame(rig.publisher, Revision<UiRenderSnapshotRevision>(2)).HasError());
        }

        TEST_CASE("HUD provider query rejects revocation retirement moved stores and false scopes", "[runtime_ui][hud][provider]") {
            Rig rig;
            auto &canvas = Canvas(rig.publisher);
            const auto provider = Binding().providers[0].instance;
            CHECK(canvas.bindings->HasProvider(provider, UiBindingProviderScopeKind::Player));
            CHECK_FALSE(canvas.bindings->HasProvider(provider, UiBindingProviderScopeKind::GameInstance));
            auto frame = rig.Frame();
            REQUIRE(rig.hud.ApplyPresentation(rig.publisher, frame, rig.Receipt(frame)).HasValue());
            REQUIRE(canvas.bindings->Unregister(canvas.tree, provider, *canvas.layoutEngine).HasValue());
            CHECK_FALSE(canvas.bindings->HasProvider(provider, UiBindingProviderScopeKind::Player));
            CHECK_FALSE(rig.hud.InputEligible(rig.publisher));
            CHECK(rig.hud.ResolveCanvas(rig.publisher).HasError());
            auto store = std::move(*canvas.bindings);
            CHECK_FALSE(canvas.bindings->HasProvider(provider, UiBindingProviderScopeKind::Player));
            store.BeginRetirement();
            CHECK_FALSE(store.HasProvider(provider, UiBindingProviderScopeKind::Player));
            store.Shutdown();
            CHECK_FALSE(store.HasProvider(provider, UiBindingProviderScopeKind::Player));
        }

        TEST_CASE("HUD refresh follows real whole reload and retains historical layout without admission", "[runtime_ui][hud][reload]") {
            Rig rig;
            auto old = rig.Frame();
            rig.Reload(2);
            CHECK_FALSE(rig.hud.InputEligible(rig.publisher));
            CHECK(rig.publisher.Retains(old.Generation()));
            CHECK_FALSE(rig.publisher.IsCurrent(old.Generation()));
            REQUIRE(rig.hud.Refresh(rig.publisher).HasValue());
            CHECK(rig.hud.ApplyPresentation(rig.publisher, old, rig.Receipt(old)).HasError());
            CHECK(old.Layout().Records().size() == 1);
            auto fresh = rig.Frame(2);
            REQUIRE(rig.hud.ApplyPresentation(rig.publisher, fresh, rig.Receipt(fresh)).HasValue());
            CHECK(rig.hud.InputEligible(rig.publisher));
            auto other = Publish(HudGeneration(rig.allocator, 3));
            CHECK(rig.hud.Refresh(other).HasError());
            CHECK(rig.hud.InputEligible(rig.publisher));
            UiReloadLease empty;
            CHECK_FALSE(rig.publisher.Retains(empty));
        }

        TEST_CASE("HUD skipped failed mismatched and replayed renderer receipts fail closed", "[runtime_ui][hud][presentation]") {
            Rig rig;
            auto presented = rig.Frame();
            REQUIRE(rig.hud.ApplyPresentation(rig.publisher, presented, rig.Receipt(presented)).HasValue());
            CHECK(rig.hud.ApplyPresentation(rig.publisher, presented, rig.Receipt(presented)).HasError());
            auto failed = rig.Frame(2);
            auto wrong = rig.Receipt(failed);
            wrong.snapshotRevision = Revision<UiRenderSnapshotRevision>(3);
            CHECK(rig.hud.ApplyPresentation(rig.publisher, failed, wrong).HasError());
            auto invalidReason = rig.Receipt(failed, UiPresentationOutcome::Failed);
            invalidReason.reason = UiPresentationReason::OutputUnavailable;
            CHECK(rig.hud.ApplyPresentation(rig.publisher, failed, invalidReason).HasError());
            REQUIRE(rig.hud.ApplyPresentation(rig.publisher, failed, rig.Receipt(failed, UiPresentationOutcome::Failed)).HasValue());
            CHECK_FALSE(rig.hud.InputEligible(rig.publisher));
            auto skipped = rig.Frame(3);
            REQUIRE(rig.hud.ApplyPresentation(rig.publisher, skipped, rig.Receipt(skipped, UiPresentationOutcome::Skipped)).HasValue());
            CHECK_FALSE(rig.hud.InputEligible(rig.publisher));
        }

        TEST_CASE("HUD lifetime moves shutdown and external publisher retirement invalidate admission", "[runtime_ui][hud][lifetime]") {
            Rig rig;
            auto frame = rig.Frame();
            auto moved = std::move(rig.hud);
            CHECK(rig.hud.ResolveCanvas(rig.publisher).HasError());
            REQUIRE(moved.ApplyPresentation(rig.publisher, frame, rig.Receipt(frame)).HasValue());
            REQUIRE(moved.Shutdown(rig.publisher).HasValue());
            REQUIRE(moved.Shutdown(rig.publisher).HasValue());
            CHECK_FALSE(moved.InputEligible(rig.publisher));
            CHECK(rig.publisher.Current() != nullptr);
            rig.publisher.Shutdown();
            CHECK(rig.publisher.Retains(frame.Generation()));
            CHECK_FALSE(rig.publisher.IsCurrent(frame.Generation()));
            CHECK_FALSE(rig.publisher.CanReclaim());
            CHECK(frame.Layout().Records().size() == 1);
            REQUIRE(moved.Shutdown(rig.publisher).HasValue());
        }

        TEST_CASE("HUD successful frame preparation presentation and queries allocate no frame storage", "[runtime_ui][hud][allocation]") {
            Rig rig;
            const auto before = Tests::AllocationProbe::Count();
            auto metrics = rig.hud.ResolveCanvas(rig.publisher);
            auto frame = rig.hud.PrepareFrame(rig.publisher, Revision<UiRenderSnapshotRevision>(1));
            const auto afterPreparation = Tests::AllocationProbe::Count();
            REQUIRE(metrics.HasValue());
            REQUIRE(frame.HasValue());
            const auto beforeCompletion = Tests::AllocationProbe::Count();
            auto presented = rig.hud.ApplyPresentation(rig.publisher, frame.Value(), rig.Receipt(frame.Value()));
            const auto eligible = rig.hud.InputEligible(rig.publisher);
            const auto after = Tests::AllocationProbe::Count();
            REQUIRE(presented.HasValue());
            CHECK(eligible);
            CHECK(before == afterPreparation);
            CHECK(beforeCompletion == after);
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui::ReloadTests
