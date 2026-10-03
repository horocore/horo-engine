#pragma once

namespace Horo::Tests::MixerFixture {
    /** @brief Child-process outcomes distinguish mixer rollback from MSVC checked-STL termination. */
    enum class AllocationFailureOutcome : int {
        Caught,
        Complete,
        MsvcDebugProxyTerminated,
        UnexpectedFailure
    };
}  // namespace Horo::Tests::MixerFixture
