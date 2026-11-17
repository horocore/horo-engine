#include "EditorPlaySavePrefabInventoryState.h"
#include "Horo/Prefab/PrefabCookedOverrides.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Prefab/PrefabPersistentCodec.h"
#include "Horo/Prefab/PreparedPrefabSpawn.h"

#include <algorithm>
#include <new>
#include <set>
#include <thread>

namespace Horo::Application::Internal {
    namespace {
        using namespace Runtime;
        using namespace Prefab;

        /** @brief Keeps unsupported/stale durable creation typed and actionable at the owning prefab boundary. */
        template <typename T> Result<T> Invalid(std::string message) {
            return Result<T>::Failure(MakeError(PrefabErrors::AdmissionRejected, std::move(message)));
        }
    }  // namespace

    /** @copydoc PlaySavePrefabInventory::PlaySavePrefabInventory */
    PlaySavePrefabInventory::PlaySavePrefabInventory(const ConstructionKey &, std::shared_ptr<State> state) noexcept
        : state_(std::move(state)) {}

    namespace {
        /** @brief Trusted bounds are checked before any inventory allocation or subtraction. */
        bool ValidInventoryBounds(const PrefabPersistenceLimits &limits, std::size_t authored) noexcept {
            return IsValidPrefabPersistenceLimits(limits) && limits.maximumMembers <= 16'384 && authored <= limits.maximumMembers &&
                   limits.maximumInstances <= limits.maximumMembers;
        }
    }  // namespace

    /** @copydoc PlaySavePrefabInventory::Create */
    Result<std::shared_ptr<PlaySavePrefabInventory>> PlaySavePrefabInventory::Create(
        RuntimeSceneService &scenes, SaveWorldId world, std::vector<PersistentEntityRecord> authored,
        std::shared_ptr<Security::SecureRandomSource> entropy, PrefabPersistenceLimits limits, PrefabLimitProfile profile,
        std::shared_ptr<const PrefabValueSchemaSnapshot> schemas) {
        using Outcome = Result<std::shared_ptr<PlaySavePrefabInventory>>;
        const auto scene = scenes.ActiveScene();
        if (!scene || !world.IsValid() || !entropy || !ValidInventoryBounds(limits, authored.size()))
            return Invalid<std::shared_ptr<PlaySavePrefabInventory>>(
                "The actual Play world or aggregate prefab identity bounds are invalid.");
        try {
            auto state = std::make_shared<State>(scenes, world, scene->RuntimeId(), std::move(authored), std::move(entropy), limits,
                                                 std::move(profile), std::move(schemas));
            if (auto map = state->Map(*scene); map.HasError())
                return Outcome::Failure(map.ErrorValue());
            return Outcome::Success(std::make_shared<PlaySavePrefabInventory>(ConstructionKey{}, std::move(state)));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }

    /** @copydoc PlaySavePrefabInventory::PrepareSpawn */
    Result<std::shared_ptr<IPrefabPersistencePublication>> PlaySavePrefabInventory::PrepareSpawn(
        const PreparedPrefabSpawn &prepared, const PrefabSpawnRequest &request, std::span<const RuntimeComponentSet> projected,
        std::span<const Assets::AssetId> lineage, std::string_view creator) {
        try {
            return state_->Prepare(prepared, request, projected, lineage, creator);
        } catch (const std::bad_alloc &) {
            return Result<std::shared_ptr<IPrefabPersistencePublication>>::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }

    /** @copydoc PlaySavePrefabInventory::Lookup */
    Result<PrefabPersistentLookup> PlaySavePrefabInventory::Lookup(SavePrefabInstanceId instance, std::string_view creator) const {
        try {
            return state_->Lookup(instance, creator);
        } catch (const std::bad_alloc &) {
            return Result<PrefabPersistentLookup>::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }

    /** @copydoc PlaySavePrefabInventory::PrepareForScene */
    Result<std::shared_ptr<PlaySavePrefabInventory>> PlaySavePrefabInventory::PrepareForScene(
        RuntimeSceneService &scenes, const RuntimeScene &candidate, PlaySavePrefabRestoreInput input,
        std::shared_ptr<Security::SecureRandomSource> entropy, PrefabPersistenceLimits limits, PrefabLimitProfile profile,
        std::shared_ptr<const PrefabValueSchemaSnapshot> schemas) {
        using Outcome = Result<std::shared_ptr<PlaySavePrefabInventory>>;
        try {
            if (!entropy || input.authored.size() > limits.maximumMembers || !candidate.View().IsCurrent() ||
                !scenes.IsRestoreCandidate(candidate))
                return Invalid<std::shared_ptr<PlaySavePrefabInventory>>("Invalid private Scene inventory owner.");
            if (auto valid = EncodePrefabPersistentState(input.saved, limits, profile); valid.HasError())
                return Outcome::Failure(valid.ErrorValue());
            auto state = std::make_shared<State>(scenes, input.saved.world, candidate.View().RuntimeId(), std::move(input.authored),
                                                 std::move(entropy), limits, std::move(profile), std::move(schemas));
            if (auto adopted = state->Adopt(input.saved, input.restored, candidate.View()); adopted.HasError())
                return Outcome::Failure(adopted.ErrorValue());
            return Outcome::Success(std::make_shared<PlaySavePrefabInventory>(ConstructionKey{}, std::move(state)));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }

    /** @copydoc PlaySavePrefabInventory::Capture */
    Result<PlaySavePrefabCapture> PlaySavePrefabInventory::Capture() const {
        try {
            return state_->Capture();
        } catch (const std::bad_alloc &) {
            return Result<PlaySavePrefabCapture>::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }

    /** @copydoc PlaySavePrefabInventory::Identities */
    Result<PersistentEntityIdentityMap> PlaySavePrefabInventory::Identities() const {
        try {
            auto view = state_->Current();
            if (view.HasError())
                return Result<PersistentEntityIdentityMap>::Failure(view.ErrorValue());
            return state_->Map(view.Value());
        } catch (const std::bad_alloc &) {
            return Result<PersistentEntityIdentityMap>::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }

    /** @copydoc PlaySavePrefabInventory::Close */
    void PlaySavePrefabInventory::Close() const noexcept {
        state_->closed = true;
    }
}  // namespace Horo::Application::Internal
