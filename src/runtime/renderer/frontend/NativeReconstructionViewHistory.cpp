#include "Horo/Runtime/Render/NativeReconstructionViewHistory.h"

#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/TemporalHistoryErrors.h"
#include "RenderResourceRegistry.h"

#include <algorithm>
#include <new>
#include <utility>

namespace Horo::Render {
    namespace {
        using Image = NativeReconstructionImage;

        constexpr std::size_t Slot(Image role) noexcept {
            return static_cast<std::size_t>(role);
        }

        /** @brief Distinguishes a required current producer from a history image this pass must import. */
        std::optional<RenderGraphResourceId> ReconstructionSource(const NativeReconstructionGraphSources &sources,
                                                                  const Image role) noexcept {
            using enum NativeReconstructionImage;
            switch (role) {
                case CurrentColor:
                    return sources.color;
                case CurrentDepth:
                    return sources.positiveLinearDepth;
                case Motion:
                    return sources.motion;
                default:
                    return std::nullopt;
            }
        }

        /** @brief Adds a sampled current image or imports one exact generation of a history image. */
        Result<void> BindReconstructionImage(RenderGraphBuilder &builder, const RenderGraphPassRef pass,
                                             const NativeReconstructionScenePlan &plan, const Image role, RenderGraphResourceId resource,
                                             NativeReconstructionGraphDispatch &dispatch) {
            const auto &image = plan.images[Slot(role)];
            if (!image.texture.IsValid() || !image.view.IsValid() || image.texture.owner != image.view.owner)
                return Result<void>::Failure(MakeError(TemporalHistoryErrors::InvalidFrame));
            const bool written = NativeReconstructionWritesImage(role);
            if (!resource.IsValid()) {
                const auto imported = builder.ImportTexture(image.texture, RenderGraphResourceClass::History);
                if (imported.HasError())
                    return Result<void>::Failure(imported.ErrorValue());
                resource = imported.Value();
                auto &initial = dispatch.historyImports[dispatch.historyImportCount++];
                initial.resource = resource;
                initial.contents = RenderGraphInitialContents::Discarded;
                if (!written) {
                    initial.contents = RenderGraphInitialContents::Preserved;
                    initial.state = role == Image::PreviousColor ? plan.previousStates->color : plan.previousStates->depth;
                }
            }
            dispatch.workload.images[Slot(role)] = {resource, image.view};
            return builder.AddUsage({pass, resource, written ? RenderGraphAccess::Write : RenderGraphAccess::Read,
                                     written ? RenderGraphUsageKind::Storage : RenderGraphUsageKind::Sampled});
        }

        /** @brief Gets the exact two color/depth texture pairs in the history store's fixed order. */
        auto HistoryTextures(const NativeReconstructionViewHistory::ImagePairs &images) noexcept {
            return std::array{images[0].color.texture, images[0].depth.texture, images[1].color.texture, images[1].depth.texture};
        }

        /** @brief Validates exact live generational resource identities on the selected frontend. */
        Result<void> ValidateImagePairs(RenderFrontend &frontend, const TemporalHistoryCompatibility &compatibility,
                                        const NativeReconstructionViewHistory::ImagePairs &images) {
            const auto &support = frontend.Capabilities().support;
            if (!support.features.Supports(RenderCapability::NativeReconstruction))
                return Result<void>::Failure(MakeError(NativeReconstructionErrors::Unsupported));
            if (!compatibility.IsValid() || compatibility.deviceGeneration != support.deviceIncarnation)
                return Result<void>::Failure(MakeError(TemporalHistoryErrors::InvalidDescriptor));
            auto textures = HistoryTextures(images);
            std::ranges::sort(textures);
            if (std::ranges::adjacent_find(textures) != textures.end())
                return Result<void>::Failure(MakeError(TemporalHistoryErrors::InvalidDescriptor));
            for (const auto &pair : images)
                for (const auto &image : {pair.color, pair.depth}) {
                    const auto texture = frontend.ResourceState(image.texture);
                    if (texture.HasError())
                        return Result<void>::Failure(texture.ErrorValue());
                    const auto view = frontend.ResourceState(image.view);
                    if (view.HasError())
                        return Result<void>::Failure(view.ErrorValue());
                    if (texture.Value() != RenderResourceState::Ready || view.Value() != RenderResourceState::Ready ||
                        image.texture.owner != image.view.owner)
                        return Result<void>::Failure(MakeError(TemporalHistoryErrors::InvalidDescriptor));
                }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc BindNativeReconstructionGraphDispatch */
    Result<NativeReconstructionGraphDispatch> BindNativeReconstructionGraphDispatch(
        RenderGraphBuilder &builder, const RenderGraphPassRef pass, const NativeReconstructionGraphSources &sources,
        const NativeReconstructionScenePlan &plan, std::shared_ptr<IResidentNativeReconstructionKernel> kernel) {
        using Outcome = Result<NativeReconstructionGraphDispatch>;
        const auto entry = plan.dispatch.entry;
        if (const bool usesHistory = entry == NativeReconstructionEntry::TemporalHistory;
            (!usesHistory && entry != NativeReconstructionEntry::TemporalSeed) || usesHistory != plan.previousStates.has_value() || !kernel)
            return Outcome::Failure(MakeError(TemporalHistoryErrors::InvalidFrame));

        NativeReconstructionGraphDispatch dispatch{.workload = {.dispatch = plan.dispatch, .kernel = std::move(kernel)}};
        for (std::size_t slot = 0; slot < plan.images.size(); ++slot) {
            const auto role = static_cast<Image>(slot);
            if (!NativeReconstructionUsesImage(entry, role))
                continue;
            const auto source = ReconstructionSource(sources, role);
            if (source && (!source->IsValid() || source->owner != builder.Owner()))
                return Outcome::Failure(MakeError(TemporalHistoryErrors::InvalidFrame));
            const auto bound = BindReconstructionImage(builder, pass, plan, role, source.value_or(RenderGraphResourceId{}), dispatch);
            if (bound.HasError())
                return Outcome::Failure(bound.ErrorValue());
        }
        return Outcome::Success(std::move(dispatch));
    }

    NativeReconstructionViewHistory::NativeReconstructionViewHistory(TemporalHistoryStore store, TemporalHistoryHandle handle,
                                                                     const ImagePairs &images,
                                                                     TemporalHistoryCompatibility compatibility) noexcept
        : store_(std::move(store)), handle_(handle), images_(images), compatibility_(compatibility), owner_(std::this_thread::get_id()) {}

    /** @copydoc NativeReconstructionViewHistory::Create */
    Result<std::shared_ptr<NativeReconstructionViewHistory>> NativeReconstructionViewHistory::Create(
        RenderFrontend &frontend, const TemporalHistoryCompatibility &compatibility, const ImagePairs &images) {
        using Outcome = Result<std::shared_ptr<NativeReconstructionViewHistory>>;
        if (const auto open = frontend.RequireOpenOwner(); open.HasError())
            return Outcome::Failure(open.ErrorValue());
        if (frontend.activeFrameScope_ != nullptr)
            return Outcome::Failure(MakeError(TemporalHistoryErrors::InvalidDescriptor));
        if (const auto valid = ValidateImagePairs(frontend, compatibility, images); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        const auto textures = HistoryTextures(images);
        auto created = TemporalHistoryStore::Create({.maxHistories = 1, .maxResourcesPerHistory = textures.size()});
        if (created.HasError())
            return Outcome::Failure(created.ErrorValue());
        auto store = std::move(created).Value();
        const auto history = store.Add(compatibility, textures);
        if (history.HasError())
            return Outcome::Failure(history.ErrorValue());
        try {
            return Outcome::Success(std::shared_ptr<NativeReconstructionViewHistory>{
                new NativeReconstructionViewHistory(std::move(store), history.Value(), images, compatibility)});
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(TemporalHistoryErrors::AllocationFailed));
        }
    }

    bool NativeReconstructionViewHistory::OnOwner() const noexcept {
        return owner_ == std::this_thread::get_id();
    }

    bool NativeReconstructionViewHistory::SameFrontend(RenderFrontend &frontend) const {
        return frontend.preparationOwner_ == owner_ && frontend.resourceRegistry_->Owner() == images_[0].color.texture.owner &&
               compatibility_.deviceGeneration == frontend.Capabilities().support.deviceIncarnation;
    }

    /** @copydoc NativeReconstructionViewHistory::Stage */
    Result<NativeReconstructionScenePlan> NativeReconstructionViewHistory::Stage(RenderFrontend &frontend,
                                                                                 ReconstructionFrameRequest request,
                                                                                 NativeReconstructionSceneViews views,
                                                                                 const NativeReconstructionTemporalSettings &tuning) {
        using Outcome = Result<NativeReconstructionScenePlan>;
        if (!OnOwner())
            return Outcome::Failure(MakeError(TemporalHistoryErrors::WrongThread));
        if (const auto open = frontend.RequireOpenOwner(); open.HasError())
            return Outcome::Failure(open.ErrorValue());
        if (phase_ != Phase::Idle)
            return Outcome::Failure(MakeError(TemporalHistoryErrors::FrameAlreadyPending));
        if (!SameFrontend(frontend) || requiresReset_ || request.compatibility != compatibility_)
            return Outcome::Failure(MakeError(TemporalHistoryErrors::ResetRequired));
        if (!views.color.IsValid() || !views.depth.IsValid() || !views.motion.IsValid() || views.color.owner != request.resourceOwner ||
            views.depth.owner != request.resourceOwner || views.motion.owner != request.resourceOwner)
            return Outcome::Failure(MakeError(ReconstructionErrors::IncompatibleInput));

        candidateSlot_ = publishedSlot_ ? 1U - *publishedSlot_ : 0U;
        const auto &target = images_[candidateSlot_];
        if (request.output.resident && request.output.resident != target.color.texture)
            return Outcome::Failure(MakeError(ReconstructionErrors::IncompatibleOutput));
        request.output.resident = target.color.texture;
        request.output.lifetime = RenderGraphResourceClass::History;
        auto history = BeginTemporalReconstructionFrame(request, frontend.Capabilities().support, store_, handle_);
        if (history.HasError())
            return Outcome::Failure(history.ErrorValue());
        pending_ = std::move(history).Value();
        const auto dispatch = PrepareNativeReconstructionDispatch(request, frontend.Capabilities().support, tuning,
                                                                  NativeReconstructionHistory{store_, *pending_});
        if (dispatch.HasError()) {
            Abandon();
            return Outcome::Failure(dispatch.ErrorValue());
        }

        plan_ = {};
        plan_.dispatch = dispatch.Value();
        plan_.images[Slot(Image::CurrentColor)] = {request.color.texture, views.color};
        plan_.images[Slot(Image::CurrentDepth)] = {request.positiveLinearDepth->texture, views.depth};
        plan_.images[Slot(Image::OutputColor)] = target.color;
        plan_.images[Slot(Image::OutputDepth)] = target.depth;
        if (plan_.dispatch.entry == NativeReconstructionEntry::TemporalHistory) {
            if (!publication_ || !publishedSlot_) {
                Abandon();
                return Outcome::Failure(MakeError(TemporalHistoryErrors::InvalidFrame));
            }
            plan_.previousStates = publication_->states;
            plan_.images[Slot(Image::Motion)] = {request.motion->texture, views.motion};
            plan_.images[Slot(Image::PreviousColor)] = images_[*publishedSlot_].color;
            plan_.images[Slot(Image::PreviousDepth)] = images_[*publishedSlot_].depth;
        }
        phase_ = Phase::Staged;
        return Outcome::Success(plan_);
    }

    /** @copydoc NativeReconstructionViewHistory::CancelStaged */
    Result<void> NativeReconstructionViewHistory::CancelStaged() {
        if (!OnOwner())
            return Result<void>::Failure(MakeError(TemporalHistoryErrors::WrongThread));
        if (phase_ != Phase::Staged)
            return Result<void>::Failure(MakeError(TemporalHistoryErrors::InvalidFrame));
        Abandon();
        return Result<void>::Success();
    }

    /** @copydoc NativeReconstructionViewHistory::Poll */
    Result<bool> NativeReconstructionViewHistory::Poll(RenderFrontend &frontend, const TemporalHistoryCompatibility &current) {
        using Outcome = Result<bool>;
        if (!OnOwner())
            return Outcome::Failure(MakeError(TemporalHistoryErrors::WrongThread));
        if (!SameFrontend(frontend))
            return Outcome::Failure(MakeError(TemporalHistoryErrors::WrongOwner));
        if (phase_ != Phase::Submitted)
            return Outcome::Success(false);
        const auto complete =
            frontend.ObserveSubmission({.completion = completion_, .expectedDeviceIncarnation = compatibility_.deviceGeneration});
        if (complete.HasError())
            return Outcome::Failure(complete.ErrorValue());
        if (!complete.Value() || !leaseRetired_)
            return Outcome::Success(false);
        if (current != compatibility_) {
            requiresReset_ = true;
            Abandon();
            return Outcome::Failure(MakeError(TemporalHistoryErrors::ResetRequired));
        }
        if (const auto published = store_.Publish(*pending_); published.HasError()) {
            requiresReset_ = true;
            Abandon();
            return Outcome::Failure(published.ErrorValue());
        }
        publication_ =
            NativeReconstructionScenePublication{images_[candidateSlot_], candidateStates_, pending_->frameId, pending_->contentGeneration};
        publishedSlot_ = candidateSlot_;
        pending_.reset();
        completion_ = {};
        phase_ = Phase::Idle;
        return Outcome::Success(true);
    }

    /** @copydoc NativeReconstructionViewHistory::Reset */
    Result<void> NativeReconstructionViewHistory::Reset(RenderFrontend &frontend, const TemporalHistoryCompatibility &compatibility,
                                                        const ImagePairs &images, TemporalHistoryResetCause cause) {
        if (!OnOwner())
            return Result<void>::Failure(MakeError(TemporalHistoryErrors::WrongThread));
        if (phase_ != Phase::Idle)
            return Result<void>::Failure(MakeError(TemporalHistoryErrors::FrameAlreadyPending));
        if (!SameFrontend(frontend) || compatibility.view != compatibility_.view)
            return Result<void>::Failure(MakeError(TemporalHistoryErrors::WrongOwner));
        if (const auto open = frontend.RequireOpenOwner(); open.HasError())
            return open;
        if (frontend.activeFrameScope_ != nullptr)
            return Result<void>::Failure(MakeError(TemporalHistoryErrors::InvalidDescriptor));
        if (const auto valid = ValidateImagePairs(frontend, compatibility, images); valid.HasError())
            return valid;
        if (const auto reset = store_.Reset(handle_, compatibility, HistoryTextures(images), cause); reset.HasError())
            return reset;
        images_ = images;
        compatibility_ = compatibility;
        publication_.reset();
        publishedSlot_.reset();
        requiresReset_ = false;
        return Result<void>::Success();
    }

    /** @copydoc NativeReconstructionViewHistory::Published */
    std::optional<NativeReconstructionScenePublication> NativeReconstructionViewHistory::Published(
        const TemporalHistoryCompatibility &current) const noexcept {
        return OnOwner() && !requiresReset_ && current == compatibility_ ? publication_ : std::nullopt;
    }

    void NativeReconstructionViewHistory::BeginRecording(RenderTimelinePoint completion,
                                                         NativeReconstructionHistoryStates states) noexcept {
        completion_ = completion;
        candidateStates_ = states;
        leaseRetired_ = false;
        phase_ = Phase::Recording;
    }

    void NativeReconstructionViewHistory::FinishRecording(FrameDisposition disposition) noexcept {
        if (disposition == FrameDisposition::Submitted) {
            phase_ = Phase::Submitted;
            return;
        }
        // A partial native submit can change even older imported image states.
        requiresReset_ = disposition == FrameDisposition::SubmissionUncertain;
        phase_ = Phase::Discarding;
        if (leaseRetired_)
            Abandon();
    }

    void NativeReconstructionViewHistory::NativeLeaseRetired() noexcept {
        leaseRetired_ = true;
        if (phase_ == Phase::Discarding)
            Abandon();
    }

    void NativeReconstructionViewHistory::Abandon() noexcept {
        if (pending_)
            (void)store_.Abandon(*pending_);
        pending_.reset();
        completion_ = {};
        phase_ = Phase::Idle;
    }
}  // namespace Horo::Render
