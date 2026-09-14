#include "Horo/Application/ProjectMigrationCatalog.h"
#include "ProjectMigrationProductionTestSupport.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>

namespace project_migration_tests {
    const Horo::ErrorCodeDescriptor TestFailure{
        .domain = Horo::ErrorDomainId("test.project_migration"),
        .code = Horo::ErrorCode("test.project_migration.failed"),
        .defaultSeverity = Horo::ErrorSeverity::Error,
        .summary = "Migration fixture failed.",
        .remediationHint = "Inspect the fixture.",
    };

    [[nodiscard]] Horo::Application::ContractBaselineVersion Version(const char *text) {
        const auto parsed = Horo::Application::ParseHoroVersion(text);
        REQUIRE((parsed.HasValue()));
        return Horo::Application::ContractBaselineVersion{parsed.Value()};
    }

    [[nodiscard]] Horo::Application::PersistentContractHash Contract(const std::uint8_t marker) {
        Horo::Application::PersistentContractHash hash;
        hash.bytes.fill(marker);
        return hash;
    }

    [[nodiscard]] std::vector<std::byte> Bytes(const std::string &text) {
        std::vector<std::byte> bytes(text.size());
        std::ranges::transform(text, bytes.begin(), [](const char value) {
            return static_cast<std::byte>(value);
        });
        return bytes;
    }

    [[nodiscard]] std::string Text(const std::span<const std::byte> bytes) {
        std::string text(bytes.size(), '\0');
        std::ranges::transform(bytes, text.begin(), [](const std::byte value) {
            return static_cast<char>(std::to_integer<unsigned char>(value));
        });
        return text;
    }

    class AppendStage final : public Horo::Application::IProjectMigrationDocumentStage {
    public:
        AppendStage(std::string id, std::string suffix) : id_(std::move(id)), suffix_(std::move(suffix)) {}

        [[nodiscard]] Horo::Application::MigrationStageDescriptor Describe() const override {
            return {.id = {id_}, .readFamilies = {"text"}, .writeFamilies = {"text"}};
        }

        [[nodiscard]] Horo::Result<Horo::Application::MigrationDocumentChange> Execute(
            const Horo::Application::ProjectDocumentView &source, const Horo::Application::MigrationStageContext &,
            const Horo::CancellationToken &cancellation) const override {
            if (cancellation.IsCancellationRequested())
                return Horo::Result<Horo::Application::MigrationDocumentChange>::Failure(Horo::MakeError(TestFailure, "cancelled"));
            std::string output = Text(source.bytes) + suffix_;
            return Horo::Result<Horo::Application::MigrationDocumentChange>::Success(
                {.document = source.handle, .replacement = Bytes(output)});
        }

    private:
        std::string id_;
        std::string suffix_;
    };

    class ObserveMergedStage final : public Horo::Application::IProjectMigrationStage {
    public:
        ObserveMergedStage(std::string id, std::string expected, bool *observed)
            : id_(std::move(id)), expected_(std::move(expected)), observed_(observed) {}

        [[nodiscard]] Horo::Application::MigrationStageDescriptor Describe() const override {
            return {.id = {id_}, .readFamilies = {"text"}};
        }

        [[nodiscard]] Horo::Result<void> Execute(Horo::Application::ProjectMigrationContext &context,
                                                 const Horo::CancellationToken &) const override {
            for (const auto &entry :
                 context.ListDocuments(Horo::Application::MigrationDocumentQuery::Kind(Horo::Application::MigrationDocumentKind::Other))) {
                const auto document = context.ReadDocument(entry.handle);
                if (document.HasError() || !Text(document.Value().bytes).ends_with(expected_))
                    return Horo::Result<void>::Failure(Horo::MakeError(TestFailure, "merged output missing"));
            }
            *observed_ = true;
            return Horo::Result<void>::Success();
        }

    private:
        std::string id_;
        std::string expected_;
        bool *observed_;
    };

    class SuffixValidator final : public Horo::Application::IProjectMigrationValidator {
    public:
        SuffixValidator(std::string id, std::string expected, bool *validated = nullptr)
            : id_(std::move(id)), expected_(std::move(expected)), validated_(validated) {}

        [[nodiscard]] Horo::Application::MigrationStageDescriptor Describe() const override {
            return {.id = {id_}, .readFamilies = {"text"}};
        }

        [[nodiscard]] Horo::Result<void> Validate(const Horo::Application::ProjectMigrationContext &context,
                                                  const Horo::CancellationToken &) const override {
            for (const auto &entry :
                 context.ListDocuments(Horo::Application::MigrationDocumentQuery::Kind(Horo::Application::MigrationDocumentKind::Other))) {
                const auto document = context.ReadDocument(entry.handle);
                if (document.HasError() || !Text(document.Value().bytes).ends_with(expected_))
                    return Horo::Result<void>::Failure(Horo::MakeError(TestFailure, "validation failed"));
            }
            if (validated_)
                *validated_ = true;
            return Horo::Result<void>::Success();
        }

    private:
        std::string id_;
        std::string expected_;
        bool *validated_;
    };

    [[nodiscard]] std::shared_ptr<const Horo::Application::ProjectMigrationPipeline> Pipeline(const std::string &id,
                                                                                              const std::string &append,
                                                                                              const std::string &expected,
                                                                                              bool *observed = nullptr,
                                                                                              bool *validated = nullptr) {
        auto builder = Horo::Application::ProjectMigrationPipelineBuilder::Begin({id});
        static_cast<void>(
            builder.AddForEach(Horo::Application::MigrationDocumentQuery::Kind(Horo::Application::MigrationDocumentKind::Other),
                               std::make_shared<AppendStage>(id + ".append", append)));
        if (observed)
            static_cast<void>(builder.AddThen(std::make_shared<ObserveMergedStage>(id + ".observe", expected, observed)));
        static_cast<void>(builder.AddValidator(std::make_shared<SuffixValidator>(id + ".validate", expected, validated)));
        auto built = std::move(builder).Build();
        REQUIRE((built.HasValue()));
        return built.Value();
    }

    [[nodiscard]] Horo::Application::ProjectMigrationDefinition Definition(
        const std::string &id, const Horo::Application::ProjectMigrationDefinitionKind kind, const char *from, const char *to,
        const std::uint8_t sourceContract, const std::uint8_t targetContract,
        std::shared_ptr<const Horo::Application::ProjectMigrationPipeline> pipeline) {
        return {.id = {id},
                .kind = kind,
                .from = Version(from),
                .to = Version(to),
                .sourceContract = Contract(sourceContract),
                .targetContract = Contract(targetContract),
                .pipeline = std::move(pipeline)};
    }

    [[nodiscard]] Horo::Application::ProjectMigrationPlan ProductionCompressionPlan() {
        auto definition = ProductionCompressionDefinition();
        return {.source = definition.from, .target = definition.to, .definitions = {std::move(definition)}};
    }

    class TemporaryProject {
    public:
        TemporaryProject() {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            root = std::filesystem::temp_directory_path() / ("horo-migration-test-" + std::to_string(stamp) + "-" +
                                                             std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
            std::error_code error;
            REQUIRE((std::filesystem::create_directories(root / ".horo/local", error)));
            REQUIRE_FALSE((error));
            REQUIRE((std::filesystem::create_directories(root / "assets", error)));
            REQUIRE_FALSE((error));
            Write(".horo/project.json", R"({"horoVersion":"0.0.1"})");
            Write(".horo/local/ignored.txt", "ignored");
            Write("assets/a.txt", "alpha");
            Write("assets/b.txt", "beta");
        }

        TemporaryProject(const TemporaryProject &) = delete;
        TemporaryProject &operator=(const TemporaryProject &) = delete;
        TemporaryProject(TemporaryProject &&) = delete;
        TemporaryProject &operator=(TemporaryProject &&) = delete;

        ~TemporaryProject() {
            std::error_code ignored;
            std::filesystem::remove_all(root, ignored);
        }

        void Write(const std::filesystem::path &relative, const std::string &value) const {
            std::error_code error;
            std::filesystem::create_directories((root / relative).parent_path(), error);
            REQUIRE_FALSE((error));
            std::ofstream stream(root / relative, std::ios::binary | std::ios::trunc);
            REQUIRE((stream.good()));
            stream << value;
            REQUIRE((stream.good()));
        }

        [[nodiscard]] std::string Read(const std::filesystem::path &relative) const {
            std::ifstream stream(root / relative, std::ios::binary);
            return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        }

        std::filesystem::path root;
        inline static std::atomic<std::uint64_t> sequence{};
    };

    class ProductionMigrationFixture {
    public:
        ProductionMigrationFixture() : plan(ProductionCompressionPlan()), jobs({.workerCount = 2, .maxQueuedJobs = 16}) {}

        ProductionMigrationFixture(const ProductionMigrationFixture &) = delete;
        ProductionMigrationFixture &operator=(const ProductionMigrationFixture &) = delete;
        ProductionMigrationFixture(ProductionMigrationFixture &&) = delete;
        ProductionMigrationFixture &operator=(ProductionMigrationFixture &&) = delete;

        ~ProductionMigrationFixture() {
            jobs.Shutdown(Horo::ShutdownPolicy::Drain);
        }

        [[nodiscard]] Horo::Result<Horo::Application::PreparedProjectMigration> Prepare(const TemporaryProject &project) {
            return Horo::Application::ProjectMigrationExecutor::Prepare(project.root,
                                                                        project.root.parent_path() /
                                                                            (project.root.filename().string() + "-candidate"),
                                                                        plan, jobs);
        }

        Horo::Application::ProjectMigrationPlan plan;
        Horo::JobSystem jobs;
    };

    TEST_CASE("Pipeline Requires Terminal Validation", "[unit][application]") {
        auto builder = Horo::Application::ProjectMigrationPipelineBuilder::Begin({"test.invalid"});
        static_cast<void>(
            builder.AddForEach(Horo::Application::MigrationDocumentQuery::Any(), std::make_shared<AppendStage>("test.append", "A")));
        const auto built = std::move(builder).Build();
        REQUIRE((built.HasError()));
        REQUIRE((built.ErrorValue().code.Value() == "project.migration.pipeline_invalid"));
    }

    TEST_CASE("Registry Plans Sequential And Declared Checkpoint Paths", "[unit][application]") {
        const auto first = Definition("test.c1_c2", Horo::Application::ProjectMigrationDefinitionKind::Sequential, "0.0.1", "0.1.0", 1, 2,
                                      Pipeline("test.c1_c2", "A", "A"));
        const auto second = Definition("test.c2_c3", Horo::Application::ProjectMigrationDefinitionKind::Sequential, "0.1.0", "0.2.0", 2, 3,
                                       Pipeline("test.c2_c3", "B", "B"));
        const auto checkpoint = Definition("test.c1_c3", Horo::Application::ProjectMigrationDefinitionKind::Checkpoint, "0.0.1", "0.2.0", 1,
                                           3, Pipeline("test.c1_c3", "AB", "AB"));
        const std::vector definitions{first, second, checkpoint};
        const auto registry = Horo::Application::ProjectMigrationRegistry::Create(definitions);
        REQUIRE((registry.HasValue()));
        const std::vector sequentialDefinitions{first, second};
        const auto sequentialRegistry = Horo::Application::ProjectMigrationRegistry::Create(sequentialDefinitions);
        REQUIRE((sequentialRegistry.HasValue()));

        Horo::Application::ProjectMigrationSupportDescriptor support{.target = Version("0.2.0"),
                                                                     .minimumMigratable = Version("0.0.1"),
                                                                     .targetContract = Contract(3),
                                                                     .targetValidator =
                                                                         std::make_shared<SuffixValidator>("test.target", "AB")};
        const auto sequential = sequentialRegistry.Value().Plan(Version("0.0.1"), Contract(1), support);
        REQUIRE((sequential.HasValue()));
        REQUIRE((sequential.Value().definitions.size() == 2));
        REQUIRE((sequential.Value().definitions[0].id.value == "test.c1_c2"));

        const auto undeclared = registry.Value().Plan(Version("0.0.1"), Contract(1), support);
        REQUIRE((undeclared.HasError()));
        REQUIRE((undeclared.ErrorValue().code.Value() == "project.migration.catalog_invalid"));
        support.checkpoints.push_back({.id = {"test.c1_c3"}, .source = Version("0.0.1"), .target = Version("0.2.0")});
        const auto direct = registry.Value().Plan(Version("0.0.1"), Contract(1), support);
        REQUIRE((direct.HasValue()));
        REQUIRE((direct.Value().definitions.size() == 1));
        REQUIRE((direct.Value().definitions[0].kind == Horo::Application::ProjectMigrationDefinitionKind::Checkpoint));
    }

    TEST_CASE("Planner Rejects Ambiguous Sequential Next Hop And Missing Provider", "[unit][application]") {
        auto first = Definition("test.first", Horo::Application::ProjectMigrationDefinitionKind::Sequential, "0.0.1", "0.1.0", 1, 2,
                                Pipeline("test.first", "A", "A"));
        auto alternative = Definition("test.alternative", Horo::Application::ProjectMigrationDefinitionKind::Sequential, "0.0.1", "0.1.1",
                                      1, 4, Pipeline("test.alternative", "X", "X"));
        auto terminal = Definition("test.final", Horo::Application::ProjectMigrationDefinitionKind::Sequential, "0.1.0", "0.2.0", 2, 3,
                                   Pipeline("test.final", "B", "B"));
        const std::vector definitions{first, alternative, terminal};
        const auto registry = Horo::Application::ProjectMigrationRegistry::Create(definitions);
        REQUIRE((registry.HasValue()));
        Horo::Application::ProjectMigrationSupportDescriptor support{.target = Version("0.2.0"),
                                                                     .minimumMigratable = Version("0.0.1"),
                                                                     .targetContract = Contract(3),
                                                                     .targetValidator =
                                                                         std::make_shared<SuffixValidator>("test.target", "AB")};
        const auto ambiguous = registry.Value().Plan(Version("0.0.1"), Contract(1), support);
        REQUIRE((ambiguous.HasError()));
        REQUIRE((ambiguous.ErrorValue().code.Value() == "project.migration.ambiguous"));

        first.requiredProviders.emplace_back("plugin.test");
        const std::vector providerDefinitions{first, terminal};
        const auto providerRegistry = Horo::Application::ProjectMigrationRegistry::Create(providerDefinitions);
        REQUIRE((providerRegistry.HasValue()));
        const auto unavailable = providerRegistry.Value().Plan(Version("0.0.1"), Contract(1), support);
        REQUIRE((unavailable.HasError()));
        REQUIRE((unavailable.ErrorValue().code.Value() == "project.migration.provider_missing"));
    }

    TEST_CASE("Verified Dry Run Uses Merged Hop State And Preserves Authoritative Tree", "[unit][application]") {
        bool observedMerged = false;
        bool firstValidated = false;
        bool secondValidated = false;
        bool targetValidated = false;
        const auto first = Definition("test.first", Horo::Application::ProjectMigrationDefinitionKind::Sequential, "0.0.1", "0.1.0", 1, 2,
                                      Pipeline("test.first", "A", "A", &observedMerged, &firstValidated));
        const auto second = Definition("test.second", Horo::Application::ProjectMigrationDefinitionKind::Sequential, "0.1.0", "0.2.0", 2, 3,
                                       Pipeline("test.second", "B", "AB", nullptr, &secondValidated));
        Horo::Application::ProjectMigrationPlan plan{.source = Version("0.0.1"),
                                                     .target = Version("0.2.0"),
                                                     .definitions = {first, second},
                                                     .targetValidator =
                                                         std::make_shared<SuffixValidator>("test.target.validate", "AB", &targetValidated)};
        TemporaryProject project;
        const std::string beforeA = project.Read("assets/a.txt");
        const std::string beforeB = project.Read("assets/b.txt");
        const std::string ignored = project.Read(".horo/local/ignored.txt");
        Horo::JobSystem jobs({.workerCount = 2, .maxQueuedJobs = 8});
        const auto result = Horo::Application::ProjectMigrationExecutor::VerifiedDryRun(project.root, plan, jobs, {.maxConcurrency = 2});
        REQUIRE((result.HasValue()));
        REQUIRE((observedMerged));
        REQUIRE((firstValidated));
        REQUIRE((secondValidated));
        REQUIRE((targetValidated));
        REQUIRE((result.Value().changedFiles == std::vector<std::string>{"assets/a.txt", "assets/b.txt"}));
        REQUIRE((project.Read("assets/a.txt") == beforeA));
        REQUIRE((project.Read("assets/b.txt") == beforeB));
        REQUIRE((project.Read(".horo/local/ignored.txt") == ignored));
        for (const auto &entry : std::filesystem::directory_iterator(project.root))
            REQUIRE((!entry.path().filename().string().starts_with(".horo-migration-dry-run-")));
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Verified Dry Run Honors Parent Cancellation Without Mutation", "[unit][application]") {
        const auto definition = Definition("test.cancel", Horo::Application::ProjectMigrationDefinitionKind::Sequential, "0.0.1", "0.1.0",
                                           1, 2, Pipeline("test.cancel", "A", "A"));
        Horo::Application::ProjectMigrationPlan plan{.source = Version("0.0.1"), .target = Version("0.1.0"), .definitions = {definition}};
        TemporaryProject project;
        const std::string before = project.Read("assets/a.txt");
        Horo::CancellationSource cancellation;
        cancellation.RequestCancellation();
        Horo::JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 4});
        const auto result = Horo::Application::ProjectMigrationExecutor::VerifiedDryRun(project.root, plan, jobs, {}, cancellation.Token());
        REQUIRE((result.HasError()));
        REQUIRE((result.ErrorValue().code.Value() == "project.migration.cancelled"));
        REQUIRE((project.Read("assets/a.txt") == before));
        jobs.Shutdown(Horo::ShutdownPolicy::Drain);
    }

    TEST_CASE("Production Compression Migration Preserves Unknown Json Semantics", "[unit][application]") {
        ProductionMigrationFixture fixture;
        TemporaryProject project;
        project.Write(
            ".horo/project.json",
            R"({"horoVersion":"0.0.1","persistentContract":"sha256:5ef87e96e24c0a3a5e44f4dee182dbd3bfb5402e08e07aaf3d64d4a3ff24ae6d","projectId":"migration-fixture","name":"Fixture","projectVersion":"0.2.0","createdAt":"2026-07-19T00:00:00Z","settings":{"renderBackend":"opengl","unknown":{"array":[1,true,null],"label":"kept"}},"unknownRoot":{"enabled":true}})");

        auto prepared = fixture.Prepare(project);
        REQUIRE((prepared.HasValue()));
        const auto root = prepared.Value().ReadCandidateDocument(".horo/project.json");
        REQUIRE((root.HasValue()));
        const std::string migrated = Text(root.Value());
        REQUIRE((migrated.find("\"assetCompression\": \"lz4\"") != std::string::npos));
        REQUIRE((migrated.find("\"textureCompression\": \"bc7\"") != std::string::npos));
        REQUIRE((migrated.find("\"unknownRoot\"") != std::string::npos));
        REQUIRE((migrated.find("\"array\"") != std::string::npos));
        REQUIRE((project.Read(".horo/project.json").find("assetCompression") == std::string::npos));
    }

    TEST_CASE("Production Compression Migration Accepts Supported Values", "[unit][application]") {
        ProductionMigrationFixture fixture;

        constexpr std::array assetValues{"lz4", "none", "zstd"};
        constexpr std::array textureValues{"bc7", "bc5", "astc", "none"};
        for (const char *asset : assetValues)
            for (const char *texture : textureValues) {
                TemporaryProject project;
                project.Write(".horo/project.json",
                              std::format(R"({{"projectId":"supported","settings":{{"assetCompression":"{}","textureCompression":"{}"}}}})"
                                          "\n",
                                          asset, texture));
                auto prepared = fixture.Prepare(project);
                REQUIRE((prepared.HasValue()));
                const auto root = prepared.Value().ReadCandidateDocument(".horo/project.json");
                REQUIRE((root.HasValue()));
                const std::string migrated = Text(root.Value());
                REQUIRE((migrated.find(std::string{"\"assetCompression\": \""} + asset + "\"") != std::string::npos));
                REQUIRE((migrated.find(std::string{"\"textureCompression\": \""} + texture + "\"") != std::string::npos));
            }
    }

    TEST_CASE("Production Compression Migration Rejects Invalid Values Without Mutation", "[unit][application]") {
        ProductionMigrationFixture fixture;
        constexpr std::array
            invalidSettings{R"({"projectId":"invalid","settings":{"assetCompression":"","textureCompression":"bc7"}})",
                            R"({"projectId":"invalid","settings":{"assetCompression":17,"textureCompression":"bc7"}})",
                            R"({"projectId":"invalid","settings":{"assetCompression":"brotli","textureCompression":"bc7"}})",
                            R"({"projectId":"invalid","settings":{"assetCompression":"lz4","textureCompression":""}})",
                            R"({"projectId":"invalid","settings":{"assetCompression":"lz4","textureCompression":17}})",
                            R"({"projectId":"invalid","settings":{"assetCompression":"lz4","textureCompression":"etc2"}})"};
        for (const char *invalid : invalidSettings) {
            TemporaryProject project;
            project.Write(".horo/project.json", invalid);
            const std::string authoritative = project.Read(".horo/project.json");
            auto prepared = fixture.Prepare(project);
            REQUIRE((prepared.HasError()));
            REQUIRE((prepared.ErrorValue().message.find("core.project_settings.compression_defaults") != std::string::npos));
            REQUIRE((prepared.ErrorValue().message.find("validate_compression_postconditions") != std::string::npos));
            REQUIRE((project.Read(".horo/project.json") == authoritative));
        }
    }

    TEST_CASE("Production Compression Migration Attributes Target Validation Failure", "[unit][application]") {
        const auto support = Horo::Application::BuildBuiltInProjectMigrationSupportDescriptor();
        REQUIRE((support.HasValue()));
        ProductionMigrationFixture fixture;
        TemporaryProject finalProject;
        finalProject.Write(".horo/project.json",
                           R"({"projectId":"target-attribution","settings":{"assetCompression":"lz4","textureCompression":"bc7"}})"
                           "\n");
        auto prepared = fixture.Prepare(finalProject);
        REQUIRE((prepared.HasValue()));
        auto candidate = std::move(prepared).Value();
        const auto finalValidation = Horo::Application::ProjectMigrationExecutor::Finalize(candidate, {}, support.Value().targetValidator);
        REQUIRE((finalValidation.HasError()));
        REQUIRE((finalValidation.ErrorValue().message.find("horo.project.target_contract") != std::string::npos));
        REQUIRE((finalValidation.ErrorValue().message.find("validate_0_1_0_target_contract") != std::string::npos));
    }

    TEST_CASE("Production Compression Verified Dry Run Preserves Authoritative Bytes", "[unit][application]") {
        ProductionMigrationFixture fixture;
        TemporaryProject project;
        project.Write(".horo/project.json", R"({"projectId":"dry-run","settings":{"renderBackend":"opengl"},"unknown":[1,true,null]})"
                                            "\n");
        const std::string before = project.Read(".horo/project.json");
        // Transaction-owned root/history overlay is intentionally absent from this definition-level dry run.
        const auto dryRun = Horo::Application::ProjectMigrationExecutor::VerifiedDryRun(project.root, fixture.plan, fixture.jobs);
        REQUIRE((dryRun.HasValue()));
        REQUIRE((dryRun.Value().changedFiles == std::vector<std::string>{".horo/project.json"}));
        REQUIRE((project.Read(".horo/project.json") == before));
    }

    TEST_CASE("Production Migration Adopts Prefab Identity Before Scene References", "[unit][application][prefab]") {
        constexpr std::string_view PrefabId = "00112233-4455-6677-8899-aabbccddeeff";
        ProductionMigrationFixture fixture;
        TemporaryProject project;
        project.Write(".horo/project.json", R"({"projectId":"prefab-adoption","settings":{}})");
        project.Write("assets/prefabs/player.prefab.horo",
                      R"({"schemaVersion":1,"assetId":"00112233-4455-6677-8899-aabbccddeeff","assetType":"core.prefab"})");
        project.Write(
            "assets/prefabs/player.prefab",
            R"({"projectVersion":"0.0.1","prefabId":"00112233-4455-6677-8899-aabbccddeeff","objects":[{"components":[{"type":"game.unknown","payload":{"bytes":[0,127,255]}}]}],"unknown":{"kept":true}})");
        project.Write(
            "assets/scenes/main.horo",
            R"({"schemaVersion":1,"objects":[],"prefabInstances":[{"instanceId":7,"sourcePath":"assets/prefabs/player.prefab","unknown":"kept"}]})");
        const std::string sourcePrefab = project.Read("assets/prefabs/player.prefab");
        const std::string sourceScene = project.Read("assets/scenes/main.horo");

        auto prepared = fixture.Prepare(project);
        REQUIRE((prepared.HasValue()));
        std::vector<std::string> changedPaths;
        std::ranges::transform(prepared.Value().Changes(), std::back_inserter(changedPaths), [](const auto &change) {
            return change.path;
        });
        REQUIRE(
            (changedPaths == std::vector<std::string>{".horo/project.json", "assets/prefabs/player.prefab", "assets/scenes/main.horo"}));
        const auto prefabBytes = prepared.Value().ReadCandidateDocument("assets/prefabs/player.prefab");
        const auto sceneBytes = prepared.Value().ReadCandidateDocument("assets/scenes/main.horo");
        REQUIRE((prefabBytes.HasValue()));
        REQUIRE((sceneBytes.HasValue()));
        const std::string prefab = Text(prefabBytes.Value());
        const std::string scene = Text(sceneBytes.Value());
        REQUIRE((prefab.find("\"projectVersion\": \"0.1.0\"") != std::string::npos));
        REQUIRE((prefab.find(std::string{"\"assetId\": \""} + std::string{PrefabId} + "\"") != std::string::npos));
        REQUIRE((prefab.find("\"prefabId\"") == std::string::npos));
        REQUIRE((prefab.find("\"bytes\"") != std::string::npos));
        REQUIRE((prefab.find("127") != std::string::npos));
        REQUIRE((prefab.find("255") != std::string::npos));
        REQUIRE((prefab.find("\"kept\": true") != std::string::npos));
        REQUIRE((scene.find(std::string{"\"sourceAsset\": \""} + std::string{PrefabId} + "\"") != std::string::npos));
        REQUIRE((scene.find("\"sourcePath\"") == std::string::npos));
        REQUIRE((scene.find("\"unknown\": \"kept\"") != std::string::npos));
        REQUIRE((project.Read("assets/prefabs/player.prefab") == sourcePrefab));
        REQUIRE((project.Read("assets/scenes/main.horo") == sourceScene));
    }

    TEST_CASE("Production Prefab Migration Enforces Source Payload Boundary", "[unit][application][prefab]") {
        ProductionMigrationFixture fixture;
        TemporaryProject project;
        project.Write(".horo/project.json", R"({"projectId":"prefab-boundary","settings":{}})");
        project.Write("assets/prefabs/boundary.prefab.horo",
                      R"({"schemaVersion":1,"assetId":"10112233-4455-6677-8899-aabbccddeeff","assetType":"core.prefab"})");
        constexpr std::string_view Prefix = R"({"projectVersion":"0.0.1","padding":")";
        constexpr std::string_view Suffix = R"("})";
        constexpr std::size_t MaximumPrefabBytes = 4U * 1024U * 1024U;
        std::string bounded{Prefix};
        bounded.append(MaximumPrefabBytes - Prefix.size() - Suffix.size(), 'x');
        bounded.append(Suffix);
        REQUIRE((bounded.size() == MaximumPrefabBytes));
        project.Write("assets/prefabs/boundary.prefab", bounded);
        REQUIRE((fixture.Prepare(project).HasValue()));

        bounded.push_back(' ');
        project.Write("assets/prefabs/boundary.prefab", bounded);
        const std::string authoritative = project.Read("assets/prefabs/boundary.prefab");
        auto rejected = fixture.Prepare(project);
        REQUIRE((rejected.HasError()));
        REQUIRE((rejected.ErrorValue().message.find("payload bound") != std::string::npos));
        REQUIRE((project.Read("assets/prefabs/boundary.prefab") == authoritative));
    }

    TEST_CASE("Production Prefab Migration Rejects Future And Conflicting References Transactionally", "[unit][application][prefab]") {
        constexpr std::string_view Sidecar =
            R"({"schemaVersion":1,"assetId":"20112233-4455-6677-8899-aabbccddeeff","assetType":"core.prefab"})";
        ProductionMigrationFixture fixture;

        TemporaryProject future;
        future.Write(".horo/project.json", R"({"projectId":"prefab-future","settings":{}})");
        future.Write("assets/prefabs/future.prefab.horo", std::string{Sidecar});
        future.Write("assets/prefabs/future.prefab", R"({"projectVersion":"0.2.0","unknown":{"kept":true}})");
        const std::string futureSource = future.Read("assets/prefabs/future.prefab");
        auto futureResult = fixture.Prepare(future);
        REQUIRE((futureResult.HasError()));
        REQUIRE((futureResult.ErrorValue().message.find("newer than the migration target") != std::string::npos));
        REQUIRE((future.Read("assets/prefabs/future.prefab") == futureSource));

        TemporaryProject conflicting;
        conflicting.Write(".horo/project.json", R"({"projectId":"prefab-conflict","settings":{}})");
        conflicting.Write("assets/prefabs/player.prefab.horo", std::string{Sidecar});
        conflicting.Write("assets/prefabs/player.prefab", R"({"projectVersion":"0.0.1"})");
        conflicting.Write(
            "assets/scenes/main.scene.horo",
            R"({"schemaVersion":1,"objects":[],"prefabInstances":[{"instanceId":1,"sourcePath":"assets/prefabs/player.prefab","sourceAsset":"30112233-4455-6677-8899-aabbccddeeff"}]})");
        const std::string conflictingPrefab = conflicting.Read("assets/prefabs/player.prefab");
        const std::string conflictingScene = conflicting.Read("assets/scenes/main.scene.horo");
        auto conflictResult = fixture.Prepare(conflicting);
        REQUIRE((conflictResult.HasError()));
        REQUIRE((conflictResult.ErrorValue().message.find("conflicts with the typed prefab sidecar registry") != std::string::npos));
        REQUIRE((conflicting.Read("assets/prefabs/player.prefab") == conflictingPrefab));
        REQUIRE((conflicting.Read("assets/scenes/main.scene.horo") == conflictingScene));
    }
}  // namespace project_migration_tests
