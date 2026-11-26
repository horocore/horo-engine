#include "Horo/Application/CloudSaveTransport.h"
#include "Horo/Application/CloudSaveMutationJournal.h"

#include <algorithm>
#include <array>
#include <span>
#include <string_view>
#include <utility>

namespace Horo::Application {
    namespace {
        using namespace PlatformServices;

        /** @brief Tags the durable partition encoding independently from the slot suffix. */
        constexpr std::string_view partitionDomain = "horo.cloud-save.partition.v1";
        constexpr std::string_view slotDomain = "horo.cloud-save.slot.v1";
        constexpr std::string_view journalDomain = "horo.cloud-save.journal.v1";

        /** @brief Hashes portable partition identities while excluding the installation-local user ID. */
        Sha256Digest HashPartition(const Runtime::SaveCloudMetadataScope &scope, const Runtime::GameProfileId &profile) {
            const std::array<std::span<const std::byte>, 6> parts{
                std::as_bytes(std::span{partitionDomain}), std::as_bytes(std::span{scope.localNamespace.product.Bytes()}),
                std::as_bytes(std::span{scope.localNamespace.environment.Bytes()}), std::as_bytes(std::span{profile.Bytes()}),
                std::as_bytes(std::span{scope.provider.Bytes()}), std::as_bytes(std::span{scope.account.Bytes()})};
            return ComputeSha256Fragments(parts);
        }

        /** @brief Separates durable local ownership from portable remote addressing. */
        Sha256Digest HashJournalPartition(const Runtime::CanonicalSaveNamespaceKey &local, const Sha256Digest &remote) {
            const std::array<std::span<const std::byte>, 3> parts{
                std::as_bytes(std::span{journalDomain}), local.Bytes(), std::as_bytes(std::span{remote.bytes})};
            return ComputeSha256Fragments(parts);
        }

        /** @brief Projects a durable Save mutation UUID onto the exact opaque transfer bytes. */
        CloudMutationId MutationId(const Runtime::SaveCloudMutationId &mutation) {
            CloudMutationId result;
            std::ranges::copy(std::as_bytes(std::span{mutation.Bytes()}), result.bytes.begin());
            return result;
        }

        /** @brief Admits only operations with an application owner and preserves the frontend result. */
        template <typename T, typename Submit>
        Result<CloudSaveRequest<T>> Admit(const OperationId operation, Submit submit) {
            using Response = Result<CloudSaveRequest<T>>;
            if (operation == 0)
                return Response::Failure(MakeError(Runtime::SaveErrors::OperationInvalid));
            auto request = submit();
            if (request.HasError())
                return Response::Failure(request.ErrorValue());
            return Response::Success({.operation = operation, .handle = std::move(request).Value()});
        }
    }

    CloudSaveTransport::CloudSaveTransport(const PlatformServicesFrontend &frontend, const PlatformSubjectHandle subject,
                                           CloudSaveObjectPrefix prefix, const Sha256Digest journalPartition) noexcept
        : frontend_(&frontend), subject_(subject), prefix_(std::move(prefix)), journalPartition_(journalPartition) {}

    /** @copydoc CloudSaveTransport::Create */
    Result<CloudSaveTransport> CloudSaveTransport::Create(PlatformServicesFrontend &frontend,
                                                         const Runtime::SaveCloudMetadataScope &scope,
                                                         const PlatformSubjectHandle subject) {
        const auto *owner = std::get_if<Runtime::UserProfileOwner>(&scope.localNamespace.owner);
        if (!owner || !scope.IsValid() || !subject.IsValid())
            return Result<CloudSaveTransport>::Failure(MakeError(Runtime::SaveErrors::CloudMetadataInvalid));
        auto local = Runtime::EncodeSaveNamespaceKey(scope.localNamespace);
        if (local.HasError())
            return Result<CloudSaveTransport>::Failure(local.ErrorValue());
        const auto digest = HashPartition(scope, owner->profile);
        auto prefix = CloudSaveObjectPrefix::Copy(std::as_bytes(std::span{digest.bytes}));
        if (prefix.HasError())
            return Result<CloudSaveTransport>::Failure(prefix.ErrorValue());
        return Result<CloudSaveTransport>::Success(
            CloudSaveTransport{frontend, subject, std::move(prefix).Value(), HashJournalPartition(local.Value(), digest)});
    }

    /** @copydoc CloudSaveTransport::Key */
    Result<CloudSaveObjectKey> CloudSaveTransport::Key(const Runtime::SaveGameSlotId slot) const {
        if (!slot.IsValid())
            return Result<CloudSaveObjectKey>::Failure(MakeError(Runtime::SaveErrors::CloudMetadataInvalid));
        const std::array<std::span<const std::byte>, 3> parts{
            std::as_bytes(std::span{slotDomain}), prefix_.Bytes(), std::as_bytes(std::span{slot.Bytes()})};
        const auto suffix = ComputeSha256Fragments(parts);
        constexpr auto digestBytes = Sha256Digest{}.bytes.size();
        std::array<std::byte, 2 * digestBytes> key{};
        const auto end = std::ranges::copy(prefix_.Bytes(), key.begin()).out;
        std::ranges::copy(std::as_bytes(std::span{suffix.bytes}), end);
        return CloudSaveObjectKey::Copy(key);
    }

    /** @copydoc CloudSaveTransport::List */
    Result<CloudSaveRequest<CloudObjectPage>> CloudSaveTransport::List(const OperationId operation, const std::uint32_t pageSize,
                                                                      std::optional<CloudListCursor> cursor) const {
        return Admit<CloudObjectPage>(operation, [&] {
            return frontend_->ListCloudObjects({.subject = subject_, .prefix = prefix_, .cursor = std::move(cursor), .pageSize = pageSize});
        });
    }

    /** @copydoc CloudSaveTransport::Read */
    Result<CloudSaveRequest<CloudBlobReadResult>> CloudSaveTransport::Read(const OperationId operation,
                                                                         const Runtime::SaveGameSlotId slot,
                                                                         const std::uint64_t maximumBytes) const {
        return Admit<CloudBlobReadResult>(operation, [&]() -> Result<PlatformRequestHandle<CloudBlobReadResult>> {
            auto key = Key(slot);
            if (key.HasError())
                return Result<PlatformRequestHandle<CloudBlobReadResult>>::Failure(key.ErrorValue());
            return frontend_->ReadCloudObject(
                CloudBlobReadRequest{.subject = subject_, .key = std::move(key).Value(), .maximumBytes = maximumBytes});
        });
    }

    /** @copydoc CloudSaveTransport::DispatchMutation */
    Result<CloudSaveRequest<CloudMutationResult>> CloudSaveTransport::DispatchMutation(
        const OperationId operation, const CloudSaveDurableMutation &mutation, std::optional<CloudBlobOwnedBytes> bytes) const {
        return Admit<CloudMutationResult>(operation, [&]() -> Result<PlatformRequestHandle<CloudMutationResult>> {
            using Response = Result<PlatformRequestHandle<CloudMutationResult>>;
            if (mutation.partition_ != journalPartition_)
                return Response::Failure(MakeError(Runtime::SaveErrors::CloudMetadataStale));
            const auto &intent = mutation.intent_;
            if (intent.upload.has_value() != bytes.has_value())
                return Response::Failure(MakeError(CloudObjectErrors::InvalidRequest));
            if (!intent.upload)
                return frontend_->DeleteCloudObject({.subject = subject_,
                                                     .key = mutation.key_,
                                                     .expectedRevision = *intent.revision,
                                                     .mutation = MutationId(intent.mutation)});
            if (bytes->Bytes().size() != intent.upload->size)
                return Response::Failure(MakeError(CloudObjectErrors::IntegrityMismatch));

            CloudWritePrecondition condition = CloudCreateIfAbsent{};
            if (intent.revision)
                condition = CloudMatchProviderRevision{*intent.revision};
            return frontend_->WriteCloudObject({.subject = subject_,
                                                .key = mutation.key_,
                                                .bytes = std::move(*bytes),
                                                .expectedDigest = intent.upload->digest,
                                                .precondition = std::move(condition),
                                                .mutation = MutationId(intent.mutation)});
        });
    }
}
