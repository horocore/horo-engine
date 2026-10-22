#include "Horo/Foundation/Platform.h"
#include "UiInspectorDraft.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <array>
#include <format>
#include <limits>

namespace Horo::Editor {
    namespace {
        /** @brief Derives a stable candidate from the exact document revision and owner state, probing bounded authored collisions. */
        Result<Runtime::Ui::UiElementId> NewElementId(const UiCanvasDocument &document) {
            using namespace Runtime::Ui;
            for (std::size_t probe = 0; probe <= MaximumUiDocumentElements; ++probe) {
                const auto seed = std::format("{}:{}:{}:{}", Assets::AssetId::FromBytes(document.Document().Id().Bytes()).ToString(),
                                              document.Document().Revision().Value(), document.CurrentState().value, probe);
                const auto hash = ComputeSha256(std::as_bytes(std::span{seed.data(), seed.size()}));
                SerializedUiId bytes{};
                std::copy_n(hash.bytes.begin(), bytes.size(), bytes.begin());
                const auto candidate = UiElementId::Create(bytes);
                if (candidate.HasError())
                    continue;
                if (std::ranges::none_of(document.Document().Elements(),
                                         [&](const auto &element) {
                    return element.id == candidate.Value();
                }) &&
                    std::ranges::none_of(document.Document().Canvases(), [&](const auto &canvas) {
                    return canvas.rootElement == candidate.Value();
                }))
                    return candidate;
            }
            return Result<UiElementId>::Failure(MakeError(UiErrors::CapacityExceeded));
        }

        /** @brief Routes detached hierarchy/search gestures to the canonical owner after its session fence is checked. */
        Result<void> ApplyHierarchy(UiCanvasDocument &owner, UiCanvasWorkspaceDocument &projection,
                                    const EditorWorkspaceViewCommandData &command) {
            using enum EditorWorkspaceViewCommand;
            switch (command.command) {
                case UiCanvasSelect:
                    return owner.Select(command.uiCanvasState, command.uiCanvasElement);
                case UiCanvasCreate: {
                    auto id = NewElementId(owner);
                    if (id.HasError())
                        return Result<void>::Failure(id.ErrorValue());
                    return owner.Execute(command.uiCanvasState,
                                         UiCanvasCreateElement{id.Value(), command.uiCanvasElement, {}, command.uiCanvasKind});
                }
                case UiCanvasMove:
                    return command.uiCanvasMove ? owner.Execute(command.uiCanvasState, *command.uiCanvasMove)
                                                : Result<void>::Failure(MakeError(UiCanvasDocumentErrors::InvalidDocument));
                case UiCanvasSearch: {
                    auto rows = owner.Hierarchy(command.stringPayload.value_or(""));
                    if (rows.HasError())
                        return Result<void>::Failure(rows.ErrorValue());
                    projection.search = command.stringPayload.value_or("");
                    projection.rows = std::move(rows).Value();
                    return Result<void>::Success();
                }
                case UiCanvasUndo:
                    return owner.Undo(command.uiCanvasState);
                case UiCanvasRedo:
                    return owner.Redo(command.uiCanvasState);
                default:
                    return Result<void>::Failure(MakeError(UiCanvasDocumentErrors::InvalidDocument));
            }
        }

        /** @brief Refresh cached authored rows and selected values only after their canonical fences change. */
        void RefreshDocumentProjection(const UiCanvasDocument &owner, UiCanvasWorkspaceDocument &projection) {
            const bool sourceChanged = projection.state != owner.CurrentState();
            const bool selectionChanged = projection.selection != owner.Selection();
            if (selectionChanged && projection.selectionRevision != std::numeric_limits<std::uint64_t>::max())
                ++projection.selectionRevision;
            if (sourceChanged || selectionChanged) {
                const auto elements = owner.Document().Elements();
                const auto selected = std::ranges::find(elements, owner.Selection(), &Runtime::Ui::UiDocumentElement::id);
                projection.selectedElement = selected == elements.end() ? std::nullopt : std::optional{*selected};
                projection.documentRevision = owner.Document().Revision();
            }
            if (sourceChanged) {
                if (auto rows = owner.Hierarchy(projection.search); rows.HasValue())
                    projection.rows = std::move(rows).Value();
                else {
                    projection.rows.clear();
                    projection.diagnostic = rows.ErrorValue();
                }
                projection.state = owner.CurrentState();
            }
        }
    }  // namespace

    /** @copydoc EditorWorkspaceController::UpdateUiCanvasResources */
    void EditorWorkspaceController::UpdateUiCanvasResources() {
        RefreshUiCanvasProjection();
    }

    /** @copydoc EditorWorkspaceController::RefreshUiCanvasProjection */
    void EditorWorkspaceController::RefreshUiCanvasProjection() {
        auto &projections = m_viewModel.uiCanvasDocuments;
        const auto refreshed = m_uiInspectorResources ? m_uiInspectorResources->Refresh() : Result<void>::Success();
        const auto resources = m_uiInspectorResources ? m_uiInspectorResources->Capture() : UiInspectorResourceSnapshot{};
        std::erase_if(projections, [this](const auto &projection) {
            return std::ranges::none_of(m_uiCanvasDocuments, [&](const auto &owner) {
                return owner.Identity().instance == projection.identity.instance;
            });
        });
        for (const auto &owner : m_uiCanvasDocuments) {
            if (owner.IsClosed())
                continue;
            auto projection = std::ranges::find_if(projections, [&](const auto &entry) {
                return entry.identity.instance == owner.Identity().instance;
            });
            if (projection == projections.end()) {
                projections.push_back({.identity = owner.Identity()});
                projection = projections.end() - 1;
            }
            RefreshDocumentProjection(owner, *projection);
            projection->inspectorAssets = resources.assets.Revision();
            projection->inspectorStyles = resources.StyleGeneration();
            projection->inspectorResourcePublication = resources.publication;
            projection->inspectorThemes = resources.themes;
            projection->inspectorTheme = resources.theme;
            projection->inspectorStylePending = resources.pending;
            if (refreshed.HasError())
                projection->diagnostic = refreshed.ErrorValue();
            projection->root = owner.Document().Canvases().front().rootElement;
            projection->selection = owner.Selection();
            projection->canUndo = owner.CanUndo();
            projection->canRedo = owner.CanRedo();
            projection->dirty = owner.IsDirty();
            static_cast<void>(m_viewModel.workspacePanelHost.SetDocumentDirty(owner.Identity().instance, owner.IsDirty()));
        }
    }

    /** @copydoc EditorWorkspaceController::SaveUiCanvasSource */
    Result<void> EditorWorkspaceController::SaveUiCanvasSource(UiCanvasDocument &owner) const {
        if (!m_mutations)
            return Result<void>::Failure(MakeError(UiCanvasDocumentErrors::WriteFailed));
        NativeDurableFileSystem files;
        const auto saved = owner.Save(m_viewModel.projectRoot, *m_mutations, files);
        if (saved.HasError())
            return Result<void>::Failure(saved.ErrorValue());
        if (saved.Value().status != UiCanvasDocumentSaveStatus::Saved)
            return Result<void>::Failure(MakeError(UiCanvasDocumentErrors::Conflict));
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::ProcessUiCanvasCommand */
    bool EditorWorkspaceController::ProcessUiCanvasCommand(const EditorWorkspaceViewCommandData &command) {
        using enum EditorWorkspaceViewCommand;
        if (command.command < UiCanvasSelect || command.command > UiCanvasReload)
            return false;
        if (!command.documentInstance)
            return true;
        const auto owner = std::ranges::find_if(m_uiCanvasDocuments, [&](const auto &entry) {
            return entry.Identity().instance == *command.documentInstance;
        });
        if (owner == m_uiCanvasDocuments.end())
            return true;
        RefreshUiCanvasProjection();
        const auto projection = std::ranges::find_if(m_viewModel.uiCanvasDocuments, [&](const auto &entry) {
            return entry.identity.instance == *command.documentInstance;
        });
        const auto resources = m_uiInspectorResources ? m_uiInspectorResources->Capture() : UiInspectorResourceSnapshot{};
        Result<void> result = Result<void>::Success();
        if (command.uiCanvasState != owner->CurrentState()) {
            result = Result<void>::Failure(MakeError(UiCanvasDocumentErrors::Conflict));
        } else if (command.command == UiCanvasSelectTheme) {
            if (!m_uiInspectorResources || command.uiCanvasInspectorAssets != resources.assets.Revision() ||
                command.uiCanvasInspectorResourcePublication != resources.publication)
                result = Result<void>::Failure(MakeError(Runtime::Ui::UiErrors::AssetRegistryStale));
            else
                result = m_uiInspectorResources->SelectTheme(command.uiCanvasTheme);
        } else if (command.command == UiCanvasApplyPresentation) {
            if (!command.uiCanvasPresentation || projection->selectionRevision == std::numeric_limits<std::uint64_t>::max())
                result = Result<void>::Failure(MakeError(UiCanvasDocumentErrors::InvalidDocument));
            else
                result = ExecuteUiInspectorPresentation(*owner,
                                                        {.document = *command.documentInstance,
                                                         .state = command.uiCanvasState,
                                                         .revision = command.uiCanvasDocumentRevision,
                                                         .element = command.uiCanvasElement,
                                                         .selectionRevision = command.uiCanvasSelectionRevision,
                                                         .styles = command.uiCanvasInspectorStyles,
                                                         .assets = command.uiCanvasInspectorAssets,
                                                         .resourcePublication = command.uiCanvasInspectorResourcePublication},
                                                        projection->selectionRevision, *command.uiCanvasPresentation, &resources);
        } else if (command.command == UiCanvasSave) {
            result = SaveUiCanvasSource(*owner);
        } else if (command.command == UiCanvasReload) {
            result = owner->Reload();
        } else {
            result = ApplyHierarchy(*owner, *projection, command);
        }
        projection->diagnostic = result.HasError() ? std::optional{result.ErrorValue()} : std::nullopt;
        RefreshUiCanvasProjection();
        return true;
    }
}  // namespace Horo::Editor
