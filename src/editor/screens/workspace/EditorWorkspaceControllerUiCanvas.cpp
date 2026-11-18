#include "Horo/Foundation/Platform.h"
#include "UiInspectorDraft.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <array>
#include <cmath>
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

        /** @brief Publish geometry and failure evidence from the same evaluation. */
        void RefreshCanvasPreview(const UiCanvasDocument &owner, UiCanvasPreviewPresentation &preview,
                                  const UiInspectorResourceSnapshot &resources) {
            auto evaluated = BuildUiCanvasPreview(owner.Document(), preview.state, owner.Identity().instance, &resources, &preview.problem);
            preview.diagnostic = evaluated.HasError() ? std::optional{evaluated.ErrorValue()} : std::nullopt;
            preview.layout = evaluated.HasValue() ? std::optional{std::move(evaluated).Value()} : std::nullopt;
        }

        /** @brief Refresh cached authored rows and selected values only after their canonical fences change. */
        void RefreshDocumentProjection(const UiCanvasDocument &owner, UiCanvasWorkspaceDocument &projection,
                                       const UiInspectorResourceSnapshot &resources) {
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
            if (sourceChanged || projection.inspectorResourcePublication != resources.publication ||
                projection.inspectorStyles != resources.StyleGeneration() || projection.inspectorTheme != resources.theme)
                RefreshCanvasPreview(owner, projection.preview, resources);
        }

        /** @brief Change only session device state, resolving actual source inputs when geometry changes. */
        Result<void> UpdateCanvasPreview(const UiCanvasDocument &owner, UiCanvasWorkspaceDocument &projection,
                                         const EditorWorkspaceViewCommandData &command, const UiInspectorResourceSnapshot &resources) {
            if (!command.uiCanvasPreviewState || !command.uiCanvasPreviewState->IsValid())
                return Result<void>::Failure(MakeError(UiCanvasDocumentErrors::InvalidDocument));
            const bool resolve = projection.preview.state.device != command.uiCanvasPreviewState->device ||
                                 projection.preview.state.rotated != command.uiCanvasPreviewState->rotated;
            projection.preview.state = *command.uiCanvasPreviewState;
            if (resolve)
                RefreshCanvasPreview(owner, projection.preview, resources);
            return Result<void>::Success();
        }

        /** @brief Resolve the actual selected layout and submit one fenced canonical transaction. */
        Result<void> CommitCanvasLayout(UiCanvasDocument &owner, const UiCanvasWorkspaceDocument &projection,
                                        const EditorWorkspaceViewCommandData &command) {
            if (!projection.preview.layout || !projection.selectedElement || command.uiCanvasElement != owner.Selection())
                return Result<void>::Failure(MakeError(UiCanvasDocumentErrors::Conflict));
            const auto &boxes = projection.preview.layout->boxes;
            const auto box = std::ranges::find(boxes, owner.Selection(), &UiCanvasPreviewBox::element);
            if (box == boxes.end())
                return Result<void>::Failure(MakeError(UiCanvasDocumentErrors::Conflict));
            const auto parent = std::ranges::find(boxes, projection.selectedElement->parent, &UiCanvasPreviewBox::element);
            const auto edit = PrepareUiCanvasLayoutGesture(*projection.selectedElement, box->bounds, command.uiCanvasLayoutDelta,
                                                           parent == boxes.end() ? Runtime::Ui::UiLogicalExtent{} : parent->bounds.extent,
                                                           projection.preview.state);
            return edit.HasError() ? Result<void>::Failure(edit.ErrorValue())
                                   : ExecuteUiCanvasLayoutGesture(owner, command.uiCanvasState, command.uiCanvasSelectionRevision,
                                                                  projection.selectionRevision, edit.Value());
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
            RefreshDocumentProjection(owner, *projection, resources);
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
    Result<void> EditorWorkspaceController::SaveUiCanvasSource(UiCanvasDocument &owner) {
        if (!m_mutations)
            return Result<void>::Failure(MakeError(UiCanvasDocumentErrors::WriteFailed));
        if (auto drained = DrainUiCanvasRecovery(); drained.HasError())
            return drained;
        NativeDurableFileSystem fallback;
        auto &files = m_durableFiles ? *m_durableFiles : fallback;
        const auto saved = owner.Save(ProjectRoot(), *m_mutations, files);
        if (saved.HasError())
            return Result<void>::Failure(saved.ErrorValue());
        if (saved.Value().status != UiCanvasDocumentSaveStatus::Saved)
            return Result<void>::Failure(MakeError(UiCanvasDocumentErrors::Conflict));
        DocumentRecoveryService recovery(ProjectRoot(), *m_mutations, files);
        return owner.DiscardRecovery(m_workspaceSession, recovery);
    }

    /** @copydoc EditorWorkspaceController::InspectUiCanvasRecovery */
    void EditorWorkspaceController::InspectUiCanvasRecovery(const UiCanvasDocument &owner, UiCanvasWorkspaceDocument &view) {
        view.recovery.reset();
        if (!m_mutations || !m_durableFiles)
            return;
        DocumentRecoveryService recovery(ProjectRoot(), *m_mutations, *m_durableFiles);
        auto record = owner.InspectRecovery(recovery);
        if (record.HasError())
            view.diagnostic = record.ErrorValue();
        else if (record.Value() && (record.Value()->sessionOwner != m_workspaceSession ||
                                    record.Value()->sessionGeneration != owner.Identity().instance.Value()))
            view.recovery = std::make_shared<const DocumentRecoveryRecord>(std::move(*record.Value()));
    }

    /** @copydoc EditorWorkspaceController::ReportUiCanvasRecoveryError */
    void EditorWorkspaceController::ReportUiCanvasRecoveryError(const DocumentInstanceId instance, const Error &error) {
        const auto projection = std::ranges::find_if(m_viewModel.uiCanvasDocuments, [instance](const auto &entry) {
            return entry.identity.instance == instance;
        });
        if (projection != m_viewModel.uiCanvasDocuments.end())
            projection->diagnostic = error;
    }

    /** @copydoc EditorWorkspaceController::ObserveUiCanvasRecovery */
    bool EditorWorkspaceController::ObserveUiCanvasRecovery() {
        if (!m_uiCanvasRecovery)
            return true;
        const auto snapshot = m_uiCanvasRecovery->job.Snapshot();
        if (!snapshot || !snapshot->terminalResult)
            return false;
        auto checkpoint = std::move(m_uiCanvasRecovery->checkpoint);
        m_uiCanvasRecovery.reset();
        if (snapshot->state == JobState::Succeeded) {
            const auto canvas = std::ranges::find_if(m_uiCanvasDocuments, [&](const auto &entry) {
                return entry.Identity().instance == checkpoint->state.instance;
            });
            if (canvas != m_uiCanvasDocuments.end())
                canvas->AcknowledgeRecovery(*checkpoint);
        } else if (snapshot->state != JobState::Cancelled && snapshot->terminalResult->error) {
            ReportUiCanvasRecoveryError(checkpoint->state.instance, *snapshot->terminalResult->error);
        }
        return true;
    }

    /** @copydoc EditorWorkspaceController::DrainUiCanvasRecovery */
    Result<void> EditorWorkspaceController::DrainUiCanvasRecovery(const std::optional<DocumentInstanceId> instance) {
        if (!m_uiCanvasRecovery || (instance && m_uiCanvasRecovery->checkpoint->state.instance != *instance))
            return Result<void>::Success();
        static_cast<void>(m_uiCanvasRecovery->job.RequestCancel());
        const auto joined = m_uiCanvasRecovery->job.Wait();
        if (!ObserveUiCanvasRecovery())
            return joined;
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::ScheduleUiCanvasRecovery */
    void EditorWorkspaceController::ScheduleUiCanvasRecovery(UiCanvasDocument &canvas) {
        auto captured = canvas.CaptureRecovery(m_workspaceSession);
        if (captured.HasError()) {
            ReportUiCanvasRecoveryError(canvas.Identity().instance, captured.ErrorValue());
            return;
        }
        if (!captured.Value())
            return;
        auto checkpoint = std::make_shared<const UiCanvasRecoveryCheckpoint>(std::move(captured).Value().value());
        auto submitted = m_sourceRecoveryJobs->SubmitResult({.priority = JobPriority::Background, .resource = JobResource::Io},
                                                            [root = std::filesystem::path(ProjectRoot()), mutations = m_mutations,
                                                             files = m_durableFiles, checkpoint](const CancellationToken &cancellation) {
            DocumentRecoveryService recovery(root, *mutations, *files);
            return WriteUiCanvasRecovery(*checkpoint, recovery, cancellation);
        });
        if (submitted.HasError()) {
            ReportUiCanvasRecoveryError(canvas.Identity().instance, submitted.ErrorValue());
            return;
        }
        m_uiCanvasRecovery.emplace(CanvasRecoveryWork{std::move(submitted).Value(), std::move(checkpoint)});
    }

    /** @copydoc EditorWorkspaceController::UpdateUiCanvasAutosave */
    void EditorWorkspaceController::UpdateUiCanvasAutosave(const float seconds, const int minutes) {
        if (!ObserveUiCanvasRecovery() || !m_mutations || !m_durableFiles || !m_sourceRecoveryJobs || minutes <= 0 || seconds < 0 ||
            !std::isfinite(seconds))
            return;
        m_uiCanvasAutosaveSeconds += seconds;
        if (m_uiCanvasRecoveryQueue.empty() && m_uiCanvasAutosaveSeconds >= static_cast<float>(minutes) * 60) {
            m_uiCanvasAutosaveSeconds = 0;
            for (const auto &canvas : m_uiCanvasDocuments)
                if (canvas.IsDirty())
                    m_uiCanvasRecoveryQueue.push_back(canvas.Identity().instance);
        }
        if (!m_uiCanvasRecoveryQueue.empty() && !m_uiCanvasRecovery) {
            const auto instance = m_uiCanvasRecoveryQueue.back();
            m_uiCanvasRecoveryQueue.pop_back();
            const auto canvas = std::ranges::find_if(m_uiCanvasDocuments, [instance](const auto &entry) {
                return entry.Identity().instance == instance;
            });
            if (canvas != m_uiCanvasDocuments.end())
                ScheduleUiCanvasRecovery(*canvas);
        }
    }

    /** @copydoc EditorWorkspaceController::ExecuteUiCanvasCommand */
    Result<void> EditorWorkspaceController::ExecuteUiCanvasCommand(UiCanvasDocument &owner, UiCanvasWorkspaceDocument &projection,
                                                                   const EditorWorkspaceViewCommandData &command,
                                                                   const UiInspectorResourceSnapshot &resources) {
        using enum EditorWorkspaceViewCommand;
        Result<void> result = Result<void>::Success();
        if (command.uiCanvasState != owner.CurrentState()) {
            result = Result<void>::Failure(MakeError(UiCanvasDocumentErrors::Conflict));
        } else if (command.command == UiCanvasSelectTheme) {
            if (!m_uiInspectorResources || command.uiCanvasInspectorAssets != resources.assets.Revision() ||
                command.uiCanvasInspectorResourcePublication != resources.publication)
                result = Result<void>::Failure(MakeError(Runtime::Ui::UiErrors::AssetRegistryStale));
            else
                result = m_uiInspectorResources->SelectTheme(command.uiCanvasTheme);
        } else if (command.command == UiCanvasUpdatePreview) {
            result = UpdateCanvasPreview(owner, projection, command, resources);
        } else if (command.command == UiCanvasCommitLayout) {
            result = CommitCanvasLayout(owner, projection, command);
        } else if (command.command == UiCanvasApplyPresentation) {
            if (!command.uiCanvasPresentation || projection.selectionRevision == std::numeric_limits<std::uint64_t>::max())
                result = Result<void>::Failure(MakeError(UiCanvasDocumentErrors::InvalidDocument));
            else
                result = ExecuteUiInspectorPresentation(owner,
                                                        {.document = *command.documentInstance,
                                                         .state = command.uiCanvasState,
                                                         .revision = command.uiCanvasDocumentRevision,
                                                         .element = command.uiCanvasElement,
                                                         .selectionRevision = command.uiCanvasSelectionRevision,
                                                         .styles = command.uiCanvasInspectorStyles,
                                                         .assets = command.uiCanvasInspectorAssets,
                                                         .resourcePublication = command.uiCanvasInspectorResourcePublication},
                                                        projection.selectionRevision, *command.uiCanvasPresentation, &resources);
        } else if (command.command == UiCanvasSave) {
            result = SaveUiCanvasSource(owner);
        } else if (command.command == UiCanvasReload) {
            if (auto drained = DrainUiCanvasRecovery(owner.Identity().instance); drained.HasError())
                return drained;
            result = owner.Reload();
        } else if (command.command == UiCanvasRecover || command.command == UiCanvasDiscardRecovery) {
            if (!m_mutations || !m_durableFiles || !command.uiCanvasRecovery)
                return Result<void>::Failure(MakeError(UiCanvasDocumentErrors::Conflict));
            if (auto drained = DrainUiCanvasRecovery(owner.Identity().instance); drained.HasError())
                return drained;
            DocumentRecoveryService recovery(ProjectRoot(), *m_mutations, *m_durableFiles);
            result = command.command == UiCanvasRecover
                         ? owner.Recover(command.uiCanvasState, *command.uiCanvasRecovery, recovery)
                         : owner.DiscardRecovery(m_workspaceSession, recovery, command.uiCanvasRecovery.get());
            if (result.HasValue())
                projection.recovery.reset();
        } else {
            result = ApplyHierarchy(owner, projection, command);
        }
        return result;
    }

    /** @copydoc EditorWorkspaceController::ProcessUiCanvasCommand */
    bool EditorWorkspaceController::ProcessUiCanvasCommand(const EditorWorkspaceViewCommandData &command) {
        using enum EditorWorkspaceViewCommand;
        if (command.command < UiCanvasSelect || command.command > UiCanvasDiscardRecovery)
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
        const auto result = ExecuteUiCanvasCommand(*owner, *projection, command, resources);
        projection->diagnostic = result.HasError() ? std::optional{result.ErrorValue()} : std::nullopt;
        RefreshUiCanvasProjection();
        return true;
    }
}  // namespace Horo::Editor
