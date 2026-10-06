#include "UiHotReloadInternal.h"

#include <algorithm>
#include <new>

namespace Horo::Runtime::Ui::UiReloadDetail {
    namespace {
        /** @brief Compares complete typed geometry lineage including font-scale presentation policy. */
        [[nodiscard]] bool SameLayout(const UiLayoutSnapshotDescriptor &a, const UiLayoutSnapshotDescriptor &b) noexcept {
            return a.instance == b.instance && a.canvas == b.canvas && a.document == b.document && a.sources == b.sources &&
                   a.interaction == b.interaction && a.fontScale == b.fontScale;
        }

        /** @brief Compares complete copied focus audience, target and modal incarnation evidence. */
        [[nodiscard]] bool SameFocus(const UiFocusSnapshot &a, const UiFocusSnapshot &b) noexcept {
            return a.owner == b.owner && a.focused == b.focused && a.activeModal == b.activeModal && a.modalDepth == b.modalDepth &&
                   a.modalRoot == b.modalRoot;
        }

        /** @brief Captures a single canvas's actual runtime owners, with no authoritative state duplication. */
        [[nodiscard]] Result<CanvasStamp> CaptureCanvas(const UiReloadCanvas &canvas) {
            CanvasStamp stamp;
            stamp.id = canvas.id;
            stamp.tree = canvas.tree.Revision();
            if (canvas.focus) {
                const auto snapshot = canvas.focus->Snapshot();
                if (snapshot.HasError())
                    return Result<CanvasStamp>::Failure(snapshot.ErrorValue());
                stamp.focus = snapshot.Value();
            }
            if (canvas.routes)
                stamp.routes = canvas.routes->Revision();
            if (canvas.bindings)
                stamp.bindings = canvas.bindings->Current();
            if (canvas.layout)
                stamp.layout = canvas.layout->Descriptor();
            if (canvas.clipped) {
                stamp.clipped = canvas.clipped->Descriptor();
                stamp.scrolls.assign(canvas.clipped->Scrolls().begin(), canvas.clipped->Scrolls().end());
            }
            stamp.policies = canvas.clipPolicies;
            stamp.controls.reserve(canvas.controls.size());
            for (const auto &control : canvas.controls) {
                const auto actual = control.control.CaptureReloadStamp();
                if (actual.HasError())
                    return Result<CanvasStamp>::Failure(actual.ErrorValue());
                stamp.controls.emplace_back(control.id, actual.Value());
            }
            return Result<CanvasStamp>::Success(std::move(stamp));
        }

        /** @brief Checks actual control order, associations, draft/cancel baseline and input admission evidence. */
        [[nodiscard]] bool ControlsMatch(const UiReloadCanvas &canvas, const CanvasStamp &stamp) noexcept {
            if (canvas.controls.size() != stamp.controls.size())
                return false;
            for (std::size_t i = 0; i < stamp.controls.size(); ++i)
                if (canvas.controls[i].id != stamp.controls[i].first ||
                    !canvas.controls[i].control.MatchesReloadStamp(stamp.controls[i].second))
                    return false;
            return true;
        }

        /** @brief Verifies exact owner admission and copied immutable layout/scroll publication. */
        [[nodiscard]] bool GeometryMatch(const UiReloadCanvas &canvas, const CanvasStamp &stamp) noexcept {
            if (canvas.layout.has_value() != stamp.layout.has_value() || canvas.clipped.has_value() != stamp.clipped.has_value())
                return false;
            if (canvas.layout && !SameLayout(canvas.layout->Descriptor(), *stamp.layout))
                return false;
            if (canvas.clipped && canvas.clipped->Descriptor() != *stamp.clipped)
                return false;
            if (!std::ranges::equal(canvas.clipPolicies, stamp.policies))
                return false;
            return !canvas.clipped || std::ranges::equal(canvas.clipped->Scrolls(), stamp.scrolls);
        }

        /** @brief Rejects source mutation and held/reentrant owner operations before any publication side effect. */
        [[nodiscard]] bool CanvasMatches(const UiReloadCanvas &canvas, const CanvasStamp &stamp) noexcept {
            if (canvas.tree.State() != UiElementTreeState::Active || canvas.tree.Revision() != stamp.tree ||
                canvas.focus.has_value() != stamp.focus.has_value() || canvas.routes.has_value() != stamp.routes.has_value() ||
                canvas.bindings.has_value() != stamp.bindings.has_value())
                return false;
            if (canvas.actions && canvas.actions->State() != UiActionRouterState::Active)
                return false;
            if (canvas.captures && canvas.captures->State() != UiPointerCaptureStoreState::Active)
                return false;
            if (canvas.layoutEngine && canvas.layoutEngine->State() != UiLayoutEngineState::Active)
                return false;
            if (canvas.clipping && canvas.clipping->State() != UiLayoutClipEngineState::Active)
                return false;
            if (canvas.focus) {
                const auto current = canvas.focus->Snapshot();
                if (current.HasError() || !SameFocus(current.Value(), *stamp.focus))
                    return false;
            }
            if (canvas.routes && (!canvas.routes->CanRetire() || canvas.routes->Revision() != *stamp.routes))
                return false;
            if (canvas.bindings) {
                const auto current = canvas.bindings->Current();
                if (canvas.bindings->ValidateOwner(canvas.tree).HasError() || current.revision != stamp.bindings->revision ||
                    current.content != stamp.bindings->content)
                    return false;
            }
            return ControlsMatch(canvas, stamp) && GeometryMatch(canvas, stamp);
        }
    }  // namespace

    Result<std::vector<CanvasStamp>> CaptureSource(const UiReloadGeneration &generation) {
        try {
            std::vector<CanvasStamp> stamps;
            stamps.reserve(generation.Canvases().size());
            for (const auto &canvas : generation.Canvases()) {
                auto stamp = CaptureCanvas(canvas);
                if (stamp.HasError())
                    return Result<std::vector<CanvasStamp>>::Failure(stamp.ErrorValue());
                stamps.push_back(std::move(stamp).Value());
            }
            return Result<std::vector<CanvasStamp>>::Success(std::move(stamps));
        } catch (const std::bad_alloc &) {
            return Result<std::vector<CanvasStamp>>::Failure(MakeError(UiErrors::CapacityExceeded));
        }
    }

    bool MatchesSource(const UiReloadGeneration &generation, std::span<const CanvasStamp> stamps) noexcept {
        if (generation.Canvases().size() != stamps.size())
            return false;
        for (std::size_t i = 0; i < stamps.size(); ++i)
            if (generation.Canvases()[i].id != stamps[i].id || !CanvasMatches(generation.Canvases()[i], stamps[i]))
                return false;
        return true;
    }
}  // namespace Horo::Runtime::Ui::UiReloadDetail
