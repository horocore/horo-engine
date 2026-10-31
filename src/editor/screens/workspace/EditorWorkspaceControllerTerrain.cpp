#include "EditorWorkspaceController.h"
#include "Horo/Editor/SourceDocumentService.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "terrain/TerrainDocumentHost.h"
#include "terrain/TerrainWorkspaceTypes.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace Horo::Editor {
    namespace {
        /** @brief Keep typed diagnostics in the model and route original evidence through the existing console. */
        void Failure(EditorWorkspaceViewModel &view, const Error &error) {
            view.terrainDocumentError = error;
            LOG_WARN("editor.terrain", "%s: %s", error.code.Value().c_str(), error.message.c_str());
        }

        /** @brief Establish an explicit isolated asset-preview camera without changing any Scene entity. */
        Result<void> FrameSource(EditorViewportCamera &camera, const Terrain::TerrainCanonicalSource &source) {
            const auto [minimum, maximum] = std::minmax_element(source.heightsMeters.begin(), source.heightsMeters.end());
            const double x = (source.width - 1) * source.coordinates.spacingX, z = (source.height - 1) * source.coordinates.spacingZ;
            const double y = (double(*minimum) + double(*maximum)) * 0.5;
            const double extent = std::max({x, z, double(*maximum) - double(*minimum), 1.0});
            if (!std::isfinite(extent) || extent > 8192 || std::abs(y) > 8192)
                return Result<void>::Failure(MakeError(Terrain::TerrainSourceErrors::PrecisionLost));
            camera.target = {static_cast<float>(x * 0.5), static_cast<float>(y), static_cast<float>(z * 0.5)};
            camera.position =
                camera.target + Math::Vec3{static_cast<float>(extent * 0.7), static_cast<float>(extent), static_cast<float>(extent * 0.7)};
            camera.up = {0, 1, 0};
            camera.nearPlane = 0.05F;
            camera.farPlane = static_cast<float>(extent * 8 + 100);
            camera.orthographicHeight = static_cast<float>(extent * 1.5);
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc EditorWorkspaceController::TryOpenTerrainDocument */
    bool EditorWorkspaceController::TryOpenTerrainDocument(const SourceOpenRequest &request) {
        const auto root = m_sourceOpenService.ProjectRoot();
        const auto relative = request.path.is_absolute() ? request.path.lexically_relative(root) : request.path;
        const auto utf8 = relative.generic_u8string();
        const std::string path{utf8.begin(), utf8.end()};
        const auto *asset = m_assetRegistry.FindByPath(path);
        if (!asset || asset->type.Value() != "core.terrain")
            return false;
        const auto fail = [&](const Error &error) {
            Failure(m_viewModel, error);
            m_viewModel.contentBrowserOperationError = "terrain.tools.unavailable";
        };
        if (!m_mutations || !m_importerCatalog || !m_importerCatalog->FindById(TerrainRawImporterId) ||
            m_viewModel.playState != EditorPlayState::Idle) {
            fail(MakeError(TerrainEditErrors::CapabilityUnavailable));
            return true;
        }
        for (const auto &document : m_terrainDocuments)
            if (document->Identity().key.source.Value() == path) {
                static_cast<void>(m_viewModel.workspacePanelHost.FocusDocument(document->Identity().instance));
                RefreshTerrainDocuments();
                return true;
            }
        if (m_terrainDocuments.size() >= 4) {
            fail(MakeError(TerrainEditErrors::LimitExceeded));
            return true;
        }
        try {
            m_terrainDocuments.reserve(4);
            m_viewModel.terrainDocuments.reserve(4);
            auto source = SourceDocumentId::Parse(path);
            if (source.HasError()) {
                fail(source.ErrorValue());
                return true;
            }
            const DocumentOpenKey key{DocumentKind::Asset, source.Value()};
            auto identity = m_documentRegistry.Open(key);
            if (identity.HasError()) {
                fail(identity.ErrorValue());
                return true;
            }

            struct OpenIdentity final {
                DocumentIdentityRegistry &registry;
                DocumentInstanceId instance;
                bool owned;

                ~OpenIdentity() {
                    if (owned)
                        static_cast<void>(registry.Close(instance));
                }
            } admission{m_documentRegistry, identity.Value().identity.instance,
                        identity.Value().disposition == DocumentOpenDisposition::Opened};

            auto opened =
                TerrainDocumentHost::Open(root, root / relative, asset->id, identity.Value().identity, m_workspaceSession, *m_mutations);
            if (opened.HasError()) {
                fail(opened.ErrorValue());
                return true;
            }
            auto camera = m_viewport.Current().camera;
            const auto framed = FrameSource(camera, *opened.Value()->BorrowSource());
            if (framed.HasError()) {
                static_cast<void>(opened.Value()->Close());
                fail(framed.ErrorValue());
                return true;
            }
            auto tab = m_viewModel.workspacePanelHost.OpenDocument(key);
            if (tab.HasError()) {
                static_cast<void>(opened.Value()->Close());
                fail(tab.ErrorValue());
                return true;
            }
            m_terrainDocuments.push_back(std::move(opened).Value());
            admission.owned = false;
            m_viewModel.terrainDocumentError.reset();
            RefreshTerrainDocuments();
            return true;
        } catch (const std::bad_alloc &) {
            fail(MakeError(TerrainEditErrors::LimitExceeded));
            return true;
        }
    }

    /** @copydoc EditorWorkspaceController::RefreshTerrainDocuments */
    void EditorWorkspaceController::RefreshTerrainDocuments() {
        const bool wasTerrain = m_activeTerrainDocument.has_value();
        auto &model = m_viewModel.terrainDocuments;
        model.clear();
        model.reserve(4);
        const auto active = m_viewModel.workspacePanelHost.ActiveDocument();
        std::optional<DocumentInstanceId> terrain;
        for (const auto &document : m_terrainDocuments) {
            const auto view = document->View();
            if (view.closed)
                continue;
            model.push_back(document);
            static_cast<void>(m_viewModel.workspacePanelHost.SetDocumentDirty(view.instance, view.dirty));
            if (active == view.instance) {
                terrain = view.instance;
                if (m_activeTerrainDocument != terrain) {
                    if (!m_terrainPriorCamera)
                        m_terrainPriorCamera = m_viewport.Current().camera;
                    if (const auto *source = document->BorrowSource()) {
                        auto camera = m_viewport.Current().camera;
                        const auto framed = FrameSource(camera, *source);
                        if (framed.HasValue())
                            static_cast<void>(m_viewport.SetCamera(camera));
                        else
                            Failure(m_viewModel, framed.ErrorValue());
                    }
                }
            }
        }
        if (!terrain && m_activeTerrainDocument && m_terrainPriorCamera) {
            static_cast<void>(m_viewport.SetCamera(*m_terrainPriorCamera));
            m_terrainPriorCamera.reset();
        }
        m_activeTerrainDocument = terrain;
        if (terrain || wasTerrain)
            m_viewModel.viewportCamera = m_viewport.Current().camera;
    }

    /** @copydoc EditorWorkspaceController::TerrainDocumentRevisions */
    std::vector<std::pair<DocumentInstanceId, std::uint64_t>> EditorWorkspaceController::TerrainDocumentRevisions() const {
        std::vector<std::pair<DocumentInstanceId, std::uint64_t>> result;
        result.reserve(m_terrainDocuments.size());
        for (const auto &document : m_terrainDocuments) {
            const auto view = document->View();
            if (!view.closed)
                result.emplace_back(view.instance, view.fence.expectedRevision.Value());
        }
        return result;
    }

    /** @copydoc EditorWorkspaceController::SaveTerrainDocuments */
    Result<void> EditorWorkspaceController::SaveTerrainDocuments() {
        for (const auto &document : m_terrainDocuments) {
            const auto view = document->View();
            if (!view.dirty)
                continue;
            if (!m_mutations || !m_durableFiles)
                return Result<void>::Failure(MakeError(TerrainEditErrors::CapabilityUnavailable));
            auto saved = document->Save(view.fence, *m_mutations, *m_durableFiles);
            if (saved.HasError()) {
                Failure(m_viewModel, saved.ErrorValue());
                return Result<void>::Failure(std::move(saved).ErrorValue());
            }
            if (saved.Value().status != Assets::AssetSourcePublicationStatus::Durable)
                return Result<void>::Failure(saved.Value().durabilityError ? *saved.Value().durabilityError
                                                                           : MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed));
        }
        RefreshTerrainDocuments();
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::CloseTerrainDocument */
    Result<void> EditorWorkspaceController::CloseTerrainDocument(DocumentInstanceId instance, const bool discard) {
        const auto found = std::ranges::find_if(m_terrainDocuments, [&](const auto &document) {
            return document->Identity().instance == instance;
        });
        if (found == m_terrainDocuments.end())
            return Result<void>::Failure(MakeError(WorkspaceDocumentErrors::DocumentUnknown));
        if (!discard && (*found)->View().dirty)
            return Result<void>::Failure(MakeError(WorkspaceDocumentErrors::DirtyDocument));
        auto closed = (*found)->Close();
        if (closed.HasError())
            return closed;
        static_cast<void>(m_viewModel.workspacePanelHost.CloseDocument(instance, WorkspaceDocumentClosePolicy::DiscardChanges));
        static_cast<void>(m_documentRegistry.Close(instance));
        m_terrainDocuments.erase(found);
        RefreshTerrainDocuments();
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::ShutdownTerrainDocuments */
    Result<void> EditorWorkspaceController::ShutdownTerrainDocuments(const bool discard) {
        if (!discard)
            for (const auto &document : m_terrainDocuments)
                if (document->View().dirty)
                    return Result<void>::Failure(MakeError(WorkspaceDocumentErrors::DirtyDocument));
        while (!m_terrainDocuments.empty()) {
            auto closed = CloseTerrainDocument(m_terrainDocuments.back()->Identity().instance, true);
            if (closed.HasError())
                return closed;
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::ProcessTerrainDocumentCommand */
    bool EditorWorkspaceController::ProcessTerrainDocumentCommand(const EditorWorkspaceViewCommandData &command) {
        using enum EditorWorkspaceViewCommand;
        if (command.command == CloseWorkspaceDocument && command.documentInstance) {
            const auto found = std::ranges::find_if(m_terrainDocuments, [&](const auto &document) {
                return document->Identity().instance == *command.documentInstance;
            });
            if (found != m_terrainDocuments.end()) {
                auto closed = CloseTerrainDocument(*command.documentInstance, false);
                if (closed.HasError())
                    Failure(m_viewModel, closed.ErrorValue());
                return true;
            }
        }
        if (command.command == SaveAllSourceDocuments) {
            const auto saved = SaveSourceDocuments();
            if (saved.HasError())
                Failure(m_viewModel, saved.ErrorValue());
            return true;
        }
        if (command.command != TerrainTool)
            return false;
        if (!command.terrainTool) {
            Failure(m_viewModel, MakeError(TerrainEditErrors::Invalid));
            return true;
        }
        const auto &intent = *command.terrainTool;
        const auto found = std::ranges::find_if(m_terrainDocuments, [&](const auto &document) {
            return document->Identity().instance == intent.document.instance;
        });
        if (intent.document.workspace != m_workspaceSession || found == m_terrainDocuments.end() ||
            m_viewModel.workspacePanelHost.ActiveDocument() != intent.document.instance || m_viewModel.playState != EditorPlayState::Idle) {
            Failure(m_viewModel, MakeError(TerrainEditErrors::WrongDocument));
            return true;
        }
        auto &document = **found;
        m_viewModel.terrainDocumentError.reset();
        switch (intent.action) {
            case TerrainToolAction::Stroke: {
                if (m_nextTerrainOperation == std::numeric_limits<std::uint64_t>::max() || intent.stampCount > intent.stamps.size()) {
                    Failure(m_viewModel, MakeError(TerrainEditErrors::LimitExceeded));
                    break;
                }
                const auto operation = TerrainEditOperationId::Create(m_nextTerrainOperation++).Value();
                const auto result =
                    document.Sculpt(intent.document.fence, operation, intent.brush, std::span(intent.stamps.data(), intent.stampCount));
                if (result.HasError())
                    Failure(m_viewModel, result.ErrorValue());
                break;
            }
            case TerrainToolAction::Undo:
            case TerrainToolAction::Redo: {
                const auto result = document.History(intent.document.fence, intent.action == TerrainToolAction::Redo);
                if (result.HasError())
                    Failure(m_viewModel, result.ErrorValue());
                break;
            }
            case TerrainToolAction::Save: {
                if (!m_mutations || !m_durableFiles) {
                    Failure(m_viewModel, MakeError(TerrainEditErrors::CapabilityUnavailable));
                    break;
                }
                const auto result = document.Save(intent.document.fence, *m_mutations, *m_durableFiles);
                if (result.HasError())
                    Failure(m_viewModel, result.ErrorValue());
                else if (result.Value().durabilityError)
                    Failure(m_viewModel, *result.Value().durabilityError);
                break;
            }
            case TerrainToolAction::Close:
            case TerrainToolAction::Discard: {
                const auto view = document.View();
                if (view.fence.expectedRevision != intent.document.fence.expectedRevision ||
                    view.fence.session != intent.document.fence.session) {
                    Failure(m_viewModel, MakeError(TerrainEditErrors::StaleRevision));
                    break;
                }
                const auto result = CloseTerrainDocument(intent.document.instance, intent.action == TerrainToolAction::Discard);
                if (result.HasError())
                    Failure(m_viewModel, result.ErrorValue());
                break;
            }
            case TerrainToolAction::ReportFailure:
                if (intent.failure)
                    Failure(m_viewModel, *intent.failure);
                break;
        }
        RefreshTerrainDocuments();
        return true;
    }

    /** @copydoc EditorWorkspaceController::ProcessFocusedTerrainCommand */
    void EditorWorkspaceController::ProcessFocusedTerrainCommand(const EditorWorkspaceViewCommandData &command) {
        using enum EditorWorkspaceViewCommand;
        if (command.command == SaveScene || command.command == UndoScene || command.command == RedoScene) {
            const auto found = std::ranges::find_if(m_terrainDocuments, [&](const auto &document) {
                return document->Identity().instance == m_activeTerrainDocument;
            });
            if (found != m_terrainDocuments.end()) {
                auto intent = std::make_shared<TerrainToolCommand>();
                intent->document = (*found)->View();
                intent->action = command.command == SaveScene   ? TerrainToolAction::Save
                                 : command.command == UndoScene ? TerrainToolAction::Undo
                                                                : TerrainToolAction::Redo;
                EditorWorkspaceViewCommandData typed;
                typed.command = TerrainTool;
                typed.terrainTool = std::move(intent);
                static_cast<void>(ProcessTerrainDocumentCommand(typed));
            }
            return;
        }
        if (command.command == StopPlay || command.command == PausePlay || command.command == ResumePlay || command.command == StepPlay) {
            static_cast<void>(ProcessPlayCommand(command));
            return;
        }
        if (ProcessViewportCameraCommand(command))
            return;
        if (command.command != None)
            Failure(m_viewModel, MakeError(TerrainEditErrors::CapabilityUnavailable));
    }
}  // namespace Horo::Editor
