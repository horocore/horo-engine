#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "RuntimeUiPaintHostInternal.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

namespace Horo::Application::Internal {
    using namespace Runtime::Ui;
    using State = Detail::RuntimeUiPaintHostState;
    using Source = Detail::RuntimeUiPaintSource;

    namespace {
        /** @brief Reserves the complete World pointer route and semantic queue before source publication. */
        Result<void> PrepareWorldInteraction(Source &candidate, const UiRenderSnapshotDescriptor &source) {
            auto dispatcher = UiEventDispatcher::Create({source.instance, source.canvas, source.document, MaximumUiTreeDepth});
            if (dispatcher.HasError())
                return Result<void>::Failure(dispatcher.ErrorValue());
            auto feedback = UiFeedbackQueue::Create({{source.instance, source.canvas, source.document, source.documentRevision,
                                                      source.treeRevision, source.interactionRevision},
                                                     MaximumUiInteractionSamples});
            if (feedback.HasError())
                return Result<void>::Failure(feedback.ErrorValue());
            candidate.worldDispatcher.emplace(std::move(dispatcher).Value());
            candidate.worldFeedback.emplace(std::move(feedback).Value());
            return Result<void>::Success();
        }
    }  // namespace

    /** @brief Actual application Scene participant candidate; the generic Scene claim remains the only publication authority. */
    class RuntimeUiPaintSceneTransaction final : public Runtime::SceneActivationCandidate {
    public:
        RuntimeUiPaintSceneTransaction(std::shared_ptr<State> state, const UiSceneTransitionRequest &transition)
            : state_(std::move(state)), transition_(transition), revision_(state_->sourceRevision) {
            reservations_.fill(NoUiRenderIndex);
        }

        ~RuntimeUiPaintSceneTransaction() override {
            Shutdown();
        }

        Result<void> ValidatePublication() const override {
            if (state_->owner != std::this_thread::get_id() || state_->revoked || state_->dispatching ||
                revision_ != state_->sourceRevision || !publication_)
                return Result<void>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
            if (sealed_)
                return publication_->CanPublish();
            if (state_->sceneSealed)
                return Result<void>::Failure(MakeError(UiErrors::CapabilityBusy));
            if (const auto sealed = publication_->Seal(UiStructuralCommitPoint::CommitDeferredLifecycleChanges); sealed.HasError())
                return sealed;
            state_->sceneSealed = true;
            sealed_ = true;
            return publication_->CanPublish();
        }

        void Publish() noexcept override {
            if (!sealed_ || published_ || revision_ != state_->sourceRevision)
                std::terminate();
            publication_->Publish();
            if (transition_.previous)
                RetireScope(*transition_.previous);
            for (auto &source : incoming_) {
                if (!source)
                    continue;
                const auto free = std::ranges::find_if(state_->sceneSources, [](const auto &entry) {
                    return !entry;
                });
                if (free == state_->sceneSources.end())
                    std::terminate();
                *free = std::move(source);
            }
            ++state_->sourceRevision;
            state_->phase = std::ranges::any_of(state_->Sources(),
                                                [](auto *source) {
                return source != nullptr;
            })
                                ? RuntimeUiPaintHostPhase::Active
                                : RuntimeUiPaintHostPhase::Retiring;
            sealed_ = false;
            state_->sceneSealed = false;
            published_ = true;
        }

        void Shutdown() noexcept override {
            if (closed_)
                return;
            if (state_->owner != std::this_thread::get_id())
                std::terminate();
            closed_ = true;
            if (sealed_) {
                sealed_ = false;
                state_->sceneSealed = false;
            }
            publication_.reset();
            if (published_) {
                if (transition_.next) {
                    state_->service.RevokeScene(*transition_.next);
                    if (RetireScope(*transition_.next) && state_->sourceRevision != std::numeric_limits<std::uint64_t>::max())
                        ++state_->sourceRevision;
                }
            } else {
                for (auto &source : incoming_)
                    if (source)
                        Retain(std::move(source));
            }
            for (const auto slot : reservations_)
                if (slot != NoUiRenderIndex && !state_->retiredSources[slot] && !Occupied(slot))
                    state_->reservedRetirements[slot] = false;
        }

        /** @brief Reserves the complete product/retirement inventory before any canonical or native preparation. */
        Result<void> Reserve(const std::size_t count) {
            std::size_t preserved = 0;
            for (auto *source : state_->Sources())
                if (source && (!transition_.previous || source->scene != transition_.previous))
                    ++preserved;
            if (const auto capacity = std::min(Detail::MaximumSceneUiSources, state_->frontend->MaximumUiCapturesPerFrame());
                count > capacity || preserved > capacity - count || state_->sourceRevision == std::numeric_limits<std::uint64_t>::max())
                return Result<void>::Failure(MakeError(UiErrors::CapacityExceeded));
            std::size_t reserved = 0;
            for (std::size_t index = 0; index < state_->reservedRetirements.size() && reserved < count; ++index)
                if (!state_->reservedRetirements[index] && !state_->retiredSources[index]) {
                    state_->reservedRetirements[index] = true;
                    reservations_[reserved++] = static_cast<std::uint32_t>(index);
                }
            return reserved == count ? Result<void>::Success() : Result<void>::Failure(MakeError(UiErrors::CapacityExceeded));
        }

        /** @brief Prepares the same private generation, initial derived caches and complete native program before sealing. */
        Result<void> Prepare(std::vector<RuntimeUiPaintSceneSourcePreparation> inputs, std::vector<UiSceneBindingReplacement> replacements,
                             const CancellationToken &cancellation);

    private:
        /** @brief Moves validated activations into the sole canonical preparation, preserving coordinated outgoing owners. */
        Result<UiSceneReconciliation::Prepared> PrepareCanonical(std::vector<RuntimeUiPaintSceneSourcePreparation> &inputs,
                                                                 std::vector<UiSceneBindingReplacement> replacements,
                                                                 const CancellationToken &cancellation);
        /** @brief Retains one derived candidate before fallible resource admission so every failure follows reserved retirement. */
        Result<void> PrepareSource(RuntimeUiPaintSourcePreparation &input, const UiSceneReconciliation::Prepared &prepared,
                                   std::size_t index, const CancellationToken &cancellation);
        /** @brief Prepares output, the initial immutable program and optional startup for an already retained candidate. */
        Result<void> PrepareProgram(Source &candidate, const UiReloadLease &pin, const CancellationToken &cancellation) const;
        /** @brief Completes native readiness and inventory checks before acquiring the sole publication token. */
        Result<void> PreparePublication(UiSceneReconciliation::Prepared prepared, bool hasIncoming, const CancellationToken &cancellation);

        /** @brief Drains only the already bounded cold request inventory; no GPU wait or unbounded replenishment is admitted. */
        Result<void> RealizeResources() const {
            std::size_t remaining = state_->frontend->UploadSnapshot().pendingRequests;
            while (remaining != 0) {
                const auto realized = state_->frontend->ProcessResourceRequests();
                if (realized.HasError())
                    return Result<void>::Failure(realized.ErrorValue());
                if (state_->revoked || !state_->frontend)
                    return Result<void>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
                if (realized.Value() == 0 || realized.Value() > remaining)
                    return Result<void>::Failure(MakeError(Render::UiPaintFrameErrors::Pending));
                remaining -= realized.Value();
            }
            for (const auto &source : incoming_)
                if (source && source->programs.active)
                    if (const auto ready = source->programs.active->ValidatePreparation(); ready.HasError())
                        return ready;
            return Result<void>::Success();
        }

        /** @brief Rejects complete admitted potential work before Scene publication; actual frame admission remains independently strict.
         */
        Result<void> ValidateInventory() const {
            std::size_t passes{};
            std::size_t resources{};
            std::size_t draws{};
            std::array<Runtime::SceneRuntimeId, Detail::MaximumSceneUiSources> viewportOwners{};
            std::size_t viewportCount{};
            const auto add = [](std::size_t &sum, const std::size_t count, const std::size_t maximum) {
                if (count > maximum || sum > maximum - count)
                    return false;
                sum += count;
                return true;
            };
            const auto include = [&](const Source &source) {
                if (source.sceneCanvas) {
                    const auto owners = std::span{viewportOwners}.first(viewportCount);
                    if (!source.scene || viewportCount == viewportOwners.size() || std::ranges::find(owners, *source.scene) != owners.end())
                        return false;
                    viewportOwners[viewportCount++] = *source.scene;
                    return source.programs.policy.capacity.IsValid();
                }
                if (!source.programs.active)
                    return false;
                const auto inventory = source.programs.active->Inventory();
                return inventory.passes != 0 && add(passes, inventory.passes, Render::RenderGraphLimits::HardMaxPasses) &&
                       add(resources, inventory.resources, Render::RenderGraphLimits::HardMaxResources) &&
                       add(draws, inventory.draws, Render::UiPaintLimits::MaximumDraws);
            };
            for (auto *source : state_->Sources())
                if (source && (!transition_.previous || source->scene != transition_.previous) && !include(*source))
                    return Result<void>::Failure(MakeError(UiErrors::CapacityExceeded));
            for (const auto &source : incoming_)
                if (source && !include(*source))
                    return Result<void>::Failure(MakeError(UiErrors::CapacityExceeded));
            return Result<void>::Success();
        }

        /** @brief Transfers one exact source into its previously reserved retirement slot without teardown or destruction. */
        void Retain(std::unique_ptr<Source> source) const noexcept {
            const auto slot = source->retirementSlot;
            if (slot >= state_->retiredSources.size() || state_->retiredSources[slot])
                std::terminate();
            source->retiring = true;
            state_->retiredSources[slot] = std::move(source);
        }

        /** @brief Retires only this exact runtime Scene incarnation; replacement's newly promoted scope is untouched. */
        bool RetireScope(const Runtime::SceneRuntimeId scene) const noexcept {
            bool changed = false;
            for (auto &source : state_->sceneSources)
                if (source && source->scene == scene) {
                    Retain(std::move(source));
                    changed = true;
                }
            return changed;
        }

        /** @brief Keeps reservation ownership with its exact active source until explicit quiescent collection. */
        bool Occupied(const std::uint32_t slot) const noexcept {
            return std::ranges::any_of(state_->sceneSources, [slot](const auto &source) {
                return source && source->retirementSlot == slot;
            });
        }

        std::shared_ptr<State> state_;
        UiSceneTransitionRequest transition_;
        std::uint64_t revision_;
        std::array<std::unique_ptr<Source>, Detail::MaximumSceneUiSources> incoming_;
        std::array<std::uint32_t, Detail::MaximumSceneUiSources> reservations_;
        mutable std::optional<RuntimeUiService::ScenePublication> publication_;
        mutable bool sealed_{};
        bool published_{};
        bool closed_{};
    };

    /** @copydoc RuntimeUiPaintSceneTransaction::Prepare */
    Result<void> RuntimeUiPaintSceneTransaction::Prepare(std::vector<RuntimeUiPaintSceneSourcePreparation> inputs,
                                                         std::vector<UiSceneBindingReplacement> replacements,
                                                         const CancellationToken &cancellation) {
        if (const auto reserved = Reserve(inputs.size()); reserved.HasError())
            return reserved;
        auto preparation = PrepareCanonical(inputs, std::move(replacements), cancellation);
        if (preparation.HasError())
            return Result<void>::Failure(preparation.ErrorValue());
        auto prepared = std::move(preparation).Value();
        for (std::size_t index = 0; index < inputs.size(); ++index)
            if (const auto source = PrepareSource(inputs[index].paint, prepared, index, cancellation); source.HasError())
                return source;
        return PreparePublication(std::move(prepared), !inputs.empty(), cancellation);
    }

    /** @copydoc RuntimeUiPaintSceneTransaction::PrepareCanonical */
    Result<UiSceneReconciliation::Prepared> RuntimeUiPaintSceneTransaction::PrepareCanonical(
        std::vector<RuntimeUiPaintSceneSourcePreparation> &inputs, std::vector<UiSceneBindingReplacement> replacements,
        const CancellationToken &cancellation) {
        std::vector<UiSceneActivation> incoming;
        incoming.reserve(inputs.size());
        for (auto &input : inputs) {
            if (input.paint.instance != input.activation.descriptor.instance || input.activation.descriptor.owner.scene != transition_.next)
                return Result<UiSceneReconciliation::Prepared>::Failure(MakeError(UiErrors::HandleOwnerMismatch));
            incoming.push_back(std::move(input.activation));
        }
        std::array<RuntimeUiInstanceId, Detail::MaximumSceneUiSources> coordinated;
        std::size_t count = 0;
        for (auto *source : state_->Sources())
            if (source && transition_.previous && source->scene == transition_.previous)
                coordinated[count++] = source->instance;
        auto preparation = state_->service.PrepareTransition(transition_, std::move(replacements), std::move(incoming),
                                                             {coordinated.data(), count}, cancellation);
        if (preparation.HasError())
            return Result<UiSceneReconciliation::Prepared>::Failure(preparation.ErrorValue());
        return preparation;
    }

    /** @copydoc RuntimeUiPaintSceneTransaction::PrepareSource */
    Result<void> RuntimeUiPaintSceneTransaction::PrepareSource(RuntimeUiPaintSourcePreparation &input,
                                                               const UiSceneReconciliation::Prepared &prepared, const std::size_t index,
                                                               const CancellationToken &cancellation) {
        if (state_->revoked || !state_->frontend || cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        if ((input.allocatorLifetime && input.allocatorLifetime.get() != &input.allocator) ||
            input.extraction.view != input.geometry.view || input.program.frameSlots == 0)
            return Result<void>::Failure(MakeError(UiErrors::HandleMalformed));
        auto pin = state_->service.AcquirePrepared(prepared, input.instance);
        if (pin.HasError())
            return Result<void>::Failure(pin.ErrorValue());
        auto projection = RuntimeUiPaintProjection::PrepareReload(*pin.Value().Get(), std::move(input.declaration), input.extraction);
        if (projection.HasError())
            return Result<void>::Failure(projection.ErrorValue());
        auto arena = UiRenderGeometryArena::Create(input.geometry);
        if (arena.HasError())
            return Result<void>::Failure(arena.ErrorValue());
        auto animation = state_->service.PrepareAnimation(prepared, input.instance, input.allocator, std::move(input.registry),
                                                          std::move(input.styles), std::move(input.animation), input.animationLimits);
        if (animation.HasError())
            return Result<void>::Failure(animation.ErrorValue());
        auto derived = Runtime::UiAnimationRuntimeParticipant::Compose(std::move(animation).Value(), state_->dispatch, input.runtime);
        if (derived.HasError())
            return Result<void>::Failure(derived.ErrorValue());
        auto source = std::make_unique<Source>(input.instance, std::move(derived).Value(), std::move(projection).Value(),
                                               std::move(input.resources), std::move(arena).Value(),
                                               Detail::RuntimeUiPaintSourcePolicy{input.extraction, input.geometry, input.program,
                                                                                  std::move(input.kernel), input.runtime.viewport},
                                               std::move(input.allocatorLifetime));
        source->scene = transition_.next;
        source->worldCanvas = std::move(input.worldCanvas);
        source->sceneCanvas = input.sceneCanvas;
        source->retirementSlot = reservations_[index];
        incoming_[index] = std::move(source);
        auto &candidate = *incoming_[index];
        return PrepareProgram(candidate, pin.Value(), cancellation);
    }

    /** @copydoc RuntimeUiPaintSceneTransaction::PrepareProgram */
    Result<void> RuntimeUiPaintSceneTransaction::PrepareProgram(Source &candidate, const UiReloadLease &pin,
                                                                const CancellationToken &cancellation) const {
        if (const auto resources = candidate.AdmitResources(); resources.HasError())
            return resources;
        if (!candidate.worldCanvas)
            candidate.output.pending = candidate.sceneCanvas ? state_->cameraOutput : state_->output;
        if ((candidate.output.canvas.renderMode == UiRenderMode::WorldSpace) != candidate.worldCanvas.has_value())
            return Result<void>::Failure(MakeError(UiErrors::CanvasSpaceModeMismatch));
        if (candidate.sceneCanvas && !candidate.worldCanvas && candidate.output.canvas.renderMode != UiRenderMode::ScreenSpaceCamera)
            return Result<void>::Failure(MakeError(UiErrors::CanvasSpaceModeMismatch));
        if (const auto output = candidate.ApplyOutput(); output.HasError())
            return output;
        if (state_->accessibility.revision != 0) {
            if (auto admitted = candidate.animation->SetVisualAccessibility(state_->accessibility); admitted.HasError())
                return admitted;
        }
        auto baseline = candidate.animation->PrepareSourcePresentation();
        if (baseline.HasError())
            return Result<void>::Failure(baseline.ErrorValue());
        auto prototype = candidate.projection->ExtractPrototype(*pin.Get(), baseline.Value());
        if (prototype.HasError())
            return Result<void>::Failure(prototype.ErrorValue());
        if (candidate.worldCanvas)
            if (const auto interaction = PrepareWorldInteraction(candidate, prototype.Value().Descriptor()); interaction.HasError())
                return interaction;
        if (!candidate.sceneCanvas) {
            auto program = Render::UiPaintProgram::Create(*state_->frontend, candidate.programs.policy, prototype.Value(),
                                                          candidate.textures, candidate.programs.kernel, cancellation);
            if (program.HasError())
                return Result<void>::Failure(program.ErrorValue());
            candidate.programs.active = std::move(program).Value();
        }
        candidate.refresh = false;
        if (state_->started) {
            if (const auto started = candidate.animation->Startup(state_->cancellation.Token()); started.HasError())
                return started;
            candidate.started = true;
        }
        return Result<void>::Success();
    }

    /** @copydoc RuntimeUiPaintSceneTransaction::PreparePublication */
    Result<void> RuntimeUiPaintSceneTransaction::PreparePublication(UiSceneReconciliation::Prepared prepared, const bool hasIncoming,
                                                                    const CancellationToken &cancellation) {
        if (state_->revoked || !state_->frontend || cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        if (hasIncoming) {
            if (const auto ready = RealizeResources(); ready.HasError())
                return ready;
        }
        if (state_->revoked || cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        if (const auto inventory = ValidateInventory(); inventory.HasError())
            return inventory;
        auto publication = state_->service.PreparePublication(std::move(prepared));
        if (publication.HasError())
            return Result<void>::Failure(publication.ErrorValue());
        publication_.emplace(std::move(publication).Value());
        return Result<void>::Success();
    }

    /** @copydoc RuntimeUiPaintPort::PrepareScene */
    Result<std::unique_ptr<Runtime::SceneActivationCandidate>> RuntimeUiPaintPort::PrepareScene(
        const UiSceneTransitionRequest &transition, std::vector<RuntimeUiPaintSceneSourcePreparation> incoming,
        std::vector<UiSceneBindingReplacement> replacements, const CancellationToken &cancellation) const {
        using Prepared = Result<std::unique_ptr<Runtime::SceneActivationCandidate>>;
        if (!state_ || !state_->Available() || state_->dispatching)
            return Prepared::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        const auto &capabilities = state_->frontend->Capabilities();
        if (!capabilities.support.features.Supports(Render::RenderCapability::UiPaint) &&
            std::ranges::any_of(incoming, [&capabilities](const auto &source) {
            return !source.paint.sceneCanvas || !capabilities.supportsMaterialRasterGraph;
        }))
            return Prepared::Failure(MakeError(Render::UiPaintFrameErrors::Unavailable));

        struct PreparationGuard final {
            PreparationGuard(const PreparationGuard &) = delete;
            PreparationGuard &operator=(const PreparationGuard &) = delete;
            PreparationGuard(PreparationGuard &&) = delete;
            PreparationGuard &operator=(PreparationGuard &&) = delete;

            State &state;

            explicit PreparationGuard(State &value) : state(value) {
                state.scenePreparing = true;
            }

            ~PreparationGuard() {
                state.scenePreparing = false;
            }
        };

        PreparationGuard guard{*state_};

        auto candidate = std::make_unique<RuntimeUiPaintSceneTransaction>(state_, transition);
        if (const auto prepared = candidate->Prepare(std::move(incoming), std::move(replacements), cancellation); prepared.HasError())
            return Prepared::Failure(prepared.ErrorValue());
        return Prepared::Success(std::move(candidate));
    }
}  // namespace Horo::Application::Internal
