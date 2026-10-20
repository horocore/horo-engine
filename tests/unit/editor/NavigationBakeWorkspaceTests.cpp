#include "EditorNavigationBakeHost.h"
#include "Horo/Editor/NavigationBakeWorkspace.h"
#include "navigation/NavigationProjectBakeInputFixture.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

namespace Horo::Editor {
    namespace {
        using Application::NavigationInputTestSupport::Project;

        /** @brief Pumps the executable owner with a finite tooling deadline, never a UI callback or worker wait. */
        template <typename Predicate>
        NavigationBakeWorkspaceSnapshot Until(EditorNavigationBakeHost &host, const NavigationBakeWorkspaceAccess &access,
                                              const std::filesystem::path &root, Predicate done) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{15};
            while (std::chrono::steady_clock::now() < deadline) {
                host.Pump();
                auto view = access.query(root);
                REQUIRE(view.HasValue());
                if (done(view.Value()))
                    return view.Value();
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            FAIL("Navigation workspace owner did not reach the requested state");
            return {};
        }

        /** @brief Observes complete preparation, including typed failure rather than treating unavailable as ready. */
        NavigationBakeWorkspaceSnapshot Ready(EditorNavigationBakeHost &host, const NavigationBakeWorkspaceAccess &access,
                                              const std::filesystem::path &root) {
            REQUIRE(access.prepare(root).HasValue());
            auto view = Until(host, access, root, [](const auto &snapshot) {
                return snapshot.catalog || snapshot.diagnostic;
            });
            REQUIRE_FALSE(view.diagnostic);
            REQUIRE(view.catalog);
            return view;
        }

        /** @brief Waits for the actual application terminal projection rather than inferring completion from a log. */
        NavigationBakeWorkspaceSnapshot Finished(EditorNavigationBakeHost &host, const NavigationBakeWorkspaceAccess &access,
                                                 const std::filesystem::path &root) {
            return Until(host, access, root, [](const auto &snapshot) {
                return snapshot.operation && snapshot.operation->IsTerminal();
            });
        }
    }  // namespace

    TEST_CASE("Closing navigation decisions and dropping views leave the actual executable bake running",
              "[editor][navigation][bake][lifecycle]") {
        Project project;
        OperationStore operations{16, 32};
        EditorNavigationBakeHost host{operations};
        const auto root = std::filesystem::canonical(project.source.directory.Path());
        const auto access = host.Access();
        const auto ready = Ready(host, access, root);
        REQUIRE(ready.catalog->surfaces.size() == 2);
        CHECK(ready.catalog->surfaces[0].surface == ready.catalog->surfaces[1].surface);
        CHECK(ready.catalog->surfaces[0].scope != ready.catalog->surfaces[1].scope);
        REQUIRE_FALSE(ready.catalog->profiles.empty());
        const auto started = access.start(root, {.execution = Navigation::NavigationBakeExecutionMode::ValidateOnly});
        REQUIRE(started.HasValue());
        {
            NavigationBakeCancelDecision dismissed{[&](NavigationBakeOperationIdentity id) {
                return access.cancel(root, id);
            }, started.Value()};
            auto detachedView = access.query(root).Value();
            CHECK(detachedView.operation->id == started.Value().command);
        }
        const auto finished = Finished(host, access, root);
        CHECK(finished.operation->state == OperationState::Succeeded);
        CHECK_FALSE(finished.operation->publication);
        CHECK(finished.elapsed.count() >= 0);
        REQUIRE(finished.operation->bakeOperation);
        const auto shared = operations.SnapshotIfChanged(0);
        REQUIRE(shared);
        CHECK(std::ranges::any_of(shared->operations, [&](const auto &operation) {
            return operation.id == *finished.operation->bakeOperation && operation.state == finished.operation->state;
        }));
    }

    TEST_CASE("Retry captures new source with a fresh identity and stale confirmations cannot cancel it",
              "[editor][navigation][bake][retry]") {
        Project project;
        OperationStore operations{16, 32};
        EditorNavigationBakeHost host{operations};
        const auto root = std::filesystem::canonical(project.source.directory.Path());
        const auto access = host.Access();
        Ready(host, access, root);
        const auto first = access.start(root, {.execution = Navigation::NavigationBakeExecutionMode::ValidateOnly});
        REQUIRE(first.HasValue());
        CHECK_FALSE(access.start(root, {}).HasValue());
        CHECK_FALSE(access.retry(root, first.Value()).HasValue());
        REQUIRE(Finished(host, access, root).operation->state == OperationState::Succeeded);
        project.Material(2);
        const auto retry = access.retry(root, first.Value());
        REQUIRE(retry.HasValue());
        CHECK(retry.Value() != first.Value());
        NavigationBakeCancelDecision stale{[&](NavigationBakeOperationIdentity id) {
            return access.cancel(root, id);
        }, first.Value()};
        CHECK_FALSE(stale.Confirm().HasValue());
        const auto result = Finished(host, access, root);
        CHECK(result.operation->state == OperationState::Succeeded);
        CHECK(result.operation->id == retry.Value().command);
        CHECK_FALSE(access.retry(root, first.Value()).HasValue());
    }

    TEST_CASE("Explicit confirmation cancels the exact capture and can be confirmed idempotently", "[editor][navigation][bake][cancel]") {
        Project project;
        OperationStore operations{16, 32};
        EditorNavigationBakeHost host{operations};
        const auto root = std::filesystem::canonical(project.source.directory.Path());
        const auto access = host.Access();
        Ready(host, access, root);
        const auto first = access.start(root, {});
        REQUIRE(first.HasValue());
        NavigationBakeCancelDecision confirm{[&](NavigationBakeOperationIdentity id) {
            return access.cancel(root, id);
        }, first.Value()};
        REQUIRE(confirm.Confirm().HasValue());
        REQUIRE(confirm.Confirm().HasValue());
        const auto result = Finished(host, access, root);
        CHECK(result.operation->state == OperationState::Cancelled);
        CHECK_FALSE(result.operation->publication);
        CHECK_FALSE(std::filesystem::exists(root / ".horo/cooked/navigation"));
    }

    TEST_CASE("Selected surface retries retain the complete closure and reuse the canonical published workspace",
              "[editor][navigation][bake][publication]") {
        Project project;
        OperationStore operations{16, 32};
        EditorNavigationBakeHost host{operations};
        const auto root = std::filesystem::canonical(project.source.directory.Path());
        const auto access = host.Access();
        const auto ready = Ready(host, access, root);
        REQUIRE(access.start(root, {}).HasValue());
        const auto published = Finished(host, access, root);
        REQUIRE(published.operation->state == OperationState::Succeeded);
        REQUIRE(published.operation->publication);
        const auto selected = access.start(root, {.surfaces = std::vector{ready.catalog->surfaces.front()}});
        REQUIRE(selected.HasValue());
        const auto replaced = Finished(host, access, root);
        REQUIRE(replaced.operation->state == OperationState::Succeeded);
        CHECK(replaced.operation->surfaceCount == 2);
        CHECK(replaced.operation->tileCount == published.operation->tileCount);
        REQUIRE(replaced.operation->publication);
        CHECK(replaced.operation->publication->reusedTiles == published.operation->tileCount);
    }

    TEST_CASE("Reloaded saved choices cannot retarget a prior opening confirmation", "[editor][navigation][bake][reload]") {
        Project project;
        OperationStore operations{16, 32};
        EditorNavigationBakeHost host{operations};
        const auto root = std::filesystem::canonical(project.source.directory.Path());
        const auto access = host.Access();
        const auto original = Ready(host, access, root);
        const auto first = access.start(root, {.execution = Navigation::NavigationBakeExecutionMode::ValidateOnly});
        REQUIRE(first.HasValue());
        CHECK_FALSE(access.reload(root).HasValue());
        REQUIRE(Finished(host, access, root).operation->state == OperationState::Succeeded);
        auto object = Project::Object();
        object.components.navigationSurface->id = Navigation::TestSupport::Id<Navigation::SurfaceId>(99);
        object.components.navigationSources.front().surface = object.components.navigationSurface->id;
        project.Scenes(object);
        REQUIRE(access.reload(root).HasValue());
        const auto refreshed = Ready(host, access, root);
        CHECK(refreshed.opening != original.opening);
        REQUIRE(refreshed.catalog->surfaces.size() == 2);
        CHECK(refreshed.catalog->surfaces.front().surface == object.components.navigationSurface->id);
        const auto second = access.start(root, {.execution = Navigation::NavigationBakeExecutionMode::ValidateOnly});
        REQUIRE(second.HasValue());
        CHECK(second.Value() != first.Value());
        CHECK_FALSE(access.cancel(root, first.Value()).HasValue());
        CHECK_FALSE(access.retry(root, first.Value()).HasValue());
        CHECK(Finished(host, access, root).operation->state == OperationState::Succeeded);
    }

    TEST_CASE("Executable shutdown drains accepted capture without any navigation view remaining", "[editor][navigation][bake][shutdown]") {
        Project project;
        OperationStore operations{16, 32};
        const auto root = std::filesystem::canonical(project.source.directory.Path());
        {
            EditorNavigationBakeHost host{operations};
            const auto access = host.Access();
            Ready(host, access, root);
            REQUIRE(access.start(root, {}).HasValue());
        }
        CHECK_FALSE(std::filesystem::exists(root / ".horo/cooked/navigation"));
    }
}  // namespace Horo::Editor
