#pragma once
/** @file VfxWorld.h
 * @brief Scene-owned prepared live CPU effects and their immutable extraction channel.
 */
#include "Horo/Math/WorldCoordinate64.h"
#include "Horo/Runtime/Render/RenderWorldSnapshot.h"
#include "Horo/Vfx/CpuParticleBuffer.h"

#include <thread>
#include <vector>

namespace Horo::Vfx {
    /** @brief Fixed cooked emitter binding realized before scene activation. */
    struct VfxEmitterPreparation final {
        EffectSystemId effect{};
        EmitterId emitter{};
        CpuParticleBufferCreateInfo particles{};
        Math::WorldCoordinate64 simulationOrigin{}; /**< Canonical origin of local CPU particle positions. */
        Render::MaterialRuntimeId material{};
        Render::MaterialAlphaMode materialBlend{Render::MaterialAlphaMode::Additive};
        Render::RenderMeshHandle mesh{};
        Render::VfxParticlePass blend{Render::VfxParticlePass::Additive};
        Render::VfxPrimitiveTopology topology{Render::VfxPrimitiveTopology::CameraFacingBillboard};
        bool deferredCompatible{};
        bool shadowCaster{};
    };

    /** @brief Aggregate host-ledger reservation, shared across all effects in one scene.
     * @details Draw credits reserve the worst permitted depth/color/shadow multiplicity per batch.
     * The host subtracts ordinary scene draws before assigning this slice. No independent per-effect allowance exists.
     */
    struct VfxExtractionBudget final {
        std::uint32_t maximumBatches{}, maximumInstances{}, maximumDrawCalls{}, frameSlots{};
        std::size_t maximumBytes{}; /**< Includes all prepared immutable frame payload copies. */
    };
    /** @brief Typed frame-hot outcome; failure/overrun builds no heap-backed diagnostic. */
    enum class VfxExtractionStatus : std::uint8_t {
        Complete,
        BudgetExceeded,
        FramesRetained,
        InvalidFrame,
        ThreadViolation,
        InvalidParticleData,
        ShutDown
    };

    /** @brief Full frame admission/omission evidence. A batch is never partially packed. */
    struct VfxExtractionReport final {
        VfxExtractionStatus status{VfxExtractionStatus::Complete};
        std::uint32_t admittedBatches{}, admittedInstances{}, reservedDrawCalls{}, omittedBatches{};
        std::uint64_t omittedInstances{};
        Render::VfxBatchId firstOmitted{};
    };

    /** @brief Retained immutable values plus non-allocating overload report. */
    struct VfxExtractionOutcome final {
        Render::RenderWorldSnapshot snapshot;
        VfxExtractionReport report;
    };
    class VfxWorld;

    /** @brief One bounded immutable frame ring; never submits graph work or mutates simulation. */
    class VfxRenderExtractor final {
    public:
        VfxRenderExtractor(const VfxRenderExtractor &) = delete;
        VfxRenderExtractor &operator=(const VfxRenderExtractor &) = delete;
        /** @brief Transfers prepared ownership without changing thread affinity. */
        VfxRenderExtractor(VfxRenderExtractor &&) noexcept = default;
        /** @brief Transfers prepared ownership after consumers quiesce. @return This extractor. */
        VfxRenderExtractor &operator=(VfxRenderExtractor &&) noexcept = default;

    private:
        friend class VfxWorld;
        VfxRenderExtractor() = default;
        /** @brief Materializes the host-reserved immutable frame ring.
         * @param budget Finite scene-wide payload/draw bounds. @return Prepared ring or typed admission failure.
         */
        [[nodiscard]] static Result<VfxRenderExtractor> Prepare(VfxExtractionBudget budget);
        /** @brief Packs the current live prefix on the scene owner without allocation.
         * @param world Scene owner; borrowed only during copying. @param frame Increasing one-based frame ordinal.
         * @param origin Captured render origin, borrowed only for this synchronous copy. @param originRevision Non-zero captured revision.
         * @return Immutable lease and exact admission/overrun status. Retained frames are never overwritten.
         */
        [[nodiscard]] VfxExtractionOutcome Extract(const VfxWorld &world, std::uint64_t frame, const Math::WorldCoordinate64 &origin,
                                                   std::uint64_t originRevision) noexcept;

        /** @brief Copies validated particle values into admitted frame storage; never publishes a partial frame.
         * @param binding Prepared emitter coordinates. @param particles Exact live identity source.
         * @param source Borrowed packed values. @param data Preallocated candidate frame. @param origin Captured render origin, never
         * retained.
         * @return Whether every particle value and identity was valid.
         */
        [[nodiscard]] static bool CopyParticles(const VfxEmitterPreparation &binding, const CpuParticleBuffer &particles,
                                                const CpuParticleSoAConstView &source, Render::RenderWorldFrameData &data,
                                                const Math::WorldCoordinate64 &origin) noexcept;
        /** @brief Admits and copies one complete emitter or records its bounded omission.
         * @param binding Prepared semantics. @param particles Borrowed simulation owner. @param data Candidate frame storage.
         * @param origin Captured render origin, never retained. @param report Aggregate draw/instance admission and typed failure evidence.
         * @return False only for invalid particle data; budget omission returns true to continue later emitters.
         */
        [[nodiscard]] bool AppendEmitter(const VfxEmitterPreparation &binding, const CpuParticleBuffer &particles,
                                         Render::RenderWorldFrameData &data, const Math::WorldCoordinate64 &origin,
                                         VfxExtractionReport &report) const noexcept;

    private:
        friend class VfxWorld;
        VfxExtractionBudget budget_{};
        std::vector<std::shared_ptr<Render::RenderWorldFrameData>> slots_;
        std::thread::id owner_{};
        std::uint64_t lastFrame_{};
    };

    /** @brief Scene-owner-thread live effect storage prepared by the host and attached to RuntimeScene.
     * @details Prepared bindings own CPU buffers. Play activates a complete prepared effect; Stop clears
     * its simulation and advances generation before reuse. Immutable snapshots own copies, so stopped effects
     * and destroyed scenes cannot invalidate readers. GPU simulation and native resources are not owned here.
     */
    class VfxWorld final {
    public:
        VfxWorld(const VfxWorld &) = delete;
        VfxWorld &operator=(const VfxWorld &) = delete;
        VfxWorld(VfxWorld &&) noexcept = default;
        VfxWorld &operator=(VfxWorld &&) noexcept = default;
        /** @brief Prepares fixed live emitters and aggregate frame capacity before scene activation.
         * @param scene Exact scene incarnation. @param emitters Immutable cooked bindings, grouped by effect.
         * @param budget Host-reserved aggregate extraction slice. @return Owned world or typed preparation failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<VfxWorld>> Prepare(VfxIdentityScope scene,
                                                                       std::span<const VfxEmitterPreparation> emitters,
                                                                       VfxExtractionBudget budget);
        /** @brief Activates one entire prepared effect without allocation.
         * @param effect Exact current generation. @return Success or typed stale/thread/state failure.
         */
        [[nodiscard]] Result<void> Play(EffectSystemId effect);
        /** @brief Stops one effect and advances its identity; immutable frame copies remain valid.
         * @param effect Exact live generation. @return Next reusable identity or typed failure.
         */
        [[nodiscard]] Result<EffectSystemId> Stop(EffectSystemId effect);
        /** @brief Borrows live owner-thread particle state for the existing CPU simulator/spawn pipeline.
         * @param effect Exact live effect. @param emitter Exact prepared emitter. @return Owned buffer or typed rejection.
         * @pre Caller must not retain the pointer past Stop or world destruction, or transfer it across threads.
         */
        [[nodiscard]] Result<CpuParticleBuffer *> Particles(EffectSystemId effect, EmitterId emitter);
        /** @brief Captures all live effects into the sole immutable channel.
         * @param frame Increasing frame ordinal. @param origin Captured render origin. @param originRevision Captured revision.
         * @return Immutable values and typed bounds/backpressure evidence.
         */
        [[nodiscard]] VfxExtractionOutcome Extract(std::uint64_t frame, const Math::WorldCoordinate64 &origin,
                                                   std::uint64_t originRevision) noexcept;

        /** @brief Returns the exact scene incarnation. */
        [[nodiscard]] VfxIdentityScope Scene() const noexcept {
            return scene_;
        }

        /** @brief Closes admission and stops simulation; retained immutable snapshots survive.
         * @return Success or owner-thread failure. */
        [[nodiscard]] Result<void> Shutdown();

    private:
        friend class VfxRenderExtractor;

        struct Emitter final {
            VfxEmitterPreparation binding;
            CpuParticleBuffer particles;
            bool live{};
            bool retired{};
        };

        /** @brief Takes already materialized scene/frame storage. */
        explicit VfxWorld(VfxIdentityScope scene, VfxRenderExtractor extractor);
        VfxIdentityScope scene_;
        VfxRenderExtractor extractor_;
        std::vector<Emitter> emitters_;
        std::thread::id owner_{};
        bool shutDown_{};
    };
}  // namespace Horo::Vfx
