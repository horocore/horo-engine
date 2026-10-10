#include "Horo/Runtime/Ui/UiSceneReconciliation.h"
#include "UiHotReloadTestFixture.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Runtime::Ui {
    namespace {
        using namespace ReloadTests;
        constexpr auto Cutoff = UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands;
        const SceneRuntimeId First{101};
        const SceneRuntimeId Next{102};

        template <typename T> T Take(Result<T> result) {
            INFO((result.HasError() ? result.ErrorValue().code.Value() + ": " + result.ErrorValue().message : ""));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        UiSceneInstanceDescriptor Descriptor(std::uint32_t slot, UiOwnerScopeKind kind, std::optional<SceneRuntimeId> scene = {},
                                             std::optional<SceneRuntimeId> provider = {}) {
            UiSemanticOwner owner;
            owner.kind = kind;
            if (kind == UiOwnerScopeKind::Scene)
                owner.scene = scene;
            if (kind == UiOwnerScopeKind::Player)
                owner.player = UiFocusPlayerId{Owner(), 3, 1};
            if (kind == UiOwnerScopeKind::Viewport)
                owner.viewport = UiOverlayViewportId{Owner(), 10, 1};
            return {{Owner(), slot, 1}, owner, provider};
        }

        UiBindingProviderSchema Schema(bool writable = false) {
            const std::array properties{
                UiBindingPropertyDescriptor{.id = Take(UiBindingPropertyId::Parse("available")),
                                            .type = UiBindingValueType::Boolean,
                                            .access = writable ? UiBindingAccess::ReadWriteCommand : UiBindingAccess::Read}};
            UiBindingProviderDescriptor descriptor{Take(UiBindingProviderTypeId::Parse("game.scene.values")),
                                                   ModuleId{"game.scene"},
                                                   {1, 0, 0},
                                                   properties,
                                                   UiBindingProviderScopeBit(UiBindingProviderScopeKind::Scene) |
                                                       UiBindingProviderScopeBit(UiBindingProviderScopeKind::Player)};
            descriptor.schema.fingerprint = ComputeUiBindingSchemaFingerprint(descriptor);
            return Take(UiBindingProviderSchema::Create(descriptor));
        }

        UiReloadGeneration Make(UiElementSlotAllocator &slots, RuntimeUiInstanceId instance, std::uint64_t runtime = 1,
                                std::uint32_t providerSlot = 0, bool available = true, bool removing = false, std::uint64_t document = 1,
                                bool playerProvider = false, bool writable = false) {
            LayerOptions options;
            options.instance = instance;
            options.canvas.slot = 100 + instance.slot;
            options.layer.slot = 200 + instance.slot;
            options.runtimeRevision = runtime;
            auto generation = Generation(slots, document, 32, false, 0, 0, 3, options);
            if (providerSlot != 0) {
                auto &canvas = *generation.Canvas(Stable<UiCanvasId>(2));
                const auto schema = Schema(writable);
                const UiBindingProviderInstanceId provider{Owner(), providerSlot, 1};
                const std::array initial{UiBindingPropertyUpdate{0, available}};
                const std::array registrations{
                    UiBindingProviderRegistration{provider,
                                                  playerProvider ? UiBindingProviderScopeKind::Player : UiBindingProviderScopeKind::Scene,
                                                  &schema, Revision<UiBindingSnapshotRevision>(1), initial}};
                const auto &property = schema.Properties()[0];
                const std::array bindings{
                    UiResolvedBindingDescriptor{provider,
                                                {.id = Stable<UiBindingId>(33),
                                                 .source = {schema.Type(), property.id, {1, 0}, property.signatureFingerprint},
                                                 .target = {Stable<UiElementId>(10),
                                                            writable ? UiBindingTargetProperty::BooleanValue
                                                                     : UiBindingTargetProperty::Enabled,
                                                            {}},
                                                 .direction = writable ? UiBindingDirection::TwoWay : UiBindingDirection::SourceToTarget}}};
                canvas.bindings.emplace(Take(UiBindingStore::Create(canvas.tree, registrations, bindings)));
                if (removing)
                    REQUIRE(canvas.bindings->Unregister(canvas.tree, provider, *canvas.layoutEngine).HasValue());
            }
            return generation;
        }

        UiHotReload PublisherFor(UiElementSlotAllocator &slots, const UiSceneInstanceDescriptor &descriptor, std::uint32_t provider = 0,
                                 bool playerProvider = false) {
            return Take(UiHotReload::Create(Make(slots, descriptor.instance, 1, provider, true, false, 1, playerProvider)));
        }

        void Admit(UiSceneReconciliation &service, UiElementSlotAllocator &slots, const UiSceneInstanceDescriptor &descriptor,
                   std::uint32_t provider = 0, bool playerProvider = false) {
            auto publisher = PublisherFor(slots, descriptor, provider, playerProvider);
            REQUIRE(service.Admit(descriptor, std::move(publisher), Cutoff).HasValue());
        }
    }  // namespace

    TEST_CASE("Scene reconciliation retires only exact Scene owners and preserves all foreign scopes", "[runtime_ui][scene]") {
        auto slots = Take(UiElementSlotAllocator::Create(Owner()));
        auto service = Take(UiSceneReconciliation::Create({Owner(), 8, 8, 4}));
        const auto game = Descriptor(1, UiOwnerScopeKind::GameInstance);
        const auto player = Descriptor(2, UiOwnerScopeKind::Player);
        const auto scene = Descriptor(3, UiOwnerScopeKind::Scene, First);
        const auto foreign = Descriptor(4, UiOwnerScopeKind::Scene, SceneRuntimeId{999});
        const auto viewport = Descriptor(5, UiOwnerScopeKind::Viewport);
        for (const auto &descriptor : {game, player, scene, foreign, viewport})
            Admit(service, slots, descriptor);
        auto gameLease = Take(service.Acquire(game.instance));
        auto sceneLease = Take(service.Acquire(scene.instance));
        const auto incoming = Descriptor(6, UiOwnerScopeKind::Scene, Next);
        std::vector<UiSceneActivation> activations;
        activations.push_back({incoming, Make(slots, incoming.instance)});
        auto prepared = Take(service.Prepare({First, Next}, {}, std::move(activations)));
        Horo::Tests::AllocationProbe::Measurement measured;
        {
            Horo::Tests::AllocationProbe::ScopedMeasurement probe;
            auto committed = service.Commit(prepared, Cutoff);
            measured = probe.Snapshot();
            REQUIRE(committed.HasValue());
            CHECK(committed.Value().retiredSceneInstances == 1);
            CHECK(committed.Value().activatedSceneInstances == 1);
        }
        CHECK(measured.requests == 0);
        CHECK(service.Publisher(scene.instance) == nullptr);
        CHECK(service.Publisher(game.instance)->IsCurrent(gameLease));
        CHECK(service.Publisher(player.instance) != nullptr);
        CHECK(service.Publisher(viewport.instance) != nullptr);
        CHECK(service.Publisher(foreign.instance) != nullptr);
        CHECK(service.Publisher(incoming.instance) != nullptr);
        CHECK(sceneLease.Get()->Instance().State() == UiRuntimeInstanceState::Retiring);
        CHECK(service.Commit(prepared, Cutoff).HasError());
        CHECK(Take(service.CollectRetired()) == 0);
        service.Shutdown();
        service.Shutdown();
        CHECK_FALSE(service.CanReclaim());
        CHECK(service.Acquire(game.instance).HasError());
    }

    TEST_CASE("Persistent provider rebind preserves semantic control state with unchanged authored publication", "[runtime_ui][scene]") {
        auto slots = Take(UiElementSlotAllocator::Create(Owner()));
        auto service = Take(UiSceneReconciliation::Create({Owner()}));
        const auto game = Descriptor(1, UiOwnerScopeKind::GameInstance, {}, First);
        const auto scene = Descriptor(2, UiOwnerScopeKind::Scene, First);
        Admit(service, slots, game, 301);
        Admit(service, slots, scene);
        auto oldLease = Take(service.Acquire(game.instance));
        auto &canvas = *service.Publisher(game.instance)->Current()->Canvas(Stable<UiCanvasId>(2));
        Draft(canvas, "persist");
        const auto oldElement = canvas.tree.Root().Value().handle;
        std::vector<UiSceneBindingReplacement> replacements;
        replacements.push_back({game.instance, Make(slots, game.instance, 2, 302, false)});
        auto prepared = Take(service.Prepare({First, Next}, std::move(replacements)));
        CHECK(prepared.Result().reboundPersistentInstances == 1);
        REQUIRE(service.Commit(prepared, Cutoff).HasValue());
        auto &next = *service.Publisher(game.instance)->Current()->Canvas(Stable<UiCanvasId>(2));
        CHECK(next.tree.SourceDocumentRevision() == Revision<UiDocumentRevision>(1));
        CHECK(next.tree.Revision() == Revision<UiRuntimeTreeRevision>(2));
        CHECK(next.tree.Get(oldElement).HasError());
        CHECK_FALSE(service.Publisher(game.instance)->IsCurrent(oldLease));
        CHECK(std::get<UiTextInputControlState>(next.controls.front().control.Snapshot().Value()).text.View().find("persist") !=
              std::string_view::npos);
        const auto *bound = next.bindings->Find(next.tree, Stable<UiBindingId>(33));
        REQUIRE(bound != nullptr);
        CHECK_FALSE(std::get<bool>(bound->value));
        CHECK_FALSE(service.Publisher(game.instance)->InputEligible(next.id, {Owner(), 10, 1}));
        CHECK(service.Publisher(scene.instance) == nullptr);
    }

    TEST_CASE("Missing stale or cancelled provider candidates preserve every active owner", "[runtime_ui][scene]") {
        auto slots = Take(UiElementSlotAllocator::Create(Owner()));
        auto service = Take(UiSceneReconciliation::Create({Owner()}));
        const auto game = Descriptor(1, UiOwnerScopeKind::GameInstance, {}, First);
        const auto scene = Descriptor(2, UiOwnerScopeKind::Scene, First);
        Admit(service, slots, game, 301);
        Admit(service, slots, scene);
        auto old = Take(service.Acquire(game.instance));
        CHECK(service.Prepare({First, Next}, {}).HasError());
        std::vector<UiSceneBindingReplacement> stale;
        stale.push_back({game.instance, Make(slots, game.instance, 2, 301)});
        CHECK(service.Prepare({First, Next}, std::move(stale)).HasError());
        std::vector<UiSceneBindingReplacement> candidates;
        candidates.push_back({game.instance, Make(slots, game.instance, 3, 303)});
        auto prepared = Take(service.Prepare({First, Next}, std::move(candidates)));
        REQUIRE(service.Publisher(game.instance)->Current()->Canvas(Stable<UiCanvasId>(2))->routes->Push(Stable<UiRouteId>(9)).HasValue());
        CHECK(service.Commit(prepared, Cutoff).HasError());
        CHECK(service.Publisher(game.instance)->IsCurrent(old));
        CHECK(service.Publisher(scene.instance) != nullptr);
    }

    TEST_CASE("Explicit unload publishes required unavailability and supports idempotent unload", "[runtime_ui][scene]") {
        auto slots = Take(UiElementSlotAllocator::Create(Owner()));
        auto service = Take(UiSceneReconciliation::Create({Owner()}));
        const auto game = Descriptor(1, UiOwnerScopeKind::GameInstance, {}, First);
        Admit(service, slots, game, 301);
        std::vector<UiSceneBindingReplacement> candidates;
        candidates.push_back({game.instance, Make(slots, game.instance, 2, 301, true, true)});
        auto prepared = Take(service.Prepare({First, {}}, std::move(candidates)));
        CHECK(prepared.Result().requiredUnavailable == 1);
        REQUIRE(service.Commit(prepared, Cutoff).HasValue());
        auto &canvas = *service.Publisher(game.instance)->Current()->Canvas(Stable<UiCanvasId>(2));
        CHECK(canvas.bindings->Find(canvas.tree, Stable<UiBindingId>(33)) == nullptr);
        auto repeated = Take(service.Prepare({First, {}}, {}));
        CHECK(Take(service.Commit(repeated, Cutoff)).retiredSceneInstances == 0);
        std::vector<UiSceneBindingReplacement> resume;
        resume.push_back({game.instance, Make(slots, game.instance, 3, 303)});
        auto resumed = Take(service.Prepare({First, Next}, std::move(resume)));
        REQUIRE(service.Commit(resumed, Cutoff).HasValue());
        CHECK(service.Publisher(game.instance) != nullptr);
    }

    TEST_CASE("Ownership admission rejects mixed dimensions and reused exact instance slots", "[runtime_ui][scene]") {
        auto slots = Take(UiElementSlotAllocator::Create(Owner()));
        auto service = Take(UiSceneReconciliation::Create({Owner(), 1, 1, 1}));
        const auto game = Descriptor(1, UiOwnerScopeKind::GameInstance);
        Admit(service, slots, game);
        const auto next = Descriptor(2, UiOwnerScopeKind::Player);
        auto publisher = PublisherFor(slots, next);
        CHECK(service.Admit(next, std::move(publisher), Cutoff).HasError());
        CHECK(publisher.Current() != nullptr);
        CHECK(service.LastIssuedInstanceSlot() == 2);
        auto malformed = next.owner;
        malformed.scene = First;
        CHECK_FALSE(malformed.IsValid(Owner()));
        auto candidate = Take(service.Prepare({First, Next}, {}));
        service.Shutdown();
        CHECK(service.Commit(candidate, Cutoff).HasError());
        CHECK_FALSE(service.CanReclaim());
    }

    TEST_CASE("Provider retirement callback shutdown is deferred until collector unwinds", "[runtime_ui][scene][shutdown]") {
        auto slots = Take(UiElementSlotAllocator::Create(Owner()));
        auto service = Take(UiSceneReconciliation::Create({Owner()}));
        const auto scene = Descriptor(1, UiOwnerScopeKind::Scene, First, First);
        auto generation = Make(slots, scene.instance, 1, 301, true, false, 1, false, true);
        auto &canvas = *generation.Canvas(Stable<UiCanvasId>(2));
        const auto schema = Schema(true);

        class Authority final : public UiBindingWriteAuthority {
        public:
            UiBindingWriteFence fence;
            UiSceneReconciliation *service{};
            std::size_t abandons{};

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
                service->Shutdown();
            }
        };

        auto authority = std::make_shared<Authority>();
        authority->fence =
            {{Owner(), 301, 1}, UiBindingProviderScopeKind::Scene, schema.Version(), 0, schema.Properties()[0].signatureFingerprint, 1};
        authority->service = &service;
        const UiActionOwnerContext context{canvas.tree.Instance(),       canvas.tree.Canvas(),
                                           canvas.tree.SourceDocument(), canvas.tree.SourceDocumentRevision(),
                                           canvas.tree.Revision(),       canvas.layout->Descriptor().interaction};
        const std::array admission{UiBindingWriteAdmission{Stable<UiBindingId>(33), context, Stable<UiActionId>(8),
                                                           UiBindingCommitTrigger::Change, UiBindingConflictPolicy::RejectStale,
                                                           authority}};
        REQUIRE(canvas.bindings->AdmitWrites(canvas.tree, admission).HasValue());
        canvas.actions.emplace(Take(UiActionRouter::Create({context, 8})));
        const UiActionSource source{context, canvas.tree.Root().Value().handle};
        auto edit = Take(canvas.bindings->BeginEdit(canvas.tree, Stable<UiBindingId>(33), source));
        UiActionPayload payload;
        REQUIRE(payload.Add(false).HasValue());
        REQUIRE(canvas.actions->Enqueue(source, UiGameplayActionCommand{Stable<UiActionId>(8), payload}).HasValue());
        auto request = Take(canvas.actions->TryDequeue());
        REQUIRE(request.has_value());
        REQUIRE(canvas.bindings->QueueWrite(canvas.tree, edit, *request, UiBindingCommitTrigger::Change).HasValue());
        auto publisher = Take(UiHotReload::Create(std::move(generation)));
        REQUIRE(service.Admit(scene, std::move(publisher), Cutoff).HasValue());
        const auto game = Descriptor(2, UiOwnerScopeKind::GameInstance);
        Admit(service, slots, game);
        auto prepared = Take(service.Prepare({First, Next}, {}));
        REQUIRE(service.Commit(prepared, Cutoff).HasValue());
        CHECK(authority->abandons == 0);
        REQUIRE(service.CollectRetired().HasValue());
        CHECK(authority->abandons == 1);
        CHECK(service.Publisher(game.instance) == nullptr);
        CHECK(service.Acquire(game.instance).HasError());
        service.Shutdown();
        CHECK(authority->abandons == 1);
    }

    TEST_CASE("Scene cancellation and invalid incoming ownership cannot partially retire UI", "[runtime_ui][scene][rollback]") {
        auto slots = Take(UiElementSlotAllocator::Create(Owner()));
        auto service = Take(UiSceneReconciliation::Create({Owner()}));
        const auto scene = Descriptor(1, UiOwnerScopeKind::Scene, First);
        Admit(service, slots, scene);
        CancellationSource cancellation;
        auto prepared = Take(service.Prepare({First, Next}, {}, {}, cancellation.Token()));
        cancellation.RequestCancellation();
        CHECK(service.CanCommit(prepared, Cutoff).HasError());
        CHECK(service.Commit(prepared, Cutoff).HasError());
        CHECK(service.Publisher(scene.instance) != nullptr);
        CHECK(service.Prepare({First, Next}, {}, {}, cancellation.Token()).HasError());
        const auto invalid = Descriptor(2, UiOwnerScopeKind::Player);
        std::vector<UiSceneActivation> incoming;
        incoming.push_back({invalid, Make(slots, invalid.instance)});
        CHECK(service.Prepare({First, Next}, {}, std::move(incoming)).HasError());
        CHECK(service.Publisher(scene.instance) != nullptr);
    }

    TEST_CASE("Scene rebind rejects unrelated provider changes and runtime-only fake scene lineage", "[runtime_ui][scene][bindings]") {
        auto slots = Take(UiElementSlotAllocator::Create(Owner()));
        const auto player = Descriptor(1, UiOwnerScopeKind::Player);
        auto publisher = PublisherFor(slots, player, 401, true);
        auto original = Take(publisher.Acquire());
        CHECK(publisher.PrepareSceneRebind(Make(slots, player.instance, 2, 401, false, false, 1, true), false).HasError());
        CHECK(publisher.IsCurrent(original));
        auto service = Take(UiSceneReconciliation::Create({Owner()}));
        REQUIRE(service.Admit(player, std::move(publisher), Cutoff).HasValue());
        std::vector<UiSceneBindingReplacement> fake;
        fake.push_back({player.instance, Make(slots, player.instance, 3, 401, true, false, 1, true)});
        CHECK(service.Prepare({First, Next}, std::move(fake)).HasError());
        CHECK(service.Publisher(player.instance)->IsCurrent(original));
    }

    TEST_CASE("Provider grant revocation invalidates prepared scene rebind even without a value revision",
              "[runtime_ui][scene][bindings]") {
        auto slots = Take(UiElementSlotAllocator::Create(Owner()));
        const auto player = Descriptor(1, UiOwnerScopeKind::Player);
        auto publisher = PublisherFor(slots, player, 401, true);
        auto prepared = Take(publisher.PrepareSceneRebind(Make(slots, player.instance, 2, 401, true, false, 1, true), false));
        auto &canvas = *publisher.Current()->Canvas(Stable<UiCanvasId>(2));
        const auto before = canvas.bindings->Current().revision;
        auto wrongLayout = Take(UiLayoutEngine::Create(
            {{Owner(), 999, 1}, {Owner(), 999, 1}, canvas.tree.SourceDocument(), 8, 8, 3, Revision<UiInteractionRevision>(1)}));
        CHECK(canvas.bindings->Unregister(canvas.tree, {Owner(), 401, 1}, wrongLayout).HasError());
        CHECK(canvas.bindings->Current().revision == before);
        CHECK(publisher.CanCommit(prepared, Cutoff).HasError());
        CHECK(publisher.Commit(prepared, Cutoff).HasError());
        CHECK(publisher.Current()->Instance().DocumentRevision() == Revision<UiDocumentRevision>(1));
    }
}  // namespace Horo::Runtime::Ui
