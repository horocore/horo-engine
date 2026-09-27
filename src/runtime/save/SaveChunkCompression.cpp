#include "Horo/Runtime/Save/SaveArchiveFraming.h"
#include "Horo/Runtime/Save/SaveErrors.h"
#include "SaveChunkCompressionInternal.h"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <miniz.h>
#include <new>
#include <utility>

namespace Horo::Runtime {
    namespace {
        constexpr std::array<SaveChunkCodecCapability, 2> kCodecs{{
            {SaveChunkCodec::Raw, 0, 0, false},
            {SaveChunkCodec::Deflate, 1, 9, false},
        }};

        /** @brief Finds an installed stateless codec without consulting platform or module state. */
        [[nodiscard]] const SaveChunkCodecCapability *FindCodec(const SaveChunkCodec codec) noexcept {
            const auto found = std::ranges::find(kCodecs, codec, &SaveChunkCodecCapability::codec);
            return found == kCodecs.end() ? nullptr : std::to_address(found);
        }

        /** @brief Verifies every untrusted declared size before allocating decoded storage. */
        [[nodiscard]] Result<void> AdmitSizes(const SaveChunkDirectoryEntry &entry, const std::span<const std::byte> stored,
                                              const SaveChunkDirectoryLimits &limits) {
            if (entry.storedByteLength == 0 || entry.decodedByteLength == 0 || entry.storedByteLength != stored.size())
                return Result<void>::Failure(MakeError(SaveErrors::ArchivePayloadTruncated));
            if (limits.maximumStoredChunkBytes == 0 || limits.maximumDecodedChunkBytes == 0 || limits.maximumExpansionRatio == 0 ||
                entry.storedByteLength > limits.maximumStoredChunkBytes || entry.decodedByteLength > limits.maximumDecodedChunkBytes ||
                entry.decodedByteLength > std::numeric_limits<std::size_t>::max())
                return Result<void>::Failure(MakeError(SaveErrors::ArchiveDecompressionLimitExceeded));
            if (entry.codec == SaveChunkCodec::Raw) {
                if (entry.storedByteLength != entry.decodedByteLength)
                    return Result<void>::Failure(MakeError(SaveErrors::ArchiveEntryInvalid));
            } else if (entry.decodedByteLength / entry.storedByteLength > limits.maximumExpansionRatio ||
                       (entry.decodedByteLength / entry.storedByteLength == limits.maximumExpansionRatio &&
                        entry.decodedByteLength % entry.storedByteLength != 0)) {
                return Result<void>::Failure(MakeError(SaveErrors::ArchiveDecompressionLimitExceeded));
            }
            return Result<void>::Success();
        }

        /** @brief Copies a bounded raw chunk; raw and compressed selection share one owned-result contract. */
        [[nodiscard]] Result<std::vector<std::byte>> CopyRaw(const std::span<const std::byte> decoded) {
            try {
                return Result<std::vector<std::byte>>::Success(std::vector<std::byte>{decoded.begin(), decoded.end()});
            } catch (const std::bad_alloc &) {
                return Result<std::vector<std::byte>>::Failure(MakeError(SaveErrors::ArchiveAllocationFailed));
            }
        }

        /** @brief Preserves canonical digest and bytes when policy selects an uncompressed representation. */
        [[nodiscard]] Result<EncodedSaveChunk> EncodeRaw(const std::span<const std::byte> decoded, const SaveChunkDirectoryLimits &limits) {
            if (decoded.size() > limits.maximumStoredChunkBytes)
                return Result<EncodedSaveChunk>::Failure(MakeError(SaveErrors::ArchiveDecompressionLimitExceeded));
            auto stored = CopyRaw(decoded);
            if (stored.HasError())
                return Result<EncodedSaveChunk>::Failure(stored.ErrorValue());
            return Result<EncodedSaveChunk>::Success({.codec = SaveChunkCodec::Raw,
                                                      .stored = std::move(stored).Value(),
                                                      .decodedByteLength = decoded.size(),
                                                      .decodedHash = ComputeSha256(decoded)});
        }

        /** @brief Applies the pinned miniz implementation within a preallocated output ceiling. */
        [[nodiscard]] Result<std::vector<std::byte>> EncodeDeflate(const std::span<const std::byte> decoded, const std::uint8_t level,
                                                                   const SaveChunkDirectoryLimits &limits) {
            if (decoded.size() > std::numeric_limits<mz_ulong>::max())
                return Result<std::vector<std::byte>>::Failure(MakeError(SaveErrors::ArchiveDecompressionLimitExceeded));
            const auto bound = mz_compressBound(static_cast<mz_ulong>(decoded.size()));
            const auto capacity = std::min<std::uint64_t>(bound, limits.maximumStoredChunkBytes);
            if (capacity == 0 || capacity > std::numeric_limits<std::size_t>::max())
                return Result<std::vector<std::byte>>::Failure(MakeError(SaveErrors::ArchiveDecompressionLimitExceeded));
            try {
                std::vector<std::byte> stored(static_cast<std::size_t>(capacity));
                const auto flags = tdefl_create_comp_flags_from_zip_params(level, MZ_DEFAULT_WINDOW_BITS, MZ_DEFAULT_STRATEGY);
                const auto storedLength = tdefl_compress_mem_to_mem(stored.data(), stored.size(), decoded.data(), decoded.size(), flags);
                if (storedLength == 0)
                    return Result<std::vector<std::byte>>::Failure(
                        MakeError(capacity < bound ? SaveErrors::ArchiveDecompressionLimitExceeded : SaveErrors::ArchiveCompressionFailed));
                stored.resize(static_cast<std::size_t>(storedLength));
                return Result<std::vector<std::byte>>::Success(std::move(stored));
            } catch (const std::bad_alloc &) {
                return Result<std::vector<std::byte>>::Failure(MakeError(SaveErrors::ArchiveAllocationFailed));
            }
        }

        struct DecodedOutput final {
            std::span<std::byte> bytes;
            std::size_t written{};
        };

        /** @brief Copies miniz output only while it fits the pre-admitted decoded length. */
        int CopyDecodedBytes(const void *source, const int length, void *context) noexcept {
            auto &output = *static_cast<DecodedOutput *>(context);
            if (length < 0 || (length > 0 && source == nullptr) || output.written > output.bytes.size() ||
                static_cast<std::size_t>(length) > output.bytes.size() - output.written)
                return 0;
            const std::span input{static_cast<const std::byte *>(source), static_cast<std::size_t>(length)};
            const auto destination = output.bytes.subspan(output.written, input.size());
            std::ranges::copy(input, destination.begin());
            output.written += static_cast<std::size_t>(length);
            return 1;
        }

        /** @brief Compares an exact decoded/stored ratio without overflowing multiplication. */
        [[nodiscard]] bool ExceedsRatio(const std::uint64_t decoded, const std::uint64_t stored,
                                        const std::uint64_t maximumRatio) noexcept {
            return decoded / stored > maximumRatio || (decoded / stored == maximumRatio && decoded % stored != 0);
        }
    }  // namespace

    /** @copydoc InstalledSaveChunkCodecs */
    std::span<const SaveChunkCodecCapability> InstalledSaveChunkCodecs() noexcept {
        return kCodecs;
    }

    namespace SaveChunkCompressionDetail {
        bool Supports(const SaveChunkCodec codec) noexcept {
            return FindCodec(codec) != nullptr;
        }

        Result<std::vector<std::byte>> Decode(const SaveChunkDirectoryEntry &entry, const std::span<const std::byte> stored,
                                              const SaveChunkDirectoryLimits &limits) {
            if (!Supports(entry.codec))
                return Result<std::vector<std::byte>>::Failure(MakeError(SaveErrors::ArchiveCodecUnsupported));
            if (const auto admitted = AdmitSizes(entry, stored, limits); admitted.HasError())
                return Result<std::vector<std::byte>>::Failure(admitted.ErrorValue());
            if (entry.codec == SaveChunkCodec::Raw)
                return CopyRaw(stored);
            if (stored.size() > std::numeric_limits<mz_ulong>::max() || entry.decodedByteLength > std::numeric_limits<mz_ulong>::max())
                return Result<std::vector<std::byte>>::Failure(MakeError(SaveErrors::ArchiveDecompressionLimitExceeded));
            try {
                std::vector<std::byte> decoded(static_cast<std::size_t>(entry.decodedByteLength));
                DecodedOutput output{decoded};
                auto storedLength = stored.size();
                if (const int status = tinfl_decompress_mem_to_callback(stored.data(), &storedLength, CopyDecodedBytes, &output,
                                                                        TINFL_FLAG_PARSE_ZLIB_HEADER);
                    status != 1 || output.written != decoded.size() || storedLength != stored.size())
                    return Result<std::vector<std::byte>>::Failure(MakeError(SaveErrors::ArchiveChunkDecodeFailed));
                return Result<std::vector<std::byte>>::Success(std::move(decoded));
            } catch (const std::bad_alloc &) {
                return Result<std::vector<std::byte>>::Failure(MakeError(SaveErrors::ArchiveAllocationFailed));
            }
        }
    }  // namespace SaveChunkCompressionDetail

    /** @copydoc EncodeSaveChunk */
    Result<EncodedSaveChunk> EncodeSaveChunk(const std::span<const std::byte> decoded, const SaveChunkCompressionPolicy &policy,
                                             const SaveChunkDirectoryLimits &limits) {
        if (decoded.empty() || limits.maximumStoredChunkBytes == 0 || limits.maximumDecodedChunkBytes == 0 ||
            limits.maximumExpansionRatio == 0 || decoded.size() > limits.maximumDecodedChunkBytes)
            return Result<EncodedSaveChunk>::Failure(MakeError(SaveErrors::ArchiveDecompressionLimitExceeded));
        const auto *codec = FindCodec(policy.preferred);
        if (codec == nullptr && policy.required && !policy.metadata)
            return Result<EncodedSaveChunk>::Failure(MakeError(SaveErrors::ArchiveCodecUnsupported));
        if (!policy.metadata && codec != nullptr && policy.preferred != SaveChunkCodec::Raw &&
            (policy.level < codec->minimumLevel || policy.level > codec->maximumLevel))
            return Result<EncodedSaveChunk>::Failure(MakeError(SaveErrors::ArchiveCompressionPolicyInvalid));

        if (policy.metadata || policy.preferred == SaveChunkCodec::Raw || codec == nullptr || decoded.size() < policy.minimumByteLength) {
            if (policy.required && policy.preferred != SaveChunkCodec::Raw && !policy.metadata)
                return Result<EncodedSaveChunk>::Failure(MakeError(SaveErrors::ArchiveCompressionPolicyInvalid));
            return EncodeRaw(decoded, limits);
        }
        auto compressed = EncodeDeflate(decoded, policy.level, limits);
        if (compressed.HasError()) {
            if (!policy.required && compressed.ErrorValue().code.Value() == SaveErrors::ArchiveDecompressionLimitExceeded.code.Value())
                return EncodeRaw(decoded, limits);
            return Result<EncodedSaveChunk>::Failure(compressed.ErrorValue());
        }
        if (compressed.Value().size() >= decoded.size() ||
            ExceedsRatio(decoded.size(), compressed.Value().size(), limits.maximumExpansionRatio)) {
            if (policy.required)
                return Result<EncodedSaveChunk>::Failure(MakeError(SaveErrors::ArchiveDecompressionLimitExceeded));
            return EncodeRaw(decoded, limits);
        }
        return Result<EncodedSaveChunk>::Success({.codec = SaveChunkCodec::Deflate,
                                                  .stored = std::move(compressed).Value(),
                                                  .decodedByteLength = decoded.size(),
                                                  .decodedHash = ComputeSha256(decoded)});
    }
}  // namespace Horo::Runtime
