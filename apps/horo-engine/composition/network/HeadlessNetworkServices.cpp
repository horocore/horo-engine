#include "HeadlessInitialStateAuthority.h"
#include "HeadlessNetworkWorldService.h"
#include "HeadlessPerceptionPolicy.h"
#include "HeadlessReplicaAnimationInputs.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Gameplay/GameplayVfxScene.h"
#include "Horo/Network/NetworkErrors.h"
#include "Horo/Network/ReplicationWorldLifecycle.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "Horo/Scene/CookedSceneDefinition.h"
#include "PhysicsStateHostLifecycle.h"

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <tuple>
#include <utility>

namespace Horo::Application::Internal {
    namespace {
        using ReplayEntity = Result<AI::AiSavedEntityReference>;

        /** @brief Checks that a slot still contains the original authored entity for this replay. */
        bool IsOriginalReplayEntity(const HeadlessNetworkDetail::ReplayAuthoredScene &authored, const Runtime::EntityId entity) noexcept {
            return entity.index < authored.slots.size() && entity == authored.slots[entity.index];
        }

        /** @brief Uses a fixed byte order so the hash is independent of platform layout and process addresses. */
        void HashPerceptionWord(Sha256Builder &hash, const std::uint64_t value) noexcept {
            std::array<std::byte, sizeof(value)> bytes;
            for (std::size_t index = 0; index < bytes.size(); ++index)
                bytes[index] = static_cast<std::byte>(value >> (8 * index));
            (void)hash.Update(bytes);  // Snapshot record limits are far below the SHA-256 byte limit.
        }

        /** @brief Hashes common spatial facts followed by participant-specific admission facts. */
        template <class Record>
        void HashPerceptionEndpoint(Sha256Builder &hash, const Runtime::SceneObjectId object, const Record &record) {
            HashPerceptionWord(hash, object.value);
            for (const auto coordinate : record.position.Millimeters())
                HashPerceptionWord(hash, static_cast<std::uint64_t>(coordinate));
            HashPerceptionWord(hash, record.ownerCancellation.IsCancellationRequested());
            HashPerceptionWord(hash, record.senseCount);
            for (const auto sense : std::span{record.senses}.first(record.senseCount))
                HashPerceptionWord(hash, sense.Value());
        }

        /** @brief Encodes every source disclosure field used by the spatial query. */
        void HashPerceptionPolicy(Sha256Builder &hash, const AI::PerceptionSpatialSource &source) {
            for (const auto field :
                 {source.layers, source.affiliation, source.team.value, source.listenerMask, static_cast<std::uint64_t>(source.visibility)})
                HashPerceptionWord(hash, field);
        }

        /** @brief Encodes the frozen listener filter and its bounded custom allowlist. */
        void HashPerceptionPolicy(Sha256Builder &hash, const AI::PerceptionSpatialListener &listener) {
            const auto &filter = listener.filtering;
            const auto &admission = listener.admission;
            for (const auto field : {filter.team.value, filter.layers, filter.mask, static_cast<std::uint64_t>(filter.visibility),
                                     static_cast<std::uint64_t>(admission.affiliations), static_cast<std::uint64_t>(admission.teams),
                                     static_cast<std::uint64_t>(admission.customCount)})
                HashPerceptionWord(hash, field);
            for (const auto custom : std::span{admission.custom}.first(admission.customCount))
                HashPerceptionWord(hash, custom.value);
        }

        /** @brief Orders immutable participants by durable Scene ID before consuming their frozen bytes. */
        template <class Record>
        Result<void> HashPerceptionParticipants(Sha256Builder &hash, std::span<const Record> records,
                                                const HeadlessNetworkDetail::ReplayAuthoredScene &authored,
                                                const Runtime::RuntimeSceneView &scene) {
            using Entry = std::pair<Runtime::SceneObjectId, const Record *>;
            std::vector<Entry> ordered;
            ordered.reserve(records.size());
            for (const auto &record : records) {
                const auto entity = scene.Get(record.entity);
                if (entity.HasError() || !IsOriginalReplayEntity(authored, record.entity.entity) || !entity.Value().authoredObject)
                    return Result<void>::Failure(MakeError(AI::AIErrors::CanonicalRestoreStale,
                                                           "Perception replay requires original authored participant identities."));
                ordered.emplace_back(*entity.Value().authoredObject, &record);
            }
            std::ranges::sort(ordered, {}, &Entry::first);
            HashPerceptionWord(hash, ordered.size());
            for (const auto &[object, record] : ordered) {
                HashPerceptionEndpoint(hash, object, *record);
                HashPerceptionPolicy(hash, *record);
            }
            return Result<void>::Success();
        }

        /** @brief Captures the spatial input before sensing, without running providers or exposing their payloads. */
        Result<HeadlessReplayPerception> CapturePerceptionReplay(const AI::PerceptionSpatialSnapshot &snapshot,
                                                                 const HeadlessNetworkDetail::ReplayAuthoredScene &authored,
                                                                 const Runtime::RuntimeSceneView &scene) {
            using Capture = Result<HeadlessReplayPerception>;
            if (snapshot.Scene() != scene.RuntimeId() || !scene.SameIncarnation(authored.initialScene))
                return Capture::Failure(MakeError(AI::AIErrors::PerceptionSpatialStale));
            const auto input = snapshot.Inputs();
            try {
                Sha256Builder hash;
                HashPerceptionWord(hash, input.filtering.relationshipCount);
                for (const auto &relationship : std::span{input.filtering.relationships}.first(input.filtering.relationshipCount)) {
                    HashPerceptionWord(hash, relationship.listener.value);
                    HashPerceptionWord(hash, relationship.source.value);
                    HashPerceptionWord(hash, static_cast<std::uint64_t>(relationship.affiliation.kind));
                    HashPerceptionWord(hash, relationship.affiliation.custom.value);
                }
                if (const auto result = HashPerceptionParticipants(hash, input.sources, authored, scene); result.HasError())
                    return Capture::Failure(result.ErrorValue());
                if (const auto result = HashPerceptionParticipants(hash, input.listeners, authored, scene); result.HasError())
                    return Capture::Failure(result.ErrorValue());
                return Capture::Success({snapshot.Revision(), hash.Finalize(), input.sources.size(), input.listeners.size()});
            } catch (const std::bad_alloc &) {
                return Capture::Failure(MakeError(AI::AIErrors::DecisionAssetStorageUnavailable));
            }
        }

        /** @brief Resolves the exact current root authored entity; raw runtime references never enter the encoded record. */
        ReplayEntity ProjectReplayEntity(const HeadlessNetworkDetail::ReplayAuthoredScene &authored, const Runtime::RuntimeSceneView &scene,
                                         const AI::BlackboardStoredEntityReference &stored) {
            const Runtime::EntityRef reference{{stored.sceneIncarnation}, {stored.slot, stored.generation}};
            auto entity = scene.Get(reference);
            if (entity.HasError())
                return ReplayEntity::Failure(std::move(entity).ErrorValue());
            if (!scene.SameIncarnation(authored.initialScene) || !IsOriginalReplayEntity(authored, reference.entity) ||
                !entity.Value().authoredObject)
                return ReplayEntity::Failure(
                    MakeError(AI::AIErrors::CanonicalStateInvalid, "AI replay requires an original root authored entity reference."));

            // The original authored incarnation is independent of transient slot generations and addresses.
            AI::AiSavedEntityReference projected{.generation = 1};
            constexpr auto width = sizeof(std::uint64_t);
            for (std::size_t byte = 0; byte < width; ++byte) {
                const auto shift = (width - 1 - byte) * 8;
                projected.identity[byte] = static_cast<std::uint8_t>(scene.DefinitionId().value >> shift);
                projected.identity[width + byte] = static_cast<std::uint8_t>(entity.Value().authoredObject->value >> shift);
            }
            return ReplayEntity::Success(projected);
        }

        /** @brief Detaches canonical reference bytes while the fixed owner still holds the exact Scene borrow. */
        HeadlessDecisionReplay::Capture EncodeReplayCapture(AI::AiSceneDecisionReplayCapture evidence,
                                                            const HeadlessNetworkDetail::ReplayAuthoredScene &authored,
                                                            const Runtime::RuntimeSceneView &scene,
                                                            const std::optional<HeadlessReplayPerception> perception,
                                                            const std::optional<HeadlessReplayNavigation> &navigation) {
            if (navigation && navigation->truncated)
                return HeadlessDecisionReplay::Capture::Failure(
                    MakeError(AI::AIErrors::CanonicalStateInvalid, "Navigation replay exceeded its outcome budget."));
            auto encoded = AI::EncodeAiCanonicalStateRecord(evidence.canonical, HeadlessDecisionReplay::CanonicalBudget,
                                                            [&](const AI::BlackboardStoredEntityReference &stored) {
                return ProjectReplayEntity(authored, scene, stored);
            });
            if (encoded.HasError())
                return HeadlessDecisionReplay::Capture::Failure(std::move(encoded).ErrorValue());
            return HeadlessDecisionReplay::Capture::Success({.evidence = std::move(evidence),
                                                             .canonicalRecord = std::move(encoded).Value(),
                                                             .definition = scene.DefinitionId(),
                                                             .definitionRevision = authored.definitionRevision,
                                                             .sceneInput = authored.sceneInput,
                                                             .perception = perception,
                                                             .navigation = navigation});
        }

        /** @brief Copies exact solved Character results over stable authored keys without stepping a subsystem. */
        Result<std::optional<std::vector<HeadlessReplayCharacterMotion>>> CaptureCharacterReplay(
            ScenePhysicsHost &native, const HeadlessNetworkDetail::ReplayAuthoredScene &authored, const Runtime::RuntimeSceneView &scene,
            const Runtime::FixedStepContext &context) {
            using Capture = Result<std::optional<std::vector<HeadlessReplayCharacterMotion>>>;
            const auto scope = native.Borrow();
            if (scope.HasError())
                return Capture::Failure(scope.ErrorValue());
            const auto bindings = scope.Value().characterBindings;
            if (bindings.empty())
                return Capture::Success(std::nullopt);
            if (!scene.SameIncarnation(authored.initialScene))
                return Capture::Failure(MakeError(AI::AIErrors::CanonicalRestoreStale));
            constexpr std::size_t MaximumControllers = 256;
            if (bindings.size() > MaximumControllers)
                return Capture::Failure(
                    MakeError(AI::AIErrors::CanonicalStateInvalid, "Character replay evidence exceeds its controller budget."));
            try {
                std::vector<HeadlessReplayCharacterMotion> result;
                result.reserve(bindings.size());
                for (const auto &binding : bindings) {
                    if (!IsOriginalReplayEntity(authored, binding.owner.entity))
                        return Capture::Failure(
                            MakeError(AI::AIErrors::CanonicalStateInvalid, "Character replay requires an authored controller identity."));
                    const auto movement = native.ReadCharacterFixedMovement(binding.handle, context);
                    if (movement.HasError())
                        return Capture::Failure(movement.ErrorValue());
                    if (const auto &solved = movement.Value())
                        result.push_back({binding.object, binding.component, solved->tick, solved->finalPosition, solved->finalHeading,
                                          solved->achievedVelocityMetersPerSecond, solved->termination, solved->grounded});
                }
                std::ranges::sort(result, {}, [](const HeadlessReplayCharacterMotion &motion) {
                    return std::tuple{motion.object, motion.component};
                });
                return Capture::Success(std::move(result));
            } catch (const std::bad_alloc &) {
                return Capture::Failure(MakeError(AI::AIErrors::DecisionAssetStorageUnavailable));
            }
        }

        /** @brief Copies the real Physics publication and original authored body state without advancing the solver. */
        Result<HeadlessReplayPhysics> CapturePhysicsReplay(ScenePhysicsHost &native,
                                                           const HeadlessNetworkDetail::ReplayAuthoredScene &authored,
                                                           const Runtime::RuntimeSceneView &scene, const std::uint64_t previousTick) {
            using Capture = Result<HeadlessReplayPhysics>;
            const auto scope = native.Borrow();
            if (scope.HasError())
                return Capture::Failure(scope.ErrorValue());
            const auto &physics = scope.Value().physics;
            const auto publication = physics.PublishedTick();
            if (publication.completedTick <= previousTick || publication.completedTick - previousTick != 1 ||
                publication.completedTick != publication.transformTick || !scene.SameIncarnation(authored.initialScene))
                return Capture::Failure(MakeError(Physics::PhysicsErrors::QuerySnapshotStale));

            const auto bindings = scope.Value().bodyBindings;
            constexpr std::size_t MaximumBodies = 128;
            if (bindings.size() > MaximumBodies)
                return Capture::Failure(MakeError(AI::AIErrors::CanonicalStateInvalid, "Physics replay exceeds its body budget."));
            const auto configuration = physics.ReadReplayConfiguration();
            if (configuration.HasError())
                return Capture::Failure(configuration.ErrorValue());
            try {
                HeadlessReplayPhysics result{publication, configuration.Value(), scope.Value().character.Settings().Identity(), {}};
                result.bodies.reserve(bindings.size());
                for (const auto &binding : bindings) {
                    const auto owner = scene.Find(binding.object);
                    if (!owner || !IsOriginalReplayEntity(authored, owner->entity))
                        return Capture::Failure(
                            MakeError(AI::AIErrors::CanonicalStateInvalid, "Physics replay requires an original authored body identity."));
                    const auto body = physics.ReadSceneBodyReconciliation(binding.handle);
                    if (body.HasError())
                        return Capture::Failure(body.ErrorValue());
                    const auto &state = body.Value().state;
                    result.bodies.push_back(
                        {binding.object, binding.body, state.pose, state.linearVelocity, state.angularVelocity, state.activity});
                }
                std::ranges::sort(result.bodies, {}, [](const HeadlessReplayRigidBody &body) {
                    return std::tuple{body.object, body.slot};
                });
                return Capture::Success(std::move(result));
            } catch (const std::bad_alloc &) {
                return Capture::Failure(MakeError(AI::AIErrors::DecisionAssetStorageUnavailable));
            }
        }

        /** @brief Copies the actual dispatched Gameplay phase using stable authored transform recipients. */
        Result<HeadlessReplayGameplay> CaptureGameplayReplay(const GameplayWorldComposition &owner,
                                                             const Gameplay::BehaviorFixedEvidence &evidence,
                                                             const HeadlessNetworkDetail::ReplayAuthoredScene &authored,
                                                             const Runtime::RuntimeSceneView &scene) {
            using Capture = Result<HeadlessReplayGameplay>;
            const auto profile = owner.ReplayProfile();
            if (evidence.truncated || profile.revision == 0 || !scene.SameIncarnation(authored.initialScene))
                return Capture::Failure(MakeError(AI::AIErrors::CanonicalStateInvalid, "Gameplay replay evidence is incomplete."));
            try {
                HeadlessReplayGameplay result{profile, evidence.callbacks, evidence.sceneCommandsQueued, {}};
                result.transforms.reserve(evidence.transformCount);
                for (const auto &command : std::span{evidence.transforms}.first(evidence.transformCount)) {
                    const auto entity = scene.Get(command.entity);
                    if (entity.HasError() || !IsOriginalReplayEntity(authored, command.entity.entity) || !entity.Value().authoredObject)
                        return Capture::Failure(MakeError(AI::AIErrors::CanonicalRestoreStale,
                                                          "Gameplay replay requires original authored transform recipients."));
                    result.transforms.emplace_back(*entity.Value().authoredObject, command.local);
                }
                return Capture::Success(std::move(result));
            } catch (const std::bad_alloc &) {
                return Capture::Failure(MakeError(AI::AIErrors::DecisionAssetStorageUnavailable));
            }
        }

        /** @brief Adds selected native evidence to an otherwise complete detached replay capture. */
        HeadlessDecisionReplay::Capture CaptureNativeReplay(HeadlessDecisionReplayData data, HeadlessNetworkDetail::WorldRecord &world,
                                                            const HeadlessNetworkDetail::ReplayAuthoredScene &authored,
                                                            const Runtime::RuntimeSceneView &scene,
                                                            const Runtime::FixedStepContext &context,
                                                            const std::optional<std::uint64_t> previousPhysicsTick,
                                                            const std::optional<Gameplay::BehaviorFixedEvidence> &gameplay) {
            using Capture = HeadlessDecisionReplay::Capture;
            if (gameplay && world.activeGameplay) {
                auto observed = CaptureGameplayReplay(*world.activeGameplay, *gameplay, authored, scene);
                if (observed.HasError())
                    return Capture::Failure(std::move(observed).ErrorValue());
                data.gameplay = std::move(observed).Value();
            }
            if (world.decisionQueries) {
                const auto publications = world.decisionQueries->ReadReplay(context.simulationTick);
                if (publications.HasError())
                    return Capture::Failure(publications.ErrorValue());
                const auto records = publications.Value();
                if (std::ranges::any_of(records, [](const AI::DecisionQueryPublication &record) {
                    return record.mode != AI::QueryExecutionMode::DeterministicWorkUnits ||
                           (record.winner && !std::holds_alternative<Math::WorldCoordinate64>(record.winner->item));
                }))
                    return Capture::Failure(MakeError(AI::AIErrors::CanonicalSchemaUnsupported,
                                                      "Headless replay requires deterministic point query evidence."));
                try {
                    data.queries.emplace(HeadlessReplayQueries{world.decisionQueries->ReplayProfile(), {records.begin(), records.end()}});
                } catch (const std::bad_alloc &) {
                    return Capture::Failure(MakeError(AI::AIErrors::DecisionAssetStorageUnavailable));
                }
            }
            auto motion = CaptureCharacterReplay(world.native, authored, scene, context);
            if (motion.HasError())
                return Capture::Failure(std::move(motion).ErrorValue());
            data.characterMotion = std::move(motion).Value();
            if (previousPhysicsTick) {
                auto physics = CapturePhysicsReplay(world.native, authored, scene, *previousPhysicsTick);
                if (physics.HasError())
                    return Capture::Failure(std::move(physics).ErrorValue());
                data.physics = std::move(physics).Value();
            }
            return Capture::Success(std::move(data));
        }
    }  // namespace

    /** @copydoc HeadlessDecisionReplay::BindAuthoredScene */
    Result<void> HeadlessDecisionReplay::BindAuthoredScene(HeadlessNetworkDetail::WorldRecord &world,
                                                           const Runtime::RuntimeSceneDefinition &definition) {
        const auto scene = world.scenes->ActiveScene();
        if (!scene || scene->RuntimeId() != world.request.scene || !scene->DefinitionId().IsValid())
            return Result<void>::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
        const auto slots = scene->SlotCount();
        if (slots > CanonicalBudget / sizeof(Runtime::EntityId))
            return Result<void>::Failure(
                MakeError(AI::AIErrors::CanonicalStateInvalid, "Root Scene exceeds the replay identity inventory limit."));
        try {
            const auto encoded = SceneCook::EncodeCookedSceneDefinition(definition);
            if (encoded.HasError())
                return Result<void>::Failure(encoded.ErrorValue());
            HeadlessNetworkDetail::ReplayAuthoredScene authored{*scene, std::vector<Runtime::EntityId>(slots), definition.Revision(),
                                                                ComputeSha256(std::as_bytes(std::span{encoded.Value()}))};
            for (std::size_t slot = 0; slot < authored.slots.size(); ++slot) {
                const auto entity = scene->EntityAt(slot);
                if (entity && entity->authoredObject)
                    authored.slots[slot] = entity->entity.entity;
            }
            world.replayAuthoredScene.emplace(std::move(authored));
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(AI::AIErrors::DecisionAssetStorageUnavailable));
        }
    }

    /** @copydoc HeadlessDecisionReplay::TakeCommitted */
    std::optional<HeadlessDecisionReplay::Capture> HeadlessDecisionReplay::TakeCommitted() noexcept {
        return owner_ == std::this_thread::get_id() ? std::exchange(committed_, std::nullopt) : std::nullopt;
    }

    void HeadlessDecisionReplay::Arm(HeadlessNetworkDetail::WorldRecord &world, const Runtime::FixedStepContext &context) {
        if (owner_ != std::this_thread::get_id() || published_ || attemptWorld_ || context.simulationTick != selection_.tick || !world.ai)
            return;
        attemptWorld_ = &world;
        attempt_ = context.attemptNumber;
        gameplay_.reset();
        navigation_.reset();
        perception_.reset();
        if (const auto spatial = world.perception.Current()) {
            const auto scene = world.scenes->ActiveScene();
            if (!scene || !world.replayAuthoredScene) {
                pending_ = Capture::Failure(MakeError(AI::AIErrors::PerceptionSpatialStale));
                return;
            }
            const auto input = CapturePerceptionReplay(*spatial, *world.replayAuthoredScene, *scene);
            if (input.HasError()) {
                pending_ = Capture::Failure(input.ErrorValue());
                return;
            }
            perception_ = input.Value();
        }
        const auto native = world.native.Borrow();
        if (native.HasError()) {
            pending_ = Capture::Failure(native.ErrorValue());
            return;
        }
        if (const auto &physics = native.Value().physics; physics.State() == Physics::PhysicsWorldState::ActiveSolver)
            previousPhysicsTick_ = physics.PublishedTick().completedTick;
        else
            previousPhysicsTick_.reset();
        const auto agent = world.ai->CapabilityAtSafePoint().ResolveAgent(selection_.agent);
        if (agent.HasError()) {
            pending_ = Capture::Failure(agent.ErrorValue());
            return;
        }
        const auto armed = world.ai->ArmDecisionReplayAtSafePoint(agent.Value(), selection_.tick);
        if (armed.HasError())
            pending_ = Capture::Failure(armed.ErrorValue());
        else {
            armedAgent_ = agent.Value();
            if (world.decisionQueries)
                world.decisionQueries->ArmReplay(armedAgent_, selection_.tick);
        }
    }

    /** @copydoc HeadlessDecisionReplay::ObserveGameplay */
    Gameplay::BehaviorFixedEvidence *HeadlessDecisionReplay::ObserveGameplay(const HeadlessNetworkDetail::WorldRecord &world,
                                                                             const Runtime::FixedStepContext &context) noexcept {
        if (owner_ != std::this_thread::get_id() || !armedAgent_.IsValid() || attemptWorld_ != &world || attempt_ != context.attemptNumber)
            return nullptr;
        return &gameplay_.emplace();
    }

    /** @copydoc HeadlessDecisionReplay::ObserveNavigation */
    HeadlessReplayNavigation *HeadlessDecisionReplay::ObserveNavigation(const HeadlessNetworkDetail::WorldRecord &world,
                                                                        const Runtime::FixedStepContext &context) noexcept {
        if (owner_ != std::this_thread::get_id() || !armedAgent_.IsValid() || attemptWorld_ != &world || attempt_ != context.attemptNumber)
            return nullptr;
        return &navigation_.emplace();
    }

    void HeadlessDecisionReplay::Prepare(HeadlessNetworkDetail::WorldRecord &world, const Runtime::FixedStepContext &context) {
        if (owner_ != std::this_thread::get_id() || pending_ || attemptWorld_ != &world || attempt_ != context.attemptNumber ||
            selection_.tick != context.simulationTick)
            return;
        const auto scene = world.closeRequested ? std::nullopt : world.scenes->ActiveScene();
        auto evidence = scene && world.replayAuthoredScene
                            ? world.ai->CaptureDecisionReplayAtSafePoint(*scene, selection_.tick, CanonicalBudget)
                            : Result<AI::AiSceneDecisionReplayCapture>::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
        if (evidence.HasError()) {
            pending_ = Capture::Failure(std::move(evidence).ErrorValue());
            DiscardArm();
            return;
        }
        armedAgent_ = {};
        auto captured = EncodeReplayCapture(std::move(evidence).Value(), *world.replayAuthoredScene, *scene, perception_, navigation_);
        pending_ = captured.HasError() ? std::move(captured)
                                       : CaptureNativeReplay(std::move(captured).Value(), world, *world.replayAuthoredScene, *scene,
                                                             context, previousPhysicsTick_, gameplay_);
    }

    void HeadlessDecisionReplay::Commit(const Runtime::FixedStepContext &context) noexcept {
        static_assert(std::is_nothrow_move_constructible_v<Capture>);
        if (owner_ != std::this_thread::get_id() || !pending_ || attempt_ != context.attemptNumber ||
            selection_.tick != context.simulationTick)
            return;
        committed_.emplace(std::move(*pending_));
        published_ = true;
        pending_.reset();
        attemptWorld_ = nullptr;
    }

    void HeadlessDecisionReplay::Abort(const Runtime::FixedStepContext &context) noexcept {
        if (owner_ == std::this_thread::get_id() && attemptWorld_ && attempt_ == context.attemptNumber &&
            selection_.tick == context.simulationTick) {
            pending_.reset();
            DiscardArm();
            attemptWorld_ = nullptr;
        }
    }

    /** @copydoc HeadlessDecisionReplay::DiscardArm */
    void HeadlessDecisionReplay::DiscardArm() noexcept {
        if (armedAgent_.IsValid())
            attemptWorld_->ai->DiscardDecisionReplayAtSafePoint(armedAgent_, selection_.tick);
        armedAgent_ = {};
    }

    namespace {
        using HeadlessNetworkDetail::HeadlessFixedParticipant;
        using HeadlessNetworkDetail::WorldOwners;
        using HeadlessNetworkDetail::WorldRecord;
        using HeadlessNetworkDetail::WorldService;

        /** @brief Allocate one non-reused Scene activation epoch with a bounded unpaired-factory slot. */
        Result<std::shared_ptr<WorldRecord>> CreateRecord(WorldOwners &owners, const Network::NetworkModeServiceRequest &request) {
            const auto free = std::ranges::find_if(owners.unpaired, [](const auto &record) {
                return record.expired();
            });
            if (free == owners.unpaired.end() || owners.nextWorldGeneration == 0)
                return Result<std::shared_ptr<WorldRecord>>::Failure(MakeError(Physics::PhysicsErrors::GenerationExhausted));
            std::weak_ptr<WorldRecord> *knownSlot{};
            if (owners.configured) {
                if (!request.scene.IsValid())
                    return Result<std::shared_ptr<WorldRecord>>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid));
                const auto &world = owners.configured->mountedWorld;
                for (auto &known : owners.known) {
                    const auto retained = known.lock();
                    if (world && world->liveCell && retained && retained->mountedWorld && !retained->closeRequested)
                        return Result<std::shared_ptr<WorldRecord>>::Failure(
                            MakeError(WorldStreaming::WorldArtifactErrors::Unsupported,
                                      "Replacing a live mounted World requires a cold shutdown."));
                    if (retained && retained->request.scene == request.scene)
                        return Result<std::shared_ptr<WorldRecord>>::Failure(
                            MakeError(Network::NetworkErrors::NetworkModeInvalid,
                                      "Scene identity is already reserved by a current, pending or retained world record."));
                    if (!retained && !knownSlot)
                        knownSlot = &known;
                }
                if (!knownSlot)
                    return Result<std::shared_ptr<WorldRecord>>::Failure(
                        MakeError(Network::NetworkErrors::NetworkModeInvalid,
                                  "The bounded current/replacement role records have not retired."));
            }
            auto record = std::make_shared<WorldRecord>(request);
            record->request = request;
            record->generation = owners.nextWorldGeneration++;
            *free = record;
            if (knownSlot)
                *knownSlot = record;
            return Result<std::shared_ptr<WorldRecord>>::Success(std::move(record));
        }

        /** @brief Match the complete world-instance request, not merely its reusable Scene ID. */
        bool MatchesRequest(const std::shared_ptr<WorldRecord> &record, const Network::NetworkModeServiceRequest &request) {
            return record && record->request.world == request.world && record->request.scene == request.scene &&
                   record->request.mode == request.mode;
        }

        /** @brief Host factories borrow only their actual owner lane and cannot reopen retired native admission. */
        Result<void> ValidateFactoryOwner(const WorldOwners &owners) {
            if (std::this_thread::get_id() != owners.owner)
                return Result<void>::Failure(MakeError(Physics::PhysicsErrors::ThreadAffinityViolation));
            if (owners.retirementStarted)
                return Result<void>::Failure(MakeError(Physics::PhysicsErrors::InvalidState));
            return Result<void>::Success();
        }

        /** @brief Pair factories before preparation, preserving exact candidate ownership even for repeated Scene IDs. */
        [[nodiscard]] Result<std::shared_ptr<WorldRecord>> SelectRecord(WorldOwners &owners,
                                                                        const Network::NetworkModeServiceRequest &request) {
            if (auto valid = ValidateFactoryOwner(owners); valid.HasError())
                return Result<std::shared_ptr<WorldRecord>>::Failure(valid.ErrorValue());
            using Kind = Network::NetworkModeServiceKind;
            if (request.service != Kind::Scene && request.service != Kind::Physics)
                return Result<std::shared_ptr<WorldRecord>>::Success({});
            if (request.service == Kind::Scene)
                return CreateRecord(owners, request);
            std::shared_ptr<WorldRecord> selected;
            std::weak_ptr<WorldRecord> *slot{};
            for (auto &candidate : owners.unpaired) {
                auto record = candidate.lock();
                if (!MatchesRequest(record, request))
                    continue;
                if (selected)
                    return Result<std::shared_ptr<WorldRecord>>::Failure(
                        MakeError(Network::NetworkErrors::NetworkModeInvalid,
                                  "Ambiguous unpaired Scene candidates; construct each Scene/Physics pair together."));
                selected = std::move(record);
                slot = &candidate;
            }
            if (!selected)
                return Result<std::shared_ptr<WorldRecord>>::Failure(
                    MakeError(Network::NetworkErrors::NetworkModeInvalid, "Physics has no exact unpaired Scene candidate."));
            slot->reset();
            return Result<std::shared_ptr<WorldRecord>>::Success(std::move(selected));
        }

    }  // namespace

    namespace {
        /** @brief Registers the same exact pair factories for synthetic and explicit project compositions. */
        Network::NetworkModeFactories InstallWorldFactories(const std::shared_ptr<WorldOwners> &owners,
                                                            Network::NetworkModeFactories selectedFactories) {
            using enum Network::NetworkModeServiceKind;
            for (const auto service : {Scene, Physics, Replication}) {
                selectedFactories.services[static_cast<std::size_t>(service)] =
                    [owners](const Network::NetworkModeServiceRequest &request) {
                    auto record = SelectRecord(*owners, request);
                    if (record.HasError())
                        return Result<std::unique_ptr<Network::INetworkModeService>>::Failure(record.ErrorValue());
                    return Result<std::unique_ptr<Network::INetworkModeService>>::Success(
                        std::make_unique<WorldService>(owners, request, std::move(record).Value()));
                };
            }
            return selectedFactories;
        }
    }  // namespace

    Network::NetworkModeFactories ComposeHeadlessNetworkServices(
        std::shared_ptr<const Runtime::RuntimeSceneDefinition> sceneDefinition, Network::NetworkModeFactories selectedFactories,
        const std::optional<GameplayWorldSelection> &gameplay,
        const std::optional<Gameplay::GameplayReplicationAdmissionPolicy> &replicationAdmission, NetworkNativeRetirementPoll *retirement) {
        const auto owners = std::make_shared<WorldOwners>(std::move(sceneDefinition), gameplay, replicationAdmission);
        if (retirement)
            *retirement = [owners](std::uint32_t maximumRecords) {
                return owners->PollRetirement(maximumRecords);
            };
        return InstallWorldFactories(owners, std::move(selectedFactories));
    }

    namespace {
        /** @brief Copies only finite counters from the selected world's latest committed AI work. */
        HeadlessAiHealth CollectAiHealth(const WorldRecord &record, const std::uint64_t committedTick) {
            HeadlessAiHealth health;
            if (const auto perception = record.perception.Current())
                health.perceptionListeners = perception->ListenerCount();
            const auto scheduler = record.ai->TaskSchedulerAtSafePoint();
            const auto *report = scheduler.HasValue() ? scheduler.Value()->CompletedReportAtSafePoint() : nullptr;
            if (report && report->tick == committedTick) {
                health.decisionTick = report->tick;
                health.evaluated = report->evaluated;
                health.workUnits = report->workUnits;
                health.workersSubmitted = report->workerSubmissions;
                health.deferred = report->deferred;
                health.exhausted = report->exhausted;
                health.failed = report->failed;
                health.degraded = report->failed != 0 || report->exhausted != 0 || report->firstFailure || report->firstAdvisoryFailure;
            }
            return health;
        }

        /** @brief Looks up the exact live authoritative record within the host's bounded world slots. */
        std::optional<HeadlessAiHealth> ReadAiHealth(const WorldOwners &owners, const Network::NetworkModeWorld world,
                                                     const std::uint64_t committedTick) {
            if (std::this_thread::get_id() != owners.owner || owners.retirementStarted)
                return std::nullopt;
            for (const auto &slot : owners.known) {
                const auto record = slot.lock();
                if (record && record->request.world == world.kind && record->request.scene == world.scene && !record->closeRequested &&
                    record->ai && record->ai->ActiveBinding().HasValue())
                    return CollectAiHealth(*record, committedTick);
            }
            return std::nullopt;
        }

        Result<void> ValidateConfiguredInput(const std::shared_ptr<const Runtime::RuntimeSceneDefinition> &definition,
                                             const HeadlessSceneActivationInput &input,
                                             const std::optional<GameplayWorldSelection> &gameplay) {
            if (const auto policy = Prefab::PrefabLimitProfile::Create(input.prefabPolicy); policy.HasError())
                return Result<void>::Failure(policy.ErrorValue());
            if (!definition || input.load.deadline == std::chrono::steady_clock::time_point{} ||
                input.load.deadline == std::chrono::steady_clock::time_point::max())
                return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid));
            if (input.perceptionExecution && (!input.perceptionPositions || !input.perceptionExecution->generation))
                return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid,
                                                       "Native Perception requires a selected producer and Scene positions."));
            if (input.publicTransforms && (!gameplay || !gameplay->enabled))
                return Result<void>::Failure(MakeError(Network::NetworkErrors::ReplicationDescriptorIncompatible));
            const auto &decisions = input.cookedDecisions;
            if (input.decisionReplay && (!input.decisionReplay->Selection().IsValid() || !decisions || !decisions->execution))
                return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid,
                                                       "AI replay requires an exact agent, tick and configured cooked execution."));
            if (decisions && decisions->provider == HeadlessDecisionProvider::SelectedNativeGameplay &&
                (!decisions->execution || decisions->environment.nodes || !gameplay || !gameplay->enabled ||
                 gameplay->nativeArtifact.empty() || !gameplay->moduleRetirements))
                return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid,
                                                       "Selected native AI requires scoped native gameplay and execution."));
            const bool needsPrefab = decisions && std::ranges::any_of(decisions->bindings, [](const auto &binding) {
                return binding.prefab.has_value();
            });
            if (needsPrefab && (!gameplay || gameplay->prefabPermission != GameplayPrefabPermission::Granted))
                return Result<void>::Failure(
                    MakeError(Network::NetworkErrors::NetworkModeInvalid, "AI prefab selection requires the actual host's prefab grant."));
            const bool executesDecisions = decisions && decisions->execution;
            const bool dispatchesMotion =
                input.navigationMotion && executesDecisions && decisions->execution->scheduler.mode == AI::AiSchedulingMode::BestEffort;
            if (input.navigationPath && (!input.navigationPath->IsValid() || !input.navigation || !gameplay || !gameplay->enabled ||
                                         gameplay->nativeArtifact.empty()))
                return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid,
                                                       "Best-effort paths require cooked Navigation and native gameplay."));
            if (input.navigationMotion && (!input.navigationPath || !dispatchesMotion || input.decisionReplay))
                return Result<void>::Failure(MakeError(Navigation::NavigationErrors::CapabilityDescriptorInvalid));
            if (gameplay && gameplay->prefabPermission == GameplayPrefabPermission::Granted && !input.prefabLoads)
                return Result<void>::Failure(
                    MakeError(Network::NetworkErrors::NetworkModeInvalid, "Prefab grant requires an explicit committed artifact loader."));
            return Result<void>::Success();
        }

        /** @brief Denies unsupported live composition before creating Scene and native owners. */
        Result<void> ValidateLiveWorldSelection(const HeadlessSceneActivationInput &input,
                                                const std::optional<GameplayWorldSelection> &gameplay) {
            if (!input.mountedWorld || !input.mountedWorld->liveCell)
                return Result<void>::Success();
            if (!input.worldJobs || input.mountedWorld->initialCell || gameplay)
                return Result<void>::Failure(MakeError(WorldStreaming::WorldArtifactErrors::Unsupported,
                                                       "Live mounted CoreEcs requires the standalone Scene structural owner."));
            return Result<void>::Success();
        }

        Result<void> ValidateAnimationSelection(HeadlessSceneActivationInput &input) {
            if (input.animationTarget.IsValid() != static_cast<bool>(input.animationIdentities))
                return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid));
            if ((input.animationInputs || input.replicaAnimationInputs) && !input.animationIdentities)
                return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid));
            return HeadlessNetworkDetail::ValidateReplicaAnimationBindings(input);
        }
    }  // namespace

    namespace {
        /** @brief Binds a declared origin to this exact committed definition before installing either callback. */
        Result<void> AdmitPerceptionProject(const std::shared_ptr<const Runtime::RuntimeSceneDefinition> &definition,
                                            HeadlessSceneActivationInput &input) {
            if (!input.perceptionProject)
                return Result<void>::Success();
            const auto &[policy, registry, provider] = *input.perceptionProject;
            if (!definition || !policy || input.perceptionPositions || input.perceptionGroupPositions)
                return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionDependencyMissing,
                                                       "Project Perception needs one complete position and descriptor authority."));
            switch (provider) {
                case HeadlessPerceptionProvider::CapturedRegistry:
                    if (!registry)
                        return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionDependencyMissing));
                    break;
                case HeadlessPerceptionProvider::SelectedNativeGameplay:
                    if (registry || input.perceptionExecution || !policy->native || !input.cookedDecisions ||
                        input.cookedDecisions->provider != HeadlessDecisionProvider::SelectedNativeGameplay)
                        return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionDependencyMissing,
                                                               "Native Perception requires schema two and a selected prepared module."));
                    break;
            }
            if (definition->Id() != policy->definition || definition->Revision() != policy->revision)
                return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionSpatialStale));
            auto sources = MakeHeadlessPerceptionPositionSources(policy);
            input.perceptionPositions = std::move(sources.scene);
            input.perceptionGroupPositions = std::move(sources.group);
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ComposeHeadlessSceneNetworkServices */
    Result<NetworkProductComposition> ComposeHeadlessSceneNetworkServices(
        std::shared_ptr<const Runtime::RuntimeSceneDefinition> definition, HeadlessSceneActivationInput input,
        Network::NetworkModeFactories selectedFactories, const std::optional<GameplayWorldSelection> &gameplay,
        const std::optional<Gameplay::GameplayReplicationAdmissionPolicy> &replicationAdmission) {
        if (auto admitted = AdmitPerceptionProject(definition, input); admitted.HasError())
            return Result<NetworkProductComposition>::Failure(admitted.ErrorValue());
        if (auto valid = ValidateConfiguredInput(definition, input, gameplay); valid.HasError())
            return Result<NetworkProductComposition>::Failure(std::move(valid).ErrorValue());
        if (auto valid = ValidateAnimationSelection(input); valid.HasError())
            return Result<NetworkProductComposition>::Failure(std::move(valid).ErrorValue());
        if (auto valid = ValidateLiveWorldSelection(input, gameplay); valid.HasError())
            return Result<NetworkProductComposition>::Failure(std::move(valid).ErrorValue());
        auto owners = std::make_shared<WorldOwners>(std::move(definition), gameplay, replicationAdmission);
        if (auto prepared = owners->PreparePhysicsRuntime(input.mode); prepared.HasError())
            return Result<NetworkProductComposition>::Failure(prepared.ErrorValue());
        owners->configured.emplace(std::move(input));
        if (auto &configured = *owners->configured; configured.initialSceneState) {
            if (!configured.publicTransforms)
                return Result<NetworkProductComposition>::Failure(MakeError(Network::NetworkErrors::AdmissionPolicyInvalid));
            if (auto bound = configured.initialSceneState->BindWorlds(owners); bound.HasError())
                return Result<NetworkProductComposition>::Failure(std::move(bound).ErrorValue());
        }
        NetworkProductComposition composition{InstallWorldFactories(owners, std::move(selectedFactories)),
                                              {},
                                              [owners](std::uint32_t maximumRecords) {
            return owners->PollRetirement(maximumRecords);
        }};
        if (owners->configured->cookedDecisions || owners->configured->perceptionPositions) {
            composition.aiHealth = [owners](const Network::NetworkModeWorld world, const std::uint64_t tick) {
                return ReadAiHealth(*owners, world, tick);
            };
        }
        const auto &mount = owners->configured->mountedWorld;
        if (owners->configured->decisionReplay || owners->configured->animationIdentities || owners->configured->publicTransforms ||
            (mount && mount->liveCell) || (owners->gameplay && owners->gameplay->vfxScene)) {
            if (owners->configured->animationIdentities && !owners->configured->animationTarget.IsValid())
                return Result<NetworkProductComposition>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid));
            composition.fixedParticipant = [owners](Runtime::RuntimeDispatchSource source) {
                return HeadlessFixedParticipant::Create(owners, std::move(source));
            };
        }
        return Result<NetworkProductComposition>::Success(std::move(composition));
    }

    /** @copydoc InspectHeadlessVfxFrame */
    const Vfx::VfxExtractionOutcome *InspectHeadlessVfxFrame(const Network::INetworkModeService &service) noexcept {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        return world ? world->VfxFrame() : nullptr;
    }

    const Runtime::SceneAnimationRuntime *InspectHeadlessAnimation(const Network::INetworkModeService &service) noexcept {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        return world ? world->AnimationOwner() : nullptr;
    }

    /** @copydoc QueueHeadlessAnimationAction */
    Animation::GraphActionCommandOutcome QueueHeadlessAnimationAction(Network::INetworkModeService &service, Runtime::EntityRef actor,
                                                                      const Animation::GraphActionRequest &request) {
        auto *world = dynamic_cast<WorldService *>(&service);
        return world ? world->QueueAnimationAction(actor, request)
                     : Animation::GraphActionCommandOutcome{Animation::GraphActionFailure::Closed};
    }

    /** @copydoc AcquireHeadlessPublicTransforms */
    std::shared_ptr<const Network::SceneReplicationCommitSource> AcquireHeadlessPublicTransforms(
        const Network::INetworkModeService &service) noexcept {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        return world ? world->PublicTransforms() : nullptr;
    }

    /** @copydoc InspectHeadlessPerception */
    std::shared_ptr<const AI::PerceptionSpatialSnapshot> InspectHeadlessPerception(const Network::INetworkModeService &service) noexcept {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        return world ? world->PerceptionSnapshot() : nullptr;
    }

    /** @copydoc InspectHeadlessPerceptionMemory */
    Result<AI::PerceptionMemorySnapshot> InspectHeadlessPerceptionMemory(const Network::INetworkModeService &service,
                                                                         const Runtime::EntityRef recipient) {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        if (!world)
            return Result<AI::PerceptionMemorySnapshot>::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
        return world->PerceptionMemory(recipient);
    }

    /** @copydoc InspectHeadlessDecisionValue */
    Result<std::optional<AI::BlackboardValue>> InspectHeadlessDecisionValue(const Network::INetworkModeService &service, AI::AgentId agent,
                                                                            AI::BlackboardKeyId key) {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        if (!world)
            return Result<std::optional<AI::BlackboardValue>>::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
        return world->ReadDecisionValue(agent, key);
    }

    /** @copydoc InspectHeadlessDecisionFailure */
    Result<std::optional<AI::AiRuntimeDecisionDiagnostic>> InspectHeadlessDecisionFailure(const Network::INetworkModeService &service) {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        if (!world)
            return Result<std::optional<AI::AiRuntimeDecisionDiagnostic>>::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
        return world->ReadDecisionFailure();
    }

    /** @copydoc InspectHeadlessScene */
    std::optional<Runtime::RuntimeSceneView> InspectHeadlessScene(const Network::INetworkModeService &service) noexcept {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        return world ? world->ActiveScene() : std::nullopt;
    }

    /** @copydoc InspectHeadlessWorldOrigin */
    std::optional<WorldStreaming::OriginFrameLease> InspectHeadlessWorldOrigin(const Network::INetworkModeService &service) {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        return world ? world->WorldOrigin() : std::nullopt;
    }

    /** @copydoc RequestHeadlessWorldCell */
    Result<void> RequestHeadlessWorldCell(const Network::INetworkModeService &service, const CommittedWorldInitialCell &selected,
                                          const CancellationToken &cancellation) {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        return world ? world->RequestMountedCell(selected, cancellation)
                     : Result<void>::Failure(MakeError(WorldStreaming::WorldArtifactErrors::Stale));
    }

    /** @copydoc WithdrawHeadlessWorldCell(const Network::INetworkModeService &) */
    Result<void> WithdrawHeadlessWorldCell(const Network::INetworkModeService &service) {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        return world ? world->WithdrawMountedCell() : Result<void>::Failure(MakeError(WorldStreaming::WorldArtifactErrors::Stale));
    }

    /** @copydoc WithdrawHeadlessWorldCell(const Network::INetworkModeService &, Runtime::SceneInstanceId) */
    Result<void> WithdrawHeadlessWorldCell(const Network::INetworkModeService &service, const Runtime::SceneInstanceId occurrence) {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        return world ? world->WithdrawMountedCell(occurrence)
                     : Result<void>::Failure(MakeError(WorldStreaming::WorldArtifactErrors::Stale));
    }

    /** @copydoc CaptureHeadlessWorldRenderGeometry */
    Result<MountedWorldRenderPacket> CaptureHeadlessWorldRenderGeometry(const Network::INetworkModeService &service,
                                                                        const Runtime::SceneObjectId camera,
                                                                        std::span<const Runtime::SceneObjectId> meshes,
                                                                        Runtime::PrimitiveMeshCache &primitives,
                                                                        Render::RenderOriginSceneAdapter &renderer) {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        if (!world)
            return Result<MountedWorldRenderPacket>::Failure(MakeError(WorldStreaming::WorldArtifactErrors::Stale));
        return world->CaptureWorldRenderGeometry(camera, meshes, primitives, renderer);
    }

    std::optional<Runtime::EntityRef> InspectHeadlessAnimationEntity(const Network::INetworkModeService &service,
                                                                     Runtime::SceneObjectId object) noexcept {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        return world ? world->AnimationEntity(object) : std::nullopt;
    }

    /** @copydoc InspectHeadlessNavigationContext */
    std::shared_ptr<const Gameplay::GameplayNavigationContext> InspectHeadlessNavigationContext(
        const Network::INetworkModeService &service) noexcept {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        return world ? world->NavigationContext() : nullptr;
    }

    /** @copydoc StageHeadlessNavigationMotion */
    Result<void> StageHeadlessNavigationMotion(const Network::INetworkModeService &service, AI::AiTaskContinuation task,
                                               Navigation::NavigationMovementRequest request, const std::uint64_t tick) {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        return world ? world->StageNavigationMotion(std::move(task), std::move(request), tick)
                     : Result<void>::Failure(MakeError(AI::AIErrors::RuntimeUnavailable));
    }

    /** @copydoc InspectHeadlessGameplayContext */
    std::shared_ptr<const Gameplay::GameplayPhysicsContext> InspectHeadlessGameplayContext(
        const Network::INetworkModeService &service) noexcept {
        const auto *world = dynamic_cast<const WorldService *>(&service);
        return world ? world->PhysicsContext() : nullptr;
    }
}  // namespace Horo::Application::Internal
