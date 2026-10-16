#include "Horo/Editor/NavigationDefinitionDocument.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::Editor {
    namespace {
        /** @brief Constructs typed workflow rejections without losing lower-layer errors. */
        Error Reject(Navigation::NavigationSettingsFailure reason) {
            return MakeError(Navigation::NavigationSettingsError(reason));
        }

        /** @brief Bounds retained source memory using its canonical record representation. */
        Result<std::size_t> Bytes(const Navigation::NavigationDefinition &source) {
            auto record = Navigation::EncodeNavigationDefinitionRecord(source, Navigation::NavigationAuthoredRecordId::Create(1).Value());
            if (record.HasError())
                return Result<std::size_t>::Failure(record.ErrorValue());
            return Result<std::size_t>::Success(record.Value().payload.size());
        }
    }  // namespace

    /** @copydoc CopyNavigationDefinition */
    Navigation::NavigationDefinitionInput CopyNavigationDefinition(const Navigation::NavigationDefinition &source) {
        return {{source.Profiles().begin(), source.Profiles().end()},
                {source.Registry().Areas().begin(), source.Registry().Areas().end()},
                {source.Registry().Filters().begin(), source.Registry().Filters().end()},
                source.Coordinates(),
                source.Tiles(),
                source.Scope()};
    }

    /** @copydoc NavigationDefinitionDocument::NavigationDefinitionDocument */
    NavigationDefinitionDocument::NavigationDefinitionDocument(NavigationDefinitionDocument &&other) noexcept
        : lifetime_(std::move(other.lifetime_)), asset_(other.asset_), session_(other.session_), revision_(other.revision_),
          state_(other.state_), savedState_(other.savedState_), nextState_(other.nextState_), sourceRevision_(other.sourceRevision_),
          source_(std::move(other.source_)), undo_(std::move(other.undo_)), redo_(std::move(other.redo_)), owner_(other.owner_),
          closed_(other.closed_) {
        if (lifetime_)
            lifetime_->owner = this;
        other.closed_ = true;
    }

    /** @copydoc NavigationDefinitionDocument::~NavigationDefinitionDocument */
    NavigationDefinitionDocument::~NavigationDefinitionDocument() {
        Close();
    }

    /** @copydoc NavigationDefinitionDocument::Open */
    Result<NavigationDefinitionDocument> NavigationDefinitionDocument::Open(const Assets::AssetId asset, const std::uint64_t session,
                                                                            const std::uint64_t sourceRevision,
                                                                            Navigation::NavigationDefinition definition) try {
        if (!asset.IsValid() || !session || !sourceRevision)
            return Result<NavigationDefinitionDocument>::Failure(Reject(Navigation::NavigationSettingsFailure::Invalid));
        NavigationDefinitionDocument result;
        result.lifetime_ = std::make_shared<Lifetime>(Lifetime{&result});
        result.asset_ = asset;
        result.session_ = session;
        result.sourceRevision_ = sourceRevision;
        result.source_ = std::make_shared<const Navigation::NavigationDefinition>(std::move(definition));
        return Result<NavigationDefinitionDocument>::Success(std::move(result));
    } catch (const std::bad_alloc &) {
        return Result<NavigationDefinitionDocument>::Failure(Reject(Navigation::NavigationSettingsFailure::Capacity));
    }

    /** @copydoc NavigationDefinitionDocument::Snapshot */
    Result<NavigationDefinitionSnapshot> NavigationDefinitionDocument::Snapshot() const {
        if (closed_ || owner_ != std::this_thread::get_id())
            return Result<NavigationDefinitionSnapshot>::Failure(Reject(Navigation::NavigationSettingsFailure::Closed));
        return Result<NavigationDefinitionSnapshot>::Success({asset_, session_, revision_, state_, source_});
    }

    /** @copydoc NavigationDefinitionDocument::Admit */
    Result<void> NavigationDefinitionDocument::Admit(const NavigationDefinitionSnapshot &expected) const {
        auto current = Snapshot();
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        if (expected.asset != asset_ || expected.session != session_ || expected.revision != revision_ || expected.state != state_ ||
            expected.definition != source_)
            return Result<void>::Failure(Reject(Navigation::NavigationSettingsFailure::Stale));
        return Result<void>::Success();
    }

    /** @copydoc NavigationDefinitionDocument::Prepare */
    Result<NavigationDefinitionDocument::Prepared> NavigationDefinitionDocument::Prepare(const NavigationDefinitionSnapshot &expected,
                                                                                         Navigation::NavigationDefinitionInput input,
                                                                                         const bool coordinated) const {
        if (auto admitted = Admit(expected); admitted.HasError())
            return Result<Prepared>::Failure(admitted.ErrorValue());
        if (revision_ == UINT64_MAX || nextState_ == UINT64_MAX || undo_.size() >= 64)
            return Result<Prepared>::Failure(Reject(Navigation::NavigationSettingsFailure::Capacity));
        auto validated = Navigation::NavigationDefinition::Create(std::move(input));
        if (validated.HasError())
            return Result<Prepared>::Failure(validated.ErrorValue());
        auto nextBytes = Bytes(validated.Value());
        auto priorBytes = Bytes(*source_);
        if (nextBytes.HasError())
            return Result<Prepared>::Failure(nextBytes.ErrorValue());
        if (priorBytes.HasError())
            return Result<Prepared>::Failure(priorBytes.ErrorValue());
        std::size_t charge = nextBytes.Value() + priorBytes.Value();
        for (const auto &entry : undo_) {
            if (entry.bytes > 4 * 1024 * 1024 - std::min(charge, std::size_t{4 * 1024 * 1024}))
                return Result<Prepared>::Failure(Reject(Navigation::NavigationSettingsFailure::Capacity));
            charge += entry.bytes;
        }
        if (charge > 4 * 1024 * 1024)
            return Result<Prepared>::Failure(Reject(Navigation::NavigationSettingsFailure::Capacity));
        Prepared result{{std::make_shared<const Navigation::NavigationDefinition>(std::move(validated).Value()), nextState_,
                         nextBytes.Value()},
                        undo_};
        result.undo.push_back({source_, state_, priorBytes.Value(), coordinated});
        return Result<Prepared>::Success(std::move(result));
    }

    /** @copydoc NavigationDefinitionDocument::Publish */
    void NavigationDefinitionDocument::Publish(Prepared &prepared) noexcept {
        source_.swap(prepared.next.source);
        undo_.swap(prepared.undo);
        redo_.swap(prepared.redo);
        state_ = prepared.next.state;
        if (!prepared.replay)
            ++nextState_;
        ++revision_;
    }

    /** @copydoc NavigationDefinitionDocument::CaptureSave */
    Result<NavigationDefinitionSave> NavigationDefinitionDocument::CaptureSave(
        const Navigation::NavigationAuthoredRecordId recordId) const {
        auto snapshot = Snapshot();
        if (snapshot.HasError())
            return Result<NavigationDefinitionSave>::Failure(snapshot.ErrorValue());
        if (sourceRevision_ == UINT64_MAX)
            return Result<NavigationDefinitionSave>::Failure(Reject(Navigation::NavigationSettingsFailure::Capacity));
        auto record = Navigation::EncodeNavigationDefinitionRecord(*source_, recordId);
        if (record.HasError())
            return Result<NavigationDefinitionSave>::Failure(record.ErrorValue());
        NavigationDefinitionSave save;
        save.snapshot_ = snapshot.Value();
        save.expectedSourceRevision_ = sourceRevision_;
        save.record_ = std::move(record).Value();
        save.owner_ = lifetime_;
        return Result<NavigationDefinitionSave>::Success(std::move(save));
    }

    /** @copydoc NavigationDefinitionDocument::AcknowledgeSave */
    Result<void> NavigationDefinitionDocument::AcknowledgeSave(const NavigationDefinitionSave &save,
                                                               const std::uint64_t publishedRevision) {
        if (auto current = Snapshot(); current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        if (save.owner_.lock().get() != lifetime_.get() || save.snapshot_.asset != asset_ || save.snapshot_.session != session_ ||
            save.expectedSourceRevision_ != sourceRevision_ ||
            (sourceRevision_ == UINT64_MAX || publishedRevision != sourceRevision_ + 1) || !save.snapshot_.definition ||
            save.snapshot_.revision > revision_)
            return Result<void>::Failure(Reject(Navigation::NavigationSettingsFailure::Stale));
        savedState_ = save.snapshot_.state;
        sourceRevision_ = publishedRevision;
        return Result<void>::Success();
    }

    /** @copydoc NavigationDefinitionDocument::PrepareReplay */
    Result<NavigationDefinitionDocument::Prepared> NavigationDefinitionDocument::PrepareReplay(const NavigationDefinitionSnapshot &expected,
                                                                                               const bool redo) const {
        if (auto admitted = Admit(expected); admitted.HasError())
            return Result<Prepared>::Failure(admitted.ErrorValue());
        const auto &from = redo ? redo_ : undo_;
        if (from.empty() || revision_ == UINT64_MAX)
            return Result<Prepared>::Failure(Reject(Navigation::NavigationSettingsFailure::Invalid));
        auto bytes = Bytes(*source_);
        if (bytes.HasError())
            return Result<Prepared>::Failure(bytes.ErrorValue());
        Prepared result{{}, undo_, redo_, true};
        auto &source = redo ? result.redo : result.undo;
        auto &target = redo ? result.undo : result.redo;
        result.next = source.back();
        target.push_back({source_, state_, bytes.Value(), result.next.coordinated});
        source.pop_back();
        return Result<Prepared>::Success(std::move(result));
    }

    /** @copydoc NavigationDefinitionDocument::Replay */
    Result<NavigationDefinitionSnapshot> NavigationDefinitionDocument::Replay(const NavigationDefinitionSnapshot &expected,
                                                                              const bool redo) {
        if (auto admitted = Admit(expected); admitted.HasError())
            return Result<NavigationDefinitionSnapshot>::Failure(admitted.ErrorValue());
        const auto &from = redo ? redo_ : undo_;
        if (!from.empty() && from.back().coordinated)
            return Result<NavigationDefinitionSnapshot>::Failure(Reject(Navigation::NavigationSettingsFailure::Referenced));
        auto result = PrepareReplay(expected, redo);
        if (result.HasError())
            return Result<NavigationDefinitionSnapshot>::Failure(result.ErrorValue());
        auto prepared = std::move(result).Value();
        Publish(prepared);
        return Snapshot();
    }
}  // namespace Horo::Editor
