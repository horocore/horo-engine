#include "../AssetErrors.h"
#include "Horo/Assets/AssetCook.h"
#include "Horo/Foundation/Sha256.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

using namespace Horo::Assets::CookErrors;

namespace Horo::Assets {
    namespace {
        constexpr std::array<char, 8> Magic{'H', 'O', 'R', 'O', 'A', 'S', 'T', '\0'};
        constexpr std::uint32_t CurrentFormatVersion = 1;
        constexpr std::size_t FixedHeaderSize = 8 + 4 + 2 + 2 + 16 + 32 + 32 + 32 + 8;

        void WriteU32LE(std::vector<std::uint8_t> &out, const std::uint32_t value) {
            out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
            out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
            out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
            out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
        }

        void WriteU16LE(std::vector<std::uint8_t> &out, const std::uint16_t value) {
            out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
            out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
        }

        void WriteU64LE(std::vector<std::uint8_t> &out, const std::uint64_t value) {
            for (int shift = 0; shift < 64; shift += 8)
                out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
        }

        void WriteDigest(std::vector<std::uint8_t> &out, const Sha256Digest &digest) {
            for (const auto b : digest.bytes)
                out.push_back(b);
        }

        [[nodiscard]] std::uint32_t ReadU32LE(const std::span<const std::byte> bytes, const std::size_t offset) {
            return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1]) << 8U) | (std::uint32_t(bytes[offset + 2]) << 16U) |
                   (std::uint32_t(bytes[offset + 3]) << 24U);
        }

        [[nodiscard]] std::uint16_t ReadU16LE(const std::span<const std::byte> bytes, const std::size_t offset) {
            return std::uint16_t(std::uint16_t(bytes[offset]) | (std::uint16_t(bytes[offset + 1]) << 8U));
        }

        [[nodiscard]] std::uint64_t ReadU64LE(const std::span<const std::byte> bytes, const std::size_t offset) {
            std::uint64_t value = 0;
            for (int shift = 0; shift < 64; shift += 8)
                value |= std::uint64_t(bytes[offset + shift / 8]) << shift;
            return value;
        }

        [[nodiscard]] Sha256Digest ReadDigest(const std::span<const std::byte> bytes, const std::size_t offset) {
            Sha256Digest digest{};
            for (std::size_t i = 0; i < digest.bytes.size(); ++i)
                digest.bytes[i] = std::to_integer<std::uint8_t>(bytes[offset + i]);
            return digest;
        }

        [[nodiscard]] Result<AssetCookArtifact> MakeMalformedError() {
            return Result<AssetCookArtifact>::Failure(MakeError(MalformedArtifact, "Cooked artifact is malformed."));
        }
    }  // namespace

    /** @copydoc EncodeCookedArtifact */
    Result<std::vector<std::uint8_t>> EncodeCookedArtifact(const AssetCookArtifact &artifact, const AssetCookLimits &limits) {
        const auto &targetText = artifact.target.Value();
        const auto &typeText = artifact.type.Value();
        if (targetText.size() > 65535 || typeText.size() > 65535)
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(TooLarge, "Cooked artifact target/type text exceeds bounds."));

        const std::size_t totalSize = FixedHeaderSize + targetText.size() + typeText.size() + artifact.payload.size();
        if (totalSize > limits.maximumArtifactBytes)
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(TooLarge, "Cooked artifact exceeds the maximum artifact size."));

        std::vector<std::uint8_t> out;
        out.reserve(totalSize);
        for (const auto c : Magic)
            out.push_back(static_cast<std::uint8_t>(c));
        WriteU32LE(out, CurrentFormatVersion);
        WriteU16LE(out, static_cast<std::uint16_t>(targetText.size()));
        WriteU16LE(out, static_cast<std::uint16_t>(typeText.size()));
        for (const auto b : artifact.id.Bytes())
            out.push_back(b);
        WriteDigest(out, artifact.cacheKeyDigest);
        WriteDigest(out, artifact.sourceDigest);
        WriteDigest(out, artifact.payloadDigest);
        WriteU64LE(out, artifact.payload.size());
        for (const auto c : targetText)
            out.push_back(static_cast<std::uint8_t>(c));
        for (const auto c : typeText)
            out.push_back(static_cast<std::uint8_t>(c));
        for (const auto b : artifact.payload)
            out.push_back(b);
        return Result<std::vector<std::uint8_t>>::Success(std::move(out));
    }

    /** @copydoc DecodeCookedArtifact */
    Result<AssetCookArtifact> DecodeCookedArtifact(const std::span<const std::uint8_t> bytes, const AssetCookLimits &limits) {
        return DecodeCookedArtifactBytes(std::as_bytes(bytes), limits);
    }

    /** @copydoc DecodeCookedArtifactBytes */
    Result<AssetCookArtifact> DecodeCookedArtifactBytes(const std::span<const std::byte> bytes, const AssetCookLimits &limits) {
        if (bytes.size() < FixedHeaderSize || bytes.size() > limits.maximumArtifactBytes)
            return MakeMalformedError();

        for (std::size_t i = 0; i < Magic.size(); ++i)
            if (bytes[i] != static_cast<std::byte>(Magic[i]))
                return MakeMalformedError();

        if (const auto version = ReadU32LE(bytes, 8); version != CurrentFormatVersion)
            return Result<AssetCookArtifact>::Failure(MakeError(UnsupportedFormat, "Cooked artifact format version is unsupported."));

        const auto targetLen = ReadU16LE(bytes, 12);
        const auto typeLen = ReadU16LE(bytes, 14);
        const std::size_t headerEnd = FixedHeaderSize + targetLen + typeLen;
        const auto payloadSize = ReadU64LE(bytes, FixedHeaderSize - 8);
        if (headerEnd + payloadSize != bytes.size())
            return MakeMalformedError();

        if (payloadSize > limits.maximumArtifactBytes)
            return Result<AssetCookArtifact>::Failure(MakeError(TooLarge, "Cooked artifact payload exceeds the maximum size."));

        std::string targetText(reinterpret_cast<const char *>(bytes.data() + FixedHeaderSize), targetLen);
        std::string typeText(reinterpret_cast<const char *>(bytes.data() + FixedHeaderSize + targetLen), typeLen);

        auto targetResult = AssetCookTargetId::Parse(targetText);
        if (targetResult.HasError())
            return MakeMalformedError();

        auto typeResult = AssetTypeId::Parse(typeText);
        if (typeResult.HasError())
            return MakeMalformedError();

        AssetCookArtifact artifact;
        std::array<std::uint8_t, 16> idBytes{};
        for (std::size_t i = 0; i < 16; ++i)
            idBytes[i] = std::to_integer<std::uint8_t>(bytes[16 + i]);
        artifact.id = AssetId::FromBytes(idBytes);
        artifact.type = typeResult.Value();
        artifact.target = targetResult.Value();
        artifact.cacheKeyDigest = ReadDigest(bytes, 32);
        artifact.sourceDigest = ReadDigest(bytes, 64);
        artifact.payloadDigest = ReadDigest(bytes, 96);
        artifact.payload.resize(payloadSize);
        if (payloadSize > 0)
            std::memcpy(artifact.payload.data(), bytes.data() + headerEnd, payloadSize);

        if (const auto computedDigest = ComputeSha256(std::as_bytes(std::span<const std::uint8_t>(artifact.payload)));
            computedDigest != artifact.payloadDigest)
            return Result<AssetCookArtifact>::Failure(MakeError(HashMismatch, "Cooked artifact payload digest does not match."));

        return Result<AssetCookArtifact>::Success(std::move(artifact));
    }
}  // namespace Horo::Assets
