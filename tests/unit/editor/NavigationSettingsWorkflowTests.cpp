#include "AllocationProbe.h"
#include "Horo/Scene/SceneSource.h"
#include "NavigationSettingsTestSupport.h"
#include "SceneAllocationQualification.h"

#include <cstdio>
#include <thread>

using namespace Horo;
using namespace Horo::Tests::NavigationSettingsSupport;

TEST_CASE("Navigation destructive review enumerates real Scene occurrences before atomic repair", "[navigation][settings][reference]") {
    Fixture f;
    auto review = f.Removal();
    REQUIRE(review.CanCommit());
    REQUIRE(review.Impacts().size() == 4);
    REQUIRE(f.first.Snapshot().Value().objects[0].components.navigationSurface->profiles[0] == Id<N::NavigationAgentProfileId>(1));
    REQUIRE(f.workflow.Commit(review, true, {}).HasValue());
    REQUIRE(f.definition.Snapshot().Value().definition->Profiles().size() == 1);
    for (auto *scene : f.scenes) {
        const auto snapshot = scene->Snapshot().Value();
        REQUIRE(snapshot.dirty);
        REQUIRE(snapshot.objects[0].components.navigationSurface->profiles[0] == Id<N::NavigationAgentProfileId>(2));
        REQUIRE(snapshot.objects[1].components.navigationAgent->profile == Id<N::NavigationAgentProfileId>(2));
    }
    REQUIRE(f.workflow.Commit(review, true, {}).HasError());
}

TEST_CASE("Navigation coordinated history replays definition and both canonical Scene owners together", "[navigation][settings][history]") {
    Fixture f;
    auto review = f.Removal();
    REQUIRE(f.workflow.Commit(review, true, {}).HasValue());
    const auto snapshot = f.definition.Snapshot().Value();
    REQUIRE(f.definition.Replay(snapshot, false).HasError());
    auto scene = f.first.Snapshot().Value();
    REQUIRE(f.first.Replay({scene.session, scene.revision, scene.state, true, {}}, false).HasError());
    REQUIRE(f.workflow.Replay(false, {}).HasValue());
    REQUIRE(f.definition.Snapshot().Value().definition->Profiles().size() == 2);
    REQUIRE(f.first.Snapshot().Value().objects[1].components.navigationAgent->profile == Id<N::NavigationAgentProfileId>(1));
    REQUIRE(f.second.Snapshot().Value().objects[1].components.navigationAgent->profile == Id<N::NavigationAgentProfileId>(1));
    REQUIRE(f.workflow.Replay(true, {}).HasValue());
    REQUIRE(f.definition.Snapshot().Value().definition->Profiles().size() == 1);
}

TEST_CASE("Navigation reference preview refuses incomplete registry inventory and unrepaired deletion",
          "[navigation][settings][admission]") {
    Fixture f;
    SECTION("unloaded Scene") {
        const std::array<E::SceneDocumentTransactions *, 1> scenes{&f.first};
        E::NavigationSettingsWorkflow incomplete{f.registry, f.definitions, scenes};
        const std::array<E::NavigationDefinitionEdit, 1> edit{E::RemoveNavigationProfile{Id<N::NavigationAgentProfileId>(1), {}}};
        REQUIRE(incomplete.Preview(Asset(1), edit).HasError());
    }
    SECTION("missing replacement") {
        const std::array<E::NavigationDefinitionEdit, 1> edit{E::RemoveNavigationProfile{Id<N::NavigationAgentProfileId>(1), {}}};
        auto review = f.workflow.Preview(Asset(1), edit);
        REQUIRE(review.HasValue());
        REQUIRE(review.Value().Impacts().size() == 4);
        REQUIRE_FALSE(review.Value().CanCommit());
        REQUIRE(f.workflow.Commit(review.Value(), true, {}).HasError());
    }
    REQUIRE(f.definition.Snapshot().Value().revision == 1);
    REQUIRE(f.first.Snapshot().Value().revision == 0);
}

TEST_CASE("Navigation stale or closed owner fences prevent every grouped publication", "[navigation][settings][lifetime]") {
    Fixture f;
    auto review = f.Removal();
    SECTION("closed second Scene") {
        f.second.Shutdown();
    }
    SECTION("closed definition") {
        f.definition.Close();
    }
    SECTION("new registry generation") {
        auto records = f.registry.Snapshot().Records();
        REQUIRE(f.registry.Publish({records.begin(), records.end()}).status == Assets::AssetRegistryBuildStatus::Complete);
    }
    SECTION("foreign thread") {
        bool rejected{};
        std::thread worker([&] {
            rejected = f.workflow.Commit(review, true, {}).HasError();
        });
        worker.join();
        REQUIRE(rejected);
        return;
    }
    REQUIRE(f.workflow.Commit(review, true, {}).HasError());
    REQUIRE(f.first.Snapshot().Value().revision == 0);
}

TEST_CASE("Navigation second-owner validation failure preserves first owner and definition history", "[navigation][settings][rollback]") {
    Fixture f;
    auto snapshot = f.second.Snapshot().Value();
    auto desired = snapshot.objects.front();
    desired.editorState.locked = true;
    const auto digest = E::FingerprintSceneObjectContent(snapshot.objects.front(), 1024 * 1024).Value();
    const E::SceneObjectReplacement lock{desired.id, digest, {}, desired, {}};
    REQUIRE(f.second.Execute({snapshot.session, snapshot.revision, snapshot.state, true, {}}, std::span{&lock, 1}).HasValue());
    auto review = f.Removal();
    REQUIRE(f.workflow.Commit(review, true, {}).HasError());
    REQUIRE(f.first.Snapshot().Value().revision == 0);
    REQUIRE(f.definition.Snapshot().Value().revision == 1);
    REQUIRE(f.definition.Snapshot().Value().definition->Profiles().size() == 2);
}

TEST_CASE("Every navigation commit allocation failure precedes all owner publication", "[navigation][settings][allocation][rollback]") {
    Tests::RequireSupportedSceneAllocationInjection();
    std::size_t allocations{};
    {
        Fixture f;
        const auto review = f.Removal();
        bool committed{};
        {
            Tests::AllocationProbe::ScopedMeasurement measure;
            committed = f.workflow.Commit(review, true, {}).HasValue();
            allocations = measure.Snapshot().requests;
        }
        REQUIRE(committed);
    }
    REQUIRE(allocations > 10);
    std::size_t completed{};
    for (std::size_t failure = 0; failure < allocations; ++failure) {
        Fixture f;
        const auto review = f.Removal();
        bool rejected{};
        {
            Tests::AllocationProbe::ScopedFailure inject{failure};
            rejected = f.workflow.Commit(review, true, {}).HasError();
        }
        INFO("allocation position " << failure);
        REQUIRE(rejected);
        REQUIRE(f.definition.Snapshot().Value().revision == 1);
        REQUIRE(f.first.Snapshot().Value().revision == 0);
        REQUIRE(f.second.Snapshot().Value().revision == 0);
        ++completed;
    }
    std::fprintf(stderr, "[navigation-allocation] commit measured=%zu completed=%zu\n", allocations, completed);
    REQUIRE(completed == allocations);
}

TEST_CASE("Navigation area removal repairs definition filter costs and Scene modifiers", "[navigation][settings][area]") {
    Fixture f;
    const std::array<E::NavigationDefinitionEdit, 1> edit{E::RemoveNavigationArea{Id<N::NavigationAreaId>(3), Id<N::NavigationAreaId>(5)}};
    auto review = f.workflow.Preview(Asset(1), edit);
    REQUIRE(review.HasValue());
    REQUIRE(review.Value().Impacts().size() == 3);
    REQUIRE(f.workflow.Commit(review.Value(), true, {}).HasValue());
    REQUIRE(f.definition.Snapshot()
                .Value()
                .definition->Registry()
                .ResolveFilter(Id<N::NavigationFilterId>(4))
                .Value()
                ->costOverrides.front()
                .area == Id<N::NavigationAreaId>(5));
    REQUIRE(f.first.Snapshot().Value().objects[2].components.navigationModifier->area == Id<N::NavigationAreaId>(5));
}

TEST_CASE("Navigation create and rename retain identity and unrelated authored fields", "[navigation][settings][authoring]") {
    Fixture f;
    const auto before = f.definition.Snapshot().Value();
    auto profile = before.definition->Profiles().front();
    profile.displayName = "Renamed Walker";
    const std::array<E::NavigationDefinitionEdit, 2> edits{E::PutNavigationProfile{profile},
                                                           E::PutNavigationProfile{{Id<N::NavigationAgentProfileId>(7), "New"}}};
    auto review = f.workflow.Preview(Asset(1), edits);
    REQUIRE(review.HasValue());
    REQUIRE(review.Value().Impacts().empty());
    REQUIRE(f.workflow.Commit(review.Value(), true, {}).HasValue());
    REQUIRE(f.definition.Snapshot().Value().definition->Profiles().size() == 3);
    REQUIRE(f.first.Snapshot().Value().revision == 0);
    REQUIRE(f.workflow.Replay(false, {}).HasValue());
    REQUIRE(f.definition.Snapshot().Value().definition->Profiles().size() == 2);
}

TEST_CASE("Navigation grouped undo rejects independently advanced or retired scene owners", "[navigation][settings][history][lifetime]") {
    Fixture f;
    const auto review = f.Removal();
    REQUIRE(f.workflow.Commit(review, true, {}).HasValue());
    SECTION("closed owner") {
        f.second.Shutdown();
    }
    SECTION("independent owner revision") {
        const auto before = f.second.Snapshot().Value();
        auto object = before.objects.front();
        object.name = "Independent";
        const E::SceneObjectReplacement change{object.id,
                                               E::FingerprintSceneObjectContent(before.objects.front(), 1024 * 1024).Value(),
                                               {},
                                               object,
                                               {}};
        REQUIRE(f.second.Execute({before.session, before.revision, before.state, true, {}}, std::span{&change, 1}).HasValue());
    }
    const auto definitionRevision = f.definition.Snapshot().Value().revision;
    const auto firstRevision = f.first.Snapshot().Value().revision;
    REQUIRE(f.workflow.Replay(false, {}).HasError());
    REQUIRE(f.definition.Snapshot().Value().revision == definitionRevision);
    REQUIRE(f.first.Snapshot().Value().revision == firstRevision);
}

TEST_CASE("Every navigation grouped replay allocation failure preserves all owner histories",
          "[navigation][settings][allocation][history]") {
    Tests::RequireSupportedSceneAllocationInjection();
    std::size_t count{};
    {
        Fixture f;
        auto review = f.Removal();
        REQUIRE(f.workflow.Commit(review, true, {}).HasValue());
        bool success{};
        {
            Tests::AllocationProbe::ScopedMeasurement measure;
            success = f.workflow.Replay(false, {}).HasValue();
            count = measure.Snapshot().requests;
        }
        REQUIRE(success);
    }
    REQUIRE(count > 0);
    std::size_t completed{};
    for (std::size_t position = 0; position < count; ++position) {
        Fixture f;
        auto review = f.Removal();
        REQUIRE(f.workflow.Commit(review, true, {}).HasValue());
        bool failed{};
        {
            Tests::AllocationProbe::ScopedFailure fail{position};
            failed = f.workflow.Replay(false, {}).HasError();
        }
        INFO(position);
        REQUIRE(failed);
        REQUIRE(f.definition.Snapshot().Value().revision == 2);
        REQUIRE(f.first.Snapshot().Value().revision == 1);
        REQUIRE(f.second.Snapshot().Value().revision == 1);
        ++completed;
    }
    std::fprintf(stderr, "[navigation-allocation] replay measured=%zu completed=%zu\n", count, completed);
    REQUIRE(completed == count);
}

TEST_CASE("Navigation unsupported prefab owners are enumerated and prevent every destruction", "[navigation][settings][inventory]") {
    Fixture f;
    const auto records = f.registry.Snapshot().Records();
    std::vector<Assets::AssetRecord> withPrefab{records.begin(), records.end()};
    withPrefab.push_back(Record(4, "core.prefab", "assets/unsupported.prefab"));
    withPrefab.push_back(Record(5, "core.prefab", "assets/other.prefab"));
    REQUIRE(f.registry.Publish(std::move(withPrefab)).status == Assets::AssetRegistryBuildStatus::Complete);
    auto review = f.Removal();
    REQUIRE_FALSE(review.CanCommit());
    REQUIRE(review.UnsupportedOwners().size() == 2);
    REQUIRE(review.UnsupportedOwners()[0].asset == Asset(4));
    REQUIRE(review.UnsupportedOwners()[1].asset == Asset(5));
    REQUIRE(f.workflow.Commit(review, true, {}).HasError());
    REQUIRE(f.first.Snapshot().Value().revision == 0);
    REQUIRE(f.second.Snapshot().Value().revision == 0);
    REQUIRE(f.definition.Snapshot().Value().revision == 1);
    const std::array<E::NavigationDefinitionEdit, 1> create{E::PutNavigationProfile{{Id<N::NavigationAgentProfileId>(7), "New profile"}}};
    const auto safe = f.workflow.Preview(Asset(1), create);
    REQUIRE(safe.HasValue());
    REQUIRE(safe.Value().CanCommit());
}

TEST_CASE("Navigation retained reviews reject moved then destroyed or reloaded canonical owners",
          "[navigation][settings][lifetime][reload]") {
    Fixture f;
    SECTION("moved owner destroyed") {
        auto review = f.Removal();
        auto relocated = std::make_unique<E::SceneDocumentTransactions>(std::move(f.second));
        relocated.reset();
        REQUIRE(f.workflow.Commit(review, true, {}).HasError());
    }
    REQUIRE(f.first.Snapshot().Value().revision == 0);
    REQUIRE(f.definition.Snapshot().Value().revision == 1);
}

TEST_CASE("Navigation frame dirty projection does not copy Scene roots or allocate", "[navigation][settings][allocation]") {
    Fixture f;
    bool dirty{};
    std::size_t requests{};
    {
        Tests::AllocationProbe::ScopedMeasurement measure;
        dirty = f.first.IsDirty().Value() || f.second.IsDirty().Value();
        requests = measure.Snapshot().requests;
    }
    REQUIRE_FALSE(dirty);
    REQUIRE(requests == 0);
}

TEST_CASE("Navigation area and filter identity rename repairs all typed occurrences", "[navigation][settings][rename]") {
    Fixture f;
    auto input = Definition();
    auto area = input.areas.front();
    area.id = Id<N::NavigationAreaId>(7);
    auto filter = input.filters.front();
    filter.id = Id<N::NavigationFilterId>(9);
    const std::array<E::NavigationDefinitionEdit, 4> edits{E::PutNavigationArea{area}, E::PutNavigationFilter{filter},
                                                           E::RemoveNavigationFilter{Id<N::NavigationFilterId>(4), filter.id},
                                                           E::RemoveNavigationArea{Id<N::NavigationAreaId>(3), area.id}};
    auto review = f.workflow.Preview(Asset(1), edits);
    REQUIRE(review.HasValue());
    REQUIRE(review.Value().Impacts().size() == 5);
    REQUIRE(f.workflow.Commit(review.Value(), true, {}).HasValue());
    for (auto *scene : f.scenes) {
        const auto snapshot = scene->Snapshot().Value();
        REQUIRE(snapshot.objects[1].components.navigationAgent->filter == filter.id);
        REQUIRE(snapshot.objects[2].components.navigationModifier->area == area.id);
    }
    const auto definition = f.definition.Snapshot().Value().definition;
    REQUIRE(definition->Registry().ResolveFilter(filter.id).Value()->costOverrides.front().area == area.id);
    REQUIRE(definition->Registry().ResolveFilter(Id<N::NavigationFilterId>(4)).HasError());
}

TEST_CASE("Navigation cancellation before publication preserves every canonical owner", "[navigation][settings][cancellation]") {
    Fixture f;
    auto review = f.Removal();
    CancellationSource cancellation;
    cancellation.RequestCancellation();
    REQUIRE(f.workflow.Commit(review, true, cancellation.Token()).HasError());
    REQUIRE(f.first.Snapshot().Value().revision == 0);
    REQUIRE(f.second.Snapshot().Value().revision == 0);
    REQUIRE(f.definition.Snapshot().Value().revision == 1);
}

TEST_CASE("Scene decoding releases completed DOM without allocating during retirement", "[navigation][settings][allocation][scene]") {
    Tests::RequireSupportedSceneAllocationInjection();
    const std::string source = R"({"schemaVersion":1,"objects":[],"unknown":{"nested":[1,2,{"value":"preserved"}]}})";
    std::size_t count{};
    {
        Tests::AllocationProbe::ScopedMeasurement measure;
        static_cast<void>(SceneSource::DecodeSceneSource(source));
        count = measure.Snapshot().requests;
    }
    REQUIRE(count > 0);
    std::size_t completed{};
    for (std::size_t position = 0; position < count; ++position) {
        bool failed{};
        {
            Tests::AllocationProbe::ScopedFailure inject{position};
            try {
                failed = SceneSource::DecodeSceneSource(source).HasError();
            } catch (const std::bad_alloc &) {
                failed = true;
            }
        }
        INFO(position);
        REQUIRE(failed);
        ++completed;
    }
    REQUIRE(SceneSource::DecodeSceneSource(source).HasValue());
    std::fprintf(stderr, "[navigation-allocation] scene measured=%zu completed=%zu\n", count, completed);
    REQUIRE(completed == count);
}
