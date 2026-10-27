#pragma once
/** @file OpenXRClockTestSupport.h @brief Same official conversion script used by actual clock and signed-loader lifecycle fixtures. */
#include "OpenXRInputClock.h"
#include "support/OpenXRNativeTestSupport.h"

#include <bit>
#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

namespace Horo::Tests::OpenXR {
#if defined(_WIN32)
    constexpr const char *ClockExtension = "XR_KHR_win32_convert_performance_counter_time";
    constexpr std::string_view ClockFunction = "xrConvertWin32PerformanceCounterToTimeKHR";
#else
    constexpr const char *ClockExtension = "XR_KHR_convert_timespec_time";
    constexpr std::string_view ClockFunction = "xrConvertTimespecTimeToTimeKHR";
#endif

    /** @brief Converts actual OS counter input through the same official ABI used by the production clock. */
    struct ClockScript final {
        inline static thread_local ClockScript *active{};
        Application::Internal::OpenXRInputClock *clock{};
        XrTime sample{42'000'000};
        XrResult result{XR_SUCCESS};
        std::size_t conversions{};
        bool reenter{};
        bool nestedRejected{};
        bool missing{};

        explicit ClockScript(Application::Internal::OpenXRInputClock *owner = nullptr) : clock(owner) {
            REQUIRE(active == nullptr);
            active = this;
        }

        ClockScript(const ClockScript &) = delete;
        ClockScript &operator=(const ClockScript &) = delete;

        ~ClockScript() {
            active = nullptr;
        }

#if defined(_WIN32)
        static XRAPI_ATTR XrResult XRAPI_CALL Convert(XrInstance instance, const LARGE_INTEGER *counter, XrTime *output) {
            CHECK(counter->QuadPart > 0);
#else
        static XRAPI_ATTR XrResult XRAPI_CALL Convert(XrInstance instance, const timespec *counter, XrTime *output) {
            CHECK(counter->tv_sec > 0);
            CHECK(counter->tv_nsec >= 0);
            CHECK(counter->tv_nsec < 1'000'000'000);
#endif
            CHECK(instance != XR_NULL_HANDLE);
            ++active->conversions;
            if (active->reenter && active->clock)
                active->nestedRejected = active->clock->PresentationNow().HasError();
            *output = active->sample;
            return active->result;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Resolve(XrInstance instance, const char *name, PFN_xrVoidFunction *output) {
            if (std::string_view{name} == ClockFunction) {
                *output = active->missing ? nullptr : std::bit_cast<PFN_xrVoidFunction>(&Convert);
                return *output ? XR_SUCCESS : XR_ERROR_FUNCTION_UNSUPPORTED;
            }
            return Script::GetProc(instance, name, output);
        }
    };

}  // namespace Horo::Tests::OpenXR
