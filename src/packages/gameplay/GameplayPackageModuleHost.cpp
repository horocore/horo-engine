#include "Horo/Packages/GameplayPackageModuleHost.h"

#include "Horo/Gameplay/BehaviorRegistry.h"
#include "Horo/Gameplay/ComponentRegistry.h"
#include "Horo/Gameplay/GameAssetTypeRegistry.h"
#include "Horo/Gameplay/GameServiceRegistry.h"
#include "Horo/Gameplay/SystemRegistry.h"
#include "Horo/Packages/PackageLifecycle.h"
#include "Horo/Packages/PackageLifecycleErrors.h"
#include "PackageContentStaging.h"

#include <algorithm>
#include <new>
#include <thread>

namespace Horo::Packages {
    namespace {
        /** @brief Owner-lane SDK module and its verified resources, retained by every borrowed runtime view. */
        struct GameplayRoleState final {
            VerifiedPackageModule verified;
            std::vector<std::shared_ptr<IPackageModuleActivation>> dependencies;
            std::filesystem::path root;
            std::unique_ptr<Gameplay::GameModuleHost> host;
            std::unique_ptr<Gameplay::LoadedGameModule> selectedModule;
            std::optional<Error> failure;
            std::thread::id owner{std::this_thread::get_id()};
            bool retired{};
            bool finalized{};

            GameplayRoleState(VerifiedPackageModule verifiedModule, const std::span<const std::shared_ptr<IPackageModuleActivation>> owners)
                : verified(std::move(verifiedModule)), dependencies(owners.begin(), owners.end()) {}

            ~GameplayRoleState() {
                // Framework retirement keeps this owner until callbacks and all borrowed views have drained.
                selectedModule.reset();
                if (!root.empty()) {
                    std::error_code ignored;
                    std::filesystem::remove_all(root, ignored);
                }
            }
        };

        /** @brief Actual gameplay SDK receipt; cancellation closes admission before owner-lane drain and native unload. */
        class GameplayRoleReceipt final : public IPackageModuleActivation {
        public:
            explicit GameplayRoleReceipt(std::shared_ptr<GameplayRoleState> state) : state_(std::move(state)) {}

            bool IsActive() const noexcept override {
                return !state_->retired && !state_->failure && state_->selectedModule && state_->verified.Activation().IsUsable();
            }

            std::optional<Error> Diagnostic() const override {
                return state_->failure;
            }

            void Retire() noexcept override {
                if (state_->retired)
                    return;
                state_->retired = true;
                if (state_->selectedModule)
                    state_->selectedModule->RequestRetirement();
            }

            bool Finalize() noexcept override {
                if (!state_->retired || std::this_thread::get_id() != state_->owner || state_.use_count() != 1)
                    return false;
                try {
                    if (state_->selectedModule) {
                        if (auto retired = state_->selectedModule->TryRetire(); retired.HasError())
                            return false;
                        state_->selectedModule.reset();
                    }
                    std::error_code error;
                    if (!state_->root.empty())
                        std::filesystem::remove_all(state_->root, error);
                    if (error)
                        return false;
                    state_->root.clear();
                    state_->dependencies.clear();
                    state_->host.reset();
                    state_->finalized = true;
                    return true;
                } catch (...) {  // NOSONAR(cpp:S2738) Native module boundary contains arbitrary plugin exceptions and preserves rollback
                                 // ownership.
                    return false;
                }
            }

        private:
            std::shared_ptr<GameplayRoleState> state_;
        };

        /** @brief Requires the exact capability already intersected with the runtime permission before any executable factory. */
        bool HasRuntimeGrant(const VerifiedPackageModule &selectedModule) {
            return std::ranges::any_of(selectedModule.Capabilities(), [](const auto &grant) {
                return grant.Capability().value == "runtime.participate" && grant.IsUsable();
            });
        }

        bool LiveRole(const GameplayRoleState &state) {
            return !state.retired && !state.failure && state.selectedModule && state.verified.Activation().IsUsable() &&
                   HasRuntimeGrant(state.verified);
        }

        bool SelectedRole(const GameplayRoleState &state, const HoroPackageId &package, std::string_view moduleId,
                          std::uint64_t generation) {
            return state.verified.Identity().package == package && state.verified.Module().id == moduleId &&
                   state.verified.Identity().generation == generation;
        }

        std::shared_ptr<GameplayRoleState> FindRole(std::span<const std::weak_ptr<GameplayRoleState>> roles, std::thread::id owner,
                                                    const HoroPackageId &package, std::string_view moduleId, std::uint64_t generation) {
            if (std::this_thread::get_id() != owner)
                return {};
            for (const auto &weak : roles) {
                auto state = weak.lock();
                if (state && LiveRole(*state) && SelectedRole(*state, package, moduleId, generation))
                    return state;
            }
            return {};
        }

        /** @brief Selects only the runtime roles published for this concrete host and generation. */
        bool IsPublishedRole(const PackageActivationSnapshot &active, const PackageModuleReport &report, std::string_view host) {
            return report.role == Extensions::ExtensionModuleRole::RuntimeParticipant && report.outcome == PackageModuleOutcome::Active &&
                   report.host == host && report.generation == active.generation;
        }

        /** @brief Matches the actual lease rather than relying on a coincidentally equal generation number. */
        bool OwnsPublishedActivation(const GameplayRoleState &state, const PackageActivationSnapshot &active) {
            if (state.verified.Install() != active.install)
                return false;
            const auto &proof = state.verified.Activation().Activation();
            return std::ranges::any_of(active.activations, [&proof](const auto &lease) {
                return lease.IsUsable() && &lease.Activation() == &proof;
            });
        }

        /** @brief Stages and verifies an immutable artifact under the host's private owner-only root. */
        Result<std::filesystem::path> StageGameplayArtifact(GameplayRoleState &state, const GameplayPackageHostConfiguration &configuration,
                                                            const CancellationToken &cancellation) {
            auto root = Detail::CreatePrivatePackageRoot(configuration.temporaryRoot, PackageLifecycleErrors::StorageFailed);
            if (root.HasError())
                return Result<std::filesystem::path>::Failure(root.ErrorValue());
            state.root = std::move(root).Value();
            const auto &packages = state.verified.Install()->Graph()->packages;
            const auto selected = std::ranges::find(packages, state.verified.Identity().package, [](const auto &package) {
                return package.lock.package;
            });
            const auto index = static_cast<std::size_t>(selected - packages.begin());
            if (auto staged =
                    Detail::MaterializeVerifiedPackageRoot(*state.verified.Install(), index, state.verified.Descriptor(), state.root,
                                                           cancellation,
                                                           {PackageLifecycleErrors::InvalidCandidate, PackageLifecycleErrors::StorageFailed,
                                                            PackageLifecycleErrors::Cancelled});
                staged.HasError())
                return Result<std::filesystem::path>::Failure(staged.ErrorValue());
            const auto artifact = state.root / state.verified.Artifact().Value();
            const auto files = state.verified.Package().archive->Manifest().Entries();
            const auto expected = std::ranges::find_if(files, [&state](const auto &file) {
                return file.id.path.Value() == state.verified.Artifact().Value();
            });
            if (expected == files.end())
                return Result<std::filesystem::path>::Failure(MakeError(PackageLifecycleErrors::InvalidCandidate));
            auto evidence = configuration.artifactGate->Verify(artifact);
            if (evidence.HasError())
                return Result<std::filesystem::path>::Failure(evidence.ErrorValue());
            if (evidence.Value().ArtifactDigest() != expected->digest)
                return Result<std::filesystem::path>::Failure(MakeError(PackageLifecycleErrors::TrustRequired));
            return Result<std::filesystem::path>::Success(artifact);
        }

        /** @brief Checks SDK identity before binding world authority, then retains any partial native initializer. */
        Result<void> LoadGameplayOwner(GameplayRoleState &state, const GameplayPackageModuleBinding &binding,
                                       const std::filesystem::path &artifact, const CancellationToken &cancellation) {
            const Gameplay::GameModuleLoadExpectation expected{binding.module, binding.buildFingerprint, binding.descriptorRevision};
            Gameplay::GameModuleHost validation{binding.capabilities};
            if (auto compatible = validation.ValidateArtifact(artifact, expected); compatible.HasError())
                return compatible;
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(PackageLifecycleErrors::Cancelled));
            std::shared_ptr<Gameplay::GameplayPhysicsContext> physics;
            if (binding.physics) {
                auto resolved = binding.physics(state.verified);
                if (resolved.HasError())
                    return Result<void>::Failure(resolved.ErrorValue());
                physics = std::move(resolved).Value();
            }
            state.host = std::make_unique<Gameplay::GameModuleHost>(binding.capabilities, std::move(physics));
            auto loaded = state.host->Load(artifact, expected);
            if (loaded.HasError())
                return Result<void>::Failure(loaded.ErrorValue());
            auto candidate = std::move(loaded).Value();
            state.selectedModule = std::move(candidate.owner);
            if (candidate.diagnostic)
                return Result<void>::Failure(std::move(*candidate.diagnostic));
            return Result<void>::Success();
        }
    }  // namespace

    struct GameplayPackageModuleHost::Impl final {
        GameplayPackageHostConfiguration configuration;
        std::vector<GameplayPackageModuleBinding> bindings;
        std::vector<std::weak_ptr<GameplayRoleState>> roles;
        std::thread::id owner{std::this_thread::get_id()};
        bool valid{};
    };

    /** @copydoc GameplayPackageModuleHost::GameplayPackageModuleHost */
    GameplayPackageModuleHost::GameplayPackageModuleHost(GameplayPackageHostConfiguration configuration,
                                                         std::vector<GameplayPackageModuleBinding> bindings)
        : impl_(std::make_unique<Impl>()) {
        impl_->configuration = std::move(configuration);
        impl_->bindings = std::move(bindings);
        impl_->roles.reserve(64);
        std::error_code error;
        const auto canonical = std::filesystem::canonical(impl_->configuration.temporaryRoot, error);
        impl_->valid = !error && canonical == impl_->configuration.temporaryRoot && impl_->configuration.artifactGate &&
                       !impl_->bindings.empty() && impl_->bindings.size() <= 64;
        for (std::size_t index = 0; index < impl_->bindings.size(); ++index) {
            const auto &binding = impl_->bindings[index];
            if (binding.module.empty() || binding.module.size() > 256 || binding.buildFingerprint.empty() || !binding.descriptorRevision)
                impl_->valid = false;
            for (std::size_t prior = 0; prior < index; ++prior)
                if (binding.package == impl_->bindings[prior].package && binding.module == impl_->bindings[prior].module)
                    impl_->valid = false;
        }
    }

    GameplayPackageModuleHost::~GameplayPackageModuleHost() = default;

    /** @copydoc GameplayPackageModuleHost::Identity */
    std::string_view GameplayPackageModuleHost::Identity() const noexcept {
        return "gameplay.sdk";
    }

    /** @copydoc GameplayPackageModuleHost::Supports */
    bool GameplayPackageModuleHost::Supports(const Extensions::ExtensionModuleRole role) const noexcept {
        return impl_->valid && role == Extensions::ExtensionModuleRole::RuntimeParticipant;
    }

    /** @copydoc GameplayPackageModuleHost::Activate */
    Result<std::unique_ptr<IPackageModuleActivation>> GameplayPackageModuleHost::Activate(
        const VerifiedPackageModule &selectedModule, const std::span<const std::shared_ptr<IPackageModuleActivation>> dependencies,
        const CancellationToken &cancellation) {
        if (std::this_thread::get_id() != impl_->owner || !Supports(selectedModule.Identity().role) || !HasRuntimeGrant(selectedModule))
            return Result<std::unique_ptr<IPackageModuleActivation>>::Failure(MakeError(PackageLifecycleErrors::TrustRequired));
        std::erase_if(impl_->roles, [](const auto &role) {
            return role.expired();
        });
        for (const auto &weak : impl_->roles)
            if (auto previous = weak.lock(); previous && !previous->finalized &&
                                             previous->verified.Identity().package == selectedModule.Identity().package &&
                                             previous->verified.Module().id == selectedModule.Module().id)
                return Result<std::unique_ptr<IPackageModuleActivation>>::Failure(
                    MakeError(PackageLifecycleErrors::InvalidLifecycle,
                              "Disable and drain the exact prior gameplay generation before loading its replacement."));
        const auto binding = std::ranges::find_if(impl_->bindings, [&selectedModule](const auto &value) {
            return value.package == selectedModule.Identity().package && value.module == selectedModule.Module().id;
        });
        if (binding == impl_->bindings.end() || impl_->roles.size() >= 64)
            return Result<std::unique_ptr<IPackageModuleActivation>>::Failure(MakeError(PackageLifecycleErrors::InvalidCandidate));
        auto state = std::make_shared<GameplayRoleState>(selectedModule, dependencies);
        auto receipt = std::make_unique<GameplayRoleReceipt>(state);
        impl_->roles.push_back(state);
        auto activationFailure = MakeError(PackageLifecycleErrors::InvalidCandidate);
        try {
            auto artifact = StageGameplayArtifact(*state, impl_->configuration, cancellation);
            if (artifact.HasError())
                state->failure = artifact.ErrorValue();
            else if (cancellation.IsCancellationRequested())
                state->failure = MakeError(PackageLifecycleErrors::Cancelled);
            else if (auto activated = LoadGameplayOwner(*state, *binding, artifact.Value(), cancellation); activated.HasError())
                state->failure = activated.ErrorValue();
        } catch (...) {  // NOSONAR(cpp:S2738) Native module boundary contains arbitrary plugin exceptions and preserves rollback ownership.
            state->failure = std::move(activationFailure);
        }
        return Result<std::unique_ptr<IPackageModuleActivation>>::Success(std::move(receipt));
    }

    /** @copydoc GameplayPackageModuleHost::Acquire */
    std::shared_ptr<const Gameplay::LoadedGameModule> GameplayPackageModuleHost::Acquire(const HoroPackageId &package,
                                                                                         const std::string_view selectedModule,
                                                                                         const std::uint64_t generation) const {
        auto state = FindRole(impl_->roles, impl_->owner, package, selectedModule, generation);
        if (state)
            return {state, state->selectedModule.get()};
        return {};
    }

    Result<PackageDecisionNodeGeneration> GameplayPackageModuleHost::AcquireDecisionNodes(const HoroPackageId &package,
                                                                                          std::string_view moduleId,
                                                                                          std::uint64_t generation) const {
        auto state = FindRole(impl_->roles, impl_->owner, package, moduleId, generation);
        if (!state)
            return Result<PackageDecisionNodeGeneration>::Failure(MakeError(PackageLifecycleErrors::InvalidLifecycle));
        auto nodes = state->selectedModule->AcquireDecisionNodes();
        if (nodes.HasError())
            return Result<PackageDecisionNodeGeneration>::Failure(nodes.ErrorValue());
        const auto &locked = state->verified.Package().lock;
        return Result<PackageDecisionNodeGeneration>::Success(
            {package, locked.version, locked.artifactDigest, generation, std::move(nodes).Value()});
    }

    /** @copydoc GameplayPackageModuleHost::AcquireActiveDecisionNodes */
    Result<std::vector<PackageDecisionNodeGeneration>> GameplayPackageModuleHost::AcquireActiveDecisionNodes(
        const PackageLifecycleService &lifecycle) const {
        using Contributions = Result<std::vector<PackageDecisionNodeGeneration>>;
        if (std::this_thread::get_id() != impl_->owner)
            return Contributions::Failure(MakeError(PackageLifecycleErrors::InvalidLifecycle));
        const auto active = lifecycle.Active();
        std::vector<PackageDecisionNodeGeneration> contributions;
        if (!active)
            return Contributions::Success(std::move(contributions));
        try {
            for (const auto &report : active->modules) {
                if (!IsPublishedRole(*active, report, Identity()))
                    continue;
                const auto state = FindRole(impl_->roles, impl_->owner, report.package, report.module, report.generation);
                if (!state || !OwnsPublishedActivation(*state, *active))
                    return Contributions::Failure(MakeError(PackageLifecycleErrors::InvalidLifecycle));
                auto nodes = state->selectedModule->AcquireDecisionNodes();
                if (nodes.HasError())
                    return Contributions::Failure(nodes.ErrorValue());
                if (!nodes.Value()->Contracts().empty()) {
                    const auto &locked = state->verified.Package().lock;
                    contributions.emplace_back(report.package, locked.version, locked.artifactDigest, report.generation,
                                               std::move(nodes).Value());
                }
            }
            return Contributions::Success(std::move(contributions));
        } catch (const std::bad_alloc &) {
            return Contributions::Failure(MakeError(PackageLifecycleErrors::StorageFailed));
        }
    }

    namespace {
        /** @brief Detects registration classes not yet covered by the saved-reference checks. */
        Result<bool> HasAdditionalReferenceClasses(const Gameplay::LoadedGameModule &module) {
            if (!module.Components().Descriptors().empty() || !module.AssetTypes().Registrations().empty())
                return Result<bool>::Success(true);
            const auto queries = module.AcquireGameplayQueryTests();
            if (queries.HasError())
                return Result<bool>::Failure(queries.ErrorValue());
            const auto persistence = module.PersistenceDeclarations();
            if (persistence.HasError())
                return Result<bool>::Failure(persistence.ErrorValue());
            return Result<bool>::Success(bool{queries.Value().schema} || !persistence.Value().empty());
        }
    }  // namespace

    /** @copydoc GameplayPackageModuleHost::CaptureActiveRegistrations */
    Result<std::vector<GameplayPackageModuleHost::RegistrationInventory>> GameplayPackageModuleHost::CaptureActiveRegistrations(
        const PackageLifecycleService &lifecycle) const {
        using Inventory = Result<std::vector<RegistrationInventory>>;
        if (std::this_thread::get_id() != impl_->owner)
            return Inventory::Failure(MakeError(PackageLifecycleErrors::InvalidLifecycle));
        const auto active = lifecycle.Active();
        if (!active)
            return Inventory::Success({});
        try {
            std::vector<RegistrationInventory> registrations;
            for (const auto &report : active->modules) {
                if (!IsPublishedRole(*active, report, Identity()))
                    continue;
                const auto state = FindRole(impl_->roles, impl_->owner, report.package, report.module, report.generation);
                if (!state || !OwnsPublishedActivation(*state, *active))
                    return Inventory::Failure(MakeError(PackageLifecycleErrors::InvalidLifecycle));
                const auto &loaded = *state->selectedModule;
                const auto &locked = state->verified.Package().lock;
                auto &inventory =
                    registrations.emplace_back(locked.package, locked.version, locked.artifactDigest, report.generation, report.module);
                inventory.descriptorPath = state->verified.Descriptor().Value();
                for (const auto &behavior : loaded.Registry().Registrations())
                    inventory.behaviors.push_back(behavior.descriptor.typeId);
                for (const auto &service : loaded.Services().Registrations())
                    inventory.services.push_back(service.descriptor);
                for (const auto &system : loaded.Systems().Registrations())
                    inventory.systems.push_back(system.descriptor);
                auto decisionNodes = loaded.AcquireDecisionNodes();
                if (decisionNodes.HasError())
                    return Inventory::Failure(decisionNodes.ErrorValue());
                for (const auto &node : decisionNodes.Value()->Contracts())
                    inventory.decisionNodes.push_back(node.descriptor.type);
                const auto senses = loaded.AcquirePerceptionSenses();
                if (senses.HasError())
                    return Inventory::Failure(senses.ErrorValue());
                inventory.hasCustomSenses = !senses.Value()->Contracts().empty();
                const auto additional = HasAdditionalReferenceClasses(loaded);
                if (additional.HasError())
                    return Inventory::Failure(additional.ErrorValue());
                inventory.hasAdditionalReferenceClasses = additional.Value();
            }
            return Inventory::Success(std::move(registrations));
        } catch (const std::bad_alloc &) {
            return Inventory::Failure(MakeError(PackageLifecycleErrors::StorageFailed));
        }
    }

    /** @copydoc GameplayPackageModuleHost::AcquirePerceptionSenses */
    Result<PackagePerceptionSenseGeneration> GameplayPackageModuleHost::AcquirePerceptionSenses(const HoroPackageId &package,
                                                                                                const std::string_view module,
                                                                                                const std::uint64_t generation) const {
        auto state = FindRole(impl_->roles, impl_->owner, package, module, generation);
        if (!state)
            return Result<PackagePerceptionSenseGeneration>::Failure(MakeError(PackageLifecycleErrors::InvalidLifecycle));
        auto senses = state->selectedModule->AcquirePerceptionSenses();
        if (senses.HasError())
            return Result<PackagePerceptionSenseGeneration>::Failure(senses.ErrorValue());
        const auto &locked = state->verified.Package().lock;
        return Result<PackagePerceptionSenseGeneration>::Success(
            {package, locked.version, locked.artifactDigest, generation, std::move(senses).Value()});
    }

    Result<std::unique_ptr<Gameplay::LuaDecisionNodeProgram>> GameplayPackageModuleHost::CompileDecisionNode(
        const HoroPackageId &package, std::string_view moduleId, std::uint64_t generation, const PackagePath &source,
        AI::DecisionNodeTypeId type, AI::DecisionProviderId provider, std::uint32_t descriptorVersion,
        Gameplay::LuaBehaviorLimits limits) const {
        using Output = Result<std::unique_ptr<Gameplay::LuaDecisionNodeProgram>>;
        auto state = FindRole(impl_->roles, impl_->owner, package, moduleId, generation);
        if (!state)
            return Output::Failure(MakeError(PackageLifecycleErrors::InvalidLifecycle));
        if (!source.Value().ends_with(".horo_script"))
            return Output::Failure(MakeError(PackageLifecycleErrors::InvalidCandidate));
        auto parent = state->selectedModule->AcquireDecisionNodes();
        if (parent.HasError())
            return Output::Failure(parent.ErrorValue());
        auto bytes = state->verified.ReadDeclaredFile(source, 256U * 1024U);
        if (bytes.HasError())
            return Output::Failure(bytes.ErrorValue());
        const auto &owned = bytes.Value();
        std::string code(reinterpret_cast<const char *>(owned.data()), owned.size());
        if (!LiveRole(*state))
            return Output::Failure(MakeError(PackageLifecycleErrors::InvalidLifecycle));
        return Gameplay::LuaDecisionNodeProgram::Compile(std::move(code), type, provider, descriptorVersion, {source.Value(), 0, 0}, limits,
                                                         std::move(parent).Value());
    }

    /** @copydoc GameplayPackageModuleHost::CompilePerceptionSense */
    Result<std::unique_ptr<Gameplay::LuaPerceptionSenseProgram>> GameplayPackageModuleHost::CompilePerceptionSense(
        const HoroPackageId &package, std::string_view module, const std::uint64_t generation, const PackagePath &source,
        AI::PerceptionSenseContract contract, const Gameplay::LuaBehaviorLimits limits) const {
        using Output = Result<std::unique_ptr<Gameplay::LuaPerceptionSenseProgram>>;
        auto state = FindRole(impl_->roles, impl_->owner, package, module, generation);
        if (!state)
            return Output::Failure(MakeError(PackageLifecycleErrors::InvalidLifecycle));
        if (!source.Value().ends_with(".horo_script"))
            return Output::Failure(MakeError(PackageLifecycleErrors::InvalidCandidate));
        auto parent = state->selectedModule->AcquirePerceptionSenses();
        if (parent.HasError())
            return Output::Failure(parent.ErrorValue());
        auto bytes = state->verified.ReadDeclaredFile(source, 256U * 1024U);
        if (bytes.HasError())
            return Output::Failure(bytes.ErrorValue());
        const auto &owned = bytes.Value();
        std::string code(reinterpret_cast<const char *>(owned.data()), owned.size());
        if (!LiveRole(*state))
            return Output::Failure(MakeError(PackageLifecycleErrors::InvalidLifecycle));
        contract.source = {source.Value(), 1, 1};
        return Gameplay::LuaPerceptionSenseProgram::Compile(code, contract, std::move(parent).Value(), limits);
    }
}  // namespace Horo::Packages
