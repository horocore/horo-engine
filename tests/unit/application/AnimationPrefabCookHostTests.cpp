#include "AnimationGraphInstanceTestFixtures.h"
#include "Horo/Animation/AnimationAssetSource.h"
#include "Horo/Animation/AnimationGraphSource.h"
#include "Horo/Prefab/PrefabSpawnService.h"
#include "Horo/Prefab/PrefabTemplateCook.h"
#include "Horo/Scene/PrefabComponentProjection.h"
#include "PrefabSceneCookHostFixture.h"
#include "animation/AnimationRuntimeFixture.h"

#include <nlohmann/json.hpp>

namespace Horo::Application {
    namespace {
        using namespace PrefabCookTestSupport;

        template <typename T> T Take(Result<T> result) {
            INFO((result.HasError() ? result.ErrorValue().code.Value() + ": " + result.ErrorValue().message : "success"));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        Animation::AnimationAuthoringComponent Binding(const Animation::Test::Instance::Prepared &source) {
            Animation::AnimationAuthoringComponent binding;
            binding.component = Animation::Test::Id<Animation::AnimationComponentId>(7);
            binding.skeleton = source.skeleton->Data().skeleton;
            binding.source = {Animation::AnimationSourceKind::Graph, {}, source.program->Id()};
            return binding;
        }

        Prefab::RawComponentPayload NativeComponent(const Animation::AnimationAuthoringComponent &binding) {
            const std::vector<SceneSource::SceneObjectSnapshot> objects{
                {.id = {1}, .name = "Animation source", .components = {.animation = binding}}};
            const auto scene = nlohmann::json::parse(SceneSource::EncodeSceneSource({objects, {}}));
            const auto payload = scene.at("objects")[0].at("components").at("animation").dump();
            const auto schemas = Runtime::BuiltinSceneComponentSchemas();
            const auto schema =
                std::ranges::find(schemas, Runtime::BuiltinSceneComponentKind::Animation, &Runtime::BuiltinSceneComponentSchema::kind);
            REQUIRE(schema != schemas.end());
            Prefab::RawComponentPayload raw{Prefab::PrefabComponentInstanceId::Create(1).Value(),
                                            {.typeId = Take(Gameplay::ComponentTypeId::Parse(schema->typeId)),
                                             .schemaVersion = SceneSource::PrefabBuiltinComponentPayloadSchemaVersion}};
            for (const auto byte : payload)
                raw.component.payload.push_back(static_cast<std::byte>(byte));
            Runtime::RuntimeComponentSet checked;
            std::optional<Runtime::PrimitiveMeshDescriptor> primitive;
            REQUIRE(Take(SceneSource::ProjectPrefabBuiltinComponent(raw, checked, primitive)));
            CHECK(checked.animation == binding);
            return raw;
        }

        AssetRecord Add(HostFixture &fixture, std::string_view name, Assets::AssetId id, std::string_view type, std::string source) {
            const auto path = "assets/" + std::string(name) + ".json";
            fixture.WriteText(path, source);
            fixture.WriteText(path + ".horo", SidecarJson(id.ToString(), type));
            return {id, Type(type), Take(ProjectPath::Parse(path)), Take(ProjectPath::Parse(path + ".horo"))};
        }

        Prefab::PrefabDocument WriteRoot(HostFixture &fixture, const Animation::Test::Instance::Prepared &source,
                                         std::vector<Prefab::RawComponentPayload> components) {
            std::vector<Assets::AssetId> roles{source.skeleton->Data().skeleton.Asset(), source.program->Id().Asset()};
            std::ranges::sort(roles);
            auto document = Take(Prefab::PrefabDocument::Create({.projectVersion = fixture.version,
                                                                 .assetId = fixture.prefabId,
                                                                 .objects = {{.localId = {0},
                                                                              .name = "Actual animated root",
                                                                              .components = std::move(components)}},
                                                                 .referencedAssets = roles},
                                                                Take(Prefab::PrefabLimitProfile::Create({}))));
            fixture.WriteText("assets/hierarchy.prefab", Take(document.SerializeCanonical()));
            return document;
        }

        void Configure(HostFixture &fixture, const Animation::Test::Instance::Prepared &source) {
            auto records =
                std::vector<AssetRecord>(fixture.registry.Snapshot().Records().begin(), fixture.registry.Snapshot().Records().end());
            records.push_back(Add(fixture, "skeleton", source.skeleton->Data().skeleton.Asset(), "core.animation.skeleton",
                                  Take(Animation::SerializeSkeletonAnimationSource(*source.skeleton))));
            records.push_back(Add(fixture, "clip", source.compileClips.front().Data().descriptor.id.Asset(), "core.animation.clip",
                                  Take(Animation::SerializeClipAnimationSource(source.compileClips.front()))));
            records.push_back(Add(fixture, "graph", source.program->Id().Asset(), "core.animation.graph",
                                  Take(Animation::SerializeAnimationGraphSource(Animation::Test::Graph::Simple()))));
            REQUIRE(fixture.registry.Publish(std::move(records)).status == AssetRegistryBuildStatus::Complete);
            (void)WriteRoot(fixture, source, {NativeComponent(Binding(source))});
            fixture.request.runtimePrefabRoots = {fixture.prefabId};
            REQUIRE_FALSE(fixture.request.schemas);  // Native builtin validation is the actual Scene codec, not a fake Gameplay descriptor.
        }

        Prefab::CookedPrefab ReadTemplate(const HostFixture &fixture, const AssetCookGeneration &generation) {
            const auto contents = Take(ReadCookGenerationContents(NativeDurableFileSystem{}, generation, 16U * 1024U * 1024U));
            const auto found = std::ranges::find(contents.entries, fixture.prefabId, &Assets::AssetCookManifestEntry::assetId);
            REQUIRE(found != contents.entries.end());
            const auto index = static_cast<std::size_t>(found - contents.entries.begin());
            const auto artifact = Take(Assets::DecodeCookedArtifact(contents.artifacts[index]));
            return Take(Prefab::CookedPrefab::Parse(std::as_bytes(std::span{artifact.payload}), fixture.prefabId,
                                                    Take(Prefab::PrefabLimitProfile::Create({}))));
        }

        struct BuiltinProjector final : Prefab::PrefabComponentProjector {
            Result<bool> Project(const Prefab::RawComponentPayload &raw, Runtime::RuntimeComponentSet &components,
                                 std::optional<Runtime::PrimitiveMeshDescriptor> &primitive) const override {
                return SceneSource::ProjectPrefabBuiltinComponent(raw, components, primitive);
            }
        };

        struct AnimatedPrefabWorld final {
            HostFixture &host;
            std::unique_ptr<Internal::CommittedSceneAssets> committed;
            Assets::AssetLoadService sceneLoads;
            Assets::AssetLoadService prefabLoads;
            Runtime::RuntimeSceneService scenes;
            DeterministicClock clock;
            CancellationSource lifetime;
            CancellationToken token{lifetime.Token()};
            std::unique_ptr<Runtime::FrameScheduler> scheduler;
            Runtime::RuntimeLifecycle lifecycle;
            Animation::Test::Scene::Forwarder *forward{};
            Prefab::PrefabTemplateProvider templates;
            BuiltinProjector projector;
            std::unique_ptr<Prefab::PrefabSpawnService> service;
            std::shared_ptr<Gameplay::GameplayPrefabContext> context;
            Sha256Digest templateHash;
            std::uint64_t tick{1};

            explicit AnimatedPrefabWorld(HostFixture &fixture, const AssetCookGeneration &generation)
                : host(fixture), committed(Take(Internal::CommittedSceneAssets::Capture(*host.request.assets.publicationFiles,
                                                                                        {host.cooked.path, host.request.assets.target},
                                                                                        host.registry.Snapshot()))),
                  sceneLoads(host.jobs, committed->SceneProvider()), prefabLoads(host.jobs, committed->ArtifactProvider()),
                  scenes(host.registry, sceneLoads),
                  templates(host.registry, prefabLoads, scenes, Take(Prefab::PrefabLimitProfile::Create({}))) {
                const auto contents = Take(ReadCookGenerationContents(host.files, generation, 16U * 1024U * 1024U));
                const auto found = std::ranges::find(contents.entries, host.prefabId, &Assets::AssetCookManifestEntry::assetId);
                REQUIRE(found != contents.entries.end());
                templateHash =
                    ComputeSha256(std::as_bytes(std::span{contents.artifacts[static_cast<std::size_t>(found - contents.entries.begin())]}));
                PrepareRuntime();
                REQUIRE(templates.Startup(token).HasValue());
                service = std::make_unique<Prefab::PrefabSpawnService>(templates, scenes, nullptr, &projector);
                context = Take(service->Acquire({"game.test", scenes.ActiveScene()->RuntimeId(), true, true}));
            }

            ~AnimatedPrefabWorld() {
                service->Shutdown();
                templates.Shutdown();
                lifecycle.Shutdown();
                scenes.Shutdown();
                prefabLoads.Shutdown();
                sceneLoads.Shutdown();
            }

            void PrepareRuntime() {
                scheduler = Take(Runtime::FrameScheduler::Create(clock, {.fixedStep = Duration::FromNanoseconds(10)}));
                Runtime::SceneAnimationRuntimeConfig config;
                config.expectedDispatch = scheduler->DispatchSource();
                config.runtimeOwner = Animation::Test::Id<Animation::AnimationRuntimeId>(801);
                config.target = host.request.assets.target;
                config.ownerCancellation = token;
                auto adapter = Take(Runtime::SceneAnimationRuntime::Create(config));
                REQUIRE(scenes.AddActivationParticipant(adapter->MakeActivationParticipant()).HasValue());
                REQUIRE(scenes.AddStructuralParticipant(adapter->MakeStructuralParticipant()).HasValue());
                auto owner = std::make_unique<Animation::Test::Scene::Forwarder>(std::move(adapter));
                forward = owner.get();
                REQUIRE(lifecycle.AddParticipant(std::move(owner)).HasValue());
                REQUIRE(lifecycle.Startup(token).HasValue());
                REQUIRE(scenes.Startup(token).HasValue());
                Runtime::SceneDefinitionBuilder builder{{37}, {1}};
                builder.Add({.object = {900}});
                REQUIRE(scenes.QueuePreparation(Take(std::move(builder).Build())).HasValue());
                CommitScene();
                REQUIRE(scenes.ActiveScene());
                REQUIRE(scheduler->RunFrame(lifecycle, token, false).HasValue());
            }

            void CommitScene() {
                REQUIRE(scenes
                            .OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges,
                                     Runtime::FrameContext{1, {}, 0, 0, {}, false, token})
                            .HasValue());
            }

            void Drain(const Prefab::PrefabOperation &operation) {
                for (std::size_t attempt = 0; attempt < 2000 && operation.State() == Prefab::PrefabSpawnState::Pending; ++attempt) {
                    REQUIRE(service->Advance(tick).HasValue());
                    CommitScene();
                    REQUIRE(service->Advance(tick).HasValue());
                    if (operation.State() == Prefab::PrefabSpawnState::Pending)
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                INFO((operation.Failure() ? operation.Failure()->code.Value() + ": " + operation.Failure()->message : "success"));
                REQUIRE(operation.State() == Prefab::PrefabSpawnState::Committed);
            }

            Prefab::PrefabInstance Spawn() {
                auto operation = Take(
                    context->Spawn({.source = {host.prefabId, templateHash, host.request.assets.target}, .notBeforeTick = tick}, token));
                Drain(operation);
                return Take(operation.Spawned());
            }

            void Tick() {
                clock.Advance(Duration::FromNanoseconds(10));
                REQUIRE(scheduler->RunFrame(lifecycle, token, false).HasValue());
                ++tick;
            }

            float PoseX(Runtime::EntityRef entity) const {
                const auto observation = Take(forward->adapter->Query(entity));
                REQUIRE(observation.component);
                std::array<Math::Transform, 1> pose;
                REQUIRE(forward->adapter->CopyPose(entity, observation.component->currentPose, pose).HasValue());
                return pose[0].translation.x;
            }
        };
    }  // namespace

    TEST_CASE("Actual prefab host cooks native Animation and true Graph Clip Skeleton resource closure with strict cache hits",
              "[animation][prefab-cook][native][closure]") {
        HostFixture fixture;
        Animation::Test::Instance::Prepared source;
        Configure(fixture, source);
        const auto first = Take(fixture.Cook());
        CHECK(first.cacheHits == 0);
        const auto cooked = ReadTemplate(fixture, first.generation);
        REQUIRE(cooked.Data().dependencies.size() == 3);
        CHECK(std::ranges::find(cooked.Data().dependencies, TestMeshRecord().id, [](const auto &value) {
            return value.asset.id;
        }) == cooked.Data().dependencies.end());
        CHECK(
            std::ranges::find(cooked.Data().dependencies, source.compileClips.front().Data().descriptor.id.Asset(), [](const auto &value) {
            return value.asset.id;
        }) != cooked.Data().dependencies.end());
        REQUIRE(cooked.Data().entities.size() == 1);
        REQUIRE(cooked.Data().entities.front().members.size() == 1);
        Runtime::RuntimeComponentSet projected;
        std::optional<Runtime::PrimitiveMeshDescriptor> primitive;
        REQUIRE(Take(SceneSource::ProjectPrefabBuiltinComponent(std::get<Prefab::RawComponentPayload>(
                                                                    cooked.Data().entities.front().members.front()),
                                                                projected, primitive)));
        CHECK(projected.animation == Binding(source));
        const auto hit = Take(fixture.Cook());
        CHECK(hit.cacheHits == 6);
        CHECK(hit.generation.manifestDigest == first.generation.manifestDigest);
        CHECK(std::ranges::equal(ReadTemplate(fixture, hit.generation).Bytes(), cooked.Bytes()));
    }

    TEST_CASE("Actual cook rejects duplicate malformed builtin and custom missing-schema without changing published generation",
              "[animation][prefab-cook][native][schema]") {
        HostFixture fixture;
        Animation::Test::Instance::Prepared source;
        Configure(fixture, source);
        const auto previous = Take(fixture.Cook());
        auto native = NativeComponent(Binding(source));
        std::vector<Prefab::RawComponentPayload> components{native};
        SECTION("duplicate native occurrence") {
            native.instance = Prefab::PrefabComponentInstanceId::Create(2).Value();
            components.push_back(native);
        }
        SECTION("malformed native body") {
            components.front().component.payload = {std::byte{'{'}, std::byte{'}'}};
        }
        SECTION("custom opaque occurrence still needs its real owner") {
            components.front().component.typeId = Take(Gameplay::ComponentTypeId::Parse("game.test.custom"));
        }
        (void)WriteRoot(fixture, source, std::move(components));
        CHECK(fixture.Cook().HasError());
        fixture.AssertRetained(previous.generation);
    }
}  // namespace Horo::Application

namespace Horo::Application {
    TEST_CASE("Actual cooked animated prefabs spawn tick independently and despawn through canonical structural ownership",
              "[animation][prefab-cook][native][spawn][lifecycle]") {
        HostFixture fixture;
        Animation::Test::Instance::Prepared source;
        Configure(fixture, source);
        const auto report = Take(fixture.Cook());
        AnimatedPrefabWorld world(fixture, report.generation);
        REQUIRE_FALSE(world.scenes.ActiveScene()->FindAsset(source.program->Id().Asset()));
        const auto first = world.Spawn();
        world.Tick();
        CHECK(world.PoseX(first.Root()) == 10.0F);
        const auto second = world.Spawn();
        world.Tick();
        CHECK(world.PoseX(first.Root()) == 20.0F);
        CHECK(world.PoseX(second.Root()) == 10.0F);
        auto retired = Take(world.context->Despawn(first, world.tick, world.token));
        world.Drain(retired);
        world.Tick();
        CHECK(world.forward->adapter->Query(first.Root()).HasError());
        CHECK(world.PoseX(second.Root()) == 20.0F);
        CHECK_FALSE(world.scenes.ActiveScene()->FindAsset(source.program->Id().Asset()));
        auto final = Take(world.context->Despawn(second, world.tick, world.token));
        world.Drain(final);
        CHECK(world.forward->adapter->Query(second.Root()).HasError());
    }
}  // namespace Horo::Application

namespace Horo::Application {
    TEST_CASE("Actual animated template rejects unrelated emitted envelopes outside its projected resource closure",
              "[animation][prefab-cook][closure][integrity]") {
        HostFixture fixture;
        Animation::Test::Instance::Prepared source;
        Configure(fixture, source);
        const auto document = WriteRoot(fixture, source, {NativeComponent(Binding(source))});
        const auto report = Take(fixture.Cook());
        const auto cooked = ReadTemplate(fixture, report.generation);
        const auto limits = Take(Prefab::PrefabLimitProfile::Create({}));
        const auto bytes = Take(document.SerializeCanonical());
        const auto sources =
            Take(Prefab::BuildPrefabSourceResolverSnapshot(fixture.registry.Snapshot(),
                                                           {{document, {fixture.version, ComputeSha256(std::as_bytes(std::span{bytes}))}}},
                                                           limits));
        const auto contents = Take(ReadCookGenerationContents(fixture.files, report.generation, 16U * 1024U * 1024U));
        std::vector<Assets::AssetDependency> projected;
        std::vector<Prefab::PrefabTemplateCookResource> resources;
        for (const auto &dependency : cooked.Data().dependencies) {
            projected.push_back(dependency.asset);  // Exact actual host-projected roles admitted by the positive cook above.
            const auto found = std::ranges::find(contents.entries, dependency.asset.id, &Assets::AssetCookManifestEntry::assetId);
            REQUIRE(found != contents.entries.end());
            resources.push_back({dependency.asset.id, contents.artifacts[static_cast<std::size_t>(found - contents.entries.begin())]});
        }
        const Prefab::PrefabTemplateCookOptions options{.projectedResourceDependencies = projected};
        REQUIRE(Prefab::CookPrefabTemplate(sources, fixture.registry.Snapshot(), fixture.prefabId, resources, fixture.request.assets.target,
                                           limits, options)
                    .HasValue());
        const auto unrelated = std::ranges::find(contents.entries, TestMeshRecord().id, &Assets::AssetCookManifestEntry::assetId);
        REQUIRE(unrelated != contents.entries.end());
        resources.push_back({TestMeshRecord().id, contents.artifacts[static_cast<std::size_t>(unrelated - contents.entries.begin())]});
        const auto rejected = Prefab::CookPrefabTemplate(sources, fixture.registry.Snapshot(), fixture.prefabId, resources,
                                                         fixture.request.assets.target, limits, options);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == Prefab::PrefabErrors::DependencyGraphInvalid.code.Value());
        fixture.AssertRetained(report.generation);
    }
}  // namespace Horo::Application
