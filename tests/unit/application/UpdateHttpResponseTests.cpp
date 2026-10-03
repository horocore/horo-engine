#include "UpdateHttpResponse.h"

#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <string>
#include <string_view>

using Horo::Release::UpdateHttpResponseHeaders;
using Horo::Release::UpdateTransferResponse;

namespace {
    constexpr std::string_view Url = "https://updates.example.test/editor.zip";

    [[nodiscard]] std::optional<UpdateTransferResponse> Complete(UpdateHttpResponseHeaders &parser,
                                                                 const std::initializer_list<std::string_view> lines,
                                                                 const std::string_view effectiveUrl = Url) {
        std::optional<UpdateTransferResponse> response;
        for (const auto line : lines) {
            auto result = parser.Feed(line, effectiveUrl);
            REQUIRE(result.HasValue());
            response = std::move(result).Value();
        }
        return response;
    }
}  // namespace

TEST_CASE("HTTP response parser admits an exact fresh response only after complete headers", "[release][update]") {
    UpdateHttpResponseHeaders parser{std::string{Url}};
    const auto response =
        Complete(parser, {"HTTP/1.1 200 OK\r\n", "Content-Length: 4\r\n", "ETag: \"v1\"\r\n", "Content-Encoding: identity\r\n", "\r\n"});
    REQUIRE(response);
    CHECK(response->status == 200U);
    CHECK(response->requestedUrl == Url);
    CHECK(response->effectiveUrl == Url);
    CHECK(response->contentLength == 4U);
    CHECK(response->strongEtag == "\"v1\"");
    CHECK_FALSE(response->rangeStart);
    CHECK(parser.Feed("\r\n", Url).HasError());
}

TEST_CASE("HTTP response parser preserves an exact partial range after interim headers", "[release][update]") {
    UpdateHttpResponseHeaders parser{std::string{Url}};
    const auto response = Complete(parser, {"HTTP/1.1 100 Continue\r\n", "\r\n", "HTTP/1.1 206 Partial Content\r\n",
                                            "Content-Length: 3\r\n", "Content-Range: bytes 2-4/5\r\n", "ETag: \"v1\"\r\n", "\r\n"});
    REQUIRE(response);
    CHECK(response->status == 206U);
    CHECK(response->contentLength == 3U);
    CHECK(response->rangeStart == 2U);
    CHECK(response->rangeEnd == 4U);
    CHECK(response->rangeTotal == 5U);
}

TEST_CASE("HTTP response parser rejects ambiguous or unsafe framing", "[release][update]") {
    for (const std::string_view field :
         {"Content-Length: 4\r\n", "Transfer-Encoding: chunked\r\n", "Content-Encoding: gzip\r\n", "Content-Range: bytes */5\r\n"}) {
        UpdateHttpResponseHeaders parser{std::string{Url}};
        REQUIRE(parser.Feed("HTTP/1.1 200 OK\r\n", Url).HasValue());
        REQUIRE(parser.Feed("Content-Length: 5\r\n", Url).HasValue());
        CHECK(parser.Feed(field, Url).HasError());
    }
}

TEST_CASE("HTTP response parser rejects oversized or incomplete metadata", "[release][update]") {
    UpdateHttpResponseHeaders parser{std::string{Url}};
    CHECK(parser.Feed("HTTP/1.1 200 OK", Url).HasError());

    UpdateHttpResponseHeaders tooLong{std::string{Url}};
    CHECK(tooLong.Feed(std::string(8193U, 'x'), Url).HasError());

    UpdateHttpResponseHeaders noLength{std::string{Url}};
    REQUIRE(noLength.Feed("HTTP/1.1 200 OK\r\n", Url).HasValue());
    CHECK(noLength.Feed("\r\n", Url).HasError());

    UpdateHttpResponseHeaders noEffectiveUrl{std::string{Url}};
    REQUIRE(noEffectiveUrl.Feed("HTTP/1.1 200 OK\r\n", Url).HasValue());
    REQUIRE(noEffectiveUrl.Feed("Content-Length: 5\r\n", Url).HasValue());
    CHECK(noEffectiveUrl.Feed("\r\n", {}).HasError());
}
