#include "Horo/Gameplay/GameModuleHost.h"

#include "GameModuleHostDetail.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/PathUtils.h"
#include "Horo/Gameplay/GameplayCharacterContext.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "Horo/Gameplay/GameplayNavigationContext.h"
#include "Horo/Gameplay/GameplayPhysicsContext.h"
#include "Horo/Runtime/Ui/RuntimeUiClient.h"

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
            if (descriptor == nullptr)
                return Result<ValidatedModuleExports>::Failure(
                    MakeError(GameplayErrors::InvalidGameModuleDescriptor, "Gameplay module descriptor export returned null."));
            if (const Result<void> valid = ValidateGameModuleDescriptor(*descriptor, expectation); valid.HasError())
                return Result<ValidatedModuleExports>::Failure(valid.ErrorValue());
            const GeneratedGameplayDescriptorBundle *bundle = getBundle();
            if (bundle == nullptr)
                return Result<ValidatedModuleExports>::Failure(
                    MakeError(GameplayErrors::InvalidGeneratedDescriptorBundle, "Generated gameplay descriptor export returned null."));
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

        /** @brief Establish code-image ownership and shadow cleanup before opening native code or resolving exports. */
        [[nodiscard]] Result<std::shared_ptr<Detail::GameModuleCodeImage>> OpenCodeImage(const std::filesystem::path &libraryPath,
                                                                                         const bool ownedShadow) {
            auto image = std::make_shared<Detail::GameModuleCodeImage>(std::unique_ptr<Platform::DynamicLibrary>{});
            if (ownedShadow)
                image->shadowArtifact = Foundation::Paths::NativeApiPath(libraryPath);
            auto loaded = Platform::LoadDynamicLibraryAtPath(libraryPath);
            if (loaded.HasError())
                return Result<std::shared_ptr<Detail::GameModuleCodeImage>>::Failure(loaded.ErrorValue());
            image->library = std::move(loaded).Value();
            return Result<std::shared_ptr<Detail::GameModuleCodeImage>>::Success(std::move(image));
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

        /** @brief Resolve only the selected principal, preserving an explicit unavailable context for omitted navigation. */
        Result<std::shared_ptr<GameplayNavigationContext>> ResolveNavigationContext(
            const std::shared_ptr<GameplayNavigationContext> &navigation, const std::string &moduleId) {
            if (navigation && navigation->Binding().moduleId != moduleId)
                return Result<std::shared_ptr<GameplayNavigationContext>>::Failure(MakeError(NavigationCapabilityErrors::PermissionDenied));
            if (navigation && !navigation->IsUsable())
                return Result<std::shared_ptr<GameplayNavigationContext>>::Failure(MakeError(NavigationCapabilityErrors::ScopeRevoked));
            if (navigation)
                return Result<std::shared_ptr<GameplayNavigationContext>>::Success(navigation);
            return GameplayNavigationContext::Create({.moduleId = moduleId, .queryGranted = true, .movementGranted = true}, nullptr,
                                                     nullptr, nullptr, {});
        }
    }  // namespace

    /** @brief Owns validated generated contributions before any module factory or lifecycle callback can run. */
    Result<void> LoadedGameModule::Impl::CaptureGeneratedContributions(const GeneratedGameplayDescriptorBundle &bundle) {
        auto registry = BuildRegistry(bundle);
        if (registry.HasError())
            return Result<void>::Failure(registry.ErrorValue());
        registries.registry = std::move(registry).Value();
        if (bundle.decisionNodeCount != 0)
            decisionNodes.assign(bundle.decisionNodes, bundle.decisionNodes + bundle.decisionNodeCount);
        if (bundle.perceptionSenseCount != 0)
            perceptionSenses.assign(bundle.perceptionSenses, bundle.perceptionSenses + bundle.perceptionSenseCount);
        return Result<void>::Success();
    }

    /** @copydoc GameModuleHost::ValidateArtifact */
    Result<void> GameModuleHost::ValidateArtifact(const std::filesystem::path &libraryPath,
                                                  const GameModuleLoadExpectation &expectation) const {
        if (!libraryPath.is_absolute())
            return Result<void>::Failure(MakeError(GameplayErrors::InvalidGameModuleDescriptor));
        auto image = Platform::LoadDynamicLibraryAtPath(libraryPath);
        if (image.HasError())
            return Result<void>::Failure(image.ErrorValue());
        if (auto validated = ReadValidatedExports(*image.Value(), expectation); validated.HasError())
            return Result<void>::Failure(validated.ErrorValue());
        return Result<void>::Success();
    }

    /** @copydoc GameModuleHost::Load */
    Result<GameModuleLoadCandidate> GameModuleHost::Load(const std::filesystem::path &libraryPath,
                                                         const GameModuleLoadExpectation &expectation) const {
        return LoadArtifact(libraryPath, expectation, false);
    }

    /** @copydoc GameModuleHost::LoadArtifact */
    Result<GameModuleLoadCandidate> GameModuleHost::LoadArtifact(const std::filesystem::path &libraryPath,
                                                                 const GameModuleLoadExpectation &expectation,
                                                                 const bool ownedShadow) const {
        auto prepared = PrepareArtifactImage(libraryPath, expectation, ownedShadow, {});
        if (prepared.HasError())
            return Result<GameModuleLoadCandidate>::Failure(prepared.ErrorValue());
        auto owner = std::move(prepared).Value();
        return ActivatePreparedImage(owner, {});
    }

    /** @copydoc GameModuleHost::PrepareImage */
    Result<std::unique_ptr<PreparedGameModuleImage>> GameModuleHost::PrepareImage(const std::filesystem::path &libraryPath,
                                                                                  const GameModuleLoadExpectation &expectation,
                                                                                  const CancellationToken &cancellation) {
        using Output = Result<std::unique_ptr<PreparedGameModuleImage>>;
        auto owner = PrepareArtifactImage(libraryPath, expectation, false, cancellation);
        if (owner.HasError())
            return Output::Failure(owner.ErrorValue());
        return Output::Success(std::unique_ptr<PreparedGameModuleImage>(new PreparedGameModuleImage(std::move(owner).Value())));
    }

    /** @copydoc GameModuleHost::PrepareArtifactImage */
    Result<std::unique_ptr<LoadedGameModule>> GameModuleHost::PrepareArtifactImage(const std::filesystem::path &libraryPath,
                                                                                   const GameModuleLoadExpectation &expectation,
                                                                                   const bool ownedShadow,
                                                                                   const CancellationToken &cancellation) {
        using Output = Result<std::unique_ptr<LoadedGameModule>>;
        if (cancellation.IsCancellationRequested())
            return Output::Failure(JobCancelled().ErrorValue());
        if (!libraryPath.is_absolute())
            return Output::Failure(MakeError(GameplayErrors::InvalidGameModuleDescriptor, "Gameplay module path must be absolute."));

        auto image = OpenCodeImage(libraryPath, ownedShadow);
        if (image.HasError())
            return Output::Failure(image.ErrorValue());
        auto impl = std::make_shared<LoadedGameModule::Impl>();
        impl->codeImage = std::move(image).Value();
        impl->loadedArtifactPath = libraryPath;
        const auto exports = ReadValidatedExports(*impl->codeImage->library, expectation);
        if (exports.HasError())
            return Output::Failure(exports.ErrorValue());
        const auto &validated = exports.Value();
        impl->moduleId = validated.descriptor->moduleId;
        impl->buildFingerprint = validated.descriptor->buildFingerprint;
        impl->descriptorRevision = validated.bundle->descriptorRevision;
        impl->create = validated.bundle->lifecycle.create;
        impl->destroy = validated.bundle->lifecycle.destroy;
        if (auto captured = impl->CaptureGeneratedContributions(*validated.bundle); captured.HasError())
            return Output::Failure(captured.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Output::Failure(JobCancelled().ErrorValue());
        // This wrapper already owns rollback before platform services or module factories can publish work.
        return Output::Success(std::make_unique<LoadedGameModule>(LoadedGameModule::ConstructionKey{}, std::move(impl)));
    }

    /** @copydoc GameModuleHost::ActivatePrepared */
    Result<GameModuleLoadCandidate> GameModuleHost::ActivatePrepared(PreparedGameModuleImage &prepared,
                                                                     const CancellationToken &cancellation) const {
        if (cancellation.IsCancellationRequested())
            return ActivatePreparedImage(prepared.owner_, cancellation);
        if (!runtimeServices_.physics || !runtimeServices_.character)
            return Result<GameModuleLoadCandidate>::Failure(
                MakeError(GameplayErrors::PhysicsUnavailable, "Prepared gameplay activation requires paired Scene service scopes."));
        const auto &physics = runtimeServices_.physics->Binding();
        const auto &character = runtimeServices_.character->Binding();
        if (physics.scene == 0 || physics.sceneGeneration == 0 || character.scene != physics.scene ||
            character.sceneGeneration != physics.sceneGeneration)
            return Result<GameModuleLoadCandidate>::Failure(
                MakeError(GameplayErrors::PhysicsUnavailable, "Prepared gameplay scopes must belong to the same live Scene."));
        return ActivatePreparedImage(prepared.owner_, cancellation);
    }

    /** @copydoc GameModuleHost::ActivatePreparedImage */
    Result<GameModuleLoadCandidate> GameModuleHost::ActivatePreparedImage(std::unique_ptr<LoadedGameModule> &prepared,
                                                                          const CancellationToken &cancellation) const {
        using Output = Result<GameModuleLoadCandidate>;
        if (!prepared || std::this_thread::get_id() != prepared->impl_->ownerThread)
            return Output::Failure(
                MakeError(GameplayErrors::InvalidGameModuleDescriptor, "Prepared module is consumed or on a foreign lane."));
        auto owner = std::move(prepared);
        if (cancellation.IsCancellationRequested())
            return Output::Failure(JobCancelled().ErrorValue());
        auto &impl = *owner->impl_;
        if (const auto bound = BindRuntimeServices(impl); bound.HasError()) {
            owner->RequestRetirement();
            return Output::Failure(bound.ErrorValue());
        }

        std::optional<Error> diagnostic;
        auto initializationFailure =
            MakeError(GameplayErrors::GameplayFactoryFailed, "Gameplay host initialization failed after factory admission.");
        try {
            impl.gameplayModule = impl.create();
            if (impl.gameplayModule == nullptr)
                diagnostic = MakeError(GameplayErrors::InvalidBehaviorComponent, "Gameplay module factory returned no module object.");
            else if (auto activated = impl.RegisterAndStart(hostCapabilities_, replicationAdmission_); activated.HasError())
                diagnostic = activated.ErrorValue();
            else if (cancellation.IsCancellationRequested())
                diagnostic = JobCancelled().ErrorValue();
        } catch (...) {  // NOSONAR(cpp:S2738) Native module boundary contains arbitrary plugin exceptions and preserves rollback ownership.
            diagnostic = std::move(initializationFailure);
        }
        if (diagnostic)
            owner->RequestRetirement();
        else
            impl.decisionFactoryAdmission.store(true, std::memory_order_release);
        return Output::Success({std::move(owner), std::move(diagnostic)});
    }

    /** @copydoc GameModuleHost::BindRuntimeServices */
    Result<void> GameModuleHost::BindRuntimeServices(LoadedGameModule::Impl &impl) const {
        auto physics = ResolvePhysicsContext(runtimeServices_.physics, impl.moduleId);
        if (physics.HasError())
            return Result<void>::Failure(physics.ErrorValue());
        auto navigation = ResolveNavigationContext(runtimeServices_.navigation, impl.moduleId);
        if (navigation.HasError())
            return Result<void>::Failure(navigation.ErrorValue());
        if (auto pinned = navigation.Value()->RetainCodeImage(impl.codeImage); pinned.HasError())
            return Result<void>::Failure(pinned.ErrorValue());
        impl.navigation = std::move(navigation).Value();
        impl.physics = std::move(physics).Value();
        if (runtimeServices_.runtimeUi) {
            if (runtimeServices_.runtimeUi->Grant().moduleId != impl.moduleId)
                return Result<void>::Failure(MakeError(Runtime::Ui::UiErrors::CapabilityDenied));
            auto ui = runtimeServices_.runtimeUi->Derive();
            if (ui.HasError())
                return Result<void>::Failure(ui.ErrorValue());
            impl.runtimeUi = std::move(ui).Value();
        }

        if (runtimeServices_.character) {
            if (const auto &binding = runtimeServices_.character->Binding(); binding.moduleId != impl.moduleId)
                return Result<void>::Failure(MakeError(GameplayErrors::CharacterPermissionDenied));
            if (auto dependency = runtimeServices_.character->ValidateDependency(impl.physics.get()); dependency.HasError())
                return Result<void>::Failure(dependency.ErrorValue());
            auto character = runtimeServices_.character->Derive();
            if (character.HasError())
                return Result<void>::Failure(character.ErrorValue());
            impl.character = std::move(character).Value();
        } else {
            impl.character = GameplayCharacterContext::Withheld({.moduleId = impl.moduleId}, GameplayCharacterDenial::Unavailable);
        }
        if (runtimeServices_.ai && runtimeServices_.ai->Binding().moduleId != impl.moduleId)
            return Result<void>::Failure(MakeError(AI::AIErrors::CapabilityUnavailable));
        // Derive before registration so rollback cannot revoke the reusable host anchor.
        if (runtimeServices_.ai) {
            auto child = runtimeServices_.ai->Derive({});
            if (child.HasError())
                return Result<void>::Failure(child.ErrorValue());
            impl.ai = std::move(child).Value();
        }
        return Result<void>::Success();
    }

}  // namespace Horo::Gameplay
