#include "Horo/Runtime/Ui/RuntimeUiService.h"
#include "Horo/Runtime/Ui/UiRenderGeometry.h"
#include "UiAnimationOwnerIntegrationFixture.h"
#include "support/AllocationProbe.h"

#include <array>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;
    using namespace Horo::Runtime::Ui;
    using namespace Horo::Runtime::Ui::ReloadTests;
    using namespace Horo::Runtime::Ui::AnimationTests;
    constexpr auto Cutoff = UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands;
    constexpr SceneRuntimeId First{101};

    template <typename T> T Take(Result<T> result) {
        INFO((result.HasError() ? result.ErrorValue().message : ""));
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    /** @brief Real cooked/private canonical Scene publisher with the actual style/animation owner attached before activation. */
    struct Incoming final {
        UiElementSlotAllocator allocator = Take(UiElementSlotAllocator::Create(Owner()));
        RuntimeUiService service = Take(RuntimeUiService::Create(Take(UiSceneReconciliation::Create({Owner()}))));
        std::optional<UiSceneReconciliation::Prepared> prepared;
        std::optional<UiAnimationOwner> animation;
        CancellationSource cancellation;

        Incoming() {
            auto generation = Generation(allocator, 1, 32, false, 0, 0, 4);
            auto resources = Resources(generation.Canvases().front());
            std::vector<UiSceneActivation> incoming;
            incoming.push_back({{Instance(), {.kind = UiOwnerScopeKind::Scene, .scene = First}, {}}, std::move(generation)});
            prepared.emplace(Take(service.PrepareTransition({{}, First}, {}, std::move(incoming), cancellation.Token())));
            animation.emplace(Take(service.PrepareAnimation(*prepared, Instance(), allocator, std::move(resources.registry),
                                                            std::move(resources.styles), std::move(resources.definition))));
        }

        RuntimeUiService::ScenePublication Publication() {
            return Take(service.PreparePublication(std::move(*prepared)));
        }
    };

    /** @brief Actual Scene-provider binding with stable authored provenance and independently advanced runtime owners. */
    UiReloadGeneration BoundGeneration(UiElementSlotAllocator &allocator, const std::uint64_t runtime, const std::uint32_t providerSlot) {
        LayerOptions layer;
        layer.runtimeRevision = runtime;
        auto generation = Generation(allocator, 1, 32, false, 0, 0, 4, layer);
        const std::array properties{
            UiBindingPropertyDescriptor{.id = Take(UiBindingPropertyId::Parse("available")), .type = UiBindingValueType::Boolean}};
        UiBindingProviderDescriptor descriptor{Take(UiBindingProviderTypeId::Parse("game.scene.values")),
                                               ModuleId{"game.scene"},
                                               {1, 0, 0},
                                               properties,
                                               UiBindingProviderScopeBit(UiBindingProviderScopeKind::Scene)};
        descriptor.schema.fingerprint = ComputeUiBindingSchemaFingerprint(descriptor);
        const auto schema = Take(UiBindingProviderSchema::Create(descriptor));
        const UiBindingProviderInstanceId provider{Owner(), providerSlot, 1};
        const std::array values{UiBindingPropertyUpdate{0, true}};
        const std::array registrations{UiBindingProviderRegistration{provider, UiBindingProviderScopeKind::Scene, &schema,
                                                                     Revision<UiBindingSnapshotRevision>(1), values}};
        const auto &property = schema.Properties().front();
        const std::array bindings{
            UiResolvedBindingDescriptor{provider,
                                        {.id = Stable<UiBindingId>(33),
                                         .source = {schema.Type(), property.id, {1, 0}, property.signatureFingerprint},
                                         .target = {Stable<UiElementId>(10), UiBindingTargetProperty::Enabled, {}}}}};
        auto &canvas = *generation.Canvas(Stable<UiCanvasId>(2));
        canvas.bindings.emplace(Take(UiBindingStore::Create(canvas.tree, registrations, bindings)));
        return generation;
    }

    /** @brief Same service-owned persistent publisher, with distinct active and private derived owners. */
    struct PersistentRebind final {
        UiElementSlotAllocator allocator = Take(UiElementSlotAllocator::Create(Owner()));
        RuntimeUiService service = Take(RuntimeUiService::Create(Take(UiSceneReconciliation::Create({Owner()}))));
        UiAnimationOwner active = CreateActive();
        UiReloadLease previous = Take(service.Acquire(Instance()));
        UiPresentationAttachmentId activeAttachment = Take(active.Attach({Owner(), 31, 1}, Config().viewport));
        std::optional<UiSceneReconciliation::Prepared> prepared;
        std::optional<UiAnimationOwner> candidate;
        std::optional<UiPresentationAttachmentId> candidateAttachment;

        UiAnimationOwner CreateActive() {
            auto generation = BoundGeneration(allocator, 1, 301);
            auto resources = Resources(generation.Canvases().front());
            auto publisher = Take(UiHotReload::Create(std::move(generation)));
            REQUIRE(service.Admit({Instance(), {}, First}, std::move(publisher), Cutoff).HasValue());
            return Take(UiAnimationOwner::Create(service, Instance(), allocator, std::move(resources.registry), std::move(resources.styles),
                                                 std::move(resources.definition)));
        }

        void Prepare() {
            auto generation = BoundGeneration(allocator, 2, 302);
            auto resources = Resources(generation.Canvases().front());
            std::vector<UiSceneBindingReplacement> replacements;
            replacements.push_back({Instance(), std::move(generation)});
            const std::array sources{Instance()};
            prepared.emplace(Take(service.PrepareTransition({First, SceneRuntimeId{102}}, std::move(replacements), {}, sources)));
            candidate.emplace(Take(service.PrepareAnimation(*prepared, Instance(), allocator, std::move(resources.registry),
                                                            std::move(resources.styles), std::move(resources.definition))));
            candidateAttachment = Take(candidate->Attach({Owner(), 32, 1}, Config().viewport));
            CHECK(Take(service.Acquire(Instance())).Get() == previous.Get());
            CHECK(previous.Get()->Canvases().front().tree.Revision() == Revision<UiRuntimeTreeRevision>(1));
            CHECK(active.UpdateAttachment(activeAttachment, Config().viewport).HasValue());
        }
    };
}  // namespace

TEST_CASE("Initial Scene publication promotes the exact private animation publisher without allocating",
          "[runtime_ui][animation][scene][publication]") {
    Incoming fixture;
    REQUIRE(fixture.service.Acquire(Instance()).HasError());
    const auto attachment = Take(fixture.animation->Attach({Owner(), 21, 1}, Config().viewport));
    auto publication = fixture.Publication();
    REQUIRE(publication.Seal(Cutoff).HasValue());
    const auto fenced = fixture.service.Acquire(Instance());
    REQUIRE(fenced.HasError());
    CHECK(fenced.ErrorValue().code.Value() == UiErrors::CapabilityBusy.code.Value());
    Horo::Tests::AllocationProbe::Measurement measured;
    {
        Horo::Tests::AllocationProbe::ScopedMeasurement probe;
        publication.Publish();
        measured = probe.Snapshot();
    }
    CHECK(measured.requests == 0);
    const auto active = Take(fixture.service.Acquire(Instance()));
    CHECK(active.Get()->Instance().InstanceId() == Instance());
    CHECK(fixture.animation->Detach(attachment).HasValue());
    CHECK(publication.Seal(Cutoff).HasError());
}

TEST_CASE("Abandoned or cancelled private Scene animation cannot retain mutation authority",
          "[runtime_ui][animation][scene][publication]") {
    Incoming fixture;
    const auto attachment = Take(fixture.animation->Attach({Owner(), 21, 1}, Config().viewport));
    fixture.prepared.reset();
    CHECK(fixture.animation->UpdateAttachment(attachment, Config().viewport).HasError());
    CHECK(fixture.service.Acquire(Instance()).HasError());
    Incoming cancelled;
    auto publication = cancelled.Publication();
    cancelled.cancellation.RequestCancellation();
    CHECK(publication.Seal(Cutoff).HasError());
    CHECK(cancelled.service.Acquire(Instance()).HasError());
}

TEST_CASE("Sealed Scene publication is owner-thread fenced and rollback restores command admission",
          "[runtime_ui][animation][scene][publication]") {
    Incoming fixture;
    {
        auto publication = fixture.Publication();
        std::optional<Error> failure;
        std::thread other([&] {
            auto result = publication.Seal(Cutoff);
            if (result.HasError())
                failure = result.ErrorValue();
        });
        other.join();
        REQUIRE(failure);
        CHECK(failure->code.Value() == UiErrors::CapabilityWrongThread.code.Value());
        REQUIRE(publication.Seal(Cutoff).HasValue());
        const auto reentry = fixture.service.ApplyQueuedCommands(RuntimeUiCommandCutoff::OwnerCommands);
        REQUIRE(reentry.HasError());
        CHECK(reentry.ErrorValue().code.Value() == UiErrors::CapabilityBusy.code.Value());
    }
    CHECK(fixture.service.ApplyQueuedCommands(RuntimeUiCommandCutoff::OwnerCommands).HasValue());
    CHECK(fixture.service.Acquire(Instance()).HasError());
}

TEST_CASE("Coordinated Scene unload preserves game scope and retained canonical generation leases",
          "[runtime_ui][animation][scene][publication]") {
    Incoming fixture;
    auto activated = fixture.Publication();
    REQUIRE(activated.Seal(Cutoff).HasValue());
    activated.Publish();
    auto old = Take(fixture.service.Acquire(Instance()));
    LayerOptions gameLayer;
    gameLayer.instance = {Owner(), 2, 1};
    gameLayer.canvas.slot = 102;
    gameLayer.layer.slot = 202;
    auto game = Take(UiHotReload::Create(Generation(fixture.allocator, 1, 32, false, 0, 0, 4, gameLayer)));
    REQUIRE(fixture.service.Admit({gameLayer.instance, {}}, std::move(game), Cutoff).HasValue());
    const auto gameBefore = Take(fixture.service.Acquire(gameLayer.instance));
    const std::array sources{Instance()};
    auto prepared = Take(fixture.service.PrepareTransition({First, {}}, {}, {}, sources));
    auto publication = Take(fixture.service.PreparePublication(std::move(prepared)));
    REQUIRE(publication.Seal(Cutoff).HasValue());
    publication.Publish();
    CHECK(fixture.service.Acquire(Instance()).HasError());
    CHECK(Take(fixture.service.Acquire(gameLayer.instance)).Get() == gameBefore.Get());
    CHECK(old.Get()->Instance().State() == UiRuntimeInstanceState::Retiring);
    CHECK(Take(fixture.service.CollectRetired()) == 0);
    fixture.animation->Shutdown();
    REQUIRE(fixture.animation->DrainRetired().HasValue());
    fixture.animation.reset();
    CHECK(old.Get() != nullptr);
}

TEST_CASE("Initial Scene activation preserves unrelated game player and Scene publishers", "[runtime_ui][animation][scene][publication]") {
    auto allocator = Take(UiElementSlotAllocator::Create(Owner()));
    auto service = Take(RuntimeUiService::Create(Take(UiSceneReconciliation::Create({Owner()}))));
    std::array<UiReloadLease, 3> previous;
    for (std::uint32_t index = 0; index < previous.size(); ++index) {
        LayerOptions layer;
        layer.instance = {Owner(), index + 1, 1};
        layer.canvas.slot = 100 + index;
        layer.layer.slot = 200 + index;
        UiSemanticOwner semantic;
        if (index == 1)
            semantic = {.kind = UiOwnerScopeKind::Player, .player = layer.player};
        else if (index == 2)
            semantic = {.kind = UiOwnerScopeKind::Scene, .scene = SceneRuntimeId{90}};
        auto publisher = Take(UiHotReload::Create(Generation(allocator, 1, 32, false, 0, 0, 4, layer)));
        REQUIRE(service.Admit({layer.instance, semantic}, std::move(publisher), Cutoff).HasValue());
        previous[index] = Take(service.Acquire(layer.instance));
    }
    auto prepared = Take(service.PrepareTransition({{}, First}, {}, {}));
    auto publication = Take(service.PreparePublication(std::move(prepared)));
    REQUIRE(publication.Seal(Cutoff).HasValue());
    publication.Publish();
    for (std::uint32_t index = 0; index < previous.size(); ++index)
        CHECK(Take(service.Acquire({Owner(), index + 1, 1})).Get() == previous[index].Get());
}

TEST_CASE("Scene revocation invalidates an exact zero-animation private publication and releases its bounded slot",
          "[runtime_ui][animation][scene][publication]") {
    auto allocator = Take(UiElementSlotAllocator::Create(Owner()));
    auto registry = Take(UiSceneReconciliation::Create({.ownership = Owner(), .maximumPreparedTransitions = 1}));
    auto service = Take(RuntimeUiService::Create(std::move(registry)));
    std::vector<UiSceneActivation> incoming;
    incoming.push_back({{Instance(), {.kind = UiOwnerScopeKind::Scene, .scene = First}}, Generation(allocator, 1)});
    auto prepared = Take(service.PrepareTransition({{}, First}, {}, std::move(incoming)));
    auto publication = std::make_optional(Take(service.PreparePublication(std::move(prepared))));
    REQUIRE(service.PrepareTransition({{}, SceneRuntimeId{102}}, {}, {}).HasError());
    SECTION("unsealed") {
        service.RevokeScene(First);
        CHECK(publication->Seal(Cutoff).HasError());
    }
    SECTION("sealed before the Scene claim") {
        REQUIRE(publication->Seal(Cutoff).HasValue());
        REQUIRE(publication->CanPublish().HasValue());
        service.RevokeScene(First);
        CHECK(publication->CanPublish().HasError());
    }
    CHECK(service.Acquire(Instance()).HasError());
    publication.reset();
    auto reused = Take(service.PrepareTransition({{}, SceneRuntimeId{102}}, {}, {}));
    auto next = Take(service.PreparePublication(std::move(reused)));
    REQUIRE(next.Seal(Cutoff).HasValue());
    REQUIRE(next.CanPublish().HasValue());
    next.Publish();
    CHECK(service.Acquire(Instance()).HasError());
}

TEST_CASE("A forced old Scene revocation preserves persistent ownership and allows exact provider recovery",
          "[runtime_ui][animation][scene][publication]") {
    PersistentRebind fixture;
    fixture.service.RevokeScene(First);
    const auto unavailable = fixture.service.Acquire(Instance());
    REQUIRE(unavailable.HasError());
    CHECK(unavailable.ErrorValue().code.Value() == UiErrors::BindingProviderUnknown.code.Value());
    CHECK(fixture.active.UpdateAttachment(fixture.activeAttachment, Config().viewport).HasError());
    auto generation = BoundGeneration(fixture.allocator, 2, 302);
    auto resources = Resources(generation.Canvases().front());
    std::vector<UiSceneBindingReplacement> replacements;
    replacements.push_back({Instance(), std::move(generation)});
    auto prepared = Take(fixture.service.PrepareTransition({First, SceneRuntimeId{102}}, std::move(replacements)));
    auto nextOwner = Take(fixture.service.PrepareAnimation(prepared, Instance(), fixture.allocator, std::move(resources.registry),
                                                           std::move(resources.styles), std::move(resources.definition)));
    auto publication = Take(fixture.service.PreparePublication(std::move(prepared)));
    REQUIRE(publication.Seal(Cutoff).HasValue());
    REQUIRE(publication.CanPublish().HasValue());
    publication.Publish();
    const auto recovered = Take(fixture.service.Acquire(Instance()));
    CHECK(recovered.Get() != fixture.previous.Get());
    fixture.service.RevokeScene(First);
    CHECK(Take(fixture.service.Acquire(Instance())).Get() == recovered.Get());
    fixture.active.Shutdown();
    CHECK(Take(fixture.service.Acquire(Instance())).Get() == recovered.Get());
    REQUIRE(nextOwner.Attach({Owner(), 32, 1}, Config().viewport).HasValue());
}

TEST_CASE("Shutdown during a sealed Scene candidate revokes callers and abandons without activation",
          "[runtime_ui][animation][scene][publication]") {
    Incoming fixture;
    {
        auto publication = fixture.Publication();
        REQUIRE(publication.Seal(Cutoff).HasValue());
        fixture.service.Shutdown();
        CHECK(fixture.service.Acquire(Instance()).HasError());
        CHECK_FALSE(Take(fixture.service.CanReclaim()));
    }
    CHECK(fixture.service.Acquire(Instance()).HasError());
    fixture.animation->Shutdown();
    REQUIRE(fixture.animation->DrainRetired().HasValue());
    fixture.animation.reset();
    REQUIRE(fixture.service.CollectRetired().HasValue());
    CHECK(Take(fixture.service.CanReclaim()));
}

TEST_CASE("Persistent Scene provider publication promotes only its exact prepared animation generation",
          "[runtime_ui][animation][scene][publication]") {
    PersistentRebind fixture;
    fixture.Prepare();
    auto publication = Take(fixture.service.PreparePublication(std::move(*fixture.prepared)));
    REQUIRE(publication.Seal(Cutoff).HasValue());
    publication.Publish();
    const auto current = Take(fixture.service.Acquire(Instance()));
    CHECK(current.Get() != fixture.previous.Get());
    CHECK(current.Get()->Canvases().front().tree.Revision() == Revision<UiRuntimeTreeRevision>(2));
    CHECK(fixture.previous.Get()->Canvases().front().tree.Revision() == Revision<UiRuntimeTreeRevision>(1));
    CHECK(fixture.active.UpdateAttachment(fixture.activeAttachment, Config().viewport).HasError());
    CHECK(fixture.candidate->UpdateAttachment(*fixture.candidateAttachment, Config().viewport).HasValue());
}

TEST_CASE("Abandoning a prepared persistent Scene provider preserves the old animation publisher",
          "[runtime_ui][animation][scene][publication]") {
    PersistentRebind fixture;
    fixture.Prepare();
    fixture.prepared.reset();
    CHECK(Take(fixture.service.Acquire(Instance())).Get() == fixture.previous.Get());
    CHECK(fixture.previous.Get()->Canvases().front().tree.Revision() == Revision<UiRuntimeTreeRevision>(1));
    CHECK(fixture.candidate->UpdateAttachment(*fixture.candidateAttachment, Config().viewport).HasError());
    CHECK(fixture.active.UpdateAttachment(fixture.activeAttachment, Config().viewport).HasValue());
}

TEST_CASE("Private source baseline prepares canonical presentation without host time or input authority",
          "[runtime_ui][animation][scene][publication][prototype]") {
    Incoming fixture;
    DeterministicClock clock;
    auto host = Take(RuntimeHost::Create(clock));
    auto composition = Take(UiAnimationRuntimeParticipant::Compose(std::move(*fixture.animation), host->DispatchSource(), Config()));
    fixture.animation.reset();
    auto *participant = composition.participant.get();
    const auto baseline = Take(participant->PrepareSourcePresentation());
    CHECK(baseline.Layout().Descriptor().sources.document == Revision<UiDocumentRevision>(1));
    CHECK(baseline.Clipping() != nullptr);
    CHECK(participant->Acquire().HasError());
    CHECK(participant->PrepareSourcePresentation().HasError());
    const UiPresentationReceipt fabricated{{Owner(), 10, 1},
                                           baseline.Layout().Descriptor().canvas,
                                           baseline.Layout().Descriptor().interaction,
                                           Revision<UiRenderSnapshotRevision>(1),
                                           UiPresentationOutcome::Presented,
                                           UiPresentationReason::None};
    CHECK(participant->ApplyPresentation(fabricated).HasError());
    CHECK_FALSE(participant->InputEligible(fabricated.view));
    CHECK(fixture.service.Acquire(Instance()).HasError());
    auto publication = fixture.Publication();
    REQUIRE(publication.Seal(Cutoff).HasValue());
    publication.Publish();
    CHECK(participant->Acquire().HasError());
    REQUIRE(host->AddParticipant(std::move(composition.participant)).HasValue());
    REQUIRE(host->Startup().HasValue());
    REQUIRE(host->RunFrame().HasValue());
    const auto frame = Take(participant->Acquire());
    CHECK(frame.Clocks().updateSequence == 1);
    for (const auto &domain : frame.Clocks().domains)
        CHECK(domain.elapsed.nanoseconds == 0);
    CHECK_FALSE(participant->InputEligible(fabricated.view));
    CHECK(baseline.Layout().Descriptor().sources.document == frame.Layout().Descriptor().sources.document);
    host->Shutdown();
}

TEST_CASE("Animated render extraction pins exact source issuance until pooled readers retire",
          "[runtime_ui][animation][scene][publication][render_lifetime]") {
    Incoming fixture;
    DeterministicClock clock;
    auto host = Take(RuntimeHost::Create(clock));
    auto composition = Take(UiAnimationRuntimeParticipant::Compose(std::move(*fixture.animation), host->DispatchSource(), Config()));
    fixture.animation.reset();
    auto *participant = composition.participant.get();
    auto publication = fixture.Publication();
    REQUIRE(publication.Seal(Cutoff).HasValue());
    publication.Publish();
    REQUIRE(host->AddParticipant(std::move(composition.participant)).HasValue());
    REQUIRE(host->Startup().HasValue());
    REQUIRE(host->RunFrame().HasValue());
    auto frame = Take(participant->Acquire());
    auto source = Take(fixture.service.Acquire(Instance()));
    const auto &tree = source.Get()->Canvases().front().tree;
    const auto &layout = frame.Layout().Descriptor();
    const UiRenderViewId view{Owner(), 10, 1};
    const UiRenderSnapshotLimits limits{1, 0, 0, 0, 0, 1, 0};
    const UiRenderSnapshotDescriptor descriptor{layout.instance,
                                                layout.canvas,
                                                layout.document,
                                                layout.sources.document,
                                                layout.sources.tree,
                                                layout.interaction,
                                                Revision<UiRenderSnapshotRevision>(1),
                                                view,
                                                limits};
    auto extractor = Take(UiRenderExtractor::Create({view, limits, 1}));
    auto arena = Take(UiRenderGeometryArena::Create({view, {4, 6, 1}, 1}));
    const std::array transforms{UiLogicalTransform{}};
    const std::array commands{UiDrawCommand{tree.Root().Value().handle,
                                            {{0, 0}, {4, 4}},
                                            0,
                                            NoUiRenderIndex,
                                            NoUiRenderIndex,
                                            1.0F,
                                            UiSolidDraw{{1.0F, 1.0F, 1.0F, 1.0F}}}};
    const UiRenderProjection projection{commands, {}, {}, {}, {}, transforms, {}, {}};

    SECTION("independent source with equal numeric lineage cannot borrow the frame") {
        auto foreignAllocator = Take(UiElementSlotAllocator::Create(Owner()));
        auto foreign = Generation(foreignAllocator, 1, 32, false, 0, 0, 4);
        const auto &otherTree = foreign.Canvases().front().tree;
        REQUIRE(otherTree.Instance() == tree.Instance());
        REQUIRE(otherTree.Canvas() == tree.Canvas());
        REQUIRE(otherTree.Revision() == tree.Revision());
        CHECK(extractor.Extract(otherTree, descriptor, projection, frame).HasError());
        CHECK(extractor.IsDrained());
        CHECK(extractor.Extract(tree, descriptor, projection, UiAnimationFrameLease{}).HasError());
        CHECK(extractor.IsDrained());
        auto snapshot = Take(extractor.Extract(tree, descriptor, projection, frame));
        CHECK(snapshot.IsValid());
    }
    SECTION("same-issued frame copies extract and build without success allocations") {
        const auto frameCopy = frame;
        std::optional<UiRenderSnapshot> snapshot;
        std::optional<UiRenderGeometryPlan> geometry;
        Horo::Tests::AllocationProbe::Measurement measured;
        {
            Horo::Tests::AllocationProbe::ScopedMeasurement probe;
            auto extracted = extractor.Extract(tree, descriptor, projection, frameCopy);
            if (extracted.HasValue()) {
                snapshot.emplace(std::move(extracted).Value());
                auto built = arena.Build(*snapshot);
                if (built.HasValue())
                    geometry.emplace(std::move(built).Value());
            }
            measured = probe.Snapshot();
        }
        REQUIRE(snapshot.has_value());
        REQUIRE(geometry.has_value());
        CHECK(measured.requests == 0);
    }
    SECTION("unsent snapshot copies release without extractor reuse") {
        std::optional<UiRenderSnapshot> snapshot{Take(extractor.Extract(tree, descriptor, projection, frame))};
        std::optional<UiRenderSnapshot> copy{*snapshot};
        frame = {};
        source = {};
        host->Shutdown();
        REQUIRE(participant->DrainRetired().HasValue());
        CHECK_FALSE(participant->CanReclaim());
        snapshot.reset();
        CHECK_FALSE(extractor.IsDrained());
        std::thread reader([retained = std::move(copy)]() mutable {
            retained.reset();
        });
        reader.join();
        CHECK(extractor.IsDrained());
        REQUIRE(participant->DrainRetired().HasValue());
        CHECK(participant->CanReclaim());
    }
    SECTION("failed geometry preparation releases its source before making the slot reusable") {
        auto smallArena = Take(UiRenderGeometryArena::Create({view, {1, 1, 1}, 1}));
        std::optional<UiRenderSnapshot> snapshot{Take(extractor.Extract(tree, descriptor, projection, frame))};
        CHECK(smallArena.Build(*snapshot).HasError());
        CHECK(smallArena.IsDrained());
        frame = {};
        source = {};
        snapshot.reset();
        CHECK(extractor.IsDrained());
        host->Shutdown();
        REQUIRE(participant->DrainRetired().HasValue());
        CHECK(participant->CanReclaim());
    }
    SECTION("geometry copies release both pooled source pins without arena reuse") {
        std::optional<UiRenderSnapshot> snapshot{Take(extractor.Extract(tree, descriptor, projection, frame))};
        std::optional<UiRenderGeometryPlan> geometry{Take(arena.Build(*snapshot))};
        std::optional<UiRenderGeometryPlan> copy{*geometry};
        frame = {};
        source = {};
        snapshot.reset();
        host->Shutdown();
        REQUIRE(participant->DrainRetired().HasValue());
        CHECK_FALSE(participant->CanReclaim());
        geometry.reset();
        CHECK_FALSE(arena.IsDrained());
        CHECK_FALSE(extractor.IsDrained());
        std::thread reader([retained = std::move(copy)]() mutable {
            retained.reset();
        });
        reader.join();
        CHECK(arena.IsDrained());
        CHECK(extractor.IsDrained());
        REQUIRE(participant->DrainRetired().HasValue());
        CHECK(participant->CanReclaim());
    }
    host->Shutdown();
}
