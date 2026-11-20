#include "AgentConversationStore.h"

#include "Horo/Assets/AssetSourcePublication.h"
#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>
#include <ranges>
#include <sqlite3.h>

namespace Horo::Agent {
    namespace {
        constexpr std::size_t MaximumSnapshotBytes = 8U << 20U;
        constexpr std::size_t DatabaseOverhead = 64U << 10U;
        constexpr std::size_t MaximumTitleBytes = 256;
        constexpr std::uint32_t MaximumTokenCount = 1'000'000;
        constexpr int HistoryApplication = 0x48414948;  // HAIH
        constexpr int HistoryVersion = 1;
        constexpr auto HistoryPath = ".horo/local/agent/conversations.sqlite";
        constexpr auto HistoryTable =
            "CREATE TABLE history(id INTEGER PRIMARY KEY CHECK(id = 1), project TEXT NOT NULL, payload TEXT NOT NULL)";
        using StoreAvailability = AgentHistoryAvailability;
        using MessageState = AgentHistoryMessageState;
        using Role = AgentHistoryRole;
        using Json = nlohmann::json;

        /** @brief Creates a payload-free diagnostic suitable for history availability UI. */
        ErrorCodeDescriptor HistoryError(std::string code, std::string message) {
            return {ErrorDomainId{"editor.agent.history"},
                    ErrorCode{std::move(code)},
                    ErrorSeverity::Error,
                    std::move(message),
                    {},
                    false,
                    false,
                    {}};
        }

        struct History final {
            std::uint64_t nextConversation{1};
            std::uint64_t nextMessage{1};
            std::vector<AgentConversation> conversations;
        };

        /** @brief Checks configured upper bounds before storage or parsing is admitted. */
        bool ValidLimits(const AgentHistoryLimits &limits) {
            return limits.conversations > 0 && limits.conversations <= 256 && limits.messagesPerConversation >= 2 &&
                   limits.messagesPerConversation <= 1024 && limits.textBytesPerMessage > 0 && limits.textBytesPerMessage <= (64U << 10U) &&
                   limits.referencesPerMessage <= 32 && limits.snapshotBytes >= 256 && limits.snapshotBytes <= MaximumSnapshotBytes;
        }

        /** @brief Checks canonical UTF-8 without accepting an embedded terminator. */
        bool ValidText(const std::string_view text, const std::size_t limit) {
            return text.size() <= limit && text.find('\0') == std::string_view::npos && IsValidUtf8ScalarSequence(text);
        }

        /** @brief Validates bounded producer evidence without resolving any attachment. */
        bool ValidReference(const AgentHistoryReference &reference) {
            return reference.kind <= AgentHistoryReferenceKind::ToolCall && reference.identity != 0 && reference.revision != 0;
        }

        /** @brief Validates numeric usage without retaining provider metadata. */
        bool ValidUsage(const AgentHistoryUsage usage) {
            return usage.inputTokens <= MaximumTokenCount && usage.outputTokens <= MaximumTokenCount;
        }

        /** @brief Returns whether this conversation currently owns a streaming response. */
        bool IsStreaming(const AgentConversation &conversation) {
            return !conversation.messages.empty() && conversation.messages.back().state == MessageState::Streaming;
        }

        /** @brief Encodes the bounded display fields of one conversation. */
        Json EncodeConversation(const AgentConversation &conversation) {
            auto messages = Json::array();
            for (const auto &message : conversation.messages) {
                auto references = Json::array();
                for (const auto &reference : message.references)
                    references.push_back(Json::array({reference.kind, reference.identity, reference.revision}));
                messages.push_back({{"id", message.id.value},
                                    {"role", message.role},
                                    {"state", message.state},
                                    {"text", message.text},
                                    {"references", std::move(references)},
                                    {"usage", Json::array({message.usage.inputTokens, message.usage.outputTokens})}});
            }
            return {{"id", conversation.id.value},
                    {"title", conversation.title},
                    {"updated", conversation.updatedUnixSeconds},
                    {"archived", conversation.archived},
                    {"messages", std::move(messages)}};
        }

        /** @brief Encodes one complete candidate for atomic replacement. */
        std::string EncodeHistory(const History &history) {
            auto conversations = Json::array();
            for (const auto &conversation : history.conversations)
                conversations.push_back(EncodeConversation(conversation));
            return Json{{"nextConversation", history.nextConversation},
                        {"nextMessage", history.nextMessage},
                        {"conversations", std::move(conversations)}}
                .dump();
        }

        /** @brief Reads a nonnegative JSON integer without coercion or signed wrapping. */
        std::uint64_t ReadUnsigned(const Json &node) {
            if (!node.is_number_unsigned())
                throw std::invalid_argument{"Invalid history integer"};
            return node.get<std::uint64_t>();
        }

        /** @brief Decodes bounded numeric reference metadata. */
        AgentHistoryReference DecodeReference(const Json &node) {
            if (!node.is_array() || node.size() != 3)
                throw std::invalid_argument{"Invalid history reference"};
            const auto kind = ReadUnsigned(node[0]);
            if (kind > static_cast<std::uint8_t>(AgentHistoryReferenceKind::ToolCall))
                throw std::invalid_argument{"Invalid history reference kind"};
            const AgentHistoryReference reference{static_cast<AgentHistoryReferenceKind>(kind), ReadUnsigned(node[1]),
                                                  ReadUnsigned(node[2])};
            if (!ValidReference(reference))
                throw std::invalid_argument{"Invalid history reference identity"};
            return reference;
        }

        /** @brief Decodes one sanitized message under the active retention policy. */
        AgentHistoryMessage DecodeMessage(const Json &node, const AgentHistoryLimits &limits) {
            if (!node.is_object() || node.size() != 6)
                throw std::invalid_argument{"Invalid history message"};
            const auto role = ReadUnsigned(node.at("role"));
            const auto state = ReadUnsigned(node.at("state"));
            const auto &text = node.at("text").get_ref<const std::string &>();
            const auto &references = node.at("references");
            const auto &usage = node.at("usage");
            if (role > static_cast<std::uint8_t>(Role::Assistant) || state > static_cast<std::uint8_t>(MessageState::Failed) ||
                !ValidText(text, limits.textBytesPerMessage) || !references.is_array() || references.size() > limits.referencesPerMessage ||
                !usage.is_array() || usage.size() != 2)
                throw std::invalid_argument{"Invalid history message fields"};
            const auto input = ReadUnsigned(usage[0]);
            const auto output = ReadUnsigned(usage[1]);
            if (input > MaximumTokenCount || output > MaximumTokenCount)
                throw std::invalid_argument{"Invalid history usage"};
            AgentHistoryMessage message{{ReadUnsigned(node.at("id"))},
                                        static_cast<Role>(role),
                                        static_cast<MessageState>(state),
                                        text,
                                        {},
                                        {static_cast<std::uint32_t>(input), static_cast<std::uint32_t>(output)}};
            message.references.reserve(references.size());
            for (const auto &reference : references)
                message.references.push_back(DecodeReference(reference));
            return message;
        }

        /** @brief Decodes the paired user/assistant sequence and checks its only recoverable in-progress state. */
        AgentConversation DecodeConversation(const Json &node, const AgentHistoryLimits &limits) {
            if (!node.is_object() || node.size() != 5)
                throw std::invalid_argument{"Invalid history conversation"};
            const auto &title = node.at("title").get_ref<const std::string &>();
            const auto &messages = node.at("messages");
            if (title.empty() || !ValidText(title, MaximumTitleBytes) || !messages.is_array() ||
                messages.size() > limits.messagesPerConversation || messages.size() % 2 != 0)
                throw std::invalid_argument{"Invalid history conversation fields"};
            AgentConversation conversation{{ReadUnsigned(node.at("id"))},
                                           title,
                                           ReadUnsigned(node.at("updated")),
                                           node.at("archived").get<bool>(),
                                           {}};
            if (conversation.updatedUnixSeconds == 0)
                throw std::invalid_argument{"Invalid history timestamp"};
            conversation.messages.reserve(messages.size());
            for (std::size_t index = 0; index < messages.size(); ++index) {
                auto message = DecodeMessage(messages[index], limits);
                const auto expected = index % 2 == 0 ? Role::User : Role::Assistant;
                if (message.role != expected ||
                    (message.role == Role::User && (message.state != MessageState::Complete || message.text.empty())) ||
                    (message.state == MessageState::Streaming && (conversation.archived || index + 1 != messages.size())) ||
                    (!conversation.messages.empty() && message.id <= conversation.messages.back().id))
                    throw std::invalid_argument{"Invalid history sequence"};
                conversation.messages.push_back(std::move(message));
            }
            return conversation;
        }

        /** @brief Checks the exact bounded snapshot and all lifetime identities before publication. */
        History DecodeHistory(const std::string_view text, const AgentHistoryLimits &limits) {
            bool nestingExceeded{};
            const auto node = Json::parse(text, [&](const int depth, Json::parse_event_t, const Json &) {
                nestingExceeded |= depth > 8;
                return !nestingExceeded;
            });
            if (nestingExceeded || !node.is_object() || node.size() != 3)
                throw std::invalid_argument{"Invalid history snapshot"};
            const auto &conversations = node.at("conversations");
            History history{ReadUnsigned(node.at("nextConversation")), ReadUnsigned(node.at("nextMessage")), {}};
            if (history.nextConversation == 0 || history.nextMessage == 0 || !conversations.is_array() ||
                conversations.size() > limits.conversations)
                throw std::invalid_argument{"Invalid history limits"};
            std::vector<AgentHistoryMessageId> messageIds;
            for (const auto &record : conversations) {
                auto conversation = DecodeConversation(record, limits);
                if (conversation.id.value == 0 || conversation.id.value >= history.nextConversation ||
                    (!history.conversations.empty() && conversation.id <= history.conversations.back().id))
                    throw std::invalid_argument{"Invalid history conversation ID"};
                for (const auto &message : conversation.messages) {
                    if (message.id.value == 0 || message.id.value >= history.nextMessage)
                        throw std::invalid_argument{"Invalid history message ID"};
                    messageIds.push_back(message.id);
                }
                history.conversations.push_back(std::move(conversation));
            }
            std::ranges::sort(messageIds);
            if (std::ranges::adjacent_find(messageIds) != messageIds.end())
                throw std::invalid_argument{"Repeated history message ID"};
            return history;
        }

        using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
        using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

        /** @brief Opens an isolated SQLite image. Filesystem access remains owned by the durable publication boundary. */
        Database OpenImage(const AgentHistoryLimits &limits) {
            sqlite3 *raw{};
            const auto status = sqlite3_open_v2(":memory:", &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_MEMORY, nullptr);
            Database database{raw, sqlite3_close};
            if (status != SQLITE_OK)
                return {nullptr, sqlite3_close};
            if (sqlite3_db_config(raw, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr) != SQLITE_OK ||
                sqlite3_db_config(raw, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr) != SQLITE_OK)
                return {nullptr, sqlite3_close};
            sqlite3_limit(raw, SQLITE_LIMIT_LENGTH, static_cast<int>(limits.snapshotBytes + 1024));
            sqlite3_limit(raw, SQLITE_LIMIT_ATTACHED, 0);
            sqlite3_limit(raw, SQLITE_LIMIT_VDBE_OP, 512);
            return database;
        }

        /** @brief Prepares a single fixed-schema statement without virtual tables. */
        Statement Prepare(sqlite3 *database, const char *sql) {
            sqlite3_stmt *statement{};
            sqlite3_prepare_v3(database, sql, -1, SQLITE_PREPARE_NO_VTAB, &statement, nullptr);
            return {statement, sqlite3_finalize};
        }

        /** @brief Reads one bounded integer from the SQLite header. */
        std::optional<int> HeaderValue(sqlite3 *database, const char *sql) {
            const auto statement = Prepare(database, sql);
            if (!statement || sqlite3_step(statement.get()) != SQLITE_ROW)
                return std::nullopt;
            return sqlite3_column_int(statement.get(), 0);
        }

        /** @brief Borrows a checked SQLite text field only until its statement advances. */
        std::optional<std::string_view> ReadText(sqlite3_stmt *statement, const int column, const std::size_t maximum) {
            if (sqlite3_column_type(statement, column) != SQLITE_TEXT)
                return std::nullopt;
            const auto *bytes = sqlite3_column_text(statement, column);
            const auto size = static_cast<std::size_t>(sqlite3_column_bytes(statement, column));
            if (!bytes || size > maximum)
                return std::nullopt;
            return std::string_view{reinterpret_cast<const char *>(bytes), size};
        }

        /** @brief Accepts only the ordinary table owned by this exact history schema version. */
        bool HasHistoryTable(sqlite3 *database) {
            const auto schema = Prepare(database, "SELECT sql FROM main.sqlite_schema WHERE type = 'table' AND name = 'history'");
            return schema && sqlite3_step(schema.get()) == SQLITE_ROW && ReadText(schema.get(), 0, 256) == HistoryTable &&
                   sqlite3_step(schema.get()) == SQLITE_DONE;
        }

        /** @brief Reads the supported SQLite version and exact project before accepting the snapshot. */
        StoreAvailability ReadImage(std::span<std::byte> bytes, const std::string_view project, const AgentHistoryLimits &limits,
                                    History &history) {
            const auto database = OpenImage(limits);
            if (!database)
                return StoreAvailability::Unavailable;
            const auto size = static_cast<sqlite3_int64>(bytes.size());
            if (bytes.empty() || sqlite3_deserialize(database.get(), "main", reinterpret_cast<unsigned char *>(bytes.data()), size, size,
                                                     SQLITE_DESERIALIZE_READONLY) != SQLITE_OK)
                return StoreAvailability::Corrupt;
            if (HeaderValue(database.get(), "PRAGMA application_id") != HistoryApplication)
                return StoreAvailability::Corrupt;
            const auto version = HeaderValue(database.get(), "PRAGMA user_version");
            if (!version)
                return StoreAvailability::Corrupt;
            if (*version != HistoryVersion)
                return StoreAvailability::UnsupportedVersion;
            if (!HasHistoryTable(database.get()))
                return StoreAvailability::Corrupt;
            const auto statement = Prepare(database.get(), "SELECT project, payload FROM main.history WHERE id = 1");
            if (!statement || sqlite3_step(statement.get()) != SQLITE_ROW)
                return StoreAvailability::Corrupt;
            const auto storedProject = ReadText(statement.get(), 0, 128);
            const auto text = ReadText(statement.get(), 1, limits.snapshotBytes);
            if (!storedProject || !text)
                return StoreAvailability::Corrupt;
            if (*storedProject != project)
                return StoreAvailability::ForeignProject;
            history = DecodeHistory(*text, limits);
            return sqlite3_step(statement.get()) == SQLITE_DONE ? StoreAvailability::Ready : StoreAvailability::Corrupt;
        }

        /** @brief Produces a complete formatted SQLite image without a database or journal sidecar on disk. */
        Result<std::vector<std::byte>> WriteImage(const History &history, const std::string_view project,
                                                  const AgentHistoryLimits &limits) {
            using Output = Result<std::vector<std::byte>>;
            const auto text = EncodeHistory(history);
            if (text.size() > limits.snapshotBytes)
                return Output::Failure(MakeError(AgentHistoryErrors::Capacity));
            const auto database = OpenImage(limits);
            if (!database)
                return Output::Failure(MakeError(AgentHistoryErrors::Unavailable));
            const auto schema = "PRAGMA application_id = " + std::to_string(HistoryApplication) +
                                "; PRAGMA user_version = " + std::to_string(HistoryVersion) + "; " + HistoryTable;
            if (sqlite3_exec(database.get(), schema.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
                return Output::Failure(MakeError(AgentHistoryErrors::Unavailable));
            const auto statement = Prepare(database.get(), "INSERT INTO history VALUES (1, ?1, ?2)");
            if (!statement ||
                sqlite3_bind_text(statement.get(), 1, project.data(), static_cast<int>(project.size()), SQLITE_STATIC) != SQLITE_OK ||
                sqlite3_bind_text(statement.get(), 2, text.data(), static_cast<int>(text.size()), SQLITE_STATIC) != SQLITE_OK ||
                sqlite3_step(statement.get()) != SQLITE_DONE)
                return Output::Failure(MakeError(AgentHistoryErrors::Unavailable));
            sqlite3_int64 size{};
            const auto freeImage = [](std::byte *data) {
                sqlite3_free(data);
            };
            const std::unique_ptr<std::byte, decltype(freeImage)> image{reinterpret_cast<std::byte *>(
                                                                            sqlite3_serialize(database.get(), "main", &size, 0)),
                                                                        freeImage};
            if (!image || size <= 0 || static_cast<std::uint64_t>(size) > limits.snapshotBytes + DatabaseOverhead)
                return Output::Failure(MakeError(AgentHistoryErrors::Unavailable));
            return Output::Success({image.get(), image.get() + size});
        }

        /** @brief Retires only the legacy writer's reserved sibling after an interrupted pre-commit write. */
        Result<void> RecoverPublicationStage(DurableFileSystem &files, const std::filesystem::path &path) {
            auto stage = path;
            stage += ".source-publication.tmp";
            const auto entry = files.InspectEntry(stage);
            if (entry.HasError())
                return Result<void>::Failure(entry.ErrorValue());
            if (!entry.Value())
                return Result<void>::Success();
            if (entry.Value()->kind != FileEntryKind::RegularFile || entry.Value()->hardLinks != 1)
                return Result<void>::Failure(MakeError(AgentHistoryErrors::Unavailable));
            return files.RemoveDurable(stage);
        }
    }  // namespace

    namespace AgentHistoryErrors {
        const ErrorCodeDescriptor Invalid = HistoryError("editor.agent.history.invalid", "Invalid conversation request.");
        const ErrorCodeDescriptor Capacity = HistoryError("editor.agent.history.capacity", "Conversation history storage is full.");
        const ErrorCodeDescriptor Stale = HistoryError("editor.agent.history.stale", "The conversation or response is no longer current.");
        const ErrorCodeDescriptor RejectedText =
            HistoryError("editor.agent.history.rejected_text", "This text was not admitted to history.");
        const ErrorCodeDescriptor Unavailable = HistoryError("editor.agent.history.unavailable", "Conversation history is unavailable.");
    }  // namespace AgentHistoryErrors

    struct AgentConversationStore::State final {
        DurableFileSystem &files;
        std::filesystem::path root;
        std::filesystem::path path;
        std::string project;
        AgentHistoryTextFilter textFilter;
        AgentHistoryLimits limits;
        ExclusiveFileLock lease;
        Assets::AssetSourceIdentity identity;
        History history;
        StoreAvailability availability{StoreAvailability::Unavailable};

        /** @brief Returns whether this owner is allowed to replace the accepted image. */
        [[nodiscard]] bool Writable() const noexcept {
            return availability == StoreAvailability::Ready || availability == StoreAvailability::Recovered ||
                   availability == StoreAvailability::DurabilityUnconfirmed;
        }

        /** @brief Gets bounded, display-only text from the host history admission boundary. */
        [[nodiscard]] Result<std::string> Admit(const AgentHistoryTextKind kind, const std::string_view text, const std::size_t limit,
                                                const bool required) const {
            using Output = Result<std::string>;
            if (!ValidText(text, limit) || (required && text.empty()))
                return Output::Failure(MakeError(AgentHistoryErrors::Invalid));
            if (text.empty())
                return Output::Success({});
            auto decision = textFilter(kind, text);
            if (decision.classification != Security::AgentDataClass::EditorContext || !decision.admittedText ||
                !ValidText(*decision.admittedText, limit) || (required && decision.admittedText->empty()))
                return Output::Failure(MakeError(AgentHistoryErrors::RejectedText));
            return Output::Success(std::move(*decision.admittedText));
        }

        /** @brief Publish only complete images under the exact source identity and retained exclusive lock. */
        [[nodiscard]] Result<void> Commit(History next) {
            auto image = WriteImage(next, project, limits);
            if (image.HasError())
                return Result<void>::Failure(image.ErrorValue());
            const auto published = Assets::PublishAssetSource(root, path, identity, image.Value(), lease, files);
            if (published.HasError())
                return Result<void>::Failure(published.ErrorValue());
            identity = published.Value().identity;
            history = std::move(next);
            availability = published.Value().status == Assets::AssetSourcePublicationStatus::Durable
                               ? StoreAvailability::Ready
                               : StoreAvailability::DurabilityUnconfirmed;
            return Result<void>::Success();
        }

        /** @brief Update an exact conversation in a candidate snapshot and leave the old snapshot on failure. */
        template <typename Mutation> [[nodiscard]] Result<void> Edit(const AgentConversationId id, Mutation mutate) {
            auto next = history;
            const auto conversation = std::ranges::find(next.conversations, id, &AgentConversation::id);
            if (conversation == next.conversations.end())
                return Result<void>::Failure(MakeError(AgentHistoryErrors::Stale));
            auto changed = mutate(*conversation);
            return changed.HasError() ? changed : Commit(std::move(next));
        }
    };

    AgentConversationStore::AgentConversationStore(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    AgentConversationStore::~AgentConversationStore() = default;
    AgentConversationStore::AgentConversationStore(AgentConversationStore &&) noexcept = default;
    AgentConversationStore &AgentConversationStore::operator=(AgentConversationStore &&) noexcept = default;

    /** @copydoc AgentConversationStore::Open */
    AgentConversationStore AgentConversationStore::Open(DurableFileSystem &files, const std::filesystem::path &projectRoot,
                                                        const std::string_view projectId, AgentHistoryTextFilter textFilter,
                                                        const AgentHistoryLimits &limits) {
        auto state = std::make_unique<State>(State{files, {}, {}, {}, std::move(textFilter), limits});
        AgentConversationStore store{std::move(state)};
        auto &owner = *store.state_;
        if (!owner.textFilter || !projectRoot.is_absolute() || projectId.empty() || !ValidText(projectId, 128) || !ValidLimits(limits))
            return store;
        owner.project = projectId;

        std::error_code error;
        owner.root = std::filesystem::canonical(projectRoot, error);
        if (error || !std::filesystem::is_directory(owner.root, error) || error)
            return store;
        owner.path = owner.root / HistoryPath;
        const auto lockPath = Assets::AssetSourcePublicationLockPath(owner.root, owner.path);
        if (lockPath.HasError())
            return store;
        auto lease = files.TryAcquireExclusive(lockPath.Value(), "editor-agent-history");
        if (lease.HasError())
            return store;
        auto captured = Assets::CaptureAssetSource(owner.root, owner.path, limits.snapshotBytes + DatabaseOverhead);
        if (captured.HasError())
            return store;
        auto source = std::move(captured).Value();
        owner.lease = std::move(lease).Value();
        owner.identity = source.identity;
        if (!owner.identity.exists) {
            const auto directory = files.CreateDirectoryPath(owner.path.parent_path(), DirectoryCreationMode::Parents);
            if (directory.HasValue() && RecoverPublicationStage(files, owner.path).HasValue())
                owner.availability = StoreAvailability::Ready;
            else
                owner.lease = {};
            return store;
        }

        History recovered;
        try {
            owner.availability = ReadImage(source.bytes, owner.project, limits, recovered);
        } catch (const Json::exception &) {
            owner.availability = StoreAvailability::Corrupt;
        } catch (const std::invalid_argument &) {
            owner.availability = StoreAvailability::Corrupt;
        }
        if (!owner.Writable()) {
            owner.lease = {};
            return store;
        }
        if (RecoverPublicationStage(files, owner.path).HasError()) {
            owner.availability = StoreAvailability::Unavailable;
            owner.lease = {};
            return store;
        }
        bool interrupted{};
        for (auto &conversation : recovered.conversations) {
            if (IsStreaming(conversation)) {
                conversation.messages.back().state = MessageState::Incomplete;
                interrupted = true;
            }
        }
        if (!interrupted) {
            owner.history = std::move(recovered);
            return store;
        }
        if (owner.Commit(std::move(recovered)).HasError()) {
            owner.availability = StoreAvailability::Unavailable;
            owner.lease = {};
        } else if (owner.availability == StoreAvailability::Ready) {
            owner.availability = StoreAvailability::Recovered;
        }
        return store;
    }

    /** @copydoc AgentConversationStore::Availability */
    AgentHistoryAvailability AgentConversationStore::Availability() const noexcept {
        return state_ ? state_->availability : StoreAvailability::Unavailable;
    }

    /** @copydoc AgentConversationStore::Conversations */
    std::span<const AgentConversation> AgentConversationStore::Conversations() const noexcept {
        return state_ ? std::span<const AgentConversation>{state_->history.conversations} : std::span<const AgentConversation>{};
    }

    /** @copydoc AgentConversationStore::Create */
    Result<AgentConversationId> AgentConversationStore::Create(const std::string_view title, const std::uint64_t now) {
        using Output = Result<AgentConversationId>;
        if (!state_ || !state_->Writable())
            return Output::Failure(MakeError(AgentHistoryErrors::Unavailable));
        if (now == 0)
            return Output::Failure(MakeError(AgentHistoryErrors::Invalid));
        if (state_->history.conversations.size() == state_->limits.conversations ||
            state_->history.nextConversation == std::numeric_limits<std::uint64_t>::max())
            return Output::Failure(MakeError(AgentHistoryErrors::Capacity));
        auto admitted = state_->Admit(AgentHistoryTextKind::Title, title, MaximumTitleBytes, true);
        if (admitted.HasError())
            return Output::Failure(admitted.ErrorValue());
        auto next = state_->history;
        const AgentConversationId id{next.nextConversation++};
        next.conversations.emplace_back(id, std::move(admitted).Value(), now);
        const auto committed = state_->Commit(std::move(next));
        return committed.HasValue() ? Output::Success(id) : Output::Failure(committed.ErrorValue());
    }

    /** @copydoc AgentConversationStore::Rename */
    Result<void> AgentConversationStore::Rename(const AgentConversationId conversation, const std::string_view title) {
        if (!state_ || !state_->Writable())
            return Result<void>::Failure(MakeError(AgentHistoryErrors::Unavailable));
        auto admitted = state_->Admit(AgentHistoryTextKind::Title, title, MaximumTitleBytes, true);
        if (admitted.HasError())
            return Result<void>::Failure(admitted.ErrorValue());
        return state_->Edit(conversation, [&](AgentConversation &record) {
            record.title = std::move(admitted).Value();
            return Result<void>::Success();
        });
    }

    /** @copydoc AgentConversationStore::SetArchived */
    Result<void> AgentConversationStore::SetArchived(const AgentConversationId conversation, const bool archived) {
        if (!state_ || !state_->Writable())
            return Result<void>::Failure(MakeError(AgentHistoryErrors::Unavailable));
        return state_->Edit(conversation, [&](AgentConversation &record) {
            if (IsStreaming(record))
                return Result<void>::Failure(MakeError(AgentHistoryErrors::Stale));
            record.archived = archived;
            return Result<void>::Success();
        });
    }

    /** @copydoc AgentConversationStore::Delete */
    Result<void> AgentConversationStore::Delete(const AgentConversationId conversation) {
        if (!state_ || !state_->Writable())
            return Result<void>::Failure(MakeError(AgentHistoryErrors::Unavailable));
        auto next = state_->history;
        const auto found = std::ranges::find(next.conversations, conversation, &AgentConversation::id);
        if (found == next.conversations.end() || IsStreaming(*found))
            return Result<void>::Failure(MakeError(AgentHistoryErrors::Stale));
        next.conversations.erase(found);
        return state_->Commit(std::move(next));
    }

    /** @copydoc AgentConversationStore::ArchiveBefore */
    Result<void> AgentConversationStore::ArchiveBefore(const std::uint64_t idleBeforeUnixSeconds) {
        if (!state_ || !state_->Writable())
            return Result<void>::Failure(MakeError(AgentHistoryErrors::Unavailable));
        auto next = state_->history;
        bool changed{};
        for (auto &conversation : next.conversations) {
            if (!conversation.archived && conversation.updatedUnixSeconds < idleBeforeUnixSeconds && !IsStreaming(conversation)) {
                conversation.archived = true;
                changed = true;
            }
        }
        return changed ? state_->Commit(std::move(next)) : Result<void>::Success();
    }

    /** @copydoc AgentConversationStore::Export */
    Result<std::string> AgentConversationStore::Export(const AgentConversationId conversation) const {
        using Output = Result<std::string>;
        if (!state_ || !state_->Writable())
            return Output::Failure(MakeError(AgentHistoryErrors::Unavailable));
        const auto found = std::ranges::find(state_->history.conversations, conversation, &AgentConversation::id);
        if (found == state_->history.conversations.end())
            return Output::Failure(MakeError(AgentHistoryErrors::Stale));
        return Output::Success(Json{{"version", HistoryVersion}, {"conversation", EncodeConversation(*found)}}.dump());
    }

    /** @copydoc AgentConversationStore::BeginTurn */
    Result<AgentHistoryMessageId> AgentConversationStore::BeginTurn(const AgentConversationId conversation, const std::string_view userText,
                                                                    const std::span<const AgentHistoryReference> references,
                                                                    const std::uint64_t now) {
        using Output = Result<AgentHistoryMessageId>;
        if (!state_ || !state_->Writable())
            return Output::Failure(MakeError(AgentHistoryErrors::Unavailable));
        if (references.size() > state_->limits.referencesPerMessage || !std::ranges::all_of(references, ValidReference))
            return Output::Failure(MakeError(AgentHistoryErrors::Invalid));
        auto admitted = state_->Admit(AgentHistoryTextKind::UserMessage, userText, state_->limits.textBytesPerMessage, true);
        if (admitted.HasError())
            return Output::Failure(admitted.ErrorValue());
        auto next = state_->history;
        const auto found = std::ranges::find(next.conversations, conversation, &AgentConversation::id);
        if (found == next.conversations.end() || found->archived || IsStreaming(*found) || now < found->updatedUnixSeconds)
            return Output::Failure(MakeError(AgentHistoryErrors::Stale));
        if (found->messages.size() > state_->limits.messagesPerConversation - 2 ||
            next.nextMessage > std::numeric_limits<std::uint64_t>::max() - 2)
            return Output::Failure(MakeError(AgentHistoryErrors::Capacity));
        const AgentHistoryMessageId prompt{next.nextMessage++};
        const AgentHistoryMessageId response{next.nextMessage++};
        found->messages.push_back(
            {prompt, Role::User, MessageState::Complete, std::move(admitted).Value(), {references.begin(), references.end()}, {}});
        found->messages.push_back({response, Role::Assistant, MessageState::Streaming, {}, {}, {}});
        found->updatedUnixSeconds = now;
        const auto committed = state_->Commit(std::move(next));
        return committed.HasValue() ? Output::Success(response) : Output::Failure(committed.ErrorValue());
    }

    /** @copydoc AgentConversationStore::Checkpoint */
    Result<void> AgentConversationStore::Checkpoint(const AgentConversationId conversation, const AgentHistoryMessageId response,
                                                    const AgentHistoryMessageState state, const std::string_view visibleText,
                                                    const std::span<const AgentHistoryReference> references, const AgentHistoryUsage usage,
                                                    const std::uint64_t now) {
        if (!state_ || !state_->Writable())
            return Result<void>::Failure(MakeError(AgentHistoryErrors::Unavailable));
        if (state > MessageState::Failed || references.size() > state_->limits.referencesPerMessage ||
            !std::ranges::all_of(references, ValidReference) || !ValidUsage(usage))
            return Result<void>::Failure(MakeError(AgentHistoryErrors::Invalid));
        auto admitted = state_->Admit(AgentHistoryTextKind::AssistantMessage, visibleText, state_->limits.textBytesPerMessage, false);
        if (admitted.HasError())
            return Result<void>::Failure(admitted.ErrorValue());
        return state_->Edit(conversation, [&](AgentConversation &record) {
            if (!IsStreaming(record) || record.messages.back().id != response || now < record.updatedUnixSeconds)
                return Result<void>::Failure(MakeError(AgentHistoryErrors::Stale));
            auto &message = record.messages.back();
            message.state = state;
            message.text = std::move(admitted).Value();
            message.references.assign(references.begin(), references.end());
            message.usage = usage;
            record.updatedUnixSeconds = now;
            return Result<void>::Success();
        });
    }
}  // namespace Horo::Agent
