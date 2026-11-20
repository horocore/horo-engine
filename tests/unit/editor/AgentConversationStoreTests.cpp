#include "AgentConversationStore.h"
#include "AgentHistoryReview.h"
#include "AgentHistorySession.h"
#include "Horo/Foundation/JobSystem.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <future>
#include <iterator>
#include <thread>

namespace Horo::Agent {
    namespace {
        using Availability = AgentHistoryAvailability;
        using MessageState = AgentHistoryMessageState;

        /** @brief Classifies the controlled test vocabulary and models a host-owned redaction callback. */
        AgentHistoryTextDecision HistoryFilter(AgentHistoryTextKind, const std::string_view visible) {
            if (visible == "redactor exception fixture")
                throw std::runtime_error{"filter failure with private input"};
            if (visible == "credential fixture")
                return {Security::AgentDataClass::Credential, std::string{visible}};
            if (visible == "raw sensor fixture")
                return {Security::AgentDataClass::RawSensorData, std::string{visible}};
            if (visible == "raw attachment fixture")
                return {Security::AgentDataClass::ProjectFile, std::string{visible}};
            if (visible == "visible prompt with private fragment")
                return {Security::AgentDataClass::EditorContext, "visible prompt with [redacted]"};
            return {Security::AgentDataClass::EditorContext, std::string{visible}};
        }

        /** @brief Owns a dedicated real project root and durable file authority. */
        struct Project final {
            NativeDurableFileSystem files;
            std::filesystem::path root =
                std::filesystem::temp_directory_path() /
                ("horo agent history " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

            Project() {
                REQUIRE(std::filesystem::create_directory(root));
            }

            ~Project() {
                std::error_code ignored;
                std::filesystem::remove_all(root, ignored);
            }

            [[nodiscard]] AgentConversationStore Open(const std::string_view id = "history-project",
                                                      const AgentHistoryLimits &limits = {}) {
                return AgentConversationStore::Open(files, root, id, HistoryFilter, limits);
            }

            [[nodiscard]] std::filesystem::path Path() const {
                return root / ".horo/local/agent/conversations.sqlite";
            }

            [[nodiscard]] std::vector<std::uint8_t> Image() const {
                const auto result = files.ReadFileBounded(Path(), 2U << 20U);
                REQUIRE(result.HasValue());
                return result.Value();
            }
        };

        /** @brief Requires a typed denial without relying on diagnostic prose. */
        void RequireError(const auto &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == expected.code.Value());
        }

        /** @brief Compares bytes as opaque ASCII substrings without printing any stored text. */
        bool Contains(const std::vector<std::uint8_t> &bytes, const std::string_view value) {
            return std::string_view{reinterpret_cast<const char *>(bytes.data()), bytes.size()}.find(value) != std::string_view::npos;
        }

        /** @brief Waits outside production lanes while polling the exact lifecycle projection. */
        void PollUntil(AgentHistorySession &session, const auto &complete) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            do {
                session.Poll();
                if (complete())
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            } while (std::chrono::steady_clock::now() < deadline);
            FAIL("History worker did not reach the expected lifecycle state");
        }

        /** @brief Writes one printable record through production storage and releases its native lease. */
        void Seed(Project &project, const std::string_view title) {
            auto store = project.Open();
            REQUIRE(store.Create(title, 1).HasValue());
        }

        /** @brief Consumes one correlated terminal operation while the owner pumps the session. */
        AgentHistoryMutationCompletion TakeMutation(AgentHistorySession &session, const AgentHistoryOperationId operation) {
            std::optional<AgentHistoryMutationCompletion> completion;
            PollUntil(session, [&] {
                completion = session.TakeMutation();
                return completion.has_value();
            });
            REQUIRE(completion);
            REQUIRE(completion->operation == operation);
            return std::move(*completion);
        }

        /** @brief Parks the test scheduler's only I/O worker until the scope or explicit release. */
        class PauseHistoryIo final {
        public:
            explicit PauseHistoryIo(const JobSystem &jobs) {
                auto wait = released_.get_future().share();
                auto started = std::make_shared<std::promise<void>>();
                auto acknowledged = started->get_future();
                auto queued = jobs.Submit({.resource = JobResource::Io}, [wait = std::move(wait), started](const CancellationToken &) {
                    started->set_value();
                    wait.wait();
                });
                REQUIRE(queued.HasValue());
                job_ = std::move(queued).Value();
                if (acknowledged.wait_for(std::chrono::seconds{5}) != std::future_status::ready) {
                    Release();
                    FAIL("History I/O worker did not begin the test pause");
                }
            }

            ~PauseHistoryIo() {
                Release();
            }

            void Release() {
                if (releasedOnce_)
                    return;
                releasedOnce_ = true;
                released_.set_value();
                static_cast<void>(job_.Wait());
            }

        private:
            std::promise<void> released_;
            JobHandle job_;
            bool releasedOnce_{};
        };
    }  // namespace

    TEST_CASE("Agent history recovers an interrupted turn with stable identities and safe references",
              "[unit][agent][conversation_history]") {
        Project project;
        AgentConversationId id;
        AgentHistoryMessageId response;
        {
            auto store = project.Open();
            REQUIRE(store.Availability() == Availability::Ready);
            const auto created = store.Create("Conversation", 10);
            if (created.HasError())
                UNSCOPED_INFO(created.ErrorValue().code.Value() << ' ' << created.ErrorValue().message);
            REQUIRE(created.HasValue());
            id = created.Value();
            const std::array attachments{AgentHistoryReference{AgentHistoryReferenceKind::Document, 41, 12}};
            const auto begun = store.BeginTurn(id, "visible prompt with private fragment", attachments, 11);
            REQUIRE(begun.HasValue());
            response = begun.Value();
            const std::array toolReferences{AgentHistoryReference{AgentHistoryReferenceKind::ToolCall, 23, 7}};
            REQUIRE(store.Checkpoint(id, response, MessageState::Streaming, "Partial answer", toolReferences, {15, 2}, 12).HasValue());
            const auto content = project.Image();
            CHECK(Contains(content, "SQLite format 3"));
            CHECK_FALSE(Contains(content, "private fragment"));
            CHECK(store.Conversations().front().messages.back().state == MessageState::Streaming);
            const auto concurrent = project.Open();
            CHECK(concurrent.Availability() == Availability::Unavailable);
            CHECK(concurrent.Conversations().empty());
        }
        auto interruptedStage = project.Path();
        interruptedStage += ".source-publication.tmp";
        const std::array partialImage{std::byte{0x53}, std::byte{0x51}};
        REQUIRE(project.files.WriteDurable(interruptedStage, partialImage).HasValue());
        {
            auto recovered = project.Open();
            REQUIRE(recovered.Availability() == Availability::Recovered);
            CHECK_FALSE(std::filesystem::exists(interruptedStage));
            REQUIRE(recovered.Conversations().size() == 1);
            const auto &record = recovered.Conversations().front();
            REQUIRE(record.id == id);
            REQUIRE(record.messages.size() == 2);
            CHECK(record.messages[0].text == "visible prompt with [redacted]");
            REQUIRE(record.messages[0].references.size() == 1);
            CHECK(record.messages[0].references.front().identity == 41);
            CHECK(record.messages[1].id == response);
            CHECK(record.messages[1].state == MessageState::Incomplete);
            CHECK(record.messages[1].text == "Partial answer");
            CHECK(record.messages[1].usage.inputTokens == 15);
            REQUIRE(record.messages[1].references.size() == 1);
            CHECK(record.messages[1].references.front().revision == 7);
            RequireError(recovered.Checkpoint(id, response, MessageState::Complete, "Late response", {}, {}, 13),
                         AgentHistoryErrors::Stale);
        }
        const auto reopened = project.Open();
        REQUIRE(reopened.Availability() == Availability::Ready);
        CHECK(reopened.Conversations().front().messages.back().state == MessageState::Incomplete);
    }

    TEST_CASE("Agent history rejects raw inputs and enforces project and retention boundaries", "[unit][agent][conversation_history]") {
        Project project;
        const AgentHistoryLimits limits{.conversations = 1, .messagesPerConversation = 2};
        AgentConversationId id;
        {
            auto store = project.Open("history-project", limits);
            REQUIRE(store.Availability() == Availability::Ready);
            RequireError(store.Create("credential fixture", 1), AgentHistoryErrors::RejectedText);
            REQUIRE(store.Conversations().empty());
            const auto created = store.Create("First", 1);
            REQUIRE(created.HasValue());
            id = created.Value();
            RequireError(store.Create("Second", 2), AgentHistoryErrors::Capacity);
            for (const auto text : {"credential fixture", "raw attachment fixture", "raw sensor fixture"})
                RequireError(store.BeginTurn(id, text, {}, 2), AgentHistoryErrors::RejectedText);
            const std::array invalid{AgentHistoryReference{AgentHistoryReferenceKind::Asset, 0, 4}};
            RequireError(store.BeginTurn(id, "Prompt", invalid, 2), AgentHistoryErrors::Invalid);
            REQUIRE(store.Conversations().front().messages.empty());
            const auto begun = store.BeginTurn(id, "Prompt", {}, 2);
            REQUIRE(begun.HasValue());
            RequireError(store.Checkpoint(id, begun.Value(), MessageState::Complete, "raw attachment fixture", {}, {}, 3),
                         AgentHistoryErrors::RejectedText);
            RequireError(store.Checkpoint(id, begun.Value(), MessageState::Complete, "Response", {}, {1'000'001, 0}, 3),
                         AgentHistoryErrors::Invalid);
            CHECK(store.Conversations().front().messages.back().state == MessageState::Streaming);
            REQUIRE(store.Checkpoint(id, begun.Value(), MessageState::Complete, "Response", {}, {10, 2}, 3).HasValue());
            RequireError(store.BeginTurn(id, "Second turn", {}, 4), AgentHistoryErrors::Capacity);
            REQUIRE(store.Rename(id, "Updated").HasValue());
            REQUIRE(store.ArchiveBefore(4).HasValue());
            REQUIRE(store.Conversations().front().archived);
            const auto exportText = store.Export(id);
            REQUIRE(exportText.HasValue());
            CHECK(exportText.Value().find("Updated") != std::string::npos);
            CHECK(exportText.Value().find("raw attachment fixture") == std::string::npos);
        }
        {
            const auto foreign = project.Open("another-project", limits);
            CHECK(foreign.Availability() == Availability::ForeignProject);
            CHECK(foreign.Conversations().empty());
        }
        {
            auto reopened = project.Open("history-project", limits);
            REQUIRE(reopened.Availability() == Availability::Ready);
            REQUIRE(reopened.Delete(id).HasValue());
            const auto fresh = reopened.Create("Fresh", 5);
            REQUIRE(fresh.HasValue());
            CHECK(fresh.Value() > id);
        }
    }

    TEST_CASE("Agent history tolerates corrupt and future images without replacing them", "[unit][agent][conversation_history]") {
        Project project;
        Availability expected = Availability::Corrupt;
        std::vector<std::uint8_t> image;
        SECTION("Malformed database") {
            image = {0, 1, 2, 3};
        }
        SECTION("Unknown SQLite history format") {
            {
                auto store = project.Open();
                REQUIRE(store.Create("Prior conversation", 1).HasValue());
            }
            image = project.Image();
            REQUIRE(image.size() >= 64);
            // SQLite records the application-controlled PRAGMA user_version as a big-endian integer at offset 60.
            image[63] = 99;
            expected = Availability::UnsupportedVersion;
        }
        REQUIRE(project.files.WriteDurable(project.Path(), std::as_bytes(std::span{image})).HasValue());
        auto store = project.Open();
        CHECK(store.Availability() == expected);
        CHECK(store.Conversations().empty());
        RequireError(store.Create("New", 1), AgentHistoryErrors::Unavailable);
        CHECK(project.Image() == image);
    }

    TEST_CASE("Editor history switches committed projects on its I/O lane without publishing stale history",
              "[unit][agent][conversation_history]") {
        using Phase = AgentHistorySessionPhase;
        Project first;
        Project second;
        Seed(first, "First history");
        Seed(second, "Second history");
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4, .ioWorkerCount = 1}};
        AgentHistorySession session{jobs, first.files, HistoryFilter};
        const auto settled = [&] {
            return session.View().phase == Phase::Settled;
        };
        const AgentHistoryProject a{first.root, "history-project"};
        const AgentHistoryProject b{second.root, "history-project"};
        REQUIRE(session.View().phase == Phase::NoProject);

        SECTION("Rapid project changes coalesce while the I/O lane is unavailable") {
            PauseHistoryIo pause{jobs};
            session.SelectProject(a);
            CHECK(session.View().phase == Phase::Opening);
            session.SelectProject(b);
            CHECK(session.View().phase == Phase::Opening);
            CHECK(session.View().conversations.empty());
            pause.Release();
        }

        SECTION("Switching hides the open project immediately and releases its lease on the worker") {
            session.SelectProject(a);
            PollUntil(session, settled);
            REQUIRE(session.View().availability == Availability::Ready);
            REQUIRE(session.View().conversations.size() == 1);
            CHECK(session.View().conversations.front().title == "First history");
            CHECK(first.Open().Availability() == Availability::Unavailable);

            session.SelectProject(b);
            CHECK(session.View().phase == Phase::Opening);
            CHECK(session.View().conversations.empty());
        }

        PollUntil(session, settled);
        REQUIRE(session.View().availability == Availability::Ready);
        REQUIRE(session.View().conversations.size() == 1);
        CHECK(session.View().conversations.front().title == "Second history");
        CHECK(first.Open().Availability() == Availability::Ready);
        CHECK(second.Open().Availability() == Availability::Unavailable);
        session.SelectProject(std::nullopt);
        CHECK(session.View().phase == Phase::NoProject);
        CHECK(session.View().conversations.empty());
        PollUntil(session, [&] {
            return second.Open().Availability() == Availability::Ready;
        });
    }

    TEST_CASE("Editor history startup remains available when history cannot open", "[unit][agent][conversation_history]") {
        using Phase = AgentHistorySessionPhase;
        Project project;
        const std::array invalidImage{std::byte{1}, std::byte{2}};
        REQUIRE(project.files.WriteDurable(project.Path(), invalidImage).HasValue());
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 4, .ioWorkerCount = 1}};
        AgentHistorySession session{jobs, project.files, HistoryFilter};
        session.SelectProject(AgentHistoryProject{project.root, "history-project"});
        REQUIRE(session.View().phase == Phase::Opening);
        CHECK(session.View().conversations.empty());
        PollUntil(session, [&] {
            return session.View().phase == Phase::Settled;
        });
        CHECK(session.View().availability == Availability::Corrupt);
        CHECK(session.View().conversations.empty());

        session.Shutdown();
        session.SelectProject(AgentHistoryProject{project.root, "history-project"});
        CHECK(session.View().phase == Phase::NoProject);
    }

    TEST_CASE("Editor history commits visible turns and terminal responses through one bounded writer",
              "[unit][agent][conversation_history]") {
        using Phase = AgentHistorySessionPhase;
        Project project;
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 2, .ioWorkerCount = 1}};
        AgentHistorySession session{jobs, project.files, HistoryFilter};
        session.SelectProject(AgentHistoryProject{project.root, "history-project"});
        PollUntil(session, [&] {
            return session.View().phase == Phase::Settled;
        });
        REQUIRE(session.View().availability == Availability::Ready);
        const auto revision = session.View().project;
        REQUIRE(revision.value != 0);

        const std::string oversized((16U << 10U) + 1U, 'x');
        RequireError(session.BeginTurn(revision, {.title = "Never created", .userText = oversized, .now = 1}),
                     AgentHistoryErrors::Capacity);
        for (const auto text : {"credential fixture", "redactor exception fixture"}) {
            const auto queued = session.BeginTurn(revision, {.title = "Never created", .userText = text, .now = 1});
            REQUIRE(queued.HasValue());
            const auto failed = TakeMutation(session, queued.Value());
            REQUIRE(failed.project == revision);
            REQUIRE(failed.error);
            CHECK(failed.error->code.Value() == AgentHistoryErrors::RejectedText.code.Value());
            CHECK_FALSE(failed.started);
            CHECK(session.View().conversations.empty());
            CHECK_FALSE(std::filesystem::exists(project.Path()));
        }

        auto input = std::string{"visible prompt with private fragment"};
        std::array references{AgentHistoryReference{AgentHistoryReferenceKind::Document, 17, 2}};
        PauseHistoryIo pause{jobs};
        const auto began = session.BeginTurn(revision, {.title = "Turn title", .userText = input, .references = references, .now = 2});
        REQUIRE(began.HasValue());
        input = "rewritten after admission";
        references.front().identity = 99;
        RequireError(session.BeginTurn(revision, {.title = "Busy", .userText = "Busy", .now = 2}), AgentHistoryErrors::Busy);
        CHECK(session.View().conversations.empty());
        pause.Release();
        const auto beginReceipt = TakeMutation(session, began.Value());
        REQUIRE(beginReceipt.started);
        REQUIRE_FALSE(beginReceipt.error);
        REQUIRE(session.View().conversations.size() == 1);
        const auto started = *beginReceipt.started;
        const auto &user = session.View().conversations.front().messages.front();
        CHECK(user.text == "visible prompt with [redacted]");
        REQUIRE(user.references.size() == 1);
        CHECK(user.references.front().identity == 17);
        const auto checkpoint = session.Checkpoint(revision, {.conversation = started.conversation,
                                                              .response = started.response,
                                                              .state = MessageState::Complete,
                                                              .visibleText = "Visible answer",
                                                              .usage = {20, 5},
                                                              .now = 3});
        REQUIRE(checkpoint.HasValue());
        const auto answerReceipt = TakeMutation(session, checkpoint.Value());
        CHECK(answerReceipt.project == revision);
        CHECK_FALSE(answerReceipt.error);
        CHECK_FALSE(answerReceipt.started);
        CHECK(session.View().conversations.front().messages.back().state == MessageState::Complete);
        session.Shutdown();
        const auto reopened = project.Open();
        REQUIRE(reopened.Availability() == Availability::Ready);
        const auto &answer = reopened.Conversations().front().messages.back();
        CHECK(answer.id == started.response);
        CHECK(answer.text == "Visible answer");
        CHECK(answer.usage.outputTokens == 5);
        CHECK_FALSE(Contains(project.Image(), "private fragment"));
    }

    TEST_CASE("Editor history rejects edits when the worker queue is full and drops writes after project change",
              "[unit][agent][conversation_history]") {
        using Phase = AgentHistorySessionPhase;
        Project first;
        Project second;
        Seed(second, "Second project");
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 1, .ioWorkerCount = 1}};
        AgentHistorySession session{jobs, first.files, HistoryFilter};
        session.SelectProject(AgentHistoryProject{first.root, "history-project"});
        const auto settled = [&] {
            return session.View().phase == Phase::Settled;
        };
        PollUntil(session, settled);
        const auto prior = session.View().project;
        PauseHistoryIo pause{jobs};
        auto blocker = jobs.Submit({.resource = JobResource::Io}, [](const CancellationToken &) {
        });
        REQUIRE(blocker.HasValue());
        const AgentHistoryTurnRequest request{.title = "Stale write", .userText = "Stale prompt", .now = 2};
        CHECK(session.BeginTurn(prior, request).HasError());
        CHECK_FALSE(session.TakeMutation());
        CHECK(session.View().conversations.empty());
        REQUIRE(blocker.Value().RequestCancel().HasValue());
        const auto queued = session.BeginTurn(prior, request);
        REQUIRE(queued.HasValue());
        session.SelectProject(AgentHistoryProject{second.root, "history-project"});
        CHECK(session.View().conversations.empty());
        CHECK_FALSE(session.TakeMutation());
        pause.Release();
        PollUntil(session, settled);
        CHECK(session.View().project != prior);
        CHECK_FALSE(session.TakeMutation());
        RequireError(session.BeginTurn(prior, request), AgentHistoryErrors::Stale);
        REQUIRE(session.View().conversations.size() == 1);
        CHECK(session.View().conversations.front().title == "Second project");
        CHECK_FALSE(std::filesystem::exists(first.Path()));
    }

    TEST_CASE("History review stores exactly the approved display pair with credential-bearing text removed",
              "[unit][agent][conversation_history]") {
        using Phase = AgentHistorySessionPhase;
        Project project;
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 2, .ioWorkerCount = 1}};
        AgentHistorySession session{jobs, project.files, RedactHistoryDisplayText};
        const AgentHistoryProject selected{project.root, "history-project"};
        session.SelectProject(selected);
        PollUntil(session, [&] {
            return session.View().phase == Phase::Settled;
        });
        REQUIRE(session.View().availability == Availability::Ready);

        constexpr auto prompt = "Keep this question.\nExample: Api-Key=example-value\nAfter the header.";
        constexpr auto response = "Summary.\nAuthorization: Bearer synthetic-value\n-----BEGIN PRIVATE KEY-----\nprivate fixture line\n"
                                  "-----END PRIVATE KEY-----\nProject answer.";
        const auto review = AgentHistoryExchangeReview::Prepare(session.View().project, prompt, response);
        REQUIRE(review.HasValue());
        CHECK(review.Value().Prompt() == "Keep this question.\n[redacted]\nAfter the header.");
        CHECK(review.Value().Response() == "Summary.\n[redacted]\n[redacted]\nProject answer.");
        CHECK(session.View().conversations.empty());
        CHECK_FALSE(std::filesystem::exists(project.Path()));

        const auto submitted = review.Value().SaveApproved(session, "Local exchange", 2);
        REQUIRE(submitted.HasValue());
        const auto committed = TakeMutation(session, submitted.Value());
        REQUIRE_FALSE(committed.error);
        REQUIRE(committed.started);
        REQUIRE(session.View().conversations.size() == 1);
        const auto &messages = session.View().conversations.front().messages;
        REQUIRE(messages.size() == 2);
        CHECK(messages.front().text == review.Value().Prompt());
        CHECK(messages.back().text == review.Value().Response());
        CHECK(messages.back().state == MessageState::Complete);
        CHECK(messages.front().references.empty());
        CHECK(messages.back().references.empty());
        const auto image = project.Image();
        CHECK_FALSE(Contains(image, "synthetic-value"));
        CHECK_FALSE(Contains(image, "example-value"));
        CHECK_FALSE(Contains(image, "private fixture line"));

        session.SelectProject(std::nullopt);
        RequireError(review.Value().SaveApproved(session, "Local exchange", 3), AgentHistoryErrors::Stale);
    }

    TEST_CASE("History review rejects invalid display bytes and handles incomplete sensitive blocks",
              "[unit][agent][conversation_history]") {
        constexpr AgentHistoryProjectRevision revision{1};
        RequireError(AgentHistoryExchangeReview::Prepare(revision, std::string_view{"not\0display", 11}, "Answer"),
                     AgentHistoryErrors::RejectedText);
        RequireError(AgentHistoryExchangeReview::Prepare(revision, "Prompt", std::string(16U * 1024U + 1U, 'x')),
                     AgentHistoryErrors::Capacity);
        const auto review = AgentHistoryExchangeReview::Prepare(revision, "Here is an example ghp_0123456789abcdef",
                                                                "-----BEGIN OPENSSH PRIVATE KEY-----\nprivate fixture line");
        REQUIRE(review.HasValue());
        CHECK(review.Value().Prompt() == "[redacted]");
        CHECK(review.Value().Response() == "[redacted]\n");
    }

    TEST_CASE("History rejects an unadmitted terminal answer without writing a partial conversation",
              "[unit][agent][conversation_history]") {
        using Phase = AgentHistorySessionPhase;
        Project project;
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 2, .ioWorkerCount = 1}};
        AgentHistorySession session{jobs, project.files, HistoryFilter};
        session.SelectProject(AgentHistoryProject{project.root, "history-project"});
        PollUntil(session, [&] {
            return session.View().phase == Phase::Settled;
        });
        const auto operation =
            session.RecordCompleted(session.View().project, {.title = "Discarded", .userText = "Visible prompt", .now = 1},
                                    {"credential fixture", {}});
        REQUIRE(operation.HasValue());
        const auto failure = TakeMutation(session, operation.Value());
        REQUIRE(failure.error);
        CHECK(failure.error->code.Value() == AgentHistoryErrors::RejectedText.code.Value());
        CHECK(session.View().conversations.empty());
        CHECK_FALSE(std::filesystem::exists(project.Path()));
    }

    TEST_CASE("Editor history manages one exact conversation and exports display data without a file write",
              "[unit][agent][conversation_history]") {
        using Phase = AgentHistorySessionPhase;
        Project project;
        AgentConversationId conversation;
        {
            auto store = project.Open();
            const auto recorded = store.RecordCompleted({.title = "Original", .userText = "visible prompt with private fragment", .now = 2},
                                                        {"Visible answer", {}});
            REQUIRE(recorded.HasValue());
            conversation = recorded.Value().conversation;
        }
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 2, .ioWorkerCount = 1}};
        AgentHistorySession session{jobs, project.files, HistoryFilter};
        session.SelectProject(AgentHistoryProject{project.root, "history-project"});
        PollUntil(session, [&] {
            return session.View().phase == Phase::Settled;
        });
        REQUIRE(session.View().availability == Availability::Ready);
        const auto projectRevision = session.View().project;
        const auto finish = [&session, projectRevision](const Result<AgentHistoryOperationId> &submission) {
            REQUIRE(submission.HasValue());
            auto completion = TakeMutation(session, submission.Value());
            CHECK(completion.project == projectRevision);
            CHECK_FALSE(completion.started);
            return completion;
        };

        RequireError(session.Rename(projectRevision, conversation, std::string(256 + 1, 'x')), AgentHistoryErrors::Capacity);
        const auto denied = finish(session.Rename(projectRevision, conversation, "credential fixture"));
        REQUIRE(denied.error);
        CHECK(denied.error->code.Value() == AgentHistoryErrors::RejectedText.code.Value());
        CHECK(session.View().conversations.front().title == "Original");

        PauseHistoryIo pause{jobs};
        std::string title{"Reviewed name"};
        const auto submitted = session.Rename(projectRevision, conversation, title);
        title = "Edited after queue admission";
        REQUIRE(submitted.HasValue());
        RequireError(session.Delete(projectRevision, conversation), AgentHistoryErrors::Busy);
        pause.Release();
        CHECK_FALSE(finish(submitted).error);
        CHECK(session.View().conversations.front().title == "Reviewed name");

        CHECK_FALSE(finish(session.SetArchived(projectRevision, conversation, true)).error);
        CHECK(session.View().conversations.front().archived);
        const auto persisted = project.Image();
        const auto exported = finish(session.Export(projectRevision, conversation));
        REQUIRE_FALSE(exported.error);
        REQUIRE(exported.exportedText);
        CHECK(exported.exportedText->find("Reviewed name") != std::string::npos);
        CHECK(exported.exportedText->find("Visible answer") != std::string::npos);
        CHECK(exported.exportedText->find("private fragment") == std::string::npos);
        CHECK(project.Image() == persisted);
        const auto directory = std::filesystem::directory_iterator{project.Path().parent_path()};
        CHECK(std::distance(directory, std::filesystem::directory_iterator{}) == 1);

        CHECK_FALSE(finish(session.SetArchived(projectRevision, conversation, false)).error);
        CHECK_FALSE(session.View().conversations.front().archived);
        CHECK_FALSE(finish(session.Delete(projectRevision, conversation)).error);
        CHECK(session.View().conversations.empty());
        const auto deleted = finish(session.Export(projectRevision, conversation));
        REQUIRE(deleted.error);
        CHECK(deleted.error->code.Value() == AgentHistoryErrors::Stale.code.Value());
        CHECK_FALSE(deleted.exportedText);
        session.Shutdown();
        const auto reopened = project.Open();
        REQUIRE(reopened.Availability() == Availability::Ready);
        CHECK(reopened.Conversations().empty());
    }

    TEST_CASE("Editor history discards a queued export across exact project replacement", "[unit][agent][conversation_history]") {
        using Phase = AgentHistorySessionPhase;
        Project first;
        Project second;
        Seed(first, "First project history");
        Seed(second, "Second project history");
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 2, .ioWorkerCount = 1}};
        AgentHistorySession session{jobs, first.files, HistoryFilter};
        session.SelectProject(AgentHistoryProject{first.root, "history-project"});
        const auto settled = [&session] {
            return session.View().phase == Phase::Settled;
        };
        PollUntil(session, settled);
        REQUIRE(session.View().conversations.size() == 1);
        const auto oldRevision = session.View().project;
        const auto reusedConversationId = session.View().conversations.front().id;
        PauseHistoryIo pause{jobs};
        REQUIRE(session.Export(oldRevision, reusedConversationId).HasValue());
        session.SelectProject(AgentHistoryProject{second.root, "history-project"});
        CHECK_FALSE(session.TakeMutation());
        pause.Release();
        PollUntil(session, settled);
        CHECK_FALSE(session.TakeMutation());
        REQUIRE(session.View().conversations.size() == 1);
        CHECK(session.View().conversations.front().id == reusedConversationId);
        RequireError(session.Export(oldRevision, reusedConversationId), AgentHistoryErrors::Stale);
        const auto current = session.Export(session.View().project, reusedConversationId);
        REQUIRE(current.HasValue());
        const auto exported = TakeMutation(session, current.Value());
        REQUIRE(exported.exportedText);
        CHECK(exported.exportedText->find("Second project history") != std::string::npos);
        CHECK(exported.exportedText->find("First project history") == std::string::npos);
    }

    TEST_CASE("An explicitly approved recoverable prompt survives interruption without saving the unapproved response",
              "[unit][agent][conversation_history]") {
        using Phase = AgentHistorySessionPhase;
        Project project;
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 2, .ioWorkerCount = 1}};
        AgentHistorySession session{jobs, project.files, RedactHistoryDisplayText};
        session.SelectProject(AgentHistoryProject{project.root, "history-project"});
        PollUntil(session, [&session] {
            return session.View().phase == Phase::Settled;
        });
        const auto revision = session.View().project;
        const auto review = AgentHistoryPromptReview::Prepare(revision, "Visible prompt.\nPassword: prompt-secret");
        REQUIRE(review.HasValue());
        CHECK(review.Value().Prompt() == "Visible prompt.\n[redacted]");
        CHECK_FALSE(std::filesystem::exists(project.Path()));
        const auto start = review.Value().StartApproved(session, "Recoverable local ask", 2);
        REQUIRE(start.HasValue());
        const auto accepted = TakeMutation(session, start.Value());
        REQUIRE_FALSE(accepted.error);
        REQUIRE(accepted.started);
        const AgentHistoryRecoveryTurn recovery{accepted.project, *accepted.started};
        const auto &saved = session.View().conversations.front().messages;
        REQUIRE(saved.size() == 2);
        CHECK(saved.front().text == review.Value().Prompt());
        CHECK(saved.back().state == MessageState::Streaming);
        CHECK(saved.back().text.empty());
        const auto checkpoint = project.Image();
        CHECK_FALSE(Contains(checkpoint, "prompt-secret"));

        const auto answer = AgentHistoryAnswerReview::Prepare(recovery, "Visible answer.\nAuthorization: Bearer response-secret");
        REQUIRE(answer.HasValue());
        CHECK(answer.Value().Answer() == "Visible answer.\n[redacted]");
        CHECK(project.Image() == checkpoint);
        bool approved{};
        SECTION("Ending the owner before approval keeps the response empty and recovers it as incomplete") {}
        SECTION("Explicit answer approval updates the same response without appending a second conversation") {
            const auto save = answer.Value().SaveApproved(session, 3);
            REQUIRE(save.HasValue());
            const auto settled = TakeMutation(session, save.Value());
            REQUIRE_FALSE(settled.error);
            CHECK_FALSE(settled.started);
            approved = true;
        }
        session.Shutdown();
        const auto reopened = project.Open();
        REQUIRE(reopened.Availability() == (approved ? Availability::Ready : Availability::Recovered));
        REQUIRE(reopened.Conversations().size() == 1);
        const auto &conversation = reopened.Conversations().front();
        CHECK(conversation.id == recovery.turn.conversation);
        REQUIRE(conversation.messages.size() == 2);
        CHECK(conversation.messages.back().id == recovery.turn.response);
        CHECK(conversation.messages.back().state == (approved ? MessageState::Complete : MessageState::Incomplete));
        CHECK(conversation.messages.back().text == (approved ? answer.Value().Answer() : std::string_view{}));
        const auto image = project.Image();
        CHECK_FALSE(Contains(image, "response-secret"));
        CHECK(Contains(image, "Visible answer.") == approved);
    }
}  // namespace Horo::Agent
