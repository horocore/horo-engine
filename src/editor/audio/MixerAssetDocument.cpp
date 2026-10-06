#include "Horo/Editor/MixerAssetDocument.h"

#include "MixerDocumentCommands.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace Horo::Editor {
    namespace MixerDocumentErrors {
        const ErrorCodeDescriptor InvalidCommand{ErrorDomainId{"editor.mixer"}, ErrorCode{"editor.mixer.command.invalid"},
                                                 ErrorSeverity::Error, "Mixer command has an invalid identity, reference or position.",
                                                 "Refresh the source projection."};
        const ErrorCodeDescriptor StaleRevision{ErrorDomainId{"editor.mixer"}, ErrorCode{"editor.mixer.revision.stale"},
                                                ErrorSeverity::Error, "Mixer source revision is stale.",
                                                "Capture the current document revision."};
        const ErrorCodeDescriptor HistoryUnavailable{ErrorDomainId{"editor.mixer"}, ErrorCode{"editor.mixer.history.unavailable"},
                                                     ErrorSeverity::Error, "Mixer history step is unavailable.",
                                                     "Query the current history state."};
        const ErrorCodeDescriptor HistoryLimit{ErrorDomainId{"editor.mixer"}, ErrorCode{"editor.mixer.history.limit"}, ErrorSeverity::Error,
                                               "Mixer transaction exceeds the bounded history budget.", "Submit a smaller transaction."};
        const ErrorCodeDescriptor DirtyDocument{ErrorDomainId{"editor.mixer"}, ErrorCode{"editor.mixer.document.dirty"},
                                                ErrorSeverity::Error, "Mixer document has unsaved changes.",
                                                "Save or explicitly discard changes."};
        const ErrorCodeDescriptor Closed{ErrorDomainId{"editor.mixer"}, ErrorCode{"editor.mixer.document.closed"}, ErrorSeverity::Error,
                                         "Mixer document is closed.", "Open a document session."};
        const ErrorCodeDescriptor StateExhausted{ErrorDomainId{"editor.mixer"}, ErrorCode{"editor.mixer.state.exhausted"},
                                                 ErrorSeverity::Error, "Mixer document identity sequence is exhausted.",
                                                 "Open a new session."};
    }  // namespace MixerDocumentErrors

    namespace {
        /** @brief Semantic before/after values of one stable authored element, not a whole-document snapshot. */
        template <typename T> struct Change final {
            std::optional<T> before;
            std::optional<T> after;
            std::size_t beforeIndex{};
            std::size_t afterIndex{};
        };

        /** @brief One bounded committed transaction with exact source-state identity. */
        struct HistoryEntry final {
            std::vector<Change<Audio::MixerBusDescriptor>> buses;
            std::vector<Change<Audio::MixerRouteDescriptor>> routes;
            std::uint64_t beforeState{};
            std::uint64_t afterState{};
            std::size_t bytes{sizeof(HistoryEntry)};
        };

        /** @brief Measures owned variable-size bus history data against the history budget. */
        std::size_t PayloadBytes(const Audio::MixerBusDescriptor &bus) {
            return bus.displayName.size() + bus.effects.size() * sizeof(Audio::MixerEffectDescriptor) +
                   bus.layout.orderedChannels.size() * sizeof(Audio::AudioChannelRole);
        }

        /** @brief Routes contain only bounded inline data. */
        std::size_t PayloadBytes(const Audio::MixerRouteDescriptor &) {
            return 0;
        }

        /** @brief Computes stable-ID deltas while retaining source presentation positions. */
        template <typename T>
        std::vector<Change<T>> Changes(const std::vector<T> &before, const std::vector<T> &after, std::size_t &bytes) {
            std::vector<Change<T>> changes;
            for (std::size_t index = 0; index < before.size(); ++index) {
                const auto found = std::ranges::find(after, before[index].id, &T::id);
                if (found != after.end() && *found == before[index] && static_cast<std::size_t>(found - after.begin()) == index)
                    continue;
                Change<T> change{before[index], {}, index, 0};
                bytes += sizeof(Change<T>) + PayloadBytes(before[index]);
                if (found != after.end()) {
                    change.after = *found;
                    change.afterIndex = static_cast<std::size_t>(found - after.begin());
                    bytes += PayloadBytes(*found);
                }
                changes.push_back(std::move(change));
            }
            for (std::size_t index = 0; index < after.size(); ++index) {
                if (std::ranges::find(before, after[index].id, &T::id) != before.end())
                    continue;
                changes.push_back({{}, after[index], 0, index});
                bytes += sizeof(Change<T>) + PayloadBytes(after[index]);
            }
            return changes;
        }

        /** @brief Applies stable element deltas at their authored positions to a detached candidate. */
        template <typename T> void ApplyChanges(std::vector<T> &values, const std::vector<Change<T>> &changes, const bool undo) {
            for (const auto &change : changes) {
                const auto id = change.before ? change.before->id : change.after->id;
                std::erase_if(values, [&](const T &value) {
                    return value.id == id;
                });
            }
            std::vector<const Change<T> *> insertions;
            for (const auto &change : changes) {
                if (undo ? change.before.has_value() : change.after.has_value())
                    insertions.push_back(&change);
            }
            std::ranges::sort(insertions, [undo](const Change<T> *left, const Change<T> *right) {
                return (undo ? left->beforeIndex : left->afterIndex) < (undo ? right->beforeIndex : right->afterIndex);
            });
            for (const auto *change : insertions) {
                const auto index = undo ? change->beforeIndex : change->afterIndex;
                HORO_INVARIANT(index <= values.size());
                values.insert(values.begin() + static_cast<std::ptrdiff_t>(index), undo ? *change->before : *change->after);
            }
        }

        /** @brief Rejects unknown dirty-content policies before a lifecycle action. */
        bool ValidPolicy(const MixerDocumentDiscardPolicy policy) {
            return policy == MixerDocumentDiscardPolicy::RequireClean || policy == MixerDocumentDiscardPolicy::DiscardChanges;
        }
    }  // namespace

    /** @brief Owner-thread authored state and bounded semantic history; no runtime or UI references. */
    struct MixerAssetDocument::State final {
        DocumentIdentity identity;
        Audio::MixerAssetSchema asset;
        Audio::MixerAssetSchemaLimits limits;
        std::vector<HistoryEntry> history;
        std::size_t cursor{};
        std::uint64_t revision{1};
        std::uint64_t currentState{1};
        std::uint64_t savedState{1};
        std::uint64_t nextState{2};
        std::uint64_t firstState{1}; /**< Reload barrier: pre-reload captures cannot mark a new source saved. */
    };

    MixerAssetDocument::MixerAssetDocument(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    MixerAssetDocument::MixerAssetDocument(MixerAssetDocument &&) noexcept = default;
    MixerAssetDocument &MixerAssetDocument::operator=(MixerAssetDocument &&) noexcept = default;
    MixerAssetDocument::~MixerAssetDocument() = default;

    /** @copydoc MixerAssetDocument::Open */
    Result<MixerAssetDocument> MixerAssetDocument::Open(DocumentIdentity identity, const Audio::MixerAssetSchema &source,
                                                        const Audio::MixerAssetSchemaLimits limits) {
        if (!identity.IsValid() || identity.key.kind != DocumentKind::Asset)
            return Result<MixerAssetDocument>::Failure(MakeError(MixerDocumentErrors::InvalidCommand));
        auto migrated = Audio::MigrateMixerAssetSchema(source, limits);
        if (migrated.HasError())
            return Result<MixerAssetDocument>::Failure(std::move(migrated).ErrorValue());
        auto state = std::make_unique<State>();
        state->identity = std::move(identity);
        state->asset = std::move(migrated).Value();
        state->limits = limits;
        if (source.version != Audio::CurrentMixerAssetSchemaVersion)
            state->savedState = 0;
        return Result<MixerAssetDocument>::Success(MixerAssetDocument{std::move(state)});
    }

    /** @copydoc MixerAssetDocument::Snapshot */
    Result<MixerDocumentSnapshot> MixerAssetDocument::Snapshot() const {
        if (IsClosed())
            return Result<MixerDocumentSnapshot>::Failure(MakeError(MixerDocumentErrors::Closed));
        return Result<MixerDocumentSnapshot>::Success({state_->identity, state_->revision, state_->currentState, state_->asset});
    }

    /** @copydoc MixerAssetDocument::Revision */
    std::uint64_t MixerAssetDocument::Revision() const noexcept {
        return state_ ? state_->revision : 0;
    }

    /** @copydoc MixerAssetDocument::IsDirty */
    bool MixerAssetDocument::IsDirty() const noexcept {
        return state_ && state_->currentState != state_->savedState;
    }

    /** @copydoc MixerAssetDocument::IsClosed */
    bool MixerAssetDocument::IsClosed() const noexcept {
        return !state_;
    }

    /** @copydoc MixerAssetDocument::CanUndo */
    bool MixerAssetDocument::CanUndo() const noexcept {
        return state_ && state_->cursor != 0;
    }

    /** @copydoc MixerAssetDocument::CanRedo */
    bool MixerAssetDocument::CanRedo() const noexcept {
        return state_ && state_->cursor < state_->history.size();
    }

    /** @copydoc MixerAssetDocument::CheckRevision */
    Result<void> MixerAssetDocument::CheckRevision(const std::uint64_t expectedRevision) const {
        if (IsClosed())
            return Result<void>::Failure(MakeError(MixerDocumentErrors::Closed));
        if (expectedRevision != state_->revision)
            return Result<void>::Failure(MakeError(MixerDocumentErrors::StaleRevision));
        if (state_->revision == std::numeric_limits<std::uint64_t>::max() || state_->nextState == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(MixerDocumentErrors::StateExhausted));
        return Result<void>::Success();
    }

    /** @copydoc MixerAssetDocument::Execute */
    Result<void> MixerAssetDocument::Execute(const std::uint64_t expectedRevision, const std::span<const MixerDocumentCommand> commands) {
        auto checked = CheckRevision(expectedRevision);
        if (checked.HasError())
            return checked;
        if (commands.empty() || commands.size() > MaximumMixerDocumentCommands)
            return Result<void>::Failure(MakeError(MixerDocumentErrors::InvalidCommand));
        auto candidate = state_->asset;
        for (const auto &command : commands) {
            auto applied = MixerDocumentInternal::Apply(candidate, command, state_->limits);
            if (applied.HasError())
                return applied;
        }
        auto validated = Audio::ValidateMixerAssetSchema(candidate, state_->limits);
        if (validated.HasError())
            return validated;
        if (candidate == state_->asset)
            return Result<void>::Success();
        HistoryEntry entry;
        entry.buses = Changes(state_->asset.buses, candidate.buses, entry.bytes);
        entry.routes = Changes(state_->asset.routes, candidate.routes, entry.bytes);
        entry.beforeState = state_->currentState;
        entry.afterState = state_->nextState;
        if (entry.bytes > MaximumMixerDocumentHistoryBytes)
            return Result<void>::Failure(MakeError(MixerDocumentErrors::HistoryLimit));
        // Allocate the new history before touching redo or any authoritative state.
        auto history = state_->history;
        history.resize(state_->cursor);
        history.push_back(std::move(entry));
        std::size_t bytes = 0;
        for (const auto &retained : history)
            bytes += retained.bytes;
        while (history.size() > MaximumMixerDocumentHistoryEntries || bytes > MaximumMixerDocumentHistoryBytes) {
            bytes -= history.front().bytes;
            history.erase(history.begin());
        }
        state_->history.swap(history);
        state_->cursor = state_->history.size();
        std::swap(state_->asset, candidate);
        state_->currentState = state_->nextState++;
        ++state_->revision;
        return Result<void>::Success();
    }

    /** @copydoc MixerAssetDocument::ApplyHistory */
    Result<void> MixerAssetDocument::ApplyHistory(const std::uint64_t expectedRevision, const bool undo) {
        auto checked = CheckRevision(expectedRevision);
        if (checked.HasError())
            return checked;
        if (undo ? !CanUndo() : !CanRedo())
            return Result<void>::Failure(MakeError(MixerDocumentErrors::HistoryUnavailable));
        const auto &entry = state_->history[undo ? state_->cursor - 1 : state_->cursor];
        auto candidate = state_->asset;
        ApplyChanges(candidate.buses, entry.buses, undo);
        ApplyChanges(candidate.routes, entry.routes, undo);
        auto validated = Audio::ValidateMixerAssetSchema(candidate, state_->limits);
        if (validated.HasError())
            return validated;
        std::swap(state_->asset, candidate);
        state_->currentState = undo ? entry.beforeState : entry.afterState;
        if (undo)
            --state_->cursor;
        else
            ++state_->cursor;
        ++state_->revision;
        return Result<void>::Success();
    }

    /** @copydoc MixerAssetDocument::Undo */
    Result<void> MixerAssetDocument::Undo(const std::uint64_t expectedRevision) {
        return ApplyHistory(expectedRevision, true);
    }

    /** @copydoc MixerAssetDocument::Redo */
    Result<void> MixerAssetDocument::Redo(const std::uint64_t expectedRevision) {
        return ApplyHistory(expectedRevision, false);
    }

    /** @copydoc MixerAssetDocument::MarkSaved */
    Result<void> MixerAssetDocument::MarkSaved(const MixerDocumentSnapshot &snapshot) {
        if (IsClosed())
            return Result<void>::Failure(MakeError(MixerDocumentErrors::Closed));
        if (snapshot.Identity() != state_->identity || snapshot.Revision() > state_->revision || snapshot.State() < state_->firstState ||
            snapshot.State() >= state_->nextState)
            return Result<void>::Failure(MakeError(MixerDocumentErrors::StaleRevision));
        auto validated = Audio::ValidateMixerAssetSchema(snapshot.Asset(), state_->limits);
        if (validated.HasError())
            return validated;
        state_->savedState = snapshot.State();
        return Result<void>::Success();
    }

    /** @copydoc MixerAssetDocument::Reload */
    Result<void> MixerAssetDocument::Reload(const Audio::MixerAssetSchema &source, const MixerDocumentDiscardPolicy policy) {
        auto checked = CheckRevision(Revision());
        if (checked.HasError())
            return checked;
        if (!ValidPolicy(policy))
            return Result<void>::Failure(MakeError(MixerDocumentErrors::InvalidCommand));
        if (IsDirty() && policy == MixerDocumentDiscardPolicy::RequireClean)
            return Result<void>::Failure(MakeError(MixerDocumentErrors::DirtyDocument));
        auto migrated = Audio::MigrateMixerAssetSchema(source, state_->limits);
        if (migrated.HasError())
            return Result<void>::Failure(std::move(migrated).ErrorValue());
        auto candidate = std::move(migrated).Value();
        std::swap(state_->asset, candidate);
        state_->history.clear();
        state_->cursor = 0;
        state_->currentState = state_->nextState++;
        state_->firstState = state_->currentState;
        state_->savedState = source.version == Audio::CurrentMixerAssetSchemaVersion ? state_->currentState : 0;
        ++state_->revision;
        return Result<void>::Success();
    }

    /** @copydoc MixerAssetDocument::Close */
    Result<void> MixerAssetDocument::Close(const MixerDocumentDiscardPolicy policy) {
        if (!ValidPolicy(policy))
            return Result<void>::Failure(MakeError(MixerDocumentErrors::InvalidCommand));
        if (IsDirty() && policy == MixerDocumentDiscardPolicy::RequireClean)
            return Result<void>::Failure(MakeError(MixerDocumentErrors::DirtyDocument));
        state_.reset();
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
