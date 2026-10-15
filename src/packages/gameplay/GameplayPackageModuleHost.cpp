#include "Horo/Packages/GameplayPackageModuleHost.h"

#include "Horo/Packages/PackageLifecycleErrors.h"
#include "PackageContentStaging.h"

#include <algorithm>
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
        if (std::this_thread::get_id() != impl_->owner)
            return {};
        for (const auto &weak : impl_->roles)
            if (auto state = weak.lock(); state && !state->retired && !state->failure && state->verified.Identity().package == package &&
                                          state->verified.Module().id == selectedModule &&
                                          state->verified.Identity().generation == generation && state->selectedModule)
                return {state, state->selectedModule.get()};
        return {};
    }
}  // namespace Horo::Packages
