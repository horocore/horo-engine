#include "Horo/Runtime/Save/SaveArchiveContainerWriter.h"
#include "SaveSlotLifecycleInternal.h"

#include <algorithm>

namespace Horo::Runtime::SaveSlotLifecycleDetail {
    namespace {
        /** @brief Checks the typed archive identity scope without allowing header-selected ownership. */
        [[nodiscard]] bool InScope(const SaveArchiveHeader &header, const SaveSlotArchiveScope &scope) noexcept {
            return header.product == scope.name.product && header.environment == scope.name.environment && header.user == scope.user &&
                   header.profile == scope.profile;
        }

        /** @brief Validates one host-authorized archive scope, including the client-profile tuple. */
        [[nodiscard]] bool ValidScope(const SaveSlotArchiveScope &scope) noexcept {
            if (!scope.name.IsValid() || !scope.user.IsValid() || !scope.profile.IsValid())
                return false;
            const auto *owner = std::get_if<UserProfileOwner>(&scope.name.owner);
            return !owner || (owner->user == scope.user && owner->profile == scope.profile);
        }

        /** @brief Allows signing to change framing/trailer only, preserving every stored record and identity. */
        [[nodiscard]] Result<void> VerifyRepack(const ValidatedSaveArchive &verified, const SaveArchiveHeader &header,
                                                const SaveGameManifest &manifest, const std::span<const PreservedSaveChunk> chunks) {
            if (verified.Header() != header || verified.Manifest() != manifest || verified.Directory().Entries().size() != chunks.size())
                return Result<void>::Failure(MakeError(SaveErrors::StorageResultInvalid));
            // Signing may change only the framing/trailer; preserve every stored record, including known
            // presentation and optional opaque records. Unknown-data verification alone is insufficient.
            for (std::size_t index = 0; index < chunks.size(); ++index) {
                const auto &entry = verified.Directory().Entries()[index];
                const auto selected =
                    verified.Payload().subspan(static_cast<std::size_t>(entry.offset), static_cast<std::size_t>(entry.storedByteLength));
                auto expected = chunks[index].entry;
                expected.offset = entry.offset;
                if (entry != expected || !std::ranges::equal(selected, chunks[index].storedBytes))
                    return Result<void>::Failure(MakeError(SaveErrors::StorageResultInvalid));
            }
            return Result<void>::Success();
        }

    }  // namespace

    /** @copydoc ValidatePolicy */
    Result<void> ValidatePolicy(const SaveSlotLifecyclePolicy &policy) {
        if (constexpr auto count = static_cast<std::uint8_t>(SaveSlotLifecycleKind::Count);
            !ValidScope(policy.destination) || policy.profile > SaveSlotLifecycleProfile::Server ||
            policy.signature > SaveSignaturePolicy::Required || (policy.capabilities >> count) != 0 || policy.maximumSlots == 0 ||
            policy.maximumSlots > 4096 || policy.maximumCatalogBytes < 1024 || policy.maximumCatalogBytes > (64ULL << 20U) ||
            policy.archiveLimits.maximumArchiveBytes == 0 || policy.importSources.size() > 32 ||
            (policy.profile == SaveSlotLifecycleProfile::Server && policy.signature != SaveSignaturePolicy::Required))
            return Result<void>::Failure(MakeError(SaveErrors::StoragePolicyInvalid));
        if (auto retention = ValidateRetentionPolicy(policy.retention); retention.HasError())
            return retention;
        for (std::size_t index = 0; index < policy.importSources.size(); ++index) {
            const auto &scope = policy.importSources[index];
            if (!ValidScope(scope))
                return Result<void>::Failure(MakeError(SaveErrors::StoragePolicyInvalid));
            for (std::size_t previous = 0; previous < index; ++previous) {
                const auto &other = policy.importSources[previous];
                if (scope.name.product == other.name.product && scope.name.environment == other.name.environment &&
                    scope.user == other.user && scope.profile == other.profile)
                    return Result<void>::Failure(MakeError(SaveErrors::StoragePolicyInvalid));
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc Admit */
    Result<ValidatedSaveArchive> Admit(std::vector<std::byte> bytes, const SaveSlotArchiveScope &scope,
                                       const SaveSlotLifecyclePolicy &policy, ISaveSlotLifecycleHost &host) {
        const auto key = EncodeSaveNamespaceKey(scope.name);
        if (key.HasError())
            return Result<ValidatedSaveArchive>::Failure(key.ErrorValue());
        auto admitted = AdmitSignedSaveArchive(std::move(bytes), policy.signature, key.Value().Bytes(), host.Verifier(),
                                               SaveArchiveReader{policy.archiveLimits}, policy.archiveLimits.maximumArchiveBytes);
        if (admitted.HasError())
            return admitted;
        if (!InScope(admitted.Value().Header(), scope))
            return Result<ValidatedSaveArchive>::Failure(MakeError(SaveErrors::StoragePermissionDenied));
        const auto &archive = admitted.Value();
        if (const auto compatibility = EvaluateSaveCompatibility(archive.Preamble().archiveFormatVersion, archive.Header(),
                                                                 archive.Manifest(), policy.compatibility);
            compatibility.disposition != SaveCompatibilityDisposition::DirectRead)
            return Result<ValidatedSaveArchive>::Failure(MakeError(SaveErrors::VersionUnsupportedNewer));
        if (auto unknown = archive.InspectUnknownData(policy.compatibility, policy.archiveLimits.maximumArchiveBytes); unknown.HasError())
            return Result<ValidatedSaveArchive>::Failure(unknown.ErrorValue());
        if (auto semantics = host.ValidateSemantics(archive); semantics.HasError())
            return Result<ValidatedSaveArchive>::Failure(semantics.ErrorValue());
        return admitted;
    }

    /** @copydoc Metadata */
    SaveSlotCatalogEntry Metadata(const ValidatedSaveArchive &archive, SaveSlotDisplayMetadata display) {
        const auto &header = archive.Header();
        return {.publication = {.slot = header.slot,
                                .generation = header.slotGeneration,
                                .kind = SaveSlotKind::Manual,
                                .savedAtUnixMilliseconds = header.capturedAtUnixMilliseconds,
                                .playTimeNanoseconds = header.playTimeNanoseconds,
                                .baseScene = header.baseScene,
                                .productCompatibility = header.productCompatibility,
                                .saveSchema = archive.Manifest().saveSchemaVersion,
                                .projectBuildId = header.projectBuildId,
                                .canonicalState = archive.Manifest().canonicalState,
                                .archiveContent = archive.Integrity().archiveContent,
                                .cloudState = SaveSlotCloudState::LocalOnly},
                .display = std::move(display)};
    }

    /** @copydoc Matches */
    Result<void> Matches(const SaveSlotCatalogEntry &entry, const ValidatedSaveArchive &archive) {
        auto expected = Metadata(archive, entry.display);
        expected.publication.kind = entry.publication.kind;
        expected.publication.checkpoint = entry.publication.checkpoint;
        expected.publication.thumbnail = entry.publication.thumbnail;
        expected.publication.cloudState = entry.publication.cloudState;
        if (expected != entry)
            return Result<void>::Failure(MakeError(SaveErrors::SlotCommitInvalid));
        return Result<void>::Success();
    }

    /** @copydoc Repack */
    Result<SaveStorageWrite> Repack(const ValidatedSaveArchive &source, const SaveGameSlotId slot,
                                    const std::optional<SlotGenerationId> previous, SaveSlotDisplayMetadata display,
                                    const SaveSlotLifecyclePolicy &policy, ISaveSlotLifecycleHost &host) {
        auto generation = host.AllocateGeneration();
        if (generation.HasError())
            return Result<SaveStorageWrite>::Failure(generation.ErrorValue());
        if (!generation.Value().IsValid() || generation.Value() == source.Header().slotGeneration || generation.Value() == previous)
            return Result<SaveStorageWrite>::Failure(MakeError(SaveErrors::SlotGenerationConflict));
        auto header = source.Header();
        header.slot = slot;
        header.slotGeneration = generation.Value();
        header.parentGeneration = previous;
        header.product = policy.destination.name.product;
        header.environment = policy.destination.name.environment;
        header.user = policy.destination.user;
        header.profile = policy.destination.profile;

        std::vector<PreservedSaveChunk> chunks;
        chunks.reserve(source.Directory().Entries().size());
        for (const auto &entry : source.Directory().Entries()) {
            const auto bytes =
                source.Payload().subspan(static_cast<std::size_t>(entry.offset), static_cast<std::size_t>(entry.storedByteLength));
            chunks.push_back({entry, {bytes.begin(), bytes.end()}});
        }
        auto written = SaveArchiveContainerWriter::Write(header, source.Manifest(), chunks, source.Preamble().archiveFormatVersion,
                                                         policy.archiveLimits);
        if (written.HasError())
            return Result<SaveStorageWrite>::Failure(written.ErrorValue());
        std::vector<std::byte> bytes(written.Value().Bytes().begin(), written.Value().Bytes().end());
        if (policy.signature == SaveSignaturePolicy::Required) {
            auto signedArchive = host.SignDestination(std::move(bytes), policy.destination);
            if (signedArchive.HasError())
                return Result<SaveStorageWrite>::Failure(signedArchive.ErrorValue());
            bytes = std::move(signedArchive).Value();
        }
        auto verified = Admit(bytes, policy.destination, policy, host);
        if (verified.HasError())
            return Result<SaveStorageWrite>::Failure(verified.ErrorValue());
        if (auto parity = VerifyRepack(verified.Value(), header, source.Manifest(), chunks); parity.HasError())
            return Result<SaveStorageWrite>::Failure(parity.ErrorValue());
        if (auto retained =
                VerifyUnknownDataRoundTrip(source, verified.Value(), policy.compatibility, policy.archiveLimits.maximumArchiveBytes);
            retained.HasError())
            return Result<SaveStorageWrite>::Failure(retained.ErrorValue());
        return Result<SaveStorageWrite>::Success(
            {Metadata(verified.Value(), std::move(display)), {std::make_shared<const std::vector<std::byte>>(std::move(bytes))}});
    }
}  // namespace Horo::Runtime::SaveSlotLifecycleDetail
