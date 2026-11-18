#include "Horo/Editor/BlackboardSchemaDocument.h"

#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Editor/SemanticDocumentHistory.h"

#include <algorithm>
#include <limits>
#include <thread>
#include <type_traits>

namespace Horo::Editor {
    namespace BlackboardDocumentErrors {
        namespace {
            const ErrorDomainId Domain{"editor.blackboard"};
        }

        const ErrorCodeDescriptor InvalidEdit{Domain, ErrorCode{"editor.blackboard.edit.invalid"}, ErrorSeverity::Error,
                                              "Blackboard document identity, row label or edit is invalid.",
                                              "Check the typed key and row position."};
        const ErrorCodeDescriptor StaleRevision{Domain, ErrorCode{"editor.blackboard.revision.stale"}, ErrorSeverity::Error,
                                                "The Blackboard edit refers to an older document revision.",
                                                "Capture the current document rows."};
        const ErrorCodeDescriptor
            ReferenceResolutionRequired{Domain, ErrorCode{"editor.blackboard.references.unresolved"}, ErrorSeverity::Error,
                                        "Changing or removing an existing Blackboard key requires reference resolution.",
                                        "Resolve decision and query references before applying this schema change."};
        const ErrorCodeDescriptor HistoryUnavailable{Domain, ErrorCode{"editor.blackboard.history.unavailable"}, ErrorSeverity::Error,
                                                     "This Blackboard history step is unavailable.",
                                                     "Inspect the current undo and redo state."};
        const ErrorCodeDescriptor HistoryCapacity{Domain, ErrorCode{"editor.blackboard.history.capacity"}, ErrorSeverity::Error,
                                                  "The Blackboard edit exceeds the history budget.", "Use a smaller edit."};
        const ErrorCodeDescriptor IdentityExhausted{Domain, ErrorCode{"editor.blackboard.identity.exhausted"}, ErrorSeverity::Error,
                                                    "The Blackboard document has exhausted its stable identities.",
                                                    "Open the latest source revision."};
        const ErrorCodeDescriptor WrongThread{Domain, ErrorCode{"editor.blackboard.thread.wrong"}, ErrorSeverity::Error,
                                              "The Blackboard document requires its owner thread.", "Dispatch through the document owner."};
        const ErrorCodeDescriptor Cancelled{Domain,
                                            ErrorCode{"editor.blackboard.edit.cancelled"},
                                            ErrorSeverity::Error,
                                            "The Blackboard edit was cancelled before publication.",
                                            {}};
    }  // namespace BlackboardDocumentErrors

    namespace {
        using namespace AI;
        using namespace BlackboardDocumentErrors;
        using Schema = std::shared_ptr<const BlackboardSchema>;

        constexpr SemanticHistoryLimits HistoryLimits{64, 512 * 1024};

        /** @brief Exact authored row before or after an edit, including the stable table order. */
        struct PositionedRow final {
            BlackboardSchemaRow row;
            std::size_t index{};
        };

        /** @brief The one affected row only; reserved history owns no document or runtime root. */
        struct RowDelta final {
            std::optional<PositionedRow> before;
            std::optional<PositionedRow> after;
            std::size_t bytes{};
        };

        /** @brief Captures all descriptors through the existing immutable runtime validator. */
        Result<Schema> CaptureSchema(const BlackboardSchemaSource &source) {
            if (source.rows.size() > MaximumBlackboardKeys)
                return Result<Schema>::Failure(MakeError(AIErrors::BlackboardLimitExceeded));
            std::vector<BlackboardKeyDescriptor> descriptors;
            descriptors.reserve(source.rows.size());
            for (const auto &row : source.rows) {
                if (row.displayName && !IsValidBlackboardDisplayName(*row.displayName))
                    return Result<Schema>::Failure(MakeError(InvalidEdit));
                descriptors.push_back(row.descriptor);
            }
            auto captured = BlackboardSchema::Capture({source.schema, source.version, source.unknownValues, descriptors});
            if (captured.HasError())
                return Result<Schema>::Failure(captured.ErrorValue());
            return Result<Schema>::Success(std::make_shared<const BlackboardSchema>(std::move(captured).Value()));
        }

        /** @brief Previews one exact validated typed command without mutating the live source. */
        Result<RowDelta> MakeDelta(const BlackboardSchemaSource &source, const BlackboardSchemaEdit &edit, const std::uint64_t issuedKey) {
            using Delta = Result<RowDelta>;
            return std::visit([&](const auto &command) -> Delta {
                using Command = std::decay_t<decltype(command)>;
                const auto &rows = source.rows;
                if constexpr (std::is_same_v<Command, AddBlackboardKey>) {
                    if (issuedKey == std::numeric_limits<std::uint64_t>::max())
                        return Delta::Failure(MakeError(IdentityExhausted));
                    if (command.row.descriptor.key.Value() != issuedKey + 1 || command.index > rows.size() ||
                        (command.row.displayName && !IsValidBlackboardDisplayName(*command.row.displayName)))
                        return Delta::Failure(MakeError(InvalidEdit));
                    if (rows.size() == MaximumBlackboardKeys)
                        return Delta::Failure(MakeError(AIErrors::BlackboardLimitExceeded));
                    return Delta::Success({.after = PositionedRow{command.row, command.index}});
                } else {
                    BlackboardKeyId key;
                    if constexpr (std::is_same_v<Command, EditBlackboardKey>)
                        key = command.descriptor.key;
                    else
                        key = command.key;
                    const auto found = std::ranges::find_if(rows, [key](const auto &row) {
                        return row.descriptor.key == key;
                    });
                    if (found == rows.end())
                        return Delta::Failure(MakeError(InvalidEdit));
                    PositionedRow before{*found, static_cast<std::size_t>(found - rows.begin())};
                    if constexpr (std::is_same_v<Command, RemoveBlackboardKey>) {
                        return Delta::Success({.before = std::move(before)});
                    } else {
                        auto after = before;
                        if constexpr (std::is_same_v<Command, RenameBlackboardKey>) {
                            if (!IsValidBlackboardDisplayName(command.displayName))
                                return Delta::Failure(MakeError(InvalidEdit));
                            after.row.displayName = command.displayName;
                        }
                        if constexpr (std::is_same_v<Command, MoveBlackboardKey>) {
                            if (command.index >= rows.size())
                                return Delta::Failure(MakeError(InvalidEdit));
                            after.index = command.index;
                        }
                        if constexpr (std::is_same_v<Command, EditBlackboardKey>)
                            after.row.descriptor = command.descriptor;
                        return Delta::Success({.before = std::move(before), .after = std::move(after)});
                    }
                }
            }, edit);
        }
    }  // namespace

    struct BlackboardSchemaDocument::State final {
        const std::thread::id lane{std::this_thread::get_id()};
        DocumentIdentity document;
        std::shared_ptr<const BlackboardSchemaSource> source;
        Schema runtime;
        Schema opened;
        SemanticDocumentHistory<RowDelta> history{HistoryLimits};
        std::uint64_t revision{1};
        std::uint64_t issuedKey{};

        /** @brief Rejects wrong-thread, stale and cancelled mutations before borrowed state is accessed. */
        [[nodiscard]] Result<void> Check(std::uint64_t expected, CancellationToken cancellation) const {
            if (lane != std::this_thread::get_id())
                return Result<void>::Failure(MakeError(WrongThread));
            if (expected != revision)
                return Result<void>::Failure(MakeError(StaleRevision));
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(Cancelled));
            if (revision == std::numeric_limits<std::uint64_t>::max())
                return Result<void>::Failure(MakeError(IdentityExhausted));
            return Result<void>::Success();
        }

        /** @brief Creates a complete editable root off to the side. Existing durable key semantics require host review. */
        [[nodiscard]] Result<void> Publish(const std::optional<PositionedRow> &before, const std::optional<PositionedRow> &after,
                                           CancellationToken cancellation) {
            const bool affectsSchema = !before || !after || before->row.descriptor != after->row.descriptor;
            if (affectsSchema && before) {
                const auto key = before->row.descriptor.key;
                const auto keys = opened->Keys();
                const auto original = std::ranges::lower_bound(keys, key, {}, &BlackboardKeyDescriptor::key);
                if (original != keys.end() && original->key == key)
                    return Result<void>::Failure(MakeError(ReferenceResolutionRequired));
            }

            auto next = std::make_shared<BlackboardSchemaSource>(*source);
            if (before)
                next->rows.erase(next->rows.begin() + static_cast<std::ptrdiff_t>(before->index));
            if (after)
                next->rows.insert(next->rows.begin() + static_cast<std::ptrdiff_t>(after->index), after->row);

            auto schema = runtime;
            if (affectsSchema) {
                auto validated = CaptureSchema(*next);
                if (validated.HasError())
                    return Result<void>::Failure(validated.ErrorValue());
                schema = std::move(validated).Value();
            }
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(Cancelled));
            source = std::move(next);
            runtime = std::move(schema);
            ++revision;
            return Result<void>::Success();
        }
    };

    /** @copydoc BlackboardSchemaDocument::Open */
    Result<std::unique_ptr<BlackboardSchemaDocument>> BlackboardSchemaDocument::Open(DocumentIdentity document,
                                                                                     BlackboardSchemaSource source) {
        using Document = Result<std::unique_ptr<BlackboardSchemaDocument>>;
        if (!document.IsValid() || document.key.kind != DocumentKind::Asset || !source.asset.IsValid())
            return Document::Failure(MakeError(InvalidEdit));
        auto schema = CaptureSchema(source);
        if (schema.HasError())
            return Document::Failure(schema.ErrorValue());
        auto state = std::make_unique<State>();
        state->document = std::move(document);
        state->source = std::make_shared<const BlackboardSchemaSource>(std::move(source));
        state->runtime = std::move(schema).Value();
        state->opened = state->runtime;
        state->issuedKey = state->runtime->Keys().back().key.Value();
        return Document::Success(std::unique_ptr<BlackboardSchemaDocument>{new BlackboardSchemaDocument(std::move(state))});
    }

    /** @copydoc BlackboardSchemaDocument::OpenRegistered */
    Result<std::unique_ptr<BlackboardSchemaDocument>> BlackboardSchemaDocument::OpenRegistered(
        DocumentIdentity document, const Assets::AssetRegistrySnapshot &registry, const Assets::AssetId asset,
        const std::span<const std::uint8_t> bytes) {
        using Document = Result<std::unique_ptr<BlackboardSchemaDocument>>;
        const auto *record = registry.Find(asset);
        if (!record || record->type.Value() != BlackboardSchemaAssetType || record->sourcePath.Value() != document.key.source.Value())
            return Document::Failure(MakeError(InvalidEdit));
        auto decoded = BlackboardSchemaSourceCodec::Decode(bytes);
        if (decoded.HasError())
            return Document::Failure(decoded.ErrorValue());

        auto data = std::move(decoded).Value();
        BlackboardSchemaSource source{asset, data.schema.Identity(), data.schema.Version(), data.schema.UnknownValuePolicy()};
        source.rows.reserve(data.rows.size());
        const auto keys = data.schema.Keys();
        for (auto &row : data.rows) {
            const auto key = std::ranges::lower_bound(keys, row.key, {}, &BlackboardKeyDescriptor::key);
            source.rows.push_back({*key, std::move(row.displayName)});
        }
        return Open(std::move(document), std::move(source));
    }

    /** @brief Retains one validated owner state. */
    BlackboardSchemaDocument::BlackboardSchemaDocument(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    BlackboardSchemaDocument::~BlackboardSchemaDocument() = default;

    /** @copydoc BlackboardSchemaDocument::Snapshot */
    Result<BlackboardSchemaDocumentSnapshot> BlackboardSchemaDocument::Snapshot() const {
        using Snapshot = Result<BlackboardSchemaDocumentSnapshot>;
        if (state_->lane != std::this_thread::get_id())
            return Snapshot::Failure(MakeError(WrongThread));
        return Snapshot::Success({state_->document, state_->revision, state_->source, state_->runtime});
    }

    /** @copydoc BlackboardSchemaDocument::EncodeSource */
    Result<std::vector<std::uint8_t>> BlackboardSchemaDocument::EncodeSource(const std::uint64_t expectedRevision) const {
        using Encoded = Result<std::vector<std::uint8_t>>;
        if (state_->lane != std::this_thread::get_id())
            return Encoded::Failure(MakeError(WrongThread));
        if (expectedRevision != state_->revision)
            return Encoded::Failure(MakeError(StaleRevision));

        std::vector<BlackboardSchemaAuthoringRow> rows;
        rows.reserve(state_->source->rows.size());
        for (const auto &source : state_->source->rows)
            rows.push_back({source.descriptor.key, source.displayName});
        return BlackboardSchemaSourceCodec::Encode(*state_->runtime, rows);
    }

    /** @copydoc BlackboardSchemaDocument::NextKeyId */
    Result<BlackboardKeyId> BlackboardSchemaDocument::NextKeyId() const {
        if (state_->lane != std::this_thread::get_id())
            return Result<BlackboardKeyId>::Failure(MakeError(WrongThread));
        if (state_->issuedKey == std::numeric_limits<std::uint64_t>::max())
            return Result<BlackboardKeyId>::Failure(MakeError(IdentityExhausted));
        return BlackboardKeyId::Create(state_->issuedKey + 1);
    }

    /** @copydoc BlackboardSchemaDocument::Execute */
    Result<void> BlackboardSchemaDocument::Execute(const std::uint64_t expectedRevision, const BlackboardSchemaEdit &edit,
                                                   const CancellationToken cancellation) {
        if (auto valid = state_->Check(expectedRevision, cancellation); valid.HasError())
            return valid;
        auto prepared = MakeDelta(*state_->source, edit, state_->issuedKey);
        if (prepared.HasError())
            return Result<void>::Failure(prepared.ErrorValue());
        auto delta = std::move(prepared).Value();
        if (delta.before && delta.after && delta.before->index == delta.after->index &&
            delta.before->row.descriptor == delta.after->row.descriptor && delta.before->row.displayName == delta.after->row.displayName)
            return Result<void>::Success();
        delta.bytes = sizeof(RowDelta);
        if (delta.before && delta.before->row.displayName)
            delta.bytes += delta.before->row.displayName->size();
        if (delta.after && delta.after->row.displayName)
            delta.bytes += delta.after->row.displayName->size();

        auto history = state_->history.Prepare(delta);
        if (!history)
            return Result<void>::Failure(MakeError(HistoryCapacity));
        if (auto published = state_->Publish(delta.before, delta.after, cancellation); published.HasError())
            return published;
        state_->history.Commit(*history);
        if (!delta.before && delta.after)
            state_->issuedKey = delta.after->row.descriptor.key.Value();
        return Result<void>::Success();
    }

    /** @copydoc BlackboardSchemaDocument::Undo */
    Result<void> BlackboardSchemaDocument::Undo(const std::uint64_t expectedRevision, const CancellationToken cancellation) {
        return Replay(expectedRevision, false, cancellation);
    }

    /** @copydoc BlackboardSchemaDocument::Redo */
    Result<void> BlackboardSchemaDocument::Redo(const std::uint64_t expectedRevision, const CancellationToken cancellation) {
        return Replay(expectedRevision, true, cancellation);
    }

    /** @copydoc BlackboardSchemaDocument::Replay */
    Result<void> BlackboardSchemaDocument::Replay(const std::uint64_t expectedRevision, const bool redo,
                                                  const CancellationToken cancellation) {
        if (auto valid = state_->Check(expectedRevision, cancellation); valid.HasError())
            return valid;
        const auto *delta = state_->history.Next(redo);
        if (!delta)
            return Result<void>::Failure(MakeError(HistoryUnavailable));
        const auto &before = redo ? delta->before : delta->after;
        const auto &after = redo ? delta->after : delta->before;
        if (auto published = state_->Publish(before, after, cancellation); published.HasError())
            return published;
        state_->history.DidReplay(redo);
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
