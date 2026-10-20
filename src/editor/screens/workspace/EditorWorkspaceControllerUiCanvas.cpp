#include "Horo/Foundation/Platform.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <array>

namespace Horo::Editor {
    namespace {
        /** @brief Derives a stable candidate from the exact document revision and owner state, probing bounded authored collisions. */
        Result<Runtime::Ui::UiElementId> NewElementId(const UiCanvasDocument &document) {
            using namespace Runtime::Ui;
            for (std::size_t probe = 0; probe <= MaximumUiDocumentElements; ++probe) {
                const auto seed = Assets::AssetId::FromBytes(document.Document().Id().Bytes()).ToString() + ":" +
                                  std::to_string(document.Document().Revision().Value()) + ":" +
                                  std::to_string(document.CurrentState().value) + ":" + std::to_string(probe);
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
    }  // namespace

    /** @copydoc EditorWorkspaceController::RefreshUiCanvasProjection */
    void EditorWorkspaceController::RefreshUiCanvasProjection() {
        auto &projections = m_viewModel.uiCanvasDocuments;
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
            if (projection->state != owner.CurrentState()) {
                auto rows = owner.Hierarchy(projection->search);
                if (rows.HasValue())
                    projection->rows = std::move(rows).Value();
                else {
                    projection->rows.clear();
                    projection->diagnostic = rows.ErrorValue();
                }
                projection->state = owner.CurrentState();
            }
            projection->root = owner.Document().Canvases().front().rootElement;
            projection->selection = owner.Selection();
            projection->canUndo = owner.CanUndo();
            projection->canRedo = owner.CanRedo();
            projection->dirty = owner.IsDirty();
            static_cast<void>(m_viewModel.workspacePanelHost.SetDocumentDirty(owner.Identity().instance, owner.IsDirty()));
        }
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
        Result<void> result = Result<void>::Success();
        if (command.uiCanvasState != owner->CurrentState()) {
            result = Result<void>::Failure(MakeError(UiCanvasDocumentErrors::Conflict));
        } else {
            switch (command.command) {
                case UiCanvasSelect:
                    result = owner->Select(command.uiCanvasState, command.uiCanvasElement);
                    break;
                case UiCanvasCreate: {
                    auto id = NewElementId(*owner);
                    if (id.HasError())
                        result = Result<void>::Failure(id.ErrorValue());
                    else
                        result = owner->Execute(command.uiCanvasState,
                                                UiCanvasCreateElement{id.Value(), command.uiCanvasElement, {}, command.uiCanvasKind});
                    break;
                }
                case UiCanvasMove:
                    result = command.uiCanvasMove ? owner->Execute(command.uiCanvasState, *command.uiCanvasMove)
                                                  : Result<void>::Failure(MakeError(UiCanvasDocumentErrors::InvalidDocument));
                    break;
                case UiCanvasSearch: {
                    auto rows = owner->Hierarchy(command.stringPayload.value_or(""));
                    if (rows.HasError())
                        result = Result<void>::Failure(rows.ErrorValue());
                    else {
                        projection->search = command.stringPayload.value_or("");
                        projection->rows = std::move(rows).Value();
                    }
                    break;
                }
                case UiCanvasUndo:
                    result = owner->Undo(command.uiCanvasState);
                    break;
                case UiCanvasRedo:
                    result = owner->Redo(command.uiCanvasState);
                    break;
                case UiCanvasReload:
                    result = owner->Reload();
                    break;
                case UiCanvasSave: {
                    if (!m_mutations) {
                        result = Result<void>::Failure(MakeError(UiCanvasDocumentErrors::WriteFailed));
                        break;
                    }
                    NativeDurableFileSystem files;
                    const auto saved = owner->Save(m_viewModel.projectRoot, *m_mutations, files);
                    if (saved.HasError())
                        result = Result<void>::Failure(saved.ErrorValue());
                    else if (saved.Value().status != UiCanvasDocumentSaveStatus::Saved)
                        result = Result<void>::Failure(MakeError(UiCanvasDocumentErrors::Conflict));
                    break;
                }
                default:
                    break;
            }
        }
        projection->diagnostic = result.HasError() ? std::optional{result.ErrorValue()} : std::nullopt;
        RefreshUiCanvasProjection();
        return true;
    }
}  // namespace Horo::Editor
