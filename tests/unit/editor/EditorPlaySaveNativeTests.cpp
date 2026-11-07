#include "AllocationProbe.h"
#include "EditorPlaySaveHost.h"
#include "GameplayBehaviorRestoreFixture.h"
#include "GameplayRetirementTestSupport.h"
#include "Horo/Application/ProjectSourceDocument.h"
#include "Horo/Platform/SecureRandom.h"
#include "ProjectReplicationTestSupport.h"
#include "SaveFilesystemTestSupport.h"
#include "editor/document/SceneDocumentPersistence.h"
#include "editor/gameplay/EditorPlaySessionController.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Runtime;
    using namespace Horo::Application::Internal;
    using namespace Horo::Runtime::SaveFilesystemTest;

    /** @brief Preserves genuine loader, scheduler, filesystem and canonical operation diagnostics. */
    template <class T> T Take(Result<T> result) {
        INFO((result.HasError() ? result.ErrorValue().message : "Actual operation accepted."));
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    /** @brief Requires an actual void operation; no success value is synthesized by this fixture. */
    void Accept(Result<void> result) {
        INFO((result.HasError() ? result.ErrorValue().message : "Actual operation accepted."));
        REQUIRE(result.HasValue());
    }

    /** @brief Issues fixture-owned opaque durable identities; production still uses its native entropy policy. */
    template <class T> T Identity(std::uint8_t value) {
        SaveIdentityDetail::Bytes bytes{};
        bytes.back() = value;
        return Take(T::FromBytes(bytes));
    }

    /** @brief Selects the same explicit Test storage capability as the qualified save-host tests. */
    PlaySaveStorageConfig Storage(const std::filesystem::path &path) {
        const NullProcessService process;
        const auto product = Identity<ProductStorageId>(11);
        return {Take(ResolveProductSaveRoot({product, SaveRootPlatform::Test, path}, process)),
                {product, Identity<EnvironmentStorageId>(12),
                 UserProfileOwner{Identity<LocalUserStorageId>(13), Identity<GameProfileId>(14)}},
                Identity<SaveProjectId>(15),
                Identity<SaveWorldId>(1),
                Identity<SaveBaseSceneId>(16),
                Platform::CreateNativeSecureRandomSource(),
                "native-play-test",
                "native-play-test"};
    }

    /** @brief Acquires the complete actual installed inventory, retaining one canonical adapter per real installation. */
    std::vector<PlaySaveGameplayBinding> Bindings(const Gameplay::LoadedGameModule &module) {
        auto declarations = Take(module.PersistenceDeclarations());
        std::vector<PlaySaveGameplayBinding> result;
        for (const auto &declaration : declarations) {
            auto installed = Take(module.AcquireInstalledPersistence(declaration.descriptor.participant.participant));
            auto adapter = std::dynamic_pointer_cast<const GameplayPersistenceAdapter>(installed.AcquireAdapter());
            REQUIRE(adapter);
            result.push_back({declaration, std::move(installed), std::move(adapter)});
        }
        return result;
    }

    /** @brief Uses two real native attachments with duplicate per-object IDs and distinct durable entities. */
    SceneDocumentSnapshot Authoring() {
        const auto type = Take(Gameplay::BehaviorTypeId::Parse("game.tests.dynamic_mover"));
        SceneObjectComponentSet first, second;
        first.camera = CameraComponent{};
        first.behaviors.emplace_back(Gameplay::BehaviorInstanceId{1}, type, 1, true, std::vector<Gameplay::BehaviorField>{});
        second.behaviors.emplace_back(Gameplay::BehaviorInstanceId{1}, type, 1, false, std::vector<Gameplay::BehaviorField>{});
        return {DocumentRevision{7},
                DocumentStateId{11},
                {{SceneSource::SceneObjectId{1}, {}, "First", {}, {}, std::move(first)},
                 {SceneSource::SceneObjectId{2}, {}, "Second", {}, {}, std::move(second)}}};
    }

    /** @brief Serializes the real detached authoring snapshot through the existing source-free scene codec. */
    std::vector<std::uint8_t> AuthoringBytes(const SceneDocumentSnapshot &document) {
        auto runtime = Take(ConvertSceneDocumentToRuntime(document, SceneDefinitionId{2}));
        return Take(SceneCook::EncodeCookedSceneDefinition(runtime));
    }

    /** @brief Associates every observed native value with its real durable entity, independent of visit/storage order. */
    struct DurableValue final {
        PersistentEntityId entity;
        Tests::GameplayBehaviorDurableFields state;
    };

    /** @brief Reads or writes actual native objects only through the Gameplay-owned scoped visitor. */
    class Fields final : public Gameplay::IBehaviorPersistenceVisitor {
    public:
        bool write{};
        std::uint64_t base{42};
        std::vector<DurableValue> actual;

        Result<void> Visit(std::span<const Gameplay::BehaviorRestoreInstance> instances) override {
            for (const auto &instance : instances) {
                auto *native = dynamic_cast<Tests::IGameplayBehaviorDurableFixture *>(instance.implementation);
                REQUIRE(native);
                if (write) {
                    Tests::GameplayBehaviorDurableFields fields{base + instance.persistentEntity.Bytes().back(), {}, {}};
                    if (instance.persistentEntity == Identity<PersistentEntityId>(1))
                        fields.peer = Gameplay::BehaviorEntityRestoreTarget{{Identity<SaveWorldId>(1), Identity<PersistentEntityId>(2)}, 1};
                    native->SetDurableFields(std::move(fields));
                }
                actual.push_back({instance.persistentEntity, native->DurableFields()});
            }
            return Result<void>::Success();
        }
    };

    /** @brief Assetless native-Gameplay fixture host: all serialization, slots, operations and restore bundles are production owners. */
    class NativePlayHost final : public EditorPlaySceneHost {
    public:
        NativePlayHost(SceneRuntimeId id, RuntimeSceneDefinition baseline, PlaySaveStorageConfig storage, JobSystem &jobs,
                       OperationStore &operations, Clock &clock, WallClock &wall)
            : scenes_(id), baseline_(std::move(baseline)), storage_(std::move(storage)), jobs_(jobs), operations_(operations),
              clock_(clock), wall_(wall) {}

        RuntimeSceneService &Scenes() noexcept override {
            return scenes_;
        }

        EditorPlaySaveSession *SaveSession() noexcept override {
            return save_.get();
        }

        EditorPlaySaveHost &Save() {
            return *save_;
        }

        std::shared_ptr<Gameplay::BehaviorRuntimeSceneOwner> Owner() const {
            return owner_;
        }

        Result<void> ConfigureSaveSession(std::vector<PlaySaveGameplayBinding> bindings,
                                          std::shared_ptr<Gameplay::BehaviorRuntimeSceneOwner> owner) override {
            owner_ = std::move(owner);
            if (save_)
                return save_->RebindGameplay(std::move(bindings));
            std::vector<Gameplay::GameplayPersistenceDeclaration> declarations;
            for (const auto &binding : bindings)
                declarations.push_back(binding.declaration);
            auto schema = std::make_shared<PlaySaveStorageSchema>(
                PlaySaveStorageSchema{baseline_, Records(), std::move(declarations), Gameplay::BehaviorLifecycleParticipant()});
            auto live = std::make_shared<const std::vector<PlaySaveGameplayBinding>>(std::move(bindings));
            auto created = EditorPlaySaveHost::Create(scenes_, {jobs_, operations_, clock_, wall_, storage_}, schema, live, owner_);
            if (created.HasError())
                return Result<void>::Failure(created.ErrorValue());
            schema_ = std::move(schema);
            save_ = std::move(created).Value();
            return Result<void>::Success();
        }

        bool RetireSaveSession() noexcept override {
            return !save_ || save_->SuspendForModuleReload();
        }

        Result<void> BeginPresentationUpdate() override {
            if (!save_ || !save_->HasRuntimeBinding())
                return Result<void>::Success();
            const auto begun = save_->BeginMutation();
            mutating_ = begun.HasValue();
            return begun;
        }

        Result<void> BeginFixedUpdate(Gameplay::FixedDeltaTime) override {
            return BeginPresentationUpdate();
        }

        Result<void> FixedUpdate(Gameplay::FixedDeltaTime) override {
            return Result<void>::Success();
        }

        Result<void> CommitSceneChanges(const FrameContext &frame) override {
            const auto scene = scenes_.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, frame);
            if (scene.HasError())
                return scene;
            if (!save_)
                return Result<void>::Success();
            if (mutating_ && !save_->IsRetiring()) {
                const auto ended = save_->EndMutation();
                if (ended.HasError())
                    return ended;
                mutating_ = false;
            }
            return save_->CommitAtSafePoint(frame);
        }

        PersistentEntityIdentityMap Map() const {
            return Take(CapturePlaySaveIdentities(*scenes_.ActiveScene(), baseline_, schema_->identities));
        }

        void Shutdown() noexcept override {
            if (save_ && !save_->IsSettled())
                std::terminate();
            save_.reset();
            scenes_.Shutdown();
            owner_.reset();
        }

    private:
        /** @brief Binds the explicit fixture identities to actual authored objects, never transient runtime slots. */
        std::vector<PersistentEntityRecord> Records() const {
            std::vector<PersistentEntityRecord> records;
            for (const auto &entity : baseline_.Entities()) {
                REQUIRE(entity.object.value <= 2);
                records.push_back({Identity<PersistentEntityId>(static_cast<std::uint8_t>(entity.object.value)),
                                   {1},
                                   AuthoredPersistentEntityProvenance{baseline_.Id(), entity.object},
                                   PersistentEntityDisposition::Live});
            }
            return records;
        }

        RuntimeSceneService scenes_;
        RuntimeSceneDefinition baseline_;
        PlaySaveStorageConfig storage_;
        JobSystem &jobs_;
        OperationStore &operations_;
        Clock &clock_;
        WallClock &wall_;
        std::shared_ptr<const PlaySaveStorageSchema> schema_;
        std::unique_ptr<EditorPlaySaveHost> save_;
        std::shared_ptr<Gameplay::BehaviorRuntimeSceneOwner> owner_;
        bool mutating_{};
    };

    /** @brief Measures the real read-only snapshot call so the next failure targets the actual owner lifecycle vector allocation. */
    std::size_t ReloadSnapshotAllocations(Gameplay::BehaviorRuntime &runtime) {
        std::optional<Result<Gameplay::BehaviorRuntimeReloadSnapshot>> captured;
        Tests::AllocationProbe::Measurement measured;
        {
            Tests::AllocationProbe::ScopedMeasurement measurement;
            captured.emplace(runtime.CaptureReloadSnapshot());
            measured = measurement.Snapshot();
        }
        REQUIRE(captured->HasValue());
        REQUIRE(measured.requests > 0);
        return measured.requests;
    }

    /** @brief Injects exactly one actual allocation failure without Catch/framework allocations inside the measured call. */
    Result<EditorPlayReloadSnapshot> FailQuiesceAllocation(EditorPlaySessionController &play, std::size_t snapshotAllocations) {
        Tests::AllocationProbe::ScopedFailure failure{snapshotAllocations};
        return play.QuiesceForReload();
    }

    /** @brief Owns actual native images, canonical Play/session/slot owners and bounded explicit fixture waits. */
    class NativeCycle final {
    public:
        NativeCycle() {
            probe = std::make_unique<Tests::NativeGameplayFailureControl>(0);
            module = Take(loader.Load(HORO_TEST_GAME_MODULE_PATH, Tests::NativeRetirementExpectation())).owner;
            Start();
        }

        ~NativeCycle() {
            Stop();
            jobs.StopAccepting();
            jobs.Shutdown(ShutdownPolicy::Drain);
            module.reset();
            probe.reset();
        }

        /** @brief Starts through the real controller/factory and receives the actual host selected for that activation. */
        void Start() {
            phase = "Start";
            play.ConfigureSaveBindings(Bindings(*module));
            EditorPlaySceneFactory factory = [this](SceneRuntimeId id, const RuntimeSceneDefinition &definition) {
                auto host = std::make_unique<NativePlayHost>(id, definition, Storage(directory.Path()), jobs, operations, clock, wall);
                active = host.get();
                return Result<std::unique_ptr<EditorPlaySceneHost>>::Success(std::move(host));
            };
            Accept(play.Start(authoring, module->Registry(), module->Components(), factory));
            Await([this] {
                return play.State() == EditorPlaySessionState::Playing && Ready();
            });
        }

        /** @brief Drains actual asynchronous owner work without sleeping or manufacturing terminal state. */
        template <class Predicate> void Await(Predicate ready) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!ready() && std::chrono::steady_clock::now() < deadline) {
                play.PresentationUpdate({0});
                std::this_thread::yield();
            }
            INFO((play.LastError() ? play.LastError()->message : "No controller error."));
            INFO(WaitDiagnostics());
            REQUIRE(ready());
        }

        bool Ready() {
            const auto binding = play.SaveBinding().lock();
            INFO((binding && binding->saveError ? binding->saveError->message : "No save setup error."));
            auto *save = active ? active->SaveSession() : nullptr;
            const auto snapshot = save ? save->Snapshot() : nullptr;
            INFO((snapshot && snapshot->error ? snapshot->error->message : "No save error."));
            return snapshot && !snapshot->operationPending && snapshot->availability == PlaySaveAvailability::Ready;
        }

        void Command(PlaySaveCommand command) {
            phase = "Command";
            lastCommand = command.kind;
            ++commandSequence;
            auto binding = play.SaveBinding().lock();
            REQUIRE(binding);
            REQUIRE(binding->save);
            binding->saveCommand = std::move(command);
            play.PresentationUpdate({0});
            INFO((binding->saveError ? binding->saveError->message : "Actual UI save intent admitted."));
            REQUIRE_FALSE(binding->saveError);
            Await([this] {
                return Ready();
            });
        }

        SaveManagerPage Page() const {
            return Take(active->Save().Snapshot()->manager->Page({.maximumRows = 16}));
        }

        std::vector<DurableValue> Values(bool write = false, std::uint64_t value = 42) {
            Fields fields;
            fields.write = write;
            fields.base = value;
            if (write)
                Accept(active->Save().BeginMutation());
            auto target =
                Take(active->Owner()->BindingForActive(Take(Gameplay::BehaviorTypeId::Parse("game.tests.dynamic_mover")), active->Map()));
            Accept(target.Visit(fields));
            if (write)
                Accept(active->Save().EndMutation());
            return fields.actual;
        }

        /** @brief Resolves an observed value by the complete admitted durable entity key, not callback index. */
        DurableValue ValueAt(std::uint8_t id) {
            const auto values = Values();
            const auto found = std::ranges::find(values, Identity<PersistentEntityId>(id), &DurableValue::entity);
            REQUIRE(found != values.end());
            return *found;
        }

        void Stop() {
            phase = "Stop";
            play.Stop();
            Await([this] {
                return play.State() == EditorPlaySessionState::Idle;
            });
            active = nullptr;
        }

        void Reload() {
            phase = "Reload retirement";
            Await([this] {
                return play.PrepareSaveRetirement();
            });
            auto snapshot = Take(play.QuiesceForReload());
            auto moduleState = Take(module->PrepareReload());
            probe->configure(nullptr);
            probe->configure = [](Tests::GameplayRetirementFixture *) noexcept {
            };
            probe->image.reset();
            module.reset();
            probe.reset();
            probe = std::make_unique<Tests::NativeGameplayFailureControl>(0);
            module = Take(loader.Load(HORO_TEST_GAME_MODULE_PATH, Tests::NativeRetirementExpectation())).owner;
            Accept(module->RestoreReload(moduleState));
            Accept(play.RestoreAfterReload(module->Registry(), snapshot));
            play.ConfigureSaveBindings(Bindings(*module));
            phase = "Reload rebind";
            Await([this] {
                return Ready();
            });
        }

        /** @brief Formats genuine error identities rather than hiding the host failure beneath readiness. */
        static void DescribeError(std::ostream &output, const std::optional<Error> &error) {
            if (error)
                output << " error=" << error->domain.Value() << '/' << error->code.Value() << ':' << error->message;
        }

        /** @brief Reads the immutable Manager projection; no slot or operation is manufactured by the diagnostic. */
        static void DescribeManager(std::ostream &output, const SaveManagerProjection &manager) {
            const auto page = manager.Page({.maximumRows = 16});
            if (page.HasValue())
                output << " slots=" << page.Value().rows.size();
            else
                output << " pageError=" << page.ErrorValue().code.Value() << ':' << page.ErrorValue().message;
            for (const auto &operation : manager.Operations())
                output << " managerOperation=" << operation.operation << ",kind=" << static_cast<int>(operation.kind)
                       << ",state=" << static_cast<int>(operation.state) << ",stage=" << static_cast<int>(operation.stage)
                       << ",commit=" << static_cast<int>(operation.commit);
        }

        /** @brief Reports the actual current host projection through its stable controller binding. */
        void DescribeSave(std::ostream &output) {
            const auto binding = play.SaveBinding().lock();
            if (!binding) {
                output << " noSaveBinding";
                return;
            }
            DescribeError(output, binding->saveError);
            const auto snapshot = binding->save ? binding->save->Snapshot() : nullptr;
            if (!snapshot) {
                output << " noSaveSnapshot";
                return;
            }
            output << " revision=" << snapshot->revision << " availability=" << static_cast<int>(snapshot->availability)
                   << " pending=" << snapshot->operationPending << " loading=" << snapshot->loading;
            DescribeError(output, snapshot->error);
            if (snapshot->manager)
                DescribeManager(output, *snapshot->manager);
        }

        /** @brief Copies bounded real scheduler and application-operation state only after the unchanged wait. */
        void DescribeWork(std::ostream &output) const {
            if (const auto snapshot = jobs.SnapshotIfChanged(0)) {
                for (const auto &job : snapshot->jobs) {
                    output << " job=" << job.id << ",state=" << static_cast<int>(job.state);
                    DescribeError(output, job.error);
                }
            }
            if (const auto snapshot = operations.SnapshotIfChanged(0)) {
                for (const auto &operation : snapshot->operations) {
                    output << " applicationOperation=" << operation.id << ",state=" << static_cast<int>(operation.state)
                           << ",phase=" << operation.phase << ",message=" << operation.message;
                    DescribeError(output, operation.error);
                }
            }
        }

        /** @brief Preserves phase and accepted-command sequence at the failing assertion, outside helper INFO scopes. */
        std::string WaitDiagnostics() {
            std::ostringstream output;
            output << "phase=" << phase << " commandSequence=" << commandSequence << " commandKind=" << static_cast<int>(lastCommand)
                   << " controllerState=" << static_cast<int>(play.State());
            DescribeSave(output);
            DescribeWork(output);
            return output.str();
        }

        const char *phase{"Construction"};
        std::size_t commandSequence{};
        PlaySaveCommandKind lastCommand{PlaySaveCommandKind::Refresh};

        TemporaryDirectory directory;
        SteadyClock clock;
        SystemWallClock wall;
        JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 16}};
        OperationStore operations{16, 32};
        std::unique_ptr<Tests::NativeGameplayFailureControl> probe;
        Gameplay::GameModuleHost loader;
        std::unique_ptr<Gameplay::LoadedGameModule> module;
        SceneDocumentSnapshot authoring{Authoring()};
        NativePlayHost *active{};
        EditorPlaySessionController play;
    };

    /** @brief Uses the real workspace native manifest/reload transaction and real save host, in an isolated project. */
    class WorkspaceNativeCycle final {
    public:
        WorkspaceNativeCycle() {
            PrepareProject();
            manifest = Tests::WriteGameplayModuleManifest(directory.Path(), HORO_TEST_GAME_MODULE_PATH, "game.tests",
                                                          Gameplay::CurrentGameplayBuildFingerprint(),
                                                          Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH));
            Accept(preview.Startup(cancellation.Token()));
            EditorWorkspaceDependencies dependencies{.mutations = &mutations, .durableFiles = &files};
            dependencies.gameplay.sceneFactory = [this](SceneRuntimeId id, const RuntimeSceneDefinition &definition) {
                auto host = std::make_unique<NativePlayHost>(id, definition, Storage(directory.Path()), jobs, operations, clock, wall);
                active = host.get();
                return Result<std::unique_ptr<EditorPlaySceneHost>>::Success(std::move(host));
            };
            workspace =
                std::make_unique<EditorWorkspaceController>(directory.Path(), preview, Assets::AssetRegistrySnapshot{}, dependencies);
            INFO((workspace->InitializationError() ? workspace->InitializationError()->message : "Canonical project Scene admitted."));
            REQUIRE_FALSE(workspace->InitializationError());
            PrepareActor();
            Send(EditorWorkspaceViewCommand::StartPlay);
            Await([&] {
                return workspace->ViewModel().playState == EditorPlayState::Playing && Ready();
            });
            RequireObjects();
        }

        ~WorkspaceNativeCycle() {
            Send(EditorWorkspaceViewCommand::StopPlay);
            Await([&] {
                return workspace->ViewModel().playState == EditorPlayState::Idle;
            });
            workspace.reset();
            preview.Shutdown();
            jobs.StopAccepting();
            jobs.Shutdown(ShutdownPolicy::Drain);
        }

        /** @brief Extends the complete current fixture project without discarding its version, identity or network metadata. */
        void SeedDefaultSceneField() const {
            const auto path = directory.Path() / ".horo/project.json";
            std::ifstream input(path, std::ios::binary);
            REQUIRE(input.good());
            auto metadata = nlohmann::json::parse(input);
            metadata.at("settings")["defaultScene"] = "";
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << metadata.dump(2);
            REQUIRE(output.good());
        }

        /** @brief Seeds a real current project and explicit empty default Scene; no implicit bootstrap is assumed. */
        void PrepareProject() {
            Tests::WriteReplicationProject(directory.Path());
            SeedDefaultSceneField();
            const auto scene = directory.Path() / "assets/scenes/main.horo";
            std::filesystem::create_directories(scene.parent_path());
            {
                std::ofstream output(scene, std::ios::binary);
                output << "{\"schemaVersion\":1,\"objects\":[]}\n";
                REQUIRE(output.good());
            }
            Accept(SetProjectDefaultScenePath(directory.Path(), scene, mutations, files));
            REQUIRE(Application::LoadProjectSourceDocument(directory.Path()).HasValue());
            const auto loaded = Take(LoadProjectDefaultScene(directory.Path()));
            REQUIRE(loaded);
            REQUIRE(loaded->objects.empty());
        }

        /** @brief Verifies both authored objects are present in the canonical document and genuine active Play Scene. */
        void RequireObjects() const {
            const auto captured = Take(workspace->CaptureExternalSceneComparison());
            REQUIRE(captured.document.objects.size() == 2);
            const auto view = active->Scenes().ActiveScene();
            REQUIRE(view);
            for (const auto &object : captured.document.objects)
                REQUIRE(view->Find({object.id.value}));
            CHECK(captured.document.objects[0].id != captured.document.objects[1].id);
        }

        /** @brief Authors both camera and native actor explicitly before Play captures the exact document. */
        void PrepareActor() {
            EditorWorkspaceViewCommandData camera;
            camera.command = EditorWorkspaceViewCommand::CreatePrimitive;
            camera.primitivePayload = PrimitiveId{"primitive.object.camera"};
            workspace->ProcessCommand(camera);
            REQUIRE(workspace->ViewModel().objects.size() == 1);
            REQUIRE(workspace->ViewModel().objects.front().components.camera);
            EditorWorkspaceViewCommandData actor;
            actor.command = EditorWorkspaceViewCommand::CreatePrimitive;
            actor.primitivePayload = PrimitiveId{"primitive.object.empty"};
            workspace->ProcessCommand(actor);
            REQUIRE(workspace->ViewModel().objects.size() == 2);
            EditorWorkspaceViewCommandData behavior;
            behavior.command = EditorWorkspaceViewCommand::AttachBehaviorToObject;
            behavior.objectPayload = workspace->ViewModel().objects.back().id;
            behavior.behaviorTypePayload = Take(Gameplay::BehaviorTypeId::Parse("game.tests.dynamic_mover"));
            workspace->ProcessCommand(behavior);
            REQUIRE(workspace->ViewModel().objects.back().components.behaviors.size() == 1);
        }

        void Send(EditorWorkspaceViewCommand command) {
            EditorWorkspaceViewCommandData request;
            request.command = command;
            workspace->ProcessCommand(request);
        }

        /** @brief Advances genuine workspace/save state to a bounded condition, without replacing completion authority. */
        template <class Predicate> void Await(Predicate ready) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!ready() && std::chrono::steady_clock::now() < deadline) {
                workspace->UpdatePlayPresentation(0);
                std::this_thread::yield();
            }
            INFO(workspace->ViewModel().playError);
            REQUIRE(ready());
        }

        bool Ready() {
            const auto binding = workspace->PlaySaveBinding().lock();
            INFO((binding && binding->saveError ? binding->saveError->message : "No save setup error."));
            const auto snapshot = binding && binding->save ? binding->save->Snapshot() : nullptr;
            INFO((snapshot && snapshot->error ? snapshot->error->message : "No save error."));
            return snapshot && !snapshot->operationPending && snapshot->availability == PlaySaveAvailability::Ready;
        }

        void Save() {
            auto binding = workspace->PlaySaveBinding().lock();
            REQUIRE(binding);
            REQUIRE(binding->save);
            binding->saveCommand = PlaySaveCommand{.kind = PlaySaveCommandKind::SaveNew};
            workspace->UpdatePlayPresentation(0);
            INFO((binding->saveError ? binding->saveError->message : "Actual save admitted."));
            REQUIRE_FALSE(binding->saveError);
            Await([&] {
                return Ready();
            });
        }

        SaveManagerPage Page() const {
            const auto binding = workspace->PlaySaveBinding().lock();
            REQUIRE(binding);
            REQUIRE(binding->save);
            return Take(binding->save->Snapshot()->manager->Page({.maximumRows = 16}));
        }

        void ReloadAtSafePoint() {
            Tests::AdvanceLastWriteTime(manifest);
            workspace->UpdateGameplaySources(0.5F);
            workspace->UpdatePlayFixed({}, 1.0 / 60.0);
        }

        std::filesystem::path RollbackPath() const {
            return directory.Path() / ".horo/local/gameplay_module_rollback";
        }

        TemporaryDirectory directory;
        SteadyClock clock;
        SystemWallClock wall;
        JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 16}};
        OperationStore operations{16, 32};
        NativeDurableFileSystem files;
        ProjectMutationCoordinator mutations{files};
        CancellationSource cancellation;
        RuntimeSceneService preview;
        std::filesystem::path manifest;
        NativePlayHost *active{};
        std::unique_ptr<EditorWorkspaceController> workspace;
    };
}  // namespace

TEST_CASE("Native Play host saves loads resaves retires receipts reloads its module and carries real Scene identities",
          "[editor][play][save][native][reload]") {
    NativeCycle f;
    const auto authoringRevision = f.authoring.revision;
    const auto authoringBytes = AuthoringBytes(f.authoring);
    const auto initialScene = f.play.SceneView()->RuntimeId();
    const auto created = f.probe->control.behaviorCreated;
    const auto enabled = f.probe->control.behaviorEnabled;
    const auto started = f.probe->control.behaviorStarted;
    const auto materialized = f.probe->control.behaviorMaterialized;
    REQUIRE(f.Values(true).size() == 2);
    f.Command({.kind = PlaySaveCommandKind::SaveNew});
    REQUIRE(f.Page().rows.size() == 1);
    const auto slot = f.Page().rows[0].slot;
    const auto selection = Take(f.active->Save().Snapshot()->manager->Command(SaveManagerCommandKind::Load, slot));
    f.Values(true, 100);
    f.Command({.kind = PlaySaveCommandKind::Load, .selection = selection});
    const auto restoredScene = f.play.SceneView()->RuntimeId();
    CHECK(restoredScene != initialScene);
    CHECK(f.play.State() == EditorPlaySessionState::Paused);
    const auto values = f.Values();
    REQUIRE(values.size() == 2);
    const auto first = f.ValueAt(1).state;
    CHECK(first.value == 43);
    CHECK(f.ValueAt(2).state.value == 44);
    REQUIRE(first.resolvedPeer);
    CHECK(first.resolvedPeer->identity == Identity<PersistentEntityId>(2));
    CHECK(first.resolvedPeer->incarnation == 1);
    CHECK(f.probe->control.behaviorCreated == created);
    CHECK(f.probe->control.behaviorEnabled == enabled);
    CHECK(f.probe->control.behaviorStarted == started);
    CHECK(f.probe->control.behaviorPrehooks == 2);
    CHECK(f.probe->control.behaviorMaterialized == materialized + 1);
    CHECK(f.probe->control.behaviorFixed > 0);
    CHECK(f.probe->control.preparationRetired > 0);
    const auto lifecycle = Take(f.active->Owner()->CaptureLifecycle(f.active->Map()));
    REQUIRE(lifecycle.instances.size() == 2);
    CHECK(lifecycle.instances[0].instance == lifecycle.instances[1].instance);
    CHECK(lifecycle.instances[0].entity != lifecycle.instances[1].entity);
    CHECK(lifecycle.instances[0].flags.enabled);
    CHECK_FALSE(lifecycle.instances[1].flags.enabled);
    f.Command({.kind = PlaySaveCommandKind::SaveNew});
    REQUIRE(f.Page().rows.size() == 2);
    f.Reload();
    CHECK(f.Page().rows.size() == 2);
    const auto retained = f.Page();
    CHECK(std::ranges::any_of(retained.rows, [&](const auto &row) {
        return row.slot == slot;
    }));
    CHECK(f.ValueAt(1).state.value == 43);
    CHECK(f.ValueAt(2).state.value == 44);
    CHECK(f.play.SceneView()->RuntimeId() == restoredScene);
    const auto reboundCaptures = f.probe->control.behaviorCaptured;
    f.Command({.kind = PlaySaveCommandKind::SaveNew});
    CHECK(f.Page().rows.size() == 3);
    CHECK(f.probe->control.behaviorCaptured == reboundCaptures + 1);
    CHECK(f.authoring.revision == authoringRevision);
    CHECK(AuthoringBytes(f.authoring) == authoringBytes);
    CHECK(f.authoring.objects[0].components.behaviors[0].enabled);
    CHECK_FALSE(f.authoring.objects[1].components.behaviors[0].enabled);
    f.Stop();
    f.Start();
    CHECK(f.play.SceneView()->RuntimeId().value > restoredScene.value);
}

TEST_CASE("Native controller preserves its runnable owner and issued target when reload preparation allocation fails",
          "[editor][play][save][native][reload][allocation]") {
    NativeCycle f;
    SECTION("Playing remains runnable") {}
    SECTION("Paused remains paused") {
        Accept(f.play.Pause());
    }
    const auto prior = f.play.State();
    f.Await([&] {
        return f.play.PrepareSaveRetirement();
    });
    auto *runner = f.active->Owner()->Active();
    REQUIRE(runner);
    auto target =
        Take(f.active->Owner()->BindingForActive(Take(Gameplay::BehaviorTypeId::Parse("game.tests.dynamic_mover")), f.active->Map()));
    const auto before = f.play.SceneView()->RuntimeId();
    const auto allocations = ReloadSnapshotAllocations(*runner);
    const auto failed = FailQuiesceAllocation(f.play, allocations);
    REQUIRE(failed.HasError());
    CHECK(failed.ErrorValue().domain.Value() == SaveErrors::RestoreAllocationFailed.domain.Value());
    CHECK(failed.ErrorValue().code.Value() == SaveErrors::RestoreAllocationFailed.code.Value());
    CHECK(f.play.State() == prior);
    CHECK(f.play.SceneView()->RuntimeId() == before);
    CHECK(f.active->Owner()->Active() == runner);
    REQUIRE(f.play.SaveBinding().lock());
    CHECK(f.play.SaveBinding().lock()->runtime == runner);
    CHECK(target.CanUse());
    Fields visitor;
    Accept(target.Visit(visitor));
    CHECK(visitor.actual.size() == 2);
    const auto entity = *f.play.SceneView()->Find({1});
    const auto oldX = f.play.SceneView()->Get(entity).Value().localTransform->translation.x;
    if (prior == EditorPlaySessionState::Paused)
        Accept(f.play.Step());
    const Gameplay::GameplayInputAction move{Gameplay::GameplayActionId{"game.tests.move"}, 1, 0, true, true, false};
    Accept(f.play.FixedUpdate(std::span{&move, 1}, {1.0 / 60.0}));
    CHECK(f.play.State() == prior);
    CHECK(f.play.SceneView()->Get(entity).Value().localTransform->translation.x == oldX + 1);
    auto retry = Take(f.play.QuiesceForReload());
    CHECK_FALSE(target.CanUse());
    Accept(f.play.RestoreAfterReload(f.module->Registry(), retry));
    CHECK(f.play.State() == prior);
    REQUIRE(f.active->Owner()->Active());
    f.play.ConfigureSaveBindings(Bindings(*f.module));
    f.Await([&] {
        return f.Ready();
    });
}

TEST_CASE("Workspace failed native preparation rebinds actual old save sources and successful retry rebinds the new generation",
          "[editor][play][save][native][workspace][reload]") {
    WorkspaceNativeCycle f;
    f.Save();
    REQUIRE(f.Page().rows.size() == 1);
    const auto slot = f.Page().rows[0].slot;
    auto *runner = f.active->Owner()->Active();
    REQUIRE(runner);
    {
        std::ofstream blocker(f.RollbackPath(), std::ios::binary);
        REQUIRE(blocker.good());
        blocker << "Fixture-owned file blocks the actual rollback-artifact directory.";
    }
    f.ReloadAtSafePoint();
    CHECK(f.workspace->ViewModel().playState == EditorPlayState::Playing);
    CHECK(f.active->Owner()->Active() == runner);
    f.Await([&] {
        return f.Ready();
    });
    f.Save();
    REQUIRE(f.Page().rows.size() == 2);
    const auto retained = f.Page();
    CHECK(std::ranges::any_of(retained.rows, [&](const auto &row) {
        return row.slot == slot;
    }));
    REQUIRE(std::filesystem::remove(f.RollbackPath()));
    f.ReloadAtSafePoint();
    f.Await([&] {
        return f.workspace->ViewModel().playState == EditorPlayState::Playing && f.Ready();
    });
    f.Save();
    CHECK(f.Page().rows.size() == 3);
    f.RequireObjects();
}
