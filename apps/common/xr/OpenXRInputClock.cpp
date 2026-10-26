#include "OpenXRInputClock.h"

#include <bit>
#include <limits>
#include <string>
#if defined(_WIN32)
#include <windows.h>
#ifndef XR_USE_PLATFORM_WIN32
#define XR_USE_PLATFORM_WIN32
#endif
#else
#include <time.h>
#ifndef XR_USE_TIMESPEC
#define XR_USE_TIMESPEC
#endif
#endif
#if defined(XR_USE_GRAPHICS_API_VULKAN)
#include <vulkan/vulkan.h>
#endif
#include <openxr/openxr_platform.h>

namespace Horo::Application::Internal {
    namespace {
#if defined(_WIN32)
        constexpr const char *ConversionExtension = "XR_KHR_win32_convert_performance_counter_time";
        constexpr const char *ConversionFunction = "xrConvertWin32PerformanceCounterToTimeKHR";
#else
        constexpr const char *ConversionExtension = "XR_KHR_convert_timespec_time";
        constexpr const char *ConversionFunction = "xrConvertTimespecTimeToTimeKHR";
#endif
        /** @brief Clears only the synchronous native-call reservation, including exception/reentry failure paths. */
        struct ReadScope final {
            bool &active;

            explicit ReadScope(bool &value) noexcept : active(value) {
                active = true;
            }

            ~ReadScope() {
                active = false;
            }
        };
    }  // namespace

    /** @copydoc OpenXRInputClock::OpenXRInputClock */
    OpenXRInputClock::OpenXRInputClock(XR::OpenXRInternal::OpenXRNativeSession &native) noexcept : native_(&native) {}

    /** @copydoc OpenXRInputClock::Current */
    Result<XR::OpenXRInternal::NativeSessionBorrow> OpenXRInputClock::Current() const {
        using Outcome = Result<XR::OpenXRInternal::NativeSessionBorrow>;
        if (std::this_thread::get_id() != lane_)
            return Outcome::Failure(MakeError(XR::XRErrors::OperationIncompatible));
        auto native = native_->Borrow(session_);
        if (native.HasError())
            return native;
        if (native.Value().incarnation != incarnation_)
            return Outcome::Failure(MakeError(XR::XRErrors::IdentityStale));
        return native;
    }

    /** @copydoc OpenXRInputClock::PrepareScheduler */
    Result<void> OpenXRInputClock::PrepareScheduler(Runtime::RuntimeDispatchSource source, const Duration fixedStep) {
        if (std::this_thread::get_id() != lane_ || reading_ || source_.IsValid() || fixedStep.ToNanoseconds() <= 0 ||
            source.BindingStatus() != Runtime::RuntimeDispatchStatus::Valid)
            return Result<void>::Failure(MakeError(XR::XRErrors::OperationIncompatible));
        source_ = std::move(source);
        fixedStep_ = fixedStep;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRInputClock::Bind */
    Result<void> OpenXRInputClock::Bind(Runtime::RuntimeDispatchSource source, const Duration fixedStep,
                                        PFN_xrGetInstanceProcAddr getProc) {
        if (auto prepared = PrepareScheduler(std::move(source), fixedStep); prepared.HasError())
            return prepared;

        struct SchedulerRollback final {
            Runtime::RuntimeDispatchSource &source;
            Duration &step;
            bool committed{};

            ~SchedulerRollback() {
                if (!committed) {
                    source = {};
                    step = {};
                }
            }
        } rollback{source_, fixedStep_};

        auto bound = BindNative(getProc);
        rollback.committed = bound.HasValue();
        return bound;
    }

    /** @copydoc OpenXRInputClock::BindNative */
    Result<void> OpenXRInputClock::BindNative(PFN_xrGetInstanceProcAddr getProc) {
        const auto status = source_.BindingStatus();
        if (std::this_thread::get_id() != lane_ || reading_ || convert_ || !getProc || fixedStep_.ToNanoseconds() <= 0 ||
            (status != Runtime::RuntimeDispatchStatus::Valid && status != Runtime::RuntimeDispatchStatus::Reentrant))
            return Result<void>::Failure(MakeError(XR::XRErrors::OperationIncompatible));
        ReadScope scope{reading_};
        const auto selected = native_->Session();
        auto native = native_->Borrow(selected);
        if (native.HasError())
            return Result<void>::Failure(native.ErrorValue());
        if (!native_->HasExtension(ConversionExtension))
            return Result<void>::Failure(MakeError(XR::XRErrors::OperationUnsupported));
        PFN_xrVoidFunction converter{};
        const auto result = getProc(native.Value().instance, ConversionFunction, &converter);
        if (result != XR_SUCCESS || !converter)
            return Result<void>::Failure(
                MakeError(XR::XRErrors::OperationUnavailable,
                          std::string{ConversionFunction} + " unavailable (XrResult " + std::to_string(result) + ")"));
        auto fenced = native_->Borrow(selected);
        if (fenced.HasError())
            return Result<void>::Failure(fenced.ErrorValue());
        if (fenced.Value().incarnation != native.Value().incarnation || source_.BindingStatus() != status)
            return Result<void>::Failure(MakeError(XR::XRErrors::IdentityStale));
        session_ = selected;
        incarnation_ = native.Value().incarnation;
        convert_ = converter;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRInputClock::RetireNative */
    Result<void> OpenXRInputClock::RetireNative(const XR::XRSessionId &session) {
        if (std::this_thread::get_id() != lane_ || reading_)
            return Result<void>::Failure(MakeError(XR::XRErrors::OperationIncompatible));
        if (!session.IsValid() || session != session_)
            return Result<void>::Failure(MakeError(XR::XRErrors::IdentityStale));
        convert_ = {};
        session_ = {};
        incarnation_ = {};
        return Result<void>::Success();
    }

    /** @copydoc OpenXRInputClock::NativeSample */
    Result<XR::XRRuntimeTime> OpenXRInputClock::NativeSample(const XrInstance instance) const {
        using Outcome = Result<XR::XRRuntimeTime>;
        XrTime sample{};
#if defined(_WIN32)
        LARGE_INTEGER counter{};
        if (!QueryPerformanceCounter(&counter))
            return Outcome::Failure(MakeError(XR::XRErrors::OperationUnavailable));
        const auto result = std::bit_cast<PFN_xrConvertWin32PerformanceCounterToTimeKHR>(convert_)(instance, &counter, &sample);
#else
        timespec counter{};
        if (clock_gettime(CLOCK_MONOTONIC, &counter) != 0)
            return Outcome::Failure(MakeError(XR::XRErrors::OperationUnavailable));
        const auto result = std::bit_cast<PFN_xrConvertTimespecTimeToTimeKHR>(convert_)(instance, &counter, &sample);
#endif
        if (result != XR_SUCCESS)
            return Outcome::Failure(MakeError(XR::XRErrors::OperationUnavailable,
                                              std::string{ConversionFunction} + " failed (XrResult " + std::to_string(result) + ")"));
        return XR::XRRuntimeTime::Create(sample);
    }

    /** @copydoc OpenXRInputClock::Read */
    Result<XR::XRTimeEvidence> OpenXRInputClock::Read(const Runtime::FrameContext &context) const {
        using Outcome = Result<XR::XRTimeEvidence>;
        if (std::this_thread::get_id() != lane_ || reading_ || !convert_)
            return Outcome::Failure(MakeError(XR::XRErrors::OperationIncompatible));
        ReadScope scope{reading_};
        Runtime::RuntimeDispatchFacts facts;
        if (context.dispatchEvidence.Read(source_, Runtime::RuntimePhase::BuildInputSnapshot, facts) !=
            Runtime::RuntimeDispatchStatus::Valid)
            return Outcome::Failure(MakeError(XR::XRErrors::OperationIncompatible));
        const auto quantum = fixedStep_.ToNanoseconds();
        if (quantum <= 0 || (facts.committedTick && facts.committedDuration != fixedStep_) ||
            facts.committedTick > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / quantum))
            return Outcome::Failure(MakeError(XR::XRErrors::CapacityExceeded));
        auto native = Current();
        if (native.HasError())
            return Outcome::Failure(native.ErrorValue());
        auto sample = NativeSample(native.Value().instance);
        if (sample.HasError())
            return Outcome::Failure(sample.ErrorValue());
        if (auto fence = Current(); fence.HasError())
            return Outcome::Failure(fence.ErrorValue());
        Runtime::RuntimeDispatchFacts after;
        if (context.dispatchEvidence.Read(source_, Runtime::RuntimePhase::BuildInputSnapshot, after) !=
                Runtime::RuntimeDispatchStatus::Valid ||
            after.frame != facts.frame || after.committedTick != facts.committedTick || after.committedAttempt != facts.committedAttempt)
            return Outcome::Failure(MakeError(XR::XRErrors::IdentityStale));
        auto simulation = XR::XRSimulationTime::Create(static_cast<std::int64_t>(facts.committedTick) * quantum);
        if (simulation.HasError())
            return Outcome::Failure(simulation.ErrorValue());
        return Outcome::Success({sample.Value(), simulation.Value(), {}});
    }

    /** @copydoc OpenXRInputClock::PresentationTime */
    Result<XR::XRTimeEvidence> OpenXRInputClock::PresentationTime(const XR::XRRenderPredictionTime prediction) const {
        using Outcome = Result<XR::XRTimeEvidence>;
        if (!prediction.IsValid())
            return Outcome::Failure(MakeError(XR::XRErrors::OperationIncompatible));
        auto sample = PresentationSample();
        if (sample.HasError())
            return Outcome::Failure(sample.ErrorValue());
        return Outcome::Success({sample.Value(), {}, prediction});
    }

    /** @copydoc OpenXRInputClock::PresentationNow */
    Result<XR::XRRenderPredictionTime> OpenXRInputClock::PresentationNow() const {
        auto sample = PresentationSample();
        if (sample.HasError())
            return Result<XR::XRRenderPredictionTime>::Failure(sample.ErrorValue());
        return XR::XRRenderPredictionTime::Create(sample.Value().Nanoseconds());
    }

    /** @copydoc OpenXRInputClock::PresentationSample */
    Result<XR::XRRuntimeTime> OpenXRInputClock::PresentationSample() const {
        using Outcome = Result<XR::XRRuntimeTime>;
        if (std::this_thread::get_id() != lane_ || reading_ || !convert_)
            return Outcome::Failure(MakeError(XR::XRErrors::OperationIncompatible));
        ReadScope scope{reading_};
        auto native = Current();
        if (native.HasError())
            return Outcome::Failure(native.ErrorValue());
        auto sample = NativeSample(native.Value().instance);
        if (sample.HasError())
            return Outcome::Failure(sample.ErrorValue());
        if (auto fenced = Current(); fenced.HasError())
            return Outcome::Failure(fenced.ErrorValue());
        return sample;
    }
}  // namespace Horo::Application::Internal
