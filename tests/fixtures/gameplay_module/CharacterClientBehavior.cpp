#include "Horo/Gameplay/NativeBehavior.h"
#if defined(HORO_TEST_GAMEPLAY_PHYSICS) && HORO_TEST_GAMEPLAY_PHYSICS
#include "Horo/Gameplay/GameplayCharacterContext.h"
#endif

#include <stdexcept>

using namespace Horo;
using namespace Horo::Gameplay;

/** @brief Native behavior consumer of the same Character context and owned command used by scripts. */
class CharacterClientBehavior final : public IBehaviorInstance {
public:
    static BehaviorDescriptor DescribeBehavior() {
        BehaviorDescriptor descriptor;
        descriptor.displayName = "Character Client";
        descriptor.fields = {{"slot", std::int64_t{0}}, {"generation", std::int64_t{1}}};
        descriptor.phases.push_back({BehaviorPhase::Gameplay, "game.tests.character_client", {}, {}, {}});
        return descriptor;
    }

    void OnFixedUpdate(BehaviorContext &context, FixedDeltaTime) override {
#if defined(HORO_TEST_GAMEPLAY_PHYSICS) && HORO_TEST_GAMEPLAY_PHYSICS
        if (submitted_)
            return;
        const auto holder = context.CharacterContext();
        if (!holder)
            throw std::runtime_error("Native Character context is missing.");
        const auto &binding = holder->Binding();
        auto granted = holder->Acquire("game.tests", binding.scene, binding.sceneGeneration);
        if (granted.HasError())
            throw std::runtime_error(granted.ErrorValue().code.Value());
        const auto identity = granted.Value().Identity();
        Character::CharacterControllerHandle handle{identity.sceneGeneration, identity.world, {0, 1}};
        for (const auto &field : context.Fields()) {
            if (field.name == "slot")
                handle.slot.index = static_cast<std::uint32_t>(std::get<std::int64_t>(field.value));
            if (field.name == "generation")
                handle.slot.generation = static_cast<std::uint32_t>(std::get<std::int64_t>(field.value));
        }
        auto queued = granted.Value().QueueMovementCommand(
            {.controller = handle, .tick = 1, .sequence = 968, .desiredVelocityMetersPerSecond = Math::Vec3{1, 0, 0}});
        if (queued.HasError())
            throw std::runtime_error(queued.ErrorValue().code.Value());
        auto transform = context.LocalTransform().Value();
        transform.translation.x = 21;
        if (context.SetLocalTransform(transform).HasError())
            throw std::runtime_error("Native Character marker rejected.");
        submitted_ = true;
#else
        static_cast<void>(context);
        throw std::runtime_error("Character fixture requires the explicit Physics SDK component.");
#endif
    }

private:
#if defined(HORO_TEST_GAMEPLAY_PHYSICS) && HORO_TEST_GAMEPLAY_PHYSICS
    bool submitted_{};
#endif
};
HORO_BEHAVIOR(CharacterClientBehavior, "game.tests.character_client")
