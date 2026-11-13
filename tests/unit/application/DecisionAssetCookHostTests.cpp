#include "DecisionGraphCompilerTestSupport.h"
#include "Horo/Application/DecisionAssetCookHost.h"
#include "assets/AssetCookServiceFixture.h"

namespace {
    using namespace Horo;
    using namespace Horo::AI;
    using namespace Horo::AI::DecisionGraphTestSupport;
    using namespace Horo::Application;
    using namespace Horo::Assets;
    namespace Files = Horo::Assets::ServiceTestSupport;

    struct Fixture final {
        DecisionGraphTestSupport::Fixture ai;
        Files::TempDir source, cache, cooked;
        JobSystem jobs;
        AssetRegistry registry;
        DecisionAssetCookRequest request;
        AssetId root{Files::Id("00000000-0000-0000-0000-0000000000a1")};
        AssetId child{Files::Id("00000000-0000-0000-0000-0000000000b1")};
        std::vector<AssetRecord> records;

        Fixture() {
            std::filesystem::create_directories(source.path / "assets");
            Add(root, "root.horo_sm", DecisionStateMachineSourceType, CallingMachine(1, 2, DecisionPlanKind::BehaviorTree));
            Add(child, "task.horo_bt", DecisionBehaviorTreeSourceType, TaskTree(2));
            REQUIRE(registry.Publish(records).status == AssetRegistryBuildStatus::Complete);
            request.assets.sourceRoot = source.path;
            request.assets.cacheFiles = Horo::TestSupport::NativeCacheFiles(cache.path);
            request.assets.cookedRoot = cooked.path;
            request.assets.registry = registry.Snapshot();
            request.assets.target = Files::Target("headless-null");
            request.assets.limits.maximumConcurrentCooks = 1;
            CookPublicationTestSupport::ConfigureNativeCookPublication(request.assets);
            request.environment = ai.Environment();
            const auto capturedSchema = ai.schema;
            const auto capturedProviders = ai.providers;
            request.assets.validateHostInputs = [this, capturedSchema, capturedProviders]() -> Result<void> {
                if (ai.schema != capturedSchema || ai.providers != capturedProviders || !capturedProviders->CanUse())
                    return Result<void>::Failure(MakeError(DecisionCookErrors::Stale));
                return Result<void>::Success();
            };
            Capture();
        }

        void Add(AssetId id, std::string_view name, std::string_view type, const DecisionGraphSource &graph) {
            const std::string relative = "assets/" + std::string{name};
            WriteGraph(relative, graph);
            const auto sidecar = Files::SidecarJson(id.ToString(), type);
            Files::WriteFile(source.path / (relative + ".horo"), {reinterpret_cast<const std::uint8_t *>(sidecar.data()), sidecar.size()});
            records.push_back(
                {id, Files::Type(type), ProjectPath::Parse(relative).Value(), ProjectPath::Parse(relative + ".horo").Value()});
        }

        void WriteGraph(const std::string &relative, const DecisionGraphSource &graph) {
            auto encoded = DecisionPlanCodec::EncodeSource(std::span{&graph, 1}, graph.asset);
            REQUIRE(encoded.HasValue());
            Files::WriteFile(source.path / relative, encoded.Value());
        }

        void Capture() {
            auto captured =
                AssetCookInputSnapshot::Capture(source.path, request.assets.registry, request.assets.limits, 16U * 1024U * 1024U);
            REQUIRE(captured.HasValue());
            request.assets.pinnedInputs = std::make_shared<const AssetCookInputSnapshot>(std::move(captured).Value());
        }

        Result<AssetCookReport> Cook(CancellationToken cancellation = {}) {
            return DecisionAssetCookHost::Cook(jobs, request, cancellation);
        }

        AssetCookGeneration Current() const {
            auto current = ResolveCurrentCookGeneration(NativeDurableFileSystem{}, cooked.path);
            REQUIRE(current.HasValue());
            return current.Value();
        }

        std::vector<AssetCookArtifact> Read() const {
            auto contents = ReadCookGenerationContents(NativeDurableFileSystem{}, Current(), 16U * 1024U * 1024U);
            REQUIRE(contents.HasValue());
            std::vector<AssetCookArtifact> artifacts;
            for (const auto &bytes : contents.Value().artifacts) {
                auto artifact = DecodeCookedArtifact(bytes);
                REQUIRE(artifact.HasValue());
                artifacts.push_back(std::move(artifact).Value());
            }
            return artifacts;
        }
    };

    TEST_CASE("Actual decision source pairs cook through AssetCookService and load into the real composed headless runner",
              "[native][decision-cook]") {
        Fixture fixture;
        auto first = fixture.Cook();
        REQUIRE(first.HasValue());
        CHECK(first.Value().cookedAssets == 2);
        CHECK(fixture.ai.probe.factories == 0);
        const auto artifacts = fixture.Read();
        REQUIRE(artifacts.size() == 2);
        const auto root = std::ranges::find(artifacts, fixture.root, &AssetCookArtifact::id);
        REQUIRE(root != artifacts.end());
        CHECK(root->type.Value() == DecisionStateMachineSourceType);
        auto loaded = DecisionPlanCodec::DecodeCooked(root->payload, fixture.ai.Environment());
        REQUIRE(loaded.HasValue());
        REQUIRE(loaded.Value().plan);
        CHECK(loaded.Value().plan->Root() == Graph(1));
        CHECK(loaded.Value().plan->Graphs().size() == 2);
        auto instance = fixture.ai.Instance(loaded.Value().plan);
        CHECK(fixture.ai.Step(*instance, 1) == AiTaskState::Running);
        CHECK(fixture.ai.Step(*instance, 2) == AiTaskState::Succeeded);
        auto reused = fixture.Cook();
        REQUIRE(reused.HasValue());
        CHECK(reused.Value().cacheHits == 2);
        CHECK(reused.Value().cookedAssets == 0);
    }

    TEST_CASE("Invalid missing or future decision inputs preserve the actual prior published generation", "[native][decision-cook]") {
        Fixture fixture;
        REQUIRE(fixture.Cook().HasValue());
        const auto previous = fixture.Current();
        SECTION("missing actual dependency") {
            fixture.WriteGraph("assets/root.horo_sm", CallingMachine(1, 999, DecisionPlanKind::BehaviorTree));
            fixture.Capture();
        }
        SECTION("future actual source wire version") {
            auto encoded = DecisionPlanCodec::EncodeSource(std::array{TaskTree(2)}, Graph(2));
            REQUIRE(encoded.HasValue());
            auto future = encoded.Value();
            future[8] = 2;
            Files::WriteFile(fixture.source.path / "assets/task.horo_bt", future);
            fixture.Capture();
        }
        SECTION("bounded aggregate prepared output") {
            fixture.request.maximumPreparedBytes = 1;
        }
        CHECK(fixture.Cook().HasError());
        CHECK(fixture.Current().manifestDigest == previous.manifestDigest);
        CHECK(fixture.Current().generationRoot == previous.generationRoot);
        CHECK(fixture.ai.probe.factories == 0);
    }

    TEST_CASE("Decision provider revocation at the real publication fence cannot replace its prior generation", "[native][decision-cook]") {
        Fixture fixture;
        REQUIRE(fixture.Cook().HasValue());
        const auto previous = fixture.Current();
        const auto original = fixture.request.assets.validateHostInputs;
        std::size_t observations{};
        fixture.request.assets.validateHostInputs = [&]() -> Result<void> {
            if (++observations == 3)
                fixture.ai.admission->store(false, std::memory_order_release);
            return original();
        };
        CHECK(fixture.Cook().HasError());
        CHECK(observations >= 3);
        CHECK(fixture.Current().manifestDigest == previous.manifestDigest);
        CHECK(fixture.Current().generationRoot == previous.generationRoot);
    }

    TEST_CASE("Cancellation before actual decision current pointer replacement retains the complete prior generation",
              "[native][decision-cook]") {
        Fixture fixture;
        REQUIRE(fixture.Cook().HasValue());
        const auto previous = fixture.Current();
        CancellationSource cancellation;
        auto files = std::make_shared<Files::CookPublicationFiles>();
        files->cancellation = &cancellation;
        fixture.request.assets.publicationFiles = files;
        CHECK(fixture.Cook(cancellation.Token()).HasError());
        CHECK_FALSE(files->committed);
        CHECK(fixture.Current().manifestDigest == previous.manifestDigest);
        CHECK(fixture.Current().generationRoot == previous.generationRoot);
    }

    TEST_CASE("Decision cook detects real file drift and missing host authority before any output publication", "[native][decision-cook]") {
        Fixture fixture;
        REQUIRE(fixture.Cook().HasValue());
        const auto previous = fixture.Current();
        SECTION("source bytes changed after capture") {
            fixture.WriteGraph("assets/task.horo_bt", TaskTree(9));
        }
        SECTION("schema publication changed") {
            const std::array keys{BlackboardKeyDescriptor{.key = MakeIdentity<BlackboardKeyId>(2)}};
            auto schema = BlackboardSchema::Capture({SchemaId(), 2, BlackboardUnknownValuePolicy::Reject, keys});
            REQUIRE(schema.HasValue());
            fixture.ai.schema = std::make_shared<const BlackboardSchema>(std::move(schema).Value());
        }
        SECTION("required actual host fence absent") {
            fixture.request.assets.validateHostInputs = {};
        }
        CHECK(fixture.Cook().HasError());
        CHECK(fixture.Current().manifestDigest == previous.manifestDigest);
        CHECK(fixture.Current().generationRoot == previous.generationRoot);
    }
}  // namespace
