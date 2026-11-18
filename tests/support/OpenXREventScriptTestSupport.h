#pragma once
/** @file OpenXREventScriptTestSupport.h
 * @brief Existing official native dispatch script shared by lifecycle and application tests.
 */
#include "OpenXRSessionEvents.h"
#include "support/OpenXRNativeTestSupport.h"

#include <algorithm>
#include <bit>
#include <utility>

namespace Horo::Tests::OpenXR {
    struct EventScript final {
        static inline thread_local EventScript *active{};
        std::array<XrSessionState, OpenXRSessionEvents::MaximumEventsPerPoll + 16> states{};
        std::size_t count{};
        std::size_t cursor{};
        XrSession session{};
        unsigned begins{};
        unsigned exits{};
        unsigned ends{};
        unsigned polls{};
        bool running{};
        OpenXRSessionEvents *reenter{};
        bool reentryRejected{};
        bool loseEvents{};
        bool loseInstance{};
        std::optional<XrEventDataReferenceSpaceChangePending> referenceChange;
        XrResult pollResult{XR_SUCCESS};
        XrResult beginResult{XR_SUCCESS};
        XrResult exitResult{XR_SUCCESS};
        XrResult endResult{XR_SUCCESS};

        template <typename Event> static void CopyEvent(XrEventDataBuffer &buffer, const Event &event) {
            static_assert(std::is_trivially_copyable_v<Event> && sizeof(Event) <= sizeof(XrEventDataBuffer));
            const auto source = std::as_bytes(std::span{&event, 1});
            const auto destination = std::as_writable_bytes(std::span{&buffer, 1});
            std::ranges::copy(source, destination.begin());
        }

        void Push(XrSessionState state) {
            REQUIRE(count < states.size());
            states[count++] = state;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Poll(XrInstance, XrEventDataBuffer *buffer) {
            ++active->polls;
            if (active->pollResult != XR_SUCCESS)
                return std::exchange(active->pollResult, XR_SUCCESS);
            if (active->loseInstance) {
                active->loseInstance = false;
                const XrEventDataInstanceLossPending event{XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING, nullptr, 1};
                CopyEvent(*buffer, event);
                return XR_SUCCESS;
            }
            if (active->loseEvents) {
                active->loseEvents = false;
                const XrEventDataEventsLost event{XR_TYPE_EVENT_DATA_EVENTS_LOST, nullptr, 1};
                CopyEvent(*buffer, event);
                return XR_SUCCESS;
            }
            if (active->reenter) {
                auto *owner = active->reenter;
                active->reenter = nullptr;
                active->reentryRejected = owner->Poll(true).HasError();
            }
            if (active->referenceChange) {
                CopyEvent(*buffer, *std::exchange(active->referenceChange, std::nullopt));
                return XR_SUCCESS;
            }
            if (active->cursor == active->count)
                return XR_EVENT_UNAVAILABLE;
            const XrEventDataSessionStateChanged event{XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED, nullptr, active->session,
                                                       active->states[active->cursor++], 1};
            CopyEvent(*buffer, event);
            return XR_SUCCESS;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Begin(XrSession session, const XrSessionBeginInfo *info) {
            REQUIRE(session == active->session);
            REQUIRE(info->primaryViewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);
            REQUIRE_FALSE(active->running);
            ++active->begins;
            const auto result = std::exchange(active->beginResult, XR_SUCCESS);
            if (result == XR_SUCCESS)
                active->running = true;
            return result;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Exit(XrSession session) {
            REQUIRE(session == active->session);
            REQUIRE(active->running);
            ++active->exits;
            return std::exchange(active->exitResult, XR_SUCCESS);
        }

        static XRAPI_ATTR XrResult XRAPI_CALL End(XrSession session) {
            REQUIRE(session == active->session);
            REQUIRE(active->running);
            ++active->ends;
            const auto result = std::exchange(active->endResult, XR_SUCCESS);
            if (result == XR_SUCCESS)
                active->running = false;
            return result;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL GetProc(XrInstance, const char *name, PFN_xrVoidFunction *output) {
            *output = nullptr;
            const std::string_view symbol{name};
            if (symbol == "xrPollEvent")
                *output = std::bit_cast<PFN_xrVoidFunction>(&Poll);
            if (symbol == "xrBeginSession")
                *output = std::bit_cast<PFN_xrVoidFunction>(&Begin);
            if (symbol == "xrRequestExitSession")
                *output = std::bit_cast<PFN_xrVoidFunction>(&Exit);
            if (symbol == "xrEndSession")
                *output = std::bit_cast<PFN_xrVoidFunction>(&End);
            return *output ? XR_SUCCESS : XR_ERROR_FUNCTION_UNSUPPORTED;
        }
    };

}  // namespace Horo::Tests::OpenXR
