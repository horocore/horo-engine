#pragma once

#include "Horo/Editor/EditorMenuModel.h"
#include "Horo/Editor/GuiRoute.h"
#include "Horo/Foundation/Diagnostics.h"
#include "Horo/Foundation/Result.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace Horo::Editor {

    /**
     * @file GuiScreen.h
     * @brief Top-level GUI screen and leave-guard contracts.
     */

    /** @brief Reason why a screen requires resolution before leaving. */
    enum class LeaveRequirementKind {
        DirtyDocument,
        UnsavedDraft,
        RunningOperation,
        RecoveryDecision,
        NativeDialogCompletion,
    };

    /** @brief Allowed actions when resolving a leave requirement. */
    enum class LeaveAction {
        Save,
        Discard,
        CancelOperation,
        KeepRunning,
        Wait,
        Stay,
    };

    using LeaveSubjectId = std::uint64_t;
    using LeaveRequirementRevision = std::uint32_t;
    using ScreenId = std::uint32_t;
    using ScreenInstanceId = std::uint64_t;
    using NavigationAttemptId = std::uint64_t;
    using ProjectCreationOperationId = std::uint64_t;

    /** @brief Screen-space region reserved by the editor shell for active screen content. */
    struct GuiContentRegion {
        float x = 0.0F;
        float y = 0.0F;
        float width = 0.0F;
        float height = 0.0F;
    };

    /** @brief Description of a pending leave blocker requiring resolution. */
    struct LeaveRequirement {
        LeaveRequirementKind kind;
        LeaveSubjectId subject;
        LeaveRequirementRevision revision;
        std::vector<LeaveAction> allowedActions;
    };

    /** @brief Initial decision on whether a screen allows leaving. */
    enum class LeaveDisposition {
        Allow,
        Deny,
        RequireResolution,
    };

    /** @brief Result of querying CanLeave on a screen. */
    struct LeaveDecision {
        LeaveDisposition disposition;
        std::optional<LeaveRequirement> requirement;
    };

    /** @brief Target representing process application termination. */
    struct ApplicationCloseTarget {
        bool operator==(const ApplicationCloseTarget &) const = default;
    };

    /** @brief Destination target for a leave request. */
    struct LeaveTarget {
        std::variant<GuiRoute, ApplicationCloseTarget> value;
    };

    /** @brief Resolution selected by the user or host for a pending leave requirement. */
    struct LeaveResolution {
        LeaveSubjectId subject;
        LeaveRequirementRevision revision;
        LeaveAction action;
    };

    /** @brief Error codes produced when a leave resolution attempt fails. */
    enum class LeaveErrorCode {
        StaleSubject,
        ActionNotAllowed,
        OperationFailed,
    };

    /** @brief Diagnostic error when resolve-leave fails. */
    struct LeaveError {
        LeaveErrorCode code;
        std::vector<Diagnostic> diagnostics;
    };

    /** @brief Stable codes for navigation failures. */
    enum class NavigationErrorCode {
        InvalidRouteParameters,
        Busy,
        LeaveDenied,
        Cancelled,
        LeaveResolutionLimitExceeded,
        DestinationConstructionFailed,
        DestinationEntryFailed,
    };

    /** @brief Error details returned when navigation cannot commit. */
    struct NavigationError {
        NavigationErrorCode code;
        std::vector<Diagnostic> diagnostics;
    };

    /** @brief Abstract contract for a top-level GUI screen inside HoroEditor. */
    class GuiScreen {
    public:
        virtual ~GuiScreen() = default;

        /** @brief Returns stable identifier for this screen class. */
        [[nodiscard]] virtual ScreenId Id() const = 0;

        /** @brief Called during navigation commit before becoming the active screen. */
        [[nodiscard]] virtual Result<void> OnEnter(const GuiRoute &route) = 0;

        /** @brief Updates screen-local simulation and time-dependent logic. */
        virtual void OnUpdate(float dt) = 0;

        /** @brief Captures current routed input after the host commits its snapshot and before fixed updates. */
        virtual void OnInputSnapshot() {}

        /** @brief Advances one fixed simulation tick for screens that own an isolated play session. */
        virtual void OnFixedUpdate(std::uint64_t simulationTick, double fixedDeltaSeconds) {
            static_cast<void>(simulationTick);
            static_cast<void>(fixedDeltaSeconds);
        }

        /** @brief Renders the screen UI inside the shell-provided application content area. */
        virtual void Draw(const GuiContentRegion &contentRegion) = 0;

        /** @brief Appends active panel IDs used by panel-scoped status contribution visibility. */
        virtual void CollectActivePanelIds(std::vector<std::string_view> &output) const {
            static_cast<void>(output);
        }

        /**
         * @brief Gives the active screen an opportunity to handle an application menu invocation.
         * @param invocation Typed invocation emitted by a native or in-window menu renderer.
         * @return True when the screen consumed the action.
         */
        virtual bool HandleMenuInvocation(const EditorMenuInvocation &invocation) {
            static_cast<void>(invocation);
            return false;
        }

        /** @brief Queries whether navigation to target is permitted right now. */
        [[nodiscard]] virtual LeaveDecision CanLeave(const LeaveTarget &target) const = 0;

        /** @brief Resolves a pending leave requirement with a chosen action. */
        [[nodiscard]] virtual Result<LeaveDecision> ResolveLeave(const LeaveTarget &target, const LeaveResolution &resolution) = 0;

        /** @brief Called immediately before screen destruction upon route change. */
        virtual void OnLeave() = 0;
    };

}  // namespace Horo::Editor
