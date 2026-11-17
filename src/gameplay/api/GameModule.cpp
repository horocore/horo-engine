#include "Horo/Gameplay/GameModule.h"

#include "Horo/Gameplay/GameplayErrors.h"

#include <format>
#include <optional>

#if !defined(HORO_GAMEPLAY_SDK_FINGERPRINT)
#define HORO_GAMEPLAY_SDK_FINGERPRINT "horo-unknown-gameplay-sdk"
#endif

namespace Horo::Gameplay {
    /** @copydoc IGameModule::PrepareReload */
    Result<GameModuleReloadSnapshot> IGameModule::PrepareReload(GameRuntimeContext &) {
        return Result<GameModuleReloadSnapshot>::Failure(MakeError(GameplayErrors::GameplayReloadRestartRequired));
    }

    /** @copydoc IGameModule::RestoreReload */
    Result<void> IGameModule::RestoreReload(const GameModuleReloadSnapshot &snapshot, GameRuntimeContext &) {
        if (snapshot.schemaVersion != GameModuleReloadSnapshotSchemaVersion || !snapshot.payload.empty())
            return Result<void>::Failure(MakeError(GameplayErrors::GameplayReloadRestoreFailed));
        return Result<void>::Success();
    }

    namespace {
        constexpr std::size_t MaximumModuleIdentityBytes = 256;

        [[nodiscard]] std::optional<std::string_view> BoundedText(const char *value, const std::size_t maximumBytes) noexcept {
            if (value == nullptr)
                return std::nullopt;
            std::size_t length = 0;
            while (length <= maximumBytes && value[length] != '\0')
                ++length;
            if (length == 0 || length > maximumBytes)
                return std::nullopt;
            return std::string_view{value, length};
        }

        template <typename Record>
        [[nodiscard]] bool IsBoundedArray(const std::size_t count, const std::size_t maximum, const Record *records) noexcept {
            return count <= maximum && (count == 0) == (records == nullptr);
        }

        Result<void> ValidateBundleArrays(const GeneratedGameplayDescriptorBundle &bundle) {
            if (!IsBoundedArray(bundle.behaviorCount, MaximumGeneratedBehaviorDescriptors, bundle.behaviors) ||
                bundle.nativeFactoryBindingCount != bundle.behaviorCount ||
                !IsBoundedArray(bundle.nativeFactoryBindingCount, MaximumGeneratedBehaviorDescriptors, bundle.nativeFactoryBindings) ||
                !IsBoundedArray(bundle.diagnosticCount, MaximumGeneratedDescriptorDiagnostics, bundle.diagnostics))
                return Result<void>::Failure(MakeError(GameplayErrors::InvalidGeneratedDescriptorBundle));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateBundleShape(const GeneratedGameplayDescriptorBundle &bundle) {
            if (bundle.structSize != sizeof(GeneratedGameplayDescriptorBundle) ||
                bundle.schemaVersion != GameplayDescriptorBundleSchemaVersion || bundle.sdkBoundaryVersion != GameplaySdkBoundaryVersion ||
                bundle.lifecycle.create == nullptr || bundle.lifecycle.destroy == nullptr)
                return Result<void>::Failure(MakeError(GameplayErrors::InvalidGeneratedDescriptorBundle));
            return ValidateBundleArrays(bundle);
        }

        Result<void> ValidateDecisionNodes(const GeneratedGameplayDescriptorBundle &bundle) {
            if (!IsBoundedArray(bundle.decisionNodeCount, AI::DecisionNodeGeneration::MaximumNodes, bundle.decisionNodes))
                return Result<void>::Failure(MakeError(GameplayErrors::InvalidGeneratedDescriptorBundle));
            for (std::size_t index = 0; index < bundle.decisionNodeCount; ++index) {
                if (auto valid = AI::ValidateDecisionNodeRegistration(bundle.decisionNodes[index]); valid.HasError())
                    return valid;
                for (std::size_t previous = 0; previous < index; ++previous)
                    if (bundle.decisionNodes[previous].contract.descriptor.type == bundle.decisionNodes[index].contract.descriptor.type)
                        return Result<void>::Failure(MakeError(AI::AIErrors::DecisionAssetDescriptorAmbiguous));
            }
            return Result<void>::Success();
        }

        /** @brief Validates inert custom-sense contributions before any provider or module factory can run. */
        Result<void> ValidatePerceptionSenses(const GeneratedGameplayDescriptorBundle &bundle) {
            if (!IsBoundedArray(bundle.perceptionSenseCount, AI::PerceptionSenseGeneration::MaximumSenses, bundle.perceptionSenses))
                return Result<void>::Failure(MakeError(GameplayErrors::InvalidGeneratedDescriptorBundle));
            for (std::size_t index = 0; index < bundle.perceptionSenseCount; ++index) {
                const auto &registration = bundle.perceptionSenses[index];
                if (auto valid = AI::ValidatePerceptionSenseRegistration(registration); valid.HasError())
                    return valid;
                for (std::size_t previous = 0; previous < index; ++previous) {
                    const auto &other = bundle.perceptionSenses[previous].contract;
                    if (other.sense.identity == registration.contract.sense.identity ||
                        other.stimulus.identity == registration.contract.stimulus.identity)
                        return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionDescriptorConflict));
                }
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateBundleIdentity(const GeneratedGameplayDescriptorBundle &bundle,
                                                          const GameModuleLoadExpectation &expectation) {
            const auto moduleId = BoundedText(bundle.moduleId, MaximumModuleIdentityBytes);
            const auto fingerprint = BoundedText(bundle.buildFingerprint, MaximumModuleIdentityBytes);
            if (!moduleId || !fingerprint || bundle.descriptorRevision == 0)
                return Result<void>::Failure(MakeError(GameplayErrors::InvalidGeneratedDescriptorBundle));
            if (*moduleId != expectation.moduleId || *fingerprint != expectation.buildFingerprint ||
                bundle.descriptorRevision != expectation.descriptorRevision)
                return Result<void>::Failure(MakeError(GameplayErrors::IncompatibleGameModule));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateGeneratedDiagnostics(const GeneratedGameplayDescriptorBundle &bundle) {
            if (bundle.diagnosticCount == 0)
                return Result<void>::Success();
            for (std::size_t index = 0; index < bundle.diagnosticCount; ++index) {
                const auto code = BoundedText(bundle.diagnostics[index].code, MaximumGeneratedDiagnosticCodeBytes);
                const auto message = BoundedText(bundle.diagnostics[index].message, MaximumGeneratedDiagnosticMessageBytes);
                if (!code || !message)
                    return Result<void>::Failure(MakeError(GameplayErrors::InvalidGeneratedDescriptorBundle));
            }
            const GeneratedDescriptorDiagnostic &first = bundle.diagnostics[0];
            return Result<void>::Failure(
                MakeError(GameplayErrors::GeneratedDescriptorDiagnosticsPresent, std::format("{}: {}", first.code, first.message)));
        }

        [[nodiscard]] Result<void> ValidateFactoryBindings(const GeneratedGameplayDescriptorBundle &bundle) {
            for (std::size_t index = 0; index < bundle.behaviorCount; ++index) {
                const GeneratedBehaviorFactoryBinding &binding = bundle.nativeFactoryBindings[index];
                if (!bundle.behaviors[index].typeId.IsValid() || binding.typeId != bundle.behaviors[index].typeId ||
                    binding.factory.create == nullptr || binding.factory.destroy == nullptr)
                    return Result<void>::Failure(MakeError(GameplayErrors::InvalidGeneratedDescriptorBundle));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc CurrentGameplayBuildFingerprint */
    std::string_view CurrentGameplayBuildFingerprint() noexcept {
        return HORO_GAMEPLAY_SDK_FINGERPRINT;
    }

    /** @copydoc ValidateGameModuleDescriptor */
    Result<void> ValidateGameModuleDescriptor(const GameModuleDescriptor &descriptor, const GameModuleLoadExpectation &expectation) {
        const auto moduleId = BoundedText(descriptor.moduleId, MaximumModuleIdentityBytes);
        const auto fingerprint = BoundedText(descriptor.buildFingerprint, MaximumModuleIdentityBytes);
        if (descriptor.structSize != sizeof(GameModuleDescriptor) || !moduleId || !fingerprint || expectation.moduleId.empty() ||
            expectation.buildFingerprint.empty() || expectation.descriptorRevision == 0)
            return Result<void>::Failure(MakeError(GameplayErrors::InvalidGameModuleDescriptor));
        if (descriptor.sdkBoundaryVersion != GameplaySdkBoundaryVersion || *moduleId != expectation.moduleId ||
            *fingerprint != expectation.buildFingerprint)
            return Result<void>::Failure(MakeError(GameplayErrors::IncompatibleGameModule));
        return Result<void>::Success();
    }

    /** @copydoc ValidateGeneratedGameplayDescriptorBundle */
    Result<void> ValidateGeneratedGameplayDescriptorBundle(const GeneratedGameplayDescriptorBundle &bundle,
                                                           const GameModuleLoadExpectation &expectation) {
        if (const Result<void> shape = ValidateBundleShape(bundle); shape.HasError())
            return shape;
        if (const Result<void> identity = ValidateBundleIdentity(bundle, expectation); identity.HasError())
            return identity;
        if (const Result<void> diagnostics = ValidateGeneratedDiagnostics(bundle); diagnostics.HasError())
            return diagnostics;
        if (auto factories = ValidateFactoryBindings(bundle); factories.HasError())
            return factories;
        if (auto nodes = ValidateDecisionNodes(bundle); nodes.HasError())
            return nodes;
        return ValidatePerceptionSenses(bundle);
    }
}  // namespace Horo::Gameplay
