#include "../../../apps/HoroEditor/app/EditorLocalAgentPatch.h"
#include "../../support/NativePublicationFiles.h"
#include "../../support/SourceDocumentTestProject.h"
#include "Horo/Application/GameplayTestService.h"
#include "Horo/Editor/SourceFileOpenService.h"
#include "Horo/Foundation/BuildOutputStore.h"
#include "Horo/Foundation/OperationStore.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Platform/ExternalProcess.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"
#include "editor/source_editor/SourceReviewApplication.h"
#include "editor/source_editor/SourceReviewHistory.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iterator>
#include <mutex>
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

    TEST_CASE("Applied source validation follows exact changed buffers and retires after edits or undo", "[editor][source][review]") {
        ReviewWorkspace workspace;
        const auto before = workspace.Snapshots();
        CHECK_FALSE(workspace.reviews.IsAppliedSource(before[0]));
        REQUIRE(workspace.Process(workspace.Queue("validation", "changed")));
        workspace.Drain();
        const auto applied = workspace.Snapshots();
        REQUIRE(workspace.reviews.Status().feedback == SourceTransactionFeedback::Applied);
        CHECK_FALSE(workspace.reviews.IsAppliedSource(before[0]));
        CHECK(workspace.reviews.IsAppliedSource(applied[0]));
        CHECK(workspace.reviews.IsAppliedSource(applied[1]));
        CHECK(applied[0].Dirty());
        CHECK(applied[1].Dirty());

        SECTION("Editing one buffer retires only its validation receipt") {
            const auto edited = workspace.documents.Edit(workspace.a.instance, {applied[0].Revision(), 0, 1, "C"});
            REQUIRE(edited.HasValue());
            CHECK_FALSE(workspace.reviews.IsAppliedSource(applied[0]));
            CHECK_FALSE(workspace.reviews.IsAppliedSource(edited.Value()));
            CHECK(workspace.reviews.IsAppliedSource(applied[1]));
        }
        SECTION("Group undo retires all applied validation receipts") {
            EditorWorkspaceViewCommandData undo;
            REQUIRE(workspace.reviews.QueueUndo(workspace.lifetime.Token(), undo));
            REQUIRE(workspace.Process(undo));
            CHECK_FALSE(workspace.reviews.IsAppliedSource(applied[0]));
            CHECK_FALSE(workspace.reviews.IsAppliedSource(workspace.Snapshots()[0]));
        }
    }

    TEST_CASE("Redacted source review history survives reopening without restoring edit authority", "[editor][source][review][history]") {
        ReviewWorkspace workspace;
        ProjectMutationCoordinator mutations{workspace.files};
        const auto command = workspace.Queue("private-provider-label", "agent replacement");
        REQUIRE(workspace.Process(command));
        workspace.Drain();
        REQUIRE(workspace.reviews.Status().feedback == SourceTransactionFeedback::Applied);
        const auto audit = workspace.reviews.Status().audit;
        REQUIRE(audit);
        workspace.reviews.RecordAppliedAudit(&workspace.jobs, workspace.project.directory, &mutations, &workspace.files);
        REQUIRE(workspace.reviews.Status().writingAudit);
        const auto waitForHistory = [&workspace, &mutations](SourceReviewTransactions &owner, const auto &pending) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (pending(owner.Status()) && std::chrono::steady_clock::now() < deadline) {
                owner.UpdateAuditHistory(&workspace.jobs, workspace.project.directory, &mutations, &workspace.files);
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            REQUIRE_FALSE(pending(owner.Status()));
        };
        waitForHistory(workspace.reviews, [](const auto &state) {
            return state.writingAudit;
        });
        CHECK_FALSE(workspace.reviews.Status().auditHistoryError);

        auto expected = SourceReviewDisposition::Stale;
        SECTION("A history entry can survive an interrupted unsaved application") {
            CHECK(workspace.reviews.Status().disposition == SourceReviewDisposition::Applied);
        }
        SECTION("Durable source saves advance the same historical transaction") {
            REQUIRE(workspace.documents.SaveAll(workspace.files).HasValue());
            expected = SourceReviewDisposition::Saved;
        }
        SECTION("Explicit group undo advances the same historical transaction") {
            EditorWorkspaceViewCommandData revert;
            REQUIRE(workspace.reviews.QueueUndo(workspace.lifetime.Token(), revert));
            REQUIRE(workspace.Process(revert));
            expected = SourceReviewDisposition::Reverted;
        }
        REQUIRE(workspace.reviews.DrainAuditHistory(&workspace.jobs, workspace.project.directory, &mutations, &workspace.files));
        CHECK_FALSE(workspace.reviews.Status().auditHistoryError);

        SourceReviewTransactions reopened;
        waitForHistory(reopened, [](const auto &state) {
            return !state.audit && !state.auditHistoryError;
        });
        const auto historical = reopened.Status();
        REQUIRE(historical.audit);
        CHECK(historical.audit->proposal.digest == audit->proposal.digest);
        CHECK(historical.audit->decisionDigest == audit->decisionDigest);
        CHECK(historical.feedback == SourceTransactionFeedback::None);
        CHECK(historical.disposition == expected);
        CHECK(historical.audit->historySequence == 1);
        CHECK_FALSE(historical.canUndo);
        EditorWorkspaceViewCommandData undo;
        CHECK_FALSE(reopened.QueueUndo(workspace.lifetime.Token(), undo));

        const auto historyPath = workspace.project.directory / ".horo/local/agent/source-review-history";
        std::ifstream stored(historyPath, std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>{stored}, {}};
        for (const auto *sensitive : {"private-provider-label", "agent replacement", "a.cpp", "b.cpp", "provider"})
            CHECK(bytes.find(sensitive) == std::string::npos);
        std::ifstream disk(workspace.project.directory / "a.cpp", std::ios::binary);
        const std::string source{std::istreambuf_iterator<char>{disk}, {}};
        CHECK(source == (expected == SourceReviewDisposition::Saved ? "agent replacement" : "base"));
    }

    TEST_CASE("Source review history retains bounded records and preserves corrupt evidence", "[editor][source][review][history]") {
        ReviewWorkspace workspace;
        ProjectMutationCoordinator mutations{workspace.files};
        REQUIRE(workspace.Process(workspace.Queue("proposal", "reviewed")));
        workspace.Drain();
        REQUIRE(workspace.reviews.Status().audit);
        auto audit = *workspace.reviews.Status().audit;
        for (std::size_t index = 0; index <= SourceReviewHistoryCapacity; ++index) {
            audit.proposal.revision = index + 1;
            const auto written = WriteSourceReviewHistory(workspace.project.directory, audit, 0, SourceReviewDisposition::Applied,
                                                          mutations, workspace.files);
            REQUIRE(written.HasValue());
            CHECK(written.Value() == index + 1);
        }
        const auto latest = ReadSourceReviewHistory(workspace.project.directory);
        REQUIRE(latest.HasValue());
        REQUIRE(latest.Value());
        CHECK(latest.Value()->proposal.revision == SourceReviewHistoryCapacity + 1);
        constexpr auto historyName = ".horo/local/agent/source-review-history";
        const auto path = workspace.project.directory / historyName;
        {
            std::ifstream stored(path, std::ios::binary);
            CHECK(std::count(std::istreambuf_iterator<char>{stored}, {}, '\n') == SourceReviewHistoryCapacity + 1);
        }
        workspace.project.Put("interrupted", historyName);
        CHECK(ReadSourceReviewHistory(workspace.project.directory).HasError());
        CHECK(WriteSourceReviewHistory(workspace.project.directory, audit, 0, SourceReviewDisposition::Applied, mutations, workspace.files)
                  .HasError());
        std::ifstream stored(path, std::ios::binary);
        CHECK((std::string{std::istreambuf_iterator<char>{stored}, {}} == "interrupted"));
    }

    TEST_CASE("A reviewed creation stages an unsaved source with exact undo and explicit save", "[editor][source][review][create]") {
        TestSupport::SourceProject project;
        SourceFileOpenService open{project.directory};
        WorkspacePanelHost host;
        host.AttachDocumentIdentityRegistry(open.DocumentRegistry());
        NativeDurableFileSystem files;
        JobSystem jobs{JobSystemConfig{.workerCount = 1, .ioWorkerCount = 1}};
        SourceReviewTransactions reviews;
        CancellationSource lifetime;
        const auto path = SourceDocumentId::Parse("new.cpp").Value();
        const auto native = project.directory / path.Value();
        const auto policy = ProjectPolicy::Capture("workspace", 1, {{path, false}}).Value();
        const auto proposal = ChangeSet::Capture(policy, {"proposal", 1, {"provider", "model", "request"}, {CreateFile{path, {}}}}).Value();
        const std::array observed{SourcePathObservation{path, SourcePathState::Absent}};
        const auto plan = std::make_shared<const WorkspaceEditPlan>(PlanWorkspaceEdits(proposal, policy, observed).Value());
        ReviewDecisions decisions{proposal};
        REQUIRE(decisions.Decide(*plan, {0}, ReviewChoice::Accept).HasValue());
        EditorWorkspaceViewCommandData apply;
        REQUIRE(QueueSourceReviewApplication(policy.Project(), decisions, policy, plan, lifetime.Token(), apply));
        REQUIRE(reviews.Process(apply, open.Documents(), &jobs, project.directory, &files, &open));
        bool occupied{};
        SECTION("Late arrival cancels staging before tab admission") {
            project.Put("user file", path.Value().c_str());
            occupied = true;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (reviews.Status().feedback == SourceTransactionFeedback::Preparing && std::chrono::steady_clock::now() < deadline) {
            static_cast<void>(reviews.Poll(open.Documents(), &files, &open, &host));
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        const auto id = open.DocumentRegistry().Find({DocumentKind::Source, path});
        if (occupied) {
            CHECK(reviews.Status().feedback == SourceTransactionFeedback::Conflict);
            CHECK_FALSE(id);
            CHECK(std::filesystem::file_size(native) == 9);
            return;
        }
        REQUIRE(reviews.Status().feedback == SourceTransactionFeedback::Applied);
        REQUIRE(id);
        CHECK(host.ActiveDocument() == id->instance);
        const auto draft = open.Documents().Snapshot(id->instance).Value();
        CHECK(draft.Dirty());
        CHECK(draft.BaseRevision() == 0);
        CHECK_FALSE(open.Documents().UpdateAutosave(120, 1, files));
        CHECK_FALSE(open.Documents().UpdateAutosave(120, 1, files));
        CHECK_FALSE(std::filesystem::exists(native));
        CHECK(EncodeSourceReviewAudit(*reviews.Status().audit).HasValue());

        SECTION("An explicit save materializes even an empty source") {
            const auto saved = open.Documents().Save({id->instance, draft.Revision(), {}}, files);
            REQUIRE(saved.HasValue());
            CHECK_FALSE(saved.Value().snapshot.Dirty());
            CHECK(std::filesystem::is_regular_file(native));
            CHECK(reviews.Status().disposition == SourceReviewDisposition::Saved);
            const auto receipt = reviews.CaptureSavedReview(open.Documents());
            REQUIRE(receipt.HasValue());
            REQUIRE(receipt.Value().sources.size() == 1);
            CHECK(receipt.Value().sources.front().SharesStateWith(saved.Value().snapshot));
        }
        SECTION("Undo retires the exact unsaved draft") {
            EditorWorkspaceViewCommandData undo;
            REQUIRE(reviews.QueueUndo(lifetime.Token(), undo));
            REQUIRE(reviews.Process(undo, open.Documents(), &jobs, project.directory, &files, &open));
            CHECK(reviews.Status().disposition == SourceReviewDisposition::Reverted);
            CHECK_FALSE(open.DocumentRegistry().Find(id->instance));
            host.RefreshDocuments();
            CHECK_FALSE(host.ActiveDocument());
            CHECK_FALSE(std::filesystem::exists(native));
        }
        SECTION("A stale undo cannot discard later authored text") {
            EditorWorkspaceViewCommandData undo;
            REQUIRE(reviews.QueueUndo(lifetime.Token(), undo));
            REQUIRE(open.Documents().Edit(id->instance, {draft.Revision(), 0, 0, "authored"}).HasValue());
            REQUIRE(reviews.Process(undo, open.Documents(), &jobs, project.directory, &files, &open));
            CHECK(reviews.Status().feedback == SourceTransactionFeedback::Conflict);
            CHECK(open.Documents().Snapshot(id->instance).Value().Text() == "authored");
        }
    }

    TEST_CASE("Source review applies and reverts edits and drafts together", "[editor][source][review][create]") {
        TestSupport::SourceProject project;
        project.Put("base");
        SourceFileOpenService open{project.directory};
        const auto source = *open.Open({.path = "code.cpp"}).Value().sourceSnapshot;
        const auto original = source.Identity();
        const auto first = SourceDocumentId::Parse("first.cpp").Value();
        const auto second = SourceDocumentId::Parse("second.cpp").Value();
        const auto policy = ProjectPolicy::Capture("workspace", 1, {{original.key.source, false}, {first, false}, {second, false}}).Value();
        const auto proposal =
            ChangeSet::Capture(policy, {"drafts",
                                        1,
                                        {"provider", "model", "request"},
                                        {CreateFile{first, "first"}, EditFile{FileBase::Capture(source).Value(), {{0, "base", "edited"}}},
                                         CreateFile{second, ""}}})
                .Value();
        const std::array observed{SourcePathObservation{original.key.source, source}, SourcePathObservation{first, SourcePathState::Absent},
                                  SourcePathObservation{second, SourcePathState::Absent}};
        const auto plan = std::make_shared<const WorkspaceEditPlan>(PlanWorkspaceEdits(proposal, policy, observed).Value());
        ReviewDecisions decisions{proposal};
        REQUIRE(decisions.Decide(*plan, {}, ReviewChoice::Accept).HasValue());
        NativeDurableFileSystem files;
        SECTION("A target that appears after observation rejects the entire group") {
            auto prepared = decisions.PrepareTransaction(policy, *plan);
            REQUIRE(prepared.HasValue());
            auto candidate = std::move(prepared).Value();
            REQUIRE(candidate.ObserveDisk(project.directory, files).HasValue());
            project.Put("other", "second.cpp");
            REQUIRE(candidate.CommitFileGroup(open, files).HasError());
            CHECK(source.IsCurrent());
            CHECK_FALSE(open.DocumentRegistry().Find({DocumentKind::Source, first}));
            CHECK_FALSE(open.DocumentRegistry().Find({DocumentKind::Source, second}));
            return;
        }

        WorkspacePanelHost host;
        host.AttachDocumentIdentityRegistry(open.DocumentRegistry());
        REQUIRE(host.OpenDocument(original.key).HasValue());
        JobSystem jobs{JobSystemConfig{.workerCount = 1, .ioWorkerCount = 1}};
        SourceReviewTransactions reviews;
        CancellationSource lifetime;
        EditorWorkspaceViewCommandData command;
        REQUIRE(QueueSourceReviewApplication("workspace", decisions, policy, plan, lifetime.Token(), command));
        REQUIRE(reviews.Process(command, open.Documents(), &jobs, project.directory, &files, &open));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (reviews.Status().feedback == SourceTransactionFeedback::Preparing && std::chrono::steady_clock::now() < deadline) {
            static_cast<void>(reviews.Poll(open.Documents(), &files, &open, &host));
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        REQUIRE(reviews.Status().feedback == SourceTransactionFeedback::Applied);
        REQUIRE(host.DocumentTabs().size() == 3);
        REQUIRE(reviews.Status().audit);
        CHECK(EncodeSourceReviewAudit(*reviews.Status().audit).HasValue());
        const auto firstTab = open.DocumentRegistry().Find({DocumentKind::Source, first}).value();
        const auto secondTab = open.DocumentRegistry().Find({DocumentKind::Source, second}).value();
        const auto edited = open.Documents().Snapshot(original.instance).Value();
        CHECK(edited.Text() == "edited");
        CHECK(edited.DiskBase() == "base");
        CHECK(open.Documents().Snapshot(firstTab.instance).Value().Text() == "first");
        const auto empty = open.Documents().Snapshot(secondTab.instance).Value();
        CHECK(empty.Dirty());
        CHECK_FALSE(std::filesystem::exists(project.directory / "first.cpp"));
        CHECK_FALSE(std::filesystem::exists(project.directory / "second.cpp"));
        EditorWorkspaceViewCommandData undo;
        REQUIRE(reviews.QueueUndo(lifetime.Token(), undo));
        SECTION("Undo preserves newer edits to any draft") {
            REQUIRE(open.Documents().Edit(secondTab.instance, {empty.Revision(), 0, 0, "authored"}).HasValue());
            REQUIRE(reviews.Process(undo, open.Documents(), &jobs, project.directory, &files, &open));
            CHECK(reviews.Status().feedback == SourceTransactionFeedback::Conflict);
            CHECK(edited.IsCurrent());
            CHECK(open.DocumentRegistry().IsCurrent(firstTab));
            CHECK(open.Documents().Snapshot(secondTab.instance).Value().Text() == "authored");
            return;
        }
        REQUIRE(reviews.Process(undo, open.Documents(), &jobs, project.directory, &files, &open));
        CHECK(reviews.Status().feedback == SourceTransactionFeedback::Undone);
        CHECK(open.Documents().Snapshot(original.instance).Value().Text() == "base");
        host.RefreshDocuments();
        REQUIRE(host.DocumentTabs().size() == 1);
        CHECK(host.DocumentTabs().front().identity == original);
    }

    TEST_CASE("A confirmed deletion moves only the exact clean source and rolls back a failed move", "[editor][source][review][delete]") {
        struct Files final : TestSupport::NativePublicationFiles {
            bool failAfterCommit{};
            bool replaceSourceBeforeRollback{};
            bool failEveryDirectorySync{};

            Result<void> WritePrivateDurable(const std::filesystem::path &path, std::span<const std::byte> bytes, bool &created,
                                             std::filesystem::perms mode) override {
                return native.WritePrivateDurable(path, bytes, created, mode);
            }

            Result<void> MoveFileNoReplaceTracked(const std::filesystem::path &from, const std::filesystem::path &to,
                                                  AtomicFileReplacementReceipt &receipt) override {
                auto outcome = native.MoveFileNoReplaceTracked(from, to, receipt);
                if (receipt.WasCommitted() && (std::exchange(failAfterCommit, false) || failEveryDirectorySync)) {
                    if (replaceSourceBeforeRollback)
                        std::ofstream(from, std::ios::binary) << "new occupant";
                    return Result<void>::Failure(MakeError(SourceDocumentErrors::ReadFailed));
                }
                return outcome;
            }
        } files;

        TestSupport::SourceProject project;
        project.Put("saved content");
        SourceFileOpenService open{project.directory};
        const auto opened = open.Open({.path = "code.cpp"}).Value();
        const auto source = *opened.sourceSnapshot;
        const auto identity = source.Identity();
        const auto native = project.directory / "code.cpp";
        WorkspacePanelHost host;
        host.AttachDocumentIdentityRegistry(open.DocumentRegistry());
        REQUIRE(host.OpenDocument(identity.key).HasValue());
        JobSystem jobs{JobSystemConfig{.workerCount = 1, .ioWorkerCount = 1}};
        ProjectMutationCoordinator mutations{files};
        SourceReviewTransactions reviews;
        CancellationSource lifetime;
        const auto policy = ProjectPolicy::Capture("workspace", 1, {{identity.key.source, false}}).Value();
        const auto proposal =
            ChangeSet::Capture(policy, {"proposal", 1, {"provider", "model", "request"}, {DeleteFile{FileBase::Capture(source).Value()}}})
                .Value();
        const std::array observed{SourcePathObservation{identity.key.source, source}};
        const auto plan = std::make_shared<const WorkspaceEditPlan>(PlanWorkspaceEdits(proposal, policy, observed).Value());
        ReviewDecisions decisions{proposal};
        REQUIRE(decisions.Decide(*plan, {0}, ReviewChoice::Accept).HasValue());
        CHECK(std::filesystem::is_regular_file(native));
        CHECK(source.IsCurrent());
        EditorWorkspaceViewCommandData apply;
        REQUIRE(QueueSourceReviewApplication("workspace", decisions, policy, plan, lifetime.Token(), apply));
        REQUIRE(reviews.Process(apply, open.Documents(), &jobs, project.directory, &files, &open));
        auto expected = SourceTransactionFeedback::Deleted;
        enum class Restoration {
            None,
            Original,
            Collision,
            ReplacedPayload,
            FailedDurability
        };
        auto restoration = Restoration::None;
        SECTION("A newer user edit prevents removal") {
            REQUIRE(open.Documents().Edit(identity.instance, {source.Revision(), 0, 0, "authored "}).HasValue());
            expected = SourceTransactionFeedback::Conflict;
        }
        SECTION("A failed directory durability check restores the original path") {
            files.failAfterCommit = true;
            expected = SourceTransactionFeedback::ApplyFailed;
        }
        SECTION("A new occupant survives a failed rollback") {
            files.failAfterCommit = true;
            files.replaceSourceBeforeRollback = true;
            expected = SourceTransactionFeedback::DeleteRecovery;
        }
        SECTION("A visible rollback with unconfirmed durability keeps its manifest") {
            files.failEveryDirectorySync = true;
            expected = SourceTransactionFeedback::DeleteRecovery;
        }
        SECTION("Confirmation moves the exact saved source to Trash") {}
        SECTION("Review undo restores the file without creating a new draft") {
            restoration = Restoration::Original;
        }
        SECTION("Review undo preserves a case alias that now occupies the original name") {
            restoration = Restoration::Collision;
        }
        SECTION("Review undo rejects a replaced Trash payload") {
            restoration = Restoration::ReplacedPayload;
        }
        SECTION("A failed restore returns the payload to Trash and can be retried") {
            restoration = Restoration::FailedDurability;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (reviews.Status().feedback == SourceTransactionFeedback::Preparing && std::chrono::steady_clock::now() < deadline) {
            static_cast<void>(reviews.Poll(open.Documents(), &files, &open, &host, &mutations));
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        CHECK(reviews.Status().feedback == expected);
        const auto trash = project.directory / ".horo/local/trash";
        if (files.failEveryDirectorySync) {
            CHECK(open.DocumentRegistry().Find(identity.instance) == identity);
            std::ifstream original(native, std::ios::binary);
            CHECK((std::string{std::istreambuf_iterator<char>{original}, {}} == "saved content"));
            REQUIRE(std::filesystem::is_directory(trash));
            const auto first = std::filesystem::directory_iterator{trash};
            REQUIRE(first != std::filesystem::directory_iterator{});
            const auto &entry = first->path();
            CHECK(std::filesystem::is_regular_file(entry / "source-review-manifest"));
            CHECK_FALSE(std::filesystem::exists(entry / "payload"));
            return;
        }
        if (expected == SourceTransactionFeedback::Deleted || expected == SourceTransactionFeedback::DeleteRecovery) {
            if (expected == SourceTransactionFeedback::Deleted) {
                CHECK_FALSE(std::filesystem::exists(native));
                CHECK_FALSE(open.DocumentRegistry().Find(identity.instance));
                CHECK(reviews.Status().disposition == SourceReviewDisposition::Saved);
                const auto receipt = reviews.CaptureSavedReview(open.Documents());
                REQUIRE(receipt.HasValue());
                CHECK(receipt.Value().sources.empty());
                REQUIRE(receipt.Value().removed.size() == 1);
                CHECK(receipt.Value().removed.front() == identity.key.source);
                CHECK(receipt.Value().VerifyDisk(project.directory, files, lifetime.Token()).HasValue());
            } else {
                CHECK(open.DocumentRegistry().Find(identity.instance) == identity);
                CHECK(reviews.Status().disposition == SourceReviewDisposition::Stale);
                std::ifstream occupant(native, std::ios::binary);
                CHECK((std::string{std::istreambuf_iterator<char>{occupant}, {}} == "new occupant"));
            }
            REQUIRE(reviews.Status().audit);
            CHECK(EncodeSourceReviewAudit(*reviews.Status().audit).HasValue());
            REQUIRE(std::filesystem::is_directory(trash));
            const auto first = std::filesystem::directory_iterator{trash};
            REQUIRE(first != std::filesystem::directory_iterator{});
            const auto &item = first->path();
            const auto payload = item / "payload";
            {
                std::ifstream saved(payload, std::ios::binary);
                CHECK((std::string{std::istreambuf_iterator<char>{saved}, {}} == "saved content"));
            }
            CHECK(std::filesystem::is_regular_file(item / "source-review-manifest"));
            if (restoration == Restoration::None)
                return;
            const auto alias = project.directory / "Code.cpp";
            if (restoration == Restoration::Collision)
                std::ofstream(alias, std::ios::binary) << "other author";
            if (restoration == Restoration::ReplacedPayload) {
                REQUIRE(std::filesystem::remove(payload));
                std::ofstream(payload, std::ios::binary) << "saved content";
            }
            const auto undo = [&] {
                EditorWorkspaceViewCommandData revert;
                REQUIRE(reviews.QueueUndo(lifetime.Token(), revert));
                REQUIRE(reviews.Process(revert, open.Documents(), &jobs, project.directory, &files, &open, &mutations));
            };
            if (restoration == Restoration::FailedDurability) {
                files.failAfterCommit = true;
                undo();
                REQUIRE(reviews.Status().feedback == SourceTransactionFeedback::UndoFailed);
                CHECK_FALSE(std::filesystem::exists(native));
                CHECK(std::filesystem::is_regular_file(payload));
            }
            undo();
            if (restoration == Restoration::Collision || restoration == Restoration::ReplacedPayload) {
                CHECK(reviews.Status().feedback == SourceTransactionFeedback::Conflict);
                CHECK(std::filesystem::is_regular_file(payload));
                if (restoration == Restoration::Collision) {
                    std::ifstream disk(alias, std::ios::binary);
                    CHECK((std::string{std::istreambuf_iterator<char>{disk}, {}} == "other author"));
                } else {
                    CHECK_FALSE(std::filesystem::exists(native));
                }
                return;
            }
            CHECK(reviews.Status().feedback == SourceTransactionFeedback::Undone);
            CHECK(reviews.Status().disposition == SourceReviewDisposition::Reverted);
            CHECK_FALSE(reviews.Status().canUndo);
            CHECK_FALSE(std::filesystem::exists(payload));
            CHECK_FALSE(std::filesystem::exists(item));
            const auto reopened = open.Open({.path = "code.cpp"});
            REQUIRE(reopened.HasValue());
            REQUIRE(reopened.Value().sourceSnapshot);
            CHECK(reopened.Value().sourceSnapshot->Text() == "saved content");
            CHECK_FALSE(reopened.Value().sourceSnapshot->Dirty());
        } else {
            CHECK(open.DocumentRegistry().Find(identity.instance) == identity);
            CHECK(std::filesystem::is_regular_file(native));
            std::ifstream original(native, std::ios::binary);
            CHECK((std::string{std::istreambuf_iterator<char>{original}, {}} == "saved content"));
            CHECK_FALSE((std::filesystem::exists(trash) && !std::filesystem::is_empty(trash)));
        }
    }

    TEST_CASE("Confirmed rename keeps the active tab, authored text and history", "[editor][source][review][rename]") {
        struct Files final : TestSupport::NativePublicationFiles {
            bool failAfterCommit{};

            Result<void> MoveFileNoReplaceTracked(const std::filesystem::path &from, const std::filesystem::path &to,
                                                  AtomicFileReplacementReceipt &receipt) override {
                auto outcome = native.MoveFileNoReplaceTracked(from, to, receipt);
                if (receipt.WasCommitted() && std::exchange(failAfterCommit, false))
                    return Result<void>::Failure(MakeError(SourceDocumentErrors::ReadFailed));
                return outcome;
            }

            Result<void> WritePrivateDurable(const std::filesystem::path &path, std::span<const std::byte> bytes, bool &created,
                                             std::filesystem::perms mode) override {
                return native.WritePrivateDurable(path, bytes, created, mode);
            }
        } files;

        TestSupport::SourceProject project;
        project.Put("alpha beta");
        REQUIRE(std::filesystem::create_directory(project.directory / "nested"));
        SourceFileOpenService open{project.directory};
        const auto initial = *open.Open({.path = "code.cpp"}).Value().sourceSnapshot;
        const auto identity = initial.Identity();
        const auto edited = open.Documents().Edit(identity.instance, {initial.Revision(), 6, 4, "user"});
        REQUIRE(edited.HasValue());
        const auto source = edited.Value();
        const auto destination = SourceDocumentId::Parse("nested/moved.cpp").Value();
        WorkspacePanelHost host;
        host.AttachDocumentIdentityRegistry(open.DocumentRegistry());
        REQUIRE(host.OpenDocument(identity.key).HasValue());
        JobSystem jobs{JobSystemConfig{.workerCount = 1, .ioWorkerCount = 1}};
        ProjectMutationCoordinator mutations{files};
        SourceReviewTransactions reviews;
        CancellationSource lifetime;
        const auto policy = ProjectPolicy::Capture("workspace", 1, {{identity.key.source, false}, {destination, false}}).Value();
        const auto proposal =
            ChangeSet::Capture(policy, {"proposal",
                                        1,
                                        {"provider", "model", "request"},
                                        {RenameFile{FileBase::Capture(source).Value(), destination, {{0, "alpha", "renamed"}}}}})
                .Value();
        const std::array observed{SourcePathObservation{identity.key.source, source},
                                  SourcePathObservation{destination, SourcePathState::Absent}};
        const auto plan = std::make_shared<const WorkspaceEditPlan>(PlanWorkspaceEdits(proposal, policy, observed).Value());
        ReviewDecisions decisions{proposal};
        REQUIRE(decisions.Decide(*plan, {0}, ReviewChoice::Accept).HasValue());
        CHECK_FALSE(std::filesystem::exists(project.directory / "nested/moved.cpp"));
        EditorWorkspaceViewCommandData apply;
        REQUIRE(QueueSourceReviewApplication("workspace", decisions, policy, plan, lifetime.Token(), apply));
        REQUIRE(reviews.Process(apply, open.Documents(), &jobs, project.directory, &files, &open));
        const auto drain = [&] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (reviews.Status().feedback == SourceTransactionFeedback::Preparing && std::chrono::steady_clock::now() < deadline) {
                static_cast<void>(reviews.Poll(open.Documents(), &files, &open, &host, &mutations));
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            REQUIRE(reviews.Status().feedback != SourceTransactionFeedback::Preparing);
            host.RefreshDocuments();
        };
        auto expected = SourceTransactionFeedback::Renamed;
        SECTION("A late portable alias prevents commit") {
            project.Put("other author", "nested/Moved.cpp");
            expected = SourceTransactionFeedback::Conflict;
        }
        SECTION("Directory sync failure rolls back the move and preserves the old tab") {
            files.failAfterCommit = true;
            expected = SourceTransactionFeedback::Conflict;
        }
        drain();
        REQUIRE(reviews.Status().feedback == expected);
        CHECK(host.ActiveDocument() == identity.instance);
        const auto current = open.Documents().Snapshot(identity.instance).Value();
        if (expected != SourceTransactionFeedback::Renamed) {
            CHECK(current.SharesStateWith(source));
            CHECK(open.DocumentRegistry().Find(identity.instance) == identity);
            CHECK(std::filesystem::is_regular_file(project.directory / "code.cpp"));
            const auto foreign = project.directory / "nested/Moved.cpp";
            if (std::filesystem::exists(foreign)) {
                std::ifstream existing(foreign, std::ios::binary);
                CHECK((std::string{std::istreambuf_iterator<char>{existing}, {}} == "other author"));
            }
            return;
        }
        CHECK(current.Identity().key.source == destination);
        CHECK(host.DocumentTabs().front().identity == current.Identity());
        CHECK(current.Text() == "renamed user");
        CHECK(current.DiskBase() == "alpha beta");
        CHECK(current.Dirty());
        CHECK_FALSE(std::filesystem::exists(project.directory / "code.cpp"));
        const auto movedPath = project.directory / "nested/moved.cpp";
        {
            std::ifstream disk(movedPath, std::ios::binary);
            CHECK((std::string{std::istreambuf_iterator<char>{disk}, {}} == "alpha beta"));
        }
        SECTION("A saved rename records both the live destination and original path") {
            CHECK(reviews.CaptureSavedReview(open.Documents()).HasError());
            const auto saved = open.Documents().Save({identity.instance, current.Revision(), {}}, files);
            REQUIRE(saved.HasValue());
            const auto receipt = reviews.CaptureSavedReview(open.Documents());
            REQUIRE(receipt.HasValue());
            REQUIRE(receipt.Value().sources.size() == 1);
            CHECK(receipt.Value().sources.front().Identity().key.source == destination);
            REQUIRE(receipt.Value().removed.size() == 1);
            CHECK(receipt.Value().removed.front() == identity.key.source);
            REQUIRE(receipt.Value().VerifyDisk(project.directory, files, lifetime.Token()).HasValue());
            project.Put("replacement", "code.cpp");
            CHECK(receipt.Value().VerifyDisk(project.directory, files, lifetime.Token()).HasError());
        }
        SECTION("Document undo reverses text while the confirmed path stays current") {
            const auto undo = open.Documents().Undo(identity.instance, current.Revision());
            REQUIRE(undo.HasValue());
            CHECK(undo.Value().snapshot.Text() == "alpha user");
            CHECK(undo.Value().snapshot.Identity().key.source == destination);
            CHECK_FALSE(reviews.Status().canUndo);
        }
        SECTION("Review undo moves the saved file back and restores the pre-review dirty buffer") {
            EditorWorkspaceViewCommandData revert;
            REQUIRE(reviews.QueueUndo(lifetime.Token(), revert));
            REQUIRE(reviews.Process(revert, open.Documents(), &jobs, project.directory, &files, &open));
            drain();
            REQUIRE(reviews.Status().feedback == SourceTransactionFeedback::Undone);
            CHECK(reviews.Status().disposition == SourceReviewDisposition::Reverted);
            CHECK_FALSE(reviews.Status().canUndo);
            CHECK_FALSE(std::filesystem::exists(movedPath));
            CHECK(host.ActiveDocument() == identity.instance);
            CHECK(host.DocumentTabs().front().identity == identity);
            const auto restored = open.Documents().Snapshot(identity.instance).Value();
            CHECK(restored.Text() == "alpha user");
            CHECK(restored.DiskBase() == "alpha beta");
            CHECK(restored.Dirty());
            std::ifstream disk(project.directory / "code.cpp", std::ios::binary);
            CHECK((std::string{std::istreambuf_iterator<char>{disk}, {}} == "alpha beta"));
        }
        SECTION("Review undo rejects a source edit made after it was queued") {
            EditorWorkspaceViewCommandData revert;
            REQUIRE(reviews.QueueUndo(lifetime.Token(), revert));
            REQUIRE(reviews.Process(revert, open.Documents(), &jobs, project.directory, &files, &open));
            const auto authored = open.Documents().Edit(identity.instance, {current.Revision(), 0, 7, "authored"});
            REQUIRE(authored.HasValue());
            drain();
            CHECK(reviews.Status().feedback == SourceTransactionFeedback::Conflict);
            CHECK_FALSE(reviews.Status().canUndo);
            CHECK(open.Documents().Snapshot(identity.instance).Value().SharesStateWith(authored.Value()));
            CHECK(std::filesystem::is_regular_file(movedPath));
            CHECK_FALSE(std::filesystem::exists(project.directory / "code.cpp"));
        }
        SECTION("Review undo leaves a new occupant of the original name intact") {
            project.Put("another source", "code.cpp");
            EditorWorkspaceViewCommandData revert;
            REQUIRE(reviews.QueueUndo(lifetime.Token(), revert));
            REQUIRE(reviews.Process(revert, open.Documents(), &jobs, project.directory, &files, &open));
            drain();
            CHECK(reviews.Status().feedback == SourceTransactionFeedback::Conflict);
            CHECK(reviews.Status().canUndo);
            CHECK(open.Documents().Snapshot(identity.instance).Value().SharesStateWith(current));
            CHECK(std::filesystem::is_regular_file(movedPath));
            std::ifstream original(project.directory / "code.cpp", std::ios::binary);
            CHECK((std::string{std::istreambuf_iterator<char>{original}, {}} == "another source"));
        }
    }

    TEST_CASE("Reviewed deletions and renames publish together or restore every original path", "[editor][source][review][move]") {
        const bool includeDeletion = GENERATE(true, false);

        struct Files final : TestSupport::NativePublicationFiles {
            int failOnMove{};
            int moves{};
            const ErrorCodeDescriptor *nativeRefusal{};

            Result<void> WritePrivateDurable(const std::filesystem::path &path, std::span<const std::byte> bytes, bool &created,
                                             std::filesystem::perms mode) override {
                return native.WritePrivateDurable(path, bytes, created, mode);
            }

            Result<void> MoveFileNoReplaceTracked(const std::filesystem::path &from, const std::filesystem::path &to,
                                                  AtomicFileReplacementReceipt &receipt) override {
                ++moves;
                if (moves == failOnMove && nativeRefusal)
                    return Result<void>::Failure(MakeError(*nativeRefusal));
                auto moved = native.MoveFileNoReplaceTracked(from, to, receipt);
                if (moves == failOnMove && receipt.WasCommitted())
                    return Result<void>::Failure(MakeError(SourceDocumentErrors::ReadFailed));
                return moved;
            }
        } files;

        TestSupport::SourceProject project;
        project.Put("alpha", "a.cpp");
        project.Put("beta", "b.cpp");
        project.Put("base", "edit.cpp");
        project.Put("remove", "delete.cpp");
        SourceFileOpenService open{project.directory};
        const auto a = *open.Open({.path = "a.cpp"}).Value().sourceSnapshot;
        const auto b = *open.Open({.path = "b.cpp"}).Value().sourceSnapshot;
        const auto edit = *open.Open({.path = "edit.cpp"}).Value().sourceSnapshot;
        const auto deletion = *open.Open({.path = "delete.cpp"}).Value().sourceSnapshot;
        const auto aPath = SourceDocumentId::Parse("new-a.cpp").Value();
        const auto bPath = SourceDocumentId::Parse("new-b.cpp").Value();
        const auto draft = SourceDocumentId::Parse("draft.cpp").Value();
        const auto policy = ProjectPolicy::Capture("workspace", 1,
                                                   {{a.Identity().key.source, false},
                                                    {b.Identity().key.source, false},
                                                    {edit.Identity().key.source, false},
                                                    {deletion.Identity().key.source, false},
                                                    {aPath, false},
                                                    {bPath, false},
                                                    {draft, false}})
                                .Value();
        const auto proposal = ChangeSet::Capture(policy, {"rename-group",
                                                          1,
                                                          {"provider", "model", "request"},
                                                          {RenameFile{FileBase::Capture(a).Value(), aPath, {{0, "alpha", "reviewed"}}},
                                                           RenameFile{FileBase::Capture(b).Value(), bPath, {}}, CreateFile{draft, "draft"},
                                                           EditFile{FileBase::Capture(edit).Value(), {{0, "base", "edited"}}},
                                                           DeleteFile{FileBase::Capture(deletion).Value()}}})
                                  .Value();
        const std::array observed{SourcePathObservation{a.Identity().key.source, a},
                                  SourcePathObservation{b.Identity().key.source, b},
                                  SourcePathObservation{edit.Identity().key.source, edit},
                                  SourcePathObservation{deletion.Identity().key.source, deletion},
                                  SourcePathObservation{aPath, SourcePathState::Absent},
                                  SourcePathObservation{bPath, SourcePathState::Absent},
                                  SourcePathObservation{draft, SourcePathState::Absent}};
        const auto plan = std::make_shared<const WorkspaceEditPlan>(PlanWorkspaceEdits(proposal, policy, observed).Value());
        ReviewDecisions decisions{proposal};
        REQUIRE(decisions.Decide(*plan, {}, ReviewChoice::Accept).HasValue());
        if (!includeDeletion) {
            const auto filesInReview = proposal.Files();
            const auto removal = std::ranges::find_if(filesInReview, [](const auto &file) {
                return std::holds_alternative<DeleteFile>(file);
            });
            REQUIRE(removal != filesInReview.end());
            REQUIRE(decisions.Decide(*plan, {static_cast<std::size_t>(removal - filesInReview.begin())}, ReviewChoice::Reject).HasValue());
        }
        JobSystem jobs{JobSystemConfig{.workerCount = 1, .ioWorkerCount = 1}};
        ProjectMutationCoordinator mutations{files};
        SourceReviewTransactions reviews;
        WorkspacePanelHost host;
        host.AttachDocumentIdentityRegistry(open.DocumentRegistry());
        REQUIRE(host.OpenDocument(a.Identity().key).HasValue());
        REQUIRE(host.OpenDocument(deletion.Identity().key).HasValue());
        CancellationSource lifetime;
        EditorWorkspaceViewCommandData command;
        REQUIRE(QueueSourceReviewApplication("workspace", decisions, policy, plan, lifetime.Token(), command));
        REQUIRE(reviews.Process(command, open.Documents(), &jobs, project.directory, &files, &open));
        SECTION("The second visible move cannot confirm durability") {
            files.failOnMove = 2;
        }
        SECTION("A cross-volume rejection retains its message after rolling back the first move") {
            files.failOnMove = 2;
            files.nativeRefusal = &DurableFileErrors::CrossVolume;
        }
        SECTION("A filesystem without exclusive moves reports platform support") {
            files.failOnMove = 2;
            files.nativeRefusal = &DurableFileErrors::MoveUnsupported;
        }
        if (includeDeletion) {
            SECTION("The Trash move fails after completing both renames") {
                files.failOnMove = 3;
            }
        }
        SECTION("Every move reaches durability") {}
        const auto drain = [&] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (reviews.Status().feedback == SourceTransactionFeedback::Preparing && std::chrono::steady_clock::now() < deadline) {
                static_cast<void>(reviews.Poll(open.Documents(), &files, &open, &host, &mutations));
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            host.RefreshDocuments();
        };
        drain();
        const auto trash = project.directory / ".horo/local/trash";
        if (files.failOnMove) {
            auto expected = SourceTransactionFeedback::Conflict;
            if (files.nativeRefusal == &DurableFileErrors::CrossVolume)
                expected = SourceTransactionFeedback::CrossVolume;
            if (files.nativeRefusal == &DurableFileErrors::MoveUnsupported)
                expected = SourceTransactionFeedback::MoveUnsupported;
            CHECK(reviews.Status().feedback == expected);
            CHECK(files.moves == 2 * files.failOnMove - static_cast<int>(files.nativeRefusal != nullptr));
            CHECK(a.IsCurrent());
            CHECK(b.IsCurrent());
            CHECK(edit.IsCurrent());
            CHECK(deletion.IsCurrent());
            CHECK(open.DocumentRegistry().IsCurrent(a.Identity()));
            CHECK(open.DocumentRegistry().IsCurrent(b.Identity()));
            CHECK_FALSE(open.DocumentRegistry().Find({DocumentKind::Source, draft}));
            CHECK(std::filesystem::is_regular_file(project.directory / "a.cpp"));
            CHECK(std::filesystem::is_regular_file(project.directory / "b.cpp"));
            CHECK_FALSE(std::filesystem::exists(project.directory / "new-a.cpp"));
            CHECK_FALSE(std::filesystem::exists(project.directory / "new-b.cpp"));
            CHECK(std::filesystem::is_regular_file(project.directory / "delete.cpp"));
            CHECK((!std::filesystem::exists(trash) || std::filesystem::is_empty(trash)));
            return;
        }
        REQUIRE(reviews.Status().feedback == SourceTransactionFeedback::Applied);
        CHECK(files.moves == (includeDeletion ? 3 : 2));
        CHECK(open.DocumentRegistry().Find(a.Identity().instance)->key.source == aPath);
        CHECK(open.DocumentRegistry().Find(b.Identity().instance)->key.source == bPath);
        CHECK(host.DocumentTabs().front().identity.key.source == aPath);
        const auto renamed = open.Documents().Snapshot(a.Identity().instance).Value();
        CHECK(renamed.Text() == "reviewed");
        CHECK(renamed.DiskBase() == "alpha");
        CHECK(open.Documents().Snapshot(edit.Identity().instance).Value().Text() == "edited");
        CHECK_FALSE(std::filesystem::exists(project.directory / "draft.cpp"));
        CHECK(std::filesystem::is_regular_file(project.directory / "new-a.cpp"));
        CHECK(std::filesystem::is_regular_file(project.directory / "new-b.cpp"));
        REQUIRE(reviews.Status().audit);
        CHECK(EncodeSourceReviewAudit(*reviews.Status().audit).HasValue());
        const auto deletedPath = project.directory / "delete.cpp";
        if (includeDeletion) {
            CHECK_FALSE(open.DocumentRegistry().Find(deletion.Identity().instance));
            CHECK_FALSE(deletion.IsCurrent());
            CHECK_FALSE(std::filesystem::exists(deletedPath));
            const auto attempt = std::filesystem::directory_iterator{trash};
            REQUIRE(attempt != std::filesystem::directory_iterator{});
            std::ifstream file(attempt->path() / "payload", std::ios::binary);
            CHECK((std::string{std::istreambuf_iterator<char>{file}, {}} == "remove"));
        }
        const auto undo = [&] {
            EditorWorkspaceViewCommandData reverse;
            REQUIRE(reviews.QueueUndo(lifetime.Token(), reverse));
            REQUIRE(reviews.Process(reverse, open.Documents(), &jobs, project.directory, &files, &open));
            drain();
        };
        project.Put("new occupant", "b.cpp");
        undo();
        REQUIRE(reviews.Status().feedback == SourceTransactionFeedback::Conflict);
        CHECK(files.moves == (includeDeletion ? 3 : 2));
        {
            std::ifstream file(project.directory / "b.cpp", std::ios::binary);
            CHECK((std::string{std::istreambuf_iterator<char>{file}, {}} == "new occupant"));
        }
        REQUIRE(std::filesystem::remove(project.directory / "b.cpp"));
        files.failOnMove = files.moves + 2;
        undo();
        REQUIRE(reviews.Status().feedback == SourceTransactionFeedback::Conflict);
        CHECK(open.DocumentRegistry().Find(a.Identity().instance)->key.source == aPath);
        CHECK(open.DocumentRegistry().Find(b.Identity().instance)->key.source == bPath);
        CHECK(open.DocumentRegistry().Find({DocumentKind::Source, draft}).has_value());
        CHECK(open.Documents().Snapshot(edit.Identity().instance).Value().Text() == "edited");
        CHECK(std::filesystem::is_regular_file(project.directory / "new-a.cpp"));
        CHECK(std::filesystem::is_regular_file(project.directory / "new-b.cpp"));
        CHECK(std::filesystem::exists(deletedPath) != includeDeletion);
        files.failOnMove = 0;
        undo();
        REQUIRE(reviews.Status().feedback == SourceTransactionFeedback::Undone);
        CHECK(open.DocumentRegistry().IsCurrent(a.Identity()));
        CHECK(open.DocumentRegistry().IsCurrent(b.Identity()));
        CHECK_FALSE(open.DocumentRegistry().Find({DocumentKind::Source, draft}));
        CHECK(open.Documents().Snapshot(a.Identity().instance).Value().Text() == "alpha");
        CHECK(open.Documents().Snapshot(edit.Identity().instance).Value().Text() == "base");
        CHECK(std::filesystem::is_regular_file(project.directory / "a.cpp"));
        CHECK(std::filesystem::is_regular_file(project.directory / "b.cpp"));
        CHECK_FALSE(std::filesystem::exists(project.directory / "new-a.cpp"));
        CHECK_FALSE(std::filesystem::exists(project.directory / "new-b.cpp"));
        CHECK(std::filesystem::is_regular_file(deletedPath));
        if (includeDeletion) {
            CHECK(std::filesystem::is_empty(trash));
            CHECK_FALSE(open.DocumentRegistry().Find(deletion.Identity().instance));
            std::ifstream file(deletedPath, std::ios::binary);
            CHECK((std::string{std::istreambuf_iterator<char>{file}, {}} == "remove"));
        }
    }

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
            CHECK(workspace.reviews.Status().disposition == SourceReviewDisposition::Applied);
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
            CHECK(workspace.reviews.Status().disposition == SourceReviewDisposition::Reverted);
            const auto restored = workspace.Snapshots();
            CHECK(restored[0].Text() == "base");
            CHECK(restored[1].Text() == "base");
            CHECK(restored[0].Revision() == 3 + 2 * index);
            CHECK(restored[0].Revision() == restored[1].Revision());
        }
    }

    TEST_CASE("Source review disposition requires exact in-place saves for the entire applied group", "[editor][source][review][history]") {
        ReviewWorkspace workspace;
        REQUIRE(workspace.Process(workspace.Queue("proposal", "agent")));
        workspace.Drain();
        const auto applied = workspace.Snapshots();
        REQUIRE(workspace.reviews.Status().disposition == SourceReviewDisposition::Applied);
        CHECK(workspace.reviews.CaptureSavedReview(workspace.documents).HasError());
        REQUIRE(workspace.documents.Save({workspace.a.instance, applied[0].Revision(), {}}, workspace.files).HasValue());
        CHECK(workspace.reviews.Status().disposition == SourceReviewDisposition::Applied);
        CHECK(workspace.reviews.CaptureSavedReview(workspace.documents).HasError());
        SECTION("Every changed source was saved at the applied revision") {
            REQUIRE(workspace.documents.Save({workspace.b.instance, applied[1].Revision(), {}}, workspace.files).HasValue());
            CHECK(workspace.reviews.Status().disposition == SourceReviewDisposition::Saved);
            const auto receipt = workspace.reviews.CaptureSavedReview(workspace.documents);
            REQUIRE(receipt.HasValue());
            CHECK(receipt.Value().audit == workspace.reviews.Status().audit);
            REQUIRE(receipt.Value().sources.size() == 2);
            CHECK(receipt.Value().removed.empty());
            CHECK(std::ranges::all_of(receipt.Value().sources, &SourceDocumentSnapshot::IsCurrent));
            REQUIRE(receipt.Value().VerifyDisk(workspace.project.directory, workspace.files, workspace.lifetime.Token()).HasValue());
            CancellationSource cancelled;
            cancelled.RequestCancellation();
            const auto stopped = receipt.Value().VerifyDisk(workspace.project.directory, workspace.files, cancelled.Token());
            REQUIRE(stopped.HasError());
            CHECK(stopped.ErrorValue().code.Value() == SourceDocumentErrors::Cancelled.code.Value());
            workspace.project.Put("other", "b.cpp");
            CHECK(receipt.Value().VerifyDisk(workspace.project.directory, workspace.files, workspace.lifetime.Token()).HasError());
            CHECK(receipt.Value().sources.back().IsCurrent());
            workspace.project.Put("agent", "b.cpp");
            REQUIRE(workspace.documents.Close(workspace.a.instance).HasValue());
            REQUIRE(workspace.documents.Close(workspace.b.instance).HasValue());
            CHECK(workspace.reviews.Status().disposition == SourceReviewDisposition::Saved);
            CHECK(workspace.reviews.CaptureSavedReview(workspace.documents).HasError());
            CHECK(receipt.Value().VerifyDisk(workspace.project.directory, workspace.files, workspace.lifetime.Token()).HasError());
        }
        SECTION("Editing and restoring identical bytes invalidates the saved validation revision") {
            const auto saved = workspace.documents.Save({workspace.b.instance, applied[1].Revision(), {}}, workspace.files);
            REQUIRE(saved.HasValue());
            const auto revision = saved.Value().snapshot.Revision();
            REQUIRE(workspace.documents.Edit(workspace.b.instance, {revision, 0, 5, "user"}).HasValue());
            REQUIRE(workspace.documents.Edit(workspace.b.instance, {revision + 1, 0, 4, "agent"}).HasValue());
            CHECK_FALSE(workspace.Snapshots()[1].Dirty());
            CHECK(workspace.reviews.CaptureSavedReview(workspace.documents).HasError());
        }
        SECTION("An intervening user edit invalidates the unsaved review") {
            const auto edited = workspace.documents.Edit(workspace.b.instance, {applied[1].Revision(), 0, 5, "user"});
            REQUIRE(edited.HasValue());
            REQUIRE(workspace.documents.Save({workspace.b.instance, edited.Value().Revision(), {}}, workspace.files).HasValue());
            CHECK(workspace.reviews.Status().disposition == SourceReviewDisposition::Stale);
        }
        CHECK_FALSE(workspace.reviews.Status().canUndo);
    }

    /** @brief Holds one approved native invocation until the test owner stages its source edit. */
    class GatedFailedReviewBuild final : public IExternalProcessRunner {
    public:
        Result<ExternalProcessResult> Run(const ExternalProcessRequest &, const CancellationToken &) override {
            std::unique_lock lock{mutex_};
            ++calls_;
            changed_.notify_all();
            changed_.wait(lock, [this] {
                return released_;
            });
            return Result<ExternalProcessResult>::Success({ProcessTerminationReason::Exited, 1});
        }

        [[nodiscard]] bool WaitForInvocation() {
            std::unique_lock lock{mutex_};
            return changed_.wait_for(lock, std::chrono::seconds{10}, [this] {
                return calls_ != 0;
            });
        }

        void Release() {
            {
                std::scoped_lock lock{mutex_};
                released_ = true;
            }
            changed_.notify_all();
        }

        [[nodiscard]] unsigned Calls() const {
            std::scoped_lock lock{mutex_};
            return calls_;
        }

    private:
        mutable std::mutex mutex_;
        std::condition_variable changed_;
        unsigned calls_{};
        bool released_{};
    };

    /** @brief Drives the production workspace commands with isolated application service owners. */
    struct ReviewBuildWorkspace final {
        using Command = EditorWorkspaceViewCommand;
        using Validation = SourceReviewValidation;

        ReviewBuildWorkspace() {
            project.Put("int value = 1;\n", "source.cpp");
            REQUIRE(scene.Startup(lifetime.Token()).HasValue());
            const EditorWorkspaceDependencies dependencies{.durableFiles = &files,
                                                           .jobs = &jobs,
                                                           .gameplay = {.builds = &builds,
                                                                        .environment = {.gameplaySdkPackage = project.directory},
                                                                        .tests = &tests,
                                                                        .testEnvironment = {.ctest = "ctest"}}};
            workspace =
                std::make_unique<EditorWorkspaceController>(project.directory, scene, Assets::AssetRegistrySnapshot{}, dependencies);
        }

        ~ReviewBuildWorkspace() {
            // Release held work before owner teardown, including when an assertion fails with the gate closed.
            process.Release();
        }

        /** @brief Apply the exact reviewed change and capture approval for its live audit. */
        EditorWorkspaceViewCommandData ApplyReview() {
            workspace->ProcessCommand(
                {.command = Command::OpenSourceFile,
                 .sourceOpenRequest = SourceOpenRequest{.path = project.directory / "source.cpp", .mode = SourceOpenMode::EmbeddedOnly}});
            REQUIRE(workspace->ViewModel().sourceDocuments.size() == 1);
            const auto source = workspace->ViewModel().sourceDocuments.front();
            auto proposal = PrepareEditorLocalPatchReview(
                R"({"version":1,"files":[{"kind":"edit","path":"source.cpp","hunks":[{"offset":12,"before":"1","after":"2"}]}]})",
                {{source}, "test", 1}, "validation-workspace", 1);
            REQUIRE(proposal.HasValue());
            auto review = std::move(proposal).Value();
            const auto policy =
                ProjectPolicy::Capture("validation-workspace", review.proposal.PolicyRevision(), {{source.Identity().key.source, false}});
            REQUIRE(policy.HasValue());
            REQUIRE(review.decisions.Decide(*review.plan, {}, ReviewChoice::Accept).HasValue());
            EditorWorkspaceViewCommandData command;
            REQUIRE(QueueSourceReviewApplication("validation-workspace", review.decisions, policy.Value(), review.plan, lifetime.Token(),
                                                 command));
            workspace->ProcessCommand(command);
            AwaitValidation(Validation::SaveRequired);
            const auto audit = workspace->ReadSourceReviewStatus().audit;
            REQUIRE(audit);
            return {.command = Command::BuildSourceReview, .sourceReviewLifetime = lifetime.Token(), .sourceReviewValidationAudit = audit};
        }

        /** @brief Pumps owner work while awaiting a status; the deadline reports a stalled fixture. */
        void AwaitValidation(const Validation expected) {
            REQUIRE(WaitUntil([&] {
                workspace->UpdateSourceAutosave(0, 0);
                return workspace->ReadSourceReviewStatus().validation == expected;
            }));
        }

        [[nodiscard]] bool HasBuildOperation() const {
            const auto snapshot = operations.SnapshotIfChanged(0);
            return snapshot && !snapshot->operations.empty();
        }

        /** @brief Waits for the only admitted isolated build to publish its process failure. */
        void AwaitBuildFailure() const {
            REQUIRE(WaitUntil([&] {
                const auto snapshot = operations.SnapshotIfChanged(0);
                return snapshot && snapshot->operations.size() == 1 && snapshot->operations.front().kind == OperationKind::Build &&
                       snapshot->operations.front().state == OperationState::Failed;
            }));
        }

        TestSupport::SourceProject project;
        NativeDurableFileSystem files;
        JobSystem jobs{JobSystemConfig{.workerCount = 1, .ioWorkerCount = 1}};
        OperationStore operations{8, 8};
        BuildOutputStore output{32};
        GatedFailedReviewBuild process;
        Application::GameplayBuildService builds{process, jobs, files, &output, &operations};
        Application::GameplayTestService tests{process, jobs, files, operations, output};
        Runtime::RuntimeSceneService scene;
        CancellationSource lifetime;
        std::unique_ptr<EditorWorkspaceController> workspace;

    private:
        /** @brief Observes completion only; invocation ordering is controlled by the process gate. */
        template <typename Predicate> static bool WaitUntil(const Predicate &check) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
            do {
                if (check())
                    return true;
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            } while (std::chrono::steady_clock::now() < deadline);
            return false;
        }
    };

    TEST_CASE("Workspace review build runs only for approval of the exact saved source", "[editor][source][review][validation]") {
        using Validation = ReviewBuildWorkspace::Validation;
        ReviewBuildWorkspace fixture;
        auto &workspace = *fixture.workspace;
        auto &process = fixture.process;
        const auto approved = fixture.ApplyReview();
        workspace.ProcessCommand(approved);
        CHECK(process.Calls() == 0U);
        CHECK_FALSE(fixture.HasBuildOperation());
        REQUIRE(workspace.SaveSourceDocuments().HasValue());
        fixture.AwaitValidation(Validation::Ready);
        auto wrongReview = approved;
        wrongReview.sourceReviewValidationAudit = std::make_shared<const SourceReviewAudit>(*approved.sourceReviewValidationAudit);
        workspace.ProcessCommand(wrongReview);
        CHECK(process.Calls() == 0U);
        CHECK_FALSE(fixture.HasBuildOperation());
        workspace.ProcessCommand(approved);
        REQUIRE(process.WaitForInvocation());
        process.Release();
        fixture.AwaitValidation(Validation::BuildFailed);
        CHECK(process.Calls() == 1U);
        CHECK_FALSE(workspace.ReadSourceReviewStatus().canRunTests);
        CHECK(workspace.ViewModel().sourceDocuments.front().Text() == "int value = 2;\n");
    }

    TEST_CASE("Failed approved workspace build preserves a newer dirty review buffer", "[editor][source][review][validation]") {
        using Command = ReviewBuildWorkspace::Command;
        using Validation = ReviewBuildWorkspace::Validation;
        ReviewBuildWorkspace fixture;
        auto &workspace = *fixture.workspace;
        auto &process = fixture.process;
        auto approved = fixture.ApplyReview();
        const auto instance = workspace.ViewModel().sourceDocuments.front().Identity().instance;
        REQUIRE(workspace.SaveSourceDocuments().HasValue());
        fixture.AwaitValidation(Validation::Ready);
        workspace.ProcessCommand(approved);
        REQUIRE(process.WaitForInvocation());
        REQUIRE(workspace.ReadSourceReviewStatus().validation == Validation::Building);
        const auto saved = workspace.SourceDocuments().Snapshot(instance).Value();
        REQUIRE_FALSE(saved.Dirty());
        const auto edited = workspace.SourceDocuments().Edit(instance, {saved.Revision(), 12, 1, "3"});
        REQUIRE(edited.HasValue());
        CHECK(workspace.ReadSourceReviewStatus().validation == Validation::Stale);

        process.Release();
        fixture.AwaitBuildFailure();
        const auto stale = workspace.ReadSourceReviewStatus();
        CHECK(stale.validation == Validation::Stale);
        CHECK_FALSE(stale.canRunTests);
        approved.command = Command::TestSourceReview;
        workspace.ProcessCommand(approved);
        REQUIRE(fixture.operations.SnapshotIfChanged(0)->operations.size() == 1);
        CHECK(process.Calls() == 1U);
        const auto retained = workspace.SourceDocuments().Snapshot(instance).Value();
        CHECK(retained.Dirty());
        CHECK(retained.Revision() == edited.Value().Revision());
        CHECK(retained.Text() == "int value = 3;\n");
        std::ifstream disk(fixture.project.directory / "source.cpp", std::ios::binary);
        REQUIRE(disk);
        CHECK(std::string(std::istreambuf_iterator<char>{disk}, {}) == "int value = 2;\n");
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
        REQUIRE((std::filesystem::last_write_time(path) == previousTime));
        const auto applied = workspace.documents.EditBatchWithEvidence(edits, evidence.Value(), workspace.files);
        REQUIRE(applied.HasError());
        CHECK(applied.ErrorValue().code.Value() == SourceDocumentErrors::SaveConflict.code.Value());
        CHECK(before[0].IsCurrent());
        CHECK(before[1].IsCurrent());
    }
}  // namespace
