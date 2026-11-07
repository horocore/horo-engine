#include "EditorWorkspaceController.h"
#include "terrain/TerrainDocumentHost.h"
#include "terrain/TerrainToolSelection.h"

#include <algorithm>
#include <limits>

namespace Horo::Editor {
    namespace {
        /** @brief Preserve original source errors while projecting a source mutation result into command completion. */
        template <typename T> Result<void> Completion(const Result<T> &result) {
            if (result.HasError())
                return Result<void>::Failure(result.ErrorValue());
            return Result<void>::Success();
        }

        /** @brief Admit the exact current captured type, without substituting a different active palette entry. */
        Result<const FoliagePaletteItem *> CapturedType(const TerrainToolSelection &state, const TerrainToolCommand &intent) {
            if (intent.mode != TerrainToolMode::FoliagePaint && intent.foliage.allTypes)
                return Result<const FoliagePaletteItem *>::Success(nullptr);
            const auto *active = state.palette.Active();
            if (!active || !intent.foliageType)
                return Result<const FoliagePaletteItem *>::Failure(MakeError(TerrainEditErrors::CapabilityUnavailable));
            if (active->source != *intent.foliageType)
                return Result<const FoliagePaletteItem *>::Failure(MakeError(TerrainEditErrors::StaleRevision));
            return Result<const FoliagePaletteItem *>::Success(active);
        }

        /** @brief Distinguish presentation-only palette actions from commands that require the exact focused source document. */
        bool IsPaletteAction(TerrainToolAction action) {
            return action == TerrainToolAction::ChooseFoliage || action == TerrainToolAction::RefreshFoliage;
        }

        /** @brief Compare every typed source admission field separately from workspace routing. */
        bool SameFence(const TerrainEditFence &a, const TerrainEditFence &b) {
            return a.session == b.session && a.dataset == b.dataset && a.expectedRevision == b.expectedRevision &&
                   a.capabilityRevision == b.capabilityRevision;
        }

        /** @brief Reject contradictory tool/channel intents rather than silently choosing another mutation. */
        bool PaintModeMatches(const TerrainToolCommand &intent) {
            switch (intent.mode) {
                case TerrainToolMode::Weights:
                    return intent.paint.mode == TerrainPaintMode::Weights;
                case TerrainToolMode::CarveHole:
                    return intent.paint.mode == TerrainPaintMode::CarveHole;
                case TerrainToolMode::RestoreHole:
                    return intent.paint.mode == TerrainPaintMode::RestoreHole;
                default:
                    return false;
            }
        }

        /** @brief The explicit foliage tool and brush operation must describe one mutation family. */
        bool FoliageModeMatches(const TerrainToolCommand &intent) {
            if (intent.mode == TerrainToolMode::FoliagePaint)
                return intent.foliage.mode == FoliagePaintMode::Paint;
            return intent.mode == TerrainToolMode::FoliageErase && intent.foliage.mode == FoliagePaintMode::Erase;
        }

        /** @brief Issue gesture provenance from existing workspace/session/operation authorities at command admission. */
        std::array<std::byte, 16> GestureKey(Assets::AssetId workspace, TerrainDocumentSessionId session,
                                             TerrainEditOperationId operation) {
            Sha256Builder hash;
            constexpr std::string_view domain{"horo.editor.foliage.gesture.v1"};
            static_cast<void>(hash.Update(std::as_bytes(std::span{domain.data(), domain.size()})));
            static_cast<void>(hash.Update(std::as_bytes(std::span{workspace.Bytes()})));
            std::array<std::byte, 16> numbers{};
            for (std::size_t i = 0; i < 8; ++i) {
                numbers[i] = static_cast<std::byte>(session.Value() >> (i * 8));
                numbers[i + 8] = static_cast<std::byte>(operation.Value() >> (i * 8));
            }
            static_cast<void>(hash.Update(numbers));
            const auto digest = hash.Finalize();
            std::array<std::byte, 16> key{};
            for (std::size_t i = 0; i < key.size(); ++i)
                key[i] = static_cast<std::byte>(digest.bytes[i]);
            return key;
        }
    }  // namespace

    /** @copydoc EditorWorkspaceController::TerrainSelection */
    std::shared_ptr<TerrainToolSelection> EditorWorkspaceController::TerrainSelection(DocumentInstanceId instance) {
        const auto found = std::ranges::find_if(m_terrainToolSelections, [instance](const auto &entry) {
            return entry.first == instance;
        });
        return found == m_terrainToolSelections.end() ? nullptr : found->second;
    }

    /** @copydoc EditorWorkspaceController::PrepareTerrainSelection */
    Result<std::shared_ptr<TerrainToolSelection>> EditorWorkspaceController::PrepareTerrainSelection(const TerrainDocumentHost &document) {
        auto state = std::make_shared<TerrainToolSelection>();
        state->document = document.Identity().instance;
        const auto items = document.BorrowFoliageCatalog();
        auto loaded = state->palette.Replace({items.begin(), items.end()});
        if (loaded.HasError())
            return Result<std::shared_ptr<TerrainToolSelection>>::Failure(loaded.ErrorValue());
        return Result<std::shared_ptr<TerrainToolSelection>>::Success(std::move(state));
    }

    /** @copydoc EditorWorkspaceController::TerrainTarget */
    Result<TerrainDocumentHost *> EditorWorkspaceController::TerrainTarget(const TerrainToolCommand &intent) {
        return TerrainTarget(intent.document, !IsPaletteAction(intent.action));
    }

    /** @copydoc EditorWorkspaceController::TerrainTarget */
    Result<TerrainDocumentHost *> EditorWorkspaceController::TerrainTarget(const TerrainDocumentView &view, bool requireFocus) {
        const auto found = std::ranges::find_if(m_terrainDocuments, [&](const auto &document) {
            return document->Identity().instance == view.instance;
        });
        if (view.workspace != m_workspaceSession || found == m_terrainDocuments.end() || m_viewModel.playState != EditorPlayState::Idle)
            return Result<TerrainDocumentHost *>::Failure(MakeError(TerrainEditErrors::WrongDocument));
        if (requireFocus && m_viewModel.workspacePanelHost.ActiveDocument() != view.instance)
            return Result<TerrainDocumentHost *>::Failure(MakeError(TerrainEditErrors::WrongDocument));
        const auto current = (*found)->View();
        if (current.closed || !SameFence(current.fence, view.fence))
            return Result<TerrainDocumentHost *>::Failure(MakeError(TerrainEditErrors::StaleRevision));
        return Result<TerrainDocumentHost *>::Success(found->get());
    }

    /** @copydoc EditorWorkspaceController::ExecuteTerrainPalette */
    Result<void> EditorWorkspaceController::ExecuteTerrainPalette(TerrainDocumentHost &document, const TerrainToolCommand &intent) {
        auto state = TerrainSelection(intent.document.instance);
        if (!state)
            return Result<void>::Failure(MakeError(TerrainEditErrors::Closed));
        if (intent.action == TerrainToolAction::ChooseFoliage) {
            if (!intent.foliageType)
                return Result<void>::Failure(MakeError(TerrainEditErrors::Invalid));
            const auto found = std::ranges::find_if(state->palette.Items(), [&](const auto &item) {
                return item.source == *intent.foliageType;
            });
            if (found == state->palette.Items().end())
                return Result<void>::Failure(MakeError(TerrainEditErrors::StaleRevision));
            return state->palette.Choose(found->source.type);
        }
        if (!m_mutations)
            return Result<void>::Failure(MakeError(TerrainEditErrors::CapabilityUnavailable));
        if (auto refreshed = document.RefreshResources(m_assetRegistry, *m_mutations); refreshed.HasError())
            return refreshed;
        const auto items = document.BorrowFoliageCatalog();
        return state->palette.Replace({items.begin(), items.end()});
    }

    /** @copydoc EditorWorkspaceController::ExecuteTerrainSelection */
    Result<void> EditorWorkspaceController::ExecuteTerrainSelection(TerrainDocumentHost &document, const TerrainToolCommand &intent) {
        const auto *source = document.BorrowDocument();
        auto state = TerrainSelection(intent.document.instance);
        if (!source || !state)
            return Result<void>::Failure(MakeError(TerrainEditErrors::Closed));
        auto active = CapturedType(*state, intent);
        if (active.HasError())
            return Result<void>::Failure(active.ErrorValue());
        auto selected = SelectFoliageInBrush(*source, intent.document.fence, active.Value(), intent.foliage,
                                             std::span(intent.stamps.data(), intent.stampCount));
        if (selected.HasError())
            return Result<void>::Failure(selected.ErrorValue());
        return state->selection.Replace(*source, intent.document.fence, selected.Value());
    }

    /** @copydoc EditorWorkspaceController::ExecuteTerrainFoliageStroke */
    Result<void> EditorWorkspaceController::ExecuteTerrainFoliageStroke(TerrainDocumentHost &document, const TerrainToolCommand &intent,
                                                                        TerrainEditOperationId operation) {
        if (!FoliageModeMatches(intent))
            return Result<void>::Failure(MakeError(TerrainEditErrors::Invalid));
        const auto *source = document.BorrowDocument();
        auto state = TerrainSelection(intent.document.instance);
        if (!source || !state)
            return Result<void>::Failure(MakeError(TerrainEditErrors::Closed));
        auto active = CapturedType(*state, intent);
        if (active.HasError())
            return Result<void>::Failure(active.ErrorValue());
        auto brush = intent.foliage;
        brush.gestureKey = GestureKey(m_workspaceSession, intent.document.fence.session, operation);
        auto produced = ProduceFoliagePaint(*source, intent.document.fence, operation, active.Value(), brush,
                                            std::span(intent.stamps.data(), intent.stampCount), document.TileCells());
        if (produced.HasError())
            return Result<void>::Failure(produced.ErrorValue());
        if (produced.Value().placements.empty())
            return Result<void>::Success();
        if (intent.mode == TerrainToolMode::FoliagePaint) {
            if (!m_mutations)
                return Result<void>::Failure(MakeError(TerrainEditErrors::CapabilityUnavailable));
            return Completion(document.ExecuteFoliage(intent.document.fence, produced.Value(), *intent.foliageType, *m_mutations));
        }
        return Completion(document.Execute(intent.document.fence, produced.Value()));
    }

    /** @copydoc EditorWorkspaceController::ExecuteTerrainStroke */
    Result<void> EditorWorkspaceController::ExecuteTerrainStroke(TerrainDocumentHost &document, const TerrainToolCommand &intent) {
        if (intent.stampCount > intent.stamps.size())
            return Result<void>::Failure(MakeError(TerrainEditErrors::LimitExceeded));
        if (intent.mode == TerrainToolMode::FoliageSelect)
            return ExecuteTerrainSelection(document, intent);
        if (m_nextTerrainOperation == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(TerrainEditErrors::Exhausted));
        const auto operation = TerrainEditOperationId::Create(m_nextTerrainOperation++).Value();
        const auto stamps = std::span(intent.stamps.data(), intent.stampCount);
        if (intent.mode == TerrainToolMode::Sculpt)
            return Completion(document.Sculpt(intent.document.fence, operation, intent.brush, stamps));
        if (intent.mode <= TerrainToolMode::RestoreHole) {
            if (!PaintModeMatches(intent))
                return Result<void>::Failure(MakeError(TerrainEditErrors::Invalid));
            if (!m_mutations)
                return Result<void>::Failure(MakeError(TerrainEditErrors::CapabilityUnavailable));
            return Completion(document.Paint(intent.paintFence, operation, intent.paint, stamps, *m_mutations));
        }
        return ExecuteTerrainFoliageStroke(document, intent, operation);
    }

    /** @copydoc EditorWorkspaceController::ExecuteTerrainPublication */
    Result<void> EditorWorkspaceController::ExecuteTerrainPublication(TerrainDocumentHost &document, const TerrainToolCommand &intent) {
        if (!m_mutations || !m_durableFiles)
            return Result<void>::Failure(MakeError(TerrainEditErrors::CapabilityUnavailable));
        auto published = intent.action == TerrainToolAction::ConvertSource
                             ? document.Convert(intent.document.fence, intent.conversionLayers, *m_mutations, *m_durableFiles)
                             : document.Save(intent.document.fence, *m_mutations, *m_durableFiles);
        if (published.HasError())
            return Result<void>::Failure(published.ErrorValue());
        if (published.Value().status != Assets::AssetSourcePublicationStatus::Durable)
            return Result<void>::Failure(
                published.Value().durabilityError.value_or(MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed)));
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::ExecuteTerrainDocumentAction */
    Result<void> EditorWorkspaceController::ExecuteTerrainDocumentAction(TerrainDocumentHost &document, const TerrainToolCommand &intent) {
        using enum TerrainToolAction;
        switch (intent.action) {
            case Undo:
                return Completion(document.History(intent.document.fence, false));
            case Redo:
                return Completion(document.History(intent.document.fence, true));
            case Close:
                return CloseTerrainDocument(intent.document.instance, false);
            case Discard:
                return CloseTerrainDocument(intent.document.instance, true);
            case Save:
            case ConvertSource:
                return ExecuteTerrainPublication(document, intent);
            case ReportFailure:
                return intent.failure ? Result<void>::Failure(*intent.failure)
                                      : Result<void>::Failure(MakeError(TerrainEditErrors::Invalid));
            default:
                return Result<void>::Failure(MakeError(TerrainEditErrors::CapabilityUnavailable));
        }
    }

    /** @copydoc EditorWorkspaceController::ExecuteTerrainTool */
    Result<void> EditorWorkspaceController::ExecuteTerrainTool(TerrainDocumentHost &document, const TerrainToolCommand &intent) {
        if (intent.action == TerrainToolAction::Stroke)
            return ExecuteTerrainStroke(document, intent);
        if (IsPaletteAction(intent.action))
            return ExecuteTerrainPalette(document, intent);
        if (intent.action == TerrainToolAction::Cook || intent.action == TerrainToolAction::CancelCook)
            return ExecuteTerrainCook(document, intent);
        return ExecuteTerrainDocumentAction(document, intent);
    }
}  // namespace Horo::Editor
