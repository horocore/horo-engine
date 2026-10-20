#include "navigation/NavigationBakeServiceFixture.h"

#include <map>

namespace Horo::Application {
    namespace {
        using namespace Horo::Navigation;
        using namespace Horo::Navigation::TestSupport;
        using namespace BakeTestSupport;

        /** @brief Captures directories and exact file content so validation cannot silently create cache output. */
        auto TreeState(const std::filesystem::path &root) {
            std::map<std::filesystem::path, std::optional<Sha256Digest>> result;
            for (const auto &entry : std::filesystem::recursive_directory_iterator(root)) {
                const auto path = entry.path().lexically_relative(root);
                if (entry.is_directory()) {
                    result.try_emplace(path, std::nullopt);
                    continue;
                }
                REQUIRE(entry.is_regular_file());
                std::ifstream stream(entry.path(), std::ios::binary);
                REQUIRE(stream.good());
                const std::vector<char> bytes{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
                result.try_emplace(path, ComputeSha256(std::as_bytes(std::span{bytes})));
            }
            return result;
        }
    }  // namespace

    TEST_CASE("Validate-only navigation accepts a cold complete capture without builder or output writes",
              "[navigation][cook][validation]") {
        BakeHarness harness;
        const auto before = TreeState(harness.directory.path);
        const auto id = Submit(harness, NavigationBakeExecutionMode::ValidateOnly);
        const auto terminal = Terminal(*harness.service, harness.operations, id);
        CHECK(terminal.state == OperationState::Succeeded);
        CHECK(terminal.kind == OperationKind::Validation);
        CHECK(harness.builder->builds.load() == 0);
        CHECK_FALSE(harness.service->Published());
        CHECK(TreeState(harness.directory.path) == before);
    }

    TEST_CASE("Validate-only selected navigation preserves published bytes while checking current unselected content",
              "[navigation][cook][validation]") {
        BakeHarness harness;
        harness.fixture.surfaces.push_back(Id<SurfaceId>(2));
        REQUIRE(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Succeeded);
        const auto published = harness.service->Published();
        const auto before = TreeState(harness.directory.path);
        const auto builds = harness.builder->builds.load();
        harness.fixture.ExcludeBorder();
        auto valid = harness.service->Submit(Request(harness, NavigationBakeExecutionMode::ValidateOnly, std::vector{Id<SurfaceId>(1)}));
        REQUIRE(valid.HasValue());
        const auto terminal = Terminal(*harness.service, harness.operations, valid.Value());
        CHECK(terminal.state == OperationState::Succeeded);
        CHECK(terminal.kind == OperationKind::Validation);
        CHECK(harness.builder->builds.load() == builds);
        CHECK(harness.service->Published() == published);
        CHECK(TreeState(harness.directory.path) == before);
    }

    TEST_CASE("Validate-only selected navigation reports incomplete closure without repairing it", "[navigation][cook][validation]") {
        BakeHarness harness;
        harness.fixture.surfaces.push_back(Id<SurfaceId>(2));
        const auto before = TreeState(harness.directory.path);
        auto admitted = harness.service->Submit(Request(harness, NavigationBakeExecutionMode::ValidateOnly, std::vector{Id<SurfaceId>(1)}));
        REQUIRE(admitted.HasValue());
        const auto terminal = Terminal(*harness.service, harness.operations, admitted.Value());
        REQUIRE(terminal.state == OperationState::Failed);
        REQUIRE(terminal.error);
        CHECK(terminal.error->code.Value() == NavigationErrors::BakeSelectionIncomplete.code.Value());
        CHECK(harness.builder->builds.load() == 0);
        CHECK(TreeState(harness.directory.path) == before);
    }

    TEST_CASE("Empty validate-only closure performs no builder or filesystem writes", "[navigation][cook][validation][empty]") {
        BakeHarness harness;
        SECTION("cold") {
            CHECK_FALSE(harness.service->Published());
        }
        SECTION("after existing publication") {
            REQUIRE(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Succeeded);
        }
        const auto previous = harness.service->Published();
        const auto before = TreeState(harness.directory.path);
        const auto builds = harness.builder->builds.load();
        harness.fixture.RemoveSources();
        const auto id = Submit(harness, NavigationBakeExecutionMode::ValidateOnly);
        CHECK(Terminal(*harness.service, harness.operations, id).state == OperationState::Succeeded);
        CHECK(harness.builder->builds.load() == builds);
        CHECK(harness.service->Published() == previous);
        CHECK(TreeState(harness.directory.path) == before);
    }

    TEST_CASE("Unknown navigation execution modes fail before operation or builder admission", "[navigation][cook][validation]") {
        BakeHarness harness;
        const auto mode = GENERATE(NavigationBakeExecutionMode::Count, static_cast<NavigationBakeExecutionMode>(255));
        const auto rejected = harness.service->Submit(Request(harness, mode));
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == NavigationErrors::BakeInputInvalid.code.Value());
        CHECK(harness.builder->builds.load() == 0);
        CHECK_FALSE(harness.operations.SnapshotIfChanged(0).has_value());
    }

    TEST_CASE("Validation cannot join or publish an otherwise identical pending build", "[navigation][cook][validation]") {
        BakeHarness harness;
        harness.builder->pause.store(true);
        const auto build = Submit(harness);
        for (std::size_t attempt = 0; attempt < 5000 && !harness.builder->entered.load(); ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        REQUIRE(harness.builder->entered.load());
        const auto validation = Submit(harness, NavigationBakeExecutionMode::ValidateOnly);
        CHECK(validation != build);
        harness.builder->pause.store(false);
        CHECK(Terminal(*harness.service, harness.operations, build).state == OperationState::Cancelled);
        CHECK(Terminal(*harness.service, harness.operations, validation).state == OperationState::Succeeded);
        CHECK(harness.builder->builds.load() == 1);
        CHECK_FALSE(harness.service->Published());
        CHECK_FALSE(std::filesystem::exists(harness.config.targetRoot / "current.json"));
    }

    namespace {
        /** @brief Counts worker verification calls atomically; the terminal job drains callbacks before test inspection. */
        class CapturedSourceProbe final : public Navigation::INavigationBakeSourceValidation {
        public:
            explicit CapturedSourceProbe(const std::size_t staleAt = 0) : staleAt_(staleAt) {}

            [[nodiscard]] Result<void> ValidateCurrent(const CancellationToken &cancel) const override {
                const auto call = calls_.fetch_add(1) + 1;
                if (cancel.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(NavigationErrors::BakeInputCancelled));
                if (call == staleAt_)
                    return Result<void>::Failure(MakeError(NavigationErrors::BakeInputStale));
                return Result<void>::Success();
            }

            [[nodiscard]] std::size_t Calls() const noexcept {
                return calls_.load();
            }

        private:
            const std::size_t staleAt_;
            mutable std::atomic<std::size_t> calls_{};
        };
    }  // namespace

    TEST_CASE("Owned source capture is verified at gather and each mode's final authority fence", "[navigation][cook][source_fence]") {
        const auto mode = GENERATE(NavigationBakeExecutionMode::BuildAndPublish, NavigationBakeExecutionMode::ValidateOnly);
        BakeHarness harness;
        const auto probe = std::make_shared<CapturedSourceProbe>();
        auto request = Request(harness, mode);
        request.sourceValidation = probe;
        const auto admitted = harness.service->Submit(std::move(request));
        REQUIRE(admitted.HasValue());
        const auto terminal = Terminal(*harness.service, harness.operations, admitted.Value());
        CHECK(terminal.state == OperationState::Succeeded);
        CHECK(probe->Calls() == 2);
        CHECK(static_cast<bool>(harness.service->Published()) == (mode == NavigationBakeExecutionMode::BuildAndPublish));
    }

    TEST_CASE("Stale external source capture preserves the previous generation at either worker fence",
              "[navigation][cook][source_fence]") {
        const auto mode = GENERATE(NavigationBakeExecutionMode::BuildAndPublish, NavigationBakeExecutionMode::ValidateOnly);
        const auto staleAt = GENERATE(std::size_t{1}, std::size_t{2});
        BakeHarness harness;
        REQUIRE(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Succeeded);
        const auto previous = harness.service->Published();
        const auto before = TreeState(harness.directory.path);
        const auto builds = harness.builder->builds.load();
        const auto probe = std::make_shared<CapturedSourceProbe>(staleAt);
        auto request = Request(harness, mode);
        request.sourceValidation = probe;
        const auto admitted = harness.service->Submit(std::move(request));
        REQUIRE(admitted.HasValue());
        const auto terminal = Terminal(*harness.service, harness.operations, admitted.Value());
        CHECK(terminal.state == OperationState::Cancelled);
        REQUIRE(terminal.error);
        CHECK(ErrorChainContains(terminal.error.value(), NavigationErrors::BakeInputStale.domain, NavigationErrors::BakeInputStale.code));
        CHECK(probe->Calls() == staleAt);
        CHECK(harness.service->Published() == previous);
        CHECK(harness.builder->builds.load() == builds);
        if (mode == NavigationBakeExecutionMode::ValidateOnly)
            CHECK(TreeState(harness.directory.path) == before);
        const auto durable = Assets::ResolveCurrentCookGeneration(harness.config.targetRoot);
        REQUIRE(durable.HasValue());
        CHECK(durable.Value().manifestDigest == previous->generation.manifestDigest);
    }

    TEST_CASE("Distinct retained source owners cannot join otherwise identical in-flight bake requests",
              "[navigation][cook][source_fence]") {
        BakeHarness harness;
        harness.builder->pause.store(true);
        const auto firstProbe = std::make_shared<CapturedSourceProbe>();
        const auto secondProbe = std::make_shared<CapturedSourceProbe>();
        auto request = Request(harness);
        request.sourceValidation = firstProbe;
        const auto first = harness.service->Submit(std::move(request));
        REQUIRE(first.HasValue());
        for (std::size_t attempt = 0; attempt < 5000 && !harness.builder->entered.load(); ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        REQUIRE(harness.builder->entered.load());
        request = Request(harness);
        request.sourceValidation = secondProbe;
        const auto second = harness.service->Submit(std::move(request));
        REQUIRE(second.HasValue());
        CHECK(second.Value() != first.Value());
        harness.builder->pause.store(false);
        CHECK(Terminal(*harness.service, harness.operations, first.Value()).state == OperationState::Cancelled);
        CHECK(Terminal(*harness.service, harness.operations, second.Value()).state == OperationState::Succeeded);
        CHECK(secondProbe->Calls() == 2);
    }

}  // namespace Horo::Application
