#include "Horo/Release/UpdateFileDelta.h"

#include "Horo/Release/UpdateTransferErrors.h"

#include <algorithm>
#include <map>

namespace Horo::Release {
    namespace {
        /** @brief Hashes the exact canonical full-file inventory used by a signed release descriptor. */
        [[nodiscard]] Result<Sha256Digest> InventoryIdentity(const std::span<const UpdateStagedFile> files,
                                                             const UpdateArchiveLimits &limits) {
            auto canonical = BuildCanonicalUpdateFileInventory(files, limits);
            if (canonical.HasError())
                return Result<Sha256Digest>::Failure(canonical.ErrorValue());
            return Result<Sha256Digest>::Success(ComputeSha256(std::as_bytes(std::span{canonical.Value()})));
        }

        /** @brief Checks whether a base file can be reused without copying altered bytes. */
        [[nodiscard]] bool SameFile(const UpdateStagedFile &left, const UpdateStagedFile &right) {
            return left.size == right.size && left.digest == right.digest && left.mode == right.mode && left.role == right.role;
        }
    }  // namespace

    /** @copydoc PlanUpdateFileDelta */
    Result<UpdateFileDeltaPlan> PlanUpdateFileDelta(const std::span<const UpdateStagedFile> baseFiles,
                                                    const std::span<const UpdateStagedFile> targetFiles,
                                                    const std::span<const UpdateStagedFile> deltaFiles,
                                                    const Sha256Digest &expectedBaseDigest, const Sha256Digest &expectedTargetDigest,
                                                    const Sha256Digest &expectedDeltaDigest, const UpdateArchiveLimits &limits) {
        const auto mismatch = [] {
            return Result<UpdateFileDeltaPlan>::Failure(MakeError(UpdateTransferErrors::StageMismatch));
        };
        auto baseIdentity = InventoryIdentity(baseFiles, limits);
        auto targetIdentity = InventoryIdentity(targetFiles, limits);
        auto deltaIdentity = InventoryIdentity(deltaFiles, limits);
        if (baseIdentity.HasError() || targetIdentity.HasError() || deltaIdentity.HasError())
            return Result<UpdateFileDeltaPlan>::Failure(MakeError(UpdateTransferErrors::InvalidArchive));
        if (baseIdentity.Value() != expectedBaseDigest || targetIdentity.Value() != expectedTargetDigest ||
            deltaIdentity.Value() != expectedDeltaDigest || expectedBaseDigest == expectedTargetDigest)
            return mismatch();

        std::map<std::string, const UpdateStagedFile *, std::less<>> base;
        std::map<std::string, const UpdateStagedFile *, std::less<>> delta;
        for (const auto &file : baseFiles)
            base.try_emplace(file.path, &file);
        for (const auto &file : deltaFiles)
            delta.try_emplace(file.path, &file);

        UpdateFileDeltaPlan plan{expectedBaseDigest,
                                 expectedTargetDigest,
                                 expectedDeltaDigest,
                                 {baseFiles.begin(), baseFiles.end()},
                                 {deltaFiles.begin(), deltaFiles.end()},
                                 {}};
        plan.targetFiles.reserve(targetFiles.size());
        for (const auto &target : targetFiles) {
            const auto prior = base.find(target.path);
            const auto patch = delta.find(target.path);
            if (prior != base.end() && SameFile(*prior->second, target) && patch == delta.end()) {
                plan.targetFiles.emplace_back(target, UpdateDeltaFileSource::VerifiedBase);
            } else {
                if (patch == delta.end() || !SameFile(*patch->second, target) ||
                    (prior != base.end() && SameFile(*prior->second, target) && target.role != UpdateFileRole::Entrypoint))
                    return mismatch();
                plan.targetFiles.emplace_back(target, UpdateDeltaFileSource::VerifiedDelta);
                delta.erase(patch);
            }
        }
        if (!delta.empty())
            return mismatch();
        std::ranges::sort(plan.targetFiles, {}, [](const UpdateDeltaFile &file) -> const std::string & {
            return file.target.path;
        });
        return Result<UpdateFileDeltaPlan>::Success(std::move(plan));
    }

    /** @copydoc VerifyUpdateFileDeltaStage */
    Result<void> VerifyUpdateFileDeltaStage(const std::filesystem::path &stageRoot, const UpdateFileDeltaPlan &plan,
                                            const UpdateArchiveLimits &limits) {
        std::vector<UpdateStagedFile> targetFiles;
        targetFiles.reserve(plan.targetFiles.size());
        for (const auto &file : plan.targetFiles)
            targetFiles.push_back(file.target);
        if (auto identity = InventoryIdentity(targetFiles, limits); identity.HasError() || identity.Value() != plan.targetInventoryDigest)
            return Result<void>::Failure(MakeError(UpdateTransferErrors::StageMismatch));
        return VerifyUpdateStagedTree(stageRoot, targetFiles, limits);
    }
}  // namespace Horo::Release
