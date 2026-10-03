#include "Horo/Foundation/Sha256.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace {
    [[nodiscard]] std::span<const std::byte> AsBytes(const std::string_view value) noexcept {
        return {reinterpret_cast<const std::byte *>(value.data()), value.size()};
    }

    [[nodiscard]] std::string HashRepeatedA(const std::size_t count) {
        const std::string input(count, 'a');
        return Horo::FormatSha256(Horo::ComputeSha256(AsBytes(input)));
    }

    static_assert(noexcept(Horo::ComputeSha256(std::span<const std::byte>{})), "SHA-256 computation must not allocate or throw.");

    TEST_CASE("SHA-256 Matches FIPS Known Answer Vectors", "[unit][foundation][sha256]") {
        REQUIRE((Horo::FormatSha256(Horo::ComputeSha256({})) == "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
        REQUIRE((Horo::FormatSha256(Horo::ComputeSha256(AsBytes("abc"))) ==
                 "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

        constexpr std::array binary{
            std::byte{0x00}, std::byte{0xff}, std::byte{0x10}, std::byte{0x80}, std::byte{0x7f}, std::byte{0x01},
        };
        REQUIRE(
            (Horo::FormatSha256(Horo::ComputeSha256(binary)) == "sha256:30a54680895044a463a2b3050fbee88a0ae4bba183d1b9bfb77b46b5e7e91a72"));
    }

    TEST_CASE("SHA-256 Matches The NIST Million-A Vector", "[unit][foundation][sha256]") {
        REQUIRE((HashRepeatedA(1'000'000) == "sha256:cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    }

    TEST_CASE("SHA-256 Computation Is Deterministic", "[unit][foundation][sha256]") {
        const auto input = AsBytes("deterministic input");

        REQUIRE((Horo::ComputeSha256(input) == Horo::ComputeSha256(input)));
    }

    TEST_CASE("SHA-256 Handles Padding Boundaries", "[unit][foundation][sha256]") {
        REQUIRE((HashRepeatedA(55) == "sha256:9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"));
        REQUIRE((HashRepeatedA(56) == "sha256:b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"));
        REQUIRE((HashRepeatedA(63) == "sha256:7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"));
        REQUIRE((HashRepeatedA(64) == "sha256:ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"));
        REQUIRE((HashRepeatedA(65) == "sha256:635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"));
    }

    TEST_CASE("SHA-256 builder hashes fragments across block boundaries without retaining input", "[unit][foundation][sha256]") {
        const std::string bytes(1'000'000, 'a');
        Horo::Sha256Builder builder;
        for (std::size_t offset = 0; offset < bytes.size(); offset += 7U)
            REQUIRE(builder.Update(AsBytes(std::string_view{bytes}.substr(offset, 7U))));
        const auto digest = builder.Finalize();
        CHECK(Horo::FormatSha256(digest) == "sha256:cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
        CHECK(builder.Finalize() == digest);
        CHECK_FALSE(builder.Update(AsBytes("more")));
    }

    TEST_CASE("SHA-256 Text Round Trips Canonically", "[unit][foundation][sha256]") {
        const Horo::Sha256Digest digest = Horo::ComputeSha256(AsBytes("round trip"));
        const std::string text = Horo::FormatSha256(digest);
        const Horo::Result<Horo::Sha256Digest> parsed = Horo::ParseSha256(text);

        REQUIRE((text.size() == 71));
        REQUIRE((text.starts_with("sha256:")));
        REQUIRE((parsed.HasValue()));
        REQUIRE((parsed.Value() == digest));
    }

    TEST_CASE("SHA-256 Parser Rejects Noncanonical Text With Stable Typed Error", "[unit][foundation][sha256]") {
        constexpr std::string_view valid = "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

        const auto CheckRejected = [](const std::string_view text) {
            const Horo::Result<Horo::Sha256Digest> parsed = Horo::ParseSha256(text);
            REQUIRE((parsed.HasError()));
            REQUIRE((parsed.ErrorValue().domain.Value() == "horo.foundation.hashing"));
            REQUIRE((parsed.ErrorValue().code.Value() == "foundation.sha256.invalid_text"));
            REQUIRE((parsed.ErrorValue().message == "SHA-256 text is not canonical."));
            REQUIRE((parsed.ErrorValue().message.find(text) == std::string::npos));
        };

        CheckRejected(valid.substr(0, valid.size() - 1));
        CheckRejected("sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad0");
        CheckRejected("SHA256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        CheckRejected("sha256:Ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        CheckRejected("sha256:ga7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    }
}  // namespace
