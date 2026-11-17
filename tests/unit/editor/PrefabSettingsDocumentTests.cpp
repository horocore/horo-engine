#include "PrefabDocumentSessionTestSupport.h"

namespace Horo::Editor {
    TEST_CASE("Accepted prefab policy bounds new document edits and saves without mutating retained source or runtime stage",
              "[editor][prefab][settings][document]") {
        PrefabDocumentSessionTestSupport::Fixture fixture;
        fixture.Rename("Unsaved source");
        const auto before = fixture.document->Snapshot();
        Prefab::PrefabProjectPolicy policy;
        policy.maximumObjectCount = 1;
        const auto limits = Prefab::PrefabLimitProfile::Create(policy);
        REQUIRE(limits.HasValue());
        const auto outcome = fixture.document->UpdateLimitProfile(limits.Value());
        CHECK(outcome.HasError());
        const auto after = fixture.document->Snapshot();
        CHECK(after.source == before.source);
        CHECK(after.sourceRevision == before.sourceRevision);
        CHECK(after.fence.owner == before.fence.owner);
        CHECK(after.fence.instance == before.fence.instance);
        CHECK(after.fence.revision == before.fence.revision);
        CHECK(after.dirty == before.dirty);
        CHECK(after.stageActive == before.stageActive);
        CHECK(fixture.document->Save(after.fence, fixture.project.mutations, fixture.project.files).HasError());
        CHECK(fixture.document->Execute(after.fence, RenamePrefabObject{{0}, "Rejected"}).HasError());
        CHECK(fixture.document->Snapshot().source == before.source);
        REQUIRE(fixture.document->UpdateLimitProfile(fixture.limits).HasValue());
        REQUIRE(fixture.document->Save(after.fence, fixture.project.mutations, fixture.project.files).HasValue());
        CHECK_FALSE(fixture.document->Snapshot().dirty);
    }
}  // namespace Horo::Editor
