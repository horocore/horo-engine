#include "GpuParticleSimulatorInternal.h"

#include <algorithm>
#include <cmath>

namespace Horo::Vfx {
    namespace {
        using State = Detail::GpuParticleSimulatorState;

        /** @brief Checks owner/lifetime without allocating an expected frame-hot diagnostic. */
        GpuParticleStepStatus Operational(const State &s) noexcept {
            using enum Horo::Vfx::GpuParticleStepStatus;
            if (s.owner != std::this_thread::get_id())
                return ThreadViolation;
            if (s.shutdown)
                return ShutDown;
            if (s.deviceLost)
                return DeviceLost;
            if (s.uncertain)
                return SubmissionUncertain;
            if (s.stopped || !s.active || !s.receipt.IsLive())
                return NotActive;
            return Complete;
        }

        /** @brief Checks new logical input before constructing an immutable candidate. */
        GpuParticleStepStatus RequestStatus(const State &s, std::uint64_t tick, float delta, const CancellationToken &token) noexcept {
            using enum Horo::Vfx::GpuParticleStepStatus;
            const auto &bounds = s.preparation.descriptor.arithmetic;
            if (s.nextTick >= bounds.maximumSteps)
                return StepLimit;
            if (tick != s.nextTick || !std::isfinite(delta) || delta < 0 || delta > bounds.maximumDeltaSeconds)
                return InvalidStep;
            if (token.IsCancellationRequested())
                return Cancelled;
            return Complete;
        }

        /** @brief Distinguishes unavailable optional observations from required visual observation failure. */
        bool ReadbackMatches(const Render::VfxGpuReadbackDescriptor &policy, Render::VfxGpuReadbackAdmission admission) noexcept {
            using enum Horo::Render::VfxGpuReadbackAdmission;
            if (!policy.enabled)
                return admission == Disabled;
            if (admission == Available)
                return true;
            return policy.requirement == Render::VfxGpuReadbackRequirement::Optional && admission == OptionalUnavailable;
        }

        /** @brief Compares all semantic metadata by rehashing the same actual immutable source words under the issued descriptor. */
        bool ReceiptMatches(const State &s, const Render::VfxGpuPreparationReceipt &r) noexcept {
            if (!r.IsLive() || !r.Id().IsValid() || !r.DeviceIncarnation())
                return false;
            if (!Render::SameVfxGpuIdentity(s.preparation.descriptor.identity, r.Descriptor().identity))
                return false;
            auto source = s.preparation;
            source.descriptor = r.Descriptor();
            return r.Descriptor().initializationDigest == s.preparation.descriptor.initializationDigest &&
                   Render::ComputeVfxGpuInitializationDigest(source) == s.preparation.descriptor.initializationDigest &&
                   ReadbackMatches(source.descriptor.readback, r.Readback());
        }

        /** @brief Captures one complete step and cooperative cancellation observers without heap allocation. */
        Render::GpuVfxSimulationWork Work(const State &s, float delta, const CancellationToken &caller) noexcept {
            Render::GpuVfxSimulationWork work;
            work.identity = s.preparation.descriptor.identity;
            work.step = {s.receipt.Id(), s.receipt.DeviceIncarnation(), s.nextTick, s.nextTick + 1, s.nextTick + 2};
            work.deltaSeconds = delta;
            work.callerCancellation = caller;
            work.lifetimeCancellation = s.lifetimeToken;
            const auto &policy = s.preparation.descriptor.readback;
            work.observe =
                s.receipt.Readback() == Render::VfxGpuReadbackAdmission::Available && (s.nextTick + 1) % policy.cadenceSteps == 0;
            return work;
        }

        /** @brief Validates acknowledgement identity/liveness independently from its native submission outcome. */
        bool AckMatches(const State &s, const Render::GpuVfxStepAcknowledgement &ack) noexcept {
            return ack.IsLive() && s.receipt.IsLive() && ack.Step().simulation == s.receipt.Id() &&
                   ack.Step().deviceIncarnation == s.receipt.DeviceIncarnation();
        }

        /** @brief Records actual accepted native work once; it does not claim GPU completion or compare particle values. */
        GpuParticleStepStatus AcceptSubmitted(State &s, const Render::GpuVfxStepAcknowledgement &ack) noexcept {
            using enum Horo::Vfx::GpuParticleStepStatus;
            if (!s.pending || s.pending->step != ack.Step() || !ack.Submission().IsValid())
                return StaleAcknowledgement;
            auto slot = std::ranges::find(s.submitted, false, &Detail::GpuParticlePending::occupied);
            if (slot == s.submitted.end())
                return CapacityExceeded;
            *slot = {ack.Step(), ack.Submission(), true};
            ++s.nextTick;
            s.pending.reset();
            return Complete;
        }

        /** @brief Only matching actual completion evidence releases logical in-flight capacity. */
        GpuParticleStepStatus RetireCompleted(State &s, const Render::GpuVfxStepAcknowledgement &ack) noexcept {
            auto slot = std::ranges::find_if(s.submitted, [&ack](const auto &record) {
                return record.occupied && record.step == ack.Step();
            });
            if (slot == s.submitted.end() || slot->submission != ack.Submission())
                return GpuParticleStepStatus::StaleAcknowledgement;
            slot->occupied = false;
            return GpuParticleStepStatus::Complete;
        }

        /** @brief Clears only a proven unsubmitted candidate so the exact tick may be retried safely. */
        GpuParticleStepStatus Unsubmitted(State &s, const Render::GpuVfxStepAcknowledgement &ack) noexcept {
            using enum Horo::Vfx::GpuParticleStepStatus;
            if (!s.pending || s.pending->step != ack.Step() || ack.Submission().IsValid())
                return StaleAcknowledgement;
            s.pending.reset();
            return ack.State() == Render::VfxGpuSubmissionState::CancelledUnsubmitted ? Cancelled : Complete;
        }

        /** @brief Contains an uncertain or lost native submission; it can never become an unsubmitted retry. */
        GpuParticleStepStatus TerminalNative(State &s, const Render::GpuVfxStepAcknowledgement &ack) noexcept {
            const bool candidate = s.pending && s.pending->step == ack.Step();
            const bool submitted = std::ranges::any_of(s.submitted, [&ack](const auto &record) {
                return record.occupied && record.step == ack.Step();
            });
            if (!candidate && !submitted)
                return GpuParticleStepStatus::StaleAcknowledgement;
            s.deviceLost = ack.State() == Render::VfxGpuSubmissionState::DeviceLost;
            s.uncertain = !s.deviceLost;
            s.active = false;
            s.lifetimeSource.RequestCancellation();
            return s.deviceLost ? GpuParticleStepStatus::DeviceLost : GpuParticleStepStatus::SubmissionUncertain;
        }
    }  // namespace

    /** @copydoc GpuParticleSimulator::GpuParticleSimulator */
    GpuParticleSimulator::GpuParticleSimulator(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    GpuParticleSimulator::~GpuParticleSimulator() {
        if (state_)
            state_->lifetimeSource.RequestCancellation();
    }

    /** @copydoc GpuParticleSimulator::Create */
    Result<std::unique_ptr<GpuParticleSimulator>> GpuParticleSimulator::Create(const ParticleCookAdmission &admission,
                                                                               const GpuParticleSimulatorCreateInfo &info) {
        auto state = GpuParticleDetail::PrepareGpuState(admission, info);
        if (state.HasError())
            return Result<std::unique_ptr<GpuParticleSimulator>>::Failure(state.ErrorValue());
        try {
            return Result<std::unique_ptr<GpuParticleSimulator>>::Success(
                std::unique_ptr<GpuParticleSimulator>{new GpuParticleSimulator(std::move(state).Value())});
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<GpuParticleSimulator>>::Failure(MakeError(VfxErrors::GpuSimulationAllocationFailed));
        }
    }

    /** @copydoc GpuParticleSimulator::Preparation */
    const Render::VfxGpuSimulationPreparation &GpuParticleSimulator::Preparation() const noexcept {
        return state_->preparation;
    }

    /** @copydoc GpuParticleSimulator::Activate */
    Result<void> GpuParticleSimulator::Activate(const Render::VfxGpuPreparationReceipt &receipt) {
        auto &s = *state_;
        if (s.owner != std::this_thread::get_id() || s.active || s.stopped || s.shutdown || !ReceiptMatches(s, receipt))
            return Result<void>::Failure(MakeError(VfxErrors::GpuSimulationReceiptInvalid));
        s.receipt = receipt;
        s.active = true;
        return Result<void>::Success();
    }

    /** @copydoc GpuParticleSimulator::PrepareStep */
    GpuParticleStepOutcome GpuParticleSimulator::PrepareStep(std::uint64_t tick, float delta,
                                                             const CancellationToken &cancellation) noexcept {
        using enum Horo::Vfx::GpuParticleStepStatus;
        auto &s = *state_;
        if (const auto status = Operational(s); status != Complete)
            return {status, {}};
        if (s.pending)
            return {PendingSubmission, {}};
        if (const auto status = RequestStatus(s, tick, delta, cancellation); status != Complete)
            return {status, {}};
        if (std::ranges::all_of(s.submitted, &Detail::GpuParticlePending::occupied))
            return {CapacityExceeded, {}};
        const auto work = Work(s, delta, cancellation);
        if (!Render::ValidateVfxGpuSimulationWork(s.preparation.descriptor, work))
            return {InvalidStep, {}};
        s.pending = work;
        return {Complete, work};
    }

    /** @copydoc GpuParticleSimulator::ApplyAcknowledgement */
    GpuParticleStepStatus GpuParticleSimulator::ApplyAcknowledgement(const Render::GpuVfxStepAcknowledgement &ack) noexcept {
        auto &s = *state_;
        if (s.owner != std::this_thread::get_id())
            return GpuParticleStepStatus::ThreadViolation;
        if (s.shutdown)
            return GpuParticleStepStatus::ShutDown;
        if (!AckMatches(s, ack))
            return GpuParticleStepStatus::StaleAcknowledgement;
        using enum Render::VfxGpuSubmissionState;
        switch (ack.State()) {
            case Accepted:
                return AcceptSubmitted(s, ack);
            case Completed:
                return RetireCompleted(s, ack);
            case RejectedUnsubmitted:
            case CancelledUnsubmitted:
                return Unsubmitted(s, ack);
            case SubmissionUncertain:
            case DeviceLost:
                return TerminalNative(s, ack);
        }
        return GpuParticleStepStatus::StaleAcknowledgement;
    }

    /** @copydoc GpuParticleSimulator::PendingWork */
    std::span<const Render::GpuVfxSimulationWork> GpuParticleSimulator::PendingWork() const noexcept {
        if (state_->owner != std::this_thread::get_id() || state_->stopped || state_->shutdown || !state_->pending)
            return {};
        return {&*state_->pending, 1};
    }

    /** @copydoc GpuParticleSimulator::LifetimeCancellation */
    const CancellationToken &GpuParticleSimulator::LifetimeCancellation() const noexcept {
        return state_->lifetimeToken;
    }

    /** @copydoc GpuParticleSimulator::Stop */
    GpuParticleStepStatus GpuParticleSimulator::Stop() noexcept {
        using enum Horo::Vfx::GpuParticleStepStatus;
        auto &s = *state_;
        if (s.owner != std::this_thread::get_id())
            return ThreadViolation;
        if (s.shutdown)
            return ShutDown;
        s.stopped = true;
        s.active = false;
        s.pending.reset();
        s.lifetimeSource.RequestCancellation();
        return Complete;
    }

    /** @copydoc GpuParticleSimulator::Shutdown */
    GpuParticleStepStatus GpuParticleSimulator::Shutdown() noexcept {
        auto &s = *state_;
        if (s.owner != std::this_thread::get_id())
            return GpuParticleStepStatus::ThreadViolation;
        s.shutdown = true;
        s.active = false;
        s.pending.reset();
        s.lifetimeSource.RequestCancellation();
        return GpuParticleStepStatus::Complete;
    }

    GpuParticleRenderState GpuParticleSimulator::RenderState() const noexcept {
        if (state_->owner != std::this_thread::get_id())
            return {};
        return {state_->receipt, state_->nextTick, state_->nextTick + 1, state_->lifetimeToken,
                state_->active && !state_->stopped && !state_->shutdown};
    }
}  // namespace Horo::Vfx
