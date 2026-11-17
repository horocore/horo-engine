#pragma once
/** @file VfxWorld.h
 * @brief Scene-owned prepared live CPU effects and their immutable extraction channel.
 */
#include "Horo/Math/WorldCoordinate64.h"
#include "Horo/Runtime/Render/RenderWorldSnapshot.h"
#include "Horo/Vfx/CpuParticleBuffer.h"
#include "Horo/Vfx/GpuParticleSimulator.h"
#include "Horo/Vfx/VfxSpawnPreparation.h"

#include <thread>
#include <vector>

namespace Horo::Vfx {
    namespace Detail {
        struct VfxWorldSpawnState;
        struct VfxSpawnAsset;
        struct VfxSpawnInstance;
    }  // namespace Detail

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

    /** @brief Cold ownership binding. Prepare moves the simulator only after all world admission and allocation succeeds.
     * @details Only the actual supported additive, unsorted billboard direct-draw recipe is admitted.
     */
    struct VfxGpuEmitterPreparation final {
        EffectSystemId effect;
        EmitterId emitter;
        std::unique_ptr<GpuParticleSimulator> simulator;
        Render::VfxParticlePass blend{Render::VfxParticlePass::Additive};
    };

    /** @brief Allocation-free owner-safe-phase processing evidence. */
    struct VfxGpuUpdateReport final {
        GpuParticleStepStatus status{GpuParticleStepStatus::Complete};
        std::uint32_t processed{}, pending{}, rejected{};
    };

    /** @brief Aggregate host-ledger reservation, shared across all effects in one scene.
     * @details Draw credits reserve the worst permitted depth/color/shadow multiplicity per batch.
     * The host subtracts ordinary scene draws before assigning this slice. No independent per-effect allowance exists.
     */
    struct VfxExtractionBudget final {
        std::uint32_t maximumBatches{};
        std::uint32_t maximumInstances{};
        std::uint32_t maximumDrawCalls{};
        std::uint32_t frameSlots{};
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
        std::uint32_t admittedBatches{};
        std::uint32_t admittedInstances{};
        std::uint32_t reservedDrawCalls{};
        std::uint32_t omittedBatches{};
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
        [[nodiscard]] static Result<VfxRenderExtractor> Prepare(VfxExtractionBudget budget, std::uint32_t gpuBatchCapacity = 0);
        /** @brief Packs the current live prefix on the scene owner without allocation.
         * @param world Scene owner; borrowed only during copying. @param frame Increasing one-based frame ordinal.
         * @param origin Captured render origin, borrowed only for this synchronous copy. @param originRevision Non-zero captured revision.
         * @return Immutable lease and exact admission/overrun status. Retained frames are never overwritten.
         */
        [[nodiscard]] VfxExtractionOutcome Extract(const VfxWorld &world, std::uint64_t frame, const Math::WorldCoordinate64 &origin,
                                                   std::uint64_t originRevision) noexcept;

        [[nodiscard]] bool AppendGpuEmitter(EffectSystemId, EmitterId, const GpuParticleSimulator &, std::uint32_t,
                                            Render::RenderWorldFrameData &, const Math::WorldCoordinate64 &,
                                            VfxExtractionReport &) const noexcept;
        [[nodiscard]] bool AppendGpuEmitters(const VfxWorld &, Render::RenderWorldFrameData &, const Math::WorldCoordinate64 &,
                                             VfxExtractionReport &) const noexcept;

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
        [[nodiscard]] bool AppendSpawnEmitter(Detail::VfxSpawnAsset &, Detail::VfxSpawnInstance &, Render::RenderWorldFrameData &,
                                              const Math::WorldCoordinate64 &, VfxExtractionReport &) const noexcept;
        [[nodiscard]] static bool CopySpawnParticles(Detail::VfxSpawnInstance &, const CpuParticleExtractView &,
                                                     Render::RenderWorldFrameData &, const Math::WorldCoordinate64 &) noexcept;

    private:
        friend class VfxWorld;
        VfxExtractionBudget budget_{};
        std::vector<std::shared_ptr<Render::RenderWorldFrameData>> slots_;
        std::thread::id owner_{};
        std::uint64_t lastFrame_{};
    };

    /** @brief Scene-owner-thread mixed CPU/GPU logical effect storage prepared by the host and attached to RuntimeScene.
     * @details Prepared bindings own CPU buffers. Play activates a complete prepared effect; Stop clears
     * its simulation and advances generation before reuse. Immutable snapshots own copies, so stopped effects
     * and destroyed scenes cannot invalidate readers. GPU logical owners share this world; all native resources remain renderer-owned.
     */
    class VfxWorld final {
    public:
        static constexpr std::size_t MaximumGpuAcknowledgements = 64;
        VfxWorld(const VfxWorld &) = delete;
        VfxWorld &operator=(const VfxWorld &) = delete;
        /** @brief Transfers prepared ownership; owner-thread affinity is preserved. */
        VfxWorld(VfxWorld &&) noexcept;
        /** @brief Replaces quiescent prepared ownership. @return This world. */
        VfxWorld &operator=(VfxWorld &&) noexcept;
        /** @brief Destroys Scene-owned prepared state after all external users retire. */
        ~VfxWorld();
        /** @brief Prepares fixed live emitters and aggregate frame capacity before scene activation.
         * @param scene Exact scene incarnation. @param emitters Immutable cooked bindings, grouped by effect.
         * @param budget Host-reserved aggregate extraction slice. @return Owned world or typed preparation failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<VfxWorld>> Prepare(VfxIdentityScope scene,
                                                                       std::span<const VfxEmitterPreparation> emitters,
                                                                       VfxExtractionBudget budget);
        /** @brief Cold mixed preparation; failure leaves every caller-owned GPU simulator intact.
         * @param scene Scene incarnation. @param cpu Existing CPU bindings. @param gpu Mutable ownership bindings, moved only on success.
         * @param budget One aggregate CPU/GPU extraction slice. @return Prepared canonical scene owner or typed failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<VfxWorld>> Prepare(VfxIdentityScope scene, std::span<const VfxEmitterPreparation> cpu,
                                                                       std::span<VfxGpuEmitterPreparation> gpu, VfxExtractionBudget budget);
        /** @brief Applies only frontend-issued acknowledgements at the owner safe phase, before preparing/extracting new work. */
        [[nodiscard]] VfxGpuUpdateReport ApplyGpuAcknowledgements(std::span<const Render::GpuVfxStepAcknowledgement>) noexcept;
        /** @brief Prepares each live GPU unit's exact next tick; retained unacknowledged candidates are copied unchanged. */
        [[nodiscard]] VfxGpuUpdateReport PrepareGpuSteps(float deltaSeconds, const CancellationToken &cancellation = {}) noexcept;
        /** @brief Replaces every stopped GPU unit of one effect with freshly admitted matching-generation owners atomically.
         * @details Capacities must match the original reservation; failure preserves all replacement owners.
         */
        [[nodiscard]] Result<void> ReprepareGpuEffect(EffectSystemId effect, std::span<VfxGpuEmitterPreparation> replacements);
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

        /** @brief Atomically creates the canonical spawn World with fully prepared CPU recipes and immutable extraction storage.
         * @param scene Exact published-candidate Scene incarnation.
         * @param assets Cold validated host-admitted recipes.
         * @param budget One aggregate spawn reservation.
         * @param extraction Separate host-reserved frame storage; the spawn slice must fit its work capacities.
         * @return Complete owned World or typed failure without publishing partial ownership.
         */
        [[nodiscard]] static Result<std::unique_ptr<VfxWorld>> PrepareSpawns(VfxIdentityScope scene,
                                                                             std::span<const VfxSpawnAssetPreparation> assets,
                                                                             VfxSpawnBudget budget, VfxExtractionBudget extraction);
        /** @brief Admits one exact host-issued owner incarnation before it may submit requests.
         * @param owner Explicit application owner scope. @param generation Nonzero never-reused incarnation.
         * @return Accepted, duplicate/stale/capacity/thread/lifecycle status; no allocation occurs.
         */
        [[nodiscard]] VfxSpawnStatus AdmitSpawnOwner(VfxIdentityScope owner, std::uint64_t generation) noexcept;
        /** @brief Validates and copies one complete authored fan-out atomically without allocation.
         * @param requests Canonical layer order for one occurrence, at most the hard layer ceiling.
         * @return Correlated acceptance or typed zero-mutation rejection. No completion runs inline.
         */
        [[nodiscard]] VfxSpawnOutcome TryEnqueueSpawns(std::span<const VfxSpawnRequest> requests) noexcept;
        /** @brief Freezes/drains the accepted tick prefix and advances real prepared CPU kernels on the Scene owner.
         * @param tick Strictly increasing owner tick. @param deltaSeconds Finite prepared-clock delta.
         * @return Bounded processing counts; correlated results consume pre-reserved result credits.
         */
        [[nodiscard]] VfxSpawnUpdate UpdateSpawns(std::uint64_t tick, float deltaSeconds) noexcept;
        /** @brief Revokes one exact owner and retires its queued/live work before service destruction.
         * @param owner Admitted owner scope. @param generation Exact incarnation; a stale call cannot revoke a replacement.
         * @return Accepted or a typed affinity/lifecycle/stale-owner denial.
         */
        [[nodiscard]] VfxSpawnStatus CancelSpawnOwner(VfxIdentityScope owner, std::uint64_t generation) noexcept;
        /** @brief Borrows immutable terminal result storage until ClearSpawnResults or the next owner mutation.
         * @return Bounded result prefix, empty on wrong thread or after shutdown.
         */
        [[nodiscard]] std::span<const VfxSpawnOutcome> SpawnResults() const noexcept;
        /** @brief Acknowledges terminal evidence without clearing deduplication watermarks. */
        void ClearSpawnResults() noexcept;
        /** @brief Copies cold prepared asset/schema facts without exposing any simulation state.
         * @param asset Exact prepared identity. @param output Filled only on success.
         * @return Accepted or typed missing/thread/lifecycle outcome.
         */
        [[nodiscard]] VfxSpawnStatus InspectSpawnAsset(Assets::AssetId asset, VfxSpawnAssetInfo &output) const noexcept;
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

        struct GpuEmitter final {
            EffectSystemId effect;
            EmitterId emitter;
            std::unique_ptr<GpuParticleSimulator> simulator;
            std::uint32_t capacity{};
            bool live{}, retired{};
        };

        [[nodiscard]] static Result<void> ValidateCpuBindings(VfxIdentityScope, std::span<const VfxEmitterPreparation>);
        [[nodiscard]] Result<void> PrepareCpuEmitters(std::span<const VfxEmitterPreparation>);
        [[nodiscard]] Result<void> CheckCpuPlay(EffectSystemId, bool &) const;
        [[nodiscard]] bool HasLiveEffect(EffectSystemId) const noexcept;
        [[nodiscard]] bool CompatibleGpuReplacements(EffectSystemId, std::span<VfxGpuEmitterPreparation>) const noexcept;
        [[nodiscard]] Result<void> ValidateGpuReplacements(EffectSystemId, std::span<VfxGpuEmitterPreparation>) const;
        [[nodiscard]] bool CanPlayGpu(EffectSystemId, bool &) const noexcept;
        void SetGpuLive(EffectSystemId) noexcept;
        [[nodiscard]] bool HasLiveGpu(EffectSystemId) const noexcept;
        void StopGpu(EffectSystemId, EffectSystemId, bool) noexcept;
        void ShutdownGpu() noexcept;
        [[nodiscard]] bool AppendCpuEmitters(Render::RenderWorldFrameData &, const Math::WorldCoordinate64 &,
                                             VfxExtractionReport &) const noexcept;
        [[nodiscard]] bool AppendSpawnEmitters(Render::RenderWorldFrameData &, const Math::WorldCoordinate64 &,
                                               VfxExtractionReport &) const noexcept;
        /** @brief Prepares only the unpublished atomic factory candidate. */
        [[nodiscard]] Result<void> PrepareSpawnPalette(std::span<const VfxSpawnAssetPreparation>, VfxSpawnBudget);
        void ShutdownSpawns() noexcept;
        [[nodiscard]] VfxSpawnStatus SpawnAdmissionStatus() const noexcept;

        /** @brief Takes already materialized scene/frame storage. */
        explicit VfxWorld(VfxIdentityScope scene, VfxRenderExtractor extractor);
        VfxIdentityScope scene_;
        VfxRenderExtractor extractor_;
        std::vector<Emitter> emitters_;
        std::vector<GpuEmitter> gpuEmitters_;
        std::unique_ptr<Detail::VfxWorldSpawnState> spawn_;
        std::thread::id owner_{};
        bool shutDown_{};
    };
}  // namespace Horo::Vfx
