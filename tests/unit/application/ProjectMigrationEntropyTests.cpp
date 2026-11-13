#include "Horo/Security/SecureMemory.h"
#include "Horo/Security/SecurityErrors.h"
#include "ProjectMigrationUnitFixture.h"

#include <concepts>

namespace project_migration_tests {
    namespace {
        using namespace Horo;
        using namespace Horo::Application;

        /** @brief Records synchronous injected calls without consulting a native entropy provider. */
        class TestRandom final : public Security::SecureRandomSource {
        public:
            bool fail{};
            std::uint32_t calls{};

            Result<void> Fill(const std::span<std::byte> destination) override {
                ++calls;
                std::ranges::fill(destination, std::byte{0xA5});
                if (fail)
                    return Result<void>::Failure(MakeError(TestFailure));
                return Result<void>::Success();
            }
        };

        /** @brief Observes failure clearing independently of disposable candidate cleanup. */
        struct EntropyObservation {
            std::vector<std::byte> bytes = std::vector<std::byte>(16, std::byte{0xFF});
        };

        /** @brief Requests entropy only from Execute and stages the resulting bytes without publishing them. */
        class EntropyStage final : public IProjectMigrationStage {
        public:
            explicit EntropyStage(std::shared_ptr<EntropyObservation> observation) : observation_(std::move(observation)) {}

            MigrationStageDescriptor Describe() const override {
                return {.id = {"test.entropy"}, .writeFamilies = {"other"}};
            }

            Result<void> Execute(ProjectMigrationContext &context, const CancellationToken &) const override {
                auto filled = context.FillSecureRandom(observation_->bytes);
                if (filled.HasError())
                    return filled;
                return context.AddDocument("assets/generated.bin", MigrationDocumentKind::Other, observation_->bytes);
            }

        private:
            std::shared_ptr<EntropyObservation> observation_;
        };

        /** @brief Constructs inert metadata and a pipeline; it never executes the supplied entropy capability. */
        ProjectMigrationPlan EntropyPlan(const std::shared_ptr<EntropyObservation> &observation) {
            auto builder = ProjectMigrationPipelineBuilder::Begin({"test.entropy"});
            static_cast<void>(builder.AddThen(std::make_shared<EntropyStage>(observation)));
            auto pipeline = std::move(builder).Build();
            REQUIRE(pipeline.HasValue());
            auto definition =
                Definition("test.entropy", ProjectMigrationDefinitionKind::Sequential, "0.0.1", "0.1.0", 1, 2, std::move(pipeline).Value());
            return {.source = definition.from, .target = definition.to, .definitions = {std::move(definition)}};
        }

        template <typename Context>
        concept CanRequestEntropy = requires(Context &context, std::span<std::byte> bytes) {
            { context.FillSecureRandom(bytes) } -> std::same_as<Result<void>>;
        };
        static_assert(CanRequestEntropy<ProjectMigrationContext>);
        static_assert(!CanRequestEntropy<const ProjectMigrationContext>);
    }  // namespace

    TEST_CASE("Migration preparation uses explicit entropy and releases it before publication", "[unit][application][migration-entropy]") {
        TemporaryProject project;
        JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 8});
        const auto observation = std::make_shared<EntropyObservation>();
        auto random = std::make_shared<TestRandom>();
        ProjectMigrationExecutionServices services{.random = random};
        const auto plan = EntropyPlan(observation);
        REQUIRE(random->calls == 0);
        const auto original = project.Read(".horo/project.json");
        auto prepared = ProjectMigrationExecutor::Prepare(project.root, project.root / ".horo/local/entropy", plan, jobs, {}, {}, services);
        REQUIRE(prepared.HasValue());
        REQUIRE(random->calls == 1);
        const auto generated = prepared.Value().ReadCandidateDocument("assets/generated.bin");
        REQUIRE(generated.HasValue());
        CHECK(generated.Value() == std::vector<std::byte>(16, std::byte{0xA5}));
        CHECK_FALSE(std::filesystem::exists(project.root / "assets/generated.bin"));
        CHECK(project.Read(".horo/project.json") == original);
        const std::weak_ptr<Security::SecureRandomSource> lifetime = random;
        random.reset();
        services.random.reset();
        CHECK(lifetime.expired());
    }

    TEST_CASE("Migration entropy failures clear output and preserve the authoritative tree", "[unit][application][migration-entropy]") {
        TemporaryProject project;
        JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 8});
        const auto observation = std::make_shared<EntropyObservation>();
        const auto plan = EntropyPlan(observation);
        auto random = std::make_shared<TestRandom>();
        ProjectMigrationExecutionServices services;
        std::string expected{SecurityErrors::EntropyUnavailable.code.Value()};
        SECTION("Absent capability never selects a native fallback") {}
        SECTION("Provider failure retains its machine-readable identity") {
            random->fail = true;
            services.random = random;
            expected = TestFailure.code.Value();
        }
        SECTION("Empty entropy request is bounded before dispatch") {
            observation->bytes.clear();
            services.random = random;
        }
        SECTION("Unsupported entropy request is bounded before dispatch") {
            observation->bytes.resize(257, std::byte{0xFF});
            services.random = random;
        }
        const auto original = project.Read(".horo/project.json");
        auto prepared = ProjectMigrationExecutor::Prepare(project.root, project.root / ".horo/local/entropy", plan, jobs, {}, {}, services);
        REQUIRE(prepared.HasError());
        CHECK(prepared.ErrorValue().code.Value() == expected);
        CHECK(random->calls == (random->fail ? 1U : 0U));
        CHECK(std::ranges::all_of(observation->bytes, [](const std::byte value) {
            return value == std::byte{};
        }));
        CHECK_FALSE(std::filesystem::exists(project.root / "assets/generated.bin"));
        CHECK(project.Read(".horo/project.json") == original);
    }

    TEST_CASE("Verified migration dry run uses the same explicit entropy seam", "[unit][application][migration-entropy]") {
        TemporaryProject project;
        JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 8});
        const auto observation = std::make_shared<EntropyObservation>();
        auto random = std::make_shared<TestRandom>();
        const auto plan = EntropyPlan(observation);
        const auto dryRun = ProjectMigrationExecutor::VerifiedDryRun(project.root, plan, jobs, {}, {}, {.random = random});
        REQUIRE(dryRun.HasValue());
        CHECK(random->calls == 1);
        CHECK(dryRun.Value().changedFiles == std::vector<std::string>{"assets/generated.bin"});
        CHECK_FALSE(std::filesystem::exists(project.root / "assets/generated.bin"));
    }

    TEST_CASE("Cancelled preparation never requests entropy", "[unit][application][migration-entropy]") {
        TemporaryProject project;
        JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 8});
        const auto observation = std::make_shared<EntropyObservation>();
        auto random = std::make_shared<TestRandom>();
        const auto plan = EntropyPlan(observation);
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        const auto prepared = ProjectMigrationExecutor::Prepare(project.root, project.root / ".horo/local/entropy", plan, jobs, {},
                                                                cancellation.Token(), {.random = random});
        REQUIRE(prepared.HasError());
        CHECK(prepared.ErrorValue().code.Value() == "project.migration.cancelled");
        CHECK(random->calls == 0);
        CHECK_FALSE(std::filesystem::exists(project.root / "assets/generated.bin"));
    }
}  // namespace project_migration_tests
