#include "CanonicalPhysicsStateFpControl.h"

#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#if defined(_MSC_VER)
#include <float.h>
#endif
#endif

namespace Horo::Physics::Detail {
    /** @copydoc ReadCanonicalPhysicsStateFpControl */
    Result<PhysicsStateFpControl> ReadCanonicalPhysicsStateFpControl() {
#if defined(_M_X64) || defined(__x86_64__)
        const auto simd = static_cast<std::uint64_t>(_mm_getcsr() & 0xFFC0U);
        std::uint64_t scalar{};
#if defined(_MSC_VER)
        unsigned int word{};
        if (_controlfp_s(&word, 0, 0) != 0)
            return Result<PhysicsStateFpControl>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
        scalar = word;
#else
        std::uint16_t word{};
        __asm__ __volatile__("fnstcw %0" : "=m"(word));
        scalar = word;
#endif
        return Result<PhysicsStateFpControl>::Success({PhysicsStateFpArchitecture::X86MxcsrX87, simd | (scalar << 32)});
#elif defined(__aarch64__)
        std::uint64_t control{};
        __asm__ __volatile__("mrs %0, fpcr" : "=r"(control));
        return Result<PhysicsStateFpControl>::Success({PhysicsStateFpArchitecture::ArmFpcr, control});
#else
        return Result<PhysicsStateFpControl>::Failure(MakeError(PhysicsErrors::OperationUnsupported,
                                                                "Canonical Physics state has no reviewed native floating-point "
                                                                "control encoding for this target."));
#endif
    }
}  // namespace Horo::Physics::Detail
