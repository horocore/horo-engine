#include "Horo/Runtime/Render/PipelineCache.h"

#include "Horo/Runtime/Render/PipelineCacheErrors.h"

#include <algorithm>
#include <array>
#include <limits>
#include <type_traits>

namespace Horo::Render {
    namespace {
        constexpr std::uint32_t CurrentSchemaVersion = 1;
        constexpr std::array<std::uint8_t, 8> BlobMagic{'H', 'O', 'R', 'O', 'P', 'I', 'P', 'E'};
        constexpr std::size_t BlobHeaderBytes = BlobMagic.size() + sizeof(std::uint32_t) + 32U + 32U + sizeof(std::uint64_t);

        [[nodiscard]] bool HasValue(const Sha256Digest &digest) noexcept {
            return std::ranges::any_of(digest.bytes, [](const std::uint8_t byte) {
                return byte != 0;
            });
        }

        [[nodiscard]] bool IsValidIdentity(const std::string_view value, const std::size_t maximumBytes) noexcept {
            return !value.empty() && value.size() <= maximumBytes && std::ranges::all_of(value, [](const unsigned char character) {
                return character >= 0x20U && character <= 0x7eU;
            });
        }

        [[nodiscard]] bool IsKnown(const ShaderTargetBackend backend) noexcept {
            return static_cast<std::uint8_t>(backend) <= static_cast<std::uint8_t>(ShaderTargetBackend::D3D12);
        }

        [[nodiscard]] bool IsKnown(const ShaderPayloadFormat format) noexcept {
            return static_cast<std::uint8_t>(format) <= static_cast<std::uint8_t>(ShaderPayloadFormat::Dxil60);
        }

        [[nodiscard]] bool IsValidCompatibility(const PipelineCacheCompatibility &compatibility,
                                                const PipelineCacheLimits &limits) noexcept {
            return compatibility.schemaVersion == CurrentSchemaVersion && compatibility.backend.IsValid() &&
                   compatibility.backendModuleVersion > 0 && compatibility.adapter.IsValid() &&
                   HasValue(compatibility.deviceCompatibilityDigest) &&
                   IsValidIdentity(compatibility.driverIdentity, limits.maximumIdentityBytes) &&
                   IsValidIdentity(compatibility.driverVersion, limits.maximumIdentityBytes) && IsKnown(compatibility.shaderBackend) &&
                   IsKnown(compatibility.shaderFormat) && HasValue(compatibility.shaderArtifactKey) &&
                   HasValue(compatibility.shaderInterface.digest) && HasValue(compatibility.pipelineDescriptorDigest);
        }

        template <typename OutputByteT, typename ValueT> void AppendInteger(std::vector<OutputByteT> &output, ValueT value) {
            using UnsignedT = std::make_unsigned_t<ValueT>;
            UnsignedT bits = static_cast<UnsignedT>(value);
            for (std::size_t index = 0; index < sizeof(UnsignedT); ++index) {
                output.push_back(static_cast<OutputByteT>(bits & 0xffU));
                if constexpr (sizeof(UnsignedT) > 1U)
                    bits >>= 8U;
            }
        }

        void AppendString(std::vector<std::byte> &output, const std::string_view value) {
            AppendInteger(output, static_cast<std::uint32_t>(value.size()));
            output.insert(output.end(), reinterpret_cast<const std::byte *>(value.data()),
                          reinterpret_cast<const std::byte *>(value.data() + value.size()));
        }

        template <typename OutputByteT> void AppendDigest(std::vector<OutputByteT> &output, const Sha256Digest &digest) {
            for (const std::uint8_t byte : digest.bytes)
                output.push_back(static_cast<OutputByteT>(byte));
        }

        template <typename ValueT> [[nodiscard]] ValueT ReadInteger(const std::span<const std::uint8_t> input, std::size_t &offset) {
            using UnsignedT = std::make_unsigned_t<ValueT>;
            UnsignedT value = 0;
            for (std::size_t index = 0; index < sizeof(UnsignedT); ++index)
                value |= static_cast<UnsignedT>(input[offset++]) << (index * 8U);
            return static_cast<ValueT>(value);
        }

        [[nodiscard]] Sha256Digest ReadDigest(const std::span<const std::uint8_t> input, std::size_t &offset) {
            Sha256Digest digest;
            std::ranges::copy(input.subspan(offset, digest.bytes.size()), digest.bytes.begin());
            offset += digest.bytes.size();
            return digest;
        }
    }  // namespace

    Result<PipelineCacheKey> ComputePipelineCacheKey(const PipelineCacheCompatibility &compatibility, const PipelineCacheLimits &limits) {
        if (!limits.IsValid())
            return Result<PipelineCacheKey>::Failure(MakeError(PipelineCacheErrors::InvalidLimits));
        if (!IsValidCompatibility(compatibility, limits))
            return Result<PipelineCacheKey>::Failure(MakeError(PipelineCacheErrors::InvalidCompatibility));
        try {
            std::vector<std::byte> bytes;
            bytes.reserve(256U + compatibility.backend.Value().size() + compatibility.adapter.Value().size() +
                          compatibility.driverIdentity.size() + compatibility.driverVersion.size());
            AppendString(bytes, "horo.pipeline-cache-compatibility.v1");
            AppendInteger(bytes, compatibility.schemaVersion);
            AppendString(bytes, compatibility.backend.Value());
            AppendInteger(bytes, compatibility.backendModuleVersion);
            AppendString(bytes, compatibility.adapter.Value());
            AppendDigest(bytes, compatibility.deviceCompatibilityDigest);
            AppendString(bytes, compatibility.driverIdentity);
            AppendString(bytes, compatibility.driverVersion);
            AppendInteger(bytes, static_cast<std::uint8_t>(compatibility.shaderBackend));
            AppendInteger(bytes, static_cast<std::uint8_t>(compatibility.shaderFormat));
            AppendDigest(bytes, compatibility.shaderArtifactKey);
            AppendDigest(bytes, compatibility.shaderInterface.digest);
            AppendDigest(bytes, compatibility.pipelineDescriptorDigest);
            return Result<PipelineCacheKey>::Success({ComputeSha256(bytes)});
        } catch (const std::bad_alloc &) {
            return Result<PipelineCacheKey>::Failure(MakeError(PipelineCacheErrors::IdentityUnavailable));
        }
    }

    Result<std::vector<std::uint8_t>> SerializePipelineCacheBlob(const PipelineCacheKey &key, const std::span<const std::uint8_t> payload,
                                                                 const PipelineCacheLimits &limits) {
        if (!limits.IsValid())
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(PipelineCacheErrors::InvalidLimits));
        if (!HasValue(key.digest))
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(PipelineCacheErrors::InvalidCompatibility));
        if (payload.empty() || payload.size() > limits.maximumPayloadBytes)
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(PipelineCacheErrors::PayloadTooLarge));
        try {
            std::vector<std::uint8_t> output;
            output.reserve(BlobHeaderBytes + payload.size());
            output.insert(output.end(), BlobMagic.begin(), BlobMagic.end());
            AppendInteger(output, CurrentSchemaVersion);
            AppendDigest(output, key.digest);
            AppendDigest(output, ComputeSha256(std::as_bytes(payload)));
            AppendInteger(output, static_cast<std::uint64_t>(payload.size()));
            output.insert(output.end(), payload.begin(), payload.end());
            return Result<std::vector<std::uint8_t>>::Success(std::move(output));
        } catch (const std::bad_alloc &) {
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(PipelineCacheErrors::AllocationFailed));
        }
    }

    Result<std::vector<std::uint8_t>> LoadPipelineCacheBlob(const PipelineCacheKey &expectedKey,
                                                            const std::span<const std::uint8_t> serialized,
                                                            const PipelineCacheLimits &limits) {
        if (!limits.IsValid())
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(PipelineCacheErrors::InvalidLimits));
        if (!HasValue(expectedKey.digest))
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(PipelineCacheErrors::InvalidCompatibility));
        if (serialized.size() < BlobHeaderBytes || !std::ranges::equal(BlobMagic, serialized.first(BlobMagic.size())))
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(PipelineCacheErrors::CorruptBlob));
        std::size_t offset = BlobMagic.size();
        if (ReadInteger<std::uint32_t>(serialized, offset) != CurrentSchemaVersion)
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(PipelineCacheErrors::UnsupportedVersion));
        const Sha256Digest storedKey = ReadDigest(serialized, offset);
        const Sha256Digest payloadDigest = ReadDigest(serialized, offset);
        const std::uint64_t payloadSize = ReadInteger<std::uint64_t>(serialized, offset);
        if (payloadSize > limits.maximumPayloadBytes)
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(PipelineCacheErrors::PayloadTooLarge));
        if (payloadSize == 0 || payloadSize > std::numeric_limits<std::size_t>::max() ||
            static_cast<std::size_t>(payloadSize) != serialized.size() - offset)
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(PipelineCacheErrors::CorruptBlob));
        if (storedKey != expectedKey.digest)
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(PipelineCacheErrors::IncompatibleBlob));
        const std::span<const std::uint8_t> payload = serialized.subspan(offset);
        if (ComputeSha256(std::as_bytes(payload)) != payloadDigest)
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(PipelineCacheErrors::CorruptBlob));
        try {
            return Result<std::vector<std::uint8_t>>::Success(std::vector<std::uint8_t>{payload.begin(), payload.end()});
        } catch (const std::bad_alloc &) {
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(PipelineCacheErrors::AllocationFailed));
        }
    }
}  // namespace Horo::Render
