#include "LspLifecycle.h"

#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo::Editor::Language;
    constexpr auto Now = LspLifecycle::Clock::time_point{};
    constexpr std::string_view InitializeResponse = R"({"jsonrpc":"2.0","id":-1,"result":{"capabilities":{}}})";
    constexpr std::string_view ShutdownResponse = R"({"jsonrpc":"2.0","id":-2,"result":null})";

    /** @brief Decodes the frame through the same reader a language server connection uses. */
    LspMessage ReadFrame(std::string_view frame) {
        LspFrameReader reader;
        auto body = reader.Consume(frame);
        REQUIRE(body.HasValue());
        REQUIRE(body.Value().has_value());
        CHECK(frame.empty());
        CHECK(reader.EndOfStream().HasValue());
        auto message = DecodeLspMessage(*body.Value());
        REQUIRE(message.HasValue());
        return std::move(message).Value();
    }

    /** @brief Delivers a response or notification that passed structural JSON-RPC validation. */
    Horo::Result<LspLifecycleDispatch> Receive(LspLifecycle &lifecycle, const std::string_view body,
                                               const LspLifecycle::Clock::time_point now = Now) {
        auto message = DecodeLspMessage(body);
        REQUIRE(message.HasValue());
        return lifecycle.Receive(message.Value(), now);
    }
}  // namespace

TEST_CASE("LSP lifecycle orders initialize initialized shutdown and exit on one connection") {
    LspLifecycle lifecycle;
    CHECK(lifecycle.Shutdown(Now).HasError());
    const auto initialization = lifecycle.Start("file:///project%20root", 42, Now);
    REQUIRE(initialization.HasValue());
    const auto start = ReadFrame(initialization.Value());
    CHECK(start.kind == LspMessageKind::Request);
    const auto &request = start.envelope.Get();
    CHECK(request.at("id") == -1);
    CHECK(request.at("method") == "initialize");
    CHECK(request.at("params").at("processId") == 42);
    CHECK(request.at("params").at("rootUri") == "file:///project%20root");
    CHECK(request.at("params").at("capabilities").at("general").at("positionEncodings").at(0) == "utf-16");
    CHECK(lifecycle.State() == LspLifecycleState::Initializing);
    CHECK(lifecycle.Start({}, std::nullopt, Now).HasError());
    CHECK(lifecycle.Shutdown(Now).HasError());

    const auto initialized = Receive(lifecycle, InitializeResponse);
    REQUIRE(initialized.HasValue());
    REQUIRE(initialized.Value().outgoing.has_value());
    CHECK(initialized.Value().handled);
    const auto notification = ReadFrame(*initialized.Value().outgoing);
    CHECK(notification.kind == LspMessageKind::Notification);
    CHECK(notification.envelope.Get().at("method") == "initialized");
    CHECK(notification.envelope.Get().at("params").is_object());
    CHECK(lifecycle.State() == LspLifecycleState::Ready);

    const auto work = Receive(lifecycle, R"({"jsonrpc":"2.0","id":1,"result":[]})");
    REQUIRE(work.HasValue());
    CHECK_FALSE(work.Value().handled);
    CHECK_FALSE(work.Value().outgoing);
    CHECK_FALSE(lifecycle.Expire(Now + LspLifecycle::InitializeTimeout));

    const auto shutdown = lifecycle.Shutdown(Now);
    REQUIRE(shutdown.HasValue());
    const auto stop = ReadFrame(shutdown.Value());
    CHECK(stop.kind == LspMessageKind::Request);
    CHECK(stop.envelope.Get().at("method") == "shutdown");
    CHECK(stop.envelope.Get().at("id") == -2);
    CHECK(lifecycle.State() == LspLifecycleState::ShuttingDown);
    CHECK(lifecycle.Shutdown(Now).HasError());

    const auto exit = Receive(lifecycle, ShutdownResponse);
    REQUIRE(exit.HasValue());
    REQUIRE(exit.Value().outgoing.has_value());
    const auto finish = ReadFrame(*exit.Value().outgoing);
    CHECK(exit.Value().handled);
    CHECK(finish.kind == LspMessageKind::Notification);
    CHECK(finish.envelope.Get().at("method") == "exit");
    CHECK(lifecycle.State() == LspLifecycleState::Exiting);
    lifecycle.TransportStopped(true);
    CHECK(lifecycle.State() == LspLifecycleState::Stopped);
    CHECK(lifecycle.Start({}, std::nullopt, Now).HasError());
}

TEST_CASE("LSP lifecycle isolates handshake responses from server calls and late replies") {
    LspLifecycle lifecycle;
    REQUIRE(lifecycle.Start({}, std::nullopt, Now).HasValue());
    const auto unrelatedCall = Receive(lifecycle, R"({"jsonrpc":"2.0","id":-1,"method":"window/showMessageRequest"})");
    REQUIRE(unrelatedCall.HasValue());
    CHECK(unrelatedCall.Value().handled);
    REQUIRE(unrelatedCall.Value().outgoing.has_value());
    const auto decline = ReadFrame(*unrelatedCall.Value().outgoing);
    CHECK(decline.envelope.Get().at("id") == -1);
    CHECK(decline.envelope.Get().at("result").is_null());
    const auto unexpectedShutdown = Receive(lifecycle, ShutdownResponse);
    REQUIRE(unexpectedShutdown.HasValue());
    CHECK(unexpectedShutdown.Value().handled);
    CHECK_FALSE(unexpectedShutdown.Value().outgoing);
    CHECK(lifecycle.State() == LspLifecycleState::Initializing);

    REQUIRE(Receive(lifecycle, InitializeResponse).HasValue());
    const auto duplicate = Receive(lifecycle, InitializeResponse);
    REQUIRE(duplicate.HasValue());
    CHECK(duplicate.Value().handled);
    CHECK_FALSE(duplicate.Value().outgoing);
    CHECK(lifecycle.State() == LspLifecycleState::Ready);
    lifecycle.TransportStopped(false);
    CHECK(lifecycle.State() == LspLifecycleState::Failed);
}

TEST_CASE("LSP lifecycle fails malformed or refused handshake results without starting feature work") {
    const std::string_view invalid[] = {R"({"jsonrpc":"2.0","id":-1,"result":null})",
                                        R"({"jsonrpc":"2.0","id":-1,"result":{"capabilities":{"positionEncoding":"utf-32"}}})",
                                        R"({"jsonrpc":"2.0","id":-1,"error":{"code":-32603,"message":"refused"}})"};
    for (const auto body : invalid) {
        CAPTURE(body);
        LspLifecycle lifecycle;
        REQUIRE(lifecycle.Start({}, std::nullopt, Now).HasValue());
        CHECK(Receive(lifecycle, body).HasError());
        CHECK(lifecycle.State() == LspLifecycleState::Failed);
        CHECK(lifecycle.Shutdown(Now).HasError());
    }
    LspLifecycle lifecycle;
    REQUIRE(lifecycle.Start({}, std::nullopt, Now).HasValue());
    REQUIRE(Receive(lifecycle, InitializeResponse).HasValue());
    REQUIRE(lifecycle.Shutdown(Now).HasValue());
    CHECK(Receive(lifecycle, R"({"jsonrpc":"2.0","id":-2,"result":{}})").HasError());
    CHECK(lifecycle.State() == LspLifecycleState::Failed);
}

TEST_CASE("LSP lifecycle bounds initialization shutdown and exit including a silent peer") {
    LspLifecycle initializing;
    REQUIRE(initializing.Start({}, std::nullopt, Now).HasValue());
    CHECK_FALSE(initializing.Expire(Now + LspLifecycle::InitializeTimeout - std::chrono::milliseconds{1}));
    CHECK(Receive(initializing, InitializeResponse, Now + LspLifecycle::InitializeTimeout).HasError());
    CHECK_FALSE(initializing.Expire(Now + LspLifecycle::InitializeTimeout));
    CHECK(initializing.State() == LspLifecycleState::Failed);

    for (const bool exitAcknowledged : {false, true}) {
        CAPTURE(exitAcknowledged);
        LspLifecycle closing;
        REQUIRE(closing.Start({}, std::nullopt, Now).HasValue());
        REQUIRE(Receive(closing, InitializeResponse).HasValue());
        REQUIRE(closing.Shutdown(Now).HasValue());
        if (exitAcknowledged)
            REQUIRE(Receive(closing, ShutdownResponse).HasValue());
        CHECK(closing.Expire(Now + LspLifecycle::ShutdownTimeout));
        CHECK(closing.State() == LspLifecycleState::Failed);
        closing.TransportStopped(true);
        CHECK(closing.State() == LspLifecycleState::Failed);
    }
}

TEST_CASE("LSP lifecycle rejects invalid launch metadata before opening a connection") {
    LspLifecycle lifecycle;
    CHECK(lifecycle.Start(std::string(LspLifecycle::MaximumRootUriBytes + 1, 'x'), std::nullopt, Now).HasError());
    constexpr char embeddedNul[] = "file:///a\0b";
    CHECK(lifecycle.Start(std::string_view{embeddedNul, sizeof(embeddedNul) - 1}, std::nullopt, Now).HasError());
    CHECK(lifecycle.Start("file:///\xff", std::nullopt, Now).HasError());
    CHECK(lifecycle.Start({}, 0, Now).HasError());
    CHECK(lifecycle.State() == LspLifecycleState::Idle);
    const auto rootless = lifecycle.Start({}, std::nullopt, Now);
    REQUIRE(rootless.HasValue());
    const auto frame = ReadFrame(rootless.Value());
    CHECK(frame.envelope.Get().at("params").at("rootUri").is_null());
    CHECK(frame.envelope.Get().at("params").at("processId").is_null());
    const auto &capabilities = frame.envelope.Get().at("params").at("capabilities");
    CHECK(capabilities.at("textDocument").at("publishDiagnostics").at("versionSupport") == true);
}
