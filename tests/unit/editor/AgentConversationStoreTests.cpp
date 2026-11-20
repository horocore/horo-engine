#include "AgentConversationStore.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>

namespace Horo::Agent {
    namespace {
        using Availability = AgentHistoryAvailability;
        using MessageState = AgentHistoryMessageState;

        /** @brief Classifies the controlled test vocabulary and models a host-owned redaction callback. */
        AgentHistoryTextDecision HistoryFilter(AgentHistoryTextKind, const std::string_view visible) {
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
}  // namespace Horo::Agent
