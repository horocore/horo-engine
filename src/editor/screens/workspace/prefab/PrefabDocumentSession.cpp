#include "editor/screens/workspace/prefab/PrefabDocumentSession.h"

#include "Horo/Editor/SourceDocumentService.h"
#include "Horo/Editor/WorkspacePanelHost.h"
#include "Horo/Prefab/PrefabErrors.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <new>

namespace Horo::Editor {
    /** @copydoc PrefabDocumentSession::UpdateLimitProfile */
    Result<void> PrefabDocumentSession::UpdateLimitProfile(const Prefab::PrefabLimitProfile &limits) {
        if (auto admitted = Admit({owner_, identity_.instance, revision_}); admitted.HasError())
            return admitted;
        if (limits_.Policy() != limits.Policy()) {
            externalComparison_.reset();
            keepExternalDraft_ = false;
        }
        limits_ = limits;
        if (const auto checked = source_->ValidateLimits(limits_); checked.HasError())
            return Result<void>::Failure(checked.ErrorValue());
        return Result<void>::Success();
    }

    namespace {
        /** @brief Hashes the sole canonical source encoding; no preview values enter dirty identity. */
        Result<Sha256Digest> Digest(const Prefab::PrefabDocument &source) {
            auto encoded = source.SerializeCanonical();
            if (encoded.HasError())
                return Result<Sha256Digest>::Failure(encoded.ErrorValue());
            return Result<Sha256Digest>::Success(ComputeSha256(std::as_bytes(std::span(encoded.Value()))));
        }
    }  // namespace

    /** @copydoc PrefabDocumentSession::ReadSourceText */
    Result<std::string> PrefabDocumentSession::ReadSourceText(const std::span<const std::byte> bytes) {
        // All session parse calls use the default byte ceiling and a validated source's project version.
        if (bytes.size() > Prefab::PrefabHardLimits::SourceDocumentBytes)
            return Result<std::string>::Failure(
                MakeError(Prefab::PrefabErrors::PayloadTooLarge, "Prefab source exceeds its parser byte bound."));
        try {
            std::string text(bytes.size(), '\0');
            std::ranges::transform(bytes, text.begin(), [](const std::byte byte) {
                return std::to_integer<char>(byte);
            });
            return Result<std::string>::Success(std::move(text));
        } catch (const std::bad_alloc &) {
            return Result<std::string>::Failure(MakeError(Prefab::PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }

    PrefabDocumentSession::PrefabDocumentSession(const ConstructionKey &, std::filesystem::path root, std::filesystem::path path,
                                                 DocumentIdentity identity, Assets::AssetId owner, const Prefab::PrefabLimitProfile &limits)
        : root_(std::move(root)), path_(std::move(path)), identity_(std::move(identity)), owner_(owner), limits_(limits) {}

    /** @copydoc PrefabDocumentSession::Open */
    Result<std::unique_ptr<PrefabDocumentSession>> PrefabDocumentSession::Open(const std::filesystem::path &root,
                                                                               const std::filesystem::path &path, Assets::AssetId asset,
                                                                               DocumentIdentity identity, Assets::AssetId owner,
                                                                               const Prefab::PrefabLimitProfile &limits) {
        if (!asset.IsValid() || !owner.IsValid() || !identity.IsValid() || identity.key.kind != DocumentKind::Prefab ||
            path != root / std::filesystem::path(std::u8string(identity.key.source.Value().begin(), identity.key.source.Value().end())))
            return Result<std::unique_ptr<PrefabDocumentSession>>::Failure(MakeError(Prefab::PrefabErrors::IdentityInvalid));
        auto capture = Assets::CaptureAssetSource(root, path, Prefab::PrefabHardLimits::SourceDocumentBytes);
        if (capture.HasError())
            return Result<std::unique_ptr<PrefabDocumentSession>>::Failure(capture.ErrorValue());
        auto session = std::make_unique<PrefabDocumentSession>(ConstructionKey{}, root, path, std::move(identity), owner, limits);
        auto text = ReadSourceText(capture.Value().bytes);
        if (text.HasError())
            return Result<std::unique_ptr<PrefabDocumentSession>>::Failure(text.ErrorValue());
        auto parsed = Prefab::PrefabDocument::Parse(text.Value(), limits);
        if (parsed.HasError())
            return Result<std::unique_ptr<PrefabDocumentSession>>::Failure(parsed.ErrorValue());
        if (parsed.Value().Data().assetId != asset)
            return Result<std::unique_ptr<PrefabDocumentSession>>::Failure(MakeError(Prefab::PrefabErrors::IdentityInvalid));
        auto digest = Digest(parsed.Value());
        if (digest.HasError())
            return Result<std::unique_ptr<PrefabDocumentSession>>::Failure(digest.ErrorValue());
        session->source_ = std::make_shared<const Prefab::PrefabDocument>(std::move(parsed).Value());
        session->state_ = digest.Value();
        session->saved_ = digest.Value();
        session->accepted_ = capture.Value().identity;
        return Result<std::unique_ptr<PrefabDocumentSession>>::Success(std::move(session));
    }

    /** @copydoc PrefabDocumentSession::Snapshot */
    PrefabDocumentSnapshot PrefabDocumentSession::Snapshot() const {
        return {identity_,
                {owner_, identity_.instance, revision_},
                source_,
                {source_->Data().projectVersion, state_},
                accepted_,
                saved_,
                recoveryRevision_,
                preview_.revision,
                state_ != saved_,
                conflict_,
                history_.UndoCount() != 0,
                history_.RedoCount() != 0,
                StageView().has_value(),
                externalComparison_,
                keepExternalDraft_};
    }

    /** @copydoc PrefabDocumentSession::Admit */
    Result<void> PrefabDocumentSession::Admit(const PrefabDocumentFence &fence) const {
        const ErrorCodeDescriptor *failure = nullptr;
        if (std::this_thread::get_id() != thread_)
            failure = &SourceDocumentErrors::WrongThread;
        else if (closed_)
            failure = &SourceDocumentErrors::Closed;
        else if (busy_)
            failure = &SourceDocumentErrors::Busy;
        else if (fence.owner != owner_ || fence.instance != identity_.instance || fence.revision != revision_ ||
                 revision_ == std::numeric_limits<std::uint64_t>::max())
            failure = &SourceDocumentErrors::Stale;
        return failure ? Result<void>::Failure(MakeError(*failure)) : Result<void>::Success();
    }

    /** @copydoc PrefabDocumentSession::Apply */
    Result<std::shared_ptr<const Prefab::PrefabDocument>> PrefabDocumentSession::Apply(const Edit &edit) const {
        if (const auto *replacement = std::get_if<SourceReplacement>(&edit))
            return Result<SourceReplacement>::Success(*replacement);
        auto data = source_->Data();
        auto applied = std::visit([&]<typename T>(const T &value) {
            if constexpr (std::is_same_v<T, LayerReplacement>) {
                if (!data.composition)
                    return Result<void>::Failure(MakeError(Prefab::PrefabErrors::CompositionInvalid));
                if (!value.placement) {
                    data.composition->variantAuthoredAgainst = value.revision;
                    data.composition->variantOverrides = value.overrides;
                } else {
                    auto found = std::ranges::find(data.composition->nestedPlacements, *value.placement,
                                                   &Prefab::NestedPrefabPlacement::placementLocalId);
                    if (found == data.composition->nestedPlacements.end())
                        return Result<void>::Failure(MakeError(Prefab::PrefabErrors::AddressInvalid));
                    found->authoredAgainst = value.revision;
                    found->valueOverrides = value.overrides;
                }
            } else if constexpr (!std::is_same_v<T, SourceReplacement>) {
                auto found = std::ranges::find(data.objects, value.object, &Prefab::PrefabObjectNode::localId);
                if (found == data.objects.end())
                    return Result<void>::Failure(MakeError(Prefab::PrefabErrors::AddressInvalid));
                if constexpr (std::is_same_v<T, RenamePrefabObject>)
                    found->name = value.name;
                else
                    found->localTransform = value.transform;
            }
            return Result<void>::Success();
        }, edit);
        if (applied.HasError())
            return Result<std::shared_ptr<const Prefab::PrefabDocument>>::Failure(applied.ErrorValue());
        auto validated = Prefab::PrefabDocument::Create(std::move(data), limits_);
        if (validated.HasError())
            return Result<std::shared_ptr<const Prefab::PrefabDocument>>::Failure(validated.ErrorValue());
        return Result<std::shared_ptr<const Prefab::PrefabDocument>>::Success(
            std::make_shared<const Prefab::PrefabDocument>(std::move(validated).Value()));
    }

    /** @copydoc PrefabDocumentSession::Execute */
    Result<void> PrefabDocumentSession::Execute(const PrefabDocumentFence &fence, const PrefabSourceEdit &edit) {
        if (auto admitted = Admit(fence); admitted.HasError())
            return admitted;
        return std::visit([&](const auto &value) {
            return CommitEdit(Edit{value});
        }, edit);
    }

    /** @copydoc PrefabDocumentSession::CommitEdit */
    Result<void> PrefabDocumentSession::CommitEdit(const Edit &edit) {
        if (conflict_)
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        auto candidate = Apply(edit);
        if (candidate.HasError())
            return Result<void>::Failure(candidate.ErrorValue());
        auto digest = Digest(*candidate.Value());
        if (digest.HasError())
            return Result<void>::Failure(digest.ErrorValue());
        if (digest.Value() == state_)
            return Result<void>::Success();
        auto before = Before(edit);
        if (before.HasError())
            return Result<void>::Failure(before.ErrorValue());
        auto bytes = EditBytes(before.Value(), edit);
        if (bytes.HasError())
            return Result<void>::Failure(bytes.ErrorValue());
        auto retained = history_.Prepare(Delta{std::move(before).Value(), edit, bytes.Value()});
        if (!retained)
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Limit));
        source_ = std::move(candidate).Value();
        history_.Commit(*retained);
        state_ = digest.Value();
        ++revision_;
        ReleaseStage();
        return Result<void>::Success();
    }

    /** @copydoc PrefabDocumentSession::Replay */
    Result<void> PrefabDocumentSession::Replay(const PrefabDocumentFence &fence, const bool redo) {
        if (auto admitted = Admit(fence); admitted.HasError())
            return admitted;
        if (conflict_)
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        const auto *delta = history_.Next(redo);
        if (!delta || delta->applyGroup)
            return Result<void>::Failure(MakeError(SourceDocumentErrors::Stale));
        auto candidate = Apply(redo ? delta->after : delta->before);
        if (candidate.HasError())
            return Result<void>::Failure(candidate.ErrorValue());
        auto digest = Digest(*candidate.Value());
        if (digest.HasError())
            return Result<void>::Failure(digest.ErrorValue());
        source_ = std::move(candidate).Value();
        state_ = digest.Value();
        history_.DidReplay(redo);
        ++revision_;
        ReleaseStage();
        return Result<void>::Success();
    }

    /** @copydoc PrefabDocumentSession::HasApplyHistory */
    bool PrefabDocumentSession::HasApplyHistory() const noexcept {
        const auto paired = [](const Delta &entry) {
            return entry.applyGroup.has_value();
        };
        return history_.Contains(false, paired) || history_.Contains(true, paired);
    }

    /** @copydoc PrefabDocumentSession::SuspendPresentation */
    void PrefabDocumentSession::SuspendPresentation() noexcept {
        ReleaseStage();
        presented_ = false;
    }

    /** @copydoc PrefabDocumentSession::ResumePresentation */
    void PrefabDocumentSession::ResumePresentation(DocumentIdentity identity) noexcept {
        identity_ = std::move(identity);
        presented_ = true;
    }

    /** @copydoc PrefabDocumentSession::NextApplyGroup */
    std::optional<PrefabApplyHistoryGroup> PrefabDocumentSession::NextApplyGroup(const bool redo) const noexcept {
        if (thread_ != std::this_thread::get_id() || !source_)
            return std::nullopt;
        const auto *delta = history_.Next(redo);
        return delta ? delta->applyGroup : std::nullopt;
    }

    /** @copydoc PrefabDocumentSession::PrepareApply */
    Result<PrefabDocumentSession::PreparedApply> PrefabDocumentSession::PrepareApply(const PrefabDocumentFence &fence,
                                                                                     Prefab::PrefabDocument source,
                                                                                     const PrefabApplyHistoryGroup group) const {
        using Return = Result<PreparedApply>;
        if (auto admitted = Admit(fence); admitted.HasError())
            return Return::Failure(admitted.ErrorValue());
        if (conflict_ || saved_ != state_ || source.Data().assetId != source_->Data().assetId)
            return Return::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        auto replacement = std::make_shared<const Prefab::PrefabDocument>(std::move(source));
        auto digest = Digest(*replacement);
        if (digest.HasError())
            return Return::Failure(std::move(digest).ErrorValue());
        if (digest.Value() == state_)
            return Return::Failure(MakeError(Prefab::PrefabErrors::OverrideInvalid, "Selected intent already matches the Prefab source."));
        const Edit before{source_};
        const Edit after{replacement};
        const auto bytes = EditBytes(before, after);
        if (bytes.HasError())
            return Return::Failure(bytes.ErrorValue());
        auto history = history_.Prepare(Delta{before, after, bytes.Value(), group});
        if (!history)
            return Return::Failure(MakeError(Assets::AssetSourceErrors::Limit));
        return Return::Success({std::move(replacement), digest.Value(), std::move(history)});
    }

    /** @copydoc PrefabDocumentSession::PrepareApplyReplay */
    Result<PrefabDocumentSession::PreparedApply> PrefabDocumentSession::PrepareApplyReplay(const PrefabDocumentFence &fence,
                                                                                           const PrefabApplyHistoryGroup group,
                                                                                           const bool redo) const {
        using Return = Result<PreparedApply>;
        if (auto admitted = Admit(fence); admitted.HasError())
            return Return::Failure(admitted.ErrorValue());
        if (conflict_ || saved_ != state_)
            return Return::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        const auto *delta = history_.Next(redo);
        if (!delta || delta->applyGroup != group)
            return Return::Failure(MakeError(SourceDocumentErrors::Stale));
        const auto &replacement = std::get<SourceReplacement>(redo ? delta->after : delta->before);
        auto digest = Digest(*replacement);
        if (digest.HasError())
            return Return::Failure(std::move(digest).ErrorValue());
        auto history = std::make_unique<SemanticDocumentHistory<Delta>>(history_);
        history->DidReplay(redo);
        return Return::Success({replacement, digest.Value(), std::move(history)});
    }

    /** @copydoc PrefabDocumentSession::PublishApply */
    void PrefabDocumentSession::PublishApply(PreparedApply &prepared, const Assets::AssetSourceIdentity durable) noexcept {
        source_.swap(prepared.source);
        history_.Commit(*prepared.history);
        state_ = prepared.state;
        saved_ = state_;
        accepted_ = durable;
        ++revision_;
    }

    /** @copydoc PrefabDocumentSession::CaptureExternalInspection */
    PrefabExternalInspection PrefabDocumentSession::CaptureExternalInspection() const {
        return {root_, path_, {owner_, identity_.instance, revision_}, accepted_, source_, limits_, externalComparison_};
    }

    /** @copydoc PrefabDocumentSession::ObserveExternal */
    bool PrefabDocumentSession::ObserveExternal(const PrefabExternalInspection &request,
                                                std::shared_ptr<const PrefabExternalComparison> comparison) {
        if (Admit(request.fence).HasError() || request.accepted != accepted_ || request.draft != source_ ||
            request.limits.Policy() != limits_.Policy())
            return false;
        if (externalComparison_ && externalComparison_ == comparison)
            return false;
        if (!conflict_ && !comparison)
            return false;
        conflict_ = comparison != nullptr;
        externalComparison_ = std::move(comparison);
        keepExternalDraft_ = false;
        if (conflict_)
            ReleaseStage();
        return true;
    }

    /** @copydoc PrefabDocumentSession::KeepExternalDraft */
    Result<void> PrefabDocumentSession::KeepExternalDraft(const PrefabDocumentFence &fence, const PrefabExternalComparison &expected) {
        if (auto admitted = Admit(fence); admitted.HasError())
            return admitted;
        if (!conflict_ || externalComparison_.get() != std::addressof(expected))
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        keepExternalDraft_ = true;
        return Result<void>::Success();
    }

    /** @copydoc PrefabDocumentSession::InspectExternal */
    Result<void> PrefabDocumentSession::InspectExternal() {
        if (auto admitted = Admit({owner_, identity_.instance, revision_}); admitted.HasError())
            return admitted;
        auto capture = Assets::CaptureAssetSource(root_, path_, Prefab::PrefabHardLimits::SourceDocumentBytes);
        if (capture.HasError()) {
            conflict_ = true;
            externalComparison_.reset();
            keepExternalDraft_ = false;
            return Result<void>::Failure(capture.ErrorValue());
        }
        conflict_ = capture.Value().identity != accepted_;
        if (externalComparison_ && externalComparison_->observed != capture.Value().identity) {
            externalComparison_.reset();
            keepExternalDraft_ = false;
        }
        return Result<void>::Success();
    }

    /** @copydoc PrefabDocumentSession::Replace */
    Result<void> PrefabDocumentSession::Replace(const Assets::AssetSourceCapture &capture) {
        auto text = ReadSourceText(capture.bytes);
        if (text.HasError())
            return Result<void>::Failure(text.ErrorValue());
        auto parsed = Prefab::PrefabDocument::Parse(text.Value(), limits_, {.expectedProjectVersion = source_->Data().projectVersion});
        if (parsed.HasError())
            return Result<void>::Failure(parsed.ErrorValue());
        if (parsed.Value().Data().assetId != source_->Data().assetId)
            return Result<void>::Failure(MakeError(Prefab::PrefabErrors::IdentityInvalid));
        auto digest = Digest(parsed.Value());
        if (digest.HasError())
            return Result<void>::Failure(digest.ErrorValue());
        auto replacement = std::make_shared<const Prefab::PrefabDocument>(std::move(parsed).Value());
        source_.swap(replacement);
        state_ = digest.Value();
        saved_ = digest.Value();
        accepted_ = capture.identity;
        conflict_ = false;
        externalComparison_.reset();
        keepExternalDraft_ = false;
        history_.Clear();
        ++revision_;
        recoveryRevision_ = 0;
        ReleaseStage();
        return Result<void>::Success();
    }

    /** @copydoc PrefabDocumentSession::Reload */
    Result<void> PrefabDocumentSession::Reload(const PrefabDocumentFence &fence, const bool discardDirty,
                                               const std::optional<Assets::AssetSourceIdentity> expectedDisk) {
        if (auto admitted = Admit(fence); admitted.HasError())
            return admitted;
        if (state_ != saved_ && !discardDirty)
            return Result<void>::Failure(MakeError(WorkspaceDocumentErrors::DirtyDocument));
        auto capture = Assets::CaptureAssetSource(root_, path_, Prefab::PrefabHardLimits::SourceDocumentBytes);
        if (capture.HasError())
            return Result<void>::Failure(capture.ErrorValue());
        if (expectedDisk && capture.Value().identity != *expectedDisk)
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        return Replace(capture.Value());
    }

    /** @copydoc PrefabDocumentSession::ReleaseStage */
    void PrefabDocumentSession::ReleaseStage() noexcept {
        preview_.cancellation.RequestCancellation();
        if (preview_.host)
            preview_.host->Shutdown();
        preview_.host.reset();
        preview_.scene.reset();
        preview_.published = false;
    }

    /** @copydoc PrefabDocumentSession::~PrefabDocumentSession */
    PrefabDocumentSession::~PrefabDocumentSession() {
        ReleaseStage();
    }

    /** @copydoc PrefabDocumentSession::Close */
    Result<void> PrefabDocumentSession::Close(const PrefabDocumentFence &fence, const bool discardDirty) {
        if (auto admitted = Admit(fence); admitted.HasError())
            return admitted;
        if (state_ != saved_ && !discardDirty)
            return Result<void>::Failure(MakeError(WorkspaceDocumentErrors::DirtyDocument));
        ReleaseStage();
        history_.Clear();
        closed_ = true;
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
