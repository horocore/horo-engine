#include "EditorRenderExtractionErrors.h"
#include "Horo/Runtime/Render/LightSceneExtraction.h"
#include "editor/document/EditorViewportSceneExtractor.h"

#include <algorithm>
#include <utility>

namespace Horo::Editor {
    namespace {
        Result<void> InvalidPreview() {
            return Result<void>::Failure(MakeError(ViewportSceneErrors::InvalidResult));
        }

        std::uint64_t LightIdentity(const Runtime::EntityRef entity) noexcept {
            return (std::uint64_t{entity.entity.generation} << 32U) | entity.entity.index;
        }

        struct WorldPose {
            Math::Mat4 matrix{Math::Mat4::Identity()};
            Math::Quaternion rotation;
        };

        Result<void> AccumulatePose(const Math::Transform &local, WorldPose &pose) {
            const auto matrix = local.TryToMatrix();
            if (matrix.HasError())
                return Result<void>::Failure(matrix.ErrorValue());
            const auto rotation = local.rotation.TryNormalized();
            if (rotation.HasError())
                return Result<void>::Failure(rotation.ErrorValue());
            pose.matrix = Math::Multiply(matrix.Value(), pose.matrix);
            pose.rotation = rotation.Value() * pose.rotation;
            return Result<void>::Success();
        }

        Result<WorldPose> ResolvePose(const Runtime::RuntimeSceneView &scene, Runtime::EntityRef entity,
                                      const std::span<const EditorViewportTransformOverride> overrides) {
            WorldPose pose;
            for (std::size_t depth = 0; depth < 64; ++depth) {
                const auto view = scene.Get(entity);
                if (view.HasError())
                    return Result<WorldPose>::Failure(view.ErrorValue());
                const Math::Transform *local = view.Value().localTransform;
                if (entity.entity.index < overrides.size()) {
                    const auto &override = overrides[entity.entity.index];
                    if (override.present && override.entity == entity)
                        local = &override.localTransform;
                }
                const auto accumulated = AccumulatePose(*local, pose);
                if (accumulated.HasError())
                    return Result<WorldPose>::Failure(accumulated.ErrorValue());
                if (!view.Value().parent)
                    return Result<WorldPose>::Success(pose);
                entity = *view.Value().parent;
            }
            return Result<WorldPose>::Failure(MakeError(ViewportSceneErrors::HierarchyCycle));
        }

        bool ValidSnapshot(const Runtime::RuntimeSceneView &scene, const EditorViewportSceneSnapshot &snapshot) noexcept {
            return scene.IsCurrent() && snapshot.runtimeSceneId == scene.RuntimeId() &&
                   snapshot.instances.size() == snapshot.instanceEntities.size() &&
                   snapshot.instances.size() == snapshot.instanceObjects.size() &&
                   snapshot.stagedInstanceWorlds.size() == snapshot.instances.size() &&
                   snapshot.transformOverrides.size() == scene.SlotCount() &&
                   snapshot.stagedTransformOverrides.size() == scene.SlotCount() && snapshot.lights.size() == snapshot.lightObjects.size();
        }

        Result<void> StageTransforms(const Runtime::RuntimeSceneView &scene, const std::span<const SceneObjectTransformPreview> previews,
                                     EditorViewportSceneSnapshot &snapshot) {
            if (previews.size() > scene.SlotCount())
                return InvalidPreview();
            std::ranges::fill(snapshot.stagedTransformOverrides, EditorViewportTransformOverride{});
            for (const auto &preview : previews) {
                const auto found =
                    std::ranges::lower_bound(snapshot.authoredEntities, preview.object, {}, &EditorViewportEntitySource::object);
                if (found == snapshot.authoredEntities.end() || found->object != preview.object)
                    return Result<void>::Failure(MakeError(ViewportSceneErrors::ObjectNotFound));
                const auto entity = found->entity;
                const auto current = scene.Get(entity);
                if (current.HasError())
                    return Result<void>::Failure(current.ErrorValue());
                if (entity.entity.index >= snapshot.stagedTransformOverrides.size())
                    return InvalidPreview();
                auto &slot = snapshot.stagedTransformOverrides[entity.entity.index];
                if (slot.present)
                    return Result<void>::Failure(MakeError(ViewportSceneErrors::InvalidObjectId));
                const auto validated = preview.localTransform.TryToMatrix();
                if (validated.HasError())
                    return Result<void>::Failure(validated.ErrorValue());
                slot = {entity, preview.localTransform, true};
            }
            return Result<void>::Success();
        }

        Result<void> StageInstances(const Runtime::RuntimeSceneView &scene, EditorViewportSceneSnapshot &snapshot) {
            for (std::size_t index = 0; index < snapshot.instanceEntities.size(); ++index) {
                const auto pose = ResolvePose(scene, snapshot.instanceEntities[index], snapshot.stagedTransformOverrides);
                if (pose.HasError())
                    return Result<void>::Failure(pose.ErrorValue());
                if (!Math::IsFinite(pose.Value().matrix))
                    return InvalidPreview();
                snapshot.stagedInstanceWorlds[index] = pose.Value().matrix;
            }
            return Result<void>::Success();
        }

        Result<Render::RenderLight> ResolveLight(const Runtime::RuntimeSceneView &scene, const EditorViewportLightSource &source,
                                                 const Runtime::LightComponent &component,
                                                 const std::span<const EditorViewportTransformOverride> overrides) {
            const auto pose = ResolvePose(scene, source.entity, overrides);
            if (pose.HasError())
                return Result<Render::RenderLight>::Failure(pose.ErrorValue());
            const auto position = Math::TryTransformPoint(pose.Value().matrix, {});
            if (position.HasError())
                return Result<Render::RenderLight>::Failure(position.ErrorValue());
            const auto direction = pose.Value().rotation.TryRotate({0, 0, -1});
            if (direction.HasError())
                return Result<Render::RenderLight>::Failure(direction.ErrorValue());
            auto light = Render::BuildRenderLightValue(component, position.Value(), direction.Value());
            if (light.HasError())
                return light;
            auto value = std::move(light).Value();
            value.stableIdentity = LightIdentity(source.entity);
            return Result<Render::RenderLight>::Success(std::move(value));
        }

        Result<Runtime::LightComponent> SourceComponent(const Runtime::RuntimeSceneView &scene, const Runtime::RuntimeEntityView &view,
                                                        const EditorViewportLightSource &source, const DocumentRevision documentRevision) {
            if (view.components->light)
                return Result<Runtime::LightComponent>::Success(*view.components->light);
            if (!source.disabledAuthoredLight || source.disabledAuthoredLight->enabled || source.authoredRevision != documentRevision ||
                source.definitionRevision != scene.DefinitionRevision())
                return Result<Runtime::LightComponent>::Failure(MakeError(ViewportSceneErrors::InvalidResult));
            return Result<Runtime::LightComponent>::Success(*source.disabledAuthoredLight);
        }

        Result<void> StageLight(const Runtime::RuntimeSceneView &scene, const EditorViewportLightSource &source,
                                const SceneObjectLightPreview *preview, const std::span<const EditorViewportTransformOverride> overrides,
                                EditorViewportLightStaging &bank, std::size_t &count, const DocumentRevision documentRevision) {
            const auto view = scene.Get(source.entity);
            if (view.HasError())
                return Result<void>::Failure(view.ErrorValue());
            if (!view.Value().authoredObject || view.Value().authoredObject->value != source.object.value)
                return InvalidPreview();
            const auto authored = SourceComponent(scene, view.Value(), source, documentRevision);
            if (authored.HasError())
                return Result<void>::Failure(authored.ErrorValue());
            const auto &component = preview != nullptr && preview->object == source.object ? preview->light : authored.Value();
            if (!component.enabled)
                return Result<void>::Success();
            if (count == Render::MaximumSceneLights)
                return Result<void>::Failure(MakeError(Render::LightCullingErrors::Capacity));
            const auto light = ResolveLight(scene, source, component, overrides);
            if (light.HasError())
                return Result<void>::Failure(light.ErrorValue());
            bank.lights[count] = light.Value();
            bank.objects[count++] = source.object;
            return Result<void>::Success();
        }

        // At least one bank retains its full cold size even after a snapshot copy. Publication
        // swaps only after validation; the other bank makes the next update allocation-free.
        EditorViewportLightStaging *AvailableBank(EditorViewportSceneSnapshot &snapshot) {
            for (auto &bank : snapshot.stagedLights) {
                if (bank.lights.capacity() >= Render::MaximumSceneLights && bank.objects.capacity() >= Render::MaximumSceneLights) {
                    bank.lights.resize(Render::MaximumSceneLights);
                    bank.objects.resize(Render::MaximumSceneLights);
                    return &bank;
                }
            }
            return nullptr;
        }

        Result<std::size_t> StageLights(const Runtime::RuntimeSceneView &scene, const SceneObjectLightPreview *preview,
                                        const std::span<const EditorViewportTransformOverride> overrides,
                                        const EditorViewportSceneSnapshot &snapshot, EditorViewportLightStaging &bank) {
            std::size_t count = 0;
            for (const auto &source : snapshot.eligibleLights) {
                const auto result = StageLight(scene, source, preview, overrides, bank, count, snapshot.documentRevision);
                if (result.HasError())
                    return Result<std::size_t>::Failure(result.ErrorValue());
            }
            return Result<std::size_t>::Success(count);
        }

        void PublishLights(EditorViewportSceneSnapshot &snapshot, EditorViewportLightStaging &bank, const std::size_t count) {
            bank.lights.resize(count);
            bank.objects.resize(count);
            snapshot.lights.swap(bank.lights);
            snapshot.lightObjects.swap(bank.objects);
            if (bank.lights.capacity() >= Render::MaximumSceneLights && bank.objects.capacity() >= Render::MaximumSceneLights) {
                bank.lights.resize(Render::MaximumSceneLights);
                bank.objects.resize(Render::MaximumSceneLights);
            }
        }

        Result<void> ValidateLightPreview(const SceneObjectLightPreview *preview, const EditorViewportSceneSnapshot &snapshot) {
            if (preview == nullptr)
                return Result<void>::Success();
            if (!preview->object.IsValid() || !IsValidLightComponent(preview->light))
                return InvalidPreview();
            if (std::ranges::find(snapshot.eligibleLights, preview->object, &EditorViewportLightSource::object) ==
                snapshot.eligibleLights.end())
                return Result<void>::Failure(MakeError(ViewportSceneErrors::ObjectNotFound));
            return Result<void>::Success();
        }
    }  // namespace

    Result<void> ApplyEditorViewportTransformPreview(const Runtime::RuntimeSceneView &scene,
                                                     const std::span<const SceneObjectTransformPreview> previews,
                                                     EditorViewportSceneSnapshot &snapshot) {
        if (!ValidSnapshot(scene, snapshot))
            return InvalidPreview();
        const auto transforms = StageTransforms(scene, previews, snapshot);
        if (transforms.HasError())
            return transforms;
        const auto instances = StageInstances(scene, snapshot);
        if (instances.HasError())
            return instances;
        auto *bank = AvailableBank(snapshot);
        if (bank == nullptr)
            return InvalidPreview();
        const auto lights = StageLights(scene, snapshot.lightOverride ? &*snapshot.lightOverride : nullptr,
                                        snapshot.stagedTransformOverrides, snapshot, *bank);
        if (lights.HasError())
            return Result<void>::Failure(lights.ErrorValue());
        for (std::size_t index = 0; index < snapshot.instances.size(); ++index)
            snapshot.instances[index].localToWorld = snapshot.stagedInstanceWorlds[index];
        snapshot.transformOverrides.swap(snapshot.stagedTransformOverrides);
        PublishLights(snapshot, *bank, lights.Value());
        return Result<void>::Success();
    }

    Result<void> ApplyEditorViewportLightPreview(const Runtime::RuntimeSceneView &scene, const SceneObjectLightPreview *preview,
                                                 EditorViewportSceneSnapshot &snapshot) {
        if (!ValidSnapshot(scene, snapshot))
            return InvalidPreview();
        const auto validation = ValidateLightPreview(preview, snapshot);
        if (validation.HasError())
            return validation;
        auto *bank = AvailableBank(snapshot);
        if (bank == nullptr)
            return InvalidPreview();
        const auto lights = StageLights(scene, preview, snapshot.transformOverrides, snapshot, *bank);
        if (lights.HasError())
            return Result<void>::Failure(lights.ErrorValue());
        PublishLights(snapshot, *bank, lights.Value());
        snapshot.lightOverride = preview == nullptr ? std::nullopt : std::optional{*preview};
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
