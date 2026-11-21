#include "../../support/SourceDocumentTestProject.h"
#include "Horo/Editor/ProposedChangeSet.h"
#include "Horo/Foundation/Platform.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <iterator>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Editor::Proposals;

    SourceDocumentId Path(std::string_view name) {
        return SourceDocumentId::Parse(name).Value();
    }

    struct Workspace final {
        TestSupport::SourceProject project;
        SourceDocumentService documents{project.directory};
        ProjectPolicy policy = ProjectPolicy::Capture("workspace", 1, {{Path("src"), true}}).Value();

        Workspace() {
            std::filesystem::create_directory(project.directory / "src");
        }

        SourceDocumentSnapshot Open(const char *name, std::string_view text, std::uint64_t instance = 1) {
            project.Put(text, name);
            return documents.Open(project.Identity(name, instance), project.directory / name).Value();
        }

        ChangeSet Build(std::vector<FileOperation> files) const {
            return ChangeSet::Capture(policy, {"proposal", 1, {"provider", "model", "request"}, std::move(files)}).Value();
        }
    };
}  // namespace

TEST_CASE("Review decisions target exact hunks and undo an entire decision batch", "[editor][proposal][review]") {
    Workspace workspace;
    const auto source = workspace.Open("src/edit.cpp", "one two\n");
    const auto path = source.Identity().key.source;
    const auto proposal = workspace.Build({EditFile{FileBase::Capture(source).Value(), {{0, "one", "first"}, {4, "two", "second"}}}});
    const std::array observations{SourcePathObservation{path, source}};
    const auto plan = PlanWorkspaceEdits(proposal, workspace.policy, observations).Value();
    ReviewDecisions decisions{proposal};
    CHECK(decisions.State().Value() == ReviewState::Pending);
    REQUIRE(decisions.Decide(plan, {0, 1}, ReviewChoice::Accept).HasValue());
    CHECK(decisions.State({0, 1}).Value() == ReviewState::Accepted);
    CHECK(decisions.State({0, 0}).Value() == ReviewState::Pending);
    CHECK(decisions.State({0}).Value() == ReviewState::Mixed);
    REQUIRE(decisions.Decide(plan, {0}, ReviewChoice::Reject).HasValue());
    CHECK(decisions.State().Value() == ReviewState::Rejected);
    REQUIRE(decisions.Undo(plan).Value());
    CHECK(decisions.State({0, 1}).Value() == ReviewState::Accepted);
    CHECK(decisions.State({0, 0}).Value() == ReviewState::Pending);
    REQUIRE(decisions.Undo(plan).Value());
    CHECK(decisions.State().Value() == ReviewState::Pending);
    CHECK_FALSE(decisions.Undo(plan).Value());
    CHECK(workspace.documents.Snapshot(source.Identity().instance).Value().SharesStateWith(source));
    CHECK(source.DiskBase() == "one two\n");
    std::ifstream disk(workspace.project.directory / "src/edit.cpp", std::ios::binary);
    CHECK((std::string{std::istreambuf_iterator<char>{disk}, {}} == "one two\n"));
}

TEST_CASE("Bulk acceptance fails atomically when any selected operation conflicts", "[editor][proposal][review]") {
    Workspace workspace;
    const auto a = workspace.Open("src/a.cpp", "one");
    const auto b = workspace.Open("src/b.cpp", "two", 2);
    const auto proposal =
        workspace.Build({EditFile{FileBase::Capture(a).Value(), {{0, "one", "first"}}}, DeleteFile{FileBase::Capture(b).Value()}});
    const std::array observations{SourcePathObservation{a.Identity().key.source, a}, SourcePathObservation{b.Identity().key.source, b}};
    const auto plan = PlanWorkspaceEdits(proposal, workspace.policy, observations).Value();
    ReviewDecisions decisions{proposal};
    CHECK(decisions.Decide(plan, {1, 0}, ReviewChoice::Reject).HasError());
    CHECK(decisions.State({1, 0}).HasError());
    REQUIRE(workspace.documents.Edit(b.Identity().instance, {b.Revision(), 0, 3, "changed"}).HasValue());
    const auto blocked = decisions.Decide(plan, {}, ReviewChoice::Accept);
    REQUIRE(blocked.HasError());
    CHECK(blocked.ErrorValue().code.Value() == Errors::StaleBase.code.Value());
    CHECK(decisions.State().Value() == ReviewState::Pending);
    REQUIRE(decisions.Decide(plan, {0}, ReviewChoice::Accept).HasValue());
    CHECK(decisions.State({0}).Value() == ReviewState::Accepted);
    REQUIRE(decisions.Decide(plan, {}, ReviewChoice::Reject).HasValue());
    CHECK(decisions.State().Value() == ReviewState::Rejected);
    CHECK(workspace.documents.Snapshot(a.Identity().instance).Value().SharesStateWith(a));
    CHECK(std::filesystem::exists(workspace.project.directory / "src/b.cpp"));
}

TEST_CASE("Review recovery rejects corruption and stale accepted units", "[editor][proposal][review]") {
    Workspace workspace;
    const auto source = workspace.Open("src/edit.cpp", "original");
    const auto path = source.Identity().key.source;
    const auto proposal = workspace.Build({EditFile{FileBase::Capture(source).Value(), {{0, "original", "replacement"}}}});
    const std::array observations{SourcePathObservation{path, source}};
    const auto plan = PlanWorkspaceEdits(proposal, workspace.policy, observations).Value();
    ReviewDecisions decisions{proposal};
    REQUIRE(decisions.Decide(plan, {}, ReviewChoice::Accept).HasValue());
    const auto accepted = decisions.Encode();
    CHECK(accepted.find("replacement") == std::string::npos);
    CHECK(accepted.find("src/edit.cpp") == std::string::npos);
    auto decoded = ReviewDecisions::Decode(proposal, plan, accepted);
    REQUIRE(decoded.HasValue());
    auto restored = std::move(decoded).Value();
    CHECK(restored.State().Value() == ReviewState::Accepted);
    CHECK(restored.Encode() == accepted);
    CHECK_FALSE(restored.Undo(plan).Value());
    auto corrupt = accepted;
    corrupt.back() = corrupt.back() == '0' ? '1' : '0';
    CHECK(ReviewDecisions::Decode(proposal, plan, corrupt).HasError());
    CHECK(ReviewDecisions::Decode(proposal, plan, accepted + "extra").HasError());
    const auto different = workspace.Build({EditFile{FileBase::Capture(source).Value(), {{0, "original", "different"}}}});
    CHECK(ReviewDecisions::Decode(different, plan, accepted).HasError());
    REQUIRE(decisions.Decide(plan, {}, ReviewChoice::Reject).HasValue());
    REQUIRE(workspace.documents.Edit(source.Identity().instance, {source.Revision(), 0, 8, "new"}).HasValue());
    CHECK(ReviewDecisions::Decode(proposal, plan, accepted).HasError());
    CHECK(ReviewDecisions::Decode(proposal, plan, decisions.Encode()).HasValue());
    CHECK(decisions.Undo(plan).HasError());
    CHECK(decisions.State().Value() == ReviewState::Rejected);
}

TEST_CASE("Applying accepted proposal hunks stages one unsaved buffer transaction", "[editor][proposal][apply]") {
    Workspace workspace;
    const auto a = workspace.Open("src/a.cpp", "one two end");
    const auto b = workspace.Open("src/b.cpp", "three", 2);
    const auto proposal = workspace.Build({
        EditFile{FileBase::Capture(a).Value(), {{0, "one", "first"}, {4, "two", "second"}, {8, "end", "last"}}},
        EditFile{FileBase::Capture(b).Value(), {{0, "three", "third"}}},
    });
    const std::array observations{SourcePathObservation{a.Identity().key.source, a}, SourcePathObservation{b.Identity().key.source, b}};
    const auto plan = PlanWorkspaceEdits(proposal, workspace.policy, observations).Value();
    ReviewDecisions decisions{proposal};
    REQUIRE(decisions.Decide(plan, {0, 0}, ReviewChoice::Accept).HasValue());
    REQUIRE(decisions.Decide(plan, {0, 1}, ReviewChoice::Reject).HasValue());
    REQUIRE(decisions.Decide(plan, {1}, ReviewChoice::Accept).HasValue());
    const auto revisedPolicy = ProjectPolicy::Capture("workspace", 2, {{Path("src"), true}}).Value();
    const auto denied = decisions.ApplyAccepted(workspace.documents, revisedPolicy, plan);
    REQUIRE(denied.HasError());
    CHECK(denied.ErrorValue().code.Value() == Errors::PolicyDenied.code.Value());
    CHECK(a.IsCurrent());
    CHECK(b.IsCurrent());

    const auto applied = decisions.ApplyAccepted(workspace.documents, workspace.policy, plan);
    REQUIRE(applied.HasValue());
    REQUIRE(applied.Value().Items().size() == 2);
    CHECK(applied.Value().Items()[0].snapshot.Text() == "first two end");
    CHECK(applied.Value().Items()[1].snapshot.Text() == "third");
    CHECK(applied.Value().Items()[0].snapshot.Dirty());
    CHECK(applied.Value().Items()[1].snapshot.Dirty());
    CHECK(decisions.ApplyAccepted(workspace.documents, workspace.policy, plan).HasError());
    const auto undone = workspace.documents.UndoBatch(applied.Value());
    REQUIRE(undone.HasValue());
    CHECK(undone.Value()[0].Text() == a.Text());
    CHECK(undone.Value()[1].Text() == b.Text());
}

TEST_CASE("Applied source review audit round-trips without recovery source or authority", "[editor][proposal][audit]") {
    Workspace workspace;
    const auto source = workspace.Open("src/private.cpp", "one two");
    const auto proposal = workspace.Build({EditFile{FileBase::Capture(source).Value(), {{0, "one", "first"}, {4, "two", "second"}}}});
    const std::array observations{SourcePathObservation{source.Identity().key.source, source}};
    const auto plan = PlanWorkspaceEdits(proposal, workspace.policy, observations).Value();
    ReviewDecisions decisions{proposal};
    REQUIRE(decisions.Decide(plan, {0, 0}, ReviewChoice::Accept).HasValue());
    REQUIRE(decisions.Decide(plan, {0, 1}, ReviewChoice::Reject).HasValue());
    auto prepared = decisions.PrepareAccepted(workspace.policy, plan);
    REQUIRE(prepared.HasValue());
    auto candidate = std::move(prepared).Value();
    NativeDurableFileSystem files;
    REQUIRE(candidate.ObserveDisk(workspace.project.directory, files).HasValue());
    const auto applied = candidate.CommitObserved(workspace.documents, files);
    REQUIRE(applied.HasValue());

    const auto encoded = EncodeSourceReviewAudit(*applied.Value().audit);
    REQUIRE(encoded.HasValue());
    for (const auto *privateValue : {"src/private.cpp", "one two", "first", "second", "provider"})
        CHECK(encoded.Value().find(privateValue) == std::string::npos);
    auto restored = DecodeSourceReviewAudit(encoded.Value());
    REQUIRE(restored.HasValue());
    CHECK(restored.Value().proposal.digest == proposal.Digest());
    CHECK(restored.Value().proposal.policy == proposal.PolicyDigest());
    CHECK(restored.Value().decisionDigest == applied.Value().audit->decisionDigest);
    CHECK(restored.Value().choices == applied.Value().audit->choices);
    REQUIRE(restored.Value().revisions.size() == 1);
    CHECK(restored.Value().revisions[0].file == 0);
    CHECK(restored.Value().revisions[0].before == source.Revision());
    CHECK(restored.Value().revisions[0].after == applied.Value().batch.Items()[0].snapshot.Revision());
    CHECK(EncodeSourceReviewAudit(restored.Value()).Value() == encoded.Value());
    CHECK(DecodeAudit(encoded.Value()).HasError());

    auto corrupt = encoded.Value();
    const auto digest = FormatSha256(restored.Value().decisionDigest);
    const auto committedValue = corrupt.find(digest);
    REQUIRE(committedValue != std::string::npos);
    auto &lastDigit = corrupt[committedValue + digest.size() - 1];
    lastDigit = lastDigit == '0' ? '1' : '0';
    CHECK(DecodeSourceReviewAudit(corrupt).HasError());
    CHECK(DecodeSourceReviewAudit(std::string(MaximumSourceReviewAuditBytes + 1, ' ')).HasError());
    auto mismatched = std::move(restored).Value();
    mismatched.revisions[0].file = 1;
    CHECK(EncodeSourceReviewAudit(mismatched).HasError());
    CHECK(workspace.documents.Snapshot(source.Identity().instance).Value().Text() == "first two");
}

TEST_CASE("Workspace plans preserve exact text and use canonical operation order", "[editor][proposal][plan]") {
    Workspace workspace;
    const std::string_view original = "\xef\xbb\xbf"
                                      "hello\r\nworld\r\n";
    const auto edit = workspace.Open("src/edit.cpp", original);
    const auto old = workspace.Open("src/old.cpp", "old\n", 2);
    const auto removed = workspace.Open("src/removed.cpp", "delete\n", 3);
    const auto createdPath = Path("src/created.cpp");
    const auto renamedPath = Path("src/renamed.cpp");
    const auto proposal = workspace.Build({
        DeleteFile{FileBase::Capture(removed).Value()},
        RenameFile{FileBase::Capture(old).Value(), renamedPath, {{0, "old", "renamed"}}},
        EditFile{FileBase::Capture(edit).Value(), {{10, "world", "wide world"}, {3, "hello", "hi"}}},
        CreateFile{createdPath, ""},
    });
    std::array observations{
        SourcePathObservation{renamedPath, SourcePathState::Absent}, SourcePathObservation{Path("src/edit.cpp"), edit},
        SourcePathObservation{createdPath, SourcePathState::Absent}, SourcePathObservation{Path("src/old.cpp"), old},
        SourcePathObservation{Path("src/removed.cpp"), removed},
    };
    const auto plan = PlanWorkspaceEdits(proposal, workspace.policy, observations);
    REQUIRE(plan.HasValue());
    CHECK(plan.Value().proposal == proposal.Digest());
    const auto &entries = plan.Value().entries;
    REQUIRE(entries.size() == 4);
    CHECK(std::get<FilePreview>(entries[0]).before.Revision() == 0);
    CHECK(std::get<FilePreview>(entries[0]).after == "");
    CHECK(std::get<FilePreview>(entries[1]).before.SharesStateWith(edit));
    CHECK(std::get<FilePreview>(entries[1]).after == "\xef\xbb\xbf"
                                                     "hi\r\nwide world\r\n");
    CHECK(std::get<FilePreview>(entries[2]).after == "renamed\n");
    CHECK_FALSE(std::get<FilePreview>(entries[3]).after.has_value());
    std::ranges::reverse(observations);
    const auto reorder = PlanWorkspaceEdits(proposal, workspace.policy, observations);
    REQUIRE(reorder.HasValue());
    for (std::size_t index = 0; index != entries.size(); ++index)
        CHECK(std::get<FilePreview>(entries[index]).after == std::get<FilePreview>(reorder.Value().entries[index]).after);
    CHECK(workspace.documents.Snapshot(edit.Identity().instance).Value().SharesStateWith(edit));
    CHECK(edit.Text() == original);
    CHECK(edit.DiskBase() == original);
    CHECK_FALSE(std::filesystem::exists(workspace.project.directory / createdPath.Value()));
    CHECK_FALSE(std::filesystem::exists(workspace.project.directory / renamedPath.Value()));
    CHECK(std::filesystem::exists(workspace.project.directory / "src/removed.cpp"));
}

TEST_CASE("Workspace plans distinguish stale, missing, relocated and external sources", "[editor][proposal][plan]") {
    Workspace workspace;
    const auto before = workspace.Open("src/edit.cpp", "old\n");
    const auto instance = before.Identity().instance;
    const auto proposal = workspace.Build({EditFile{FileBase::Capture(before).Value(), {{0, "old", "new"}}}});
    std::vector observations{SourcePathObservation{Path("src/edit.cpp"), before}};
    auto expected = PlanConflictReason::SourceStale;
    SECTION("local edits supersede a retained lease") {
        REQUIRE(workspace.documents.Edit(instance, {before.Revision(), 0, 3, "authored"}).HasValue());
    }
    SECTION("a valid newer publication still fails the original base fence") {
        observations[0].evidence = workspace.documents.Edit(instance, {before.Revision(), 0, 3, "authored"}).Value();
    }
    SECTION("external changes are explicit conflicts") {
        workspace.project.Put("external\n", "src/edit.cpp");
        observations[0].evidence = workspace.documents.InspectDisk(instance).Value();
        expected = PlanConflictReason::SourceChangedOnDisk;
    }
    SECTION("observed removals preserve resident text") {
        REQUIRE(std::filesystem::remove(workspace.project.directory / "src/edit.cpp"));
        REQUIRE(workspace.documents.InspectDisk(instance).HasError());
        observations[0].evidence = workspace.documents.Snapshot(instance).Value();
        expected = PlanConflictReason::SourceMissing;
    }
    SECTION("unreadable files do not appear as clean previews") {
        workspace.project.Put(std::string_view{"new\0", 4}, "src/edit.cpp");
        REQUIRE(workspace.documents.InspectDisk(instance).HasError());
        observations[0].evidence = workspace.documents.Snapshot(instance).Value();
        expected = PlanConflictReason::SourceUnavailable;
    }
    SECTION("an exact host relocation observation prevents rebasing onto another path") {
        observations[0].evidence = SourcePathState::Moved;
        expected = PlanConflictReason::SourceMoved;
    }
    SECTION("a missing observation cannot silently open a document") {
        observations.clear();
        expected = PlanConflictReason::Unobserved;
    }
    const auto current = workspace.documents.Snapshot(instance).Value();
    const auto plan = PlanWorkspaceEdits(proposal, workspace.policy, observations);
    REQUIRE(plan.HasValue());
    const auto &conflict = std::get<PlanConflict>(plan.Value().entries.front());
    CHECK(conflict.reason == expected);
    CHECK(conflict.path == Path("src/edit.cpp"));
    CHECK(workspace.documents.Snapshot(instance).Value().SharesStateWith(current));
}

TEST_CASE("Workspace plans reject ambiguous evidence and conflicting destinations", "[editor][proposal][plan]") {
    Workspace workspace;
    const auto before = workspace.Open("src/old.cpp", "old");
    const auto old = Path("src/old.cpp");
    const auto destination = Path("src/new.cpp");
    const auto rename = workspace.Build({RenameFile{FileBase::Capture(before).Value(), destination, {}}});
    std::array observations{SourcePathObservation{old, before}, SourcePathObservation{destination, SourcePathState::Absent}};
    SECTION("an observed portable alias occupies a destination") {
        observations[1] = {Path("src/New.cpp"), SourcePathState::Occupied};
        const auto plan = PlanWorkspaceEdits(rename, workspace.policy, observations);
        REQUIRE(plan.HasValue());
        CHECK(std::get<PlanConflict>(plan.Value().entries.front()).reason == PlanConflictReason::DestinationOccupied);
    }
    SECTION("canonical aliases cannot supply competing observations") {
        observations[1] = {Path("src/Old.cpp"), SourcePathState::Occupied};
        CHECK(PlanWorkspaceEdits(rename, workspace.policy, observations).ErrorValue().code.Value() == Errors::Invalid.code.Value());
    }
    SECTION("file operands cannot also be ancestors of other operands") {
        const auto nested = Path("src/new.cpp/nested.txt");
        const auto overlap = workspace.Build({CreateFile{destination, ""}, CreateFile{nested, ""}});
        const auto plan = PlanWorkspaceEdits(overlap, workspace.policy, {});
        REQUIRE(plan.HasValue());
        REQUIRE(plan.Value().entries.size() == 2);
        for (const auto &entry : plan.Value().entries)
            CHECK(std::get<PlanConflict>(entry).reason == PlanConflictReason::OverlappingPaths);
    }
    SECTION("a newer project policy invalidates the plan") {
        const auto policy = ProjectPolicy::Capture("workspace", 2, {{Path("src"), true}}).Value();
        CHECK(PlanWorkspaceEdits(rename, policy, observations).ErrorValue().code.Value() == Errors::PolicyDenied.code.Value());
    }
}

TEST_CASE("Workspace plans enforce aggregate source and preview reservations", "[editor][proposal][plan]") {
    Workspace workspace;
    const std::array paths{Path("src/a.cpp"), Path("src/b.cpp")};
    Limits limits;
    limits.maximumTextBytes = 8;
    limits.maximumTotalTextBytes = 12;
    SECTION("source hashing is bounded before building previews") {
        const auto a = workspace.Open("src/a.cpp", "abcdef");
        const auto b = workspace.Open("src/b.cpp", "abcdef", 2);
        const auto proposal = workspace.Build({DeleteFile{FileBase::Capture(a).Value()}, DeleteFile{FileBase::Capture(b).Value()}});
        const std::array observations{SourcePathObservation{paths[0], a}, SourcePathObservation{paths[1], b}};
        CHECK(PlanWorkspaceEdits(proposal, workspace.policy, observations, limits).ErrorValue().code.Value() ==
              Errors::Bounds.code.Value());
    }
    SECTION("the preview cap covers all result strings") {
        const auto proposal = workspace.Build({CreateFile{paths[0], "abcdefgh"}, CreateFile{paths[1], "abcdefgh"}});
        const std::array observations{SourcePathObservation{paths[0], SourcePathState::Absent},
                                      SourcePathObservation{paths[1], SourcePathState::Absent}};
        CHECK(PlanWorkspaceEdits(proposal, workspace.policy, observations, limits).ErrorValue().code.Value() ==
              Errors::Bounds.code.Value());
    }
}
