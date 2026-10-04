#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Gameplay/GameplayPhysicsContext.h"
#include "Horo/Physics/PhysicsWorld.h"

#include <charconv>
#include <string_view>

namespace {
    /** @brief Validate the generated descriptor revision passed by the external SDK CTest. */
    std::uint64_t DescriptorRevision(const std::string_view text) {
        std::uint64_t revision{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), revision);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
            return 0;
        return revision;
    }

#if HORO_TEST_PHYSICS_NATIVE
    /** @brief Publish the explicit SDK fixture world before module startup exercises event/query/batch operations. */
    Horo::Result<void> ActivatePublication(Horo::Physics::PhysicsWorld &world, Horo::Physics::PhysicsRuntime &runtime,
                                           const Horo::Gameplay::GameplayPhysicsBinding &binding) {
        const auto identity = runtime.IssueWorldIdentity();
        if (identity.HasError())
            return Horo::Result<void>::Failure(identity.ErrorValue());
        if (const auto activated = world.Activate(identity.Value()); activated.HasError())
            return activated;
        return world.AdvanceFixedTick(
            {.simulationTick = 1, .sceneGeneration = binding.sceneGeneration, .fixedDelta = Horo::Duration::FromNanoseconds(16'666'667)});
    }
#endif
}  // namespace

int main(int argc, char **argv) {
    if (argc != 3)
        return 1;
    const auto revision = DescriptorRevision(argv[2]);
    if (revision == 0)
        return 2;
    using namespace Horo;
    using namespace Horo::Gameplay;
    using namespace Horo::Physics;
    GameplayPhysicsBinding binding{"game.physics_sdk", 7, 1, {}, true, true};
    auto context = GameplayPhysicsContext::Withheld(binding, GameplayPhysicsDenial::Unavailable);
#if HORO_TEST_PHYSICS_NATIVE
    auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical);
    if (runtime.HasError())
        return 3;
    PhysicsWorldSettingsDescriptor descriptor;
    descriptor.world.capacity = {16, 32, 16, 4096};
    descriptor.budgets.maximumContactPairs = 32;
    descriptor.budgets.maximumContactConstraints = 16;
    descriptor.budgets.maximumInFlightPairs = 8;
    descriptor.budgets.scratchBytes = 1024 * 1024;
    auto settings = PhysicsWorldSettings::Capture(descriptor);
    if (settings.HasError())
        return 4;
    auto world = runtime.Value()->PrepareWorld(settings.Value());
    if (world.HasError())
        return 5;
    if (ActivatePublication(*world.Value(), *runtime.Value(), binding).HasError())
        return 5;
    binding.world = world.Value()->Identity();
    auto admitted = GameplayPhysicsContext::Create(binding, world.Value().get());
    if (admitted.HasError())
        return 6;
    context = std::move(admitted).Value();
#endif
    GameModuleHost host{{}, context};
    const auto loaded = host.Load(argv[1], {"game.physics_sdk", CurrentGameplayBuildFingerprint(), revision});
    return loaded.HasValue() ? 0 : 7;
}
