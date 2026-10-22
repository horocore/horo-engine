#include "UiAnimationOwnerInternal.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::Runtime::Ui {
    namespace {
        /** @brief Builds canonical copied evidence from actual owner identities and resolved local logical extent. */
        UiPresentationProfileEvidence Evidence(const UiElementTree &tree, const UiCanvasId canvas,
                                               const UiPresentationAttachmentId attachment, const UiAnimationViewport &viewport,
                                               const UiLayoutPresentationRevision revision) {
            const auto host = viewport.presentation.value_or(
                UiAnimationPresentationEvidence{UiPresentationEvidenceRevision::Create(revision.Value()).Value()});
            return {canvas,
                    tree.Instance(),
                    tree.Canvas(),
                    tree.Revision(),
                    attachment,
                    host.revision,
                    {viewport.content.extent.width, viewport.content.extent.height},
                    host.formFactor,
                    host.viewRole,
                    host.available};
        }

        /** @brief Allocates an optional clip cache with the same finite geometry retention as its layout owner. */
        Result<std::optional<UiLayoutClipEngine>> CreateAttachmentClipping(const UiReloadCanvas &canvas, const std::uint32_t elements,
                                                                           const std::uint32_t slots) {
            if (!canvas.clipping)
                return Result<std::optional<UiLayoutClipEngine>>::Success(std::nullopt);
            auto created = UiLayoutClipEngine::Create({canvas.tree.Instance(), canvas.tree.Canvas(), canvas.tree.SourceDocument(), elements,
                                                       MaximumUiLayoutClipNodes, MaximumUiLayoutScrollContainers, slots});
            if (created.HasError())
                return Result<std::optional<UiLayoutClipEngine>>::Failure(created.ErrorValue());
            return Result<std::optional<UiLayoutClipEngine>>::Success(std::move(created).Value());
        }

        /** @brief Requires strictly newer explicit evidence when any admitted viewport facts change. */
        bool NewEvidence(const UiAnimationViewport &old, const UiAnimationViewport &next) noexcept {
            return old == next || !old.presentation || (next.presentation && next.presentation->revision > old.presentation->revision);
        }
    }  // namespace

    /** @copydoc UiAnimationOwner::AttachmentInputEligible */
    bool UiAnimationOwner::AttachmentInputEligible(const Storage &storage, const UiRenderViewId view) noexcept {
        for (const auto &entry : storage.attachments) {
            if (!entry || !entry->active || entry->view != view)
                continue;
            if (!storage.currentFrame)
                return false;
            return std::ranges::any_of(storage.frames[*storage.currentFrame]->attachments, [&](const auto &frame) {
                return frame.attachment == entry->id;
            });
        }
        return true;
    }

    /** @copydoc UiAnimationOwner::PrimaryAttachment */
    UiPresentationAttachmentId UiAnimationOwner::PrimaryAttachment() const noexcept {
        return storage_ && !storage_->lifecycle.stopped ? storage_->primaryAttachment : UiPresentationAttachmentId{};
    }

    /** @copydoc UiAnimationOwner::Attach */
    Result<UiPresentationAttachmentId> UiAnimationOwner::Attach(const UiRenderViewId view, UiAnimationViewport viewport) {
        if (!storage_)
            return Result<UiPresentationAttachmentId>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        if (auto admitted = AdmitCommand(*storage_); admitted.HasError())
            return Result<UiPresentationAttachmentId>::Failure(admitted.ErrorValue());
        auto &owner = *storage_;
        auto *canvas = owner.publisher.Current()->Canvas(owner.definition.canvas);
        if (!view.IsValid() || (!canvas->presentations.empty() && canvas->presentations.front().View() == view))
            return Result<UiPresentationAttachmentId>::Failure(MakeError(UiErrors::PresentationProfileForeignIdentity));
        for (const auto &item : owner.attachments)
            if (item && item->active && item->view == view)
                return Result<UiPresentationAttachmentId>::Failure(MakeError(UiErrors::DocumentDuplicateIdentity));
        for (auto &frame : owner.frames)
            if (frame.use_count() == 1 && (!owner.currentFrame || frame != owner.frames[*owner.currentFrame]))
                frame->attachments.clear();
        const auto free = std::ranges::find_if(owner.attachments, [](const auto &item) {
            return !item || (!item->active && item->layout.IsDrained() && (!item->clipping || item->clipping->IsDrained()));
        });
        if (free == owner.attachments.end())
            return Result<UiPresentationAttachmentId>::Failure(MakeError(UiErrors::CapacityExceeded));
        const auto index = static_cast<std::size_t>(free - owner.attachments.begin());
        if (owner.attachmentGenerations[index] == std::numeric_limits<std::uint32_t>::max())
            return Result<UiPresentationAttachmentId>::Failure(MakeError(UiErrors::GenerationExhausted));
        const UiPresentationAttachmentId id{owner.primaryAttachment.ownership,
                                            owner.primaryAttachment.slot + 1 + static_cast<std::uint32_t>(index),
                                            owner.attachmentGenerations[index] + 1};
        const auto evidence = Evidence(canvas->tree, owner.definition.canvas, id, viewport, owner.presentation);
        if (!viewport.constraints.IsValid() || !viewport.content.IsValid() || !viewport.fontScale.IsValid() ||
            !viewport.intrinsic.IsValid() || !viewport.canvas.IsValid() || !viewport.policy.IsValid() || !evidence.IsValid())
            return Result<UiPresentationAttachmentId>::Failure(MakeError(UiErrors::PresentationProfileMalformed));
        return AdoptAttachment(owner, index, id, view, std::move(viewport));
    }

    /** @copydoc UiAnimationOwner::AdoptAttachment */
    Result<UiPresentationAttachmentId> UiAnimationOwner::AdoptAttachment(Storage &owner, const std::size_t index,
                                                                         const UiPresentationAttachmentId id, const UiRenderViewId view,
                                                                         UiAnimationViewport viewport) {
        auto *canvas = owner.publisher.Current()->Canvas(owner.definition.canvas);
        try {
            const auto slots = std::min(MaximumUiLayoutSnapshotsInFlight, std::max(2U, owner.limits.retainedSnapshots + 1));
            auto layout = UiLayoutEngine::Create({canvas->tree.Instance(), canvas->tree.Canvas(), canvas->tree.SourceDocument(),
                                                  static_cast<std::uint32_t>(owner.definition.elements.size()), MaximumUiStructuralCommands,
                                                  slots, UiInteractionRevision::Create(1).Value()});
            if (layout.HasError())
                return Result<UiPresentationAttachmentId>::Failure(layout.ErrorValue());
            auto clipping = CreateAttachmentClipping(*canvas, static_cast<std::uint32_t>(owner.definition.elements.size()), slots);
            if (clipping.HasError())
                return Result<UiPresentationAttachmentId>::Failure(clipping.ErrorValue());
            // Construct in final storage: MSVC Debug vector moves can allocate inside noexcept.
            // Keep the candidate private until tracker admission succeeds; failure preserves the old drained slot.
            auto attachment = std::make_unique<Storage::Attachment>(id, view, std::move(viewport), std::move(layout).Value(),
                                                                    std::move(clipping).Value(), owner.definition.elements.size());
            if (std::ranges::find(canvas->presentations, view, &UiPresentedInteractionState::View) == canvas->presentations.end()) {
                auto tracker = UiPresentedInteractionState::Create(view, canvas->tree.Canvas());
                if (tracker.HasError())
                    return Result<UiPresentationAttachmentId>::Failure(tracker.ErrorValue());
                canvas->presentations.push_back(std::move(tracker).Value());
            }
            owner.attachments[index] = std::move(attachment);
            owner.attachmentGenerations[index] = id.generation;
            owner.attachmentDirty = true;
            ++owner.commandRevision;
            ++owner.pendingCommands;
            return Result<UiPresentationAttachmentId>::Success(id);
        } catch (const std::bad_alloc &) {
            return Result<UiPresentationAttachmentId>::Failure(MakeError(UiErrors::CapacityExceeded));
        }
    }

    /** @copydoc UiAnimationOwner::UpdateAttachment */
    Result<void> UiAnimationOwner::UpdateAttachment(const UiPresentationAttachmentId attachment, UiAnimationViewport viewport) {
        if (!storage_)
            return Result<void>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        if (auto admitted = AdmitCommand(*storage_); admitted.HasError())
            return admitted;
        auto &owner = *storage_;
        const auto found = std::ranges::find_if(owner.attachments, [&](const auto &item) {
            return item && item->active && item->id == attachment;
        });
        if (found == owner.attachments.end())
            return Result<void>::Failure(MakeError(UiErrors::PresentationProfileForeignIdentity));
        if (!NewEvidence((*found)->viewport, viewport))
            return Result<void>::Failure(MakeError(UiErrors::PresentationProfileSourceStale));
        const auto *canvas = owner.publisher.Current()->Canvas(owner.definition.canvas);
        if (!viewport.constraints.IsValid() || !viewport.content.IsValid() || !viewport.fontScale.IsValid() ||
            !viewport.intrinsic.IsValid() || !viewport.canvas.IsValid() || !viewport.policy.IsValid() ||
            !Evidence(canvas->tree, owner.definition.canvas, attachment, viewport, owner.presentation).IsValid())
            return Result<void>::Failure(MakeError(UiErrors::PresentationProfileMalformed));
        if ((*found)->viewport == viewport)
            return Result<void>::Success();
        (*found)->viewport = std::move(viewport);
        owner.attachmentDirty = true;
        ++owner.commandRevision;
        ++owner.pendingCommands;
        return Result<void>::Success();
    }

    /** @copydoc UiAnimationOwner::Detach */
    Result<void> UiAnimationOwner::Detach(const UiPresentationAttachmentId attachment) {
        if (!storage_)
            return Result<void>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        if (auto admitted = AdmitCommand(*storage_); admitted.HasError())
            return admitted;
        auto &owner = *storage_;
        const auto found = std::ranges::find_if(owner.attachments, [&](const auto &item) {
            return item && item->active && item->id == attachment;
        });
        if (found == owner.attachments.end())
            return Result<void>::Failure(MakeError(UiErrors::PresentationProfileForeignIdentity));
        const auto view = (*found)->view;
        auto *canvas = owner.publisher.Current()->Canvas(owner.definition.canvas);
        std::erase_if(canvas->presentations, [&](const auto &tracker) {
            return tracker.View() == view;
        });
        (*found)->active = false;
        owner.attachmentDirty = true;
        ++owner.commandRevision;
        ++owner.pendingCommands;
        return Result<void>::Success();
    }

    /** @copydoc UiAnimationOwner::PrepareProfiles */
    Result<void> UiAnimationOwner::PrepareProfiles(Storage &storage, const UiAnimationViewport &viewport) {
        const bool responsive = !storage.profiles->Rows().empty() || viewport.presentation || storage.attachmentDirty ||
                                std::ranges::any_of(storage.attachments, [](const auto &item) {
            return item && item->active;
        });
        storage.candidate.viewport = viewport;
        storage.candidate.selection.reset();
        storage.candidate.presentation = {};
        if (!responsive)
            return Result<void>::Success();
        if (storage.publishedViewport && !NewEvidence(*storage.publishedViewport, viewport))
            return Result<void>::Failure(MakeError(UiErrors::PresentationProfileSourceStale));
        auto revision = storage.presentation;
        if (storage.attachmentDirty || (storage.publishedViewport && *storage.publishedViewport != viewport)) {
            auto next = revision.Next();
            if (next.HasError())
                return Result<void>::Failure(next.ErrorValue());
            revision = next.Value();
        }
        auto *canvas = storage.publisher.Current()->Canvas(storage.definition.canvas);
        auto selected = storage.profiles->Select(canvas->tree, Evidence(canvas->tree, storage.definition.canvas, storage.primaryAttachment,
                                                                        viewport, revision));
        if (selected.HasError())
            return Result<void>::Failure(selected.ErrorValue());
        storage.candidate.presentation = revision;
        storage.candidate.selection = selected.Value();
        std::ranges::sort(storage.work.layoutDescriptors, {}, &UiLayoutElementDescriptor::element);
        return storage.profiles->ApplyLayouts(canvas->tree, selected.Value(), storage.work.layoutDescriptors);
    }

    /** @copydoc UiAnimationOwner::PrepareAttachmentStyles */
    Result<UiPresentationProfileSelection> UiAnimationOwner::PrepareAttachmentStyles(Storage &storage, const std::size_t index) {
        const auto &tree = storage.publisher.Current()->Canvas(storage.definition.canvas)->tree;
        const auto &attachment = *storage.attachments[index];
        for (std::size_t index = 0; index < storage.work.layoutDescriptors.size(); ++index)
            storage.work.layoutDescriptors[index] = {storage.work.elementInputs[index].element, storage.definition.elements[index].layout,
                                                     storage.definition.elements[index].intrinsic};
        std::ranges::sort(storage.work.layoutDescriptors, {}, &UiLayoutElementDescriptor::element);
        auto selection = storage.profiles->Select(tree, Evidence(tree, storage.definition.canvas, attachment.id, attachment.viewport,
                                                                 storage.candidate.presentation));
        if (selection.HasError())
            return Result<UiPresentationProfileSelection>::Failure(selection.ErrorValue());
        if (auto applied = storage.profiles->ApplyLayouts(tree, selection.Value(), storage.work.layoutDescriptors); applied.HasError())
            return Result<UiPresentationProfileSelection>::Failure(applied.ErrorValue());
        if (auto projected = AnimationInternal::ProjectLayout(storage.candidate.styles->Candidate(), storage.work.layoutBindings,
                                                              storage.work.layoutDescriptors);
            projected.HasError())
            return Result<UiPresentationProfileSelection>::Failure(projected.ErrorValue());
        return selection;
    }

    /** @copydoc UiAnimationOwner::PrepareAttachments */
    Result<void> UiAnimationOwner::PrepareAttachments(Storage &storage) {
        auto *canvas = storage.publisher.Current()->Canvas(storage.definition.canvas);
        const auto &tree = canvas->tree;
        for (std::size_t index = 0; index < storage.attachments.size(); ++index) {
            auto &entry = storage.attachments[index];
            if (!entry || !entry->active)
                continue;
            auto &attachment = *entry;
            auto selection = PrepareAttachmentStyles(storage, index);
            if (selection.HasError())
                return Result<void>::Failure(selection.ErrorValue());
            auto evaluator =
                UiDeclarativeLayoutEvaluator::Create(storage.work.layoutDescriptors, storage.definition.intrinsicProvider.get());
            if (evaluator.HasError())
                return Result<void>::Failure(evaluator.ErrorValue());
            auto sources = storage.candidate.layout->Candidate().Descriptor().sources;
            sources.intrinsic = attachment.viewport.intrinsic;
            sources.canvas = attachment.viewport.canvas;
            sources.policy = attachment.viewport.policy;
            auto layout = attachment.layout.PrepareDerived(tree,
                                                           {sources, attachment.viewport.constraints, attachment.viewport.content,
                                                            &evaluator.Value(), attachment.viewport.fontScale},
                                                           *storage.candidate.layout);
            if (layout.HasError())
                return Result<void>::Failure(layout.ErrorValue());
            attachment.preparedLayout.emplace(std::move(layout).Value());
            attachment.selection = selection.Value();
            if (attachment.clipping) {
                auto clipped = attachment.clipping->Prepare(tree, attachment.preparedLayout->Candidate(), {canvas->clipPolicies, {}});
                if (clipped.HasError())
                    return Result<void>::Failure(clipped.ErrorValue());
                attachment.preparedClipping.emplace(std::move(clipped).Value());
            }
            if (auto projected = ProjectAttachmentFocus(storage, attachment.preparedLayout->Candidate(),
                                                        attachment.preparedClipping ? &attachment.preparedClipping->Candidate() : nullptr,
                                                        attachment.focusGeometry);
                projected.HasError())
                return projected;
        }
        return Result<void>::Success();
    }

    /** @copydoc UiAnimationOwner::ConfigureInspection */
    Result<void> UiAnimationOwner::ConfigureInspection(const UiPresentationAttachmentId attachment, const UiLayoutInspectorLimits limits,
                                                       const bool enabled) {
        if (!storage_)
            return Result<void>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        if (auto admitted = AdmitCommand(*storage_); admitted.HasError())
            return admitted;
        if (attachment != storage_->primaryAttachment && !std::ranges::any_of(storage_->attachments, [&](const auto &item) {
            return item && item->active && item->id == attachment;
        }))
            return Result<void>::Failure(MakeError(UiErrors::PresentationProfileForeignIdentity));
        auto inspector = UiLayoutInspector::Create(*storage_->publisher.Current(), storage_->definition.canvas, limits);
        if (inspector.HasError())
            return Result<void>::Failure(inspector.ErrorValue());
        auto ownedInspector = std::move(inspector).Value();
        ownedInspector.Enable(enabled);
        storage_->inspector.emplace(std::move(ownedInspector));
        storage_->inspectedAttachment = attachment;
        return Result<void>::Success();
    }

    /** @copydoc UiAnimationOwner::Inspect */
    std::optional<UiLayoutInspectionSnapshot> UiAnimationOwner::Inspect() const noexcept {
        return storage_ && storage_->inspector ? storage_->inspector->Latest() : std::nullopt;
    }

    /** @copydoc UiAnimationOwner::CaptureInspection */
    void UiAnimationOwner::CaptureInspection(Storage &storage) noexcept {
        if (!storage.inspector || !storage.currentFrame)
            return;
        const auto &frame = *storage.frames[*storage.currentFrame];
        const auto &tree = storage.publisher.Current()->Canvas(storage.definition.canvas)->tree;
        if (storage.inspectedAttachment == storage.primaryAttachment) {
            (void)storage.inspector->Capture(tree, *frame.layout, frame.clipped ? &*frame.clipped : nullptr,
                                             frame.selection ? &*frame.selection : nullptr);
            return;
        }
        const auto found = std::ranges::find(frame.attachments, storage.inspectedAttachment, &UiAnimationAttachmentFrame::attachment);
        if (found != frame.attachments.end())
            (void)storage.inspector->Capture(tree, *found->layout, found->clipping ? &*found->clipping : nullptr, &found->selection);
    }
}  // namespace Horo::Runtime::Ui
