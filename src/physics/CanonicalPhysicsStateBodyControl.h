#pragma once

#include "CanonicalPhysicsStateFpControl.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsStateConstructionStorage.h"

#include <Jolt/Jolt.h>

// Jolt subsidiary headers require its root definitions first.
#include <Jolt/Core/FPException.h>
#include <optional>

namespace Horo::Physics::Detail {
    /** @brief Reads actual controls without repairing caller arithmetic state. Unknown provenance remains unknown. */
    inline std::optional<PhysicsStateFpControl> ReadCanonicalBodyControl() {
        auto actual = ReadCanonicalPhysicsStateFpControl();
        return actual.HasValue() ? std::optional{actual.Value()} : std::nullopt;
    }

    /** @brief A complete synchronous construction boundary must preserve the actual controls used by native arithmetic. */
    inline std::optional<PhysicsStateFpControl> CompleteCanonicalBodyControl(const std::optional<PhysicsStateFpControl> &before,
                                                                             const std::optional<PhysicsStateFpControl> &after) noexcept {
        if (!before || !after || *before != *after)
            return std::nullopt;
        return before;
    }

    /** @brief Original constructor evidence and all subsequent mutation arithmetic must retain the same actual profile. */
    inline bool CanonicalBodyProvenanceKnown(const std::optional<PhysicsStateFpControl> &original,
                                             const std::optional<PhysicsStateFpControl> &current) noexcept {
        return original && current && *original == *current;
    }

    /** @brief Exact pinned EigenValueSymmetric invalid-exception subprofile, if that branch is selected.
     * Derived from the actual compiled FPExceptionDisableInvalid template; does not modify processor controls or claim branch travel.
     * Its guard source and JPH feature configuration are bound by SolverInputs. Unknown native encodings remain unsupported.
     */
    inline std::optional<PhysicsStateFpControl> CanonicalBodyMassSubprofile(
        const std::optional<PhysicsStateFpControl> &constructor) noexcept {
        if (!constructor)
            return std::nullopt;
        auto subprofile = *constructor;
#if defined(JPH_FLOATING_POINT_EXCEPTIONS_ENABLED)
#if defined(JPH_USE_SSE)
        if (subprofile.architecture != PhysicsStateFpArchitecture::X86MxcsrX87)
            return std::nullopt;
#if defined(JPH_COMPILER_MSVC)
        // The current reader uses _controlfp_s's combined x87/SSE abstract word. A changed SSE mask may also
        // change _EM_AMBIGUOUS in that word; absent raw-x87 evidence this nondefault profile is not admitted.
        if ((subprofile.value & static_cast<std::uint64_t>(_MM_MASK_INVALID)) == 0)
            return std::nullopt;
#endif
        subprofile.value |= static_cast<std::uint64_t>(_MM_MASK_INVALID);
#elif defined(JPH_CPU_ARM) && defined(JPH_USE_NEON) && !defined(JPH_COMPILER_MSVC)
        if (subprofile.architecture != PhysicsStateFpArchitecture::ArmFpcr)
            return std::nullopt;
        subprofile.value &= ~JPH::FP_IOE;
#else
        return std::nullopt;
#endif
#endif
        return subprofile;
    }

    /** @brief Required tracking rejects before settings/math/native effects; ordinary Optional behavior remains available. */
    inline Result<void> AdmitCanonicalBodyControl(const StateDetail::ConstructionRecipePool *pool,
                                                  const std::optional<PhysicsStateFpControl> &control) {
        if (!pool || pool->Tracking() != PhysicsStateConstructionTracking::Required)
            return Result<void>::Success();
        if (!control)
            return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        if (*control != pool->Profile())
            return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
        if (!CanonicalBodyMassSubprofile(control))
            return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
        return Result<void>::Success();
    }

    /** @brief Complete retained witness must match the exact actual compiled mask-only guard profile. */
    inline bool CanonicalBodyConstructionKnown(const std::optional<PhysicsStateFpControl> &original,
                                               const std::optional<PhysicsStateFpControl> &current,
                                               const std::optional<PhysicsStateFpControl> &mass) noexcept {
        const auto expected = CanonicalBodyMassSubprofile(original);
        return CanonicalBodyProvenanceKnown(original, current) && expected && mass && *expected == *mass;
    }

    /** @brief Once arithmetic provenance is unknown, later compatible mutations cannot manufacture original evidence. */
    inline void FoldCanonicalBodyControl(std::optional<PhysicsStateFpControl> &retained, const std::optional<PhysicsStateFpControl> &before,
                                         const std::optional<PhysicsStateFpControl> &after) noexcept {
        const auto boundary = CompleteCanonicalBodyControl(before, after);
        if (!retained || !boundary || *retained != *boundary)
            retained.reset();
    }
}  // namespace Horo::Physics::Detail
