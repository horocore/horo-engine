#include "../../support/SourceDocumentTestProject.h"
#include "Horo/Editor/ProposedChangeSet.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

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
