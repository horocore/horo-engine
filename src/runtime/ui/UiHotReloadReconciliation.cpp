#include "UiHotReloadInternal.h"

#include <algorithm>

namespace Horo::Runtime::Ui::UiReloadDetail {
    namespace {
        /** @brief Resolves immutable actual authored type compatibility for a stable element. */
        [[nodiscard]] bool SameType(const UiReloadGeneration &old, const UiReloadGeneration &replacement, UiElementId id) noexcept {
            const auto a = std::ranges::find(old.Instance().Elements(), id, &UiDocumentElement::id);
            const auto b = std::ranges::find(replacement.Instance().Elements(), id, &UiDocumentElement::id);
            return a != old.Instance().Elements().end() && b != replacement.Instance().Elements().end() && a->type == b->type;
        }

        /** @brief Preserves focus only within the identical actual semantic player/layer scope. */
        [[nodiscard]] Result<void> Focus(const UiReloadCanvas &old, UiReloadCanvas &replacement, const UiReloadGeneration &source,
                                         const UiReloadGeneration &candidate, UiReloadReconciliation &counts) {
            if (!old.focus || !replacement.focus)
                return Result<void>::Success();
            if (old.focus->Owner().scope != replacement.focus->Owner().scope ||
                old.focus->Owner().instance != replacement.focus->Owner().instance ||
                old.focus->Owner().canvas != replacement.focus->Owner().canvas ||
                replacement.focus->Owner().interaction <= old.focus->Owner().interaction)
                return Result<void>::Failure(MakeError(UiErrors::HandleOwnerMismatch));
            const auto target = old.focus->CurrentFocus();
            if (target.HasError())
                return Result<void>::Failure(target.ErrorValue());
            if (!target.Value() || !SameType(source, candidate, target.Value()->id))
                return Result<void>::Success();
            if (const auto newTarget = replacement.focus->Find(target.Value()->id);
                newTarget.HasValue() && replacement.focus->SetFocus(newTarget.Value()).HasValue())
                ++counts.preservedFocus;
            return Result<void>::Success();
        }

        /** @brief Reconciles real value-control owners without transferring pending native work or handles. */
        [[nodiscard]] Result<void> Controls(const UiReloadCanvas &old, UiReloadCanvas &replacement, const UiReloadGeneration &source,
                                            const UiReloadGeneration &candidate, UiReloadReconciliation &counts) {
            const auto focused = replacement.focus ? replacement.focus->CurrentFocus() : Result<std::optional<UiFocusTarget>>::Success({});
            if (focused.HasError())
                return Result<void>::Failure(focused.ErrorValue());
            for (auto &control : replacement.controls) {
                const auto previous = std::ranges::find(old.controls, control.id, &UiReloadControl::id);
                if (previous == old.controls.end() || !SameType(source, candidate, control.id) ||
                    old.bindings.has_value() != replacement.bindings.has_value() ||
                    (replacement.bindings &&
                     !replacement.bindings->ReloadCompatible(*old.bindings, old.tree, replacement.tree, control.id))) {
                    ++counts.resetControls;
                    continue;
                }
                const auto preserved =
                    control.control.ReconcileReload(previous->control, focused.Value() && focused.Value()->id == control.id);
                if (preserved.HasError())
                    return Result<void>::Failure(preserved.ErrorValue());
                if (preserved.Value())
                    ++counts.preservedControls;
                else
                    ++counts.resetControls;
            }
            return Result<void>::Success();
        }

        /** @brief Replays compatible canonical route definitions into fresh actual route incarnations. */
        [[nodiscard]] Result<void> Routes(const UiReloadCanvas &old, UiReloadCanvas &replacement, UiReloadReconciliation &counts) {
            if (!old.routes || !replacement.routes)
                return Result<void>::Success();
            for (const auto &route : old.routes->Routes()) {
                const auto definition = std::ranges::find(replacement.routes->Definitions(), route.metadata.id, &UiRouteMetadata::id);
                if (definition == replacement.routes->Definitions().end() || *definition != route.metadata)
                    continue;
                const auto result = replacement.routes->Push(definition->id);
                if (result.HasError())
                    return Result<void>::Failure(result.ErrorValue());
                if (!result.Value().IsCommitted())
                    return Result<void>::Failure(MakeError(UiErrors::CapacityExceeded));
                ++counts.preservedRoutes;
            }
            return Result<void>::Success();
        }

        /** @brief Projects surviving old scroll positions into the replacement's real new bounds. */
        [[nodiscard]] Result<void> Scrolls(const UiReloadCanvas &old, UiReloadCanvas &replacement, const UiReloadGeneration &source,
                                           const UiReloadGeneration &candidate, UiReloadReconciliation &counts) {
            if (!old.clipped || !replacement.clipping || !replacement.layout)
                return Result<void>::Success();
            for (auto &policy : replacement.clipPolicies) {
                if (policy.overflow != UiLayoutOverflowPolicy::Scroll)
                    continue;
                const auto target = replacement.tree.Get(policy.element);
                if (target.HasError() || !SameType(source, candidate, target.Value().id))
                    continue;
                const auto prior = old.tree.Find(target.Value().id);
                if (prior.HasError())
                    continue;
                const auto scroll = std::ranges::find(old.clipped->Scrolls(), prior.Value(), &UiLayoutScrollRecord::element);
                if (scroll != old.clipped->Scrolls().end()) {
                    policy.scrollOffset = scroll->offset;
                    ++counts.preservedScrolls;
                }
            }
            const auto projected = replacement.clipping->Update(replacement.tree, *replacement.layout, {replacement.clipPolicies, {}});
            if (projected.HasError())
                return Result<void>::Failure(projected.ErrorValue());
            replacement.clipped = projected.Value();
            return Result<void>::Success();
        }
    }  // namespace

    Result<UiReloadReconciliation> Reconcile(const UiReloadGeneration &old, UiReloadGeneration &replacement) {
        UiReloadReconciliation counts;
        for (const auto &previous : old.Canvases()) {
            auto *candidate = replacement.Canvas(previous.id);
            if (!candidate)
                continue;
            if (candidate->tree.Canvas() != previous.tree.Canvas() || candidate->tree.Revision() <= previous.tree.Revision())
                return Result<UiReloadReconciliation>::Failure(MakeError(UiErrors::RevisionStale));
            if (previous.layout && candidate->layout &&
                candidate->layout->Descriptor().interaction <= previous.layout->Descriptor().interaction)
                return Result<UiReloadReconciliation>::Failure(MakeError(UiErrors::RevisionStale));
            if (const auto focus = Focus(previous, *candidate, old, replacement, counts); focus.HasError())
                return Result<UiReloadReconciliation>::Failure(focus.ErrorValue());
            for (const auto &result : {Controls(previous, *candidate, old, replacement, counts), Routes(previous, *candidate, counts),
                                       Scrolls(previous, *candidate, old, replacement, counts)})
                if (result.HasError())
                    return Result<UiReloadReconciliation>::Failure(result.ErrorValue());
        }
        return Result<UiReloadReconciliation>::Success(counts);
    }

    void Retire(UiReloadGeneration &generation) noexcept {
        // Each owner closes admission while retaining its preallocated storage and immutable lease pools.
        for (const auto &entry : generation.Canvases()) {
            auto &canvas = *generation.Canvas(entry.id);
            if (canvas.actions)
                (void)canvas.actions->BeginRetirement(UiActionCancellationReason::Reload);
            if (canvas.routes)
                (void)canvas.routes->BeginRetirement();
            if (canvas.bindings)
                (void)canvas.bindings->CloseReloadAdmission();
            if (canvas.captures) {
                (void)canvas.captures->CancelAll(UiPointerCaptureCancellationReason::Reload);
                (void)canvas.captures->BeginRetirement();
            }
            for (auto &control : canvas.controls)
                (void)control.control.BeginRetirement();
            if (canvas.focus)
                (void)canvas.focus->BeginRetirement();
            if (canvas.clipping)
                (void)canvas.clipping->BeginRetirement();
            if (canvas.layoutEngine)
                (void)canvas.layoutEngine->BeginRetirement();
            (void)canvas.tree.BeginRetirement();
        }
    }

    Result<void> DrainBindings(UiReloadGeneration &generation) {
        for (const auto &entry : generation.Canvases()) {
            auto &canvas = *generation.Canvas(entry.id);
            if (canvas.bindings) {
                const auto drained = canvas.bindings->DrainReloadRetirement();
                if (drained.HasError())
                    return drained;
            }
        }
        return Result<void>::Success();
    }

    bool Drained(UiReloadGeneration &generation) noexcept {
        bool drained = true;
        for (const auto &entry : generation.Canvases()) {
            auto &canvas = *generation.Canvas(entry.id);
            canvas.clipped.reset();
            canvas.layout.reset();
            if (canvas.layoutEngine && !canvas.layoutEngine->IsDrained())
                drained = false;
            if (canvas.clipping && !canvas.clipping->IsDrained())
                drained = false;
            if (canvas.captures && !canvas.captures->IsDrained())
                drained = false;
        }
        return drained;
    }
}  // namespace Horo::Runtime::Ui::UiReloadDetail
