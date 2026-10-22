#pragma once

#include "PrefabHierarchyTestSupport.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace Horo::Editor::PrefabWorkspaceTestSupport {
    using namespace PrefabHierarchyTestSupport;

    /** @brief Captures one actual owner publication as a panel would, without a document mutation capability. */
    inline EditorWorkspaceViewCommandData Intent(const EditorWorkspaceViewModel &model, PrefabHierarchyTarget target,
                                                 PrefabHierarchyAction action) {
        REQUIRE(model.prefabHierarchy != nullptr);
        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::PrefabHierarchy;
        command.prefabHierarchy = PrefabHierarchyCommand{model.prefabHierarchy->documentSession, model.prefabHierarchy->publication,
                                                         model.prefabHierarchy->documentRevision, target, action};
        return command;
    }

    /** @brief Exercises actual Scene persistence, source pair capture, canonical history and workspace routes. */
    struct Workspace final {
        Project project;
        Runtime::RuntimeSceneService runtime;
        std::size_t captures{};
        std::optional<std::filesystem::path> navigated;
        std::optional<std::string> navigatedIdentity;
        std::optional<std::filesystem::path> diagnosticPath;
        bool diagnosticRoute{};
        bool persistenceAvailable{true};
        std::unique_ptr<EditorWorkspaceController> controller;

        explicit Workspace(const std::string &sourceName = "hierarchy", const bool useDiagnosticRoute = false)
            : project(sourceName), diagnosticRoute(useDiagnosticRoute) {
            project.WriteScene();
            Open(PrefabAsset(9).Asset());
        }

        void OpenPrefab() {
            const auto &model = controller->ViewModel();
            controller->ProcessCommand(Intent(model, model.prefabHierarchy->entries.front().target, PrefabHierarchyAction::OpenSource));
            REQUIRE(controller->ViewModel().prefabDocuments.size() == 1);
        }

        void Rename(const std::string &name) {
            const auto fence = controller->ViewModel().prefabDocuments.front().fence;
            controller->ProcessCommand(
                {.command = EditorWorkspaceViewCommand::PrefabDocument, .prefabFence = fence, .prefabEdit = RenamePrefabObject{{0}, name}});
        }

        void Action(const PrefabDocumentAction action, std::shared_ptr<const DocumentRecoveryRecord> recovery = {}) {
            const auto fence = controller->ViewModel().prefabDocuments.front().fence;
            controller->ProcessCommand({.command = EditorWorkspaceViewCommand::PrefabDocument,
                                        .prefabFence = fence,
                                        .prefabRecovery = std::move(recovery),
                                        .prefabAction = action});
        }

        void Open(const Assets::AssetId session) {
            controller = std::make_unique<
                EditorWorkspaceController>(project.directory.path, runtime, project.registry.Snapshot(),
                                           EditorWorkspaceDependencies{.mutations = persistenceAvailable ? &project.mutations : nullptr,
                                                                       .durableFiles = persistenceAvailable ? &project.files : nullptr,
                                                                       .sourceNavigation =
                                                                           {.sourceOpenNavigator =
                                                                                diagnosticRoute
                                                                                    ? SourceOpenNavigator{}
                                                                                    : SourceOpenNavigator{[this](const SourceOpenResult
                                                                                                                     &source) {
                navigated = source.location.absolutePath;
                navigatedIdentity = source.location.document.Value();
                return true;
            }},
                                                                            .diagnosticSourceNavigator =
                                                                                [this](const DiagnosticSourceRequest &source) {
                diagnosticPath = source.absolutePath;
                return true;
            }},
                                                                       .documentSessionOwner = session,
                                                                       .prefab = {.sources = [this](const auto &registry, const auto roots,
                                                                                                    const auto &limits,
                                                                                                    const auto &cancellation) {
                ++captures;
                return project.Capture(registry, roots, limits, cancellation);
            }}});
            REQUIRE_FALSE(controller->InitializationError().has_value());
        }
    };

}  // namespace Horo::Editor::PrefabWorkspaceTestSupport
