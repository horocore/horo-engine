#include "UiCanvasPreview.h"

#include "Horo/Runtime/Ui/UiStyleAsset.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <set>

namespace Horo::Editor {
    namespace {
        using namespace Runtime::Ui;

        template <typename T> Result<T> Failure() {
            return Result<T>::Failure(MakeError(UiCanvasDocumentErrors::InvalidDocument));
        }

        UiCanvasViewportEvidence Viewport(const UiCanvasPreviewState &state) {
            UiCanvasViewportEvidence result;
            switch (state.device) {
                case UiCanvasPreviewDevice::Desktop:
                    result = {{1920, 1080}, {1, 1}, {}};
                    break;
                case UiCanvasPreviewDevice::Phone:
                    result = {{1170, 2532}, {3, 1}, {0, 141, 0, 102}};
                    break;
                case UiCanvasPreviewDevice::Tablet:
                    result = {{2048, 2732}, {2, 1}, {0, 48, 0, 40}};
                    break;
                case UiCanvasPreviewDevice::Count:
                    break;
            }
            if (state.rotated) {
                std::swap(result.pixelExtent.width, result.pixelExtent.height);
                const auto insets = result.safeAreaInsets;
                result.safeAreaInsets = {insets.bottom, insets.left, insets.top, insets.right};
            }
            return result;
        }

        std::int64_t Snap(std::int64_t value, const UiCanvasPreviewState &state, const bool horizontal) {
            if (!state.snap)
                return value;
            auto snapped = static_cast<std::int64_t>(std::llround(static_cast<double>(value) / state.grid)) * state.grid;
            const auto &guides = horizontal ? state.verticalGuides : state.horizontalGuides;
            const auto count = horizontal ? state.verticalGuideCount : state.horizontalGuideCount;
            for (std::size_t i = 0; i < count; ++i)
                if (std::abs(static_cast<std::int64_t>(guides[i]) - value) < std::abs(snapped - value))
                    snapped = guides[i];
            return snapped;
        }

        /** @brief Resolve real selected styles through the canonical theme projection after responsive base selection. */
        Result<void> ResolvePreviewStyles(const UiDocument &source, const UiElementTree &tree,
                                          const std::vector<const UiDocumentElement *> &authored,
                                          std::vector<UiLayoutElementDescriptor> &descriptors,
                                          const UiInspectorResourceSnapshot *resources) {
            const auto capacity = tree.Size();
            const auto instance = tree.Instance();
            const auto canvas = tree.Canvas();
            if (resources && resources->styles) {
                const auto &registry = *resources->styles;
                std::vector<UiStyleElementInput> inputs;
                std::vector<UiThemeSurfaceInput> surfaces;
                std::vector<std::vector<UiStyleClassReference>> classes;
                inputs.reserve(authored.size());
                surfaces.reserve(authored.size());
                classes.reserve(authored.size());
                for (std::size_t i = 0; i < authored.size(); ++i) {
                    auto input = authored[i] ? ProjectUiElementStyle(*authored[i], descriptors[i].element)
                                             : Result<UiStyleElementInput>::Success({.element = descriptors[i].element});
                    if (input.HasError())
                        return Result<void>::Failure(input.ErrorValue());
                    auto value = input.Value();
                    const auto asset = value.asset.IsValid() ? value.asset : resources->theme;
                    value.asset = asset;
                    if (value.typeClass.id.IsValid() && !value.typeClass.asset.IsValid())
                        value.typeClass.asset = asset;
                    classes.emplace_back(value.classes.begin(), value.classes.end());
                    for (auto &reference : classes.back())
                        if (!reference.asset.IsValid())
                            reference.asset = asset;
                    value.classes = classes.back();
                    inputs.push_back(value);
                    surfaces.push_back({value, descriptors[i].style, descriptors[i].intrinsic});
                }
                const auto properties = registry.Properties().size();
                if (properties > MaximumUiStyleProperties)
                    return Result<void>::Failure(MakeError(UiErrors::CapacityExceeded));
                auto resolverResult =
                    UiStyleResolver::Create({instance, canvas, source.Id(), capacity, static_cast<std::uint32_t>(properties),
                                             std::min(capacity, MaximumUiStructuralCommands), 2, registry.Generation(),
                                             UiStylePublicationRevision::Create(1).Value()});
                if (resolverResult.HasError())
                    return Result<void>::Failure(resolverResult.ErrorValue());
                auto resolver = std::move(resolverResult).Value();
                const UiStyleSourceRevisions styleSources{source.Revision(),
                                                          tree.Revision(),
                                                          registry.Generation(),
                                                          UiStyleContentRevision::Create(1).Value(),
                                                          UiStylePolicyRevision::Create(1).Value(),
                                                          UiInteractionRevision::Create(1).Value()};
                auto computed = resolver.Update(tree, registry, {styleSources, inputs});
                if (computed.HasError())
                    return Result<void>::Failure(computed.ErrorValue());
                for (std::size_t i = 0; i < descriptors.size(); ++i) {
                    const auto projection = ProjectUiThemeSurface(computed.Value(), surfaces[i], CoreUiStyleSurfaceProperties());
                    if (projection.HasError())
                        return Result<void>::Failure(projection.ErrorValue());
                    descriptors[i] = projection.Value().layout;
                }
            } else {
                for (const auto *element : authored)
                    if (element && element->presentation &&
                        (element->presentation->styleAsset.IsValid() || element->presentation->typeClass.id.IsValid() ||
                         !element->presentation->classes.empty() || !element->presentation->inlineStyle.empty()))
                        return Result<void>::Failure(MakeError(UiErrors::StyleReferenceInvalid));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc UiCanvasPreviewState::IsValid */
    bool UiCanvasPreviewState::IsValid() const noexcept {
        return device < UiCanvasPreviewDevice::Count && tool < UiCanvasLayoutTool::Count && zoomPercent >= 10 && zoomPercent <= 800 &&
               grid > 0 && horizontalGuideCount <= horizontalGuides.size() && verticalGuideCount <= verticalGuides.size();
    }

    /** @copydoc BuildUiCanvasPreview */
    Result<UiCanvasPreview> BuildUiCanvasPreview(const UiDocument &source, const UiCanvasPreviewState &state,
                                                 const DocumentInstanceId ownership, const UiInspectorResourceSnapshot *resources) {
        try {
            if (!state.IsValid() || !ownership.IsValid() || source.Canvases().empty())
                return Failure<UiCanvasPreview>();
            UiCanvasPreview result;
            result.viewport = Viewport(state);
            const auto resolved = ResolveUiScreenCanvasWithEvidence(source.Canvases().front(), result.viewport);
            if (resolved.HasError())
                return Result<UiCanvasPreview>::Failure(resolved.ErrorValue());
            result.canvas = resolved.Value();
            const auto owner = UiOwnershipGeneration::Create(ownership.Value());
            if (owner.HasError())
                return Result<UiCanvasPreview>::Failure(owner.ErrorValue());
            auto allocatorResult = UiElementSlotAllocator::Create(owner.Value());
            if (allocatorResult.HasError())
                return Result<UiCanvasPreview>::Failure(allocatorResult.ErrorValue());
            auto allocator = std::move(allocatorResult).Value();
            std::vector<UiElementDescriptor> elements;
            std::vector<const UiDocumentElement *> authored;
            std::set<UiElementId> included{source.Canvases().front().rootElement};
            for (const auto &element : source.Elements()) {
                if (included.contains(element.id) || included.contains(element.parent)) {
                    included.insert(element.id);
                    elements.push_back({element.id, element.parent});
                    authored.push_back(&element);
                }
            }
            if (elements.empty()) {
                elements.push_back({source.Canvases().front().rootElement, {}});
                authored.push_back(nullptr);
            }
            if (elements.size() > MaximumUiTreeElements)
                return Failure<UiCanvasPreview>();
            const auto capacity = static_cast<std::uint32_t>(elements.size());
            const RuntimeUiInstanceId instance{owner.Value(), 1, 1};
            const UiCanvasInstanceId canvas{owner.Value(), 2, 1};
            auto tree = UiElementTree::Create(allocator,
                                              {instance,
                                               canvas,
                                               source.Id(),
                                               source.Revision(),
                                               UiRuntimeTreeRevision::Create(1).Value(),
                                               {capacity, MaximumUiTreeDepth, 1}},
                                              elements);
            if (tree.HasError())
                return Result<UiCanvasPreview>::Failure(tree.ErrorValue());
            std::vector<UiLayoutElementDescriptor> descriptors;
            descriptors.reserve(elements.size());
            for (std::size_t i = 0; i < elements.size(); ++i) {
                const auto handle = tree.Value().Find(elements[i].id);
                if (handle.HasError())
                    return Result<UiCanvasPreview>::Failure(handle.ErrorValue());
                const auto presentation = authored[i] ? authored[i]->presentation : std::nullopt;
                descriptors.push_back({handle.Value(), presentation ? presentation->layout : UiLayoutStyle{},
                                       presentation ? presentation->intrinsic : UiLayoutIntrinsicSource{}});
            }
            std::ranges::sort(descriptors, {}, &UiLayoutElementDescriptor::element);
            auto profiles = UiPresentationProfileTable::CreateForAuthoring(source, tree.Value(), source.Canvases().front().id);
            if (profiles.HasError())
                return Result<UiCanvasPreview>::Failure(profiles.ErrorValue());
            const auto selected =
                profiles.Value().Select(tree.Value(),
                                        {.canvas = source.Canvases().front().id,
                                         .instance = instance,
                                         .runtimeCanvas = canvas,
                                         .tree = tree.Value().Revision(),
                                         .attachment = {owner.Value(), 3, 1},
                                         .revision = UiPresentationEvidenceRevision::Create(1).Value(),
                                         .content = result.canvas.logicalExtent,
                                         .formFactor = state.device == UiCanvasPreviewDevice::Desktop ? UiPresentationFormFactor::Desktop
                                                                                                      : UiPresentationFormFactor::Handheld,
                                         .available = state.device == UiCanvasPreviewDevice::Desktop ? UiPresentationCapabilities::Pointer
                                                                                                     : UiPresentationCapabilities::Touch});
            if (selected.HasError())
                return Result<UiCanvasPreview>::Failure(selected.ErrorValue());
            if (const auto applied = profiles.Value().ApplyLayouts(tree.Value(), selected.Value(), descriptors); applied.HasError())
                return Result<UiCanvasPreview>::Failure(applied.ErrorValue());
            result.profile = selected.Value().profile;
            if (const auto styled = ResolvePreviewStyles(source, tree.Value(), authored, descriptors, resources); styled.HasError())
                return Result<UiCanvasPreview>::Failure(styled.ErrorValue());
            auto evaluator = UiDeclarativeLayoutEvaluator::Create(descriptors);
            if (evaluator.HasError())
                return Result<UiCanvasPreview>::Failure(evaluator.ErrorValue());
            auto engine = UiLayoutEngine::Create({instance, canvas, source.Id(), capacity, std::min(capacity, MaximumUiStructuralCommands),
                                                  2, UiInteractionRevision::Create(1).Value()});
            if (engine.HasError())
                return Result<UiCanvasPreview>::Failure(engine.ErrorValue());
            const UiLogicalExtent extent{result.canvas.logicalExtent.width, result.canvas.logicalExtent.height};
            const UiLayoutSourceRevisions revisions{source.Revision(),
                                                    UiRuntimeTreeRevision::Create(1).Value(),
                                                    UiLayoutContentRevision::Create(1).Value(),
                                                    UiLayoutStyleRevision::Create(1).Value(),
                                                    UiLayoutIntrinsicRevision::Create(1).Value(),
                                                    UiLayoutCanvasRevision::Create(1).Value(),
                                                    UiLayoutPolicyRevision::Create(1).Value()};
            auto layout =
                engine.Value().Update(tree.Value(),
                                      {revisions, {extent, extent}, {{0, 0}, extent}, &evaluator.Value(), result.canvas.fontScale});
            if (layout.HasError())
                return Result<UiCanvasPreview>::Failure(layout.ErrorValue());
            result.boxes.reserve(elements.size());
            for (const auto &record : layout.Value().Records()) {
                const auto element = tree.Value().Get(record.element);
                if (element.HasError())
                    return Result<UiCanvasPreview>::Failure(element.ErrorValue());
                result.boxes.push_back({element.Value().id, record.arrangement.contentBox});
            }
            return Result<UiCanvasPreview>::Success(std::move(result));
        } catch (const std::bad_alloc &) {
            return Result<UiCanvasPreview>::Failure(MakeError(UiErrors::CapacityExceeded));
        }
    }

    /** @copydoc PrepareUiCanvasLayoutGesture */
    Result<UiCanvasSetPresentation> PrepareUiCanvasLayoutGesture(const UiDocumentElement &element, const UiLogicalRect box,
                                                                 const UiLogicalPoint delta, const UiLogicalExtent parentExtent,
                                                                 const UiCanvasPreviewState &state) {
        try {
            if (!state.IsValid() || !box.IsValid() || !element.parent.IsValid() || !element.properties.empty() ||
                state.tool == UiCanvasLayoutTool::Select)
                return Failure<UiCanvasSetPresentation>();
            auto presentation = element.presentation.value_or(UiElementPresentation{});
            auto &layout = presentation.layout;
            // Flow placement must be deliberately changed in the inspector before a visual move can author offsets.
            if (layout.positioning != UiLayoutPositioning::Absolute)
                return Failure<UiCanvasSetPresentation>();
            const auto checked = [](const std::int64_t value) {
                return value >= std::numeric_limits<UiScalar>::min() && value <= std::numeric_limits<UiScalar>::max();
            };
            if (state.tool == UiCanvasLayoutTool::Move) {
                const auto x = static_cast<std::int64_t>(layout.offsets.left) +
                               Snap(static_cast<std::int64_t>(box.origin.x) + delta.x, state, true) - box.origin.x;
                const auto y = static_cast<std::int64_t>(layout.offsets.top) +
                               Snap(static_cast<std::int64_t>(box.origin.y) + delta.y, state, false) - box.origin.y;
                if (!checked(x) || !checked(y))
                    return Failure<UiCanvasSetPresentation>();
                layout.offsets.left = static_cast<UiScalar>(x);
                layout.offsets.top = static_cast<UiScalar>(y);
            } else if (state.tool == UiCanvasLayoutTool::Resize) {
                const auto width = Snap(static_cast<std::int64_t>(box.origin.x) + box.extent.width + delta.x, state, true) - box.origin.x;
                const auto height =
                    Snap(static_cast<std::int64_t>(box.origin.y) + box.extent.height + delta.y, state, false) - box.origin.y;
                if (!checked(width) || !checked(height) || width < 0 || height < 0)
                    return Failure<UiCanvasSetPresentation>();
                layout.width = UiLength::Dip(static_cast<UiScalar>(width));
                layout.height = UiLength::Dip(static_cast<UiScalar>(height));
            } else {
                if (parentExtent.width <= 0 || parentExtent.height <= 0)
                    return Failure<UiCanvasSetPresentation>();
                const auto x = static_cast<std::int64_t>(layout.anchors.horizontal.start.value_or(0)) +
                               static_cast<std::int64_t>(delta.x) * 64 / parentExtent.width;
                const auto y = static_cast<std::int64_t>(layout.anchors.vertical.start.value_or(0)) +
                               static_cast<std::int64_t>(delta.y) * 64 / parentExtent.height;
                if (x < 0 || x > 64 || y < 0 || y > 64)
                    return Failure<UiCanvasSetPresentation>();
                layout.anchors.horizontal.start = static_cast<UiScalar>(x);
                layout.anchors.vertical.start = static_cast<UiScalar>(y);
            }
            if (!presentation.IsValid())
                return Failure<UiCanvasSetPresentation>();
            return Result<UiCanvasSetPresentation>::Success({element.id, std::move(presentation)});
        } catch (const std::bad_alloc &) {
            return Result<UiCanvasSetPresentation>::Failure(MakeError(UiErrors::CapacityExceeded));
        }
    }
}  // namespace Horo::Editor
