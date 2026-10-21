#include "EditorNavigationBakeHost.h"
#include "Horo/Editor/NavigationBakeWorkspace.h"
#include "navigation/NavigationProjectBakeInputFixture.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <format>
#include <string_view>
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
        REQUIRE(published.overlays);
        CHECK(access.overlays(root) == published.overlays);
        REQUIRE(published.overlays->Scenes().size() == 2);
        for (const auto &scene : published.overlays->Scenes()) {
            CHECK(std::ranges::any_of(scene.primitives, [](const auto &primitive) {
                return primitive.kind == NavigationOverlayKind::Walkable && primitive.triangle &&
                       primitive.validity == NavigationOverlayValidity::Cooked && primitive.tile.has_value();
            }));
        }
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

    TEST_CASE("Navigation host rejects unprepared and noncanonical roots without allocating an opening",
              "[editor][navigation][bake][admission]") {
        Project project;
        OperationStore operations{16, 32};
        EditorNavigationBakeHost host{operations};
        const auto access = host.Access();
        const auto root = std::filesystem::canonical(project.source.directory.Path());
        CHECK(access.query(root).HasError());
        CHECK_FALSE(access.overlays(root));
        CHECK(access.start(root, {}).HasError());
        CHECK(access.retry(root, {1, 1}).HasError());
        CHECK(access.cancel(root, {1, 1}).HasError());
        CHECK(access.prepare("relative-project").HasError());
        CHECK(access.prepare(root / "unused" / "..").HasError());
        const auto ready = Ready(host, access, root);
        CHECK(ready.opening == 1);
        REQUIRE(access.prepare(root).HasValue());
        CHECK(access.query(root).Value().opening == ready.opening);
    }

    TEST_CASE("Preparation retains canonical source failures until explicit saved-source reload",
              "[editor][navigation][bake][preparation]") {
        Project project;
        project.source.Write("Assets/first.horo", "not a Scene document");
        const auto expected = Application::NavigationProjectBakeSource::Capture(project.source.files, project.source.Context(),
                                                                                project.source.Compatibility());
        REQUIRE(expected.HasError());
        OperationStore operations{16, 32};
        EditorNavigationBakeHost host{operations};
        const auto access = host.Access();
        const auto root = std::filesystem::canonical(project.source.directory.Path());
        REQUIRE(access.prepare(root).HasValue());
        const auto failed = Until(host, access, root, [](const auto &view) {
            return view.diagnostic.has_value();
        });
        CHECK(failed.diagnostic->code.Value() == expected.ErrorValue().code.Value());
        CHECK_FALSE(failed.catalog);
        CHECK_FALSE(access.overlays(root));
        CHECK(access.start(root, {}).HasError());
        project.Scenes(Project::Object());
        REQUIRE(access.prepare(root).HasValue());
        host.Pump();
        const auto retained = access.query(root).Value();
        REQUIRE(retained.diagnostic);
        CHECK(retained.diagnostic->code.Value() == failed.diagnostic->code.Value());
        CHECK(retained.opening == failed.opening);
        REQUIRE(access.reload(root).HasValue());
        const auto recovered = Ready(host, access, root);
        CHECK(recovered.opening != failed.opening);
        REQUIRE(recovered.overlays);
        CHECK(recovered.overlays->Scenes().size() == 2);
    }

    TEST_CASE("Navigation host bounds retained project preparations and replaces a failed opening explicitly",
              "[editor][navigation][bake][capacity]") {
        Project project;
        OperationStore operations{16, 32};
        EditorNavigationBakeHost host{operations};
        const auto access = host.Access();
        const auto root = std::filesystem::canonical(project.source.directory.Path());
        std::array<std::filesystem::path, 8> roots;
        for (std::size_t i = 0; i < roots.size(); ++i) {
            roots[i] = root / std::format("missing-project-{}", i);
            REQUIRE(access.prepare(roots[i]).HasValue());
        }
        const auto rejected = access.prepare(root);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == Navigation::NavigationErrors::CapacityExceeded.code.Value());
        for (std::size_t i = 0; i < roots.size(); ++i) {
            const auto failed = Until(host, access, roots[i], [](const auto &view) {
                return view.diagnostic.has_value();
            });
            CHECK(failed.opening == i + 1);
            CHECK_FALSE(failed.catalog);
        }
        REQUIRE(access.reload(roots.front()).HasValue());
        const auto replaced = Until(host, access, roots.front(), [](const auto &view) {
            return view.diagnostic.has_value();
        });
        CHECK(replaced.opening == 9);
        CHECK(access.prepare(root).HasError());
    }

    TEST_CASE("Failed fresh capture clears the overlay mailbox and retry publishes new immutable authored evidence",
              "[editor][navigation][bake][overlay][retry]") {
        Project project;
        OperationStore operations{16, 32};
        EditorNavigationBakeHost host{operations};
        const auto access = host.Access();
        const auto root = std::filesystem::canonical(project.source.directory.Path());
        const auto original = Ready(host, access, root).overlays;
        REQUIRE(original);
        const auto originalDigest = original->Scenes().front().sourceDigest;
        project.source.Write("Assets/first.horo", "invalid fresh source");
        const auto first = access.start(root, {.execution = Navigation::NavigationBakeExecutionMode::ValidateOnly});
        REQUIRE(first.HasValue());
        const auto failed = Finished(host, access, root);
        CHECK(failed.operation->state == OperationState::Failed);
        REQUIRE(failed.operation->diagnostic);
        CHECK_FALSE(failed.overlays);
        CHECK_FALSE(access.overlays(root));
        auto object = Project::Object();
        object.components.navigationSurface->id = Navigation::TestSupport::Id<Navigation::SurfaceId>(99);
        object.components.navigationSurface->localBounds = Runtime::NavigationLocalBounds{{4, 0, 4}, {4, 1, 4}};
        const auto invalidScope = Runtime::ValidateNavigationSurfaceComponent(*object.components.navigationSurface);
        REQUIRE(invalidScope.HasError());
        CHECK(invalidScope.ErrorValue().code.Value() == Navigation::NavigationErrors::SceneComponentInvalid.code.Value());
        object.components.navigationSurface->bakeScope = Runtime::NavigationBakeScope::LocalBounds;
        REQUIRE(Runtime::ValidateNavigationSurfaceComponent(*object.components.navigationSurface).HasValue());
        object.components.navigationSources.front().surface = object.components.navigationSurface->id;
        project.Scenes(object);
        const auto retried = access.retry(root, first.Value());
        REQUIRE(retried.HasValue());
        CHECK(retried.Value() != first.Value());
        const auto recovered = Finished(host, access, root);
        INFO("Retry error domain: " << (recovered.operation->diagnostic ? recovered.operation->diagnostic->domain.Value()
                                                                        : std::string_view{}));
        INFO(
            "Retry error code: " << (recovered.operation->diagnostic ? recovered.operation->diagnostic->code.Value() : std::string_view{}));
        INFO("Retry error reason: " << (recovered.operation->diagnostic ? std::string_view{recovered.operation->diagnostic->message}
                                                                        : std::string_view{}));
        REQUIRE(recovered.operation->state == OperationState::Succeeded);
        REQUIRE(recovered.overlays);
        CHECK(access.overlays(root) == recovered.overlays);
        CHECK(recovered.overlays != original);
        CHECK(recovered.overlays->Scenes().front().sourceDigest != originalDigest);
        CHECK(original->Scenes().front().sourceDigest == originalDigest);
        for (const auto &scene : recovered.overlays->Scenes())
            CHECK(std::ranges::any_of(scene.primitives, [](const auto &primitive) {
                return primitive.surface.Value() == 99;
            }));
        CHECK(access.cancel(root, first.Value()).HasError());
        CHECK(access.retry(root, first.Value()).HasError());
    }

    TEST_CASE("A new executable host recovers the actual published cooked overlay without restarting a bake",
              "[editor][navigation][bake][publication][preparation]") {
        Project project;
        OperationStore operations{16, 32};
        const auto root = std::filesystem::canonical(project.source.directory.Path());
        std::shared_ptr<const NavigationViewportSnapshot> published;
        {
            EditorNavigationBakeHost host{operations};
            const auto access = host.Access();
            Ready(host, access, root);
            REQUIRE(access.start(root, {}).HasValue());
            const auto finished = Finished(host, access, root);
            REQUIRE(finished.operation->state == OperationState::Succeeded);
            published = finished.overlays;
            REQUIRE(published);
            REQUIRE(published->CookedFingerprint());
        }
        EditorNavigationBakeHost reopened{operations};
        const auto access = reopened.Access();
        const auto recovered = Ready(reopened, access, root);
        CHECK_FALSE(recovered.operation);
        REQUIRE(recovered.overlays);
        CHECK(recovered.overlays->CookedFingerprint() == published->CookedFingerprint());
        REQUIRE(recovered.overlays->Scenes().size() == published->Scenes().size());
        for (std::size_t i = 0; i < recovered.overlays->Scenes().size(); ++i) {
            const auto &scene = recovered.overlays->Scenes()[i];
            CHECK(scene.scope == published->Scenes()[i].scope);
            CHECK(scene.primitives.size() == published->Scenes()[i].primitives.size());
            CHECK(std::ranges::any_of(scene.primitives, [](const auto &primitive) {
                return primitive.kind == NavigationOverlayKind::Walkable && primitive.validity == NavigationOverlayValidity::Cooked;
            }));
        }
    }
}  // namespace Horo::Editor
