#pragma once

// Test-only composition helpers for the owning HUD test translation unit.
#include "Horo/Foundation/Sha256.h"
#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiHudAssociation.h"
#include "UiHotReloadTestFixture.h"
#include "support/AllocationProbe.h"

#include <algorithm>
#include <array>
#include <source_location>

namespace Horo::Runtime::Ui::ReloadTests {
    namespace {
        template <typename T> T Take(Result<T> result, const std::source_location caller = std::source_location::current()) {
            const auto error = result.HasError() ? result.ErrorValue().code.Value() : std::string{};
            INFO(caller.function_name() << ':' << caller.line() << " error=" << error);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        /** @brief Actual schema and read binding for a host-resolved player gameplay value. */
        UiBindingProviderSchema Schema(bool writable = false) {
            const std::array properties{
                UiBindingPropertyDescriptor{.id = Take(UiBindingPropertyId::Parse("alive")),
                                            .type = UiBindingValueType::Boolean,
                                            .access = writable ? UiBindingAccess::ReadWriteCommand : UiBindingAccess::Read}};
            UiBindingProviderDescriptor descriptor{Take(UiBindingProviderTypeId::Parse("game.hud.player")),
                                                   ModuleId{"game.hud"},
                                                   {1, 0, 0},
                                                   properties,
                                                   static_cast<UiBindingProviderScopeMask>(
                                                       UiBindingProviderScopeBit(UiBindingProviderScopeKind::Player) |
                                                       UiBindingProviderScopeBit(UiBindingProviderScopeKind::GameInstance)),
                                                   UiBindingProviderFlags::None};
            descriptor.schema.fingerprint = ComputeUiBindingSchemaFingerprint(descriptor);
            return Take(UiBindingProviderSchema::Create(descriptor));
        }

        UiHudAssociationDescriptor Binding() {
            UiHudAssociationDescriptor result;
            result.id = {Owner(), 40, 1};
            result.player = {Owner(), 3, 1};
            result.viewport = {Owner(), 41, 1};
            result.view = {Owner(), 10, 1};
            result.context = {Owner(), 42, 1};
            result.canvas = Stable<UiCanvasId>(2);
            result.route = Stable<UiRouteId>(9);
            result.viewportEvidence = {{320, 240}, {1, 1}, {10, 20, 10, 20}};
            result.providers[0] = {{Owner(), 43, 1}, UiBindingProviderScopeKind::Player};
            result.providerCount = 1;
            result.interactive = true;
            return result;
        }

        UiCanvasDescriptor Authored(UiRenderMode mode = UiRenderMode::ScreenSpaceOverlay) {
            UiCanvasDescriptor descriptor{Stable<UiCanvasId>(2), Stable<UiElementId>(10)};
            descriptor.renderMode = mode;
            descriptor.scaleMode = UiScaleMode::ConstantPixelSize;
            descriptor.presentation.safeArea = UiSafeAreaMode::Inset;
            return descriptor;
        }

        /** @brief Encodes and loads a real cooked document through Assets; no terminal provenance is manufactured. */
        UiRuntimeAssetLoadResult Load(std::uint64_t version, UiCanvasDescriptor descriptor, bool writable) {
            UiDocumentBuilder builder{Stable<UiDocumentId>(1), Revision<UiDocumentRevision>(version)};
            REQUIRE(builder.AddCanvas(descriptor).HasValue());
            REQUIRE(builder
                        .AddElement(
                            {descriptor.rootElement, {}, Take(Assets::AssetTypeId::Parse(writable ? "core.toggle" : "core.panel")), {}, {}})
                        .HasValue());
            REQUIRE(builder.AddRoute({Stable<UiRouteId>(9), UiPresentationBand::Hud, 0, false}).HasValue());
            const auto document = Take(std::move(builder).Build());
            const auto cooked = Take(CookedUiDocument::Cook(document));
            SerializedUiId bytes{};
            bytes.back() = 7;
            const auto id = Assets::AssetId::FromBytes(bytes);
            const auto type = Take(Assets::AssetTypeId::Parse("runtime.ui.document"));
            Assets::AssetCookArtifact artifact;
            artifact.id = id;
            artifact.type = type;
            artifact.target = Take(AssetCookTargetId::Parse("linux-x64"));
            artifact.payload.assign(cooked.Payload().begin(), cooked.Payload().end());
            artifact.payloadDigest = ComputeSha256(std::as_bytes(std::span{artifact.payload}));
            Assets::AssetRegistry registry;
            REQUIRE(registry
                        .Publish({{id, type, Take(ProjectPath::Parse("assets/hud.horoasset")),
                                   Take(ProjectPath::Parse("assets/hud.horoasset.horo"))}})
                        .status == Assets::AssetRegistryBuildStatus::Complete);
            Assets::MemoryAssetProvider provider;
            provider.Insert(id, Take(Assets::EncodeCookedArtifact(artifact)));
            JobSystem jobs{JobSystemConfig{1, 8}};
            Assets::AssetLoadService assets{jobs, provider};
            UiRuntimeAssetLoadService loader{registry, assets};
            auto loading = Take(
                loader.LoadAsync(registry.Snapshot(), {{id, document.Id(), descriptor.id, document.Revision()}, type, artifact.target}));
            REQUIRE(loading.Wait().HasValue());
            return Take(loading.TakeResult());
        }

        class Evaluator final : public UiLayoutEvaluator {
        public:
            Result<void> ResolveChildConstraints(const UiLayoutChildConstraintRequest &request,
                                                 std::span<UiLayoutConstraints> output) const override {
                for (auto &constraint : output)
                    constraint = request.constraints;
                return Result<void>::Success();
            }

            Result<UiLayoutMeasurement> Measure(const UiLayoutMeasureRequest &request) const override {
                return Result<UiLayoutMeasurement>::Success({request.constraints.maximum, false, false});
            }

            Result<UiLayoutArrangement> Arrange(const UiLayoutArrangeRequest &request, std::span<UiLogicalRect> children) const override {
                for (auto &child : children)
                    child = request.assignedContent;
                return Result<UiLayoutArrangement>::Success({request.assignedContent, request.assignedContent, request.assignedContent,
                                                             request.assignedContent, request.assignedContent, request.assignedContent,
                                                             NoUiBaseline});
            }
        };

        /** @brief Publishes actual layout/clipping with the authored screen policy before exposing a generation. */
        void PublishGeometry(UiReloadCanvas &canvas, std::uint64_t version, UiCanvasDescriptor authored,
                             const UiHudAssociationDescriptor &binding) {
            const auto instance = canvas.tree.Instance();
            canvas.layoutEngine.emplace(Take(UiLayoutEngine::Create(
                {instance, canvas.tree.Canvas(), canvas.tree.SourceDocument(), 4, 4, 4, Revision<UiInteractionRevision>(version)})));
            if (authored.renderMode == UiRenderMode::WorldSpace)
                authored.renderMode = UiRenderMode::ScreenSpaceOverlay;
            const auto metrics = Take(ResolveUiScreenCanvasWithEvidence(authored, binding.viewportEvidence));
            const UiLogicalExtent extent{metrics.logicalExtent.width, metrics.logicalExtent.height};
            Evaluator evaluator;
            const UiLayoutSourceRevisions sources{canvas.tree.SourceDocumentRevision(),   canvas.tree.Revision(),
                                                  canvas.bindings->Current().content,     Revision<UiLayoutStyleRevision>(1),
                                                  Revision<UiLayoutIntrinsicRevision>(1), Revision<UiLayoutCanvasRevision>(1),
                                                  Revision<UiLayoutPolicyRevision>(1)};
            canvas.layout.emplace(Take(
                canvas.layoutEngine->Update(canvas.tree, {sources, {{0, 0}, extent}, {{0, 0}, extent}, &evaluator, metrics.fontScale})));
            canvas.clipping.emplace(
                Take(UiLayoutClipEngine::Create({instance, canvas.tree.Canvas(), canvas.tree.SourceDocument(), 4, 4, 4, 4})));
            canvas.clipPolicies.push_back({canvas.tree.Root().Value().handle, UiLayoutOverflowPolicy::Clip, {}});
            canvas.clipped.emplace(Take(canvas.clipping->Update(canvas.tree, *canvas.layout, {canvas.clipPolicies, {}})));
            canvas.presentations.push_back(Take(UiPresentedInteractionState::Create(binding.view, canvas.tree.Canvas())));
        }

        /** @brief Uses actual copied gameplay registrations and readable or writable typed binding metadata. */
        void InstallBindings(UiReloadCanvas &canvas, const UiHudAssociationDescriptor &binding, UiElementId root, bool writable) {
            auto schema = Schema(writable);
            const std::array values{UiBindingPropertyUpdate{0, true}};
            const auto &property = schema.Properties().front();
            const std::array registrations{UiBindingProviderRegistration{binding.providers[0].instance, binding.providers[0].scope, &schema,
                                                                         Revision<UiBindingSnapshotRevision>(1), values}};
            const std::array bindings{
                UiResolvedBindingDescriptor{binding.providers[0].instance,
                                            {.id = Stable<UiBindingId>(15),
                                             .source = {schema.Type(), property.id, {1, 0}, property.signatureFingerprint},
                                             .target = {root,
                                                        writable ? UiBindingTargetProperty::BooleanValue : UiBindingTargetProperty::Enabled,
                                                        {}},
                                             .direction = writable ? UiBindingDirection::TwoWay : UiBindingDirection::SourceToTarget}}};
            canvas.bindings.emplace(Take(UiBindingStore::Create(canvas.tree, registrations, bindings)));
        }

        /** @brief Prepares real tree, focus, route, provider, layout and clipping owners before generation publication. */
        UiReloadGeneration HudGeneration(UiElementSlotAllocator &allocator, std::uint64_t version, UiCanvasDescriptor authored = Authored(),
                                         UiHudAssociationDescriptor binding = Binding(), RuntimeUiInstanceId instance = Instance(),
                                         bool writable = false) {
            auto loaded = Load(version, authored, writable);
            const std::array elements{UiElementDescriptor{authored.rootElement, {}}};
            UiReloadCanvas canvas{authored.id, Take(UiElementTree::Create(allocator,
                                                                          {instance,
                                                                           {Owner(), 100 + instance.slot, 1},
                                                                           loaded.document.Id(),
                                                                           loaded.document.SourceRevision(),
                                                                           Revision<UiRuntimeTreeRevision>(version),
                                                                           {4, 4, 4}},
                                                                          elements))};
            const auto root = Take(canvas.tree.Root());
            const auto interaction = Revision<UiInteractionRevision>(version);
            const std::array nodes{
                UiFocusNodeDescriptor{root.handle, root.id, {}, {}, UiFocusBringIntoViewPolicy::Nearest, true, true, true}};
            canvas.focus.emplace(Take(UiFocusGraph::Create({{instance,
                                                             canvas.tree.Canvas(),
                                                             canvas.tree.SourceDocument(),
                                                             canvas.tree.SourceDocumentRevision(),
                                                             canvas.tree.Revision(),
                                                             interaction,
                                                             {binding.player, UiFocusPresentationLayerId{Owner(), 200 + instance.slot, 1}}},
                                                            authored.rootElement,
                                                            UiFocusRecoveryPolicy::AncestorThenDefaultThenFirst,
                                                            4,
                                                            4,
                                                            4},
                                                           nodes)));
            canvas.routes.emplace(Take(UiScreenStack::Create({Owner(), {Owner(), root.handle.slot, 1}, loaded.document.Routes(), 4})));
            canvas.captures.emplace(Take(UiPointerCaptureStore::Create({Owner(), 4})));
            if (writable) {
                const UiActionOwnerContext owner{instance,
                                                 canvas.tree.Canvas(),
                                                 canvas.tree.SourceDocument(),
                                                 canvas.tree.SourceDocumentRevision(),
                                                 canvas.tree.Revision(),
                                                 interaction};
                canvas.controls.push_back({root.id, Take(UiControlStateMachine::Create(
                                                        UiToggleControlDescriptor{{owner, root.handle, Stable<UiActionId>(90)}, true}))});
                canvas.actions.emplace(Take(UiActionRouter::Create({owner, 4})));
            }
            InstallBindings(canvas, binding, authored.rootElement, writable);
            PublishGeometry(canvas, version, authored, binding);
            std::vector<UiReloadCanvas> canvases;
            canvases.push_back(std::move(canvas));
            return Take(UiReloadGeneration::Create(std::move(loaded), instance, std::move(canvases)));
        }

        /** @brief Activates the actual HUD route only after whole-generation admission accepts an empty candidate stack. */
        UiHotReload Publish(UiReloadGeneration generation) {
            auto publisher = Take(UiHotReload::Create(std::move(generation)));
            REQUIRE(Take(Canvas(publisher).routes->Push(Stable<UiRouteId>(9))).IsCommitted());
            return publisher;
        }

        UiPointerCaptureToken CaptureFor(UiHotReload &publisher, const UiHudAssociationDescriptor &binding) {
            auto &canvas = Canvas(publisher);
            const auto target = canvas.tree.Root().Value().handle;
            const UiEventRoute route{canvas.tree.Instance(),
                                     canvas.tree.Canvas(),
                                     canvas.tree.SourceDocument(),
                                     canvas.tree.Revision(),
                                     canvas.layout->Descriptor().interaction,
                                     target,
                                     std::nullopt};
            return Take(
                canvas.captures->Capture({binding.context, UiPointerId::Create(1).Value(), UiPointerButton::Primary, binding.view, route},
                                         canvas.tree, canvas.presentations.front()));
        }

        /** @brief Real pending provider authority that attempts HUD owner operations during deferred retirement. */
        class RetirementAuthority final : public UiBindingWriteAuthority {
        public:
            UiBindingWriteFence fence;
            UiHotReload *publisher{};
            UiHudAssociation *hud{};
            UiPointerCaptureToken *capture{};
            std::size_t abandons{};
            bool busy{};
            bool capturePreserved{};
            bool policyPreserved{};
            bool reentryRejected{};

            const UiBindingWriteFence &Fence() const noexcept override {
                return fence;
            }

            bool Active() const noexcept override {
                return true;
            }

            Result<UiBindingWriteDisposition> Prepare(const UiBindingWriteCommand &) noexcept override {
                return Result<UiBindingWriteDisposition>::Success(UiBindingWriteDisposition::Pending);
            }

            void Commit(const UiBindingWriteCommand &) noexcept override {}

            void Abandon(const UiBindingWriteCommand &) noexcept override {
                ++abandons;
                const auto original = hud->Descriptor();
                const auto result = hud->Shutdown(*publisher);
                busy = result.HasError() && result.ErrorValue().code.Value() == UiErrors::InstanceStateInvalid.code.Value();
                reentryRejected = hud->SetPolicy(*publisher, false, false).HasError();
                auto reassociated = original;
                reassociated.camera = UiHudCameraId{Owner(), 44, 1};
                reentryRejected = reentryRejected && hud->Reassociate(reassociated, *publisher).HasError();
                policyPreserved = hud->Descriptor() == original;
                capturePreserved = capture->IsActive();
            }
        };

        void QueuePending(UiHotReload &publisher, const std::shared_ptr<RetirementAuthority> &authority) {
            auto &canvas = Canvas(publisher);
            const auto owner = canvas.controls.front().control.Owner();
            const auto schema = Schema(true);
            authority->fence = {Binding().providers[0].instance,
                                UiBindingProviderScopeKind::Player,
                                schema.Version(),
                                0,
                                schema.Properties().front().signatureFingerprint,
                                100};
            const std::array admissions{UiBindingWriteAdmission{Stable<UiBindingId>(15), owner, Stable<UiActionId>(90),
                                                                UiBindingCommitTrigger::Change, UiBindingConflictPolicy::RejectStale,
                                                                authority}};
            REQUIRE(canvas.bindings->AdmitWrites(canvas.tree, admissions).HasValue());
            const UiActionSource source{owner, canvas.controls.front().control.Element()};
            const auto edit = Take(canvas.bindings->BeginEdit(canvas.tree, Stable<UiBindingId>(15), source));
            UiActionPayload payload;
            REQUIRE(payload.Add(false).HasValue());
            REQUIRE(canvas.actions->Enqueue(source, UiGameplayActionCommand{Stable<UiActionId>(90), payload}).HasValue());
            auto request = Take(canvas.actions->TryDequeue());
            REQUIRE(request.has_value());
            REQUIRE(canvas.bindings->QueueWrite(canvas.tree, edit, *request, UiBindingCommitTrigger::Change).HasValue());
            const auto processed = Take(canvas.bindings->ProcessWrite(canvas.tree, *canvas.layoutEngine));
            REQUIRE(processed.has_value());
            REQUIRE(processed->disposition == UiBindingWriteDisposition::Pending);
        }

        struct Rig final {
            UiElementSlotAllocator allocator{Take(UiElementSlotAllocator::Create(Owner()))};
            UiHotReload publisher{Publish(HudGeneration(allocator, 1))};
            UiHudAssociation hud{Take(UiHudAssociation::Create(Binding(), publisher))};

            UiHudFrame Frame(std::uint64_t snapshot = 1) {
                return Take(hud.PrepareFrame(publisher, Revision<UiRenderSnapshotRevision>(snapshot)));
            }

            UiPresentationReceipt Receipt(const UiHudFrame &frame, UiPresentationOutcome outcome = UiPresentationOutcome::Presented) {
                const auto reason = outcome == UiPresentationOutcome::Presented ? UiPresentationReason::None
                                    : outcome == UiPresentationOutcome::Skipped ? UiPresentationReason::OutputUnavailable
                                                                                : UiPresentationReason::ExecutionFailure;
                return {frame.Association().view,
                        frame.Layout().Descriptor().canvas,
                        frame.Layout().Descriptor().interaction,
                        frame.Snapshot(),
                        outcome,
                        reason};
            }

            void Reload(std::uint64_t version) {
                auto prepared = Take(publisher.Prepare(HudGeneration(allocator, version)));
                REQUIRE(publisher.Commit(prepared, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands).HasValue());
            }

            UiPointerCaptureToken Capture() {
                return CaptureFor(publisher, hud.Descriptor());
            }
        };

    }  // namespace
}  // namespace Horo::Runtime::Ui::ReloadTests
