#include "StreamingCellArtifactInternal.h"

#include <algorithm>
#include <array>
#include <bit>

namespace Horo::WorldStreaming::Detail {
    namespace {
        constexpr std::size_t HeaderBytes = 96;
        constexpr std::size_t RowBytes = 40;
        constexpr std::size_t HashOffset = 64;
        constexpr std::size_t WorkUnitBytes = 65536;

        template <typename T> [[nodiscard]] Result<T> Fail(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Reads a little-endian scalar only from a previously bounded fixed-size record. */
        template <typename T> [[nodiscard]] T Read(const std::span<const std::byte> bytes, const std::size_t offset) noexcept {
            T value{};
            for (std::size_t index = 0; index < sizeof(T); ++index)
                value |= static_cast<T>(std::to_integer<std::uint64_t>(bytes[offset + index]) << (index * 8U));
            return value;
        }

        /** @brief Computes the wire hash in cancellable bounded units with its digest field zeroed. */
        [[nodiscard]] Result<Sha256Digest> ArtifactHash(const std::span<const std::byte> bytes, const CancellationToken &cancellation) {
            Sha256Builder builder;
            if (const std::array<std::byte, 32> zero{}; !builder.Update(bytes.first(HashOffset)) || !builder.Update(zero))
                return Fail<Sha256Digest>(WorldStreamingErrors::CellCandidateCapacityExceeded);
            for (std::size_t offset = HeaderBytes; offset < bytes.size();) {
                if (cancellation.IsCancellationRequested())
                    return Fail<Sha256Digest>(WorldStreamingErrors::CellCandidateLifecycleUnavailable);
                const auto count = std::min(WorkUnitBytes, bytes.size() - offset);
                if (!builder.Update(bytes.subspan(offset, count)))
                    return Fail<Sha256Digest>(WorldStreamingErrors::CellCandidateCapacityExceeded);
                offset += count;
            }
            return Result<Sha256Digest>::Success(builder.Finalize());
        }

        /** @brief Applies one GF(2) matrix to a reflected IEEE CRC value. */
        [[nodiscard]] std::uint32_t Apply(const std::array<std::uint32_t, 32> &matrix, std::uint32_t value) noexcept {
            std::uint32_t result{};
            for (std::size_t bit = 0; value != 0; ++bit, value >>= 1U) {
                if ((value & 1U) != 0)
                    result ^= matrix[bit];
            }
            return result;
        }

        /** @brief Combines finalized CRCs without decoding skipped or compressed blocks. */
        [[nodiscard]] std::uint32_t Combine(std::uint32_t left, const std::uint32_t right, std::uint64_t bytes) noexcept {
            std::array<std::uint32_t, 32> matrix{};
            for (std::size_t bit = 0; bit < matrix.size(); ++bit) {
                std::uint32_t value = std::uint32_t{1} << bit;
                for (unsigned shift = 0; shift < 8; ++shift)
                    value = (value >> 1U) ^ ((value & 1U) != 0 ? 0xedb88320U : 0U);
                matrix[bit] = value;
            }
            while (bytes != 0) {
                if ((bytes & 1U) != 0)
                    left = Apply(matrix, left);
                std::array<std::uint32_t, 32> square{};
                for (std::size_t bit = 0; bit < matrix.size(); ++bit)
                    square[bit] = Apply(matrix, matrix[bit]);
                matrix = square;
                bytes >>= 1U;
            }
            return left ^ right;
        }

        /** @brief Checks authenticated fixed-header controls and produces typed wire facts. */
        [[nodiscard]] Result<StreamingCellHeaderView> ParseHeader(const std::span<const std::byte> bytes, const Sha256Digest &hash) {
            if (constexpr std::array magic{std::byte{'H'}, std::byte{'O'}, std::byte{'R'}, std::byte{'O'}, std::byte{'C'}, std::byte{'E'},
                                           std::byte{'L'}, std::byte{'L'}};
                !std::ranges::equal(bytes.first(8), magic) || Read<std::uint32_t>(bytes, 56) != HeaderBytes)
                return Fail<StreamingCellHeaderView>(WorldStreamingErrors::CellCandidateInvalid);
            const auto flags = Read<std::uint32_t>(bytes, 12);
            const auto codec = Read<std::uint32_t>(bytes, 32);
            if ((flags & ~7U) != 0 || (flags & 1U) != 0 || codec >= static_cast<unsigned>(StreamingCellCompression::Count))
                return Fail<StreamingCellHeaderView>(WorldStreamingErrors::CellCandidateUnsupported);
            if (bytes[29] != std::byte{0})
                return Fail<StreamingCellHeaderView>(WorldStreamingErrors::CellCandidateUnsupported);
            if (((flags & 2U) != 0) != (codec != 0))
                return Fail<StreamingCellHeaderView>(WorldStreamingErrors::CellCandidateInvalid);
            auto layer = StreamingLayerId::Create(Read<std::uint16_t>(bytes, 30));
            if (layer.HasError())
                return Fail<StreamingCellHeaderView>(WorldStreamingErrors::CellCandidateInvalid);
            StreamingCellHeaderView header{.majorVersion = Read<std::uint16_t>(bytes, 8),
                                           .minorVersion = Read<std::uint16_t>(bytes, 10),
                                           .cell = {std::bit_cast<std::int32_t>(Read<std::uint32_t>(bytes, 16)),
                                                    std::bit_cast<std::int32_t>(Read<std::uint32_t>(bytes, 20)),
                                                    std::bit_cast<std::int32_t>(Read<std::uint32_t>(bytes, 24)),
                                                    Read<std::uint8_t>(bytes, 28), layer.Value()},
                                           .compression = static_cast<StreamingCellCompression>(codec),
                                           .compressedSize = Read<std::uint64_t>(bytes, 48),
                                           .uncompressedSize = Read<std::uint64_t>(bytes, 40),
                                           .payloadCrc32 = Read<std::uint32_t>(bytes, 36),
                                           .artifactHash = hash,
                                           .payloads = {}};
            if (header.majorVersion != StreamingCellHeaderView::CurrentMajorVersion)
                return Fail<StreamingCellHeaderView>(WorldStreamingErrors::CellCandidateUnsupported);
            if (header.compressedSize != bytes.size() - HeaderBytes)
                return Fail<StreamingCellHeaderView>(WorldStreamingErrors::CellCandidateInvalid);
            return Result<StreamingCellHeaderView>::Success(header);
        }

        /** @brief Parses a bounded TOC row, rejecting unrepresentable security flags rather than dropping them. */
        [[nodiscard]] Result<StreamingCellPayloadHeader> ParseRow(const std::span<const std::byte> row) {
            const auto flags = Read<std::uint16_t>(row, 2);
            if ((flags & ~3U) != 0)
                return Fail<StreamingCellPayloadHeader>(WorldStreamingErrors::CellCandidateUnsupported);
            if ((flags != 1 && flags != 2) || Read<std::uint32_t>(row, 36) != 0)
                return Fail<StreamingCellPayloadHeader>(WorldStreamingErrors::CellCandidateInvalid);
            return Result<StreamingCellPayloadHeader>::Success(
                {static_cast<StreamingCellProvider>(Read<std::uint16_t>(row, 0)),
                 flags == 1 ? StreamingCellPayloadRequirement::Required : StreamingCellPayloadRequirement::Optional,
                 Read<std::uint32_t>(row, 4), Read<std::uint64_t>(row, 8), Read<std::uint64_t>(row, 16), Read<std::uint64_t>(row, 24),
                 Read<std::uint32_t>(row, 32)});
        }

        /** @brief Validates contained canonical ranges, zero padding and bounded aggregate size/CRC. */
        [[nodiscard]] Result<void> ValidateRanges(const std::span<const std::byte> bytes, const ParsedCellArtifact &parsed,
                                                  const CancellationToken &cancellation) {
            std::uint64_t end = HeaderBytes + parsed.payloads.size() * RowBytes;
            std::uint64_t decoded{};
            std::uint32_t crc{};
            for (const auto &row : parsed.payloads) {
                if (cancellation.IsCancellationRequested())
                    return Fail<void>(WorldStreamingErrors::CellCandidateLifecycleUnavailable);
                if (row.offset < end || row.offset - end > 7 || (row.offset & 7U) != 0 || row.offset > bytes.size() ||
                    row.compressedSize > bytes.size() - row.offset || row.uncompressedSize > parsed.header.uncompressedSize - decoded)
                    return Fail<void>(WorldStreamingErrors::CellCandidateInvalid);
                if (const auto padding = bytes.subspan(static_cast<std::size_t>(end), static_cast<std::size_t>(row.offset - end));
                    !std::ranges::all_of(padding, [](const std::byte value) {
                    return value == std::byte{0};
                }))
                    return Fail<void>(WorldStreamingErrors::CellCandidateInvalid);
                end = row.offset + row.compressedSize;
                decoded += row.uncompressedSize;
                crc = Combine(crc, row.payloadCrc32, row.uncompressedSize);
            }
            if (end != bytes.size() || decoded != parsed.header.uncompressedSize || crc != parsed.header.payloadCrc32)
                return Fail<void>(WorldStreamingErrors::CellCandidateInvalid);
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ParseCellArtifactBytes */
    Result<ParsedCellArtifact> ParseCellArtifactBytes(const std::span<const std::byte> artifact,
                                                      const StreamingCellCandidateContext &context, const Sha256Digest &expectedHash,
                                                      const CancellationToken &cancellation) {
        if (cancellation.IsCancellationRequested())
            return Fail<ParsedCellArtifact>(WorldStreamingErrors::CellCandidateLifecycleUnavailable);
        if (artifact.size() < HeaderBytes)
            return Fail<ParsedCellArtifact>(WorldStreamingErrors::CellCandidateInvalid);
        if (artifact.size() - HeaderBytes > context.maximumCompressedBytes)
            return Fail<ParsedCellArtifact>(WorldStreamingErrors::CellCandidateCapacityExceeded);
        auto hash = ArtifactHash(artifact, cancellation);
        if (hash.HasError())
            return Result<ParsedCellArtifact>::Failure(hash.ErrorValue());
        for (std::size_t index = 0; index < hash.Value().bytes.size(); ++index) {
            if (artifact[HashOffset + index] != static_cast<std::byte>(hash.Value().bytes[index]))
                return Fail<ParsedCellArtifact>(WorldStreamingErrors::CellCandidateInvalid);
        }
        if (hash.Value() != expectedHash)
            return Fail<ParsedCellArtifact>(WorldStreamingErrors::CellCandidateStale);
        auto header = ParseHeader(artifact, hash.Value());
        if (header.HasError())
            return Result<ParsedCellArtifact>::Failure(header.ErrorValue());
        const auto count = Read<std::uint32_t>(artifact, 60);
        if (count > context.maximumPayloads || header.Value().uncompressedSize > context.maximumUncompressedBytes)
            return Fail<ParsedCellArtifact>(WorldStreamingErrors::CellCandidateCapacityExceeded);
        if (count == 0 || count > (artifact.size() - HeaderBytes) / RowBytes)
            return Fail<ParsedCellArtifact>(WorldStreamingErrors::CellCandidateInvalid);
        ParsedCellArtifact parsed{header.Value(), {}};
        parsed.payloads.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            if (cancellation.IsCancellationRequested())
                return Fail<ParsedCellArtifact>(WorldStreamingErrors::CellCandidateLifecycleUnavailable);
            auto row = ParseRow(artifact.subspan(HeaderBytes + index * RowBytes, RowBytes));
            if (row.HasError())
                return Result<ParsedCellArtifact>::Failure(row.ErrorValue());
            parsed.payloads.push_back(row.Value());
        }
        if (const auto valid = ValidateRanges(artifact, parsed, cancellation); valid.HasError())
            return Result<ParsedCellArtifact>::Failure(valid.ErrorValue());
        return Result<ParsedCellArtifact>::Success(std::move(parsed));
    }
}  // namespace Horo::WorldStreaming::Detail
