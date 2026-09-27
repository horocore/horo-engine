#include "UpdateSignatureCodec.h"

#include "Horo/Release/DistributionModel.h"

#include <array>
#include <cstdint>

namespace Horo::Release::Detail {
    namespace {
        constexpr std::size_t SignatureBytes = 64U;
        constexpr std::array ProductNames{
            std::pair{DistributionProductKind::Editor, std::string_view{"editor"}},
            std::pair{DistributionProductKind::EngineCli, std::string_view{"engine-cli"}},
            std::pair{DistributionProductKind::PackageToolCli, std::string_view{"package-tool-cli"}},
            std::pair{DistributionProductKind::PublicSdk, std::string_view{"public-sdk"}},
            std::pair{DistributionProductKind::RendererComponent, std::string_view{"renderer-component"}},
            std::pair{DistributionProductKind::GameRuntime, std::string_view{"game-runtime"}},
            std::pair{DistributionProductKind::GameDedicatedServer, std::string_view{"game-dedicated-server"}},
        };

        [[nodiscard]] int HexValue(const char character) {
            if (character >= '0' && character <= '9')
                return character - '0';
            if (character >= 'a' && character <= 'f')
                return character - 'a' + 10;
            return -1;
        }
    }  // namespace

    std::string_view ProductName(const DistributionProductKind kind) {
        for (const auto &[candidate, name] : ProductNames)
            if (candidate == kind)
                return name;
        return {};
    }

    bool ParseProductKind(const std::string_view text, DistributionProductKind &kind) {
        for (const auto &[candidate, name] : ProductNames) {
            if (name == text) {
                kind = candidate;
                return true;
            }
        }
        return false;
    }

    std::string FormatHex(const std::span<const std::byte> bytes) {
        std::string result;
        result.reserve(bytes.size() * 2U);
        for (const std::byte value : bytes) {
            result.push_back("0123456789abcdef"[std::to_integer<unsigned>(value >> 4U)]);
            result.push_back("0123456789abcdef"[std::to_integer<unsigned>(value & std::byte{0x0f})]);
        }
        return result;
    }

    bool ParseHex(const std::string_view text, const std::size_t expectedBytes, std::vector<std::byte> &bytes) {
        if (text.size() != expectedBytes * 2U)
            return false;
        bytes.clear();
        bytes.reserve(expectedBytes);
        for (std::size_t index = 0U; index < text.size(); index += 2U) {
            const int high = HexValue(text[index]);
            const int low = HexValue(text[index + 1U]);
            if (high < 0 || low < 0)
                return false;
            bytes.push_back(static_cast<std::byte>((high << 4U) | low));
        }
        return true;
    }

    bool ValidEnvelope(const Security::DetachedSignatureEnvelope &envelope, const Sha256Digest &digest) {
        return envelope.algorithm == Security::SignatureAlgorithm::EcdsaP256Sha256 && envelope.artifactDigest == digest &&
               IsValidDistributionIdentity(envelope.publisherId) && IsValidDistributionIdentity(envelope.keyId) &&
               envelope.signature.size() == SignatureBytes;
    }

    nlohmann::json WriteEnvelope(const Security::DetachedSignatureEnvelope &envelope) {
        return {{"algorithm", "ecdsa-p256-sha256"},
                {"publisherId", envelope.publisherId},
                {"keyId", envelope.keyId},
                {"signature", FormatHex(envelope.signature)}};
    }

    bool ReadEnvelope(const nlohmann::json &json, const Sha256Digest &digest, Security::DetachedSignatureEnvelope &envelope) {
        if (!json.is_object() || json.size() != 4U || json.at("algorithm") != "ecdsa-p256-sha256" || !json.at("publisherId").is_string() ||
            !json.at("keyId").is_string() || !json.at("signature").is_string())
            return false;
        envelope = {.algorithm = Security::SignatureAlgorithm::EcdsaP256Sha256,
                    .publisherId = json.at("publisherId").get<std::string>(),
                    .keyId = json.at("keyId").get<std::string>(),
                    .artifactDigest = digest};
        return ParseHex(json.at("signature").get<std::string>(), SignatureBytes, envelope.signature) && ValidEnvelope(envelope, digest);
    }
}  // namespace Horo::Release::Detail
