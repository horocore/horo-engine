#include "navigation/NavigationBakeApplicationFixture.h"

#include <algorithm>
#include <array>
#include <catch2/generators/catch_generators.hpp>

namespace Horo::Application {
    namespace {
        namespace Nav = Navigation;
        using NavigationApplicationTestSupport::Session;
        using NavigationSourceTestSupport::Asset;
    }  // namespace

    TEST_CASE("Navigation command captures real project input and validates without creating output", "[navigation][application][bake]") {
        Session session;
        const auto result = session.Execute(Nav::NavigationBakeExecutionMode::ValidateOnly);
        REQUIRE(result.state == OperationState::Succeeded);
        CHECK(result.inputFingerprint.has_value());
        CHECK(result.surfaceCount == 2);
        CHECK(result.tileCount == 2);
        CHECK_FALSE(result.publication);
        CHECK(session.builder->builds.load() == 0);
        CHECK_FALSE(std::filesystem::exists((session.project.source.directory.Path() / ".horo/cache/navigation")));
        CHECK_FALSE(std::filesystem::exists(session.service.targetRoot));
    }

    TEST_CASE("Command results distinguish fresh validation from prior publication and reuse stable content",
              "[navigation][application][bake]") {
        Session session;
        const auto first = session.Execute(Nav::NavigationBakeExecutionMode::BuildAndPublish);
        REQUIRE(first.state == OperationState::Succeeded);
        REQUIRE(first.publication);
        CHECK(first.publication->rebuiltTiles == 2);
        CHECK(first.publication->reusedTiles == 0);
        const auto validation = session.Execute(Nav::NavigationBakeExecutionMode::ValidateOnly);
        REQUIRE(validation.state == OperationState::Succeeded);
        CHECK_FALSE(validation.publication);
        CHECK(validation.inputFingerprint == first.inputFingerprint);
        const auto repeated = session.Execute(Nav::NavigationBakeExecutionMode::BuildAndPublish);
        REQUIRE(repeated.state == OperationState::Succeeded);
        REQUIRE(repeated.publication);
        CHECK(repeated.inputFingerprint == first.inputFingerprint);
        CHECK(repeated.publication->rebuiltTiles == 0);
        CHECK(repeated.publication->reusedTiles == 2);
        CHECK(session.builder->builds.load() == 2);
        CHECK_FALSE(session.application->Query(first.id).HasValue());
    }

    TEST_CASE("Command cancellation drains source capture without publication and permits a subsequent command",
              "[navigation][application][cancellation]") {
        Session session;
        CancellationSource parent;
        parent.RequestCancellation();
        const auto started = session.application->Start({.invocation = {.cancellation = parent.Token()}});
        REQUIRE(started.HasValue());
        const auto result = session.Finish(started.Value());
        CHECK(result.state == OperationState::Cancelled);
        CHECK_FALSE(result.publication);
        CHECK(session.builder->builds.load() == 0);
        CHECK_FALSE(std::filesystem::exists(session.service.targetRoot));
        CHECK(session.Execute(Nav::NavigationBakeExecutionMode::ValidateOnly).state == OperationState::Succeeded);
    }

    TEST_CASE("Navigation command rejects concurrent admission and closes only after its owned capture drains",
              "[navigation][application][lifecycle]") {
        Session session;
        const auto started = session.application->Start({});
        REQUIRE(started.HasValue());
        CHECK_FALSE(session.application->Start({}).HasValue());
        CHECK_FALSE(session.application->RequestCancel(started.Value() + 1).HasValue());
        CHECK_FALSE(session.application->Close().HasValue());
        const auto result = session.Finish(started.Value());
        CHECK(result.IsTerminal());
        CHECK(session.application->Close().HasValue());
        CHECK_FALSE(session.application->Start({}).HasValue());
    }

    TEST_CASE("Selected command keeps the complete project closure and rejects unknown Scene selection",
              "[navigation][application][selection]") {
        Session session;
        REQUIRE(session.Execute(Nav::NavigationBakeExecutionMode::BuildAndPublish).state == OperationState::Succeeded);
        const auto scope = Nav::NavigationBakeScopeId::ForScene(Asset(2).Bytes()).Value();
        const auto surface = Nav::TestSupport::Id<Nav::SurfaceId>(10);
        const auto started = session.application->Start({.surfaces = std::vector{Nav::NavigationBakeSurfaceKey{surface, scope}}});
        REQUIRE(started.HasValue());
        const auto result = session.Finish(started.Value());
        REQUIRE(result.state == OperationState::Succeeded);
        REQUIRE(result.publication);
        CHECK(result.tileCount == 2);
        CHECK(result.publication->reusedTiles == 2);
        const auto foreign = Nav::NavigationBakeScopeId::ForScene(Asset(99).Bytes()).Value();
        const auto unknown = session.application->Start({.surfaces = std::vector{Nav::NavigationBakeSurfaceKey{surface, foreign}}});
        REQUIRE(unknown.HasValue());
        CHECK(session.Finish(unknown.Value()).state == OperationState::Failed);
        CHECK(session.builder->builds.load() == 2);
    }

    TEST_CASE("Command capture failure retains canonical source diagnostics without invoking a builder",
              "[navigation][application][source]") {
        Session session;
        session.project.source.Write("Assets/first.horo", "malformed scene source");
        const auto result = session.Execute(Nav::NavigationBakeExecutionMode::BuildAndPublish);
        REQUIRE(result.state == OperationState::Failed);
        CHECK(result.diagnostic.has_value());
        CHECK_FALSE(result.publication);
        CHECK(session.builder->builds.load() == 0);
        CHECK_FALSE(std::filesystem::exists(session.service.targetRoot));
    }

    TEST_CASE("Navigation command retains live invocation authority through actual source capture",
              "[navigation][application][authority]") {
        Session session;
        auto revoked = std::make_shared<std::atomic<bool>>(true);
        const auto started = session.application->Start({.invocation = {.authorityStopped = [revoked] {
            return revoked->load();
        }}});
        REQUIRE(started.HasValue());
        const auto result = session.Finish(started.Value());
        CHECK(result.state == OperationState::Cancelled);
        CHECK_FALSE(result.publication);
        CHECK(session.builder->builds.load() == 0);
        CHECK_FALSE(std::filesystem::exists(session.service.targetRoot));
    }

    TEST_CASE("Invalid command modes and empty selections do not acquire project source", "[navigation][application][admission]") {
        Session session;
        SECTION("unknown mode") {
            const auto mode = GENERATE(Nav::NavigationBakeExecutionMode::Count, static_cast<Nav::NavigationBakeExecutionMode>(255));
            CHECK(session.application->Start({.execution = mode}).HasError());
        }
        SECTION("empty selection") {
            CHECK(session.application->Start({.surfaces = std::vector<Nav::NavigationBakeSurfaceKey>{}}).HasError());
        }
        SECTION("oversized selection") {
            CHECK(session.application->Start({.surfaces = std::vector<Nav::NavigationBakeSurfaceKey>(4097)}).HasError());
        }
        CHECK(session.application->Query(1).HasError());
        CHECK(session.application->RequestCancel(1).HasError());
        CHECK(session.application->Pump().HasValue());
        CHECK(session.builder->builds.load() == 0);
        CHECK_FALSE(std::filesystem::exists(session.service.targetRoot));
        CHECK(session.application->Close().HasValue());
        CHECK(session.application->Close().HasValue());
        CHECK(session.application->Start({}).HasError());
    }

    TEST_CASE("Foreign owner lanes cannot mutate or observe the command facade", "[navigation][application][lifecycle]") {
        Session session;
        std::array<bool, 5> rejected{};
        // Thread join publishes every observation before the owner inspects it; no callback escapes this test.
        std::jthread foreign([&session, &rejected] {
            rejected[0] = session.application->Start({}).HasError();
            rejected[1] = session.application->Pump().HasError();
            rejected[2] = session.application->Query(1).HasError();
            rejected[3] = session.application->RequestCancel(1).HasError();
            rejected[4] = session.application->Close().HasError();
        });
        foreign.join();
        CHECK(std::ranges::all_of(rejected, [](bool value) {
            return value;
        }));
        CHECK(session.Execute(Nav::NavigationBakeExecutionMode::ValidateOnly).state == OperationState::Succeeded);
    }

    TEST_CASE("Expired invocation deadline cannot become a fresh capture budget", "[navigation][application][deadline]") {
        Session session;
        const auto started =
            session.application->Start({.invocation = {.deadline = std::chrono::steady_clock::now() - std::chrono::seconds{1}}});
        REQUIRE(started.HasValue());
        const auto terminal = session.Finish(started.Value());
        CHECK(terminal.IsTerminal());
        CHECK(terminal.state != OperationState::Succeeded);
        CHECK(terminal.diagnostic.has_value());
        CHECK_FALSE(terminal.publication);
        CHECK(session.builder->builds.load() == 0);
        CHECK_FALSE(std::filesystem::exists(session.service.targetRoot));
    }

    TEST_CASE("Application admission rejects absent capture and every missing host compatibility stamp",
              "[navigation][application][admission]") {
        Session session;
        NavigationBakeApplicationConfig config{.service = session.service,
                                               .compatibility = Nav::TestSupport::IncrementalBakeFixture{}.compatibility,
                                               .captureSource = [](const NavigationBakeInvocation &) {
            return Result<std::shared_ptr<const NavigationProjectBakeSource>>::Failure(MakeError(Nav::NavigationErrors::BakeInputInvalid));
        }};
        SECTION("capture callback") {
            config.captureSource = {};
        }
        SECTION("provider stamp") {
            config.compatibility.provider = {};
        }
        SECTION("schema stamp") {
            config.compatibility.schemas = {};
        }
        SECTION("host settings stamp") {
            config.compatibility.settings = {};
        }
        auto created = NavigationBakeApplication::Create(std::move(config), session.operations, session.jobs);
        if (created.HasValue())
            REQUIRE(created.Value()->Close().HasValue());
        CHECK(created.HasError());
        CHECK(session.builder->builds.load() == 0);
        CHECK_FALSE(std::filesystem::exists(session.service.targetRoot));
    }

}  // namespace Horo::Application
