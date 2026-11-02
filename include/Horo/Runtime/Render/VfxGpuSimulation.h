#pragma once
/** @file VfxGpuSimulation.h
 * @brief Immutable GPU VFX work, renderer-issued preparation and native submission acknowledgement contracts.
 */
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Math/WorldCoordinate64.h"
#include "Horo/Runtime/Render/GpuParticleLayout.h"
#include "Horo/Runtime/Render/RenderResource.h"
#include "Horo/Runtime/Render/RenderSubmission.h"

#include <compare>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>

namespace Horo::Render {
    class RenderFrontend;

    namespace VfxGpuSimulationErrors {
        extern const ErrorCodeDescriptor InvalidPreparation;
        extern const ErrorCodeDescriptor ArithmeticUnsafe;
    }  // namespace VfxGpuSimulationErrors

    namespace Detail {
        struct VfxFrontendState;
    }

    /** @brief Exact scene and logical effect/emitter/buffer generations, independent of native storage. */
    struct VfxGpuSimulationIdentity final {
        std::uint64_t scene{};
        Vfx::ParticleRandomIdentity activation;
        Vfx::ParticleRandomIdentity emitter;
        Vfx::ParticleRandomIdentity buffer;
    };

    /** @brief Renderer-registry-issued prepared simulation identity; not a native buffer or command handle. */
    struct VfxGpuSimulationId final {
        RenderResourceOwnerId owner;
        std::uint32_t slot{std::numeric_limits<std::uint32_t>::max()}; /**< Zero-based preparation-pool slot; UINT32_MAX is invalid. */
        std::uint32_t generation{};

        /** @brief Checks the prepared-pool representation, not native residency. @return True for nonzero owner/generation and a real slot.
         */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return owner.IsValid() && slot != std::numeric_limits<std::uint32_t>::max() && generation != 0;
        }

        constexpr auto operator<=>(const VfxGpuSimulationId &) const noexcept = default;
    };

    /** @brief Authorized asynchronous observation purposes; no gameplay-authoritative purpose exists. */
    enum class VfxGpuReadbackPurpose : std::uint8_t {
        AuthoringPreview,
        DiagnosticsCapture,
        CosmeticObservation
    };
    /** @brief Whether missing observation support may degrade this visual unit. */
    enum class VfxGpuReadbackRequirement : std::uint8_t {
        Optional,
        RequiredForVisualEffect
    };
    /** @brief Explicit observation capability outcome captured before activation. */
    enum class VfxGpuReadbackAdmission : std::uint8_t {
        Disabled,
        Available,
        OptionalUnavailable
    };

    /** @brief Cooked opt-in normalized particle observations; disabled by default.
     * @details Schema one observes stable IDs and declared scalar particle values, never shader padding or native addresses.
     * Cadence is per committed logical step, not view/frame; pending storage is admitted before activation.
     */
    struct VfxGpuReadbackDescriptor final {
        bool enabled{};
        std::uint32_t schemaVersion{1};
        VfxGpuReadbackPurpose purpose{VfxGpuReadbackPurpose::DiagnosticsCapture};
        VfxGpuReadbackRequirement requirement{VfxGpuReadbackRequirement::Optional};
        std::uint32_t cadenceSteps{1};
        std::uint32_t maximumRecords{};
        std::uint32_t maximumBytesPerResult{};
        std::uint32_t maximumPendingResults{};
    };

    /** @brief Cold-proved arithmetic envelope for the admitted PreparedParticles unit.
     * @details Bounds include every admitted step up to maximumSteps, even when float age stagnates for tiny deltas.
     * Exceeding this explicit step allowance rejects the next step; it never clamps shader results or invents successful publication.
     * Doubles are CPU admission metadata only and are explicitly lowered; GPU arithmetic stays float32/uint32.
     */
    struct VfxGpuArithmeticEnvelope final {
        std::uint32_t maximumSteps{};
        float maximumDeltaSeconds{};
        double maximumPositionMagnitude{};
        double maximumVelocityMagnitude{};
        double maximumAccelerationMagnitude{};
        double maximumRotationMagnitude{};
        double maximumSizeMagnitude{};
    };

    /** @brief Span-free immutable admission metadata retained independently of initialization source buffers. */
    struct VfxGpuSimulationDescriptor final {
        VfxGpuSimulationIdentity identity;
        GpuParticleLayout layout;
        Vfx::ParticleRandomKey random;
        Math::WorldCoordinate64 simulationOrigin;
        std::uint32_t initialParticleCount{}; /**< Occupied, strictly stable-ID-ordered seed prefix; remaining lanes are canonical zero. */
        std::uint32_t forceCount{};
        std::uint32_t maximumPendingSteps{};
        std::size_t maximumRetainedBytes{}; /**< Host-reserved shared VFX/renderer ledger slice, not an additional global budget. */
        std::uint64_t capabilityRevision{};
        std::uint64_t policyRevision{};
        Sha256Digest initializationDigest;
        VfxGpuArithmeticEnvelope arithmetic;
        VfxGpuReadbackDescriptor readback;
    };

    /** @brief Returns the single canonical physical state inventory size for a valid pending-step bound.
     * @param descriptor Cold metadata with maximumPendingSteps in [1,64].
     * @return N+2 (one immutable host-upload seed plus N+1 writable device-local states), or zero for an invalid bound.
     * @details Seed inventory index zero is never a GPU destination, including on unified-memory backends.
     */
    [[nodiscard]] constexpr std::uint32_t VfxGpuStateBufferCount(const VfxGpuSimulationDescriptor &descriptor) noexcept {
        return descriptor.maximumPendingSteps > 0 && descriptor.maximumPendingSteps <= 64 ? descriptor.maximumPendingSteps + 2 : 0;
    }

    /** @brief Cold initialization copied by frontend preparation; borrowed spans never survive that synchronous call. */
    struct VfxGpuSimulationPreparation final {
        VfxGpuSimulationDescriptor descriptor;
        std::span<const std::uint32_t> initialWords;
        std::span<const std::uint32_t> forceWords;
    };

    /** @brief Non-forgeable metadata receipt issued only after real frontend/native resource and kernel admission.
     * @details This owns no native resources. The renderer retains preparation and GPU journals independently of the scene owner.
     */
    class VfxGpuPreparationReceipt final {
    public:
        VfxGpuPreparationReceipt() = default;

        /** @brief Reads immutable span-free source metadata. @return Exact admitted descriptor. */
        [[nodiscard]] const VfxGpuSimulationDescriptor &Descriptor() const noexcept {
            return descriptor_;
        }

        /** @brief Checks existing frontend-incarnation revocation without retaining frontend/native authority. @return True while issuer
         * lives. */
        [[nodiscard]] bool IsLive() const noexcept {
            return !authority_.expired();
        }

        /** @brief Reads the exact registry-issued preparation generation. @return Issued identity or invalid default. */
        [[nodiscard]] VfxGpuSimulationId Id() const noexcept {
            return id_;
        }

        /** @brief Reads the captured native device incarnation. @return Nonzero issued incarnation. */
        [[nodiscard]] std::uint64_t DeviceIncarnation() const noexcept {
            return deviceIncarnation_;
        }

        /** @brief Reads explicit observation admission. @return Disabled, available or optional-unavailable. */
        [[nodiscard]] VfxGpuReadbackAdmission Readback() const noexcept {
            return readback_;
        }

    private:
        friend class RenderFrontend;
        VfxGpuSimulationDescriptor descriptor_;
        std::weak_ptr<const Detail::VfxFrontendState> authority_;
        VfxGpuSimulationId id_;
        std::uint64_t deviceIncarnation_{};
        VfxGpuReadbackAdmission readback_{VfxGpuReadbackAdmission::Disabled};
    };

    /** @brief Exactly one candidate step in one issued renderer incarnation. */
    struct VfxGpuStepIdentity final {
        VfxGpuSimulationId simulation;
        std::uint64_t deviceIncarnation{};
        std::uint64_t tick{}; /**< Exact next logical tick, starting at zero. */
        std::uint64_t sourceGeneration{};
        std::uint64_t targetGeneration{};
        constexpr auto operator<=>(const VfxGpuStepIdentity &) const noexcept = default;
    };

    /** @brief Immutable work copied into the existing RenderWorldSnapshot; contains no backend or live VFX pointer. */
    struct GpuVfxSimulationWork final {
        VfxGpuSimulationIdentity identity;
        VfxGpuStepIdentity step;
        float deltaSeconds{};
        bool observe{};
        CancellationToken callerCancellation;
        CancellationToken lifetimeCancellation;
    };

    /** @brief Native submission facts, deliberately separate from graph encoding and presentation outcome. */
    enum class VfxGpuSubmissionState : std::uint8_t {
        Accepted,
        Completed,
        RejectedUnsubmitted,
        CancelledUnsubmitted,
        SubmissionUncertain,
        DeviceLost
    };

    /** @brief Backend journal output drained on the render owner, only from actual native commit/queue submission authority. */
    struct NativeGpuVfxStepReceipt final {
        VfxGpuStepIdentity step;
        VfxGpuSubmissionState state{VfxGpuSubmissionState::RejectedUnsubmitted};
        RenderTimelinePoint submission;
    };

    /** @brief Frontend-validated owner-drained acknowledgement; default construction conveys no issued authority. */
    class GpuVfxStepAcknowledgement final {
    public:
        GpuVfxStepAcknowledgement() = default;

        /** @brief Checks the existing frontend-incarnation authority marker. @return False for defaults or revoked issuers. */
        [[nodiscard]] bool IsLive() const noexcept {
            return !authority_.expired();
        }

        /** @brief Reads the exact source/candidate step identity. @return Immutable acknowledged step. */
        [[nodiscard]] const VfxGpuStepIdentity &Step() const noexcept {
            return step_;
        }

        /** @brief Reads the actual native submission boundary outcome. @return Explicit submitted/terminal state. */
        [[nodiscard]] VfxGpuSubmissionState State() const noexcept {
            return state_;
        }

        /** @brief Reads frontend-validated issued timeline evidence. @return Actual accepted native point, never graph-encoding evidence.
         */
        [[nodiscard]] RenderTimelinePoint Submission() const noexcept {
            return submission_;
        }

    private:
        friend class RenderFrontend;
        VfxGpuStepIdentity step_;
        VfxGpuSubmissionState state_{VfxGpuSubmissionState::RejectedUnsubmitted};
        RenderTimelinePoint submission_;
        std::weak_ptr<const Detail::VfxFrontendState> authority_;
    };

    /** @brief Recomputes a conservative finite arithmetic envelope from actual initial/force words and explicit step bounds.
     * @param preparation Complete cold source; only arithmetic.maximumSteps/maximumDeltaSeconds are accepted as input limits.
     * @return Derived safe bounds or a typed rejection before native activation; declared derived magnitudes are never trusted.
     */
    [[nodiscard]] Result<VfxGpuArithmeticEnvelope> ComputeVfxGpuArithmeticEnvelope(const VfxGpuSimulationPreparation &preparation);
    /** @brief Hashes both exact initialization/force word streams plus every scalar semantic descriptor/key/schema field.
     * @param preparation Cold complete source; initializationDigest itself is excluded, and host padding is never hashed.
     * @return Canonical little-endian word/metadata digest, without allocating or admitting resources.
     */
    [[nodiscard]] Sha256Digest ComputeVfxGpuInitializationDigest(const VfxGpuSimulationPreparation &preparation) noexcept;
    /** @brief Checks one exact-next-tick candidate against immutable semantic/range/cadence metadata, without native authority.
     * @param descriptor Cold validated metadata. @param work Candidate with sourceGeneration=tick+1 and targetGeneration=source+1.
     * @return False for malformed/stale identity, unsafe delta/tick/generation or invalid observation intent. Optional observations may be
     * omitted; required observations are present on every one-based cadence step. No error is allocated and no registry is inspected.
     */
    [[nodiscard]] bool ValidateVfxGpuSimulationWork(const VfxGpuSimulationDescriptor &descriptor,
                                                    const GpuVfxSimulationWork &work) noexcept;
    /** @brief Compares complete logical generations without relying on object representation or native IDs. */
    /** @brief Compares every immutable descriptor field exactly; the digest alone is never metadata identity. */
    [[nodiscard]] bool SameVfxGpuDescriptor(const VfxGpuSimulationDescriptor &, const VfxGpuSimulationDescriptor &) noexcept;
    [[nodiscard]] bool SameVfxGpuIdentity(const VfxGpuSimulationIdentity &left, const VfxGpuSimulationIdentity &right) noexcept;
    /** @brief Validates cold initialization, arithmetic and opted-in observation bounds before any native resource mutation. */
    [[nodiscard]] Result<void> ValidateVfxGpuSimulationPreparation(const VfxGpuSimulationPreparation &preparation);
}  // namespace Horo::Render
