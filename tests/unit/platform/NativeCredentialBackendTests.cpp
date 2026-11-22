#include "Horo/Platform/NativeCredentialBackend.h"
#include "Horo/Platform/SecureRandom.h"
#include "Horo/Security/SecurityErrors.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

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
