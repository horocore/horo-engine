#include "AllocationProbe.h"
#include "CollisionSettingsTestSupport.h"
#include "Horo/Foundation/PathUtils.h"
#include "SceneAllocationQualification.h"

#include <cstdio>
#include <thread>

namespace Horo::Tests::CollisionSettings {
    TEST_CASE("Collision session rename commit undo redo save keeps stable IDs and prior captures immutable", "[collision][session]") {
        Project project;
        auto session = project.Open();
        const auto before = session->Snapshot().Value();
        const auto oldCapture = before->Capture(11).Value();
        auto input = before->Input();
        input.profiles.front().name = "Renamed";
        Commit(*session, input);
        CHECK(session->IsDirty());
        CHECK(session->Snapshot().Value()->Input().profiles.front().id == before->Input().profiles.front().id);
        REQUIRE(session->Replay(false).HasValue());
        CHECK_FALSE(session->IsDirty());
        REQUIRE(session->Replay(true).HasValue());
        CHECK(session->Snapshot().Value()->Input().profiles.front().name == "Renamed");
        const auto saved = session->Save();
        REQUIRE(saved.HasValue());
        CHECK_FALSE(saved.Value().durabilityError);
        CHECK_FALSE(session->IsDirty());
        CHECK(project.store.Load().Value().settings->Input().revision == 2);
        CHECK(oldCapture.schema == before->Schema());
        CHECK(oldCapture.generation == 11);
        CHECK(oldCapture.schema->SemanticFingerprint() == saved.Value().snapshot.settings->Schema()->SemanticFingerprint());
    }

    TEST_CASE("Collision deletion uses actual complete inventory and fails closed after degraded publication", "[collision][inventory]") {
        Project project;
        auto session = project.Open();
        auto review = session->Preview(RemoveSpare(session->Snapshot().Value()->Input()));
        REQUIRE(review.HasValue());
        REQUIRE(review.Value().CanCommit());
        REQUIRE(session->ObserveInventory({project.inventory.revision, Assets::AssetRegistryBuildStatus::Degraded}).HasValue());
        CHECK(session->Commit(review.Value()).HasError());
        auto degraded = session->Preview(RemoveSpare(session->Snapshot().Value()->Input()));
        REQUIRE(degraded.HasValue());
        CHECK_FALSE(degraded.Value().CanCommit());
        REQUIRE(session->ObserveInventory(project.inventory).HasValue());
        Commit(*session, RemoveSpare(session->Snapshot().Value()->Input()));
        CHECK(session->Snapshot().Value()->Schema()->Profiles().size() == 1);
    }

    TEST_CASE("Collision source registry and canonical Scene fences reject stale reviewed candidates", "[collision][fences]") {
        Project project;
        auto session = project.Open();
        const auto before = session->Snapshot().Value();
        auto input = before->Input();
        input.layers.front().name = "Next";
        auto review = session->Preview(input);
        REQUIRE(review.HasValue());
        REQUIRE(review.Value().CanCommit());
        SECTION("physical source changes") {
            Write(project.root / ".horo/collision.json", "{}");
        }
        SECTION("registry publication changes") {
            project.Rebuild();
        }
        SECTION("new metadata changes inventory") {
            Write(project.root / "Assets/new.horoasset", "opaque");
            Write(project.root / "Assets/new.horoasset.horo",
                  R"({"schemaVersion":1,"assetId":"20213243-5465-7687-98a9-bacbdcedfe0f","assetType":"custom.owner"})");
            project.Rebuild();
        }
        CHECK(session->Commit(review.Value()).HasError());
        CHECK(session->Snapshot().Value() == before);
        CHECK_FALSE(session->IsDirty());
    }

    TEST_CASE("Collision active and unknown runtime evidence block semantics while preserving display rename", "[collision][activation]") {
        Project project;
        project.activation = {.revision = 2,
                              .state = Editor::CollisionActivationState::Active,
                              .projectId = "collision-project",
                              .documentRevision = 1,
                              .generation = 8,
                              .fingerprint = project.store.Load().Value().settings->Schema()->SemanticFingerprint()};
        auto session = project.Open();
        auto input = session->Snapshot().Value()->Input();
        input.layers.front().name = "Display only";
        Commit(*session, input);
        input.schema.pairs.front().response = Physics::SimulationPairResponse::Ignore;
        const auto active = session->Preview(input);
        REQUIRE(active.HasValue());
        CHECK_FALSE(active.Value().CanCommit());
        REQUIRE(session->ObserveActivation({.revision = 3, .state = Editor::CollisionActivationState::Unknown}).HasValue());
        const auto unknown = session->Preview(input);
        REQUIRE(unknown.HasValue());
        CHECK_FALSE(unknown.Value().CanCommit());
        CHECK(session->IsDirty());
        REQUIRE(session->ObserveActivation({.revision = 4, .state = Editor::CollisionActivationState::Inactive}).HasValue());
        Commit(*session, input);
    }

    TEST_CASE("Collision sessions revoke old weak handles and reject foreign-thread operations", "[collision][lifetime]") {
        Project project;
        auto session = project.Open();
        std::weak_ptr<Editor::CollisionProjectSettingsSession> old = session;
        bool foreignRejected{};
        std::jthread worker([session, &foreignRejected] {
            foreignRejected = session->Snapshot().HasError();
        });
        worker.join();
        CHECK(foreignRejected);
        session->Close();
        CHECK(session->Snapshot().HasError());
        auto replacement = project.Open();
        CHECK(replacement != old.lock());
        CHECK(old.lock()->Snapshot().HasError());
        session.reset();
        CHECK(old.expired());
        CHECK(replacement->Snapshot().HasValue());
    }

    TEST_CASE("Collision unchanged cached snapshot access allocates nothing and performs no file reads", "[collision][projection]") {
        Project project;
        auto session = project.Open();
        const auto before = session->Snapshot().Value();
        Write(project.root / ".horo/collision.json", "invalid external bytes");
        bool same{};
        AllocationProbe::Measurement measured;
        {
            AllocationProbe::ScopedMeasurement probe;
            same = true;
            for (int frame = 0; frame < 100; ++frame)
                same &= session->Snapshot().Value() == before;
            measured = probe.Snapshot();
        }
        CHECK(same);
        CHECK(measured.requests == 0);
        CHECK(session->Preview(before->Input()).HasError());
    }

    TEST_CASE("Collision session opening recovers after every one-shot allocation failure", "[collision][allocation][session]") {
        RequireSupportedSceneAllocationInjection();
        Project project;
        const Editor::CollisionProjectSettingsOwners owners{project.registry, project.files, project.mutations, project.inventory,
                                                            *project.source};
        std::size_t allocations{};
        auto baseline = [&] {
            std::string projectId{"collision-project"};
            AllocationProbe::ScopedMeasurement measure;
            auto opened = Editor::CollisionProjectSettingsSession::Open(project.root, std::move(projectId), owners, project.activation);
            allocations = measure.Snapshot().requests;
            return opened;
        }();
        REQUIRE(baseline.HasValue());
        REQUIRE(allocations > 0);
        const auto opening = baseline.Value()->Snapshot().Value();
        baseline.Value()->Close();
        for (std::size_t position = 0; position < allocations; ++position) {
            INFO(position);
            std::string projectId{"collision-project"};
            auto rejected = [&] {
                AllocationProbe::ScopedFailure fault{position};
                return Editor::CollisionProjectSettingsSession::Open(project.root, std::move(projectId), owners, project.activation);
            }();
            REQUIRE(rejected.HasError());
            auto recovered = project.Open();
            REQUIRE(recovered->Snapshot().HasValue());
            CHECK(recovered->Snapshot().Value()->Schema()->SemanticFingerprint() == opening->Schema()->SemanticFingerprint());
            CHECK_FALSE(recovered->IsDirty());
        }
    }

    TEST_CASE("Collision path admission propagates every allocation failure before owner publication", "[collision][allocation][paths]") {
        RequireSupportedSceneAllocationInjection();
        Project project;
        const auto destination = project.root / ".horo/collision.json";
        std::size_t requests{};
        bool contained{};
        {
            AllocationProbe::ScopedMeasurement measure;
            contained = Foundation::Paths::HasPathPrefix(project.root, destination);
            requests = measure.Snapshot().requests;
        }
        REQUIRE(contained);
        REQUIRE(requests > 0);
        std::size_t completed{};
        for (std::size_t position = 0; position < requests; ++position) {
            INFO(position);
            bool rejected{};
            try {
                AllocationProbe::ScopedFailure fault{position};
                static_cast<void>(Foundation::Paths::HasPathPrefix(project.root, destination));
            } catch (const std::bad_alloc &) {
                rejected = true;
            }
            REQUIRE(rejected);
            ++completed;
        }
        CHECK(Foundation::Paths::HasPathPrefix(project.root, project.root / "not created/child"));
        CHECK_FALSE(Foundation::Paths::HasPathPrefix(project.root, project.root.parent_path() / "outside"));
        std::fprintf(stderr, "[collision-allocation] paths measured=%zu completed=%zu\n", requests, completed);
        REQUIRE(completed == requests);
    }

    TEST_CASE("Collision history publication exhaustively rolls back every one-shot allocation failure", "[collision][allocation]") {
        RequireSupportedSceneAllocationInjection();
        std::size_t allocations{};
        {
            Project project;
            project.Scene(SceneSource::EncodeSceneSource({{}, {}}));
            std::filesystem::create_directories(project.root / "Assets/nested/deep/empty");
            auto session = project.Open();
            auto input = session->Snapshot().Value()->Input();
            input.layers.front().name = "Candidate";
            auto review = session->Preview(input);
            REQUIRE(review.HasValue());
            AllocationProbe::ScopedMeasurement measure;
            auto committed = session->Commit(review.Value());
            allocations = measure.Snapshot().requests;
            REQUIRE(committed.HasValue());
        }
        REQUIRE(allocations > 0);
        std::size_t completed{};
        for (std::size_t failure = 0; failure < allocations; ++failure) {
            INFO(failure);
            Project project;
            project.Scene(SceneSource::EncodeSceneSource({{}, {}}));
            std::filesystem::create_directories(project.root / "Assets/nested/deep/empty");
            auto session = project.Open();
            const auto before = session->Snapshot().Value();
            auto input = before->Input();
            input.layers.front().name = "Candidate";
            auto review = session->Preview(input);
            REQUIRE(review.HasValue());
            auto committed = [&] {
                AllocationProbe::ScopedFailure fault{failure};
                return session->Commit(review.Value());
            }();
            REQUIRE(committed.HasError());
            CHECK(session->Snapshot().Value() == before);
            CHECK_FALSE(session->IsDirty());
            CHECK(session->Replay(false).HasError());
            ++completed;
        }
        std::fprintf(stderr, "[collision-allocation] commit measured=%zu completed=%zu\n", allocations, completed);
        REQUIRE(completed == allocations);
    }
}  // namespace Horo::Tests::CollisionSettings
