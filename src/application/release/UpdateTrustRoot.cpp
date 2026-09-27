#include "Horo/Release/UpdateTrustRoot.h"

#include "Horo/Release/UpdateManifestErrors.h"
#include "UpdateSignatureCodec.h"

#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <utility>

namespace Horo::Release {
    namespace {
        using Json = nlohmann::json;
        constexpr std::size_t MaximumRootDocumentBytes = 64U * 1024U;
        constexpr std::size_t MaximumKeys = 16U;
        constexpr std::size_t PublicKeyBytes = 65U;

        [[nodiscard]] bool ValidRoot(const UpdateTrustRootData &data) {
            if (Detail::ProductName(data.product.kind).empty() ||
                (data.product.kind == DistributionProductKind::RendererComponent ? !IsValidDistributionIdentity(data.product.componentId)
                                                                                 : !data.product.componentId.empty()) ||
                data.revision == 0U || data.parentRevision >= data.revision || data.minimumManifestSequence == 0U || data.expiresAt == 0U ||
                data.keys.empty() || data.keys.size() > MaximumKeys)
                return false;
            Security::TrustedRootStore roots;
            for (const auto &key : data.keys) {
                if (key.algorithm != Security::SignatureAlgorithm::EcdsaP256Sha256 || key.publicKey.size() != PublicKeyBytes ||
                    key.publicKey.front() != std::byte{0x04} || roots.Add(key).HasError())
                    return false;
            }
            return true;
        }

        [[nodiscard]] Json WriteRoot(const UpdateTrustRootData &data) {
            Json keys = Json::array();
            for (const auto &key : data.keys)
                keys.push_back({{"algorithm", "ecdsa-p256-sha256"},
                                {"publisherId", key.publisherId},
                                {"keyId", key.keyId},
                                {"publicKey", Detail::FormatHex(key.publicKey)}});
            return {{"schemaVersion", 1},
                    {"product", {{"kind", Detail::ProductName(data.product.kind)}, {"componentId", data.product.componentId}}},
                    {"revision", data.revision},
                    {"parentRevision", data.parentRevision},
                    {"minimumManifestSequence", data.minimumManifestSequence},
                    {"expiresAt", data.expiresAt},
                    {"keys", std::move(keys)}};
        }

        [[nodiscard]] bool ReadRoot(const Json &json, UpdateTrustRootData &data) {
            if (!json.is_object() || json.size() != 7U || json.at("schemaVersion") != 1 || !json.at("product").is_object() ||
                json.at("product").size() != 2U || !json.at("keys").is_array() || json.at("keys").size() > MaximumKeys)
                return false;
            if (!Detail::ParseProductKind(json.at("product").at("kind").get<std::string>(), data.product.kind))
                return false;
            data.product.componentId = json.at("product").at("componentId").get<std::string>();
            data.revision = json.at("revision").get<std::uint64_t>();
            data.parentRevision = json.at("parentRevision").get<std::uint64_t>();
            data.minimumManifestSequence = json.at("minimumManifestSequence").get<std::uint64_t>();
            data.expiresAt = json.at("expiresAt").get<std::uint64_t>();
            for (const Json &entry : json.at("keys")) {
                if (!entry.is_object() || entry.size() != 4U || entry.at("algorithm") != "ecdsa-p256-sha256")
                    return false;
                Security::TrustedSigningKey key;
                key.publisherId = entry.at("publisherId").get<std::string>();
                key.keyId = entry.at("keyId").get<std::string>();
                if (!Detail::ParseHex(entry.at("publicKey").get<std::string>(), PublicKeyBytes, key.publicKey))
                    return false;
                data.keys.push_back(std::move(key));
            }
            return ValidRoot(data);
        }

        [[nodiscard]] Result<SignedUpdateTrustRoot> InvalidDocument() {
            return Result<SignedUpdateTrustRoot>::Failure(MakeError(UpdateManifestErrors::Invalid));
        }

        [[nodiscard]] Result<UpdateTrustRootSnapshot> InvalidRoot() {
            return Result<UpdateTrustRootSnapshot>::Failure(MakeError(UpdateManifestErrors::Invalid));
        }

        [[nodiscard]] std::shared_ptr<const Security::TrustedRootStore> BuildRoots(const UpdateTrustRootData &data) {
            auto roots = std::make_shared<Security::TrustedRootStore>();
            for (const auto &key : data.keys)
                static_cast<void>(roots->Add(key));
            return roots;
        }
    }  // namespace

    /** @copydoc BuildCanonicalUpdateTrustRootPayload */
    Result<std::string> BuildCanonicalUpdateTrustRootPayload(const UpdateTrustRootData &data) {
        if (!ValidRoot(data))
            return Result<std::string>::Failure(MakeError(UpdateManifestErrors::Invalid));
        std::string payload = WriteRoot(data).dump();
        if (payload.size() > MaximumRootDocumentBytes)
            return Result<std::string>::Failure(MakeError(UpdateManifestErrors::Invalid));
        return Result<std::string>::Success(std::move(payload));
    }

    SignedUpdateTrustRoot::SignedUpdateTrustRoot(UpdateTrustRootData data, Security::DetachedSignatureEnvelope signature,
                                                 std::string payload, std::string document)
        : data_(std::move(data)), signature_(std::move(signature)), payload_(std::move(payload)), document_(std::move(document)) {}

    /** @copydoc SignedUpdateTrustRoot::Create */
    Result<SignedUpdateTrustRoot> SignedUpdateTrustRoot::Create(UpdateTrustRootData data, Security::DetachedSignatureEnvelope signature) {
        auto payload = BuildCanonicalUpdateTrustRootPayload(data);
        if (payload.HasError() || !Detail::ValidEnvelope(signature, ComputeSha256(std::as_bytes(std::span{payload.Value()}))))
            return InvalidDocument();
        std::string payloadBytes = std::move(payload).Value();
        std::string document = Json{{"root", Json::parse(payloadBytes)}, {"signature", Detail::WriteEnvelope(signature)}}.dump();
        if (document.size() > MaximumRootDocumentBytes)
            return InvalidDocument();
        return Result<SignedUpdateTrustRoot>::Success(
            SignedUpdateTrustRoot{std::move(data), std::move(signature), std::move(payloadBytes), std::move(document)});
    }

    /** @copydoc SignedUpdateTrustRoot::ParseCanonical */
    Result<SignedUpdateTrustRoot> SignedUpdateTrustRoot::ParseCanonical(const std::string_view bytes) {
        if (bytes.empty() || bytes.size() > MaximumRootDocumentBytes)
            return InvalidDocument();
        const Json document = Json::parse(bytes, nullptr, false);
        if (document.is_discarded() || !document.is_object() || document.size() != 2U || document.dump() != bytes ||
            !document.contains("root") || !document.contains("signature"))
            return InvalidDocument();
        try {
            UpdateTrustRootData data;
            const Json &root = document.at("root");
            if (!ReadRoot(root, data))
                return InvalidDocument();
            std::string payload = root.dump();
            Security::DetachedSignatureEnvelope signature;
            if (!Detail::ReadEnvelope(document.at("signature"), ComputeSha256(std::as_bytes(std::span{payload})), signature))
                return InvalidDocument();
            return Result<SignedUpdateTrustRoot>::Success(
                SignedUpdateTrustRoot{std::move(data), std::move(signature), std::move(payload), std::string{bytes}});
        } catch (const Json::exception &) {
            return InvalidDocument();
        }
    }

    /** @copydoc SignedUpdateTrustRoot::Data */
    const UpdateTrustRootData &SignedUpdateTrustRoot::Data() const noexcept {
        return data_;
    }

    /** @copydoc SignedUpdateTrustRoot::CanonicalPayload */
    const std::string &SignedUpdateTrustRoot::CanonicalPayload() const noexcept {
        return payload_;
    }

    /** @copydoc SignedUpdateTrustRoot::CanonicalDocument */
    const std::string &SignedUpdateTrustRoot::CanonicalDocument() const noexcept {
        return document_;
    }

    /** @copydoc SignedUpdateTrustRoot::Signature */
    const Security::DetachedSignatureEnvelope &SignedUpdateTrustRoot::Signature() const noexcept {
        return signature_;
    }

    UpdateTrustRootSnapshot::UpdateTrustRootSnapshot(UpdateTrustRootData data, std::shared_ptr<const Security::TrustedRootStore> roots)
        : data_(std::move(data)), roots_(std::move(roots)) {}

    /** @copydoc UpdateTrustRootSnapshot::Bootstrap */
    Result<UpdateTrustRootSnapshot> UpdateTrustRootSnapshot::Bootstrap(UpdateTrustRootData trusted) {
        if (!ValidRoot(trusted) || trusted.revision != 1U || trusted.parentRevision != 0U)
            return InvalidRoot();
        auto roots = BuildRoots(trusted);
        return Result<UpdateTrustRootSnapshot>::Success(UpdateTrustRootSnapshot{std::move(trusted), std::move(roots)});
    }

    /** @copydoc UpdateTrustRootSnapshot::Transition */
    Result<UpdateTrustRootSnapshot> UpdateTrustRootSnapshot::Transition(const SignedUpdateTrustRoot &proposed,
                                                                        std::shared_ptr<const Security::SignatureProvider> provider,
                                                                        const std::uint64_t now) const {
        const auto &next = proposed.Data();
        if (data_.revision == std::numeric_limits<std::uint64_t>::max() || next.product != data_.product ||
            next.parentRevision != data_.revision || next.revision != data_.revision + 1U ||
            next.minimumManifestSequence < data_.minimumManifestSequence || next.expiresAt <= now)
            return Result<UpdateTrustRootSnapshot>::Failure(MakeError(UpdateManifestErrors::Stale));
        Security::ArtifactVerifier verifier{std::move(provider), roots_};
        if (auto authenticated = verifier.Verify(std::as_bytes(std::span{proposed.CanonicalPayload()}), proposed.Signature());
            authenticated.HasError())
            return Result<UpdateTrustRootSnapshot>::Failure(authenticated.ErrorValue());
        auto roots = BuildRoots(next);
        return Result<UpdateTrustRootSnapshot>::Success(UpdateTrustRootSnapshot{next, std::move(roots)});
    }

    /** @copydoc UpdateTrustRootSnapshot::Product */
    const DistributionProductIdentity &UpdateTrustRootSnapshot::Product() const noexcept {
        return data_.product;
    }

    /** @copydoc UpdateTrustRootSnapshot::Revision */
    std::uint64_t UpdateTrustRootSnapshot::Revision() const noexcept {
        return data_.revision;
    }

    /** @copydoc UpdateTrustRootSnapshot::MinimumManifestSequence */
    std::uint64_t UpdateTrustRootSnapshot::MinimumManifestSequence() const noexcept {
        return data_.minimumManifestSequence;
    }

    /** @copydoc UpdateTrustRootSnapshot::ExpiresAt */
    std::uint64_t UpdateTrustRootSnapshot::ExpiresAt() const noexcept {
        return data_.expiresAt;
    }

    /** @copydoc UpdateTrustRootSnapshot::Roots */
    std::shared_ptr<const Security::TrustedRootStore> UpdateTrustRootSnapshot::Roots() const noexcept {
        return roots_;
    }
}  // namespace Horo::Release
