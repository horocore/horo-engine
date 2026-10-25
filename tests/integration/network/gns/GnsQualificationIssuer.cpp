#include "GnsQualificationSupport.h"
#include "Horo/Network/NetworkErrors.h"
#include "Horo/Platform/SecureRandom.h"
#include "crypto.h"
#include "crypto_25519.h"
#include "steamnetworkingsockets_internal.h"
#include "steamnetworkingsockets_messages_certs.pb.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <limits>
#include <mbedtls/md.h>
#include <memory>
#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Horo::Network::NativeQualification {
    namespace {
        Error FixtureError() {
            return MakeError(NetworkErrors::AuthenticationTrustUnavailable, "Explicit native qualification fixture is unavailable");
        }

#if !defined(_WIN32)
        template <std::size_t Size> struct SecretScratch final {
            std::array<std::byte, Size> bytes{};

            ~SecretScratch() {
                Security::SecureZero(bytes);
            }
        };

        using File = std::unique_ptr<std::FILE, decltype(&std::fclose)>;

        bool PrivateDirectory(const std::filesystem::path &path) {
            struct stat status{};
            return lstat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode) && status.st_uid == geteuid() &&
                   (status.st_mode & 0777) == 0700;
        }

        bool PrivateFile(std::FILE *file) {
            struct stat status{};
            return fstat(fileno(file), &status) == 0 && S_ISREG(status.st_mode) && status.st_uid == geteuid() &&
                   (status.st_mode & 0777) == 0600;
        }

        File Open(const std::filesystem::path &path, const char *mode) {
            File owner(std::fopen(path.c_str(), mode), &std::fclose);
            if (owner && std::setvbuf(owner.get(), nullptr, _IONBF, 0) != 0)
                return {nullptr, &std::fclose};
            return owner;
        }

        Result<void> WriteExclusive(const std::filesystem::path &path, const std::span<const std::byte> bytes) {
            if (!PrivateDirectory(path.parent_path()))
                return Result<void>::Failure(FixtureError());
            auto file = Open(path, "wbx");
            if (!file)
                return Result<void>::Failure(FixtureError());
            std::error_code error;
            std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                         std::filesystem::perm_options::replace, error);
            if (error || !PrivateFile(file.get()) || std::fwrite(bytes.data(), 1, bytes.size(), file.get()) != bytes.size())
                return Result<void>::Failure(FixtureError());
            return Result<void>::Success();
        }

        template <std::size_t Size> Result<std::size_t> ReadBounded(const std::filesystem::path &path, SecretScratch<Size> &scratch) {
            if (!PrivateDirectory(path.parent_path()))
                return Result<std::size_t>::Failure(FixtureError());
            const auto status = std::filesystem::symlink_status(path);
            if (status.type() != std::filesystem::file_type::regular)
                return Result<std::size_t>::Failure(FixtureError());
            auto file = Open(path, "rb");
            if (!file || !PrivateFile(file.get()))
                return Result<std::size_t>::Failure(FixtureError());
            const auto count = std::fread(scratch.bytes.data(), 1, scratch.bytes.size(), file.get());
            std::array<unsigned char, 1> lookahead{};
            const auto extra = std::fread(lookahead.data(), 1, lookahead.size(), file.get());
            if (count == 0 || extra != 0 || std::ferror(file.get()))
                return Result<std::size_t>::Failure(FixtureError());
            return Result<std::size_t>::Success(count);
        }

        /** @brief Test credential possession only; never certifies a transport or creates an active lifecycle. */
        class FixtureCredential final : public ICredentialAuthority, public IAdmissionProofSource {
        public:
            FixtureCredential(Security::SecureBytes secret, std::shared_ptr<Security::SecureRandomSource> random, unsigned side)
                : secret_(std::move(secret)), random_(std::move(random)), side_(side) {}

            bool Available() const noexcept override {
                return !secret_.Empty() && random_ != nullptr;
            }

            Result<CredentialVerificationResult> Verify(const CredentialVerificationRequest &request) override {
                if (request.binding.Value() != 11 || request.trustLevel != NetworkTrustLevel::ProductAnchor ||
                    request.nowTick > std::numeric_limits<std::uint64_t>::max() - 30000)
                    return Result<CredentialVerificationResult>::Failure(FixtureError());
                auto expected = Mac(request.challenge);
                if (expected.HasError() || request.proof.size() != 32)
                    return Result<CredentialVerificationResult>::Failure(FixtureError());
                std::uint8_t different{};
                for (std::size_t i = 0; i < 32; ++i)
                    different |= std::to_integer<std::uint8_t>(request.proof[i] ^ expected.Value().View()[i]);
                if (different)
                    return Result<CredentialVerificationResult>::Failure(MakeError(NetworkErrors::AuthenticationRejected));
                SessionPrincipal principal;
                principal.principal = NetworkPrincipalId::Create(20 + 1 - side_).Value();
                if (auto entropy = random_->Fill(principal.session.bytes); entropy.HasError())
                    return Result<CredentialVerificationResult>::Failure(entropy.ErrorValue());
                principal.trustLevel = request.trustLevel;
                principal.roles.values[0] = NetworkRoleId::Create(30).Value();
                principal.roles.count = 1;
                principal.capabilities.values[0] = NetworkCapabilityId::Create(40).Value();
                principal.capabilities.count = 1;
                principal.provenance = CredentialProvenanceId::Create(50).Value();
                principal.expiresAtTick = request.nowTick + 30000;
                return Result<CredentialVerificationResult>::Success(
                    {{request.challenge.connection, request.challenge.sessionGeneration, 103}, request.binding, principal});
            }

            Result<Security::SecureBytes> Issue(const AdmissionProofRequest &request) override {
                if (request.nowTick >= request.authorization.DeadlineTick() ||
                    request.authorization.Peer().trustLevel != NetworkTrustLevel::ProductAnchor)
                    return Result<Security::SecureBytes>::Failure(FixtureError());
                return Mac(request.authorization.Challenge());
            }

        private:
            Result<Security::SecureBytes> Mac(const AuthenticationChallenge &challenge) const {
                if (challenge.contractVersion != AuthenticationContractVersion || !challenge.connection.IsValid() ||
                    !challenge.sessionGeneration.IsValid() || challenge.policy.Value() != 10 || challenge.policyRevision != 4)
                    return Result<Security::SecureBytes>::Failure(FixtureError());
                std::array<std::byte, 116> payload{};
                std::size_t offset{};
                const auto number = [&](std::uint64_t value, std::size_t width) {
                    for (std::size_t i = 0; i < width; ++i)
                        payload[offset++] = static_cast<std::byte>(value >> ((width - i - 1) * 8));
                };
                number(challenge.contractVersion, 4);
                number(challenge.policy.Value(), 8);
                number(challenge.policyRevision, 8);
                for (const auto field :
                     {std::span{challenge.transcriptDigest}, std::span{challenge.clientNonce}, std::span{challenge.serverNonce}}) {
                    std::ranges::copy(field, payload.begin() + static_cast<std::ptrdiff_t>(offset));
                    offset += field.size();
                }
                SecretScratch<32> digest;
                const auto *algorithm = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
                if (!algorithm || mbedtls_md_hmac(algorithm, reinterpret_cast<const unsigned char *>(secret_.View().data()),
                                                  secret_.View().size(), reinterpret_cast<const unsigned char *>(payload.data()),
                                                  payload.size(), reinterpret_cast<unsigned char *>(digest.bytes.data())) != 0)
                    return Result<Security::SecureBytes>::Failure(FixtureError());
                return Result<Security::SecureBytes>::Success(Security::SecureBytes(digest.bytes));
            }

            Security::SecureBytes secret_;
            std::shared_ptr<Security::SecureRandomSource> random_;
            unsigned side_{};
        };

        class FixtureIssuer final : public IGnsCertificateIssuer {
        public:
            [[nodiscard]] bool Load(SecretScratch<512> &pem, const std::size_t bytes, const unsigned side) {
                side_ = side;
                return key_.LoadFromAndWipeBuffer(pem.bytes.data(), bytes);
            }

            Result<Security::SecureBytes> Issue(std::span<const std::byte> request,
                                                const std::chrono::steady_clock::time_point deadline) override {
                using namespace SteamNetworkingSocketsLib;
                if (request.empty() || request.size() > 16384 || std::chrono::steady_clock::now() >= deadline)
                    return Result<Security::SecureBytes>::Failure(FixtureError());
                CMsgSteamDatagramCertificateRequest input;
                if (!input.ParseFromArray(request.data(), static_cast<int>(request.size())) || !input.has_cert() ||
                    input.cert().key_type() != CMsgSteamDatagramCertificate_EKeyType_ED25519 || input.cert().key_data().size() != 32)
                    return Result<Security::SecureBytes>::Failure(FixtureError());
                auto certificate = input.cert();  // Public CSR only; native connection private key never leaves its owner.
                certificate.clear_app_ids();
                certificate.clear_legacy_steam_id();
                certificate.clear_legacy_identity_binary();
                certificate.add_app_ids(480);
                certificate.set_identity_string(side_ == 0 ? "gen:0102030405060708090a0b0c0d0e0f10"
                                                           : "gen:1112131415161718191a1b1c1d1e1f20");
                const auto now = static_cast<std::uint32_t>(std::time(nullptr));
                certificate.set_time_created(now - 1);
                certificate.set_time_expiry(now + 120);
                CMsgSteamDatagramCertificateSigned signedCertificate;
                if (!certificate.SerializeToString(signedCertificate.mutable_cert()))
                    return Result<Security::SecureBytes>::Failure(FixtureError());
                CECSigningPublicKey publicKey;
                if (!key_.GetPublicKey(&publicKey))
                    return Result<Security::SecureBytes>::Failure(FixtureError());
                CryptoSignature_t signature{};
                key_.GenerateSignature(signedCertificate.cert().data(), signedCertificate.cert().size(), &signature);
                signedCertificate.set_ca_key_id(CalculatePublicKeyID(publicKey));
                signedCertificate.set_ca_signature(&signature, sizeof(signature));
                const auto encoded = signedCertificate.SerializeAsString();  // Public signed certificate, never proof/private fields.
                return Result<Security::SecureBytes>::Success(
                    Security::SecureBytes(std::as_bytes(std::span{encoded.data(), encoded.size()})));
            }

        private:
            CECSigningPrivateKey key_;
            unsigned side_{};
        };
#endif
    }  // namespace

    /** @copydoc CreateEphemeralFixture */
    Result<void> CreateEphemeralFixture(const std::filesystem::path &directory) {
#if defined(_WIN32)
        // The qualification fixture cannot attest restrictive Windows DACLs.
        static_cast<void>(directory);
        return Result<void>::Failure(FixtureError());
#else
        if (!PrivateDirectory(directory))
            return Result<void>::Failure(FixtureError());
        CECSigningPublicKey publicKey;
        CECSigningPrivateKey privateKey;
        CCrypto::GenerateSigningKeyPair(&publicKey, &privateKey);
        if (!publicKey.IsValid() || !privateKey.IsValid())
            return Result<void>::Failure(FixtureError());
        std::array<std::byte, 32> root{};
        if (publicKey.GetRawData(root.data()) != root.size())
            return Result<void>::Failure(FixtureError());
        SecretScratch<512> pem;
        std::uint32_t bytes{};
        if (!privateKey.GetAsPEM(reinterpret_cast<char *>(pem.bytes.data()), pem.bytes.size(), &bytes) || bytes == 0 ||
            bytes > pem.bytes.size())
            return Result<void>::Failure(FixtureError());
        if (auto written = WriteExclusive(directory / "root.bin", root); written.HasError())
            return written;
        if (auto written = WriteExclusive(directory / "issuer.pem", std::span{pem.bytes}.first(bytes)); written.HasError())
            return written;
        SecretScratch<32> credential;
        auto random = Platform::CreateNativeSecureRandomSource();
        if (!random || random->Fill(credential.bytes).HasError())
            return Result<void>::Failure(FixtureError());
        return WriteExclusive(directory / "credential.bin", credential.bytes);
#endif
    }

    /** @copydoc LoadNativeSelection */
    Result<GnsNativeSecurityOptions> LoadNativeSelection(const std::filesystem::path &directory, const unsigned side,
                                                         NetworkTrustPolicySnapshot trust) {
#if defined(_WIN32)
        // The qualification fixture cannot attest restrictive Windows DACLs.
        static_cast<void>(directory);
        static_cast<void>(side);
        static_cast<void>(trust);
        return Result<GnsNativeSecurityOptions>::Failure(FixtureError());
#else
        if (side > 1)
            return Result<GnsNativeSecurityOptions>::Failure(FixtureError());
        SecretScratch<32> root;
        auto readRoot = ReadBounded(directory / "root.bin", root);
        SecretScratch<512> pem;
        auto readPem = ReadBounded(directory / "issuer.pem", pem);
        if (readRoot.HasError() || readRoot.Value() != 32 || readPem.HasError())
            return Result<GnsNativeSecurityOptions>::Failure(FixtureError());
        auto issuer = std::make_shared<FixtureIssuer>();
        if (!issuer->Load(pem, readPem.Value(), side))
            return Result<GnsNativeSecurityOptions>::Failure(FixtureError());
        GnsNativeSecurityOptions options;
        options.trustedRoot = root.bytes;
        options.appId = 480;
        options.trust = trust;
        options.peerTrust = NetworkTrustLevel::ProductAnchor;
        options.transport = {4, 64, 1200};
        options.provisioningBudget = std::chrono::seconds(5);
        options.issuer = std::move(issuer);
        return Result<GnsNativeSecurityOptions>::Success(std::move(options));
#endif
    }
}  // namespace Horo::Network::NativeQualification

namespace Horo::Network::NativeQualification {
    /** @copydoc LoadCredentialProviders */
    Result<NetworkAdmissionProviders> LoadCredentialProviders(const std::filesystem::path &directory, const unsigned side) {
#if defined(_WIN32)
        // The qualification fixture cannot attest restrictive Windows DACLs.
        static_cast<void>(directory);
        static_cast<void>(side);
        return Result<NetworkAdmissionProviders>::Failure(FixtureError());
#else
        if (side > 1)
            return Result<NetworkAdmissionProviders>::Failure(FixtureError());
        SecretScratch<32> secret;
        auto read = ReadBounded(directory / "credential.bin", secret);
        if (read.HasError() || read.Value() != 32)
            return Result<NetworkAdmissionProviders>::Failure(FixtureError());
        auto random = Platform::CreateNativeSecureRandomSource();
        if (!random)
            return Result<NetworkAdmissionProviders>::Failure(FixtureError());
        auto credential = std::make_shared<FixtureCredential>(Security::SecureBytes(secret.bytes), random, side);
        return Result<NetworkAdmissionProviders>::Success({{}, {}, credential, {}, std::move(random), credential, {}});
#endif
    }
}  // namespace Horo::Network::NativeQualification
