#include "OpenXRInputClock.h"

#include <bit>
#include <format>
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
        /** @brief Clears only the synchronous native-call reservation, including exception/reentry failure paths. */
        struct ReadScope final {
            ReadScope(const ReadScope &) = delete;
            ReadScope &operator=(const ReadScope &) = delete;

            bool &active;

            explicit ReadScope(bool &value) noexcept : active(value) {
                value = true;
            }

            ~ReadScope() {
                active = false;
            }
        };

        /** @brief Reads an optional external clock without retaining its owner. */
        std::optional<OpenXRInputCaptureStamp> CaptureNow(const OpenXRInputCaptureClock clock) noexcept {
            return clock.read ? clock.read(clock.owner) : std::nullopt;
        }

        /** @brief Accepts only a closed interval in one ordered nonzero capture domain. */
        std::optional<OpenXRInputCaptureInterval> CaptureInterval(const std::optional<OpenXRInputCaptureStamp> &earliest,
                                                                  const std::optional<OpenXRInputCaptureStamp> &latest) noexcept {
            if (!earliest || !latest || !earliest->clockDomain || earliest->clockDomain != latest->clockDomain ||
                earliest->nanoseconds > latest->nanoseconds)
                return std::nullopt;
            return OpenXRInputCaptureInterval{*earliest, *latest};
        }

        /** @brief Checks the committed tick against the exact owner's immutable simulation step. */
        Result<XR::XRSimulationTime> CommittedSimulationTime(const Runtime::RuntimeDispatchFacts &facts, const Duration step) {
            const auto quantum = step.ToNanoseconds();
            if (quantum <= 0 || (facts.committedTick && facts.committedDuration != step) ||
                facts.committedTick > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)() / quantum))
                return Result<XR::XRSimulationTime>::Failure(MakeError(XR::XRErrors::CapacityExceeded));
            return XR::XRSimulationTime::Create(static_cast<std::int64_t>(facts.committedTick) * quantum);
        }

        /** @brief Rechecks that the scheduler still admits the same Input dispatch after foreign calls. */
        bool MatchesInputDispatch(const Runtime::FrameContext &context, const Runtime::RuntimeDispatchSource &source,
                                  const Runtime::RuntimeDispatchFacts &before) noexcept {
            Runtime::RuntimeDispatchFacts after;
            return context.dispatchEvidence.Read(source, Runtime::RuntimePhase::BuildInputSnapshot, after) ==
                       Runtime::RuntimeDispatchStatus::Valid &&
                   after.frame == before.frame && after.committedTick == before.committedTick &&
                   after.committedAttempt == before.committedAttempt;
        }
    }  // namespace

    /** @copydoc OpenXRInputClock::OpenXRInputClock */
    OpenXRInputClock::OpenXRInputClock(XR::OpenXRInternal::OpenXRNativeSession &native) noexcept : native_(&native) {}

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
    Result<void> OpenXRInputClock::Bind(Runtime::RuntimeDispatchSource source, const Duration fixedStep) {
        if (auto prepared = PrepareScheduler(std::move(source), fixedStep); prepared.HasError())
            return prepared;

        struct SchedulerRollback final {
            SchedulerRollback(Runtime::RuntimeDispatchSource &owner, Duration &fixedStep) noexcept : source(owner), step(fixedStep) {}

            SchedulerRollback(const SchedulerRollback &) = delete;
            SchedulerRollback &operator=(const SchedulerRollback &) = delete;

            Runtime::RuntimeDispatchSource &source;
            Duration &step;
            bool committed{};

            /** @brief Retains the scheduler binding after native binding succeeds. */
            void Commit() noexcept {
                committed = true;
            }

            ~SchedulerRollback() {
                if (!committed) {
                    source = {};
                    step = {};
                }
            }
        };

        SchedulerRollback rollback{source_, fixedStep_};

        auto bound = BindNative();
        if (bound.HasValue())
            rollback.Commit();
        return bound;
    }

    /** @copydoc OpenXRInputClock::BindNative */
    Result<void> OpenXRInputClock::BindNative() {
        const auto status = source_.BindingStatus();
        if (std::this_thread::get_id() != lane_ || reading_ || provider_ || fixedStep_.ToNanoseconds() <= 0 ||
            (status != Runtime::RuntimeDispatchStatus::Valid && status != Runtime::RuntimeDispatchStatus::Reentrant))
            return Result<void>::Failure(MakeError(XR::XRErrors::OperationIncompatible));
        ReadScope scope{reading_};
        const auto selected = native_->Session();
        auto provider = native_->RegisterFeatureProvider(selected, OpenXRPlatformTimeRoute());
        if (provider.HasError())
            return Result<void>::Failure(provider.ErrorValue());
        if (source_.BindingStatus() != status) {
            native_->RevokeFeatureProvider(provider.Value());
            return Result<void>::Failure(MakeError(XR::XRErrors::IdentityStale));
        }
        session_ = selected;
        provider_.emplace(std::move(provider).Value());
        return Result<void>::Success();
    }

    /** @copydoc OpenXRInputClock::RetireNative */
    Result<void> OpenXRInputClock::RetireNative(const XR::XRSessionId &session) {
        if (std::this_thread::get_id() != lane_ || reading_)
            return Result<void>::Failure(MakeError(XR::XRErrors::OperationIncompatible));
        if (!session.IsValid() || session != session_)
            return Result<void>::Failure(MakeError(XR::XRErrors::IdentityStale));
        native_->RevokeFeatureProvider(*provider_);
        provider_.reset();
        session_ = {};
        return Result<void>::Success();
    }

    /** @copydoc OpenXRInputClock::NativeSample */
    Result<XR::XRRuntimeTime> OpenXRInputClock::NativeSample(XrInstance instance, PFN_xrVoidFunction convert) {
        using Outcome = Result<XR::XRRuntimeTime>;
        XrTime sample{};
#if defined(_WIN32)
        using NativeCounter = LARGE_INTEGER;
        using NativeCounterConversion = PFN_xrConvertWin32PerformanceCounterToTimeKHR;
#else
        using NativeCounter = timespec;
        using NativeCounterConversion = PFN_xrConvertTimespecTimeToTimeKHR;
#endif
        NativeCounter counter{};
#if defined(_WIN32)
        if (!QueryPerformanceCounter(&counter))
#else
        // xrConvertTimespecTimeToTimeKHR requires the CLOCK_MONOTONIC epoch.
        // std::chrono::steady_clock does not promise that native epoch across platforms.
        if (clock_gettime(CLOCK_MONOTONIC, &counter) != 0)
#endif
            return Outcome::Failure(MakeError(XR::XRErrors::OperationUnavailable));
        if (const auto result = std::bit_cast<NativeCounterConversion>(convert)(instance, &counter, &sample); result != XR_SUCCESS)
            return Outcome::Failure(MakeError(XR::XRErrors::OperationUnavailable,
                                              std::format("{} failed (XrResult {})", OpenXRPlatformTimeRoute().extensionSymbol,
                                                          static_cast<std::int32_t>(result))));
        return XR::XRRuntimeTime::Create(sample);
    }

    /** @copydoc OpenXRInputClock::Read */
    Result<XR::XRTimeEvidence> OpenXRInputClock::Read(const Runtime::FrameContext &context) const {
        auto observation = ReadWithCaptureClock(context, {});
        return observation.HasValue() ? Result<XR::XRTimeEvidence>::Success(observation.Value().time)
                                      : Result<XR::XRTimeEvidence>::Failure(observation.ErrorValue());
    }

    /** @copydoc OpenXRInputClock::ReadWithCaptureClock */
    Result<OpenXRInputClockObservation> OpenXRInputClock::ReadWithCaptureClock(const Runtime::FrameContext &context,
                                                                               const OpenXRInputCaptureClock capture) const {
        using Outcome = Result<OpenXRInputClockObservation>;
        if (std::this_thread::get_id() != lane_ || reading_ || !provider_)
            return Outcome::Failure(MakeError(XR::XRErrors::OperationIncompatible));
        ReadScope scope{reading_};
        Runtime::RuntimeDispatchFacts facts;
        if (context.dispatchEvidence.Read(source_, Runtime::RuntimePhase::BuildInputSnapshot, facts) !=
            Runtime::RuntimeDispatchStatus::Valid)
            return Outcome::Failure(MakeError(XR::XRErrors::OperationIncompatible));
        const auto simulation = CommittedSimulationTime(facts, fixedStep_);
        if (simulation.HasError())
            return Outcome::Failure(simulation.ErrorValue());
        const auto earliest = CaptureNow(capture);
        auto native = native_->BorrowFeatureProvider(*provider_);
        if (native.HasError())
            return Outcome::Failure(native.ErrorValue());
        auto sample = NativeSample(native.Value().native.instance, native.Value().function);
        if (sample.HasError())
            return Outcome::Failure(sample.ErrorValue());
        const auto latest = CaptureNow(capture);
        if (auto fence = native_->BorrowFeatureProvider(*provider_); fence.HasError())
            return Outcome::Failure(fence.ErrorValue());
        if (!MatchesInputDispatch(context, source_, facts))
            return Outcome::Failure(MakeError(XR::XRErrors::IdentityStale));
        return Outcome::Success({{sample.Value(), simulation.Value(), {}}, CaptureInterval(earliest, latest)});
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

    /** @copydoc OpenXRInputClock::RuntimeNow */
    Result<XR::XRRuntimeTime> OpenXRInputClock::RuntimeNow() const {
        return PresentationSample();
    }

    /** @copydoc OpenXRInputClock::ReadRetiredSessionTime */
    Result<XR::XRRuntimeTime> OpenXRInputClock::ReadRetiredSessionTime(const XR::XRLoaderPreflightAttempt attempt) const {
        using Outcome = Result<XR::XRRuntimeTime>;
        if (std::this_thread::get_id() != lane_ || reading_ || provider_)
            return Outcome::Failure(MakeError(XR::XRErrors::OperationIncompatible));
        ReadScope scope{reading_};
        const auto selection = native_->BorrowDiscovery(attempt);
        if (selection.HasError())
            return Outcome::Failure(selection.ErrorValue());
        const auto route = OpenXRPlatformTimeRoute();
        const auto admitted = native_->ReadExtensionAdmission(attempt, route.extension);
        if (admitted.HasError())
            return Outcome::Failure(admitted.ErrorValue());
        if (admitted.Value().state != XR::OpenXRInternal::NativeExtensionState::Enabled)
            return Outcome::Failure(
                MakeError(XR::XRErrors::OperationUnsupported, "XR reference change requires an admitted runtime clock."));

        const auto &native = selection.Value();
        const auto fenced = [&]() -> Result<void> {
            const auto current = native_->BorrowDiscovery(attempt);
            if (current.HasError())
                return Result<void>::Failure(current.ErrorValue());
            if (current.Value().instance != native.instance || current.Value().system != native.system)
                return Result<void>::Failure(MakeError(XR::XRErrors::IdentityStale));
            return Result<void>::Success();
        };
        PFN_xrVoidFunction convert{};
        const auto resolved = native.getProc(native.instance, route.extensionSymbol.data(), &convert);
        if (auto current = fenced(); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (resolved != XR_SUCCESS || !convert)
            return Outcome::Failure(
                MakeError(XR::XRErrors::LoaderIncompatible,
                          std::format("{} unavailable (XrResult {})", route.extensionSymbol, static_cast<std::int32_t>(resolved))));
        auto sampled = NativeSample(native.instance, convert);
        if (auto current = fenced(); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        return sampled;
    }

    /** @copydoc OpenXRInputClock::PresentationSample */
    Result<XR::XRRuntimeTime> OpenXRInputClock::PresentationSample() const {
        using Outcome = Result<XR::XRRuntimeTime>;
        if (std::this_thread::get_id() != lane_ || reading_ || !provider_)
            return Outcome::Failure(MakeError(XR::XRErrors::OperationIncompatible));
        ReadScope scope{reading_};
        auto native = native_->BorrowFeatureProvider(*provider_);
        if (native.HasError())
            return Outcome::Failure(native.ErrorValue());
        auto sample = NativeSample(native.Value().native.instance, native.Value().function);
        if (sample.HasError())
            return Outcome::Failure(sample.ErrorValue());
        if (auto fenced = native_->BorrowFeatureProvider(*provider_); fenced.HasError())
            return Outcome::Failure(fenced.ErrorValue());
        return sample;
    }
}  // namespace Horo::Application::Internal
