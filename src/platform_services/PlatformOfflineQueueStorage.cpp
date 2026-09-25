#include "Horo/PlatformServices/PlatformOfflineQueueStorage.h"

#include "Horo/Foundation/Platform.h"
#include "Horo/Foundation/Sha256.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>

namespace Horo::PlatformOfflineQueue {
    namespace {
        constexpr std::array<std::byte, 8> Magic{std::byte{'H'}, std::byte{'O'}, std::byte{'R'}, std::byte{'O'},
                                                 std::byte{'Q'}, std::byte{'U'}, std::byte{'E'}, std::byte{'1'}};
        constexpr std::size_t HeaderBytes = Magic.size() + sizeof(std::uint32_t) + sizeof(std::uint32_t) + sizeof(std::uint64_t) + 32;
        constexpr std::size_t RecordFixedBytes =
            16 + 16 + sizeof(std::uint8_t) + sizeof(std::uint8_t) + sizeof(std::uint64_t) + sizeof(std::uint32_t);

        [[nodiscard]] Error PathError(const ErrorCodeDescriptor &descriptor, const std::filesystem::path &path) {
            return MakeError(descriptor, std::string{descriptor.summary} + " Path: " + path.generic_string());
        }

        [[nodiscard]] bool IsKnown(const PlatformOfflineIntentState state) noexcept {
            return state <= PlatformOfflineIntentState::Abandoned;
        }

        [[nodiscard]] bool IsKnown(const PlatformOfflineOperationKind operation) noexcept {
            return operation <= PlatformOfflineOperationKind::PresenceClear;
        }

        [[nodiscard]] bool HasNonZeroByte(const std::array<std::byte, 16> &bytes) noexcept {
            return std::ranges::any_of(bytes, [](const std::byte byte) {
                return byte != std::byte{};
            });
        }

        [[nodiscard]] std::string EncodePartition(const PlatformOfflineSubjectPartition &partition) {
            constexpr std::string_view Digits{"0123456789abcdef"};
            std::string encoded;
            encoded.reserve(partition.bytes.size() * 2);
            for (const std::byte byte : partition.bytes) {
                const auto value = std::to_integer<unsigned int>(byte);
                encoded.push_back(Digits[value >> 4U]);
                encoded.push_back(Digits[value & 0x0fU]);
            }
            return encoded;
        }

        void AppendByte(std::vector<std::byte> &output, const std::byte value) {
            output.push_back(value);
        }

        void AppendU32(std::vector<std::byte> &output, const std::uint32_t value) {
            for (int shift = 24; shift >= 0; shift -= 8)
                output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
        }

        void AppendU64(std::vector<std::byte> &output, const std::uint64_t value) {
            for (int shift = 56; shift >= 0; shift -= 8)
                output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
        }

        template <std::size_t Size> void AppendArray(std::vector<std::byte> &output, const std::array<std::byte, Size> &bytes) {
            output.insert(output.end(), bytes.begin(), bytes.end());
        }

        [[nodiscard]] bool ReadBytes(const std::span<const std::byte> input, std::size_t &offset,
                                     const std::span<std::byte> destination) noexcept {
            if (offset > input.size() || destination.size() > input.size() - offset)
                return false;
            std::copy_n(input.data() + offset, destination.size(), destination.data());
            offset += destination.size();
            return true;
        }

        [[nodiscard]] bool ReadByte(const std::span<const std::byte> input, std::size_t &offset, std::byte &value) noexcept {
            return ReadBytes(input, offset, std::span<std::byte>{&value, 1});
        }

        [[nodiscard]] bool ReadU32(const std::span<const std::byte> input, std::size_t &offset, std::uint32_t &value) noexcept {
            std::array<std::byte, sizeof(std::uint32_t)> bytes{};
            if (!ReadBytes(input, offset, bytes))
                return false;
            value = 0;
            for (const std::byte byte : bytes)
                value = (value << 8U) | std::to_integer<std::uint32_t>(byte);
            return true;
        }

        [[nodiscard]] bool ReadU64(const std::span<const std::byte> input, std::size_t &offset, std::uint64_t &value) noexcept {
            std::array<std::byte, sizeof(std::uint64_t)> bytes{};
            if (!ReadBytes(input, offset, bytes))
                return false;
            value = 0;
            for (const std::byte byte : bytes)
                value = (value << 8U) | std::to_integer<std::uint64_t>(byte);
            return true;
        }

        [[nodiscard]] Result<void> ValidateLimits(const PlatformOfflineQueueLimits &limits) {
            if (limits.maximumRecords == 0 || limits.maximumRecords > PlatformOfflineQueueHardMaximumRecords ||
                limits.maximumPayloadBytes == 0 || limits.maximumPayloadBytes > PlatformOfflineQueueHardMaximumPayloadBytes ||
                limits.maximumDocumentBytes < HeaderBytes || limits.maximumDocumentBytes > PlatformOfflineQueueHardMaximumDocumentBytes)
                return Result<void>::Failure(MakeError(PlatformOfflineQueueErrors::InvalidConfiguration));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateRecord(const PlatformOfflineQueueRecord &record, const PlatformOfflineQueueLimits &limits) {
            if (!record.IsValid())
                return Result<void>::Failure(MakeError(PlatformOfflineQueueErrors::InvalidRecord));
            if (record.payload.size() > limits.maximumPayloadBytes)
                return Result<void>::Failure(MakeError(PlatformOfflineQueueErrors::PayloadTooLarge));
            if (record.payload.size() > std::numeric_limits<std::uint32_t>::max())
                return Result<void>::Failure(MakeError(PlatformOfflineQueueErrors::PayloadTooLarge));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::vector<std::byte>> ReadDocument(const std::filesystem::path &path,
                                                                  const PlatformOfflineQueueLimits &limits) {
            if (std::error_code status; !std::filesystem::exists(path, status)) {
                if (status)
                    return Result<std::vector<std::byte>>::Failure(PathError(PlatformOfflineQueueErrors::DurableUnavailable, path));
                return Result<std::vector<std::byte>>::Success({});
            }

            std::ifstream input(path, std::ios::binary);
            if (!input)
                return Result<std::vector<std::byte>>::Failure(PathError(PlatformOfflineQueueErrors::DurableUnavailable, path));

            std::vector<std::byte> document;
            std::array<char, 8192> buffer{};
            while (input) {
                input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const std::streamsize count = input.gcount();
                if (count <= 0)
                    break;
                const auto byteCount = static_cast<std::size_t>(count);
                if (document.size() > limits.maximumDocumentBytes || byteCount > limits.maximumDocumentBytes - document.size())
                    return Result<std::vector<std::byte>>::Failure(PathError(PlatformOfflineQueueErrors::CapacityExceeded, path));
                const auto oldSize = document.size();
                document.resize(oldSize + byteCount);
                std::memcpy(document.data() + oldSize, buffer.data(), byteCount);
            }
            if (input.bad())
                return Result<std::vector<std::byte>>::Failure(PathError(PlatformOfflineQueueErrors::DurableUnavailable, path));
            return Result<std::vector<std::byte>>::Success(std::move(document));
        }

        struct QueueDocumentView final {
            std::uint32_t recordCount{};
            std::span<const std::byte> body;
        };

        [[nodiscard]] Result<QueueDocumentView> ReadQueueDocumentView(const std::vector<std::byte> &document,
                                                                      const std::filesystem::path &path,
                                                                      const PlatformOfflineQueueLimits &limits) {
            if (document.size() < HeaderBytes)
                return Result<QueueDocumentView>::Failure(PathError(PlatformOfflineQueueErrors::Corrupt, path));

            std::size_t offset = 0;
            std::array<std::byte, Magic.size()> magic{};
            std::uint32_t schemaVersion{};
            std::uint32_t recordCount{};
            std::uint64_t bodyByteCount{};
            std::array<std::byte, 32> encodedDigest{};
            if (!ReadBytes(document, offset, magic) || !ReadU32(document, offset, schemaVersion) ||
                !ReadU32(document, offset, recordCount) || !ReadU64(document, offset, bodyByteCount) ||
                !ReadBytes(document, offset, encodedDigest))
                return Result<QueueDocumentView>::Failure(PathError(PlatformOfflineQueueErrors::Corrupt, path));
            if (magic != Magic)
                return Result<QueueDocumentView>::Failure(PathError(PlatformOfflineQueueErrors::Corrupt, path));
            if (schemaVersion != PlatformOfflineQueueSchemaVersion)
                return Result<QueueDocumentView>::Failure(MakeError(PlatformOfflineQueueErrors::UnsupportedVersion));
            if (recordCount > limits.maximumRecords)
                return Result<QueueDocumentView>::Failure(MakeError(PlatformOfflineQueueErrors::CapacityExceeded));
            if (bodyByteCount > static_cast<std::uint64_t>(limits.maximumDocumentBytes - HeaderBytes) ||
                bodyByteCount != document.size() - HeaderBytes)
                return Result<QueueDocumentView>::Failure(PathError(PlatformOfflineQueueErrors::Corrupt, path));

            Sha256Digest expectedDigest;
            for (std::size_t index = 0; index < expectedDigest.bytes.size(); ++index)
                expectedDigest.bytes[index] = std::to_integer<std::uint8_t>(encodedDigest[index]);
            if (const auto body = std::span<const std::byte>{document}.subspan(HeaderBytes, static_cast<std::size_t>(bodyByteCount));
                ComputeSha256(body) != expectedDigest)
                return Result<QueueDocumentView>::Failure(PathError(PlatformOfflineQueueErrors::Corrupt, path));
            return Result<QueueDocumentView>::Success(QueueDocumentView{
                .recordCount = recordCount,
                .body = std::span<const std::byte>{document}.subspan(HeaderBytes, static_cast<std::size_t>(bodyByteCount)),
            });
        }

        [[nodiscard]] Result<PlatformOfflineQueueRecord> ReadQueueRecord(const std::span<const std::byte> body, std::size_t &offset,
                                                                         const std::filesystem::path &path,
                                                                         const PlatformOfflineQueueLimits &limits) {
            PlatformOfflineQueueRecord record;
            std::byte state{};
            std::byte operation{};
            std::uint32_t payloadBytes{};
            if (!ReadBytes(body, offset, record.identity.bytes) || !ReadBytes(body, offset, record.partition.bytes) ||
                !ReadByte(body, offset, state) || !ReadByte(body, offset, operation) || !ReadU64(body, offset, record.sequence) ||
                !ReadU32(body, offset, payloadBytes))
                return Result<PlatformOfflineQueueRecord>::Failure(PathError(PlatformOfflineQueueErrors::Corrupt, path));
            if (payloadBytes > limits.maximumPayloadBytes || payloadBytes > body.size() - offset)
                return Result<PlatformOfflineQueueRecord>::Failure(MakeError(PlatformOfflineQueueErrors::PayloadTooLarge));

            record.state = static_cast<PlatformOfflineIntentState>(std::to_integer<std::uint8_t>(state));
            record.operation = static_cast<PlatformOfflineOperationKind>(std::to_integer<std::uint8_t>(operation));
            record.payload.resize(payloadBytes);
            if (!ReadBytes(body, offset, record.payload))
                return Result<PlatformOfflineQueueRecord>::Failure(PathError(PlatformOfflineQueueErrors::Corrupt, path));
            if (const auto valid = ValidateRecord(record, limits); valid.HasError())
                return Result<PlatformOfflineQueueRecord>::Failure(valid.ErrorValue());
            return Result<PlatformOfflineQueueRecord>::Success(std::move(record));
        }

        [[nodiscard]] Result<std::vector<PlatformOfflineQueueRecord>> DecodeQueueRecords(const QueueDocumentView &document,
                                                                                         const PlatformOfflineSubjectPartition &partition,
                                                                                         const std::filesystem::path &path,
                                                                                         const PlatformOfflineQueueLimits &limits) {
            std::vector<PlatformOfflineQueueRecord> records;
            records.reserve(document.recordCount);
            std::size_t offset = 0;
            for (std::uint32_t index = 0; index < document.recordCount; ++index) {
                auto record = ReadQueueRecord(document.body, offset, path, limits);
                if (record.HasError())
                    return Result<std::vector<PlatformOfflineQueueRecord>>::Failure(record.ErrorValue());
                if (record.Value().partition != partition)
                    return Result<std::vector<PlatformOfflineQueueRecord>>::Failure(MakeError(PlatformOfflineQueueErrors::InvalidRecord));
                if (std::ranges::find_if(records, [&record](const auto &existing) {
                    return existing.identity == record.Value().identity;
                }) != records.end())
                    return Result<std::vector<PlatformOfflineQueueRecord>>::Failure(
                        MakeError(PlatformOfflineQueueErrors::IdentityConflict));
                records.push_back(std::move(record).Value());
            }
            if (offset != document.body.size())
                return Result<std::vector<PlatformOfflineQueueRecord>>::Failure(PathError(PlatformOfflineQueueErrors::Corrupt, path));
            return Result<std::vector<PlatformOfflineQueueRecord>>::Success(std::move(records));
        }
    }  // namespace

    bool PlatformOfflineIntentId::IsValid() const noexcept {
        return HasNonZeroByte(bytes);
    }

    bool PlatformOfflineSubjectPartition::IsValid() const noexcept {
        return HasNonZeroByte(bytes);
    }

    bool PlatformOfflineQueueRecord::IsValid() const noexcept {
        return identity.IsValid() && partition.IsValid() && IsKnown(state) && IsKnown(operation) && sequence != 0;
    }

    PlatformOfflineQueueStorage::PlatformOfflineQueueStorage(DurableFileSystem &files, std::filesystem::path root,
                                                             const PlatformOfflineQueueLimits limits) noexcept
        : files_(&files), root_(std::move(root)), limits_(limits) {}

    Result<PlatformOfflineQueueStorage> PlatformOfflineQueueStorage::Create(DurableFileSystem &files, std::filesystem::path root,
                                                                            const PlatformOfflineQueueLimits &limits) {
        if (root.empty())
            return Result<PlatformOfflineQueueStorage>::Failure(MakeError(PlatformOfflineQueueErrors::InvalidConfiguration));
        if (const auto valid = ValidateLimits(limits); valid.HasError())
            return Result<PlatformOfflineQueueStorage>::Failure(valid.ErrorValue());
        return Result<PlatformOfflineQueueStorage>::Success(PlatformOfflineQueueStorage{files, std::move(root), limits});
    }

    std::filesystem::path PlatformOfflineQueueStorage::PartitionPath(const PlatformOfflineSubjectPartition &partition) const {
        return root_ / "offline-queue" / "partitions" / (EncodePartition(partition) + ".horoqueue");
    }

    Result<std::vector<std::byte>> PlatformOfflineQueueStorage::Encode(const PlatformOfflineSubjectPartition &partition,
                                                                       const std::span<const PlatformOfflineQueueRecord> records) const {
        if (!partition.IsValid())
            return Result<std::vector<std::byte>>::Failure(MakeError(PlatformOfflineQueueErrors::InvalidPartition));
        if (records.size() > limits_.maximumRecords)
            return Result<std::vector<std::byte>>::Failure(MakeError(PlatformOfflineQueueErrors::CapacityExceeded));

        std::size_t bodyBytes = 0;
        for (const auto &record : records) {
            if (const auto valid = ValidateRecord(record, limits_); valid.HasError())
                return Result<std::vector<std::byte>>::Failure(valid.ErrorValue());
            if (record.partition != partition)
                return Result<std::vector<std::byte>>::Failure(MakeError(PlatformOfflineQueueErrors::InvalidRecord));
            if (bodyBytes > limits_.maximumDocumentBytes - HeaderBytes)
                return Result<std::vector<std::byte>>::Failure(MakeError(PlatformOfflineQueueErrors::CapacityExceeded));
            if (const std::size_t remainingBytes = limits_.maximumDocumentBytes - HeaderBytes - bodyBytes;
                remainingBytes < RecordFixedBytes || record.payload.size() > remainingBytes - RecordFixedBytes)
                return Result<std::vector<std::byte>>::Failure(MakeError(PlatformOfflineQueueErrors::CapacityExceeded));
            bodyBytes += RecordFixedBytes + record.payload.size();
        }

        std::vector<std::byte> body;
        body.reserve(bodyBytes);
        std::vector<PlatformOfflineIntentId> identities;
        identities.reserve(records.size());
        for (const auto &record : records) {
            if (std::ranges::find(identities, record.identity) != identities.end())
                return Result<std::vector<std::byte>>::Failure(MakeError(PlatformOfflineQueueErrors::IdentityConflict));
            identities.push_back(record.identity);
            AppendArray(body, record.identity.bytes);
            AppendArray(body, record.partition.bytes);
            AppendByte(body, static_cast<std::byte>(record.state));
            AppendByte(body, static_cast<std::byte>(record.operation));
            AppendU64(body, record.sequence);
            AppendU32(body, static_cast<std::uint32_t>(record.payload.size()));
            body.insert(body.end(), record.payload.begin(), record.payload.end());
        }

        const Sha256Digest digest = ComputeSha256(body);
        std::vector<std::byte> document;
        document.reserve(HeaderBytes + body.size());
        document.insert(document.end(), Magic.begin(), Magic.end());
        AppendU32(document, PlatformOfflineQueueSchemaVersion);
        AppendU32(document, static_cast<std::uint32_t>(records.size()));
        AppendU64(document, static_cast<std::uint64_t>(body.size()));
        for (const std::uint8_t byte : digest.bytes)
            document.push_back(static_cast<std::byte>(byte));
        document.insert(document.end(), body.begin(), body.end());
        return Result<std::vector<std::byte>>::Success(std::move(document));
    }

    Result<std::vector<PlatformOfflineQueueRecord>> PlatformOfflineQueueStorage::Load(
        const PlatformOfflineSubjectPartition &partition) const {
        if (!partition.IsValid())
            return Result<std::vector<PlatformOfflineQueueRecord>>::Failure(MakeError(PlatformOfflineQueueErrors::InvalidPartition));
        const std::filesystem::path path = PartitionPath(partition);
        std::error_code existsError;
        const bool exists = std::filesystem::exists(path, existsError);
        if (existsError)
            return Result<std::vector<PlatformOfflineQueueRecord>>::Failure(
                PathError(PlatformOfflineQueueErrors::DurableUnavailable, path));
        const auto documentResult = ReadDocument(path, limits_);
        if (documentResult.HasError())
            return Result<std::vector<PlatformOfflineQueueRecord>>::Failure(documentResult.ErrorValue());
        const std::vector<std::byte> &document = documentResult.Value();
        if (!exists && document.empty())
            return Result<std::vector<PlatformOfflineQueueRecord>>::Success({});
        if (document.empty())
            return Result<std::vector<PlatformOfflineQueueRecord>>::Failure(PathError(PlatformOfflineQueueErrors::Corrupt, path));
        const auto documentView = ReadQueueDocumentView(document, path, limits_);
        if (documentView.HasError())
            return Result<std::vector<PlatformOfflineQueueRecord>>::Failure(documentView.ErrorValue());
        return DecodeQueueRecords(documentView.Value(), partition, path, limits_);
    }

    Result<void> PlatformOfflineQueueStorage::Publish(const PlatformOfflineSubjectPartition &partition,
                                                      const std::span<const PlatformOfflineQueueRecord> records) const {
        const auto document = Encode(partition, records);
        if (document.HasError())
            return Result<void>::Failure(document.ErrorValue());
        const std::filesystem::path destination = PartitionPath(partition);
        std::filesystem::path lockPath = destination;
        lockPath += ".lock";
        std::filesystem::path preparedPath = destination;
        preparedPath += ".tmp";
        auto acquired = files_->TryAcquireExclusive(lockPath, "horo.platform.offline");
        if (acquired.HasError())
            return Result<void>::Failure(acquired.ErrorValue());
        [[maybe_unused]] auto publicationLock = std::move(acquired).Value();  // Hold through durable write and atomic replacement.

        const auto available = files_->AvailableBytes(destination.parent_path());
        if (available.HasError())
            return Result<void>::Failure(available.ErrorValue());
        if (available.Value() < document.Value().size())
            return Result<void>::Failure(MakeError(PlatformOfflineQueueErrors::CapacityExceeded));

        if (auto written = files_->WriteDurable(preparedPath, document.Value()); written.HasError()) {
            static_cast<void>(files_->RemoveDurable(preparedPath));
            return Result<void>::Failure(written.ErrorValue());
        }
        if (auto replaced = files_->AtomicReplace(preparedPath, destination); replaced.HasError())
            return Result<void>::Failure(MakeError(PlatformOfflineQueueErrors::StorageUnknown));
        return Result<void>::Success();
    }
}  // namespace Horo::PlatformOfflineQueue
