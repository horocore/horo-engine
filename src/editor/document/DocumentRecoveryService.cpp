#include "Horo/Editor/DocumentRecoveryService.h"

#include "Horo/Foundation/PathUtils.h"

#include <algorithm>
#include <array>

namespace Horo::Editor {
    namespace {
        constexpr std::array<std::byte, 8> RecoveryMagic{std::byte{'H'}, std::byte{'D'}, std::byte{'R'}, std::byte{'C'},
                                                         std::byte{0},   std::byte{0},   std::byte{0},   std::byte{1}};

        /** @brief Writes fixed-width network-order recovery metadata, never interprets source payloads. */
        void Integer(std::vector<std::byte> &bytes, std::uint64_t value, std::size_t width) {
            for (std::size_t index = width; index != 0; --index)
                bytes.push_back(static_cast<std::byte>((value >> ((index - 1) * 8)) & 0xff));
        }

        /** @brief Appends a bounded raw field to the envelope. */
        template <typename T> void Field(std::vector<std::byte> &bytes, const T &value) {
            const auto field = std::as_bytes(std::span(value));
            bytes.insert(bytes.end(), field.begin(), field.end());
        }

        /** @brief Checks complete metadata before allocation. */
        bool ValidRecord(const DocumentRecoveryRecord &record) {
            return record.asset.IsValid() && record.document.IsValid() && record.sessionOwner.IsValid() && record.sessionGeneration &&
                   record.revision && record.capturedAtUnixSeconds && record.acceptedSource.exists &&
                   record.acceptedSource.bytes <= 64 * 1024 * 1024;
        }

        /** @brief Encodes the shared checksum-protected envelope over unchanged domain source bytes. */
        Result<std::vector<std::byte>> Encode(const DocumentRecoveryRecord &record, std::size_t maximum) {
            const auto &path = record.document.source.Value();
            constexpr std::size_t maximumHeader = 256;
            if (!ValidRecord(record) || maximum < maximumHeader + path.size() ||
                record.payload.size() > maximum - maximumHeader - path.size())
                return Result<std::vector<std::byte>>::Failure(MakeError(Assets::AssetSourceErrors::Limit));
            std::vector<std::byte> bytes;
            bytes.reserve(maximumHeader + path.size() + record.payload.size());
            Field(bytes, RecoveryMagic);
            Integer(bytes, static_cast<std::uint8_t>(record.document.kind), 1);
            Field(bytes, record.asset.Bytes());
            Field(bytes, record.sessionOwner.Bytes());
            Integer(bytes, record.sessionGeneration, 8);
            Integer(bytes, record.revision, 8);
            Integer(bytes, record.capturedAtUnixSeconds, 8);
            Integer(bytes, record.acceptedSource.bytes, 8);
            Field(bytes, record.acceptedSource.digest.bytes);
            Field(bytes, record.state.bytes);
            Integer(bytes, path.size(), 2);
            Field(bytes, path);
            Integer(bytes, record.payload.size(), 4);
            Field(bytes, record.payload);
            Field(bytes, ComputeSha256(bytes).bytes);
            return Result<std::vector<std::byte>>::Success(std::move(bytes));
        }

        /** @brief Allocation-free bounded reader for envelope fields. */
        struct Reader final {
            std::span<const std::byte> bytes;
            std::size_t offset{};
            bool valid{true};

            std::span<const std::byte> Take(std::size_t size) {
                if (size > bytes.size() - offset) {
                    valid = false;
                    return {};
                }
                const auto field = bytes.subspan(offset, size);
                offset += size;
                return field;
            }

            std::uint64_t Integer(std::size_t width) {
                std::uint64_t value{};
                for (const auto byte : Take(width))
                    value = (value << 8) | std::to_integer<std::uint8_t>(byte);
                return value;
            }

            template <typename T> void Array(T &array) {
                const auto field = Take(array.size());
                if (valid)
                    for (std::size_t index = 0; index < array.size(); ++index)
                        array[index] = std::to_integer<std::uint8_t>(field[index]);
            }
        };

        /** @brief Validates the whole envelope checksum and finite fields before source payload allocation. */
        Result<DocumentRecoveryRecord> Decode(std::span<const std::byte> bytes) {
            const auto failure = [] {
                return Result<DocumentRecoveryRecord>::Failure(MakeError(Assets::AssetSourceErrors::ReadFailed));
            };
            if (bytes.size() < 175 || !std::ranges::equal(bytes.first(8), RecoveryMagic))
                return failure();
            Sha256Digest checksum;
            Reader tail{bytes.last(32)};
            tail.Array(checksum.bytes);
            if (ComputeSha256(bytes.first(bytes.size() - 32)) != checksum)
                return failure();
            Reader reader{bytes.first(bytes.size() - 32), 8};
            DocumentRecoveryRecord record;
            record.document.kind = static_cast<DocumentKind>(reader.Integer(1));
            std::array<std::uint8_t, 16> asset{}, owner{};
            reader.Array(asset);
            reader.Array(owner);
            record.asset = Assets::AssetId::FromBytes(asset);
            record.sessionOwner = Assets::AssetId::FromBytes(owner);
            record.sessionGeneration = reader.Integer(8);
            record.revision = reader.Integer(8);
            record.capturedAtUnixSeconds = reader.Integer(8);
            record.acceptedSource = {.exists = true, .bytes = static_cast<std::size_t>(reader.Integer(8))};
            reader.Array(record.acceptedSource.digest.bytes);
            reader.Array(record.state.bytes);
            const auto pathBytes = reader.Integer(2);
            if (pathBytes > MaximumSourceDocumentIdBytes)
                return failure();
            const auto path = reader.Take(static_cast<std::size_t>(pathBytes));
            if (!reader.valid)
                return failure();
            auto source = SourceDocumentId::Parse({reinterpret_cast<const char *>(path.data()), path.size()});
            if (source.HasError())
                return failure();
            record.document.source = std::move(source).Value();
            const auto size = reader.Integer(4);
            const auto payload = reader.Take(static_cast<std::size_t>(size));
            if (!reader.valid || reader.offset != reader.bytes.size() || !ValidRecord(record))
                return failure();
            record.payload.assign(payload.begin(), payload.end());
            return Result<DocumentRecoveryRecord>::Success(std::move(record));
        }
    }  // namespace

    /** @copydoc DocumentRecoveryService::DocumentRecoveryService */
    DocumentRecoveryService::DocumentRecoveryService(std::filesystem::path root, const ProjectMutationCoordinator &mutations,
                                                     DurableFileSystem &files, DocumentRecoveryLimits limits)
        : root_(std::move(root)), mutations_(mutations), files_(files), limits_(limits) {}

    /** @copydoc DocumentRecoveryService::Path */
    Result<std::filesystem::path> DocumentRecoveryService::Path(Assets::AssetId asset, const DocumentOpenKey &key) const {
        if (!asset.IsValid() || !key.IsValid())
            return Result<std::filesystem::path>::Failure(MakeError(Assets::AssetSourceErrors::InvalidPath));
        const auto path = ToString(key.kind);
        const std::array<std::span<const std::byte>, 3> fields{std::as_bytes(std::span(asset.Bytes())), std::as_bytes(std::span(path)),
                                                               std::as_bytes(std::span(key.source.Value()))};
        const auto digest = ComputeSha256Fragments(fields);
        return Result<std::filesystem::path>::Success(root_ / ".horo/local/recovery" / (FormatSha256(digest).substr(7) + ".hororecovery"));
    }

    /** @copydoc DocumentRecoveryService::Inspect */
    Result<std::optional<DocumentRecoveryRecord>> DocumentRecoveryService::Inspect(Assets::AssetId asset,
                                                                                   const DocumentOpenKey &key) const {
        const auto path = Path(asset, key);
        if (path.HasError())
            return Result<std::optional<DocumentRecoveryRecord>>::Failure(path.ErrorValue());
        auto capture = Assets::CaptureAssetSource(root_, path.Value(), limits_.maximumRecordBytes);
        if (capture.HasError())
            return Result<std::optional<DocumentRecoveryRecord>>::Failure(capture.ErrorValue());
        if (!capture.Value().identity.exists)
            return Result<std::optional<DocumentRecoveryRecord>>::Success(std::nullopt);
        auto decoded = Decode(capture.Value().bytes);
        if (decoded.HasError())
            return Result<std::optional<DocumentRecoveryRecord>>::Failure(decoded.ErrorValue());
        if (decoded.Value().asset != asset || decoded.Value().document != key)
            return Result<std::optional<DocumentRecoveryRecord>>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        return Result<std::optional<DocumentRecoveryRecord>>::Success(std::move(decoded).Value());
    }

    /** @copydoc DocumentRecoveryService::Write */
    Result<void> DocumentRecoveryService::Write(const DocumentRecoveryRecord &record, CancellationToken cancellation) {
        return WriteRecord(record, nullptr, cancellation);
    }

    /** @copydoc DocumentRecoveryService::ReplaceAccepted */
    Result<void> DocumentRecoveryService::ReplaceAccepted(const DocumentRecoveryRecord &record, const DocumentRecoveryRecord &accepted,
                                                          CancellationToken cancellation) {
        if (record.asset != accepted.asset || record.document != accepted.document)
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        return WriteRecord(record, &accepted, cancellation);
    }

    /** @copydoc DocumentRecoveryService::WriteRecord */
    Result<void> DocumentRecoveryService::WriteRecord(const DocumentRecoveryRecord &record, const DocumentRecoveryRecord *accepted,
                                                      CancellationToken cancellation) {
        auto bytes = Encode(record, limits_.maximumRecordBytes);
        if (bytes.HasError())
            return Result<void>::Failure(bytes.ErrorValue());
        auto path = Path(record.asset, record.document);
        if (path.HasError())
            return Result<void>::Failure(path.ErrorValue());
        auto lease = mutations_.TryAcquire({root_, ProjectMutationOwner::Autosave, "document-recovery"});
        if (lease.HasError())
            return Result<void>::Failure(lease.ErrorValue());
        auto existing = Inspect(record.asset, record.document);
        if (existing.HasError())
            return Result<void>::Failure(existing.ErrorValue());
        if (accepted && (!existing.Value() || *existing.Value() != *accepted))
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        if (existing.Value() && !accepted) {
            const auto &old = *existing.Value();
            if (old.sessionOwner != record.sessionOwner || old.sessionGeneration != record.sessionGeneration ||
                old.revision > record.revision ||
                (old.revision == record.revision && (old.state != record.state || old.payload != record.payload)))
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            if (old.state == record.state && old.acceptedSource == record.acceptedSource && old.payload == record.payload)
                return Result<void>::Success();
        }
        auto expected = Assets::CaptureAssetSource(root_, path.Value(), limits_.maximumRecordBytes, cancellation);
        if (expected.HasError())
            return Result<void>::Failure(expected.ErrorValue());
        if (existing.Value()) {
            const auto observed = Encode(*existing.Value(), limits_.maximumRecordBytes);
            if (observed.HasError())
                return Result<void>::Failure(observed.ErrorValue());
            if (observed.Value() != expected.Value().bytes)
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        } else if (expected.Value().identity.exists) {
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        }
        auto publication =
            PublishDocumentRecovery(root_, path.Value(), bytes.Value(), expected.Value().identity, files_, limits_, cancellation);
        if (publication.HasError())
            return Result<void>::Failure(publication.ErrorValue());
        if (publication.Value().durabilityError)
            return Result<void>::Failure(*publication.Value().durabilityError);
        return Result<void>::Success();
    }

    /** @copydoc DocumentRecoveryService::Discard */
    Result<void> DocumentRecoveryService::Discard(const DocumentRecoveryRecord &record) {
        auto encoded = Encode(record, limits_.maximumRecordBytes);
        if (encoded.HasError())
            return Result<void>::Failure(encoded.ErrorValue());
        auto path = Path(record.asset, record.document);
        if (path.HasError())
            return Result<void>::Failure(path.ErrorValue());
        auto lease = mutations_.TryAcquire({root_, ProjectMutationOwner::Autosave, "discard-document-recovery"});
        if (lease.HasError())
            return Result<void>::Failure(lease.ErrorValue());
        const auto lockPath = Assets::AssetSourcePublicationLockPath(root_, path.Value());
        if (lockPath.HasError())
            return Result<void>::Failure(lockPath.ErrorValue());
        auto sourceLease = files_.TryAcquireExclusive(lockPath.Value(), "discard-document-recovery");
        if (sourceLease.HasError())
            return Result<void>::Failure(sourceLease.ErrorValue());
        auto actual = Assets::CaptureAssetSource(root_, path.Value(), limits_.maximumRecordBytes);
        if (actual.HasError())
            return Result<void>::Failure(actual.ErrorValue());
        if (!actual.Value().identity.exists)
            return Result<void>::Success();
        if (actual.Value().bytes != encoded.Value())
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        return files_.RemoveDurable(path.Value());
    }

    /** @copydoc PublishDocumentRecovery */
    Result<Assets::AssetSourcePublication> PublishDocumentRecovery(const std::filesystem::path &root,
                                                                   const std::filesystem::path &destination,
                                                                   std::span<const std::byte> bytes,
                                                                   const Assets::AssetSourceIdentity &expected, DurableFileSystem &files,
                                                                   DocumentRecoveryLimits limits, CancellationToken cancellation) {
        const auto recoveryRoot = root / ".horo/local/recovery";
        if (!Foundation::Paths::HasPathPrefix(recoveryRoot, destination) || recoveryRoot == destination || limits.maximumRecordBytes == 0 ||
            limits.maximumRecordBytes > 64 * 1024 * 1024 || limits.maximumRecords == 0 || limits.maximumRecords > 64 ||
            limits.maximumTotalBytes > 128 * 1024 * 1024 || bytes.size() > limits.maximumRecordBytes)
            return Result<Assets::AssetSourcePublication>::Failure(MakeError(Assets::AssetSourceErrors::Limit));
        auto lockPath = Assets::AssetSourcePublicationLockPath(root, destination);
        if (lockPath.HasError())
            return Result<Assets::AssetSourcePublication>::Failure(lockPath.ErrorValue());
        std::error_code error;
        std::size_t records{};
        std::size_t total{};
        if (std::filesystem::exists(recoveryRoot, error)) {
            for (std::filesystem::directory_iterator iterator(recoveryRoot, error), end; !error && iterator != end;
                 iterator.increment(error)) {
                const auto status = iterator->symlink_status(error);
                if (error || !std::filesystem::is_regular_file(status))
                    return Result<Assets::AssetSourcePublication>::Failure(MakeError(Assets::AssetSourceErrors::InvalidPath));
                if (iterator->path() == destination)
                    continue;
                const auto size = iterator->file_size(error);
                if (error || ++records >= limits.maximumRecords || size > limits.maximumTotalBytes - total)
                    return Result<Assets::AssetSourcePublication>::Failure(MakeError(Assets::AssetSourceErrors::Limit));
                total += static_cast<std::size_t>(size);
            }
        }
        if (error)
            return Result<Assets::AssetSourcePublication>::Failure(MakeError(Assets::AssetSourceErrors::ReadFailed));
        if (bytes.size() > limits.maximumTotalBytes - total)
            return Result<Assets::AssetSourcePublication>::Failure(MakeError(Assets::AssetSourceErrors::Limit));
        std::filesystem::create_directories(recoveryRoot, error);
        if (error)
            return Result<Assets::AssetSourcePublication>::Failure(MakeError(Assets::AssetSourceErrors::ReadFailed));
        auto lock = files.TryAcquireExclusive(lockPath.Value(), "editor-document-recovery");
        if (lock.HasError())
            return Result<Assets::AssetSourcePublication>::Failure(lock.ErrorValue());
        return Assets::PublishAssetSource(root, destination, expected, bytes, lock.Value(), files, cancellation);
    }
}  // namespace Horo::Editor
