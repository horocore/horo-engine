#include "../../support/SourceDocumentTestProject.h"
#include "Horo/Foundation/Platform.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"
#include "editor/source_editor/SourceReviewApplication.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Editor::Proposals;

    struct ReviewWorkspace final {
        TestSupport::SourceProject project;
        SourceDocumentService documents{project.directory};
        DocumentIdentity a = project.Identity("a.cpp", 1);
        DocumentIdentity b = project.Identity("b.cpp", 2);
        ProjectPolicy policy = ProjectPolicy::Capture("workspace", 1, {{a.key.source, false}, {b.key.source, false}}).Value();
        SourceReviewTransactions reviews;
        NativeDurableFileSystem files;
        JobSystem jobs{JobSystemConfig{.workerCount = 1, .ioWorkerCount = 1}};
        CancellationSource lifetime;

        ReviewWorkspace() {
            project.Put("base", "a.cpp");
            project.Put("base", "b.cpp");
            REQUIRE(documents.Open(a, project.directory / "a.cpp").HasValue());
            REQUIRE(documents.Open(b, project.directory / "b.cpp").HasValue());
        }

        /** @brief Captures a second independent accepted proposal against the currently visible source leases. */
        EditorWorkspaceViewCommandData Queue(const std::string &id, const std::string &text) {
            const auto first = documents.Snapshot(a.instance).Value();
            const auto second = documents.Snapshot(b.instance).Value();
            const auto proposal =
                ChangeSet::Capture(policy, {id,
                                            1,
                                            {"provider", "model", "request"},
                                            {EditFile{FileBase::Capture(first).Value(), {{0, std::string{first.Text()}, text}}},
                                             EditFile{FileBase::Capture(second).Value(), {{0, std::string{second.Text()}, text}}}}})
                    .Value();
            const std::array observations{SourcePathObservation{a.key.source, first}, SourcePathObservation{b.key.source, second}};
            const auto planned = PlanWorkspaceEdits(proposal, policy, observations);
            REQUIRE(planned.HasValue());
            const auto plan = std::make_shared<const WorkspaceEditPlan>(planned.Value());
            ReviewDecisions decisions{proposal};
            REQUIRE(decisions.Decide(*plan, {}, ReviewChoice::Accept).HasValue());
            EditorWorkspaceViewCommandData command;
            REQUIRE(QueueSourceReviewApplication(policy.Project(), decisions, policy, plan, lifetime.Token(), command));
            CHECK_FALSE(QueueSourceReviewApplication(policy.Project(), decisions, policy, plan, lifetime.Token(), command));
            return command;
        }

        /** @brief Reads both roots at the same owner-lane checkpoint. */
        auto Snapshots() {
            return std::array{documents.Snapshot(a.instance).Value(), documents.Snapshot(b.instance).Value()};
        }

        /** @brief Drains the exact worker publication with a finite fixture deadline. */
        void Drain() {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (reviews.Status().feedback == SourceTransactionFeedback::Preparing && std::chrono::steady_clock::now() < deadline) {
                static_cast<void>(reviews.Poll(documents, &files));
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            REQUIRE(reviews.Status().feedback != SourceTransactionFeedback::Preparing);
        }

        /** @brief Dispatches through the same authority used to verify publication. */
        bool Process(const EditorWorkspaceViewCommandData &command) {
            return reviews.Process(command, documents, &jobs, project.directory, &files);
        }
    };

    TEST_CASE("Overlapping review applications and group undo produce deterministic revisions", "[editor][source][review][concurrency]") {
        ReviewWorkspace workspace;
        constexpr std::uint64_t rounds = 24;
        for (std::uint64_t index = 0; index < rounds; ++index) {
            const auto alpha = workspace.Queue("alpha", "alpha");
            const auto beta = workspace.Queue("beta", "beta");
            const bool alphaFirst = index % 2 == 0;
            REQUIRE(workspace.Process(alphaFirst ? alpha : beta));
            workspace.Drain();
            REQUIRE(workspace.reviews.Status().feedback == SourceTransactionFeedback::Applied);
            const auto applied = workspace.Snapshots();
            CHECK(applied[0].Text() == (alphaFirst ? "alpha" : "beta"));
            CHECK(applied[0].Text() == applied[1].Text());
            CHECK(applied[0].Revision() == 2 + 2 * index);
            CHECK(applied[0].Revision() == applied[1].Revision());
            REQUIRE(workspace.Process(alphaFirst ? beta : alpha));
            workspace.Drain();
            CHECK(workspace.reviews.Status().feedback == SourceTransactionFeedback::Conflict);
            CHECK(applied[0].IsCurrent());
            CHECK(applied[1].IsCurrent());
            EditorWorkspaceViewCommandData undo;
            REQUIRE(workspace.reviews.QueueUndo(workspace.lifetime.Token(), undo));
            REQUIRE(workspace.Process(undo));
            CHECK(workspace.reviews.Status().feedback == SourceTransactionFeedback::Undone);
            const auto restored = workspace.Snapshots();
            CHECK(restored[0].Text() == "base");
            CHECK(restored[1].Text() == "base");
            CHECK(restored[0].Revision() == 3 + 2 * index);
            CHECK(restored[0].Revision() == restored[1].Revision());
        }
    }

    TEST_CASE("Source edits and retired workspace requests preserve all live documents", "[editor][source][review][concurrency]") {
        ReviewWorkspace workspace;
        const auto command = workspace.Queue("proposal", "agent");
        auto before = workspace.Snapshots();
        auto expected = SourceTransactionFeedback::Conflict;
        SECTION("A newer user edit") {
            REQUIRE(workspace.documents.Edit(workspace.b.instance, {1, 0, 4, "user"}).HasValue());
            before = workspace.Snapshots();
            CHECK(before[1].Text() == "user");
        }
        SECTION("Newer disk bytes before the next watcher observation") {
            workspace.project.Put("external", "b.cpp");
            REQUIRE(before[1].IsCurrent());
        }
        SECTION("The workspace is retired after the user queued Apply") {
            workspace.lifetime.RequestCancellation();
            expected = SourceTransactionFeedback::Cancelled;
        }
        REQUIRE(workspace.Process(command));
        workspace.Drain();
        CHECK(workspace.reviews.Status().feedback == expected);
        CHECK_FALSE(workspace.reviews.Status().canUndo);
        CHECK(before[0].IsCurrent());
        CHECK(before[1].IsCurrent());
    }

    TEST_CASE("The editor keeps source ownership during background review preparation", "[editor][source][review][concurrency]") {
        ReviewWorkspace workspace;
        const auto command = workspace.Queue("proposal", "agent");
        const auto before = workspace.Snapshots();
        REQUIRE(workspace.Process(command));
        REQUIRE(workspace.reviews.Status().feedback == SourceTransactionFeedback::Preparing);
        CHECK(before[0].IsCurrent());
        CHECK(before[1].IsCurrent());
        auto expected = SourceTransactionFeedback::Conflict;
        SECTION("A user edit arrives before publication") {
            REQUIRE(workspace.documents.Edit(workspace.b.instance, {1, 0, 4, "user"}).HasValue());
        }
        SECTION("The workspace is retired before publication") {
            workspace.lifetime.RequestCancellation();
            expected = SourceTransactionFeedback::Cancelled;
        }
        workspace.Drain();
        CHECK(workspace.reviews.Status().feedback == expected);
        CHECK(before[0].IsCurrent());
        CHECK(workspace.Snapshots()[1].Text() == (expected == SourceTransactionFeedback::Conflict ? "user" : "base"));
    }

    TEST_CASE("Source publication checks worker evidence after a same-size external rewrite", "[editor][source][review][concurrency]") {
        ReviewWorkspace workspace;
        const auto before = workspace.Snapshots();
        const auto evidence = SourceBatchDiskEvidence::Capture(workspace.project.directory, before, workspace.files);
        REQUIRE(evidence.HasValue());
        const std::array edits{SourceBatchEdit{before[0], {1, 0, 4, "new A"}}, SourceBatchEdit{before[1], {1, 0, 4, "new B"}}};
        const auto path = workspace.project.directory / "b.cpp";
        const auto previousTime = std::filesystem::last_write_time(path);
        workspace.project.Put("disk", "b.cpp");
        std::filesystem::last_write_time(path, previousTime);
        REQUIRE(std::filesystem::last_write_time(path) == previousTime);
        const auto applied = workspace.documents.EditBatchWithEvidence(edits, evidence.Value(), workspace.files);
        REQUIRE(applied.HasError());
        CHECK(applied.ErrorValue().code.Value() == SourceDocumentErrors::SaveConflict.code.Value());
        CHECK(before[0].IsCurrent());
        CHECK(before[1].IsCurrent());
    }
}  // namespace
