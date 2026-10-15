#include "Horo/WorldStreaming/StreamingCellEncoding.h"

#include "Horo/WorldStreaming/WorldStreamingErrors.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>

namespace Horo::WorldStreaming {
    namespace {
        constexpr std::size_t HeaderBytes = 96;
        constexpr std::size_t RowBytes = 40;
        constexpr std::size_t WorkBytes = 65536;

        /** @brief Preserves the existing cell error domain at the encoding boundary. */
        template <typename T> Result<T> Reject(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Writes a scalar into an already bounded, zero-initialized wire record. */
        template <typename T> void Write(std::vector<std::byte> &bytes, const std::size_t offset, const T value) noexcept {
            for (std::size_t index = 0; index < sizeof(T); ++index)
                bytes[offset + index] = static_cast<std::byte>((static_cast<std::uint64_t>(value) >> (index * 8U)) & 0xffU);
        }

        /** @brief Updates reflected CRC-32/ISO-HDLC without provider calls or native representation access. */
        std::uint32_t UpdateCrc(std::uint32_t crc, const std::span<const std::byte> bytes) noexcept {
            for (const auto byte : bytes) {
                crc ^= std::to_integer<std::uint8_t>(byte);
                for (unsigned bit = 0; bit < 8; ++bit)
                    crc = (crc >> 1U) ^ ((crc & 1U) != 0 ? 0xedb88320U : 0U);
            }
            return crc;
        }

        /** @brief Bounds sorting storage before copying any submitted payload records. */
        Result<void> AdmitEnvelope(const StreamingCellId &cell, std::span<const StreamingCellPayloadBytes> payloads,
                                   const StreamingCellEncodingLimits &limits) {
            if (!cell.IsValid() || !limits.maximumPayloads || !limits.maximumArtifactBytes || !limits.maximumDecodedBytes ||
                payloads.empty())
                return Reject<void>(WorldStreamingErrors::CellCandidateInvalid);
            if (limits.compression != StreamingCellCompression::None)
                return Reject<void>(WorldStreamingErrors::CellCandidateUnsupported);
            if (payloads.size() > limits.maximumPayloads || limits.maximumArtifactBytes < HeaderBytes ||
                payloads.size() > (limits.maximumArtifactBytes - HeaderBytes) / RowBytes)
                return Reject<void>(WorldStreamingErrors::CellCandidateCapacityExceeded);
            return Result<void>::Success();
        }

        /** @brief Validates canonical provider rows and exact byte ceilings before artifact allocation. */
        Result<std::size_t> Admit(std::span<const StreamingCellPayloadBytes> payloads, const StreamingCellEncodingLimits &limits) {
            std::size_t size = HeaderBytes + payloads.size() * RowBytes;
            std::size_t decoded{};
            std::size_t cores{};
            for (const auto &payload : payloads) {
                if (const auto provider = static_cast<std::uint16_t>(payload.provider);
                    !provider || (provider > 7 && provider < 0x8000) || !payload.version || payload.bytes.empty() ||
                    payload.requirement >= StreamingCellPayloadRequirement::Count)
                    return Reject<std::size_t>(WorldStreamingErrors::CellCandidateInvalid);
                if (payload.provider == StreamingCellProvider::CoreEcs) {
                    ++cores;
                    if (payload.requirement != StreamingCellPayloadRequirement::Required)
                        return Reject<std::size_t>(WorldStreamingErrors::CellCandidateInvalid);
                }
                if (size > std::numeric_limits<std::size_t>::max() - 7)
                    return Reject<std::size_t>(WorldStreamingErrors::CellCandidateCapacityExceeded);
                size = (size + 7) & ~std::size_t{7};
                if (size > limits.maximumArtifactBytes || payload.bytes.size() > limits.maximumArtifactBytes - size ||
                    payload.bytes.size() > limits.maximumDecodedBytes - decoded)
                    return Reject<std::size_t>(WorldStreamingErrors::CellCandidateCapacityExceeded);
                size += payload.bytes.size();
                decoded += payload.bytes.size();
            }
            if (cores != 1)
                return Reject<std::size_t>(WorldStreamingErrors::CellCandidateInvalid);
            return Result<std::size_t>::Success(size);
        }

        /** @brief Initializes canonical fixed controls; zero storage supplies all reserved fields. */
        void WriteHeader(EncodedStreamingCell &result, const StreamingCellId cell, const std::size_t count) {
            auto &bytes = result.bytes;
            constexpr std::array magic{std::byte{'H'}, std::byte{'O'}, std::byte{'R'}, std::byte{'O'},
                                       std::byte{'C'}, std::byte{'E'}, std::byte{'L'}, std::byte{'L'}};
            std::ranges::copy(magic, bytes.begin());
            Write<std::uint16_t>(bytes, 8, 1);
            Write(bytes, 16, std::bit_cast<std::uint32_t>(cell.x));
            Write(bytes, 20, std::bit_cast<std::uint32_t>(cell.y));
            Write(bytes, 24, std::bit_cast<std::uint32_t>(cell.z));
            Write(bytes, 28, cell.lod);
            Write(bytes, 30, cell.layer.Value());
            Write<std::uint32_t>(bytes, 56, HeaderBytes);
            Write(bytes, 60, static_cast<std::uint32_t>(count));
        }

        /** @brief Copies and checksums one provider stream in cancellable bounded units. */
        Result<std::uint32_t> CopyPayload(std::vector<std::byte> &bytes, const std::size_t destination,
                                          const std::span<const std::byte> sourceBytes, std::uint32_t &aggregate,
                                          const CancellationToken &cancellation) {
            std::uint32_t crc = 0xffffffffU;
            for (std::size_t source = 0; source < sourceBytes.size();) {
                if (cancellation.IsCancellationRequested())
                    return Reject<std::uint32_t>(WorldStreamingErrors::CellCandidateLifecycleUnavailable);
                const auto block = sourceBytes.subspan(source, std::min(WorkBytes, sourceBytes.size() - source));
                std::ranges::copy(block, bytes.begin() + static_cast<std::ptrdiff_t>(destination + source));
                crc = UpdateCrc(crc, block);
                aggregate = UpdateCrc(aggregate, block);
                source += block.size();
            }
            return Result<std::uint32_t>::Success(crc ^ 0xffffffffU);
        }

        /** @brief Writes canonical provider rows and accumulates decoded integrity without provider callbacks. */
        Result<void> WritePayloads(EncodedStreamingCell &result, const std::span<const StreamingCellPayloadBytes> ordered,
                                   const CancellationToken &cancellation) {
            auto &bytes = result.bytes;
            std::size_t offset = HeaderBytes + ordered.size() * RowBytes;
            std::uint32_t aggregate = 0xffffffffU;
            for (std::size_t index = 0; index < ordered.size(); ++index) {
                const auto &payload = ordered[index];
                offset = (offset + 7) & ~std::size_t{7};
                const auto row = HeaderBytes + index * RowBytes;
                Write(bytes, row, static_cast<std::uint16_t>(payload.provider));
                Write<std::uint16_t>(bytes, row + 2, payload.requirement == StreamingCellPayloadRequirement::Required ? 1 : 2);
                Write(bytes, row + 4, payload.version);
                Write<std::uint64_t>(bytes, row + 8, offset);
                Write<std::uint64_t>(bytes, row + 16, payload.bytes.size());
                Write<std::uint64_t>(bytes, row + 24, payload.bytes.size());
                auto crc = CopyPayload(bytes, offset, payload.bytes, aggregate, cancellation);
                if (crc.HasError())
                    return Result<void>::Failure(crc.ErrorValue());
                Write(bytes, row + 32, crc.Value());
                result.integrity.uncompressedSize += payload.bytes.size();
                offset += payload.bytes.size();
            }
            result.integrity.payloadCrc32 = aggregate ^ 0xffffffffU;
            return Result<void>::Success();
        }

        /** @brief Seals aggregate sizes/CRC and canonical artifact SHA while its digest slot remains zero. */
        Result<void> Seal(EncodedStreamingCell &result, const CancellationToken &cancellation) {
            auto &bytes = result.bytes;
            result.integrity.compressedSize = bytes.size() - HeaderBytes;
            Write(bytes, 36, result.integrity.payloadCrc32);
            Write(bytes, 40, result.integrity.uncompressedSize);
            Write(bytes, 48, result.integrity.compressedSize);
            Sha256Builder hash;
            for (std::size_t position = 0; position < bytes.size();) {
                if (cancellation.IsCancellationRequested())
                    return Reject<void>(WorldStreamingErrors::CellCandidateLifecycleUnavailable);
                const auto block = std::span<const std::byte>{bytes}.subspan(position, std::min(WorkBytes, bytes.size() - position));
                if (!hash.Update(block))
                    return Reject<void>(WorldStreamingErrors::CellCandidateCapacityExceeded);
                position += block.size();
            }
            result.integrity.artifactHash = hash.Finalize();
            for (std::size_t index = 0; index < result.integrity.artifactHash.bytes.size(); ++index)
                bytes[64 + index] = static_cast<std::byte>(result.integrity.artifactHash.bytes[index]);
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc EncodeStreamingCellArtifact */
    Result<EncodedStreamingCell> EncodeStreamingCellArtifact(const StreamingCellId cell,
                                                             const std::span<const StreamingCellPayloadBytes> payloads,
                                                             const StreamingCellEncodingLimits &limits,
                                                             const CancellationToken &cancellation) {
        if (cancellation.IsCancellationRequested())
            return Reject<EncodedStreamingCell>(WorldStreamingErrors::CellCandidateLifecycleUnavailable);
        if (auto admitted = AdmitEnvelope(cell, payloads, limits); admitted.HasError())
            return Result<EncodedStreamingCell>::Failure(admitted.ErrorValue());
        std::vector<StreamingCellPayloadBytes> ordered(payloads.begin(), payloads.end());
        std::ranges::sort(ordered, {}, &StreamingCellPayloadBytes::provider);
        if (std::ranges::adjacent_find(ordered, {}, &StreamingCellPayloadBytes::provider) != ordered.end())
            return Reject<EncodedStreamingCell>(WorldStreamingErrors::CellCandidateInvalid);
        auto admitted = Admit(ordered, limits);
        if (admitted.HasError())
            return Result<EncodedStreamingCell>::Failure(admitted.ErrorValue());
        EncodedStreamingCell result{std::vector<std::byte>(admitted.Value()), {.cell = cell}};
        WriteHeader(result, cell, ordered.size());
        if (auto copied = WritePayloads(result, ordered, cancellation); copied.HasError())
            return Result<EncodedStreamingCell>::Failure(copied.ErrorValue());
        if (auto sealed = Seal(result, cancellation); sealed.HasError())
            return Result<EncodedStreamingCell>::Failure(sealed.ErrorValue());
        return Result<EncodedStreamingCell>::Success(std::move(result));
    }
}  // namespace Horo::WorldStreaming
