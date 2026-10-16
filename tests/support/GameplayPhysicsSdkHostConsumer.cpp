#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Gameplay/GameModuleRetirement.h"
#include "Horo/Gameplay/GameplayCharacterContext.h"
#include "Horo/Gameplay/GameplayPhysicsContext.h"
#include "Horo/Physics/CharacterWorld.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "Horo/Platform/DynamicLibrary.h"

#include <charconv>
#include <cstring>
#include <optional>
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

    /** @brief Unloads the actual external image before observing and releasing its escaped terminal batch. */
    int VerifyBatchImageRetirement(std::unique_ptr<Horo::Gameplay::LoadedGameModule> module, Horo::Physics::PhysicsWorld *world) {
        auto loadedImage = Horo::Platform::LoadDynamicLibrary(module->LoadedArtifactPath().string());
        if (loadedImage.HasError())
            return 11;
        auto image = std::move(loadedImage).Value();
        void *symbol = image->GetSymbol("HoroTestCopyPhysicsBatch");
        if (!symbol)
            return 12;
        using CopyBatch = void (*)(std::optional<Horo::Physics::PhysicsQueryBatchHandle> *);
        CopyBatch copy{};
        static_assert(sizeof(copy) == sizeof(symbol));
        std::memcpy(&copy, &symbol, sizeof(copy));
        std::optional<Horo::Physics::PhysicsQueryBatchHandle> escaped;
        copy(&escaped);
        module.reset();
        copy = nullptr;
        image.reset();
        if (!world)
            return 0;
        if (!escaped)
            return 15;
        const auto terminal = escaped->Poll();
        if (!terminal.HasError() || terminal.ErrorValue().code.Value() != Horo::Physics::PhysicsErrors::QueryCancelled.code.Value())
            return 13;
        if (world->ProcessQueryBatch().HasError())
            return 14;
        escaped.reset();  // Releases the last strong batch after both module unload and host queue drain.
        return 0;         // The caller subsequently destroys capability registry weak entries with the host world.
    }

    /** @brief Loads the SDK fixture and verifies image retirement while the caller retains its world and host owners. */
    int LoadAndVerifyRetirement(Horo::Gameplay::GameModuleHost &host, const char *artifact, std::uint64_t revision,
                                Horo::Physics::PhysicsWorld *world) {
        auto loaded = host.Load(artifact, {"game.physics_sdk", Horo::Gameplay::CurrentGameplayBuildFingerprint(), revision});
        if (loaded.HasError())
            return 7;
        auto candidate = std::move(loaded).Value();
        if (candidate.diagnostic)
            return 7;
        return VerifyBatchImageRetirement(std::move(candidate.owner), world);
    }

#if HORO_TEST_PHYSICS_NATIVE
    /** @brief Captures the fixed bounded Physics setup used by actual external SDK dependency admission. */
    Horo::Physics::PhysicsWorldSettingsDescriptor SdkWorldSettings() {
        using namespace Horo::Physics;
        PhysicsWorldSettingsDescriptor descriptor;
        descriptor.world.capacity = {16, 32, 16, 4096};
        descriptor.budgets.maximumContactPairs = 32;
        descriptor.budgets.maximumContactConstraints = 16;
        descriptor.budgets.maximumInFlightPairs = 8;
        descriptor.budgets.scratchBytes = 1024 * 1024;
        return descriptor;
    }

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
    auto character = GameplayCharacterContext::Withheld({.moduleId = "game.physics_sdk", .scene = 7, .sceneGeneration = 1},
                                                        GameplayCharacterDenial::Unavailable);
#if HORO_TEST_PHYSICS_NATIVE
    auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical);
    if (runtime.HasError())
        return 3;
    auto settings = PhysicsWorldSettings::Capture(SdkWorldSettings());
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
    Character::CharacterWorldSettingsDescriptor characterSettings;
    characterSettings.capacities.maximumControllers = 1;
    auto captured = Character::CharacterWorldSettings::Capture(characterSettings);
    if (captured.HasError())
        return 8;
    auto characterWorld = Character::CharacterWorld::Prepare({1, binding.world, 1, 1}, captured.Value());
    if (characterWorld.HasError() || characterWorld.Value()->Activate().HasError())
        return 9;
    auto characterContext = GameplayCharacterContext::Create({"game.physics_sdk", 7, 1, characterWorld.Value()->Descriptor().identity,
                                                              binding.world, true, true},
                                                             characterWorld.Value().get(), context.get());
    if (characterContext.HasError())
        return 10;
    character = std::move(characterContext).Value();
#endif
    GameModuleHost host{GameModuleHostOptions{{}, {.physics = context, .character = character}}};
#if HORO_TEST_PHYSICS_NATIVE
    return LoadAndVerifyRetirement(host, argv[1], revision, world.Value().get());
#else
    return LoadAndVerifyRetirement(host, argv[1], revision, nullptr);
#endif
}
