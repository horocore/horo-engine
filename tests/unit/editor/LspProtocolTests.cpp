#include "LspProtocol.h"

#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo::Editor::Language;

    /** @brief Wraps precisely the supplied bytes in an LSP frame. */
    std::string Frame(const std::string_view body) {
        return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + std::string{body};
    }
}  // namespace

TEST_CASE("LSP framing counts UTF-8 bytes and survives every two-chunk split") {
    const std::string_view body = "{\"jsonrpc\":\"2.0\",\"method\":\"window/logMessage\",\"params\":{\"message\":\"\xC4\xB1\"}}";
    const auto frame = Frame(body);
    for (std::size_t split = 0; split < frame.size(); ++split) {
        CAPTURE(split);
        LspFrameReader reader;
        auto head = std::string_view{frame}.substr(0, split);
        const auto first = reader.Consume(head);
        REQUIRE(first.HasValue());
        CHECK_FALSE(first.Value());
        CHECK(head.empty());

        auto tail = std::string_view{frame}.substr(split);
        const auto second = reader.Consume(tail);
        REQUIRE(second.HasValue());
        CHECK(second.Value() == body);
        CHECK(tail.empty());
        CHECK(reader.EndOfStream().HasValue());
    }
}

TEST_CASE("LSP framing returns one message and leaves pipelined frames with the caller") {
    LspFrameReader reader;
    const auto pipelined = "cOnTeNt-LeNgTh:\t 2 \r\nContent-Type: application/vscode-jsonrpc; charset=utf-8\r\n\r\n{}" + Frame("[]");
    auto input = std::string_view{pipelined};
    const auto first = reader.Consume(input);
    REQUIRE(first.HasValue());
    CHECK(first.Value() == "{}");
    CHECK(input == Frame("[]"));
    const auto second = reader.Consume(input);
    REQUIRE(second.HasValue());
    CHECK(second.Value() == "[]");
    CHECK(input.empty());
    CHECK(reader.EndOfStream().HasValue());

    input = "Content-Length: 2\r\nContent-Type: application/vscode-jsonrpc; charset=\"UTF8\"\r\n\r\n{}";
    const auto legacy = reader.Consume(input);
    REQUIRE(legacy.HasValue());
    CHECK(legacy.Value() == "{}");
}

TEST_CASE("Malformed LSP framing permanently faults only that connection") {
    const std::string_view malformed[] = {"\r\n\r\n{}",
                                          "X-Other: 2\r\n\r\n{}",
                                          "Content-Length: \r\n\r\n",
                                          "Content-Length: 0\r\n\r\n",
                                          "Content-Length: +2\r\n\r\n{}",
                                          "Content-Length: 2x\r\n\r\n{}",
                                          "Content-Length: 4194305\r\n\r\n",
                                          "Content-Length: 9999999999999999999999999999\r\n\r\n",
                                          "Content-Length: 2\r\ncontent-length: 2\r\n\r\n{}",
                                          "Content-Length: 2\r\nContent-Type: application/vscode-jsonrpc; charset=utf-16\r\n\r\n{}",
                                          "Content-Length: 2\r\nBroken Header: x\r\n\r\n{}"};
    for (auto input : malformed) {
        CAPTURE(input);
        LspFrameReader reader;
        CHECK(reader.Consume(input).HasError());
        CHECK(reader.EndOfStream().HasError());
        auto valid = std::string_view{"Content-Length: 2\r\n\r\n{}"};
        CHECK(reader.Consume(valid).HasError());
        CHECK(valid == "Content-Length: 2\r\n\r\n{}");
    }
    const std::string oversizedHeader(MaximumLspHeaderBytes + 1, 'x');
    auto input = std::string_view{oversizedHeader};
    LspFrameReader reader;
    CHECK(reader.Consume(input).HasError());
    CHECK(input == "x");
}

TEST_CASE("LSP EOF detects a partial header or body") {
    LspFrameReader reader;
    CHECK(reader.EndOfStream().HasValue());
    auto input = std::string_view{"Content-Len"};
    REQUIRE(reader.Consume(input).HasValue());
    CHECK(reader.EndOfStream().HasError());

    LspFrameReader bodyReader;
    input = "Content-Length: 2\r\n\r\n{";
    const auto body = bodyReader.Consume(input);
    REQUIRE(body.HasValue());
    CHECK_FALSE(body.Value());
    CHECK(bodyReader.EndOfStream().HasError());
    input = "}";
    CHECK(bodyReader.Consume(input).HasError());
}

TEST_CASE("LSP decoding preserves numeric and string IDs, null results, and uncorrelated errors") {
    const auto result = DecodeLspMessage(R"({"jsonrpc":"2.0","id":18446744073709551615,"result":null})");
    REQUIRE(result.HasValue());
    CHECK(result.Value().kind == LspMessageKind::Result);
    CHECK(result.Value().envelope.Get()["id"].is_number_unsigned());
    CHECK(result.Value().envelope.Get()["result"].is_null());

    const auto failure = DecodeLspMessage(R"({"jsonrpc":"2.0","id":null,"error":{"code":-32700,"message":"Parse error"}})");
    REQUIRE(failure.HasValue());
    CHECK(failure.Value().kind == LspMessageKind::Error);

    const auto request = DecodeLspMessage(R"({"jsonrpc":"2.0","id":"server-1","method":"workspace/configuration","params":{}})");
    REQUIRE(request.HasValue());
    CHECK(request.Value().kind == LspMessageKind::Request);
    CHECK(request.Value().envelope.Get()["id"] == "server-1");

    const auto notice = DecodeLspMessage(R"({"jsonrpc":"2.0","method":"textDocument/publishDiagnostics"})");
    REQUIRE(notice.HasValue());
    CHECK(notice.Value().kind == LspMessageKind::Notification);
}

TEST_CASE("LSP decoding rejects invalid syntax, conflicting envelopes, and unbounded inputs") {
    const std::string_view invalid[] = {"[]",
                                        "{",
                                        "{\"jsonrpc\":\"2.0\",\"method\":\"\xff\"}",
                                        R"({"jsonrpc":"2.0","id":1,"id":2,"result":null})",
                                        R"({"jsonrpc":"2.0","id":1,"result":null,"error":{"code":-1,"message":"conflicting"}})",
                                        R"({"jsonrpc":"2.0","id":null,"result":null})",
                                        R"({"jsonrpc":"2.0","id":1.25,"result":null})",
                                        R"({"jsonrpc":"2.0","id":1,"error":{"code":-1,"message":false}})",
                                        R"({"jsonrpc":"2.0","method":"textDocument/publishDiagnostics","params":1})",
                                        R"({"jsonrpc":"2.0","method":"textDocument/publishDiagnostics","result":[]})",
                                        R"({"jsonrpc":"1.0","id":1,"result":null})"};
    for (const auto body : invalid) {
        CAPTURE(body);
        CHECK(DecodeLspMessage(body).HasError());
    }
    const auto nested = R"({"jsonrpc":"2.0","id":1,"result":)" + std::string(80, '[') + std::string(80, ']') + "}";
    CHECK(DecodeLspMessage(nested).HasError());
    CHECK(DecodeLspMessage(std::string(MaximumLspBodyBytes + 1, ' ')).HasError());
    constexpr char embeddedNul[] = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":null}\0{}";
    CHECK(DecodeLspMessage(std::string_view{embeddedNul, sizeof(embeddedNul) - 1}).HasError());
}
