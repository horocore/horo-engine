#include "RuntimeUiPaintProjectSources.h"

#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"
#include "Horo/Runtime/Ui/UiStyleAsset.h"
#include "Horo/Runtime/Ui/UiVisualAccessibility.h"
#include "RuntimeUiPaintHostInternal.h"
#include "RuntimeUiPaintPackagedKernel.h"
#include "RuntimeUiPaintProjectKernel.h"

#include <algorithm>
#include <array>
#include <limits>

namespace Horo::Application::Internal {
    using namespace Runtime::Ui;

    namespace {
        template <class T> Result<T> Invalid(const ErrorCodeDescriptor &error, const std::string_view message = {}) {
            return Result<T>::Failure(MakeError(error, std::string{message}));
        }

        template <class T> T Initial() {
            return T::Create(1).Value();
        }

        /** @brief Accumulates the exact parent-to-local transforms without decomposing inherited scale or shear. */
        Result<Math::Mat4> CanvasToScene(const Runtime::RuntimeSceneView &scene, Runtime::RuntimeEntityView entity) {
            constexpr std::size_t MaximumCanvasAncestors = 64;
            auto world = Math::Mat4::Identity();
            for (std::size_t depth = 0; depth < MaximumCanvasAncestors; ++depth) {
                if (!entity.localTransform)
                    return Invalid<Math::Mat4>(UiErrors::CanvasSpaceInvalid);
                const auto local = entity.localTransform->TryToMatrix();
                if (local.HasError())
                    return Result<Math::Mat4>::Failure(local.ErrorValue());
                world = Math::Multiply(local.Value(), world);
                if (!entity.parent)
                    return Result<Math::Mat4>::Success(world);
                const auto parent = scene.Get(*entity.parent);
                if (parent.HasError())
                    return Result<Math::Mat4>::Failure(parent.ErrorValue());
                entity = parent.Value();
            }
            return Invalid<Math::Mat4>(UiErrors::CapacityExceeded);
        }

        /** @brief Derives the same exact affine basis for rendered geometry and eventual interaction receipt. */
        Result<UiWorldCanvasProjection> CanvasPlane(const Runtime::RuntimeSceneView &scene, const Runtime::RuntimeEntityView &owner) {
            using Projection = Result<UiWorldCanvasProjection>;
            const auto world = CanvasToScene(scene, owner);
            if (world.HasError())
                return Projection::Failure(world.ErrorValue());
            const auto origin = Math::TryTransformPoint(world.Value(), {-0.5F, 0.5F, 0.0F});
            if (origin.HasError())
                return Projection::Failure(origin.ErrorValue());
            const UiWorldCanvasProjection plane{origin.Value(), Math::TransformDirection(world.Value(), {1.0F, 0.0F, 0.0F}),
                                                Math::TransformDirection(world.Value(), {0.0F, -1.0F, 0.0F})};
            return plane.IsValid() ? Projection::Success(plane) : Invalid<UiWorldCanvasProjection>(UiErrors::CanvasSpaceInvalid);
        }

        /** @brief Copies the entity with its registered asset before leaving the private Scene candidate read. */
        struct SceneCanvasSource final {
            Runtime::EntityRef entity;
            UiCanvasAssetReference reference;
        };

        /** @brief Holds the selected canvas's raster metrics and optional exact Scene placement. */
        struct ProjectCanvasProjection final {
            UiResolvedScreenCanvas raster;
            std::optional<RuntimeUiPaintWorldCanvas> world;
            bool sceneCanvas{};
        };

        /** @brief Preserves actual authored child order under one selected canvas; no component pointer escapes preparation. */
        Result<std::vector<UiElementDescriptor>> TreeElements(const CookedUiDocument &document, const UiCanvasDescriptor &canvas,
                                                              const std::uint32_t capacity) {
            std::vector<UiElementDescriptor> result{{canvas.rootElement, {}}};
            std::size_t nextParent = 0;
            while (nextParent < result.size()) {
                const auto parent = result[nextParent++].id;
                for (const auto &element : document.Elements()) {
                    if (element.parent != parent)
                        continue;
                    if (result.size() == capacity)
                        return Invalid<std::vector<UiElementDescriptor>>(UiErrors::CapacityExceeded);
                    result.emplace_back(element.id, element.parent);
                }
            }
            return Result<std::vector<UiElementDescriptor>>::Success(std::move(result));
        }

        /** @brief Loads only actual admitted cooked style namespaces; no source fallback or fabricated default asset is supplied. */
        Result<RuntimeStyleRegistry> Registry(const UiRuntimeAssetLoadResult &loaded) {
            auto schema = CreateCoreUiStyleSchema({});
            if (schema.HasError())
                return Result<RuntimeStyleRegistry>::Failure(schema.ErrorValue());
            std::vector<UiStyleAssetDocument> documents;
            for (const auto &asset : loaded.assets)
                if (asset.dependency.expectedType.Value() == UiStyleAssetTypeName) {
                    if (!asset.payload)
                        return Invalid<RuntimeStyleRegistry>(UiErrors::AssetMissing);
                    auto decoded = DecodeCookedUiStyleAsset(*asset.payload, asset.dependency.asset, schema.Value());
                    if (decoded.HasError())
                        return Result<RuntimeStyleRegistry>::Failure(decoded.ErrorValue());
                    documents.push_back(std::move(decoded).Value());
                }
            if (documents.empty())
                return RuntimeStyleRegistry::Create({schema.Value().properties, {}}, Initial<RuntimeStyleGeneration>());
            return PrepareUiStyleRegistry(documents, schema.Value(), Initial<RuntimeStyleGeneration>());
        }

        /** @brief Builds private actual layout owners from canonical authored inputs; the animation owner recomputes its own baseline. */
        Result<void> Layout(UiReloadCanvas &canvas, const CookedUiDocument &document, const UiResolvedScreenCanvas &projection) {
            const auto count = static_cast<std::uint32_t>(canvas.tree.Size());
            std::vector<UiLayoutElementDescriptor> descriptors;
            descriptors.reserve(count);
            for (const auto &element : document.Elements()) {
                const auto handle = canvas.tree.Find(element.id);
                if (handle.HasError())
                    continue;
                auto projected = ProjectUiElementLayout(element, handle.Value());
                if (projected.HasError())
                    return Result<void>::Failure(projected.ErrorValue());
                if (projected.Value().intrinsic.kind != UiLayoutIntrinsicKind::None)
                    return Invalid<void>(UiErrors::LayoutIntrinsicUnavailable,
                                         "This authored source requires its actual text/image intrinsic owner.");
                descriptors.push_back(std::move(projected).Value());
            }
            std::ranges::sort(descriptors, {}, &UiLayoutElementDescriptor::element);
            auto evaluator = UiDeclarativeLayoutEvaluator::Create(descriptors);
            if (evaluator.HasError())
                return Result<void>::Failure(evaluator.ErrorValue());
            auto engine = UiLayoutEngine::Create(
                {canvas.tree.Instance(), canvas.tree.Canvas(), document.Id(), count, count, 8, Initial<UiInteractionRevision>()});
            if (engine.HasError())
                return Result<void>::Failure(engine.ErrorValue());
            canvas.layoutEngine.emplace(std::move(engine).Value());
            const UiLayoutSourceRevisions source{document.SourceRevision(),
                                                 canvas.tree.Revision(),
                                                 Initial<UiLayoutContentRevision>(),
                                                 Initial<UiLayoutStyleRevision>(),
                                                 Initial<UiLayoutIntrinsicRevision>(),
                                                 Initial<UiLayoutCanvasRevision>(),
                                                 Initial<UiLayoutPolicyRevision>()};
            const UiLayoutConstraints bounds{{0, 0}, {projection.logicalExtent.width, projection.logicalExtent.height}};
            auto layout = canvas.layoutEngine->Update(canvas.tree, {source, bounds, {{0, 0}, bounds.maximum}, &evaluator.Value()});
            if (layout.HasError())
                return Result<void>::Failure(layout.ErrorValue());
            canvas.layout.emplace(std::move(layout).Value());
            auto clips = UiLayoutClipEngine::Create({canvas.tree.Instance(), canvas.tree.Canvas(), document.Id(), count, count, count, 8});
            if (clips.HasError())
                return Result<void>::Failure(clips.ErrorValue());
            canvas.clipping.emplace(std::move(clips).Value());
            for (const auto &record : canvas.layout->Records())
                canvas.clipPolicies.push_back({record.element, UiLayoutOverflowPolicy::Visible, {}});
            auto clipped = canvas.clipping->Update(canvas.tree, *canvas.layout, {canvas.clipPolicies, {}});
            if (clipped.HasError())
                return Result<void>::Failure(clipped.ErrorValue());
            canvas.clipped.emplace(std::move(clipped).Value());
            return Result<void>::Success();
        }

        /** @brief Copies the real source's owned style/layout values; no theme runtime or alternate cascade is created. */
        Result<UiAnimationCanvasDefinition> Definition(const CookedUiDocument &document, const UiReloadCanvas &canvas) {
            UiAnimationCanvasDefinition result;
            result.canvas = canvas.id;
            result.content = Initial<UiStyleContentRevision>();
            result.resolvedPolicy = Initial<UiStylePolicyRevision>();
            for (const auto &element : document.Elements()) {
                const auto handle = canvas.tree.Find(element.id);
                if (handle.HasError())
                    continue;
                if (const auto *type = UiElementCatalog::Find(element.type.Value()); !type || !type->surface)
                    return Invalid<UiAnimationCanvasDefinition>(UiErrors::CapabilityUnsupported,
                                                                "The authored control/text/image requires its actual source component "
                                                                "owner; it is not replaced by solid paint.");
                auto layout = ProjectUiElementLayout(element, handle.Value());
                auto style = ProjectUiElementStyle(element, handle.Value());
                if (layout.HasError())
                    return Result<UiAnimationCanvasDefinition>::Failure(layout.ErrorValue());
                if (style.HasError())
                    return Result<UiAnimationCanvasDefinition>::Failure(style.ErrorValue());
                const auto &input = style.Value();
                result.elements.push_back({element.id,
                                           input.asset,
                                           input.typeClass,
                                           {input.classes.begin(), input.classes.end()},
                                           {input.inlineProperties.begin(), input.inlineProperties.end()},
                                           {},
                                           layout.Value().style,
                                           layout.Value().intrinsic});
                for (const auto [property, field] : std::array{std::pair{UiCoreStyleProperty::Width, UiAnimationLayoutField::Width},
                                                               std::pair{UiCoreStyleProperty::Height, UiAnimationLayoutField::Height},
                                                               std::pair{UiCoreStyleProperty::Padding, UiAnimationLayoutField::Padding}})
                    result.layoutBindings.emplace_back(element.id, CoreUiStylePropertyId(property), field,
                                                       UiAnimationLayoutValuePolicy::ExplicitOriginsOnly);
            }
            return Result<UiAnimationCanvasDefinition>::Success(std::move(result));
        }

        /** @brief Owned per-canvas derived inputs, prepared before the same authored generation is moved into publication. */
        struct PreparedPaintCanvas final {
            UiStyleResolver styles;
            UiAnimationCanvasDefinition definition;
            UiRenderExtractorDescriptor extraction;
            UiRenderGeometryArenaDescriptor geometry;
        };

        /** @brief Prepares canonical layout, cascade and a registered inactive view without publishing input or native receipts. */
        Result<PreparedPaintCanvas> PrepareCanvas(UiReloadCanvas &canvas, const CookedUiDocument &document,
                                                  const RuntimeStyleRegistry &registry, const UiResolvedScreenCanvas &resolved,
                                                  const UiOwnershipGeneration ownership) {
            if (const auto layout = Layout(canvas, document, resolved); layout.HasError())
                return Result<PreparedPaintCanvas>::Failure(layout.ErrorValue());
            auto definition = Definition(document, canvas);
            if (definition.HasError())
                return Result<PreparedPaintCanvas>::Failure(definition.ErrorValue());
            const auto count = static_cast<std::uint32_t>(definition.Value().elements.size());
            auto styles = UiStyleResolver::Create({canvas.tree.Instance(), canvas.tree.Canvas(), document.Id(), count,
                                                   static_cast<std::uint32_t>(registry.Properties().size()), count, 8,
                                                   Initial<RuntimeStyleGeneration>(), Initial<UiStylePublicationRevision>()});
            if (styles.HasError())
                return Result<PreparedPaintCanvas>::Failure(styles.ErrorValue());
            const UiRenderViewId view{ownership, canvas.tree.Canvas().slot, 1};
            auto presentation = UiPresentedInteractionState::Create(view, canvas.tree.Canvas());
            if (presentation.HasError())
                return Result<PreparedPaintCanvas>::Failure(presentation.ErrorValue());
            // The actual source view is registered while this generation is private. Only a later native receipt enables input.
            canvas.presentations.push_back(std::move(presentation).Value());
            // A registered focus graph supplies at most one focus indicator. Other cues belong to actual controls.
            const auto controls = static_cast<std::uint32_t>(canvas.controls.size());
            const auto cues = controls * UiVisualIndicatorLimits::ControlCommands;
            const auto commands = count + (canvas.focus ? count : controls);
            const auto quads = count + cues + (canvas.focus ? UiVisualIndicatorLimits::FocusQuads : 0);
            const UiRenderExtractorDescriptor extraction{view, {commands, 1, 1, count, 0, count, 1, 0}, 8};
            const UiRenderGeometryArenaDescriptor geometry{view, {quads * 4, quads * 6, commands}, 8};
            return Result<PreparedPaintCanvas>::Success({std::move(styles).Value(), std::move(definition).Value(), extraction, geometry});
        }

        /** @brief Derives the source viewport from the already resolved canonical canvas; clocks remain participant-owned. */
        Runtime::UiAnimationRuntimeConfig RuntimeConfig(const UiResolvedScreenCanvas &projection) {
            return {Runtime::UiAnimationApplication::Runtime,
                    {{{0, 0}, {projection.logicalExtent.width, projection.logicalExtent.height}},
                     {{0, 0}, {projection.logicalExtent.width, projection.logicalExtent.height}},
                     projection.fontScale,
                     Initial<UiLayoutIntrinsicRevision>(),
                     Initial<UiLayoutCanvasRevision>(),
                     Initial<UiLayoutPolicyRevision>()}};
        }
    }  // namespace

    /** @copydoc ResolveRuntimeUiPaintWorldCanvas */
    Result<RuntimeUiPaintWorldCanvas> ResolveRuntimeUiPaintWorldCanvas(const Runtime::RuntimeSceneView scene,
                                                                       const Runtime::EntityRef entity,
                                                                       const UiRuntimeAssetLoadResult &loaded,
                                                                       const std::uint32_t maximumRasterAxis) {
        using Placement = Result<RuntimeUiPaintWorldCanvas>;
        const auto owner = scene.Get(entity);
        if (owner.HasError())
            return Placement::Failure(owner.ErrorValue());
        const auto *components = owner.Value().components;
        if (!components || !components->uiCanvas)
            return Invalid<RuntimeUiPaintWorldCanvas>(UiErrors::CanvasReferenceInvalid);
        const auto &reference = components->uiCanvas->canvas;
        const auto &document = loaded.document;
        if (loaded.rootAsset != reference.asset || document.Id() != reference.document ||
            document.SourceRevision() < reference.minimumRevision)
            return Invalid<RuntimeUiPaintWorldCanvas>(UiErrors::CanvasReferenceInvalid);
        if (loaded.registryRevision != scene.AssetRegistryRevision())
            return Invalid<RuntimeUiPaintWorldCanvas>(UiErrors::RevisionStale);
        const auto selected = std::ranges::find(document.Canvases(), reference.canvas, &UiCanvasDescriptor::id);
        if (selected == document.Canvases().end())
            return Invalid<RuntimeUiPaintWorldCanvas>(UiErrors::CanvasReferenceInvalid);
        const auto &canvas = *selected;
        const auto logical = ResolveUiWorldCanvas(canvas);
        if (logical.HasError())
            return Placement::Failure(logical.ErrorValue());
        const auto [width, height] = canvas.referenceResolution;
        if (width > maximumRasterAxis || height > maximumRasterAxis)
            return Invalid<RuntimeUiPaintWorldCanvas>(UiErrors::CapacityExceeded);
        const auto plane = CanvasPlane(scene, owner.Value());
        if (plane.HasError())
            return Placement::Failure(plane.ErrorValue());
        return Placement::Success({entity,
                                   reference,
                                   document.SourceRevision(),
                                   loaded.registryRevision,
                                   plane.Value(),
                                   logical.Value(),
                                   {width, height},
                                   canvas.presentation.fontScale});
    }

    /** @copydoc CaptureRuntimeUiPaintWorldCanvasPlane */
    Result<UiWorldCanvasProjection> CaptureRuntimeUiPaintWorldCanvasPlane(const Runtime::RuntimeSceneView scene,
                                                                          const RuntimeUiPaintWorldCanvas &canvas) {
        const auto owner = scene.Get(canvas.entity);
        if (owner.HasError())
            return Result<UiWorldCanvasProjection>::Failure(owner.ErrorValue());
        const auto *components = owner.Value().components;
        if (!components || !components->uiCanvas || components->uiCanvas->canvas != canvas.reference)
            return Invalid<UiWorldCanvasProjection>(UiErrors::CanvasReferenceInvalid);
        if (scene.AssetRegistryRevision() != canvas.registryRevision)
            return Invalid<UiWorldCanvasProjection>(UiErrors::RevisionStale);
        return CanvasPlane(scene, owner.Value());
    }

    /** @copydoc BuildRuntimeUiPaintWorldCanvasTransform */
    Result<Math::Mat4> BuildRuntimeUiPaintWorldCanvasTransform(const UiWorldCanvasProjection &plane) {
        if (!plane.IsValid())
            return Invalid<Math::Mat4>(UiErrors::CanvasSpaceInvalid);
        const auto right = plane.horizontalExtent;
        const auto up = -plane.verticalExtent;
        const auto normal = Math::TryNormalize(Math::Cross(right, up));
        if (normal.HasError())
            return Invalid<Math::Mat4>(UiErrors::CanvasSpaceInvalid);
        const auto center = plane.origin + right * 0.5F - up * 0.5F;
        const auto forward = normal.Value();
        const Math::Mat4 transform{
            {right.x, right.y, right.z, 0, up.x, up.y, up.z, 0, forward.x, forward.y, forward.z, 0, center.x, center.y, center.z, 1}};
        return Math::TryInverseAffine(transform).HasValue() ? Result<Math::Mat4>::Success(transform)
                                                            : Invalid<Math::Mat4>(UiErrors::CanvasSpaceInvalid);
    }

    /** @brief Application source factory state; only the sole allocator is retained by admitted source owners. */
    struct RuntimeUiPaintProjectSources::Storage final {
        RuntimeUiPaintPort port;
        std::shared_ptr<Detail::RuntimeUiPaintHostState> host;
        std::shared_ptr<UiElementSlotAllocator> allocator;
        RuntimeUiPaintProjectPolicy policy;
        std::shared_ptr<const Render::CookedUiPaintKernel> sceneKernel;
        std::uint32_t instanceSlot{};
        std::uint32_t canvasSlot{};

        /** @brief Creates every authored canvas tree in source order using the sole allocator and bounded depth. */
        Result<std::vector<UiReloadCanvas>> BuildCanvases(const CookedUiDocument &document, RuntimeUiInstanceId instance);
        /** @brief Derives owned style, presentation and paint inputs before moving the same source into its canonical generation. */
        Result<RuntimeUiPaintSceneSourcePreparation> FinishSource(UiRuntimeAssetLoadResult loaded, const UiCanvasAssetReference &reference,
                                                                  Runtime::SceneRuntimeId scene, RuntimeUiInstanceId instance,
                                                                  std::vector<UiReloadCanvas> canvases, RuntimeStyleRegistry registry,
                                                                  ProjectCanvasProjection projection) const;
        /** @brief Lazily prepares the explicitly selected real kernel policy; failures leave the cache unpublished. */
        Result<void> PrepareKernel(bool sceneCanvas);
        /** @brief Resolves the selected source against its actual raster target. */
        Result<ProjectCanvasProjection> ResolveProjection(const UiRuntimeAssetLoadResult &loaded, const UiCanvasDescriptor &canvas,
                                                          const SceneCanvasSource &source, const Runtime::RuntimeSceneView &scene) const;

        Result<RuntimeUiPaintSceneSourcePreparation> Build(UiRuntimeAssetLoadResult loaded, const SceneCanvasSource &source,
                                                           const Runtime::RuntimeSceneView &scene);
        Result<std::unique_ptr<Runtime::SceneActivationCandidate>> Prepare(const Runtime::RuntimeSceneService &scenes,
                                                                           const Assets::AssetRegistry &registry,
                                                                           UiRuntimeAssetLoadService &loads,
                                                                           const std::optional<Assets::AssetCookTargetId> &target,
                                                                           const Runtime::RuntimeSceneView &scene,
                                                                           const std::shared_ptr<const RuntimeUiPaintWorldSceneView> &view);
    };

    /** @copydoc RuntimeUiPaintProjectSources::Storage::Build */
    Result<RuntimeUiPaintSceneSourcePreparation> RuntimeUiPaintProjectSources::Storage::Build(UiRuntimeAssetLoadResult loaded,
                                                                                              const SceneCanvasSource &source,
                                                                                              const Runtime::RuntimeSceneView &scene) {
        using Built = Result<RuntimeUiPaintSceneSourcePreparation>;
        const auto &reference = source.reference;
        if (!host->Available() || !host->output)
            return Invalid<RuntimeUiPaintSceneSourcePreparation>(UiErrors::CanvasSpaceInvalid);
        if (instanceSlot == std::numeric_limits<std::uint32_t>::max() || loaded.document.Elements().size() > policy.maximumElements ||
            loaded.document.Canvases().size() > std::numeric_limits<std::uint32_t>::max() - canvasSlot)
            return Invalid<RuntimeUiPaintSceneSourcePreparation>(UiErrors::CapacityExceeded);
        const RuntimeUiInstanceId instance{allocator->Ownership(), ++instanceSlot, 1};
        const auto selected = std::ranges::find(loaded.document.Canvases(), reference.canvas, &UiCanvasDescriptor::id);
        if (selected == loaded.document.Canvases().end())
            return Invalid<RuntimeUiPaintSceneSourcePreparation>(UiErrors::AssetMissing);
        auto resolved = ResolveProjection(loaded, *selected, source, scene);
        if (resolved.HasError())
            return Built::Failure(resolved.ErrorValue());
        if (const auto kernel = PrepareKernel(resolved.Value().sceneCanvas); kernel.HasError())
            return Built::Failure(kernel.ErrorValue());
        auto registry = Registry(loaded);
        if (registry.HasError())
            return Built::Failure(registry.ErrorValue());
        auto canvases = BuildCanvases(loaded.document, instance);
        if (canvases.HasError())
            return Built::Failure(canvases.ErrorValue());
        return FinishSource(std::move(loaded), reference, scene.RuntimeId(), instance, std::move(canvases).Value(),
                            std::move(registry).Value(), std::move(resolved).Value());
    }

    /** @copydoc RuntimeUiPaintProjectSources::Storage::ResolveProjection */
    Result<ProjectCanvasProjection> RuntimeUiPaintProjectSources::Storage::ResolveProjection(const UiRuntimeAssetLoadResult &loaded,
                                                                                             const UiCanvasDescriptor &canvas,
                                                                                             const SceneCanvasSource &source,
                                                                                             const Runtime::RuntimeSceneView &scene) const {
        using Projected = Result<ProjectCanvasProjection>;
        if (canvas.renderMode != UiRenderMode::WorldSpace) {
            const bool camera = canvas.renderMode == UiRenderMode::ScreenSpaceCamera && policy.vulkanSceneCanvasAssets.has_value();
            auto evidence = camera && host->cameraOutput ? *host->cameraOutput : *host->output;
            if (camera)
                evidence.safeAreaInsets = {};
            const auto resolved = ResolveUiScreenCanvasWithEvidence(canvas, evidence);
            return resolved.HasValue() ? Projected::Success({resolved.Value(), {}, camera}) : Projected::Failure(resolved.ErrorValue());
        }
        const auto limit = host->frontend->Capabilities().support.limits.maxTextureDimension2D;
        auto placement = ResolveRuntimeUiPaintWorldCanvas(scene, source.entity, loaded, limit);
        if (placement.HasError())
            return Projected::Failure(placement.ErrorValue());
        if (!policy.vulkanSceneCanvasAssets)
            return Invalid<ProjectCanvasProjection>(UiErrors::CapabilityUnsupported, "The selected viewport has no World canvas consumer.");
        const UiResolvedScreenCanvas raster{.logicalExtent = placement.Value().logicalExtent,
                                            .pixelExtent = placement.Value().rasterExtent,
                                            .pixelsPerDip = {1, 1},
                                            .fontScale = placement.Value().fontScale};
        return Projected::Success({raster, std::move(placement).Value(), true});
    }

    /** @copydoc RuntimeUiPaintProjectSources::Storage::BuildCanvases */
    Result<std::vector<UiReloadCanvas>> RuntimeUiPaintProjectSources::Storage::BuildCanvases(const CookedUiDocument &document,
                                                                                             const RuntimeUiInstanceId instance) {
        std::vector<UiReloadCanvas> canvases;
        canvases.reserve(document.Canvases().size());
        for (const auto &authored : document.Canvases()) {
            auto elements = TreeElements(document, authored, policy.maximumElements);
            if (elements.HasError())
                return Result<std::vector<UiReloadCanvas>>::Failure(elements.ErrorValue());
            const UiCanvasInstanceId identity{allocator->Ownership(), ++canvasSlot, 1};
            auto tree = UiElementTree::Create(*allocator,
                                              {instance,
                                               identity,
                                               document.Id(),
                                               document.SourceRevision(),
                                               Initial<UiRuntimeTreeRevision>(),
                                               {policy.maximumElements, std::min(policy.maximumElements, MaximumUiTreeDepth), 8}},
                                              elements.Value());
            if (tree.HasError())
                return Result<std::vector<UiReloadCanvas>>::Failure(tree.ErrorValue());
            canvases.emplace_back(authored.id, std::move(tree).Value());
        }
        return Result<std::vector<UiReloadCanvas>>::Success(std::move(canvases));
    }

    /** @copydoc RuntimeUiPaintProjectSources::Storage::FinishSource */
    Result<RuntimeUiPaintSceneSourcePreparation> RuntimeUiPaintProjectSources::Storage::FinishSource(
        UiRuntimeAssetLoadResult loaded, const UiCanvasAssetReference &reference, const Runtime::SceneRuntimeId scene,
        const RuntimeUiInstanceId instance, std::vector<UiReloadCanvas> canvases, RuntimeStyleRegistry registry,
        ProjectCanvasProjection projection) const {
        using Built = Result<RuntimeUiPaintSceneSourcePreparation>;
        const auto &resolved = projection.raster;
        const auto &kernel = projection.sceneCanvas ? sceneKernel : policy.kernel;
        auto selectedCanvas = std::ranges::find(canvases, reference.canvas, &UiReloadCanvas::id);
        auto &canvas = *selectedCanvas;
        auto preparation = PrepareCanvas(canvas, loaded.document, registry, resolved, allocator->Ownership());
        if (preparation.HasError())
            return Built::Failure(preparation.ErrorValue());
        auto prepared = std::move(preparation).Value();
        const auto &geometry = prepared.geometry.limits;
        const auto surfaces = static_cast<std::uint32_t>(prepared.definition.elements.size());
        const Render::UiPaintFrameBudget paint{2 * geometry.vertices, 2 * geometry.indices, 3 * surfaces + geometry.batches};
        const auto runtime = RuntimeConfig(resolved);
        RuntimeUiPaintDeclaration declaration{loaded.document.Id(), loaded.document.SourceRevision(), reference.canvas};
        const bool hasSceneProviders = std::ranges::any_of(canvases, [](const auto &entry) {
            return entry.bindings && entry.bindings->HasSceneProviders();
        });
        auto generation = UiReloadGeneration::Create(std::move(loaded), instance, std::move(canvases));
        if (generation.HasError())
            return Built::Failure(generation.ErrorValue());
        return Built::Success(
            {{{instance, {UiOwnerScopeKind::Scene, {}, scene, {}}, hasSceneProviders ? std::optional{scene} : std::nullopt},
              std::move(generation).Value()},
             {instance,
              *allocator,
              std::move(registry),
              std::move(prepared.styles),
              std::move(prepared.definition),
              {},
              runtime,
              std::move(declaration),
              {},
              prepared.extraction,
              prepared.geometry,
              {resolved, paint, 2, 256},
              *kernel,
              allocator,
              std::move(projection.world),
              projection.sceneCanvas}});
    }

    /** @copydoc RuntimeUiPaintProjectSources::Storage::PrepareKernel */
    Result<void> RuntimeUiPaintProjectSources::Storage::PrepareKernel(const bool sceneCanvas) {
        if (sceneCanvas) {
            if (!sceneKernel) {
                auto prepared = LoadPackagedProjectUiVulkanSceneKernel(*policy.vulkanSceneCanvasAssets, Render::ShaderTargetBackend::Vulkan,
                                                                       host->cancellation.Token());
                if (prepared.HasError())
                    return Result<void>::Failure(prepared.ErrorValue());
                sceneKernel = std::move(prepared).Value();
            }
            return Result<void>::Success();
        }
        if (!policy.kernel) {
            auto prepared = [&]() {
                if (const auto *compiler = std::get_if<RuntimeUiPaintProjectCompiler>(&policy.source))
                    return PrepareProjectUiKernel(*compiler, host->cancellation.Token());
                if (const auto *package = std::get_if<RuntimeUiPaintPackagedAssets>(&policy.source))
                    return LoadPackagedProjectUiMetalKernel(package->assetRoot, package->backend, host->cancellation.Token());
                return Result<std::shared_ptr<const Render::CookedUiPaintKernel>>::Failure(
                    MakeError(Render::ShaderCompilerPipelineErrors::ToolMissing));
            }();
            if (prepared.HasError())
                return Result<void>::Failure(prepared.ErrorValue());
            policy.kernel = std::move(prepared).Value();
        }
        return Result<void>::Success();
    }

    /** @copydoc RuntimeUiPaintProjectSources::Storage::Prepare */
    Result<std::unique_ptr<Runtime::SceneActivationCandidate>> RuntimeUiPaintProjectSources::Storage::Prepare(
        const Runtime::RuntimeSceneService &scenes, const Assets::AssetRegistry &registry, UiRuntimeAssetLoadService &loads,
        const std::optional<Assets::AssetCookTargetId> &target, const Runtime::RuntimeSceneView &scene,
        const std::shared_ptr<const RuntimeUiPaintWorldSceneView> &view) {
        using Prepared = Result<std::unique_ptr<Runtime::SceneActivationCandidate>>;
        std::vector<SceneCanvasSource> references;
        for (std::size_t slot = 0; slot < scene.SlotCount(); ++slot) {
            const auto entity = scene.EntityAt(slot);
            if (!entity || !entity->components || !entity->components->uiCanvas)
                continue;
            if (references.size() == Detail::MaximumSceneUiSources)
                return Prepared::Failure(MakeError(UiErrors::CapacityExceeded));
            references.push_back({entity->entity, entity->components->uiCanvas->canvas});
        }
        const auto active = scenes.ActiveScene();
        const UiSceneTransitionRequest transition{active ? std::optional{active->RuntimeId()} : std::nullopt, scene.RuntimeId()};
        if (references.empty())
            return port.PrepareScene(transition, {});
        if (!host->Available())
            return Prepared::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        if (!host->frontend->Capabilities().support.features.Supports(Render::RenderCapability::UiPaint))
            return Prepared::Failure(MakeError(UiErrors::CapabilityUnsupported));
        const auto snapshot = registry.Snapshot();
        std::vector<RuntimeUiPaintSceneSourcePreparation> sources;
        sources.reserve(references.size());
        for (const auto &source : references) {
            auto request = loads.LoadAsync(snapshot, {source.reference, {}, target}, host->cancellation.Token());
            if (request.HasError())
                return Prepared::Failure(request.ErrorValue());
            auto handle = std::move(request).Value();
            if (const auto waited = handle.Wait(); waited.HasError())
                return Prepared::Failure(waited.ErrorValue());
            auto loaded = handle.TakeResult();
            if (loaded.HasError())
                return Prepared::Failure(loaded.ErrorValue());
            auto built = Build(std::move(loaded).Value(), source, scene);
            if (built.HasError())
                return Prepared::Failure(built.ErrorValue());
            auto preparation = std::move(built).Value();
            if (auto &world = preparation.paint.worldCanvas; world)
                world->currentScene = view;
            sources.push_back(std::move(preparation));
        }
        return port.PrepareScene(transition, std::move(sources), {}, host->cancellation.Token());
    }

    /** @copydoc RuntimeUiPaintProjectSources::~RuntimeUiPaintProjectSources */
    RuntimeUiPaintProjectSources::~RuntimeUiPaintProjectSources() = default;

    /** @copydoc RuntimeUiPaintProjectSources::Create */
    Result<std::shared_ptr<RuntimeUiPaintProjectSources>> RuntimeUiPaintProjectSources::Create(
        RuntimeUiPaintPort port, std::shared_ptr<UiElementSlotAllocator> allocator, RuntimeUiPaintProjectPolicy policy) {
        using Created = Result<std::shared_ptr<RuntimeUiPaintProjectSources>>;
        if (!port.state_ || !port.state_->Available() || !allocator || !allocator->Ownership().IsValid() || policy.maximumElements == 0 ||
            policy.maximumElements > 512)
            return Created::Failure(MakeError(UiErrors::HandleOwnerMismatch));
        auto storage = std::make_shared<Storage>();
        storage->host = port.state_;
        storage->port = std::move(port);
        storage->allocator = std::move(allocator);
        storage->instanceSlot = policy.previousInstanceSlot;
        storage->canvasSlot = policy.previousCanvasSlot;
        storage->policy = std::move(policy);
        return Created::Success(std::make_shared<RuntimeUiPaintProjectSources>(ConstructionKey{}, std::move(storage)));
    }

    /** @copydoc RuntimeUiPaintProjectSources::MakeParticipant */
    std::unique_ptr<Runtime::SceneActivationParticipant> RuntimeUiPaintProjectSources::MakeParticipant(
        Runtime::RuntimeSceneService &scenes, Assets::AssetRegistry &registry, UiRuntimeAssetLoadService &loads,
        std::optional<Assets::AssetCookTargetId> target) {
        class Participant final : public Runtime::SceneActivationParticipant {
        public:
            Participant(std::shared_ptr<Storage> source, Runtime::RuntimeSceneService &sceneOwner, Assets::AssetRegistry &assets,
                        UiRuntimeAssetLoadService &loader, const std::optional<Assets::AssetCookTargetId> &cookTarget)
                : source_(std::move(source)), scenes_(sceneOwner), registry_(assets), loads_(loader), target_(cookTarget),
                  currentScene_(std::make_shared<const RuntimeUiPaintWorldSceneView>([&sceneOwner] {
                      return sceneOwner.ActiveScene();
                  })) {}

            Result<std::unique_ptr<Runtime::SceneActivationCandidate>> Prepare(const Runtime::RuntimeSceneDefinition &,
                                                                               const Runtime::RuntimeSceneView &scene) override {
                return source_->Prepare(scenes_, registry_, loads_, target_, scene, currentScene_);
            }

            Result<std::unique_ptr<Runtime::SceneActivationCandidate>> PrepareRetirement(const Runtime::RuntimeSceneView &scene) override {
                return source_->port.PrepareScene({scene.RuntimeId(), {}}, {});
            }

        private:
            std::shared_ptr<Storage> source_;
            Runtime::RuntimeSceneService &scenes_;
            Assets::AssetRegistry &registry_;
            UiRuntimeAssetLoadService &loads_;
            std::optional<Assets::AssetCookTargetId> target_;
            std::shared_ptr<const RuntimeUiPaintWorldSceneView> currentScene_;
        };

        return std::make_unique<Participant>(storage_, scenes, registry, loads, target);
    }
}  // namespace Horo::Application::Internal
