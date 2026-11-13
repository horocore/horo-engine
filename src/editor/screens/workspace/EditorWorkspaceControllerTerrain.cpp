#include "EditorWorkspaceController.h"
#include "Horo/Editor/SourceDocumentService.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "terrain/TerrainDocumentHost.h"
#include "terrain/TerrainToolSelection.h"
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
            const double x = (source.width - 1) * source.coordinates.spacingX;
            const double z = (source.height - 1) * source.coordinates.spacingZ;
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

    /** @brief Admit the existing real importer and project mutation capability before focusing or opening a source. */
    Result<void> EditorWorkspaceController::AdmitTerrainOpen() const {
        if (!m_mutations || !m_importerCatalog || !m_importerCatalog->FindById(TerrainRawImporterId) ||
            m_viewModel.playState != EditorPlayState::Idle)
            return Result<void>::Failure(MakeError(TerrainEditErrors::CapabilityUnavailable));
        return Result<void>::Success();
    }

    /** @brief Focus an already owned document without issuing another source session. */
    bool EditorWorkspaceController::FocusTerrainDocument(std::string_view path) {
        for (const auto &document : m_terrainDocuments)
            if (document->Identity().key.source.Value() == path) {
                static_cast<void>(m_viewModel.workspacePanelHost.FocusDocument(document->Identity().instance));
                RefreshTerrainDocuments();
                return true;
            }
        return false;
    }

    /** @brief Prepare the source owner, transient selection and tab before publishing the accepted document. */
    Result<void> EditorWorkspaceController::OpenTerrainDocument(Assets::AssetId asset, const std::filesystem::path &absolute,
                                                                const std::string &path) {
        m_terrainDocuments.reserve(4);
        m_terrainToolSelections.reserve(4);
        m_viewModel.terrainDocuments.reserve(4);
        auto source = SourceDocumentId::Parse(path);
        if (source.HasError())
            return Result<void>::Failure(source.ErrorValue());
        const DocumentOpenKey key{DocumentKind::Asset, source.Value()};
        auto identity = m_documentRegistry.Open(key);
        if (identity.HasError())
            return Result<void>::Failure(identity.ErrorValue());

        struct OpenIdentity final {
            DocumentIdentityRegistry &registry;
            DocumentInstanceId instance;
            bool owned;

            OpenIdentity(DocumentIdentityRegistry &owner, DocumentInstanceId value, bool owns) noexcept
                : registry(owner), instance(value), owned(owns) {}

            OpenIdentity(const OpenIdentity &) = delete;
            OpenIdentity &operator=(const OpenIdentity &) = delete;

            ~OpenIdentity() {
                if (owned)
                    static_cast<void>(registry.Close(instance));
            }
        } admission{m_documentRegistry, identity.Value().identity.instance,
                    identity.Value().disposition == DocumentOpenDisposition::Opened};

        auto opened = TerrainDocumentHost::Open(m_sourceOpenService.ProjectRoot(), absolute, asset, identity.Value().identity,
                                                m_workspaceSession, *m_mutations, m_assetRegistry);
        if (opened.HasError())
            return Result<void>::Failure(opened.ErrorValue());

        struct PreparedOwner final {
            TerrainDocumentHost &document;
            bool accepted{};

            explicit PreparedOwner(TerrainDocumentHost &value) noexcept : document(value) {}

            PreparedOwner(const PreparedOwner &) = delete;
            PreparedOwner &operator=(const PreparedOwner &) = delete;

            ~PreparedOwner() {
                if (!accepted)
                    static_cast<void>(document.Close());
            }
        } prepared{*opened.Value()};

        auto camera = m_viewport.Current().camera;
        if (auto framed = FrameSource(camera, *opened.Value()->BorrowSource()); framed.HasError())
            return Result<void>::Failure(framed.ErrorValue());
        auto selection = PrepareTerrainSelection(*opened.Value());
        if (selection.HasError())
            return Result<void>::Failure(selection.ErrorValue());
        if (auto tab = m_viewModel.workspacePanelHost.OpenDocument(key); tab.HasError())
            return Result<void>::Failure(tab.ErrorValue());
        m_terrainToolSelections.emplace_back(identity.Value().identity.instance, std::move(selection).Value());
        m_terrainDocuments.push_back(std::move(opened).Value());
        prepared.accepted = true;
        admission.owned = false;
        m_viewModel.terrainDocumentError.reset();
        RefreshTerrainDocuments();
        return Result<void>::Success();
    }

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
        if (auto admitted = AdmitTerrainOpen(); admitted.HasError()) {
            fail(admitted.ErrorValue());
            return true;
        }
        if (FocusTerrainDocument(path))
            return true;
        if (m_terrainDocuments.size() >= 4) {
            fail(MakeError(TerrainEditErrors::LimitExceeded));
            return true;
        }
        try {
            if (auto opened = OpenTerrainDocument(asset->id, root / relative, path); opened.HasError())
                fail(opened.ErrorValue());
        } catch (const std::bad_alloc &) {
            fail(MakeError(TerrainEditErrors::LimitExceeded));
        }
        return true;
    }

    /** @brief Refresh non-owning selection and revoke stale cook adoption before source projections are consumed. */
    void EditorWorkspaceController::ObserveTerrainDocument(const TerrainDocumentHost &document) {
        const auto view = document.View();
        if (const auto state = TerrainSelection(view.instance))
            if (const auto *source = document.BorrowDocument())
                state->selection.Observe(*source);
        if (m_terrainCookDocument == view.instance && m_terrainCookAdoption && !TerrainCookFenceMatches())
            CancelTerrainCook(view.instance);
        static_cast<void>(m_viewModel.workspacePanelHost.SetDocumentDirty(view.instance, view.dirty));
    }

    /** @brief Enter isolated asset framing while preserving the prior Scene camera for focus restoration. */
    void EditorWorkspaceController::ActivateTerrainCamera(const TerrainDocumentHost &document) {
        if (!m_terrainPriorCamera)
            m_terrainPriorCamera = m_viewport.Current().camera;
        const auto *source = document.BorrowSource();
        if (!source)
            return;
        auto camera = m_viewport.Current().camera;
        if (const auto framed = FrameSource(camera, *source); framed.HasValue())
            static_cast<void>(m_viewport.SetCamera(camera));
        else
            Failure(m_viewModel, framed.ErrorValue());
    }

    /** @brief Publish focus-dependent camera and retained palette projections after observing all open documents. */
    void EditorWorkspaceController::PublishTerrainFocus(std::optional<DocumentInstanceId> terrain, bool wasTerrain) {
        if (!terrain && m_activeTerrainDocument && m_terrainPriorCamera) {
            static_cast<void>(m_viewport.SetCamera(*m_terrainPriorCamera));
            m_terrainPriorCamera.reset();
        }
        m_activeTerrainDocument = terrain;
        if (m_viewModel.terrainPaletteDocument && m_viewModel.terrainPaletteDocument->View().closed)
            m_viewModel.terrainPaletteDocument.reset();
        m_viewModel.terrainToolSelection =
            m_viewModel.terrainPaletteDocument ? TerrainSelection(m_viewModel.terrainPaletteDocument->Identity().instance) : nullptr;
        if (terrain || wasTerrain)
            m_viewModel.viewportCamera = m_viewport.Current().camera;
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
            ObserveTerrainDocument(*document);
            if (active != view.instance)
                continue;
            m_viewModel.terrainPaletteDocument = document;
            terrain = view.instance;
            if (m_activeTerrainDocument != terrain)
                ActivateTerrainCamera(*document);
        }
        PublishTerrainFocus(terrain, wasTerrain);
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
        if (auto closed = (*found)->Close(); closed.HasError())
            return closed;
        CancelTerrainCook(instance);
        static_cast<void>(m_viewModel.workspacePanelHost.CloseDocument(instance, WorkspaceDocumentClosePolicy::DiscardChanges));
        static_cast<void>(m_documentRegistry.Close(instance));
        if (auto state = TerrainSelection(instance)) {
            state->selection.Clear();
            state->palette.Close();
        }
        std::erase_if(m_terrainToolSelections, [instance](const auto &entry) {
            return entry.first == instance;
        });
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

    /** @brief Handle generic close routing only for a source owner belonging to this terrain feature. */
    bool EditorWorkspaceController::TryCloseTerrainCommand(const EditorWorkspaceViewCommandData &command) {
        if (command.command != EditorWorkspaceViewCommand::CloseWorkspaceDocument || !command.documentInstance)
            return false;
        if (const auto found = std::ranges::find_if(m_terrainDocuments,
                                                    [&](const auto &document) {
            return document->Identity().instance == *command.documentInstance;
        });
            found == m_terrainDocuments.end())
            return false;
        if (auto closed = CloseTerrainDocument(*command.documentInstance, false); closed.HasError())
            Failure(m_viewModel, closed.ErrorValue());
        return true;
    }

    /** @brief Execute one exact source intent and publish its original typed feedback. */
    void EditorWorkspaceController::ProcessTerrainToolIntent(const TerrainToolCommand &intent) {
        auto target = TerrainTarget(intent);
        if (target.HasError()) {
            Failure(m_viewModel, target.ErrorValue());
            return;
        }
        m_viewModel.terrainDocumentError.reset();
        try {
            if (auto result = ExecuteTerrainTool(*target.Value(), intent); result.HasError())
                Failure(m_viewModel, result.ErrorValue());
        } catch (const std::bad_alloc &) {
            Failure(m_viewModel, MakeError(TerrainEditErrors::LimitExceeded));
        }
        RefreshTerrainDocuments();
    }

    /** @copydoc EditorWorkspaceController::ProcessTerrainDocumentCommand */
    bool EditorWorkspaceController::ProcessTerrainDocumentCommand(const EditorWorkspaceViewCommandData &command) {
        using enum EditorWorkspaceViewCommand;
        if (TryCloseTerrainCommand(command))
            return true;
        if (command.command == SaveAllSourceDocuments) {
            if (const auto saved = SaveSourceDocuments(); saved.HasError())
                Failure(m_viewModel, saved.ErrorValue());
            return true;
        }
        if (command.command != TerrainTool)
            return false;
        if (!command.terrainTool) {
            Failure(m_viewModel, MakeError(TerrainEditErrors::Invalid));
            return true;
        }
        ProcessTerrainToolIntent(*command.terrainTool);
        return true;
    }

    /** @copydoc EditorWorkspaceController::ProcessFocusedTerrainCommand */
    void EditorWorkspaceController::ProcessFocusedTerrainCommand(const EditorWorkspaceViewCommandData &command) {
        using enum EditorWorkspaceViewCommand;
        if (command.command == SaveScene || command.command == UndoScene || command.command == RedoScene) {
            if (const auto found = std::ranges::find_if(m_terrainDocuments,
                                                        [&](const auto &document) {
                return document->Identity().instance == m_activeTerrainDocument;
            });
                found != m_terrainDocuments.end()) {
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
