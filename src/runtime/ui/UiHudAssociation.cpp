#include "Horo/Runtime/Ui/UiHudAssociation.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T> Result<T> Failure(const ErrorCodeDescriptor &reason) {
            return Result<T>::Failure(MakeError(reason));
        }

        /** @brief Checks a typed host-issued incarnation in the exact publisher ownership generation. */
        template <typename Tag> bool Owned(UiRuntimeHandle<Tag> handle, UiOwnershipGeneration owner) noexcept {
            return handle.IsValid() && handle.ownership == owner;
        }

        /** @brief Validates host-issued identities without consulting ambient player/viewport services. */
        bool Identities(const UiHudAssociationDescriptor &binding, UiOwnershipGeneration owner) noexcept {
            return Owned(binding.id, owner) && Owned(binding.player, owner) && Owned(binding.viewport, owner) &&
                   Owned(binding.view, owner) && Owned(binding.context, owner) && binding.canvas.IsValid() && binding.route.IsValid() &&
                   (!binding.camera || Owned(*binding.camera, owner));
        }

        /** @brief Recognizes only explicitly supported gameplay lifetimes in the actual binding registration. */
        bool ProviderAvailable(const UiBindingStore &store, const UiHudProvider &provider) noexcept {
            return (provider.scope == UiBindingProviderScopeKind::GameInstance || provider.scope == UiBindingProviderScopeKind::Player) &&
                   store.HasProvider(provider.instance, provider.scope);
        }

        /** @brief Checks the explicit bounded gameplay registrations; schema scope is not inferred from HUD attachment. */
        Result<void> Providers(const UiReloadCanvas &canvas, const UiHudAssociationDescriptor &binding) {
            if (binding.providerCount > binding.providers.size())
                return Failure<void>(UiErrors::CapacityExceeded);
            if (binding.providerCount == 0)
                return Result<void>::Success();
            if (!canvas.bindings)
                return Failure<void>(UiErrors::BindingProviderUnknown);
            if (auto valid = canvas.bindings->ValidateOwner(canvas.tree); valid.HasError())
                return valid;
            for (std::size_t index = 0; index < binding.providerCount; ++index) {
                const auto &provider = binding.providers[index];
                if ((index != 0 && !(binding.providers[index - 1].instance < provider.instance)) ||
                    !ProviderAvailable(*canvas.bindings, provider))
                    return Failure<void>(UiErrors::BindingProviderUnknown);
            }
            return Result<void>::Success();
        }

        /** @brief Checks the real active nonmodal HUD route independently of focus and provider lifetimes. */
        Result<void> Route(const UiReloadCanvas &canvas, UiRouteId id) {
            if (!canvas.routes || canvas.routes->State() != UiScreenStackState::Active || canvas.routes->Size() != 1)
                return Failure<void>(UiErrors::RouteOperationInvalid);
            if (const auto &route = canvas.routes->Routes().front().metadata;
                route.id != id || route.band != UiPresentationBand::Hud || route.modal)
                return Failure<void>(UiErrors::RouteOperationInvalid);
            return Result<void>::Success();
        }

        /** @brief Qualifies exact player audience against the real retained focus owner. */
        Result<void> Audience(const UiReloadCanvas &canvas, UiFocusPlayerId player) {
            if (!canvas.focus || canvas.focus->State() != UiFocusGraphState::Active || canvas.focus->Owner().scope.player != player)
                return Failure<void>(UiErrors::FocusScopeMismatch);
            return canvas.focus->ValidateOwner(canvas.tree);
        }

        /** @brief Resolves immutable authored screen/camera policy against explicit copied viewport evidence. */
        Result<UiResolvedScreenCanvas> ScreenMetrics(const UiReloadGeneration &generation, const UiHudAssociationDescriptor &binding) {
            const auto authored = generation.Instance().Canvases();
            const auto descriptor = std::ranges::find(authored, binding.canvas, &UiCanvasDescriptor::id);
            if (descriptor == authored.end())
                return Failure<UiResolvedScreenCanvas>(UiErrors::HandleStale);
            if (descriptor->renderMode == UiRenderMode::ScreenSpaceCamera && !binding.camera)
                return Failure<UiResolvedScreenCanvas>(UiErrors::CanvasSpaceModeMismatch);
            return ResolveUiScreenCanvasWithEvidence(*descriptor, binding.viewportEvidence);
        }

        /** @brief Qualifies actual retained route/audience/view evidence and resolves the authored screen canvas. */
        Result<UiResolvedScreenCanvas> Validate(const UiReloadGeneration &generation, const UiHudAssociationDescriptor &binding) {
            if (!Identities(binding, generation.Instance().InstanceId().ownership))
                return Failure<UiResolvedScreenCanvas>(UiErrors::HandleOwnerMismatch);
            const auto canvases = generation.Canvases();
            const auto canvas = std::ranges::find(canvases, binding.canvas, &UiReloadCanvas::id);
            if (canvas == canvases.end() || canvas->tree.State() != UiElementTreeState::Active)
                return Failure<UiResolvedScreenCanvas>(UiErrors::HandleStale);
            if (const auto valid = Route(*canvas, binding.route); valid.HasError())
                return Result<UiResolvedScreenCanvas>::Failure(valid.ErrorValue());
            if (const auto valid = Audience(*canvas, binding.player); valid.HasError())
                return Result<UiResolvedScreenCanvas>::Failure(valid.ErrorValue());
            if (std::ranges::find(canvas->presentations, binding.view, &UiPresentedInteractionState::View) == canvas->presentations.end())
                return Failure<UiResolvedScreenCanvas>(UiErrors::RenderPresentationInvalid);
            if (const auto valid = Providers(*canvas, binding); valid.HasError())
                return Result<UiResolvedScreenCanvas>::Failure(valid.ErrorValue());
            return ScreenMetrics(generation, binding);
        }

        /** @brief Checks semantic Assets/runtime ownership independently of generation refresh eligibility. */
        bool SameSource(const UiReloadGeneration &old, const UiReloadGeneration &replacement) noexcept {
            return old.Instance().InstanceId() == replacement.Instance().InstanceId() && old.Asset() == replacement.Asset() &&
                   old.Instance().DocumentId() == replacement.Instance().DocumentId();
        }

        /** @brief Matches the real clip snapshot to all immutable layout source evidence. */
        bool ClippingMatches(const UiReloadCanvas &canvas) noexcept {
            if (!canvas.clipped)
                return false;
            const auto &clip = canvas.clipped->Descriptor();
            const auto &layout = canvas.layout->Descriptor();
            return clip.instance == layout.instance && clip.canvas == layout.canvas && clip.document == layout.document &&
                   clip.sources == layout.sources && clip.interaction == layout.interaction;
        }

        /** @brief Rejects extraction until actual root, accessibility font and clipping geometry match the resolved output. */
        Result<void> Geometry(const UiReloadCanvas &canvas, const UiResolvedScreenCanvas &metrics) {
            if (!canvas.layout || canvas.layout->Descriptor().fontScale != metrics.fontScale ||
                canvas.focus->Owner().interaction != canvas.layout->Descriptor().interaction)
                return Failure<void>(UiErrors::LayoutSourceStale);
            const auto root = canvas.tree.Root();
            if (root.HasError())
                return Result<void>::Failure(root.ErrorValue());
            const auto record = canvas.layout->Get(root.Value().handle);
            if (const UiLogicalRect expected{{0, 0}, {metrics.logicalExtent.width, metrics.logicalExtent.height}};
                record.HasError() || record.Value().arrangement.contentBox != expected)
                return Failure<void>(UiErrors::LayoutSourceStale);
            if (!ClippingMatches(canvas))
                return Failure<void>(UiErrors::LayoutClipSourceStale);
            return Result<void>::Success();
        }

        /** @brief Correlates renderer completion to the leased ticket and still-current interaction. */
        bool ReceiptMatches(const UiHudFrame &frame, const UiHudFrame &current, const UiPresentationReceipt &receipt) noexcept {
            return receipt.view == frame.Association().view && receipt.canvas == frame.Layout().Descriptor().canvas &&
                   receipt.interactionRevision == frame.Layout().Descriptor().interaction && receipt.snapshotRevision == frame.Snapshot() &&
                   current.Layout().Descriptor().interaction == frame.Layout().Descriptor().interaction;
        }
    }  // namespace

    /** @copydoc UiHudAssociation::Create */
    Result<UiHudAssociation> UiHudAssociation::Create(const UiHudAssociationDescriptor &descriptor, const UiHotReload &publisher) {
        auto lease = publisher.Acquire();
        if (lease.HasError())
            return Result<UiHudAssociation>::Failure(lease.ErrorValue());
        if (auto valid = Validate(*lease.Value().Get(), descriptor); valid.HasError())
            return Result<UiHudAssociation>::Failure(valid.ErrorValue());
        return Result<UiHudAssociation>::Success(UiHudAssociation{descriptor, std::move(lease).Value()});
    }

    /** @copydoc UiHudAssociation::UiHudAssociation */
    UiHudAssociation::UiHudAssociation(UiHudAssociationDescriptor descriptor, UiReloadLease generation) noexcept
        : descriptor_(std::move(descriptor)), generation_(std::move(generation)) {}

    /** @copydoc UiHudAssociation::UiHudAssociation */
    UiHudAssociation::UiHudAssociation(UiHudAssociation &&other) noexcept
        : descriptor_(other.descriptor_), generation_(std::move(other.generation_)), revision_(other.revision_),
          presented_(other.presented_), active_(std::exchange(other.active_, false)) {}

    /** @brief Checks exact publisher identity, including independently created publishers with identical authored IDs. */
    bool UiHudAssociation::Current(UiHotReload &publisher) const noexcept {
        return active_ && generation_.Get() && publisher.Current() && publisher.IsCurrent(generation_);
    }

    /** @brief Requires the exact association revision and generation before admitting renderer completion. */
    bool UiHudAssociation::Matches(UiHotReload &publisher, const UiHudFrame &frame) const noexcept {
        return Current(publisher) && descriptor_.enabled && descriptor_.visible && frame.revision_ == revision_ &&
               frame.association_.id == descriptor_.id && publisher.IsCurrent(frame.generation_);
    }

    /** @brief Invalidates all historical frame tickets without wrapping association evidence. */
    Result<void> UiHudAssociation::Advance() {
        if (revision_ == std::numeric_limits<std::uint64_t>::max())
            return Failure<void>(UiErrors::GenerationExhausted);
        ++revision_;
        presented_ = false;
        return Result<void>::Success();
    }

    /** @brief Cancels only the borrowed live canvas's exact input context, never another semantic owner. */
    void UiHudAssociation::CancelCapture(UiHotReload &publisher) const noexcept {
        if (!Current(publisher))
            return;
        auto *generation = publisher.Current();
        if (!generation)
            return;
        auto *canvas = generation->Canvas(descriptor_.canvas);
        if (canvas && canvas->captures)
            (void)canvas->captures->CancelContext(descriptor_.context, UiPointerCaptureCancellationReason::ContextRemoved);
    }

    /** @copydoc UiHudAssociation::Reassociate */
    Result<void> UiHudAssociation::Reassociate(const UiHudAssociationDescriptor &descriptor, UiHotReload &publisher) {
        if (!Current(publisher))
            return Failure<void>(UiErrors::HandleStale);
        if (descriptor.id != descriptor_.id || descriptor.player != descriptor_.player || descriptor.canvas != descriptor_.canvas ||
            descriptor.route != descriptor_.route)
            return Failure<void>(UiErrors::HandleOwnerMismatch);
        if (const auto valid = Validate(*generation_.Get(), descriptor); valid.HasError())
            return Result<void>::Failure(valid.ErrorValue());
        if (descriptor == descriptor_)
            return Result<void>::Success();
        if (auto advanced = Advance(); advanced.HasError())
            return advanced;
        CancelCapture(publisher);
        descriptor_ = descriptor;
        return Result<void>::Success();
    }

    /** @copydoc UiHudAssociation::Refresh */
    Result<void> UiHudAssociation::Refresh(UiHotReload &publisher) {
        if (!active_ || !generation_.Get() || !publisher.Retains(generation_))
            return Failure<void>(UiErrors::HandleStale);
        auto next = publisher.Acquire();
        if (next.HasError())
            return Result<void>::Failure(next.ErrorValue());
        const auto &old = *generation_.Get();
        const auto &replacement = *next.Value().Get();
        if (!SameSource(old, replacement))
            return Failure<void>(UiErrors::HandleOwnerMismatch);
        if (const auto valid = Validate(replacement, descriptor_); valid.HasError())
            return Result<void>::Failure(valid.ErrorValue());
        if (Current(publisher))
            return Result<void>::Success();
        if (auto advanced = Advance(); advanced.HasError())
            return advanced;
        generation_ = std::move(next).Value();
        return Result<void>::Success();
    }

    /** @copydoc UiHudAssociation::SetPolicy */
    Result<void> UiHudAssociation::SetPolicy(UiHotReload &publisher, bool enabled, bool visible) {
        auto replacement = descriptor_;
        replacement.enabled = enabled;
        replacement.visible = visible;
        return Reassociate(replacement, publisher);
    }

    /** @copydoc UiHudAssociation::ResolveCanvas */
    Result<UiResolvedScreenCanvas> UiHudAssociation::ResolveCanvas(UiHotReload &publisher) const {
        if (!Current(publisher))
            return Failure<UiResolvedScreenCanvas>(UiErrors::HandleStale);
        return Validate(*generation_.Get(), descriptor_);
    }

    /** @copydoc UiHudAssociation::PrepareFrame */
    Result<UiHudFrame> UiHudAssociation::PrepareFrame(UiHotReload &publisher, UiRenderSnapshotRevision snapshot) const {
        if (!descriptor_.enabled || !descriptor_.visible)
            return Failure<UiHudFrame>(UiErrors::InstanceStateInvalid);
        if (!snapshot.IsValid())
            return Failure<UiHudFrame>(UiErrors::RevisionInvalid);
        const auto metrics = ResolveCanvas(publisher);
        if (metrics.HasError())
            return Result<UiHudFrame>::Failure(metrics.ErrorValue());
        const auto canvases = generation_.Get()->Canvases();
        const auto canvas = std::ranges::find(canvases, descriptor_.canvas, &UiReloadCanvas::id);
        if (const auto geometry = Geometry(*canvas, metrics.Value()); geometry.HasError())
            return Result<UiHudFrame>::Failure(geometry.ErrorValue());
        return Result<UiHudFrame>::Success(
            UiHudFrame{descriptor_, metrics.Value(), *canvas->layout, *canvas->clipped, generation_, snapshot, revision_});
    }

    /** @copydoc UiHudAssociation::ApplyPresentation */
    Result<bool> UiHudAssociation::ApplyPresentation(UiHotReload &publisher, const UiHudFrame &frame,
                                                     const UiPresentationReceipt &receipt) {
        if (!Matches(publisher, frame))
            return Failure<bool>(UiErrors::HandleStale);
        const auto current = PrepareFrame(publisher, frame.snapshot_);
        if (current.HasError())
            return Result<bool>::Failure(current.ErrorValue());
        if (!ReceiptMatches(frame, current.Value(), receipt))
            return Failure<bool>(UiErrors::RenderPresentationInvalid);
        const auto adopted = publisher.ApplyPresentation(descriptor_.canvas, receipt);
        if (adopted.HasError())
            return adopted;
        presented_ = receipt.outcome == UiPresentationOutcome::Presented;
        if (!presented_)
            CancelCapture(publisher);
        return adopted;
    }

    /** @copydoc UiHudAssociation::InputEligible */
    bool UiHudAssociation::InputEligible(UiHotReload &publisher) const {
        return descriptor_.interactive && descriptor_.enabled && descriptor_.visible && presented_ && Current(publisher) &&
               PrepareFrame(publisher, UiRenderSnapshotRevision::Create(1).Value()).HasValue() &&
               publisher.InputEligible(descriptor_.canvas, descriptor_.view);
    }

    /** @copydoc UiHudAssociation::Shutdown */
    Result<void> UiHudAssociation::Shutdown(UiHotReload &publisher) {
        if (!generation_.Get())
            return Result<void>::Success();
        if (!publisher.Retains(generation_))
            return Failure<void>(UiErrors::HandleOwnerMismatch);
        if (publisher.IsCurrent(generation_) && !publisher.Current())
            return Failure<void>(UiErrors::InstanceStateInvalid);
        CancelCapture(publisher);
        active_ = false;
        presented_ = false;
        generation_ = {};
        return Result<void>::Success();
    }

    /** @brief Constructs an immutable exact-generation extraction ticket after actual geometry validation. */
    UiHudFrame::UiHudFrame(UiHudAssociationDescriptor association, const UiResolvedScreenCanvas &metrics, UiLayoutSnapshot layout,
                           UiLayoutClipSnapshot clipping, UiReloadLease generation, UiRenderSnapshotRevision snapshot,
                           std::uint64_t revision) noexcept
        : association_(std::move(association)), metrics_(metrics), layout_(std::move(layout)), clipping_(std::move(clipping)),
          generation_(std::move(generation)), snapshot_(snapshot), revision_(revision) {}
}  // namespace Horo::Runtime::Ui
