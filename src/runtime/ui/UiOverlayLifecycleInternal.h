#pragma once

#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiOverlayLifecycle.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <new>

namespace Horo::Runtime::Ui {
    namespace OverlayDetail {
        template <typename T> Result<T> Failure(const ErrorCodeDescriptor &reason) {
            return Result<T>::Failure(MakeError(reason));
        }

        /** @brief Resolves one real active route, never caller-provided route metadata. */
        inline const UiRouteInstance *Route(const UiReloadCanvas &canvas, UiRouteId id) noexcept {
            if (!canvas.routes || canvas.routes->State() != UiScreenStackState::Active || canvas.routes->Size() != 1)
                return nullptr;
            const auto routes = canvas.routes->Routes();
            return routes.front().metadata.id == id ? &routes.front() : nullptr;
        }

        /** @brief Ordinary bands have no independent modal/default authority. */
        inline bool OrdinaryPolicy(const UiOverlayLayerDescriptor &binding, const UiRouteMetadata &route) noexcept {
            return !route.modal && binding.exclusivity == UiModalExclusivity::None && !binding.modalRoot.IsValid() &&
                   !binding.defaultFocus.IsValid();
        }

        /** @brief A modal requires an interactive trap and an explicit audience exclusion policy. */
        inline bool ModalPolicy(const UiOverlayLayerDescriptor &binding, const UiRouteMetadata &route) noexcept {
            return route.modal && binding.interactive && binding.modalRoot.IsValid() && binding.exclusivity != UiModalExclusivity::None &&
                   (binding.exclusivity != UiModalExclusivity::Player || binding.player.has_value());
        }

        /** @brief Validates role-specific policy independently of priority. */
        inline bool Policy(const UiOverlayLayerDescriptor &binding, const UiRouteMetadata &route) noexcept {
            if (binding.exclusivity >= UiModalExclusivity::Count || route.band > UiPresentationBand::Modal)
                return false;
            return route.band == UiPresentationBand::Modal ? ModalPolicy(binding, route) : OrdinaryPolicy(binding, route);
        }

        /** @brief Validate explicit host associations without consulting an ambient Input or renderer service. */
        inline bool Associations(const UiOverlayLayerDescriptor &binding, UiOwnershipGeneration ownership) noexcept {
            return binding.canvas.IsValid() && binding.route.IsValid() && binding.context.IsValid() && binding.view.IsValid() &&
                   binding.viewport.IsValid() && binding.context.ownership == ownership && binding.view.ownership == ownership &&
                   binding.viewport.ownership == ownership;
        }

        /** @brief Actual focus audience and geometry must belong to the same prepared composition. */
        inline bool FocusMatches(const UiReloadCanvas &canvas, const UiOverlayLayerDescriptor &binding) noexcept {
            return canvas.focus && canvas.focus->State() == UiFocusGraphState::Active &&
                   canvas.focus->Owner().scope.player == binding.player && canvas.layout &&
                   canvas.focus->Owner().interaction == canvas.layout->Descriptor().interaction;
        }

        /** @brief Checks actual retained ownership and admitted view before any layer mutation. */
        inline Result<UiRouteInstance> Validate(UiReloadGeneration &generation, const UiOverlayLayerDescriptor &binding,
                                                UiOwnershipGeneration ownership) {
            if (!Associations(binding, ownership))
                return Failure<UiRouteInstance>(UiErrors::HandleOwnerMismatch);
            auto *canvas = generation.Canvas(binding.canvas);
            if (!canvas || canvas->tree.State() != UiElementTreeState::Active || canvas->tree.Instance().ownership != ownership)
                return Failure<UiRouteInstance>(UiErrors::HandleStale);
            const auto *route = Route(*canvas, binding.route);
            if (!route || !Policy(binding, route->metadata))
                return Failure<UiRouteInstance>(UiErrors::RouteOperationInvalid);
            if (!FocusMatches(*canvas, binding))
                return Failure<UiRouteInstance>(UiErrors::FocusScopeMismatch);
            if (auto valid = canvas->focus->ValidateOwner(canvas->tree); valid.HasError())
                return Result<UiRouteInstance>::Failure(valid.ErrorValue());
            if (std::ranges::find(canvas->presentations, binding.view, &UiPresentedInteractionState::View) == canvas->presentations.end())
                return Failure<UiRouteInstance>(UiErrors::RenderPresentationInvalid);
            return Result<UiRouteInstance>::Success(*route);
        }

        /** @brief Installs the real modal trap or default on a private candidate only. */
        inline Result<void> Focus(UiReloadCanvas &canvas, const UiOverlayLayerDescriptor &binding) {
            if (canvas.focus->Snapshot().Value().modalDepth != 0)
                return Failure<void>(UiErrors::FocusModalBoundaryViolation);
            if (!binding.interactive)
                return Result<void>::Success();
            if (binding.exclusivity == UiModalExclusivity::None) {
                const auto focused = canvas.focus->FocusDefault();
                return focused.HasError() ? Result<void>::Failure(focused.ErrorValue()) : Result<void>::Success();
            }
            const auto root = canvas.tree.Find(binding.modalRoot);
            if (root.HasError())
                return Result<void>::Failure(root.ErrorValue());
            const auto opened = canvas.focus->PushModal({root.Value(), binding.modalRoot, binding.defaultFocus});
            return opened.HasError() ? Result<void>::Failure(opened.ErrorValue()) : Result<void>::Success();
        }
    }  // namespace OverlayDetail

    struct UiOverlayLifecycle::Storage final {
        struct Layer final {
            UiOverlayLayerId id;
            UiOverlayLayerDescriptor binding;
            UiRouteInstance route;
            UiHotReload publisher;
            std::optional<UiFocusRestoration> restoration;
            bool blocked{};
            bool needsPresentation{};
        };

        explicit Storage(const UiOverlayLifecycleDescriptor &value) : descriptor(value), lastIssued(value.previousLayerIncarnation) {
            active.resize(value.maximumLayers);
            // Shutdown has independent reserved capacity even when ordinary dismissals are backpressured.
            retired.resize(value.maximumRetiredLayers + value.maximumLayers);
        }

        [[nodiscard]] Layer *Find(UiOverlayLayerId id) noexcept {
            for (auto &entry : active)
                if (entry && entry->id == id)
                    return &*entry;
            return nullptr;
        }

        [[nodiscard]] const Layer *Find(UiOverlayLayerId id) const noexcept {
            for (const auto &entry : active)
                if (entry && entry->id == id)
                    return &*entry;
            return nullptr;
        }

        [[nodiscard]] static bool Higher(const Layer &a, const Layer &b) noexcept {
            if (a.route.metadata.band != b.route.metadata.band)
                return a.route.metadata.band > b.route.metadata.band;
            if (a.route.metadata.order != b.route.metadata.order)
                return a.route.metadata.order > b.route.metadata.order;
            return a.id.generation > b.id.generation;
        }

        [[nodiscard]] static bool Excludes(const Layer &modal, const Layer &target) noexcept {
            using enum UiModalExclusivity;
            switch (modal.binding.exclusivity) {
                case Viewport:
                    return modal.binding.viewport == target.binding.viewport;
                case Player:
                    return modal.binding.player == target.binding.player;
                case GameInstance:
                    return true;
                case None:
                case Count:
                    return false;
            }
            return false;
        }

        [[nodiscard]] bool Blocked(const Layer &target) const noexcept {
            return std::ranges::any_of(active, [&target](const auto &entry) {
                return entry && Higher(*entry, target) && Excludes(*entry, target);
            });
        }

        [[nodiscard]] static UiReloadCanvas *Canvas(Layer &layer) noexcept {
            auto *generation = layer.publisher.Current();
            return generation ? generation->Canvas(layer.binding.canvas) : nullptr;
        }

        [[nodiscard]] static const UiReloadCanvas *Canvas(const Layer &layer) noexcept {
            const auto *generation = layer.publisher.OverlayCurrent();
            if (!generation)
                return nullptr;
            const auto canvases = generation->Canvases();
            const auto found = std::ranges::find(canvases, layer.binding.canvas, &UiReloadCanvas::id);
            return found == canvases.end() ? nullptr : std::to_address(found);
        }

        /** @brief Fail closed when an outside owner operation invalidates the admitted composition. */
        [[nodiscard]] static bool Live(const Layer &layer) noexcept {
            const auto *canvas = Canvas(layer);
            if (!canvas || canvas->tree.State() != UiElementTreeState::Active || !canvas->focus ||
                canvas->focus->State() != UiFocusGraphState::Active)
                return false;
            const auto *route = OverlayDetail::Route(*canvas, layer.binding.route);
            return route && route->id == layer.route.id && route->metadata == layer.route.metadata;
        }

        /** @brief Copy one projection while pinning its actual generation; caller commits the complete output later. */
        [[nodiscard]] static Result<UiOverlayLayerSnapshot> Project(const Layer &layer, UiOverlayInputStatus status) {
            const auto lease = layer.publisher.Acquire();
            if (lease.HasError())
                return Result<UiOverlayLayerSnapshot>::Failure(lease.ErrorValue());
            const auto canvases = lease.Value().Get()->Canvases();
            const auto canvas = std::ranges::find(canvases, layer.binding.canvas, &UiReloadCanvas::id);
            if (canvas == canvases.end() || !canvas->focus)
                return OverlayDetail::Failure<UiOverlayLayerSnapshot>(UiErrors::HandleStale);
            const auto focus = canvas->focus->CurrentFocus();
            if (focus.HasError())
                return Result<UiOverlayLayerSnapshot>::Failure(focus.ErrorValue());
            return Result<UiOverlayLayerSnapshot>::Success(
                {layer.id, layer.binding, layer.route.id, layer.route.metadata.band, layer.route.metadata.order, status, focus.Value()});
        }

        /** @brief A live layer owns a distinct mutable namespace even when views or players are shared. */
        [[nodiscard]] bool Conflicts(const UiOverlayLayerDescriptor &binding, const UiReloadCanvas &canvas) const noexcept {
            return std::ranges::any_of(active, [&binding, &canvas](const auto &entry) {
                if (!entry)
                    return false;
                if (entry->binding.context == binding.context)
                    return true;
                const auto *other = Canvas(*entry);
                if (other &&
                    (other->tree.Instance() == canvas.tree.Instance() || other->tree.Canvas() == canvas.tree.Canvas() ||
                     (other->focus && other->focus->Owner().scope.presentationLayer == canvas.focus->Owner().scope.presentationLayer)))
                    return true;
                return false;
            });
        }

        /** @brief Cancels only this exact context's real pointer captures. */
        static void CancelCapture(Layer &layer, UiPointerCaptureCancellationReason reason) noexcept {
            if (auto *canvas = Canvas(layer); canvas && canvas->captures)
                (void)canvas->captures->CancelContext(layer.binding.context, reason);
        }

        /** @brief Stores stable paths once on exclusion and delegates recovery to actual focus owners on uncover. */
        void ReconcileLayer(Layer &layer) const noexcept {
            const bool blocked = Blocked(layer);
            auto *canvas = Canvas(layer);
            if (!canvas || !canvas->focus)
                return;
            if (blocked && !layer.blocked) {
                if (const auto saved = canvas->focus->CaptureRestoration(); saved.HasValue()) {
                    layer.restoration = saved.Value();
                    (void)canvas->focus->ClearFocus();
                }
                CancelCapture(layer, UiPointerCaptureCancellationReason::ModalOpened);
            } else if (!blocked && layer.blocked && layer.restoration) {
                (void)canvas->focus->Restore(*layer.restoration);
                layer.restoration.reset();
            }
            layer.blocked = blocked;
        }

        /** @brief Reconcile every reserved layer in the same deterministic bounded owner pass. */
        void Reconcile() noexcept {
            for (auto &entry : active)
                if (entry)
                    ReconcileLayer(*entry);
        }

        [[nodiscard]] std::size_t RetiredCount() const noexcept {
            return std::ranges::count_if(retired, [](const auto &entry) {
                return entry.has_value();
            });
        }

        void Retire(std::optional<Layer> &entry) noexcept {
            CancelCapture(*entry, UiPointerCaptureCancellationReason::ContextRemoved);
            entry->publisher.Shutdown();
            auto slot = std::ranges::find_if(retired, [](const auto &candidate) {
                return !candidate;
            });
            slot->emplace(std::move(*entry));
            entry.reset();
        }

        UiOverlayLifecycleDescriptor descriptor;
        std::vector<std::optional<Layer>> active;
        std::vector<std::optional<Layer>> retired;
        std::uint32_t lastIssued{};
        UiOverlayLifecycleState state{UiOverlayLifecycleState::Active};
        bool suspended{};
        bool collecting{};
        bool shutdownRequested{}; /**< Deferred until retirement callback drain releases its borrowed publisher. */
    };
}  // namespace Horo::Runtime::Ui
