#include "EditorViewportColorPipeline.h"

#include <cmath>
#include <limits>

namespace Horo::Editor {
    namespace {
        /** @brief Preserves explicit pending as a typed bounded resource admission result. */
        Error ColorPending() {
            return MakeError(ViewportColorErrors::Pending);
        }
    }  // namespace

    /** @copydoc EditorViewportColorPipeline::EditorViewportColorPipeline */
    EditorViewportColorPipeline::EditorViewportColorPipeline(Render::RenderFrontend &frontend,
                                                             EditorViewportColorResourcePort port) noexcept
        : frontend_(&frontend), port_(port) {}

    /** @copydoc EditorViewportColorPipeline::~EditorViewportColorPipeline */
    EditorViewportColorPipeline::~EditorViewportColorPipeline() {
        static_cast<void>(Close());
    }

    /** @copydoc EditorViewportColorPipeline::CreateStorage */
    Result<void> EditorViewportColorPipeline::CreateStorage(Render::RenderBufferHandle &destination, std::size_t bytes) {
        std::array<std::byte, Render::ExposureHistogramBins * sizeof(std::uint32_t)> zeros{};
        const auto created =
            frontend_->CreateBuffer({bytes, Render::RenderBufferUsage::Storage | Render::RenderBufferUsage::CopyDestination,
                                     Render::RenderBufferAccess::DeviceLocal},
                                    std::span{zeros}.first(bytes));
        return OwnCreation(created, destination);
    }

    /** @copydoc EditorViewportColorPipeline::PrepareStorage */
    Result<void> EditorViewportColorPipeline::PrepareStorage() {
        for (auto &slot : slots_) {
            const std::array<std::pair<Render::RenderBufferHandle *, std::size_t>, 3> buffers{
                {{&slot.histogram, 1024}, {&slot.seed, 32}, {&slot.output, 32}}};
            for (const auto &[destination, bytes] : buffers) {
                const auto created = CreateStorage(*destination, bytes);
                if (created.HasError())
                    return created;
            }
        }
        return DrainCreations();
    }

    /** @copydoc EditorViewportColorPipeline::RequestSettings */
    Result<void> EditorViewportColorPipeline::RequestSettings(const Render::ExposureSettings &settings) {
        if (candidate_)
            return Result<void>::Failure(ColorPending());
        if (const auto valid = Render::ValidateExposureSettings(settings); valid.HasError())
            return valid;
        if (settings.timeSource != Render::ExposureTimeSource::View)
            return Result<void>::Failure(MakeError(Render::ExposureGpuErrors::Unsupported));
        if (settings == settings_)
            return Result<void>::Success();
        if (settingsGeneration_ == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(Render::ExposureGpuErrors::Capacity));
        settings_ = settings;
        ++settingsGeneration_;
        reset_ = true;
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportColorPipeline::SelectSlot */
    Result<std::size_t> EditorViewportColorPipeline::SelectSlot() {
        using Selection = Result<std::size_t>;
        for (std::size_t offset = 0; offset < activeSlots_; ++offset) {
            const auto index = (nextSlot_ + offset) % activeSlots_;
            const auto &slot = slots_[index];
            if (slot.output == previous_.buffer)
                continue;
            bool ready = true;
            for (const auto handle : {slot.histogram, slot.seed, slot.output, slot.uniform[0], slot.uniform[1]}) {
                const auto state = port_.bufferReadiness(port_.context, handle);
                if (state.HasError())
                    return Selection::Failure(state.ErrorValue());
                ready = ready && state.Value() == EditorViewportColorReadiness::Ready;
            }
            if (ready)
                return Selection::Success(index);
        }
        return Selection::Failure(ColorPending());
    }

    /** @copydoc EditorViewportColorPipeline::PrepareIdentity */
    Result<void> EditorViewportColorPipeline::PrepareIdentity(Render::RenderViewId view, Render::FramebufferExtent extent,
                                                              float deltaSeconds) {
        if (generation_ == std::numeric_limits<std::uint64_t>::max() ||
            acceptedSubmission_ >= std::numeric_limits<std::uint64_t>::max() - 1)
            return Result<void>::Failure(MakeError(Render::ExposureGpuErrors::Capacity));
        auto automatic = settings_;
        automatic.mode = Render::ExposureMode::AutomaticHistogram;
        const auto dispatch = Render::PrepareExposureDispatch(automatic, extent, {0, 0, extent.width, extent.height}, deltaSeconds);
        if (dispatch.HasError())
            return Result<void>::Failure(dispatch.ErrorValue());
        const auto ordinal = acceptedSubmission_ + 1;
        auto &slot = slots_[candidateSlot_];
        slot.produced = {.mode = Render::ExposureMode::AutomaticHistogram,
                         .view = view,
                         .generation = ++generation_,
                         .settingsGeneration = settingsGeneration_,
                         .deviceGeneration = frontend_->Capabilities().support.deviceIncarnation,
                         .sourceSubmission = ordinal,
                         .consumeSubmission = ordinal + 1,
                         .buffer = slot.output};
        dispatch_ = {dispatch.Value(),
                     {static_cast<std::uint32_t>(generation_), static_cast<std::uint32_t>(generation_ >> 32),
                      static_cast<std::uint32_t>(ordinal), static_cast<std::uint32_t>(ordinal >> 32)}};
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportColorPipeline::ValidateOutput */
    Result<void> EditorViewportColorPipeline::ValidateOutput() {
        const auto actual = frontend_->PresentationOutput();
        if (actual.HasError())
            return Result<void>::Failure(actual.ErrorValue());
        const auto &output = actual.Value();
        const Render::RenderOutputColor legacySdr;
        if (output.active != legacySdr || output.requested.color != legacySdr)
            return Result<void>::Failure(MakeError(Render::RenderPresentationErrors::OutputUnsupported));
        if (output.revision != output_.revision || output.display.revision != output_.display.revision)
            reset_ = true;
        output_ = output;
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportColorPipeline::ValidatePreparation */
    Result<void> EditorViewportColorPipeline::ValidatePreparation(Render::RenderViewId view, Render::FramebufferExtent extent) const {
        if (candidate_ || !kernel_ || !view.IsValid())
            return Result<void>::Failure(MakeError(Render::ExposureGpuErrors::InvalidInput));
        if (std::uint64_t{extent.width} * extent.height > 16'777'216U)
            return Result<void>::Failure(MakeError(Render::ExposureGpuErrors::Capacity));
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportColorPipeline::Prepare */
    Result<void> EditorViewportColorPipeline::Prepare(Render::RenderViewId view, Render::FramebufferExtent extent, float deltaSeconds) {
        if (const auto valid = ValidatePreparation(view, extent); valid.HasError())
            return valid;
        if (const auto output = ValidateOutput(); output.HasError()) {
            status_ = EditorViewportColorStatus::Failed;
            return output;
        }

        const auto slot = SelectSlot();
        if (slot.HasError()) {
            status_ = slot.ErrorValue().code.Value() == ViewportColorErrors::Pending.code.Value() ? EditorViewportColorStatus::Preparing
                                                                                                  : EditorViewportColorStatus::Failed;
            return Result<void>::Failure(slot.ErrorValue());
        }
        candidateSlot_ = slot.Value();
        pendingStateCount_ = 0;
        if (view != view_ || extent != extent_)
            reset_ = true;
        if (const auto identity = PrepareIdentity(view, extent, deltaSeconds); identity.HasError())
            return identity;
        view_ = view;
        extent_ = extent;
        candidate_ = true;
        encoded_ = false;
        status_ = EditorViewportColorStatus::Prepared;
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportColorPipeline::Finish */
    void EditorViewportColorPipeline::Finish(bool accepted) noexcept {
        if (!candidate_)
            return;
        if (accepted && encoded_) {
            previous_ = slots_[candidateSlot_].produced;
            acceptedSubmission_ = previous_.sourceSubmission;
            reset_ = resetQueued_;
            PublishBufferStates();
            tablesUsed_ = true;
        }
        if (resetQueued_)
            reset_ = true;
        resetQueued_ = false;
        pendingStateCount_ = 0;
        nextSlot_ = (candidateSlot_ + 1) % activeSlots_;
        candidate_ = false;
        encoded_ = false;
        status_ = EditorViewportColorStatus::Ready;
    }

    /** @copydoc EditorViewportColorPipeline::Encoded */
    void EditorViewportColorPipeline::Encoded() noexcept {
        if (candidate_) {
            encoded_ = true;
            status_ = EditorViewportColorStatus::Encoded;
        }
    }

    /** @copydoc EditorViewportColorPipeline::Reset */
    void EditorViewportColorPipeline::Reset() noexcept {
        if (candidate_)
            resetQueued_ = true;
        else
            reset_ = true;
    }

    /** @copydoc EditorViewportColorPipeline::Produced */
    const Render::ExposureState &EditorViewportColorPipeline::Produced() const noexcept {
        return slots_[candidateSlot_].produced;
    }

    /** @copydoc EditorViewportColorPipeline::Previous */
    const Render::ExposureState &EditorViewportColorPipeline::Previous() const noexcept {
        return reset_ ? empty_ : previous_;
    }

    /** @copydoc EditorViewportColorPipeline::MeterBuffers */
    std::array<Render::RenderBufferHandle, 3> EditorViewportColorPipeline::MeterBuffers() const noexcept {
        const auto &slot = slots_[candidateSlot_];
        return {slot.histogram, slot.seed, slot.output};
    }

    /** @copydoc EditorViewportColorPipeline::Dispatch */
    const Render::ExposureGpuDispatchParameters &EditorViewportColorPipeline::Dispatch() const noexcept {
        return dispatch_;
    }

    /** @copydoc EditorViewportColorPipeline::Kernel */
    const std::shared_ptr<Render::IResidentExposureGpuKernel> &EditorViewportColorPipeline::Kernel() const noexcept {
        return kernel_;
    }

    /** @copydoc EditorViewportColorPipeline::Settings */
    const Render::ExposureSettings &EditorViewportColorPipeline::Settings() const noexcept {
        return settings_;
    }

    /** @copydoc EditorViewportColorPipeline::Diagnostics */
    EditorViewportColorDiagnostics EditorViewportColorPipeline::Diagnostics() const noexcept {
        return {.status = status_,
                .active = kernel_ != nullptr,
                .candidate = candidate_,
                .automatic = settings_.mode == Render::ExposureMode::AutomaticHistogram,
                .resetPending = reset_,
                .settingsGeneration = settingsGeneration_,
                .acceptedSubmission = acceptedSubmission_,
                .exposureGeneration = generation_,
                .outputRevision = output_.revision,
                .unmanagedSdrPreview = output_.revision != 0};
    }
}  // namespace Horo::Editor
