#include "Horo/Gameplay/GameModuleHost.h"

#include "GameModuleHostDetail.h"
#include "Horo/Gameplay/GameplayErrors.h"

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
    }  // namespace

    /** @copydoc GameModuleHost::Load */
    Result<std::unique_ptr<LoadedGameModule>> GameModuleHost::Load(const std::filesystem::path &libraryPath,
                                                                   const GameModuleLoadExpectation &expectation) const {
        if (!libraryPath.is_absolute())
            return Result<std::unique_ptr<LoadedGameModule>>::Failure(
                MakeError(GameplayErrors::InvalidGameModuleDescriptor, "Gameplay module path must be absolute."));

        auto loaded = Platform::LoadDynamicLibrary(libraryPath.string());
        if (loaded.HasError())
            return Result<std::unique_ptr<LoadedGameModule>>::Failure(loaded.ErrorValue());
        auto impl = std::make_shared<LoadedGameModule::Impl>();
        impl->library = std::move(loaded).Value();
        impl->loadedArtifactPath = libraryPath;

        auto exports = ReadValidatedExports(*impl->library, expectation);
        if (exports.HasError())
            return Result<std::unique_ptr<LoadedGameModule>>::Failure(exports.ErrorValue());
        const ValidatedModuleExports validated = exports.Value();
        impl->moduleId = validated.descriptor->moduleId;
        impl->buildFingerprint = validated.descriptor->buildFingerprint;
        impl->descriptorRevision = validated.bundle->descriptorRevision;
        impl->destroy = validated.bundle->lifecycle.destroy;

        auto registry = BuildRegistry(*validated.bundle);
        if (registry.HasError())
            return Result<std::unique_ptr<LoadedGameModule>>::Failure(registry.ErrorValue());
        impl->registries.registry = std::move(registry).Value();

        impl->gameplayModule = validated.bundle->lifecycle.create();
        if (impl->gameplayModule == nullptr)
            return Result<std::unique_ptr<LoadedGameModule>>::Failure(
                MakeError(GameplayErrors::InvalidBehaviorComponent, "Gameplay module factory returned no module object."));
        if (Result<void> activated = impl->RegisterAndStart(hostCapabilities_); activated.HasError())
            return Result<std::unique_ptr<LoadedGameModule>>::Failure(activated.ErrorValue());
        return Result<std::unique_ptr<LoadedGameModule>>::Success(
            std::unique_ptr<LoadedGameModule>{new LoadedGameModule{std::move(impl)}});  // NOSONAR(cpp:S5950)
    }

}  // namespace Horo::Gameplay
