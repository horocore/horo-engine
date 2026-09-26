#include "Horo/AI/AIScenePerceptionSource.h"

namespace Horo::AI {
    namespace {
        /** @brief Checks the current structural revision rather than trusting a retained entity slot. */
        [[nodiscard]] bool SceneSourceAlive(void *context, const PerceptionSourceRef &source) {
            auto &scene = *static_cast<Runtime::RuntimeScene *>(context);
            const auto view = scene.View();
            if (!source.IsValid() || view.RuntimeId().value != source.sceneIncarnation)
                return false;
            const Runtime::EntityRef entity{.runtime = view.RuntimeId(),
                                            .entity = Runtime::EntityId{.index = source.slot, .generation = source.generation}};
            return view.Get(entity).HasValue();
        }
    }  // namespace

    /** @copydoc ProjectPerceptionSource */
    PerceptionSourceRef ProjectPerceptionSource(const Runtime::EntityRef entity) noexcept {
        return {.sceneIncarnation = entity.runtime.value, .slot = entity.entity.index, .generation = entity.entity.generation};
    }

    /** @copydoc PerceptionSceneLiveness */
    PerceptionSourceLiveness PerceptionSceneLiveness(Runtime::RuntimeScene &scene) noexcept {
        return {.context = &scene, .isAlive = SceneSourceAlive};
    }
}  // namespace Horo::AI
