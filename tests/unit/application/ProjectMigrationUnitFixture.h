/** @file
 * @brief Private fixtures for project migration pipeline and execution tests.
 */
#pragma once

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
    inline const Horo::ErrorCodeDescriptor TestFailure{
        .domain = Horo::ErrorDomainId("test.project_migration"),
        .code = Horo::ErrorCode("test.project_migration.failed"),
        .defaultSeverity = Horo::ErrorSeverity::Error,
        .summary = "Migration fixture failed.",
        .remediationHint = "Inspect the fixture.",
    };

    [[nodiscard]] inline Horo::Application::ContractBaselineVersion Version(const char *text) {
        const auto parsed = Horo::Application::ParseHoroVersion(text);
        REQUIRE((parsed.HasValue()));
        return Horo::Application::ContractBaselineVersion{parsed.Value()};
    }

    [[nodiscard]] inline Horo::Application::PersistentContractHash Contract(const std::uint8_t marker) {
        Horo::Application::PersistentContractHash hash;
        hash.bytes.fill(marker);
        return hash;
    }

    [[nodiscard]] inline std::vector<std::byte> Bytes(const std::string &text) {
        std::vector<std::byte> bytes(text.size());
        std::ranges::transform(text, bytes.begin(), [](const char value) {
            return static_cast<std::byte>(value);
        });
        return bytes;
    }

    [[nodiscard]] inline std::string Text(const std::span<const std::byte> bytes) {
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

    [[nodiscard]] inline std::shared_ptr<const Horo::Application::ProjectMigrationPipeline> Pipeline(const std::string &id,
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

    [[nodiscard]] inline Horo::Application::ProjectMigrationDefinition Definition(
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

    [[nodiscard]] inline Horo::Application::ProjectMigrationPlan ProductionCompressionPlan() {
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

}  // namespace project_migration_tests
