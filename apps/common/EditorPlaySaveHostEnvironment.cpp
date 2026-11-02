#include "EditorPlaySaveHost.h"
#include "Horo/Application/ProjectCompatibility.h"

namespace Horo::Application::Internal {
    namespace {
        using namespace Runtime;

        /** @brief Issues real opaque guest/session/object identities, never hashes runtime slots or paths into authority. */
        template <class Identity> Result<Identity> IssueIdentity(Security::SecureRandomSource &entropy) {
            SaveIdentityDetail::Bytes bytes{};
            if (const auto filled = entropy.Fill(std::as_writable_bytes(std::span{bytes})); filled.HasError())
                return Result<Identity>::Failure(filled.ErrorValue());
            bytes[6] = static_cast<std::uint8_t>((bytes[6] & 15U) | 64U);
            bytes[8] = static_cast<std::uint8_t>((bytes[8] & 63U) | 128U);
            return Identity::FromBytes(bytes);
        }

        /** @brief Issues the entire authored durable inventory once, preserving it through later runtime deletion/tombstones. */
        Result<std::vector<PersistentEntityRecord>> IssueEntities(const RuntimeSceneDefinition &definition,
                                                                  Security::SecureRandomSource &entropy) {
            using Outcome = Result<std::vector<PersistentEntityRecord>>;
            if (definition.Entities().size() > 16'384)
                return Outcome::Failure(MakeError(SaveErrors::CaptureBudgetExceeded));
            std::vector<PersistentEntityRecord> identities;
            identities.reserve(definition.Entities().size());
            for (const auto &entity : definition.Entities()) {
                const auto issued = IssueIdentity<PersistentEntityId>(entropy);
                if (issued.HasError())
                    return Outcome::Failure(issued.ErrorValue());
                identities.push_back({issued.Value(),
                                      {1},
                                      AuthoredPersistentEntityProvenance{definition.Id(), entity.object},
                                      PersistentEntityDisposition::Live});
            }
            return Outcome::Success(std::move(identities));
        }

        /** @brief Builds only the unique activation's guest namespace and logical world scope under a dedicated Editor product root. */
        Result<PlaySaveStorageConfig> Scope(const ProductSaveRoot &root, std::shared_ptr<Security::SecureRandomSource> entropy,
                                            SaveProjectId project) {
            using Outcome = Result<PlaySaveStorageConfig>;
            if (!project.IsValid())
                return Outcome::Failure(MakeError(SaveErrors::IdentityInvalid));
            const auto environment = IssueIdentity<EnvironmentStorageId>(*entropy);
            const auto user = IssueIdentity<LocalUserStorageId>(*entropy);
            const auto profile = IssueIdentity<GameProfileId>(*entropy);
            const auto world = IssueIdentity<SaveWorldId>(*entropy);
            const auto baseline = IssueIdentity<SaveBaseSceneId>(*entropy);
            if (environment.HasError())
                return Outcome::Failure(environment.ErrorValue());
            if (user.HasError())
                return Outcome::Failure(user.ErrorValue());
            if (profile.HasError())
                return Outcome::Failure(profile.ErrorValue());
            if (world.HasError())
                return Outcome::Failure(world.ErrorValue());
            if (baseline.HasError())
                return Outcome::Failure(baseline.ErrorValue());
            return Outcome::Success({root,
                                     {root.Product(), environment.Value(), UserProfileOwner{user.Value(), profile.Value()}},
                                     project,
                                     world.Value(),
                                     baseline.Value(),
                                     std::move(entropy),
                                     FormatHoroVersion(CurrentEngineReleaseVersion().value),
                                     "editor-play-session"});
        }
    }  // namespace

    /** @copydoc MakeEditorPlaySaveFactory */
    Result<EditorPlaySaveFactory> MakeEditorPlaySaveFactory(JobSystem &jobs, OperationStore &operations, Clock &clock, WallClock &wallClock,
                                                            const ProcessService &processes, Runtime::SaveRootPlatform platform,
                                                            std::shared_ptr<Security::SecureRandomSource> entropy) {
        using Outcome = Result<EditorPlaySaveFactory>;
        // Explicit Editor PIE product policy; deliberately disjoint from every game's
        // production ProductStorageId.
        const auto product = ProductStorageId::Parse("25302693-3fac-4b5c-b421-8be71f1f5738");
        if (product.HasError() || !entropy)
            return Outcome::Failure(MakeError(SaveErrors::CompositionUnsupported));
        auto root = ResolveProductSaveRoot({product.Value(), platform, {}}, processes);
        if (root.HasError())
            return Outcome::Failure(root.ErrorValue());
        return Outcome::Success([&jobs, &operations, &clock, &wallClock, root = std::move(root).Value(),
                                 entropy = std::move(entropy)](SaveProjectId project, RuntimeSceneService &scenes,
                                                               const RuntimeSceneDefinition &definition,
                                                               std::vector<Editor::PlaySaveGameplayBinding> bindings,
                                                               std::shared_ptr<Gameplay::BehaviorRuntimeSceneOwner> behaviorOwner)
                                    -> Result<std::unique_ptr<EditorPlaySaveHost>> {
            using Host = Result<std::unique_ptr<EditorPlaySaveHost>>;
            auto scope = Scope(root, entropy, project);
            if (scope.HasError())
                return Host::Failure(scope.ErrorValue());
            auto identities = IssueEntities(definition, *entropy);
            if (identities.HasError())
                return Host::Failure(identities.ErrorValue());
            std::vector<Gameplay::GameplayPersistenceDeclaration> declarations;
            declarations.reserve(bindings.size());
            for (const auto &binding : bindings)
                declarations.push_back(binding.declaration);
            auto schema = std::make_shared<PlaySaveStorageSchema>(
                PlaySaveStorageSchema{definition, std::move(identities).Value(), std::move(declarations),
                                      behaviorOwner ? std::optional{Gameplay::BehaviorLifecycleParticipant()} : std::nullopt});
            auto live = std::make_shared<const std::vector<Editor::PlaySaveGameplayBinding>>(std::move(bindings));
            return EditorPlaySaveHost::Create(scenes, {jobs, operations, clock, wallClock, std::move(scope).Value()}, std::move(schema),
                                              std::move(live), std::move(behaviorOwner));
        });
    }
}  // namespace Horo::Application::Internal
