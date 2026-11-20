#include "Horo/Gameplay/NativeBehavior.h"
#if defined(HORO_TEST_GAMEPLAY_PHYSICS) && HORO_TEST_GAMEPLAY_PHYSICS
#include "Horo/Gameplay/GameplayCharacterContext.h"
#endif

#include <algorithm>
#include <stdexcept>

using namespace Horo;
using namespace Horo::Gameplay;

namespace {
    /** @brief Distinguishes Character fixture failures from gameplay exceptions in the native test module. */
    class CharacterFixtureFailure final : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };
}  // namespace

/** @brief Native behavior consumer of the same Character context and owned command used by scripts. */
class CharacterClientBehavior final : public IBehaviorInstance {
public:
    static BehaviorDescriptor DescribeBehavior() {
        BehaviorDescriptor descriptor;
        descriptor.displayName = "Character Client";
        descriptor.fields = {{"slot", std::int64_t{0}}, {"generation", std::int64_t{1}}, {"continuous", false}};
        descriptor.phases.push_back({BehaviorPhase::Gameplay, "game.tests.character_client", {}, {}, {}});
        return descriptor;
    }

    void OnFixedUpdate(BehaviorContext &context, FixedDeltaTime) override {
#if defined(HORO_TEST_GAMEPLAY_PHYSICS) && HORO_TEST_GAMEPLAY_PHYSICS
        const auto fields = context.Fields();
        const auto continuousField = std::ranges::find(fields, "continuous", &BehaviorField::name);
        const bool continuous = continuousField != fields.end() && std::get<bool>(continuousField->value);
        if (submittedTick_ != 0 && !continuous)
            return;
        const auto holder = context.CharacterContext();
        if (!holder)
            throw CharacterFixtureFailure("Native Character context is missing.");
        const auto &binding = holder->Binding();
        auto granted = holder->Acquire("game.tests", binding.scene, binding.sceneGeneration);
        if (granted.HasError())
            throw CharacterFixtureFailure(granted.ErrorValue().code.Value());
        const auto handle = ResolveController(granted.Value(), fields);
        const auto tick = submittedTick_ + 1;
        const float scheduledSpeed = ((tick - 1) / 8) % 2 == 0 ? 0.25F : 0.75F;
        const float speed = continuous ? scheduledSpeed : 1.0F;
        auto queued = granted.Value().QueueMovementCommand(
            {.controller = handle, .tick = tick, .sequence = tick + 967, .desiredVelocityMetersPerSecond = Math::Vec3{speed, 0, 0}});
        if (queued.HasError())
            throw CharacterFixtureFailure(queued.ErrorValue().code.Value());
        submittedTick_ = tick;
        if (!continuous) {
            auto transform = context.LocalTransform().Value();
            transform.translation.x = 21;
            if (context.SetLocalTransform(transform).HasError())
                throw CharacterFixtureFailure("Native Character marker rejected.");
        }
#else
        static_cast<void>(context);
        throw CharacterFixtureFailure("Character fixture requires the explicit Physics SDK component.");
#endif
    }

private:
#if defined(HORO_TEST_GAMEPLAY_PHYSICS) && HORO_TEST_GAMEPLAY_PHYSICS
    /** @brief Uses the fixture's admitted controller identity and its authored slot selection. */
    static Character::CharacterControllerHandle ResolveController(const Character::CharacterCapability &capability,
                                                                  std::span<const BehaviorField> fields) {
        const auto identity = capability.Identity();
        Character::CharacterControllerHandle handle{identity.sceneGeneration, identity.world, {0, 1}};
        for (const auto &field : fields) {
            if (field.name == "slot")
                handle.slot.index = static_cast<std::uint32_t>(std::get<std::int64_t>(field.value));
            if (field.name == "generation")
                handle.slot.generation = static_cast<std::uint32_t>(std::get<std::int64_t>(field.value));
        }
        return handle;
    }

    std::uint64_t submittedTick_{};
#endif
};
HORO_BEHAVIOR(CharacterClientBehavior, "game.tests.character_client")
