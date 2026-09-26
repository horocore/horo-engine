#include "Horo/Runtime/Save/SaveArchiveProtection.h"
#include "Horo/Runtime/Save/SaveErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;

    SaveProtectionIdentity Identity() {
        SaveProtectionIdentity identity;
        identity.providerId.back() = 1;
        identity.keyReference.back() = 2;
        return identity;
    }

    SaveProtectedArchive Protected() {
        return {.identity = Identity(), .nonce = std::vector<std::byte>(12, std::byte{3}), .sealedBytes = {std::byte{4}}};
    }

    constexpr std::array AssociatedData{std::byte{1}, std::byte{2}};

    class MockProvider final : public SaveArchiveProtectionProvider {
    public:
        std::array<std::uint8_t, 16> Id() const noexcept override {
            return Identity().providerId;
        }

        SaveProtectionCapabilities Capabilities() const noexcept override {
            return capabilities;
        }

        Result<SaveProtectedArchive> Seal(std::array<std::uint8_t, 16>, std::span<const std::byte>, std::span<const std::byte>) override {
            ++sealCalls;
            if (failure != nullptr)
                return Result<SaveProtectedArchive>::Failure(MakeError(*failure, "private-token-must-not-escape"));
            return Result<SaveProtectedArchive>::Success(Protected());
        }

        Result<std::vector<std::byte>> Open(const SaveProtectedArchive &, std::span<const std::byte>) override {
            ++openCalls;
            if (failure != nullptr)
                return Result<std::vector<std::byte>>::Failure(MakeError(*failure, "private-token-must-not-escape"));
            return Result<std::vector<std::byte>>::Success({std::byte{7}});
        }

        const ErrorCodeDescriptor *failure{&SaveErrors::ProtectionAuthenticationFailed};
        SaveProtectionCapabilities capabilities{true, 12, 12, 1024};
        int openCalls{};
        int sealCalls{};
    };

    void CheckCode(const Error &error, const ErrorCodeDescriptor &expected) {
        CHECK(error.code.Value() == expected.code.Value());
        CHECK(error.message.find("private-token-must-not-escape") == std::string::npos);
    }
}  // namespace

TEST_CASE("Save protection rejects plaintext unless local policy explicitly permits it") {
    SaveArchiveReader reader;
    auto required = AdmitUnencryptedLocalSaveArchive({std::byte{1}}, SaveProtectionPolicy::RequireAuthenticatedEncryption, reader);
    REQUIRE(required.HasError());
    CheckCode(required.ErrorValue(), SaveErrors::ProtectionRequired);

    auto local = AdmitUnencryptedLocalSaveArchive({std::byte{1}}, SaveProtectionPolicy::UnencryptedLocal, reader);
    REQUIRE(local.HasError());
    CHECK(local.ErrorValue().code.Value() != SaveErrors::ProtectionRequired.code.Value());
}

TEST_CASE("Save protection authenticates before archive parsing and strips provider diagnostic text") {
    SaveArchiveReader reader;
    MockProvider provider;
    auto result = AdmitProtectedSaveArchive(Protected(), Identity(), AssociatedData, &provider, reader);
    REQUIRE(result.HasError());
    CheckCode(result.ErrorValue(), SaveErrors::ProtectionAuthenticationFailed);
    CHECK(provider.openCalls == 1);

    provider.failure = nullptr;
    result = AdmitProtectedSaveArchive(Protected(), Identity(), AssociatedData, &provider, reader);
    REQUIRE(result.HasError());
    CHECK(result.ErrorValue().code.Value() == SaveErrors::ArchiveEnvelopeInvalid.code.Value());
    CHECK(provider.openCalls == 2);
}

TEST_CASE("Save protection fails closed on unavailable, mismatched, revoked and rotated providers") {
    SaveArchiveReader reader;
    MockProvider provider;
    const auto archive = Protected();
    auto unavailable = AdmitProtectedSaveArchive(archive, Identity(), AssociatedData, nullptr, reader);
    REQUIRE(unavailable.HasError());
    CheckCode(unavailable.ErrorValue(), SaveErrors::ProtectionUnavailable);

    auto wrong = Identity();
    wrong.providerId.back() = 9;
    auto mismatch = AdmitProtectedSaveArchive(archive, wrong, AssociatedData, &provider, reader);
    REQUIRE(mismatch.HasError());
    CheckCode(mismatch.ErrorValue(), SaveErrors::ProtectionUnsupported);
    CHECK(provider.openCalls == 0);

    provider.capabilities.authenticatedEncryption = false;
    auto unsupported = AdmitProtectedSaveArchive(archive, Identity(), AssociatedData, &provider, reader);
    REQUIRE(unsupported.HasError());
    CheckCode(unsupported.ErrorValue(), SaveErrors::ProtectionUnsupported);
    CHECK(provider.openCalls == 0);
    provider.capabilities.authenticatedEncryption = true;

    for (const auto *failure : {&SaveErrors::ProtectionKeyRotated, &SaveErrors::ProtectionKeyRevoked}) {
        provider.failure = failure;
        auto result = AdmitProtectedSaveArchive(archive, Identity(), AssociatedData, &provider, reader);
        REQUIRE(result.HasError());
        CheckCode(result.ErrorValue(), *failure);
    }
}

TEST_CASE("Save protection rejects hostile lengths and absent host AAD before provider invocation") {
    SaveArchiveReader reader;
    MockProvider provider;
    auto archive = Protected();
    archive.nonce.resize(65);
    auto oversized = AdmitProtectedSaveArchive(archive, Identity(), AssociatedData, &provider, reader);
    REQUIRE(oversized.HasError());
    CheckCode(oversized.ErrorValue(), SaveErrors::ProtectionInvalid);
    CHECK(provider.openCalls == 0);

    archive = Protected();
    auto noAad = AdmitProtectedSaveArchive(archive, Identity(), {}, &provider, reader);
    REQUIRE(noAad.HasError());
    CheckCode(noAad.ErrorValue(), SaveErrors::ProtectionInvalid);
    CHECK(provider.openCalls == 0);

    SaveProtectionLimits limits;
    limits.maximumArchiveBytes = 0;
    auto invalidLimits = AdmitProtectedSaveArchive(archive, Identity(), AssociatedData, &provider, reader, limits);
    REQUIRE(invalidLimits.HasError());
    CheckCode(invalidLimits.ErrorValue(), SaveErrors::ProtectionInvalid);
    CHECK(provider.openCalls == 0);
}

TEST_CASE("Save protection validates provider output before protected publication") {
    MockProvider provider;
    const std::array plaintext{std::byte{1}};
    auto failed = ProtectSaveArchive(plaintext, Identity(), AssociatedData, &provider);
    REQUIRE(failed.HasError());
    CheckCode(failed.ErrorValue(), SaveErrors::ProtectionAuthenticationFailed);
    CHECK(provider.sealCalls == 1);

    provider.failure = nullptr;
    auto sealed = ProtectSaveArchive(plaintext, Identity(), AssociatedData, &provider);
    REQUIRE(sealed.HasValue());
    CHECK(sealed.Value().nonce.size() == 12);
    CHECK(sealed.Value().identity.keyReference == Identity().keyReference);

    SaveProtectionLimits limits;
    limits.maximumArchiveBytes = 0;
    auto invalid = ProtectSaveArchive(plaintext, Identity(), AssociatedData, &provider, limits);
    REQUIRE(invalid.HasError());
    CheckCode(invalid.ErrorValue(), SaveErrors::ProtectionInvalid);
    CHECK(provider.sealCalls == 2);
}
