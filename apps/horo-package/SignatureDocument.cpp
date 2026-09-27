#include "SignatureDocument.h"

#include "Horo/Foundation/Sha256.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <span>

namespace Horo::PackageCommand {
    namespace {
        using Json = nlohmann::json;
        constexpr std::string_view Algorithm = "ecdsa-p256-sha256";

        struct UniqueKeys final {
            std::array<std::set<std::string, std::less<>>, 5> keys;
            bool valid{true};

            bool operator()(const int depth, const Json::parse_event_t event, const Json &value) {
                if (depth < 0 || depth >= 4) {
                    valid = false;
                    return false;
                }
                const auto index = static_cast<std::size_t>(depth);
                if (event == Json::parse_event_t::object_start)
                    keys[index + 1].clear();
                else if (event == Json::parse_event_t::key)
                    valid &= keys[index].insert(value.get<std::string>()).second;
                return valid;
            }
        };

        [[nodiscard]] bool ExactKeys(const Json &value, const std::initializer_list<std::string_view> keys) {
            if (!value.is_object() || value.size() != keys.size())
                return false;
            return std::ranges::all_of(keys, [&value](const std::string_view key) {
                return value.contains(std::string{key});
            });
        }

        [[nodiscard]] Outcome LoadJson(const std::filesystem::path &path, Json &document) {
            std::vector<std::byte> bytes;
            if (const auto read = ReadBounded(path, 1U << 20U, bytes); !read.success)
                return read;
            if (bytes.empty())
                return Failure("package.document_invalid", "JSON document is empty.");
            try {
                UniqueKeys keys;
                std::string text(bytes.size(), '\0');
                std::ranges::transform(bytes, text.begin(), [](const std::byte value) {
                    return static_cast<char>(std::to_integer<unsigned char>(value));
                });
                document = Json::parse(text, std::ref(keys));
                if (!keys.valid)
                    return Failure("package.document_invalid", "JSON has duplicate fields or excessive nesting.");
            } catch (const Json::exception &) {
                return Failure("package.document_invalid", "JSON document is malformed.");
            }
            return Success();
        }

        [[nodiscard]] std::optional<std::vector<std::byte>> DecodeHex(const std::string_view text, const std::size_t expectedBytes) {
            if (text.size() != expectedBytes * 2)
                return std::nullopt;
            const auto digit = [](const char character) {
                if (character >= '0' && character <= '9')
                    return character - '0';
                if (character >= 'a' && character <= 'f')
                    return character - 'a' + 10;
                return -1;
            };
            std::vector<std::byte> bytes(expectedBytes);
            for (std::size_t index = 0; index < expectedBytes; ++index) {
                const int high = digit(text[index * 2]);
                const int low = digit(text[index * 2 + 1]);
                if (high < 0 || low < 0)
                    return std::nullopt;
                bytes[index] = static_cast<std::byte>((high << 4) | low);
            }
            return bytes;
        }

        [[nodiscard]] std::string EncodeHex(const std::span<const std::byte> bytes) {
            constexpr std::string_view digits = "0123456789abcdef";
            std::string text;
            text.reserve(bytes.size() * 2);
            for (const std::byte byte : bytes) {
                const auto value = std::to_integer<unsigned>(byte);
                text += digits[value >> 4U];
                text += digits[value & 0x0fU];
            }
            return text;
        }
    }  // namespace

    Outcome LoadSignature(const std::filesystem::path &path, Security::DetachedSignatureEnvelope &envelope) {
        Json value;
        if (const auto loaded = LoadJson(path, value); !loaded.success)
            return loaded;
        if (!ExactKeys(value, {"schemaVersion", "algorithm", "publisherId", "keyId", "artifactSha256", "signatureHex"}) ||
            !value["schemaVersion"].is_number_unsigned() || value["schemaVersion"] != 1 || !value["algorithm"].is_string() ||
            value["algorithm"] != Algorithm || !value["publisherId"].is_string() || !value["keyId"].is_string() ||
            !value["artifactSha256"].is_string() || !value["signatureHex"].is_string())
            return Failure("package.signature_invalid", "Detached signature schema is invalid.");
        auto publisher = Packages::PackagePublisherId::Parse(value["publisherId"].get<std::string>());
        auto digest = ParseSha256(value["artifactSha256"].get<std::string>());
        auto signature = DecodeHex(value["signatureHex"].get<std::string>(), 64U);
        const auto keyId = value["keyId"].get<std::string>();
        if (publisher.HasError() || digest.HasError() || !signature || keyId.empty() || keyId.size() > 256U)
            return Failure("package.signature_invalid", "Detached signature identity or bytes are invalid.");
        envelope = {.algorithm = Security::SignatureAlgorithm::EcdsaP256Sha256,
                    .publisherId = publisher.Value().Value(),
                    .keyId = keyId,
                    .artifactDigest = digest.Value(),
                    .signature = std::move(*signature)};
        return Success();
    }

    Outcome LoadTrust(const std::filesystem::path &path, Packages::PackagePublisherVerificationPolicy &policy) {
        Json value;
        if (const auto loaded = LoadJson(path, value); !loaded.success)
            return loaded;
        if (!ExactKeys(value, {"schemaVersion", "allowUnsigned", "publishers"}) || !value["schemaVersion"].is_number_unsigned() ||
            value["schemaVersion"] != 1 || !value["allowUnsigned"].is_boolean() || !value["publishers"].is_array() ||
            value["publishers"].size() > policy.limits.maximumPublisherRecords)
            return Failure("package.trust_invalid", "Trust policy schema or publisher count is invalid.");
        policy.allowUnsigned = value["allowUnsigned"].get<bool>();
        for (const auto &entry : value["publishers"]) {
            if (!ExactKeys(entry, {"publisherId", "keyId", "algorithm", "publicKeyHex", "expiresAtUnixMilliseconds", "revoked"}) ||
                !entry["publisherId"].is_string() || !entry["keyId"].is_string() || !entry["algorithm"].is_string() ||
                entry["algorithm"] != Algorithm || !entry["publicKeyHex"].is_string() ||
                !entry["expiresAtUnixMilliseconds"].is_number_unsigned() || !entry["revoked"].is_boolean())
                return Failure("package.trust_invalid", "Trust publisher entry is invalid.");
            auto publisher = Packages::PackagePublisherId::Parse(entry["publisherId"].get<std::string>());
            auto key = DecodeHex(entry["publicKeyHex"].get<std::string>(), 65U);
            const auto keyId = entry["keyId"].get<std::string>();
            if (publisher.HasError() || !key || key->front() != std::byte{0x04} || keyId.empty() || keyId.size() > 256U)
                return Failure("package.trust_invalid", "Trust publisher identity or key is invalid.");
            policy.publishers.emplace_back(
                Packages::PackagePublisherTrustRecord{.publisher = std::move(publisher).Value(),
                                                      .keyId = keyId,
                                                      .algorithm = Security::SignatureAlgorithm::EcdsaP256Sha256,
                                                      .publicKey = std::move(*key),
                                                      .expiresAtUnixMilliseconds = entry["expiresAtUnixMilliseconds"].get<std::uint64_t>(),
                                                      .publisherRevoked = entry["revoked"].get<bool>()});
        }
        return Success();
    }

    std::string EncodeSignature(const Security::DetachedSignatureEnvelope &envelope) {
        return Json{{"schemaVersion", 1},
                    {"algorithm", Algorithm},
                    {"publisherId", envelope.publisherId},
                    {"keyId", envelope.keyId},
                    {"artifactSha256", FormatSha256(envelope.artifactDigest)},
                    {"signatureHex", EncodeHex(envelope.signature)}}
                   .dump() +
               '\n';
    }
}  // namespace Horo::PackageCommand
