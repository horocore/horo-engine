#include "Horo/Runtime/FrameScheduler.h"

#include <cstdint>
#include <type_traits>

static_assert(std::is_same_v<decltype(Horo::Runtime::FrameContext::presentationClockGeneration), std::uint64_t>);
static_assert(std::is_same_v<decltype(Horo::Runtime::FrameContext::presentationContinuity), Horo::Runtime::PresentationClockContinuity>);

static_assert(std::is_same_v<decltype(Horo::Runtime::FrameContext::committedFixedStep), Horo::Runtime::CommittedFixedStepEvidence>);
static_assert(std::is_same_v<decltype(Horo::Runtime::FixedStepContext::attemptNumber), std::uint64_t>);
static_assert(std::is_same_v<decltype(Horo::Runtime::FixedStepContext::frameNumber), std::uint64_t>);
static_assert(std::is_same_v<decltype(Horo::Runtime::CommittedFixedStepEvidence::duration), Horo::Duration>);

static_assert(std::is_same_v<decltype(Horo::Runtime::FrameContext::presentationAdmittedDuration), Horo::Duration>);

static_assert(std::is_default_constructible_v<Horo::Runtime::RuntimeDispatchEvidence>);
static_assert(std::is_nothrow_copy_constructible_v<Horo::Runtime::RuntimeDispatchSource>);
static_assert(std::is_nothrow_copy_constructible_v<Horo::Runtime::RuntimeDispatchEvidence>);
static_assert(std::is_same_v<decltype(Horo::Runtime::FrameContext::dispatchEvidence), Horo::Runtime::RuntimeDispatchEvidence>);
static_assert(std::is_same_v<decltype(Horo::Runtime::FixedStepContext::dispatchEvidence), Horo::Runtime::RuntimeDispatchEvidence>);

int main() {
    return 0;
}
