#include "Horo/Gameplay/GameModuleHost.h"

#include "GameModuleHostDetail.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "Horo/Gameplay/GameplayPhysicsContext.h"

#include <cstring>
#include <utility>

namespace Horo::Gameplay {
    namespace {
        template <typename Function>
        [[nodiscard]] Function Resolve(const Platform::DynamicLibrary &library, const std::string_view name) noexcept {
            void *symbol = library.GetSymbol(name);
            Function func{};
            static_assert(sizeof(func) == sizeof(symbol));
            std::memcpy(&func, &symbol, sizeof(func));
            return func;
        }

    }  // namespace

    namespace {
        struct ValidatedModuleExports {
            const GameModuleDescriptor *descriptor{};
            const GeneratedGameplayDescriptorBundle *bundle{};
        };

        [[nodiscard]] Result<ValidatedModuleExports> ReadValidatedExports(const Platform::DynamicLibrary &library,
                                                                          const GameModuleLoadExpectation &expectation) {
            const auto getDescriptor = Resolve<GetGameModuleDescriptorFunction>(library, GetGameModuleDescriptorSymbol);
            const auto getBundle = Resolve<GetGameplayDescriptorBundleFunction>(library, GetGameplayDescriptorBundleSymbol);
            if (getDescriptor == nullptr || getBundle == nullptr)
                return Result<ValidatedModuleExports>::Failure(
                    MakeError(GameplayErrors::InvalidGameModuleDescriptor, "Gameplay module is missing required descriptor exports."));
            const GameModuleDescriptor *descriptor = getDescriptor();
            const GeneratedGameplayDescriptorBundle *bundle = getBundle();
            if (descriptor == nullptr)
                return Result<ValidatedModuleExports>::Failure(
                    MakeError(GameplayErrors::InvalidGameModuleDescriptor, "Gameplay module descriptor export returned null."));
            if (bundle == nullptr)
                return Result<ValidatedModuleExports>::Failure(
                    MakeError(GameplayErrors::InvalidGeneratedDescriptorBundle, "Generated gameplay descriptor export returned null."));
            if (const Result<void> valid = ValidateGameModuleDescriptor(*descriptor, expectation); valid.HasError())
                return Result<ValidatedModuleExports>::Failure(valid.ErrorValue());
            if (const Result<void> valid = ValidateGeneratedGameplayDescriptorBundle(*bundle, expectation); valid.HasError())
                return Result<ValidatedModuleExports>::Failure(valid.ErrorValue());
            return Result<ValidatedModuleExports>::Success({descriptor, bundle});
        }

        [[nodiscard]] Result<std::unique_ptr<BehaviorRegistry>> BuildRegistry(const GeneratedGameplayDescriptorBundle &bundle) {
            auto registry = std::make_unique<BehaviorRegistry>();
            for (std::size_t index = 0; index < bundle.behaviorCount; ++index) {
                BehaviorRegistration registration{
                    .descriptor = bundle.behaviors[index],
                    .factory = bundle.nativeFactoryBindings[index].factory,
                };
                if (Result<void> registered = registry->Register(std::move(registration)); registered.HasError())
                    return Result<std::unique_ptr<BehaviorRegistry>>::Failure(registered.ErrorValue());
            }
            if (Result<void> frozen = registry->Freeze(); frozen.HasError())
                return Result<std::unique_ptr<BehaviorRegistry>>::Failure(frozen.ErrorValue());
            return Result<std::unique_ptr<BehaviorRegistry>>::Success(std::move(registry));
        }

        /** @brief Check the host-bound principal before factories; unavailable/denied optional access does not disable the module. */
        Result<std::shared_ptr<GameplayPhysicsContext>> ResolvePhysicsContext(const std::shared_ptr<GameplayPhysicsContext> &physics,
                                                                              const std::string &moduleId) {
            if (!physics)
                return Result<std::shared_ptr<GameplayPhysicsContext>>::Success(
                    GameplayPhysicsContext::Withheld({.moduleId = moduleId}, GameplayPhysicsDenial::Unavailable));
            const auto &binding = physics->Binding();
            if (binding.moduleId != moduleId)
                return Result<std::shared_ptr<GameplayPhysicsContext>>::Failure(MakeError(GameplayErrors::PhysicsPermissionDenied));
            if (const auto admission = physics->Acquire(moduleId, binding.scene, binding.sceneGeneration);
                admission.HasError() && admission.ErrorValue().code.Value() != GameplayErrors::PhysicsPermissionDenied.code.Value() &&
                admission.ErrorValue().code.Value() != GameplayErrors::PhysicsUnavailable.code.Value())
                return Result<std::shared_ptr<GameplayPhysicsContext>>::Failure(admission.ErrorValue());
            return Result<std::shared_ptr<GameplayPhysicsContext>>::Success(physics);
        }
    }  // namespace

    /** @copydoc GameModuleHost::ValidateArtifact */
    Result<void> GameModuleHost::ValidateArtifact(const std::filesystem::path &libraryPath,
                                                  const GameModuleLoadExpectation &expectation) const {
        if (!libraryPath.is_absolute())
            return Result<void>::Failure(MakeError(GameplayErrors::InvalidGameModuleDescriptor));
        auto image = Platform::LoadDynamicLibrary(libraryPath.string());
        if (image.HasError())
            return Result<void>::Failure(image.ErrorValue());
        auto validated = ReadValidatedExports(*image.Value(), expectation);
        if (validated.HasError())
            return Result<void>::Failure(validated.ErrorValue());
        return Result<void>::Success();
    }

    /** @copydoc GameModuleHost::Load */
    Result<GameModuleLoadCandidate> GameModuleHost::Load(const std::filesystem::path &libraryPath,
                                                         const GameModuleLoadExpectation &expectation) const {
        if (!libraryPath.is_absolute())
            return Result<GameModuleLoadCandidate>::Failure(
                MakeError(GameplayErrors::InvalidGameModuleDescriptor, "Gameplay module path must be absolute."));

        auto loaded = Platform::LoadDynamicLibrary(libraryPath.string());
        if (loaded.HasError())
            return Result<GameModuleLoadCandidate>::Failure(loaded.ErrorValue());
        auto impl = std::make_shared<LoadedGameModule::Impl>();
        impl->library = std::move(loaded).Value();
        impl->loadedArtifactPath = libraryPath;

        auto exports = ReadValidatedExports(*impl->library, expectation);
        if (exports.HasError())
            return Result<GameModuleLoadCandidate>::Failure(exports.ErrorValue());
        const ValidatedModuleExports validated = exports.Value();
        impl->moduleId = validated.descriptor->moduleId;
        impl->buildFingerprint = validated.descriptor->buildFingerprint;
        impl->descriptorRevision = validated.bundle->descriptorRevision;
        impl->destroy = validated.bundle->lifecycle.destroy;
        auto physics = ResolvePhysicsContext(physics_, impl->moduleId);
        if (physics.HasError())
            return Result<GameModuleLoadCandidate>::Failure(physics.ErrorValue());
        impl->physics = std::move(physics).Value();

        auto registry = BuildRegistry(*validated.bundle);
        if (registry.HasError())
            return Result<GameModuleLoadCandidate>::Failure(registry.ErrorValue());
        impl->registries.registry = std::move(registry).Value();

        // Allocate the exported RAII wrapper before any module factory can publish work.
        GameModuleLoadCandidate candidate{std::unique_ptr<LoadedGameModule>{new LoadedGameModule{impl}}, {}};
        auto initializationFailure =
            MakeError(GameplayErrors::GameplayFactoryFailed, "Gameplay host initialization failed after factory admission.");
        try {
            impl->gameplayModule = validated.bundle->lifecycle.create();
            if (impl->gameplayModule == nullptr)
                candidate.diagnostic =
                    MakeError(GameplayErrors::InvalidBehaviorComponent, "Gameplay module factory returned no module object.");
            else if (auto activated = impl->RegisterAndStart(hostCapabilities_); activated.HasError())
                candidate.diagnostic = activated.ErrorValue();
        } catch (...) {
            candidate.diagnostic = std::move(initializationFailure);
        }
        if (candidate.diagnostic)
            candidate.owner->RequestRetirement();
        return Result<GameModuleLoadCandidate>::Success(std::move(candidate));
    }

}  // namespace Horo::Gameplay
