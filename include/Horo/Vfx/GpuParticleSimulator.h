#pragma once
/** @file GpuParticleSimulator.h @brief Scene-owned logical GPU step producer; all dispatch/resources/readback remain renderer-owned. */
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Runtime/Render/VfxGpuSimulation.h"
#include "Horo/Vfx/CpuParticleBuffer.h"
#include "Horo/Vfx/ParticleSystemDescriptor.h"

#include <memory>

namespace Horo::Vfx {
    namespace Detail {
        struct GpuParticleSimulatorState;
    }
    /** @brief Explicit first-slice initialization protocol; never an implicit claim of continuous spawning support. */
    enum class GpuParticleInitialization : std::uint8_t {
        PreparedParticles
    };

    /** @brief Required features captured from the complete cooked unit, including requirements outside the simple source carrier.
     * @details The first PreparedParticles implementation rejects every non-None bit, including unknown bits. These declarations
     * cannot authorize a fallback or erase a required source feature.
     */
    enum class GpuParticleRequiredFeature : std::uint32_t {
        None = 0,
        ContinuousSpawning = 1u << 0,
        PerStepBursts = 1u << 1,
        Collision = 1u << 2,
        OverLife = 1u << 3,
        PayloadPrograms = 1u << 4,
        AuthoritativeOutput = 1u << 5,
        SortedTransparency = 1u << 6,
        Mesh = 1u << 7,
        Ribbon = 1u << 8
    };

    /** @brief Borrowed cold seed values in canonical CPU semantic streams and strictly increasing stable particle identity order. */
    struct GpuParticleInitialState final {
        CpuParticleSoAConstView attributes;
        std::span<const ParticleSimulationId> identities;
    };

    /** @brief Complete explicit cold unit admission; arrays are copied before Create returns.
     * @details PreparedParticles supports ordered forces, semiimplicit integration, rotation/age and finite lifetime kill.
     * Continuous spawning, per-step bursts, collision, over-life/payload modules, gameplay output and unsupported sorting/topologies
     * must fail admission, not disappear. This is an intermediate unit protocol, not final full GPU emitter completion.
     */
    struct GpuParticleSimulatorCreateInfo final {
        VfxIdentityScope scene;
        EffectSystemId activation;
        ParticleBufferId buffer;
        std::uint64_t effectSeed{};
        Math::WorldCoordinate64 simulationOrigin;
        VfxResolution resolution;
        GpuParticleInitialization initialization{GpuParticleInitialization::PreparedParticles};
        GpuParticleRequiredFeature requiredFeatures{GpuParticleRequiredFeature::None};
        GpuParticleInitialState initial;
        std::span<const CpuParticleForceModule> forces;
        Render::GpuParticleLayoutLimits layoutLimits;
        std::size_t maximumRetainedBytes{}; /**< Actual host-reserved aggregate GPU rings/forces/observation ledger slice. */
        std::uint32_t maximumPendingSteps{};
        std::uint32_t maximumSteps{}; /**< Explicit finite work envelope; at most one million, enforced without catch-up. */
        float maximumDeltaSeconds{};
    };
    /** @brief Allocation-free normal-path/expected-failure status; detailed Error objects belong to cold/presentation boundaries. */
    enum class GpuParticleStepStatus : std::uint8_t {
        Complete,
        NotActive,
        PendingSubmission,
        CapacityExceeded,
        StepLimit,
        InvalidStep,
        StaleAcknowledgement,
        Cancelled,
        SubmissionUncertain,
        DeviceLost,
        ThreadViolation,
        ShutDown
    };

    /** @brief Work exists only for Complete; failures never overwrite an already prepared or accepted step. */
    struct GpuParticleStepOutcome final {
        GpuParticleStepStatus status{GpuParticleStepStatus::NotActive};
        Render::GpuVfxSimulationWork work;

        [[nodiscard]] bool HasValue() const noexcept {
            return status == GpuParticleStepStatus::Complete;
        }
    };

    /** @brief Owner-thread value capture; generation one is the initialized state, independent of GPU completion. */
    struct GpuParticleRenderState final {
        Render::VfxGpuPreparationReceipt preparation;
        std::uint64_t nextTick{};
        std::uint64_t generation{};
        CancellationToken lifetimeCancellation;
        bool active{};
    };

    /** @brief Scene-owner logical state with preallocated pending records; never owns a graphics context or encodes/maps GPU work. */
    class GpuParticleSimulator final {
    public:
        GpuParticleSimulator(const GpuParticleSimulator &) = delete;
        GpuParticleSimulator &operator=(const GpuParticleSimulator &) = delete;
        ~GpuParticleSimulator();
        /** @brief Validates supported authored stages and proves a bounded finite arithmetic envelope before activation.
         * @param descriptor Validated source descriptor; unsupported authored requirements fail explicitly.
         * @param info Captured identities/resolution/seeds/forces/budgets; observation policy belongs to descriptor.Data().gpuReadback.
         * @return Cold prepared owner or typed failure, with no native allocation or dispatch.
         */
        [[nodiscard]] static Result<std::unique_ptr<GpuParticleSimulator>> Create(const ParticleSystemDescriptor &descriptor,
                                                                                  const GpuParticleSimulatorCreateInfo &info);
        /** @brief Borrows immutable cold initialization for the renderer's synchronous preparation copy. */
        [[nodiscard]] const Render::VfxGpuSimulationPreparation &Preparation() const noexcept;
        /** @brief Activates only the matching non-forgeable actual frontend preparation receipt; no callbacks occur. */
        [[nodiscard]] Result<void> Activate(const Render::VfxGpuPreparationReceipt &receipt);
        /** @brief Prepares exact-next-tick immutable work; no delta accumulation, allocation, dispatch or same-frame wait.
         * @param tick Next logical tick starting at zero, advanced only by actual accepted-submission acknowledgement.
         * @param deltaSeconds Finite nonnegative delta within the admitted envelope.
         * @param cancellation Captured logical cancellation; submitted resources still retire on native completion.
         * @return Static status plus exact candidate work, with one unacknowledged candidate and bounded submitted records.
         */
        [[nodiscard]] GpuParticleStepOutcome PrepareStep(std::uint64_t tick, float deltaSeconds,
                                                         const CancellationToken &cancellation = {}) noexcept;
        /** @brief Applies frontend-issued receipts at the scene owner safe point; never called reentrantly from native submission.
         * @param acknowledgement Actual accepted/rejected/uncertain/completed fact drained from the frontend queue.
         * @return Static status; accepted native submission advances the logical tick once, completion only retires capacity.
         */
        [[nodiscard]] GpuParticleStepStatus ApplyAcknowledgement(const Render::GpuVfxStepAcknowledgement &acknowledgement) noexcept;
        /** @brief Borrows at most one immutable candidate for copying by the existing scene extraction owner. */
        [[nodiscard]] std::span<const Render::GpuVfxSimulationWork> PendingWork() const noexcept;
        /** @brief Borrows the cold-created unit revocation token for snapshot/draw capture, including before the first step.
         * @return Token valid for synchronous copying; Stop/Shutdown request it without releasing any native resource.
         */
        [[nodiscard]] const CancellationToken &LifetimeCancellation() const noexcept;
        /** @brief Captures the issued preparation, last accepted generation and exact next tick without mutating pending work.
         * @return Empty inactive value on a foreign thread; retained values contain no live simulator pointer.
         */
        [[nodiscard]] GpuParticleRenderState RenderState() const noexcept;
        /** @brief Revokes logical publication/admission immediately; no native lease or GPU budget is released here. */
        [[nodiscard]] GpuParticleStepStatus Stop() noexcept;
        /** @brief Closes logical ownership idempotently; retained renderer work/resources obey independent native retirement. */
        [[nodiscard]] GpuParticleStepStatus Shutdown() noexcept;

    private:
        explicit GpuParticleSimulator(std::unique_ptr<Detail::GpuParticleSimulatorState> state) noexcept;
        std::unique_ptr<Detail::GpuParticleSimulatorState> state_;
    };
}  // namespace Horo::Vfx
