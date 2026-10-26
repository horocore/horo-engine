#include "HostSignatureDocument.h"

#include <algorithm>
#include <array>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace Horo::Application::Internal {
    namespace {
        using Json = nlohmann::json;
        const ErrorCodeDescriptor InvalidDocument{ErrorDomainId{"application.host.documents"},
                                                  ErrorCode{"package.document_invalid"},
                                                  ErrorSeverity::Error,
                                                  "Invalid host signature document",
                                                  "Provide the bounded canonical author-tool document.",
                                                  false,
                                                  true,
                                                  {}};
        const ErrorCodeDescriptor InvalidSignature{ErrorDomainId{"application.host.documents"},
                                                   ErrorCode{"package.signature_invalid"},
                                                   ErrorSeverity::Error,
                                                   "Invalid detached signature",
                                                   "Provide complete exact signature identities and bytes.",
                                                   false,
                                                   true,
                                                   {}};
        const ErrorCodeDescriptor InvalidTrust{ErrorDomainId{"application.host.documents"},
                                               ErrorCode{"package.trust_invalid"},
                                               ErrorSeverity::Error,
                                               "Invalid publisher trust",
                                               "Provide explicit complete host-approved publisher records.",
                                               false,
                                               true,
                                               {}};
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

        /** @brief Applies the existing author-tool byte/depth/duplicate-key bounds before typed extraction. */
        [[nodiscard]] Result<Json> ParseDocument(const std::span<const std::byte> bytes) {
            if (bytes.empty() || bytes.size() > MaximumHostSignatureDocumentBytes)
                return Result<Json>::Failure(MakeError(InvalidDocument, "JSON document is empty or exceeds the byte limit."));
            try {
                UniqueKeys keys;
                auto document = Json::parse(bytes.begin(), bytes.end(), std::ref(keys));
                if (!keys.valid)
                    return Result<Json>::Failure(MakeError(InvalidDocument, "JSON has duplicate fields or excessive nesting."));
                return Result<Json>::Success(std::move(document));
            } catch (const Json::exception &) {
                return Result<Json>::Failure(MakeError(InvalidDocument, "JSON document is malformed."));
            }
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

    /** @copydoc DecodeDetachedSignatureDocument */
    Result<Security::DetachedSignatureEnvelope> DecodeDetachedSignatureDocument(const std::span<const std::byte> bytes) {
        using Outcome = Result<Security::DetachedSignatureEnvelope>;
        auto parsed = ParseDocument(bytes);
        if (parsed.HasError())
            return Outcome::Failure(parsed.ErrorValue());
        const auto &value = parsed.Value();
        if (!ExactKeys(value, {"schemaVersion", "algorithm", "publisherId", "keyId", "artifactSha256", "signatureHex"}) ||
            !value["schemaVersion"].is_number_unsigned() || value["schemaVersion"] != 1 || !value["algorithm"].is_string() ||
            value["algorithm"] != Algorithm || !value["publisherId"].is_string() || !value["keyId"].is_string() ||
            !value["artifactSha256"].is_string() || !value["signatureHex"].is_string())
            return Outcome::Failure(MakeError(InvalidSignature, "Detached signature schema is invalid."));
        auto publisher = Packages::PackagePublisherId::Parse(value["publisherId"].get<std::string>());
        auto digest = ParseSha256(value["artifactSha256"].get<std::string>());
        auto signature = DecodeHex(value["signatureHex"].get<std::string>(), 64U);
        const auto keyId = value["keyId"].get<std::string>();
        if (publisher.HasError() || digest.HasError() || !signature || keyId.empty() || keyId.size() > 256U)
            return Outcome::Failure(MakeError(InvalidSignature, "Detached signature identity or bytes are invalid."));
        Security::DetachedSignatureEnvelope envelope{.algorithm = Security::SignatureAlgorithm::EcdsaP256Sha256,
                                                     .publisherId = publisher.Value().Value(),
                                                     .keyId = keyId,
                                                     .artifactDigest = digest.Value(),
                                                     .signature = std::move(*signature)};
        return Outcome::Success(std::move(envelope));
    }

    /** @copydoc DecodePublisherTrustDocument */
    Result<Packages::PackagePublisherVerificationPolicy> DecodePublisherTrustDocument(
        const std::span<const std::byte> bytes, const Packages::PackagePublisherVerificationPolicy &seed) {
        using Outcome = Result<Packages::PackagePublisherVerificationPolicy>;
        auto parsed = ParseDocument(bytes);
        if (parsed.HasError())
            return Outcome::Failure(parsed.ErrorValue());
        const auto &value = parsed.Value();
        if (!ExactKeys(value, {"schemaVersion", "allowUnsigned", "publishers"}) || !value["schemaVersion"].is_number_unsigned() ||
            value["schemaVersion"] != 1 || !value["allowUnsigned"].is_boolean() || !value["publishers"].is_array() ||
            seed.publishers.size() > seed.limits.maximumPublisherRecords ||
            value["publishers"].size() > seed.limits.maximumPublisherRecords - seed.publishers.size())
            return Outcome::Failure(MakeError(InvalidTrust, "Trust policy schema or publisher count is invalid."));
        // Copy only after bounded admission. Failure, including allocation failure, cannot mutate the caller.
        auto policy = seed;
        policy.allowUnsigned = value["allowUnsigned"].get<bool>();
        for (const auto &entry : value["publishers"]) {
            if (!ExactKeys(entry, {"publisherId", "keyId", "algorithm", "publicKeyHex", "expiresAtUnixMilliseconds", "revoked"}) ||
                !entry["publisherId"].is_string() || !entry["keyId"].is_string() || !entry["algorithm"].is_string() ||
                entry["algorithm"] != Algorithm || !entry["publicKeyHex"].is_string() ||
                !entry["expiresAtUnixMilliseconds"].is_number_unsigned() || !entry["revoked"].is_boolean())
                return Outcome::Failure(MakeError(InvalidTrust, "Trust publisher entry is invalid."));
            auto publisher = Packages::PackagePublisherId::Parse(entry["publisherId"].get<std::string>());
            auto key = DecodeHex(entry["publicKeyHex"].get<std::string>(), 65U);
            const auto keyId = entry["keyId"].get<std::string>();
            if (publisher.HasError() || !key || key->front() != std::byte{0x04} || keyId.empty() || keyId.size() > 256U)
                return Outcome::Failure(MakeError(InvalidTrust, "Trust publisher identity or key is invalid."));
            policy.publishers.emplace_back(
                Packages::PackagePublisherTrustRecord{.publisher = std::move(publisher).Value(),
                                                      .keyId = keyId,
                                                      .algorithm = Security::SignatureAlgorithm::EcdsaP256Sha256,
                                                      .publicKey = std::move(*key),
                                                      .expiresAtUnixMilliseconds = entry["expiresAtUnixMilliseconds"].get<std::uint64_t>(),
                                                      .publisherRevoked = entry["revoked"].get<bool>()});
        }
        return Outcome::Success(std::move(policy));
    }

    /** @copydoc EncodeDetachedSignatureDocument */
    std::string EncodeDetachedSignatureDocument(const Security::DetachedSignatureEnvelope &envelope) {
        return Json{{"schemaVersion", 1},
                    {"algorithm", Algorithm},
                    {"publisherId", envelope.publisherId},
                    {"keyId", envelope.keyId},
                    {"artifactSha256", FormatSha256(envelope.artifactDigest)},
                    {"signatureHex", EncodeHex(envelope.signature)}}
                   .dump() +
               '\n';
    }
}  // namespace Horo::Application::Internal
