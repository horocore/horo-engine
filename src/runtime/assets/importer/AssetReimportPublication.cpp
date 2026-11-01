#include "AssetReimportPublication.h"

#include "../AssetErrors.h"

#include <algorithm>
#include <new>

namespace Horo::Assets::Detail {
    namespace {
        using Void = Result<void>;

        /** @brief Every publication uses the original admitted generation and the same canonical source primitive. */
        Void PublishEntry(ReimportAdmission &admission, ReimportEntry &entry, std::span<const std::byte> bytes,
                          const CancellationToken &cancellation) {
            auto published = PublishReimportAssetSource(admission.root, entry.target, entry.original, bytes, *entry.lease, *admission.files,
                                                        cancellation);
            if (published.HasError())
                return Void::Failure(std::move(published).ErrorValue());
            auto receipt = std::move(published).Value();
            entry.published = true;
            entry.promoted = receipt.identity;
            if (receipt.status != AssetSourcePublicationStatus::Durable) {
                admission.uncertain = true;
                return Void::Failure(std::move(*receipt.durabilityError));
            }
            return Void::Success();
        }

        /** @brief Detect source/metadata replacement after either promotion before exposing new registry state. */
        Void VerifyPromoted(const ReimportAdmission &admission, const CancellationToken &cancellation) {
            for (const auto &entry : admission.entries) {
                auto current = InspectReimportAssetSource(admission.root, entry.target, cancellation);
                if (current.HasError())
                    return Void::Failure(std::move(current).ErrorValue());
                if (current.Value() != entry.promoted)
                    return Void::Failure(MakeError(AssetSourceErrors::Conflict));
            }
            return Void::Success();
        }

        /** @brief A rollback copy has no authority unless its content still equals the originally admitted bytes. */
        Result<AssetSourceCapture> OriginalCopy(const ReimportAdmission &admission, const ReimportEntry &entry) {
            auto backup = CaptureReimportAssetSource(admission.root, entry.backup);
            if (backup.HasError())
                return backup;
            if (backup.Value().identity != entry.original)
                return Result<AssetSourceCapture>::Failure(MakeError(AssetSourceErrors::Conflict));
            return backup;
        }

        /** @brief Restore only a source still matching our promoted identity; cancellation never suppresses compensation. */
        Void RestoreEntry(ReimportAdmission &admission, ReimportEntry &entry) {
            auto current = InspectReimportAssetSource(admission.root, entry.target);
            if (current.HasError())
                return Void::Failure(std::move(current).ErrorValue());
            if (current.Value() != entry.promoted)
                return Void::Failure(MakeError(AssetSourceErrors::Conflict));
            auto original = OriginalCopy(admission, entry);
            if (original.HasError())
                return Void::Failure(std::move(original).ErrorValue());
            auto restored = PublishReimportAssetSource(admission.root, entry.target, entry.promoted, original.Value().bytes, *entry.lease,
                                                       *admission.files);
            if (restored.HasError())
                return Void::Failure(std::move(restored).ErrorValue());
            auto receipt = std::move(restored).Value();
            entry.published = false;
            if (receipt.status != AssetSourcePublicationStatus::Durable) {
                admission.uncertain = true;
                return Void::Failure(std::move(*receipt.durabilityError));
            }
            return Void::Success();
        }

        /** @brief Neither late backups nor a derived-index error may overwrite another writer's current bytes. */
        void Compensate(ReimportAdmission &admission) noexcept {
            for (auto entry = admission.entries.rbegin(); entry != admission.entries.rend(); ++entry) {
                if (!entry->published)
                    continue;
                try {
                    if (RestoreEntry(admission, *entry).HasError())
                        admission.uncertain = true;
                } catch (...) {
                    admission.uncertain = true;
                }
            }
        }

        /** @brief Preserve the original typed cause; irreversible/failed compensation is explicitly reconciliation-required. */
        Void Reject(ReimportAdmission &admission, PreparedAssetImport &prepared, Error error) {
            // Old payloads are read only for compensation. Release the no-longer-needed replacement buffer first.
            std::vector<std::uint8_t>{}.swap(prepared.editorPayload);
            if (!admission.committed)
                Compensate(admission);
            if (admission.committed || admission.uncertain)
                return Void::Failure(WrapError(AssetSourceErrors::DurabilityUnconfirmed, std::move(error)));
            return Void::Failure(std::move(error));
        }

        /** @brief Require the committed pair's actual identity/type membership in a prepared source registry candidate. */
        Void TargetMembership(const ReimportAdmission &admission, const AssetRegistryCandidate &candidate) {
            const auto target = std::ranges::find(candidate.records, admission.metadata.assetId, &AssetRecord::id);
            if (target == candidate.records.end() || target->type != admission.metadata.assetType)
                return Void::Failure(MakeError(AssetErrors::IndexMalformed));
            const auto relative = admission.entries[0].target.lexically_relative(admission.root).generic_u8string();
            if (target->sourcePath.String() != std::string(relative.begin(), relative.end()))
                return Void::Failure(MakeError(AssetErrors::IndexMalformed));
            return Void::Success();
        }

        /** @brief Registry replacement is the commit barrier; index persistence after it cannot roll back visible sources. */
        Void CommitRegistry(ReimportAdmission &admission, AssetRegistry &registry) {
            auto candidate = PrepareAssetRegistryCandidate(admission.root);
            if (candidate.HasError())
                return Void::Failure(std::move(candidate).ErrorValue());
            if (candidate.Value().status == AssetRegistryBuildStatus::Failed)
                return Void::Failure(MakeError(AssetErrors::IndexMalformed));
            if (auto member = TargetMembership(admission, candidate.Value()); member.HasError())
                return member;
            if (registry.Publish(std::move(candidate).Value()).status == AssetRegistryBuildStatus::Failed)
                return Void::Failure(MakeError(AssetErrors::IndexMalformed));
            admission.committed = true;
            auto index =
                AssetIndexStore::SaveAtomically(admission.root / std::filesystem::path{ProjectLayout::AssetIndexPath}, registry.Snapshot());
            if (index.HasError())
                return Void::Failure(std::move(index).ErrorValue());
            return Void::Success();
        }

        Void PublishPair(ReimportAdmission &admission, PreparedAssetImport &prepared, std::string_view metadata, AssetRegistry &registry,
                         const CancellationToken &cancellation) {
            if (auto payload =
                    PublishEntry(admission, admission.entries[0], std::as_bytes(std::span(prepared.editorPayload)), cancellation);
                payload.HasError())
                return Reject(admission, prepared, std::move(payload).ErrorValue());
            if (auto sidecar = PublishEntry(admission, admission.entries[1], std::as_bytes(std::span(metadata)), cancellation);
                sidecar.HasError())
                return Reject(admission, prepared, std::move(sidecar).ErrorValue());
            if (auto current = VerifyPromoted(admission, cancellation); current.HasError())
                return Reject(admission, prepared, std::move(current).ErrorValue());
            if (auto committed = CommitRegistry(admission, registry); committed.HasError())
                return Reject(admission, prepared, std::move(committed).ErrorValue());
            return Void::Success();
        }
    }  // namespace

    /** @copydoc PublishReimport */
    Result<void> PublishReimport(ReimportAdmission &admission, PreparedAssetImport prepared, std::string_view serializedMetadata,
                                 AssetRegistry &registry, const CancellationToken &cancellation) {
        auto capacity = MakeError(AssetSourceErrors::Limit);
        auto provider = MakeError(AssetSourceErrors::ReadFailed);
        auto committed = MakeError(AssetSourceErrors::DurabilityUnconfirmed);
        try {
            return PublishPair(admission, prepared, serializedMetadata, registry, cancellation);
        } catch (const std::bad_alloc &) {
            std::vector<std::uint8_t>{}.swap(prepared.editorPayload);
            if (!admission.committed)
                Compensate(admission);
            return Void::Failure(admission.committed || admission.uncertain ? std::move(committed) : std::move(capacity));
        } catch (...) {
            std::vector<std::uint8_t>{}.swap(prepared.editorPayload);
            if (!admission.committed)
                Compensate(admission);
            return Void::Failure(admission.committed || admission.uncertain ? std::move(committed) : std::move(provider));
        }
    }
}  // namespace Horo::Assets::Detail
