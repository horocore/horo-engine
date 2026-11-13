#pragma once
/** @file EditorUiStylePreparation.h @brief Executable-owned native adapter for the portable UI style producer. */
#include "Horo/Editor/ProjectMutation.h"
#include "Horo/Editor/UiInspectorResources.h"
#include "Horo/Foundation/JobSystem.h"

namespace Horo::Editor {
    /** @brief Bind actual process services to the same factory consumed by workspace activation.
     * @param jobs Scheduler. @param assets Project publisher. @param mutations Source authority. @param files Durable writer.
     * @return Inert per-workspace factory; all borrowed services outlive the returned capabilities and their joined work.
     */
    [[nodiscard]] UiInspectorResourcesFactory MakeEditorUiInspectorResourcesFactory(JobSystem &jobs, Assets::AssetRegistry &assets,
                                                                                    ProjectMutationCoordinator &mutations,
                                                                                    DurableFileSystem &files);

    /** @brief Compose the real source/cook/load producer at the executable boundary.
     * @param root Actual project root. @param workspace Never-reused activation identity. @param jobs Process scheduler.
     * @param assets Actual project publisher. @param mutations Source authority. @param files Durable publication authority.
     * @return Owned inspector capability or a typed setup error. Borrowed services outlive capability shutdown.
     */
    [[nodiscard]] Result<std::unique_ptr<UiInspectorResources>> CreateEditorUiInspectorResources(const std::filesystem::path &root,
                                                                                                 Assets::AssetId workspace, JobSystem &jobs,
                                                                                                 Assets::AssetRegistry &assets,
                                                                                                 ProjectMutationCoordinator &mutations,
                                                                                                 DurableFileSystem &files);
}  // namespace Horo::Editor
