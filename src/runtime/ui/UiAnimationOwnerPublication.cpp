#include "UiAnimationOwnerInternal.h"

namespace Horo::Runtime::Ui {
    /** @copydoc UiAnimationOwner::Commit */
    Result<void> UiAnimationOwner::Commit(Prepared &prepared) {
        if (!storage_ || storage_->ownerThread != std::this_thread::get_id() || !prepared.admitted_ ||
            prepared.owner_ != storage_.PublisherPin() || prepared.slot_ != storage_->candidate.frameSlot ||
            prepared.sourceRevision_ != storage_->commandRevision)
            return Result<void>::Failure(MakeError(UiErrors::ClockSourceStale));
        if (auto valid = CanPublish(*storage_); valid.HasError())
            return valid;
        auto &candidate = storage_->candidate;
        auto &frame = *storage_->frames[candidate.frameSlot];
        auto *canvas = storage_->publisher.Current()->Canvas(storage_->definition.canvas);
        // Every error-producing check completed above. These owners retain their storage pools; releasing an overwritten
        // inactive frame pin cannot destroy a generation, snapshot slot or external provider during this publication.
        frame.styles.emplace(storage_->resolver.PublishValidated(std::move(*candidate.styles)));
        frame.layout.emplace(canvas->layoutEngine->PublishValidated(std::move(*candidate.layout)));
        if (candidate.clipping) {
            frame.clipped.emplace(canvas->clipping->PublishValidated(std::move(*candidate.clipping)));
            canvas->clipped = *frame.clipped;
        } else {
            frame.clipped.reset();
        }
        frame.selection = candidate.selection;
        frame.attachments.clear();
        frame.focusGeometry.clear();
        for (auto &entry : storage_->attachments) {
            if (!entry || !entry->active)
                continue;
            auto &attachment = *entry;
            auto layout = attachment.layout.PublishValidated(std::move(*attachment.preparedLayout));
            std::optional<UiLayoutClipSnapshot> clipping;
            if (attachment.preparedClipping)
                clipping.emplace(attachment.clipping->PublishValidated(std::move(*attachment.preparedClipping)));
            const auto focusOffset = frame.focusGeometry.size();
            frame.focusGeometry.insert(frame.focusGeometry.end(), attachment.focusGeometry.begin(), attachment.focusGeometry.end());
            const auto focus =
                std::span<const UiAnimationFocusGeometry>{frame.focusGeometry}.subspan(focusOffset, attachment.focusGeometry.size());
            frame.attachments.push_back(
                {attachment.id, attachment.view, *attachment.selection, std::move(layout), std::move(clipping), focus});
            attachment.preparedLayout.reset();
            attachment.preparedClipping.reset();
            attachment.selection.reset();
        }
        storage_->presentation = candidate.presentation.IsValid() ? candidate.presentation : storage_->presentation;
        storage_->publishedViewport = candidate.viewport;
        storage_->attachmentDirty = false;
        if (canvas->focus)
            canvas->focus->PublishPreparedLayout(*frame.layout);
        for (auto *control : candidate.controls)
            control->PublishInteractionReplacement();
        candidate.controls.clear();
        if (candidate.actionSources)
            candidate.actionSources->PublishActionInteractionReplacements();
        candidate.actionSources = nullptr;
        if (candidate.canvasActions)
            candidate.canvasActions->PublishInteractionReplacement();
        candidate.canvasActions = nullptr;
        if (canvas->captures)
            canvas->captures->PublishInteractionValidated(canvas->tree.Canvas(), frame.layout->Descriptor().interaction);
        canvas->layout = *frame.layout;
        frame.generation = candidate.source;
        for (std::size_t index = 0; index < storage_->timelines.size(); ++index)
            storage_->timelines[index] = candidate.timelines[index];
        PublishRouteValidated(*storage_);
        storage_->binding.clocks = frame.clocks;
        storage_->lastSourceFrame = candidate.sourceFrame;
        storage_->pendingCommands = 0;
        storage_->currentFrame = candidate.frameSlot;
        candidate.admitted = false;
        prepared.admitted_ = false;
        candidate.styles.reset();
        candidate.layout.reset();
        candidate.clipping.reset();
        candidate.source = {};
        CaptureInspection(*storage_);
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime::Ui
