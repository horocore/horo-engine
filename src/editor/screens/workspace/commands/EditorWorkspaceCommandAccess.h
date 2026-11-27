#pragma once

/** @file EditorWorkspaceCommandAccess.h @brief Internal host boundary for active editor workspace commands. */

#include "Horo/Foundation/Result.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace Horo::Editor {
    struct EditorBuildCommandEndpoint;
    struct EditorSourceCommandEndpoint;
    struct EditorTestCommandEndpoint;
    struct EditorInspectionCommandEndpoint;
    struct EditorDebugCaptureEndpoint;

    /** @brief Borrowed application endpoints for the exact admitted workspace. Empty endpoints are omitted. */
    struct EditorWorkspaceCommandEndpoints {
        std::weak_ptr<const EditorSourceCommandEndpoint> source;
        std::weak_ptr<const EditorBuildCommandEndpoint> build;
        std::weak_ptr<const EditorTestCommandEndpoint> test;
        std::weak_ptr<const EditorInspectionCommandEndpoint> inspection;
        std::weak_ptr<const EditorDebugCaptureEndpoint> debugCapture;
    };

    /** @brief One committed workspace's owner callbacks. They expire when the workspace leaves. */
    struct EditorWorkspaceCommandAccess {
        std::string projectIdentity;
        std::function<EditorWorkspaceCommandEndpoints(std::uint64_t)> bind;
        std::function<void()> revoke;
    };

    /** @brief Application host for workspace command admission and retirement, called on the editor lane. */
    class IEditorWorkspaceCommandHost {
    public:
        virtual ~IEditorWorkspaceCommandHost() = default;

        /** @brief Attaches an already committed workspace. @param access Borrowed owner callbacks. @return Typed outcome. */
        [[nodiscard]] virtual Result<void> Attach(EditorWorkspaceCommandAccess access) = 0;
        /** @brief Pumps admitted commands before normal workspace update. @return Typed outcome. */
        [[nodiscard]] virtual Result<void> PumpWorkspaceCommands() = 0;
        /** @brief Closes admission and revokes borrowed commands before workspace teardown. @return Typed outcome. */
        [[nodiscard]] virtual Result<void> Detach() = 0;
    };
}  // namespace Horo::Editor
