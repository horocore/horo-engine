#include "UiHotReloadInternal.h"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>

namespace Horo::Runtime::Ui::UiReloadDetail {
    namespace {
        [[nodiscard]] Result<void> Invalid() {
            return Result<void>::Failure(MakeError(UiErrors::DocumentInvalid));
        }

        /** @brief Resolves actual authored metadata, never caller-supplied identity assertions. */
        [[nodiscard]] const UiDocumentElement *Authored(const UiRuntimeInstance &instance, UiElementId id) noexcept {
            const auto elements = instance.Elements();
            const auto found = std::ranges::find(elements, id, &UiDocumentElement::id);
            return found == elements.end() ? nullptr : std::to_address(found);
        }

        /** @brief Requires the complete cooked ancestor chain to belong to this exact canvas root. */
        [[nodiscard]] bool InCanvas(const UiRuntimeInstance &instance, UiElementId id, UiElementId root) noexcept {
            for (std::size_t depth = 0; depth < instance.Elements().size(); ++depth) {
                const auto *element = Authored(instance, id);
                if (!element)
                    return false;
                if (id == root)
                    return !element->parent.IsValid();
                id = element->parent;
            }
            return false;
        }

        /** @brief Compares real retained hierarchy with cooked stable IDs and parent evidence. */
        [[nodiscard]] Result<void> Tree(const UiRuntimeInstance &instance, const UiReloadCanvas &canvas, UiElementId root) {
            const auto &tree = canvas.tree;
            if (tree.State() != UiElementTreeState::Active || tree.Instance() != instance.InstanceId() ||
                tree.SourceDocument() != instance.DocumentId() || tree.SourceDocumentRevision() != instance.DocumentRevision())
                return Invalid();
            if (const auto actualRoot = tree.Root(); actualRoot.HasError() || actualRoot.Value().id != root)
                return Invalid();
            std::array<UiElementHandle, MaximumUiTreeElements> handles{};
            const auto count = tree.Preorder(handles);
            if (count.HasError())
                return Result<void>::Failure(count.ErrorValue());
            for (std::size_t i = 0; i < count.Value(); ++i) {
                const auto record = tree.Get(handles[i]);
                const auto *authored = record.HasValue() ? Authored(instance, record.Value().id) : nullptr;
                if (!authored || !InCanvas(instance, authored->id, root))
                    return Invalid();
                const auto parent = record.Value().parent.IsValid() ? tree.Get(record.Value().parent) : tree.Root();
                if (record.Value().parent.IsValid() != authored->parent.IsValid() ||
                    (authored->parent.IsValid() && (parent.HasError() || parent.Value().id != authored->parent)))
                    return Invalid();
            }
            return Result<void>::Success();
        }

        /** @brief Verifies exact source lineage of an actual action/control owner. */
        [[nodiscard]] bool Matches(const UiActionOwnerContext &owner, const UiElementTree &tree) noexcept {
            return owner.IsValid() && owner.instance == tree.Instance() && owner.canvas == tree.Canvas() &&
                   owner.document == tree.SourceDocument() && owner.documentRevision == tree.SourceDocumentRevision() &&
                   owner.treeRevision == tree.Revision();
        }

        /** @brief Checks each actual control's authored association and its real tree/source owner. */
        [[nodiscard]] Result<void> Controls(const UiReloadCanvas &canvas) {
            if (canvas.controls.size() > canvas.tree.Size())
                return Invalid();
            for (std::size_t i = 0; i < canvas.controls.size(); ++i) {
                const auto &entry = canvas.controls[i];
                if (const auto handle = canvas.tree.Find(entry.id); handle.HasError() || handle.Value() != entry.control.Element() ||
                                                                    !Matches(entry.control.Owner(), canvas.tree) ||
                                                                    entry.control.LifecycleState() != UiControlLifecycleState::Active)
                    return Invalid();
                for (std::size_t j = 0; j < i; ++j)
                    if (canvas.controls[j].id == entry.id)
                        return Result<void>::Failure(MakeError(UiErrors::DocumentDuplicateIdentity));
            }
            return Result<void>::Success();
        }

        /** @brief Checks actual optional input owners without invoking provider or action callbacks. */
        [[nodiscard]] Result<void> InputOwners(const UiReloadCanvas &canvas) {
            const auto &tree = canvas.tree;
            if (canvas.focus) {
                if (const auto valid = canvas.focus->ValidateOwner(tree); valid.HasError())
                    return valid;
                const auto &owner = canvas.focus->Owner();
                if (canvas.focus->State() != UiFocusGraphState::Active || owner.instance != tree.Instance() ||
                    owner.canvas != tree.Canvas() || owner.document != tree.SourceDocument() ||
                    owner.documentRevision != tree.SourceDocumentRevision() || owner.treeRevision != tree.Revision())
                    return Invalid();
            }
            if (canvas.actions && (canvas.actions->State() != UiActionRouterState::Active || !Matches(canvas.actions->Owner(), tree)))
                return Invalid();
            if (canvas.bindings) {
                if (const auto valid = canvas.bindings->ValidateOwner(tree); valid.HasError())
                    return valid;
            }
            if (canvas.captures && (canvas.captures->Ownership() != tree.Instance().ownership ||
                                    canvas.captures->State() != UiPointerCaptureStoreState::Active || !canvas.captures->IsDrained()))
                return Invalid();
            return Controls(canvas);
        }

        /** @brief Checks the complete actual derived scroll owner against its exact immutable layout. */
        [[nodiscard]] Result<void> Clipping(const UiReloadCanvas &canvas) {
            if (!canvas.clipping)
                return canvas.clipped || !canvas.clipPolicies.empty() ? Invalid() : Result<void>::Success();
            if (!canvas.layout || !canvas.clipped || canvas.clipping->State() != UiLayoutClipEngineState::Active ||
                canvas.clipPolicies.size() != canvas.layout->Records().size())
                return Invalid();
            const auto &a = canvas.clipped->Descriptor();
            if (const auto &b = canvas.layout->Descriptor(); a.instance != b.instance || a.canvas != b.canvas || a.document != b.document ||
                                                             a.sources != b.sources || a.interaction != b.interaction)
                return Invalid();
            for (std::size_t i = 0; i < canvas.clipPolicies.size(); ++i)
                if (!canvas.clipPolicies[i].IsValid() || canvas.clipPolicies[i].element != canvas.layout->Records()[i].element)
                    return Invalid();
            return Result<void>::Success();
        }

        /** @brief Checks real arranged lineage and empty presentation admission, not stale copied receipts. */
        [[nodiscard]] Result<void> Geometry(const UiReloadCanvas &canvas) {
            if (canvas.layout.has_value() != canvas.layoutEngine.has_value() ||
                (canvas.layoutEngine && canvas.layoutEngine->State() != UiLayoutEngineState::Active))
                return Invalid();
            if (canvas.layout) {
                const auto &owner = canvas.layout->Descriptor();
                if (owner.instance != canvas.tree.Instance() || owner.canvas != canvas.tree.Canvas() ||
                    owner.document != canvas.tree.SourceDocument() || owner.sources.document != canvas.tree.SourceDocumentRevision() ||
                    owner.sources.tree != canvas.tree.Revision())
                    return Invalid();
            }
            for (std::size_t i = 0; i < canvas.presentations.size(); ++i) {
                const auto &presentation = canvas.presentations[i];
                if (presentation.Canvas() != canvas.tree.Canvas() || presentation.LastObservedSnapshot().IsValid())
                    return Invalid();
                for (std::size_t j = 0; j < i; ++j)
                    if (canvas.presentations[j].View() == presentation.View())
                        return Invalid();
            }
            return Clipping(canvas);
        }

        /** @brief Requires the route namespace to be backed by the exact allocator-issued canvas root. */
        [[nodiscard]] Result<void> Routes(const UiRuntimeInstance &instance, const UiReloadCanvas &canvas) {
            if (!canvas.routes)
                return Result<void>::Success();
            const auto root = canvas.tree.Root();
            if (const auto stack = canvas.routes->Stack(); root.HasError() || stack.ownership != canvas.tree.Instance().ownership ||
                                                           stack.slot != root.Value().handle.slot ||
                                                           canvas.routes->State() != UiScreenStackState::Active || !canvas.routes->Empty())
                return Invalid();
            const auto definitions = canvas.routes->Definitions();
            if (definitions.size() != instance.Routes().size())
                return Invalid();
            for (const auto &definition : definitions)
                if (std::ranges::find(instance.Routes(), definition) == instance.Routes().end())
                    return Invalid();
            return Result<void>::Success();
        }
    }  // namespace

    Result<void> Validate(const UiRuntimeInstance &instance, std::span<const UiReloadCanvas> canvases) {
        if (canvases.size() != instance.Canvases().size())
            return Invalid();
        std::size_t total = 0;
        for (std::size_t i = 0; i < canvases.size(); ++i) {
            const auto &canvas = canvases[i];
            const auto authored = std::ranges::find(instance.Canvases(), canvas.id, &UiCanvasDescriptor::id);
            if (authored == instance.Canvases().end())
                return Invalid();
            for (std::size_t j = 0; j < i; ++j)
                if (canvases[j].id == canvas.id || canvases[j].tree.Canvas() == canvas.tree.Canvas())
                    return Result<void>::Failure(MakeError(UiErrors::DocumentDuplicateIdentity));
            for (const auto &result :
                 {Tree(instance, canvas, authored->rootElement), InputOwners(canvas), Geometry(canvas), Routes(instance, canvas)})
                if (result.HasError())
                    return result;
            total += canvas.tree.Size();
        }
        if (total != instance.Elements().size())
            return Invalid();
        const auto slots = Namespace(canvases);
        return slots.HasError() ? Result<void>::Failure(slots.ErrorValue()) : Result<void>::Success();
    }

    Result<std::pair<std::uint64_t, std::uint64_t>> Namespace(std::span<const UiReloadCanvas> canvases) {
        std::uint64_t first = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t end = 0;
        for (std::size_t i = 0; i < canvases.size(); ++i) {
            const auto range = canvases[i].tree.ReservedSlots();
            if (range.HasError())
                return Result<std::pair<std::uint64_t, std::uint64_t>>::Failure(range.ErrorValue());
            const std::uint64_t begin = range.Value().FirstSlot();
            const std::uint64_t stop = begin + range.Value().SlotCount();
            for (std::size_t j = 0; j < i; ++j) {
                const auto other = canvases[j].tree.ReservedSlots().Value();
                if (begin < std::uint64_t(other.FirstSlot()) + other.SlotCount() && other.FirstSlot() < stop)
                    return Result<std::pair<std::uint64_t, std::uint64_t>>::Failure(MakeError(UiErrors::HandleStale));
            }
            first = std::min(first, begin);
            end = std::max(end, stop);
        }
        return Result<std::pair<std::uint64_t, std::uint64_t>>::Success({first, end});
    }
}  // namespace Horo::Runtime::Ui::UiReloadDetail
