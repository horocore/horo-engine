#include "VfxWorldSpawnInternal.h"

namespace Horo::Vfx {
    namespace {
        [[nodiscard]] bool OwnedBy(const Detail::VfxSpawnInstance &instance, const VfxIdentityScope owner,
                                   const std::uint64_t generation) noexcept {
            return instance.live && instance.request.owner == owner && instance.request.ownerGeneration == generation;
        }
    }  // namespace

    /** @copydoc VfxWorld::VfxWorld */
    VfxWorld::VfxWorld(VfxWorld &&) noexcept = default;
    /** @copydoc VfxWorld::operator= */
    VfxWorld &VfxWorld::operator=(VfxWorld &&) noexcept = default;
    /** @copydoc VfxWorld::~VfxWorld */
    VfxWorld::~VfxWorld() = default;

    /** @copydoc VfxWorld::PrepareSpawns */
    Result<std::unique_ptr<VfxWorld>> VfxWorld::PrepareSpawns(const VfxIdentityScope scene,
                                                              const std::span<const VfxSpawnAssetPreparation> assets,
                                                              const VfxSpawnBudget budget, const VfxExtractionBudget extraction) {
        using Output = Result<std::unique_ptr<VfxWorld>>;
        if (!scene.IsValid())
            return Output::Failure(MakeError(VfxErrors::IdentityInvalid));
        auto extractor = VfxRenderExtractor::Prepare(extraction);
        if (extractor.HasError())
            return Output::Failure(extractor.ErrorValue());
        try {
            auto world = std::make_unique<VfxWorld>(VfxWorld{scene, std::move(extractor).Value()});
            if (auto prepared = world->PrepareSpawnPalette(assets, budget); prepared.HasError())
                return Output::Failure(prepared.ErrorValue());
            return Output::Success(std::move(world));
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(VfxErrors::SpawnPreparationAllocationFailed));
        }
    }

    VfxSpawnStatus VfxWorld::SpawnAdmissionStatus() const noexcept {
        using enum Horo::Vfx::VfxSpawnStatus;
        if (owner_ != std::this_thread::get_id())
            return ThreadViolation;
        if (shutDown_ || !spawn_)
            return ShutDown;
        return spawn_->busy ? Reentrant : Accepted;
    }

    /** @copydoc VfxWorld::PrepareSpawnPalette */
    Result<void> VfxWorld::PrepareSpawnPalette(const std::span<const VfxSpawnAssetPreparation> assets, const VfxSpawnBudget budget) {
        if (owner_ != std::this_thread::get_id() || shutDown_ || spawn_ || !emitters_.empty() || !gpuEmitters_.empty())
            return Result<void>::Failure(MakeError(VfxErrors::SpawnPreparationInvalid));
        if (budget.maximumInstances > extractor_.budget_.maximumBatches || budget.maximumParticles > extractor_.budget_.maximumInstances ||
            budget.maximumInstances > extractor_.budget_.maximumDrawCalls)
            return Result<void>::Failure(MakeError(VfxErrors::SpawnPreparationBudgetExceeded));
        auto prepared = SpawnDetail::Prepare(scene_, assets, budget);
        if (prepared.HasError())
            return Result<void>::Failure(prepared.ErrorValue());
        spawn_ = std::move(prepared).Value();
        return Result<void>::Success();
    }

    /** @copydoc VfxWorld::AdmitSpawnOwner */
    VfxSpawnStatus VfxWorld::AdmitSpawnOwner(const VfxIdentityScope owner, const std::uint64_t generation) noexcept {
        using enum Horo::Vfx::VfxSpawnStatus;
        if (const auto status = SpawnAdmissionStatus(); status != Accepted)
            return status;
        if (!owner.IsValid() || generation == 0)
            return InvalidRequest;
        for (auto &entry : spawn_->owners) {
            if (entry.owner != owner)
                continue;
            if (entry.generation >= generation)
                return StaleOwner;
            if (entry.active)
                return PermissionDenied;
            entry = {owner, generation, true};
            return Accepted;
        }
        const auto free = std::ranges::find_if(spawn_->owners, [](const auto &entry) {
            return !entry.owner.IsValid();
        });
        if (free == spawn_->owners.end())
            return ResourceBudgetExceeded;
        *free = {owner, generation, true};
        return Accepted;
    }

    /** @copydoc VfxWorld::TryEnqueueSpawns */
    VfxSpawnOutcome VfxWorld::TryEnqueueSpawns(const std::span<const VfxSpawnRequest> requests) noexcept {
        const auto status = SpawnAdmissionStatus();
        if (status != VfxSpawnStatus::Accepted)
            return requests.empty() ? VfxSpawnOutcome{.status = status} : MakeVfxSpawnOutcome(requests.front(), status);
        return SpawnDetail::Enqueue(*spawn_, scene_, requests);
    }

    /** @copydoc VfxWorld::UpdateSpawns */
    VfxSpawnUpdate VfxWorld::UpdateSpawns(const std::uint64_t tick, const float deltaSeconds) noexcept {
        const auto status = SpawnAdmissionStatus();
        if (status != VfxSpawnStatus::Accepted)
            return {.status = status};
        return SpawnDetail::Update(*spawn_, tick, deltaSeconds);
    }

    /** @copydoc VfxWorld::CancelSpawnOwner */
    VfxSpawnStatus VfxWorld::CancelSpawnOwner(const VfxIdentityScope owner, const std::uint64_t generation) noexcept {
        using enum Horo::Vfx::VfxSpawnStatus;
        if (const auto status = SpawnAdmissionStatus(); status != Accepted)
            return status;
        if (!owner.IsValid() || generation == 0)
            return InvalidRequest;
        const auto found = std::ranges::find(spawn_->owners, owner, &Detail::VfxSpawnOwner::owner);
        if (found == spawn_->owners.end() || found->generation != generation)
            return StaleOwner;
        found->active = false;
        for (auto &asset : spawn_->assets)
            for (auto &instance : asset.instances)
                if (OwnedBy(instance, owner, generation))
                    SpawnDetail::Retire(asset, instance);
        return Accepted;
    }

    /** @copydoc VfxWorld::SpawnResults */
    std::span<const VfxSpawnOutcome> VfxWorld::SpawnResults() const noexcept {
        if (SpawnAdmissionStatus() != VfxSpawnStatus::Accepted)
            return {};
        return {spawn_->results.data(), spawn_->resultCount};
    }

    /** @copydoc VfxWorld::ClearSpawnResults */
    void VfxWorld::ClearSpawnResults() noexcept {
        if (SpawnAdmissionStatus() == VfxSpawnStatus::Accepted)
            spawn_->resultCount = 0;
    }

    /** @copydoc VfxWorld::InspectSpawnAsset */
    VfxSpawnStatus VfxWorld::InspectSpawnAsset(const Assets::AssetId asset, VfxSpawnAssetInfo &output) const noexcept {
        using enum Horo::Vfx::VfxSpawnStatus;
        if (const auto status = SpawnAdmissionStatus(); status != Accepted)
            return status;
        const auto found = SpawnDetail::FindAsset(*spawn_, asset);
        if (!found)
            return MissingAsset;
        output = found->info;
        return Accepted;
    }

    void VfxWorld::ShutdownSpawns() noexcept {
        if (!spawn_)
            return;
        for (auto &asset : spawn_->assets) {
            static_cast<void>(asset.pool.Shutdown());
            for (auto &instance : asset.instances) {
                instance.live = false;
                static_cast<void>(instance.simulation.Shutdown());
            }
        }
    }
}  // namespace Horo::Vfx
