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

        struct PreviewStyleInputs final {
            std::vector<UiStyleElementInput> inputs;
            std::vector<UiThemeSurfaceInput> surfaces;
            std::vector<std::vector<UiStyleClassReference>> classes;
        };

        /** @brief Keeps copied qualified classes alive through the synchronous canonical style resolution. */
        Result<PreviewStyleInputs> PreparePreviewStyleInputs(const std::vector<const UiDocumentElement *> &authored,
                                                             const std::vector<UiLayoutElementDescriptor> &descriptors,
                                                             RuntimeStyleAssetId theme) {
            PreviewStyleInputs result;
            result.inputs.reserve(authored.size());
            result.surfaces.reserve(authored.size());
            result.classes.reserve(authored.size());
            for (std::size_t i = 0; i < authored.size(); ++i) {
                auto input = authored[i] ? ProjectUiElementStyle(*authored[i], descriptors[i].element)
                                         : Result<UiStyleElementInput>::Success({.element = descriptors[i].element});
                if (input.HasError())
                    return Result<PreviewStyleInputs>::Failure(input.ErrorValue());
                auto value = input.Value();
                const auto asset = value.asset.IsValid() ? value.asset : theme;
                value.asset = asset;
                if (value.typeClass.id.IsValid() && !value.typeClass.asset.IsValid())
                    value.typeClass.asset = asset;
                result.classes.emplace_back(value.classes.begin(), value.classes.end());
                for (auto &reference : result.classes.back())
                    if (!reference.asset.IsValid())
                        reference.asset = asset;
                value.classes = result.classes.back();
                result.inputs.push_back(value);
                result.surfaces.push_back({value, descriptors[i].style, descriptors[i].intrinsic});
            }
            return Result<PreviewStyleInputs>::Success(std::move(result));
        }

        /** @brief Run the sole Runtime UI style resolver against exact preview tree and actual registry evidence. */
        Result<UiComputedStyleSnapshot> ComputePreviewStyles(const UiDocument &source, const UiElementTree &tree,
                                                             const RuntimeStyleRegistry &registry,
                                                             std::span<const UiStyleElementInput> inputs) {
            const auto capacity = tree.Size();
            const auto instance = tree.Instance();
            const auto canvas = tree.Canvas();
            const auto properties = registry.Properties().size();
            if (properties > MaximumUiStyleProperties)
                return Result<UiComputedStyleSnapshot>::Failure(MakeError(UiErrors::CapacityExceeded));
            auto resolverResult = UiStyleResolver::Create({instance, canvas, source.Id(), capacity, static_cast<std::uint32_t>(properties),
                                                           std::min(capacity, MaximumUiStructuralCommands), 2, registry.Generation(),
                                                           UiStylePublicationRevision::Create(1).Value()});
            if (resolverResult.HasError())
                return Result<UiComputedStyleSnapshot>::Failure(resolverResult.ErrorValue());
            auto resolver = std::move(resolverResult).Value();
            const UiStyleSourceRevisions styleSources{source.Revision(),
                                                      tree.Revision(),
                                                      registry.Generation(),
                                                      UiStyleContentRevision::Create(1).Value(),
                                                      UiStylePolicyRevision::Create(1).Value(),
                                                      UiInteractionRevision::Create(1).Value()};
            auto computed = resolver.Update(tree, registry, {styleSources, inputs});
            if (computed.HasError())
                return Result<UiComputedStyleSnapshot>::Failure(computed.ErrorValue());
            return computed;
        }

        /** @brief Resolve real selected styles through the canonical theme projection after responsive base selection. */
        Result<void> ResolvePreviewStyles(const UiDocument &source, const UiElementTree &tree,
                                          const std::vector<const UiDocumentElement *> &authored,
                                          std::vector<UiLayoutElementDescriptor> &descriptors,
                                          const UiInspectorResourceSnapshot *resources) {
            if (resources && resources->styles) {
                const auto &registry = *resources->styles;
                auto prepared = PreparePreviewStyleInputs(authored, descriptors, resources->theme);
                if (prepared.HasError())
                    return Result<void>::Failure(prepared.ErrorValue());
                const auto &inputs = prepared.Value().inputs;
                const auto &surfaces = prepared.Value().surfaces;
                auto computed = ComputePreviewStyles(source, tree, registry, inputs);
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

        struct PreviewSource final {
            UiElementTree tree;
            std::vector<const UiDocumentElement *> authored;
            std::vector<UiLayoutElementDescriptor> descriptors;
        };

        /** @brief Copy canonical layout inputs with handles issued by this exact authoring tree. */
        Result<std::vector<UiLayoutElementDescriptor>> CreatePreviewLayouts(const UiElementTree &tree,
                                                                            const std::vector<const UiDocumentElement *> &authored) {
            std::vector<UiLayoutElementDescriptor> descriptors;
            descriptors.reserve(authored.size());
            for (std::size_t i = 0; i < authored.size(); ++i) {
                const auto handle = authored[i] ? tree.Find(authored[i]->id) : Result<UiElementHandle>::Success(tree.Root().Value().handle);
                if (handle.HasError())
                    return Result<std::vector<UiLayoutElementDescriptor>>::Failure(handle.ErrorValue());
                const auto presentation = authored[i] ? authored[i]->presentation : std::nullopt;
                descriptors.push_back({handle.Value(), presentation ? presentation->layout : UiLayoutStyle{},
                                       presentation ? presentation->intrinsic : UiLayoutIntrinsicSource{}});
            }
            std::ranges::sort(descriptors, {}, &UiLayoutElementDescriptor::element);
            return Result<std::vector<UiLayoutElementDescriptor>>::Success(std::move(descriptors));
        }

        /** @brief Admit one complete isolated source subtree and typed layout inputs outside draw work. */
        Result<PreviewSource> CreatePreviewSource(const UiDocument &source, UiOwnershipGeneration owner) {
            auto allocatorResult = UiElementSlotAllocator::Create(owner);
            if (allocatorResult.HasError())
                return Result<PreviewSource>::Failure(allocatorResult.ErrorValue());
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
                return Failure<PreviewSource>();
            const auto capacity = static_cast<std::uint32_t>(elements.size());
            const RuntimeUiInstanceId instance{owner, 1, 1};
            const UiCanvasInstanceId canvas{owner, 2, 1};
            auto tree = UiElementTree::Create(allocator,
                                              {instance,
                                               canvas,
                                               source.Id(),
                                               source.Revision(),
                                               UiRuntimeTreeRevision::Create(1).Value(),
                                               {capacity, MaximumUiTreeDepth, 1}},
                                              elements);
            if (tree.HasError())
                return Result<PreviewSource>::Failure(tree.ErrorValue());
            auto layouts = CreatePreviewLayouts(tree.Value(), authored);
            if (layouts.HasError())
                return Result<PreviewSource>::Failure(layouts.ErrorValue());
            return Result<PreviewSource>::Success({std::move(tree).Value(), std::move(authored), std::move(layouts).Value()});
        }

        /** @brief Apply the canonical profile selector to the actual admitted source and device evidence. */
        Result<std::optional<UiPresentationProfileId>> ApplyPreviewProfiles(const UiDocument &source, const UiElementTree &tree,
                                                                            const UiCanvasPreviewState &state,
                                                                            const UiResolvedScreenCanvas &resolved,
                                                                            std::vector<UiLayoutElementDescriptor> &descriptors) {
            auto profiles = UiPresentationProfileTable::CreateForAuthoring(source, tree, source.Canvases().front().id);
            if (profiles.HasError())
                return Result<std::optional<UiPresentationProfileId>>::Failure(profiles.ErrorValue());
            const auto selected =
                profiles.Value().Select(tree,
                                        {.canvas = source.Canvases().front().id,
                                         .instance = tree.Instance(),
                                         .runtimeCanvas = tree.Canvas(),
                                         .tree = tree.Revision(),
                                         .attachment = {tree.Instance().ownership, 3, 1},
                                         .revision = UiPresentationEvidenceRevision::Create(1).Value(),
                                         .content = resolved.logicalExtent,
                                         .formFactor = state.device == UiCanvasPreviewDevice::Desktop ? UiPresentationFormFactor::Desktop
                                                                                                      : UiPresentationFormFactor::Handheld,
                                         .available = state.device == UiCanvasPreviewDevice::Desktop ? UiPresentationCapabilities::Pointer
                                                                                                     : UiPresentationCapabilities::Touch});
            if (selected.HasError())
                return Result<std::optional<UiPresentationProfileId>>::Failure(selected.ErrorValue());
            if (const auto applied = profiles.Value().ApplyLayouts(tree, selected.Value(), descriptors); applied.HasError())
                return Result<std::optional<UiPresentationProfileId>>::Failure(applied.ErrorValue());
            return Result<std::optional<UiPresentationProfileId>>::Success(selected.Value().profile);
        }

        /** @brief Evaluate the complete shared layout authority and return its immutable publication lease. */
        Result<UiLayoutSnapshot> EvaluatePreviewLayout(const UiDocument &source, const UiElementTree &tree,
                                                       const std::vector<UiLayoutElementDescriptor> &descriptors,
                                                       const UiResolvedScreenCanvas &resolved) {
            const auto capacity = tree.Size();
            const auto instance = tree.Instance();
            const auto canvas = tree.Canvas();
            auto evaluator = UiDeclarativeLayoutEvaluator::Create(descriptors);
            if (evaluator.HasError())
                return Result<UiLayoutSnapshot>::Failure(evaluator.ErrorValue());
            auto engine = UiLayoutEngine::Create({instance, canvas, source.Id(), capacity, std::min(capacity, MaximumUiStructuralCommands),
                                                  2, UiInteractionRevision::Create(1).Value()});
            if (engine.HasError())
                return Result<UiLayoutSnapshot>::Failure(engine.ErrorValue());
            const UiLogicalExtent extent{resolved.logicalExtent.width, resolved.logicalExtent.height};
            const UiLayoutSourceRevisions revisions{source.Revision(),
                                                    UiRuntimeTreeRevision::Create(1).Value(),
                                                    UiLayoutContentRevision::Create(1).Value(),
                                                    UiLayoutStyleRevision::Create(1).Value(),
                                                    UiLayoutIntrinsicRevision::Create(1).Value(),
                                                    UiLayoutCanvasRevision::Create(1).Value(),
                                                    UiLayoutPolicyRevision::Create(1).Value()};
            auto layout =
                engine.Value().Update(tree, {revisions, {extent, extent}, {{0, 0}, extent}, &evaluator.Value(), resolved.fontScale});
            if (layout.HasError())
                return Result<UiLayoutSnapshot>::Failure(layout.ErrorValue());
            return layout;
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
            auto prepared = CreatePreviewSource(source, owner.Value());
            if (prepared.HasError())
                return Result<UiCanvasPreview>::Failure(prepared.ErrorValue());
            auto values = std::move(prepared).Value();
            const auto &tree = values.tree;
            auto &descriptors = values.descriptors;
            auto selected = ApplyPreviewProfiles(source, tree, state, result.canvas, descriptors);
            if (selected.HasError())
                return Result<UiCanvasPreview>::Failure(selected.ErrorValue());
            result.profile = selected.Value();
            if (const auto styled = ResolvePreviewStyles(source, tree, values.authored, descriptors, resources); styled.HasError())
                return Result<UiCanvasPreview>::Failure(styled.ErrorValue());
            auto layout = EvaluatePreviewLayout(source, tree, descriptors, result.canvas);
            if (layout.HasError())
                return Result<UiCanvasPreview>::Failure(layout.ErrorValue());
            result.boxes.reserve(tree.Size());
            for (const auto &record : layout.Value().Records()) {
                const auto element = tree.Get(record.element);
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

    /** @copydoc ExecuteUiCanvasLayoutGesture */
    Result<void> ExecuteUiCanvasLayoutGesture(UiCanvasDocument &owner, const UiCanvasDocumentStateId state,
                                              const std::uint64_t expectedSelection, const std::uint64_t currentSelection,
                                              const UiCanvasSetPresentation &command) {
        if (expectedSelection == 0 || expectedSelection != currentSelection ||
            currentSelection == std::numeric_limits<std::uint64_t>::max() || owner.Selection() != command.element)
            return Result<void>::Failure(MakeError(UiCanvasDocumentErrors::Conflict));
        return owner.Execute(state, command);
    }
}  // namespace Horo::Editor
