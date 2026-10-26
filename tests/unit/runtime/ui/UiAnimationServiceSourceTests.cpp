#include "Horo/Runtime/Ui/RuntimeUiService.h"
#include "UiAnimationOwnerIntegrationFixture.h"

#include <algorithm>
#include <limits>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;
    using namespace Horo::Runtime::Ui;
    using namespace Horo::Runtime::Ui::ReloadTests;
    using namespace Horo::Runtime::Ui::AnimationTests;

    template <typename T> T Take(Result<T> result) {
        INFO((result.HasError() ? result.ErrorValue().message : ""));
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    constexpr auto Cutoff = UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands;

    struct ServiceSource final {
        UiElementSlotAllocator allocator = Take(UiElementSlotAllocator::Create(Owner()));
        RuntimeUiService service = Take(RuntimeUiService::Create(Take(UiSceneReconciliation::Create({Owner()}))));
        std::optional<UiAnimationOwner> animation;

        explicit ServiceSource(bool sceneOwned = false, bool attach = true) {
            auto generation = Generation(allocator, 1, 32, false, 0, 0, 4);
            auto publisher = Take(UiHotReload::Create(std::move(generation)));
            UiSceneInstanceDescriptor descriptor{Instance(), {}};
            if (sceneOwned)
                descriptor.owner = {.kind = UiOwnerScopeKind::Scene, .scene = SceneRuntimeId{101}};
            REQUIRE(service.Admit(descriptor, std::move(publisher), Cutoff).HasValue());
            if (attach)
                Attach();
        }

        void Attach() {
            const auto source = Take(service.Acquire(Instance()));
            auto resources = Resources(source.Get()->Canvases().front());
            animation.emplace(Take(UiAnimationOwner::Create(service, Instance(), allocator, std::move(resources.registry),
                                                            std::move(resources.styles), std::move(resources.definition))));
        }

        UiAnimationReloadPreparation Replacement(std::uint64_t version, std::uint64_t runtimeRevision = 0) {
            LayerOptions layer;
            layer.runtimeRevision = runtimeRevision;
            auto generation = Generation(allocator, version, 32, false, 0, 0, 4, layer);
            auto resources = Resources(generation.Canvases().front());
            return {std::move(generation),           allocator,
                    std::move(resources.registry),   std::move(resources.styles),
                    std::move(resources.definition), {UiAnimationReloadPolicy::Cancel, Cutoff}};
        }

        std::shared_ptr<const RuntimeUiClient> Client() {
            return Take(service.Issue({"game.tests", 101, 1, Instance(), Stable<UiCanvasId>(2), RuntimeUiPermission::Elements, {}}));
        }
    };

    /** @brief Proves stale lineage rollback, then builds a fresh source-qualified runtime replacement. */
    UiAnimationReloadPreparation PrepareHostReplacement(ServiceSource &fixture, UiAnimationRuntimeParticipant &participant,
                                                        const UiReloadLease &sourceBefore, const UiAnimationFrameLease &first,
                                                        const std::shared_ptr<const RuntimeUiClient> &client) {
        const auto sourceCanvases = sourceBefore.Get()->Canvases();
        const auto currentCanvas = std::ranges::find(sourceCanvases, Stable<UiCanvasId>(2), &UiReloadCanvas::id);
        REQUIRE(currentCanvas != sourceCanvases.end());
        REQUIRE(currentCanvas->layout);
        // Authored revision 2 alone does not advance the runtime interaction already published by the actual host frame.
        auto stale = participant.Reload(fixture.service, Instance(), fixture.Replacement(2));
        REQUIRE(stale.HasError());
        REQUIRE(stale.ErrorValue().domain.Value() == UiErrors::RevisionStale.domain.Value());
        REQUIRE(stale.ErrorValue().code.Value() == UiErrors::RevisionStale.code.Value());
        REQUIRE(Take(fixture.service.Acquire(Instance())).Get() == sourceBefore.Get());
        REQUIRE(client->Validate().HasValue());
        REQUIRE(Frame(participant).Layout().Descriptor().interaction == first.Layout().Descriptor().interaction);
        const auto runtimeHighWater =
            std::max(currentCanvas->tree.Revision().Value(), currentCanvas->layout->Descriptor().interaction.Value());
        REQUIRE(runtimeHighWater < std::numeric_limits<std::uint64_t>::max());
        auto replacement = fixture.Replacement(2, runtimeHighWater + 1);
        const auto *replacementCanvas = replacement.generation.Canvas(Stable<UiCanvasId>(2));
        REQUIRE(replacementCanvas);
        REQUIRE(replacementCanvas->layout);
        INFO("source/replacement document=" << sourceBefore.Get()->Instance().DocumentRevision().Value() << "/"
                                            << replacement.generation.Instance().DocumentRevision().Value()
                                            << " registry=" << sourceBefore.Get()->RegistryRevision().value << "/"
                                            << replacement.generation.RegistryRevision().value << " tree="
                                            << currentCanvas->tree.Revision().Value() << "/" << replacementCanvas->tree.Revision().Value()
                                            << " interaction=" << currentCanvas->layout->Descriptor().interaction.Value() << "/"
                                            << replacementCanvas->layout->Descriptor().interaction.Value()
                                            << " first published interaction=" << first.Layout().Descriptor().interaction.Value()
                                            << " same asset=" << (sourceBefore.Get()->Asset() == replacement.generation.Asset())
                                            << " source/replacement root slot=" << currentCanvas->tree.Root().Value().handle.slot << "/"
                                            << replacementCanvas->tree.Root().Value().handle.slot);
        return replacement;
    }
}  // namespace

TEST_CASE("Service animation source reload preserves the sole publisher and exact client grant transaction",
          "[runtime_ui][animation][service]") {
    ServiceSource fixture;
    const auto old = Take(fixture.service.Acquire(Instance()));
    const auto client = fixture.Client();
    REQUIRE(client->Validate().HasValue());
    auto refused = fixture.service.Reload(Instance(), Generation(fixture.allocator, 2), Cutoff);
    REQUIRE(refused.HasError());
    REQUIRE(client->Validate().HasValue());
    auto invalid = fixture.Replacement(2);
    invalid.definition.elements.front().element = Stable<UiElementId>(99);
    REQUIRE(fixture.service.Reload(Instance(), *fixture.animation, std::move(invalid)).HasError());
    REQUIRE(client->Validate().HasValue());
    REQUIRE(Take(fixture.service.Acquire(Instance())).Get() == old.Get());
    REQUIRE(fixture.service.Reload(Instance(), *fixture.animation, fixture.Replacement(3)).HasValue());
    REQUIRE(client->Validate().HasError());
    REQUIRE(Take(fixture.service.Acquire(Instance())).Get() != old.Get());
    REQUIRE(fixture.Client()->Validate().HasValue());
}

TEST_CASE("Service animation source rejects foreign coordination and detaches without destroying its publisher",
          "[runtime_ui][animation][service]") {
    ServiceSource fixture;
    auto foreign = Take(RuntimeUiService::Create(Take(UiSceneReconciliation::Create({Owner()}))));
    REQUIRE(foreign.Reload(Instance(), *fixture.animation, fixture.Replacement(2)).HasError());
    const auto client = fixture.Client();
    fixture.animation->Shutdown();
    REQUIRE(fixture.animation->DrainRetired().HasValue());
    fixture.animation.reset();
    REQUIRE(client->Validate().HasValue());
    REQUIRE(fixture.service.Reload(Instance(), Generation(fixture.allocator, 3), Cutoff).HasValue());
    REQUIRE(client->Validate().HasError());
}

TEST_CASE("Service source Scene unload waits for derived retirement and host shutdown safely revokes access",
          "[runtime_ui][animation][service]") {
    ServiceSource fixture(true);
    REQUIRE(fixture.service.PrepareTransition({SceneRuntimeId{101}, {}}, {}).HasError());
    fixture.animation->Shutdown();
    fixture.animation.reset();
    auto transition = Take(fixture.service.PrepareTransition({SceneRuntimeId{101}, {}}, {}));
    REQUIRE(fixture.service.CommitTransition(transition, Cutoff).HasValue());
    REQUIRE(fixture.service.Acquire(Instance()).HasError());
    ServiceSource stopped;
    const auto client = stopped.Client();
    stopped.service.Shutdown();
    REQUIRE(client->Validate().HasError());
    stopped.animation->Shutdown();
    REQUIRE(stopped.animation->DrainRetired().HasValue());
}

TEST_CASE("Actual RuntimeHost publishes frames from the service source and coordinated participant reload",
          "[runtime_ui][animation][integration][service]") {
    ServiceSource fixture;
    DeterministicClock clock;
    auto host = Take(RuntimeHost::Create(clock));
    auto composition = Take(UiAnimationRuntimeParticipant::Compose(std::move(*fixture.animation), host->DispatchSource(), Config()));
    fixture.animation.reset();
    auto *participant = composition.participant.get();
    REQUIRE(host->AddParticipant(std::move(composition.participant)).HasValue());
    REQUIRE(host->Startup().HasValue());
    REQUIRE(host->RunFrame().HasValue());
    auto first = Frame(*participant);
    auto receipt = Receipt(first, 1);
    REQUIRE(participant->ApplyPresentation(receipt).HasValue());
    REQUIRE(participant->InputEligible(receipt.view));
    const auto client = fixture.Client();
    const auto sourceBefore = Take(fixture.service.Acquire(Instance()));
    auto replacement = PrepareHostReplacement(fixture, *participant, sourceBefore, first, client);
    auto reloaded = participant->Reload(fixture.service, Instance(), std::move(replacement));
    INFO("host state=" << static_cast<int>(host->State())
                       << " dispatch admission=" << static_cast<int>(host->DispatchSource().BindingStatus())
                       << " completed simulation ticks=" << host->Statistics().completedSimulationTick
                       << " retained document=" << first.Layout().Descriptor().sources.document.Value() << " source still readable="
                       << (sourceBefore.Get() != nullptr) << " client valid=" << client->Validate().HasValue());
    if (reloaded.HasError()) {
        INFO("reload error domain=" << reloaded.ErrorValue().domain.Value() << " code=" << reloaded.ErrorValue().code.Value()
                                    << " message=" << reloaded.ErrorValue().message);
        REQUIRE(reloaded.HasValue());
    }
    REQUIRE(reloaded.HasValue());
    REQUIRE(client->Validate().HasError());
    clock.Advance(Duration::FromMilliseconds(16));
    REQUIRE(host->RunFrame().HasValue());
    auto next = Frame(*participant);
    REQUIRE(next.Layout().Descriptor().sources.document == Revision<UiDocumentRevision>(2));
    REQUIRE(first.Layout().Descriptor().sources.document == Revision<UiDocumentRevision>(1));
    REQUIRE(participant->ApplyPresentation(receipt).HasError());
    host->Shutdown();
    REQUIRE(fixture.service.Acquire(Instance()).HasValue());
}

TEST_CASE("Moved service wrappers retain exact animation source identity and reject duplicate attachment",
          "[runtime_ui][animation][service][lifetime]") {
    ServiceSource fixture;
    auto source = Take(fixture.service.Acquire(Instance()));
    auto resources = Resources(source.Get()->Canvases().front());
    REQUIRE(UiAnimationOwner::Create(fixture.service, Instance(), fixture.allocator, std::move(resources.registry),
                                     std::move(resources.styles), std::move(resources.definition))
                .HasError());
    auto moved = std::move(fixture.service);
    REQUIRE(fixture.service.Acquire(Instance()).HasError());
    REQUIRE(moved.Reload(Instance(), *fixture.animation, fixture.Replacement(2)).HasValue());
    REQUIRE(Take(moved.Acquire(Instance())).Get() != source.Get());
}

TEST_CASE("Cancelled service animation reload preserves source and authorized queued client commands",
          "[runtime_ui][animation][service][rollback]") {
    ServiceSource fixture;
    const auto source = Take(fixture.service.Acquire(Instance()));
    const RuntimeUiGrant grant{"game.tests", 101, 1, Instance(), Stable<UiCanvasId>(2), RuntimeUiPermission::Screens, {}};
    const auto client = Take(fixture.service.Issue(grant));
    REQUIRE(client->Screen(UiRouteOperationRequest::Push(Stable<UiRouteId>(9))).HasValue());
    CancellationSource cancelled;
    cancelled.RequestCancellation();
    REQUIRE(fixture.service.Reload(Instance(), *fixture.animation, fixture.Replacement(2), cancelled.Token()).HasError());
    REQUIRE(client->Validate().HasValue());
    REQUIRE(Take(fixture.service.Acquire(Instance())).Get() == source.Get());
    REQUIRE(Take(fixture.service.ApplyQueuedCommands(RuntimeUiCommandCutoff::OwnerCommands)) == 1);
}

TEST_CASE("Source attachment after Scene preparation fences both admission and commit without revoking grants",
          "[runtime_ui][animation][service][scene]") {
    ServiceSource fixture(true, false);
    const auto client = fixture.Client();
    auto candidate = Take(fixture.service.PrepareTransition({SceneRuntimeId{101}, {}}, {}));
    fixture.Attach();
    const auto admitted = fixture.service.CanCommitTransition(candidate, Cutoff);
    REQUIRE(admitted.HasError());
    REQUIRE(admitted.ErrorValue().code.Value() == MakeError(UiErrors::CapabilityBusy).code.Value());
    REQUIRE(fixture.service.CommitTransition(candidate, Cutoff).HasError());
    REQUIRE(client->Validate().HasValue());
    REQUIRE(fixture.service.Acquire(Instance()).HasValue());
}

TEST_CASE("Service shutdown retains the canonical entry beneath an idle attached animation owner",
          "[runtime_ui][animation][service][lifetime]") {
    ServiceSource fixture;
    auto client = fixture.Client();
    fixture.service.Shutdown();
    REQUIRE(client->Validate().HasError());
    client.reset();
    REQUIRE(fixture.service.CollectRetired().HasValue());
    REQUIRE_FALSE(Take(fixture.service.CanReclaim()));
    REQUIRE(fixture.animation->Start(Stable<UiAnimationId>(1)).HasError());
    fixture.animation->Shutdown();
    REQUIRE(fixture.animation->DrainRetired().HasValue());
    fixture.animation.reset();
    REQUIRE(fixture.service.CollectRetired().HasValue());
    REQUIRE(Take(fixture.service.CanReclaim()));
}

TEST_CASE("Destroyed service wrapper revokes before attached animation cleanup and immutable lease release",
          "[runtime_ui][animation][service][lifetime]") {
    ServiceSource fixture;
    auto publication = Take(fixture.service.Acquire(Instance()));
    auto client = fixture.Client();
    std::optional<RuntimeUiService> wrapper{std::move(fixture.service)};
    wrapper.reset();
    REQUIRE(client->Validate().HasError());
    REQUIRE(fixture.animation->Start(Stable<UiAnimationId>(1)).HasError());
    REQUIRE(publication.Get() != nullptr);
    fixture.animation->Shutdown();
    REQUIRE(fixture.animation->DrainRetired().HasValue());
    REQUIRE(fixture.animation->CanReclaim());
    fixture.animation.reset();
    REQUIRE(publication.Get()->Instance().State() != UiRuntimeInstanceState::Active);
    publication = {};
    client.reset();
}
