#include "Horo/Platform/ExternalProcess.h"
#include "Horo/Platform/NativeCredentialBackend.h"
#include "Horo/Platform/SecureRandom.h"
#include "Horo/Security/SecurityErrors.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <optional>

TEST_CASE("Native credential backend preserves and removes only the exact reference", "[platform][credentials]") {
    using namespace Horo;
    const auto backend = Platform::CreateNativeCredentialBackend();
#if defined(_WIN32)
    REQUIRE(backend);
    if (!backend->Available())
        SKIP("This Windows logon does not permit machine-local user credentials.");

    const auto reader = Platform::CreateNativeCredentialBackend();
    REQUIRE(reader);
    Security::CredentialVault owner{backend, Platform::CreateNativeSecureRandomSource()};
    constexpr std::array original{std::byte{1}, std::byte{0}, std::byte{3}};
    constexpr std::array replacement{std::byte{5}, std::byte{6}};
    const auto stored = owner.Put(Security::SecureBytes{original});
    REQUIRE(stored.HasValue());

    const auto reference = CredentialReference::Parse(stored.Value().Value());
    REQUIRE(reference.HasValue());
    const auto result = reader->Resolve(reference.Value());
    REQUIRE(result.HasValue());
    CHECK(std::ranges::equal(result.Value().View(), original));

    const std::array<std::byte, 4096> oversized{};
    CHECK(backend->Put(reference.Value(), Security::SecureBytes{oversized}).HasError());
    const auto preserved = reader->Resolve(reference.Value());
    REQUIRE(preserved.HasValue());
    CHECK(std::ranges::equal(preserved.Value().View(), original));

    REQUIRE(owner.Rotate(reference.Value(), Security::SecureBytes{replacement}).HasValue());
    const auto rotated = reader->Resolve(reference.Value());
    REQUIRE(rotated.HasValue());
    CHECK(std::ranges::equal(rotated.Value().View(), replacement));

    REQUIRE(owner.Revoke(reference.Value()).HasValue());
    const auto removed = reader->Resolve(reference.Value());
    REQUIRE(removed.HasError());
    CHECK(removed.ErrorValue().code.Value() == SecurityErrors::CredentialNotFound.code.Value());
    CHECK(reader->Remove(reference.Value()).HasValue());
#else
    CHECK_FALSE(backend);
#endif
}

#if defined(_WIN32)
TEST_CASE("Persistent credential reference can be reopened in a fresh process", "[platform][credentials]") {
    using namespace Horo;
    const auto backend = Platform::CreateNativeCredentialBackend();
    REQUIRE(backend);
    if (!backend->Available())
        SKIP("This Windows logon does not permit machine-local user credentials.");

    struct Cleanup {
        const std::shared_ptr<Security::CredentialBackend> backend;
        std::optional<CredentialReference> reference;

        ~Cleanup() {
            if (reference)
                static_cast<void>(backend->Remove(*reference));
        }
    } cleanup{backend};

    constexpr std::array secret{std::byte{9}, std::byte{0}, std::byte{4}};
    {
        Security::PersistentCredentialStore store{backend, Platform::CreateNativeSecureRandomSource()};
        auto enrolled = store.Enroll(Security::SecureBytes{secret});
        REQUIRE(enrolled.HasValue());
        cleanup.reference.emplace(std::move(enrolled).Value());
    }
    const auto restored = CredentialReference::Parse(std::string{cleanup.reference->Value()});
    REQUIRE(restored.HasValue());
    Security::PersistentCredentialStore reopened{Platform::CreateNativeCredentialBackend(), {}};
    const auto bytes = reopened.Resolve(restored.Value(), 0);
    REQUIRE(bytes.HasValue());
    CHECK(std::ranges::equal(bytes.Value().View(), secret));

    NativeExternalProcessRunner runner;
    const ExternalProcessRequest probe{.executable = HORO_PROCESS_TEST_CHILD,
                                       .arguments = {"verify-credential-fixture", std::string{restored.Value().Value()}},
                                       .timeout = std::chrono::seconds{5}};
    const auto openedByChild = runner.Run(probe, {});
    REQUIRE(openedByChild.HasValue());
    CHECK(openedByChild.Value().reason == ProcessTerminationReason::Exited);
    CHECK(openedByChild.Value().exitCode == 0);

    REQUIRE(reopened.Revoke(restored.Value()).HasValue());
    CHECK(reopened.Revoke(restored.Value()).HasValue());
    const auto revoked = reopened.Resolve(restored.Value(), 0);
    REQUIRE(revoked.HasError());
    CHECK(revoked.ErrorValue().code.Value() == SecurityErrors::CredentialNotFound.code.Value());
    const auto rejectedByChild = runner.Run(probe, {});
    REQUIRE(rejectedByChild.HasValue());
    CHECK(rejectedByChild.Value().reason == ProcessTerminationReason::Exited);
    CHECK(rejectedByChild.Value().exitCode == 4);
}
#endif
