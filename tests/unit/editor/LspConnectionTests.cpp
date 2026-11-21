#include "../../support/SourceDocumentTestProject.h"
#include "Horo/Editor/SourceFileOpenService.h"
#include "LspConnection.h"

#include <catch2/catch_test_macros.hpp>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Editor::Language;

    constexpr auto Now = LspConnection::Clock::time_point{};
    constexpr std::string_view InitializeResult = R"({"jsonrpc":"2.0","id":-1,"result":{"capabilities":{}}})";
    constexpr std::string_view ShutdownResult = R"({"jsonrpc":"2.0","id":-2,"result":null})";

    /** @brief Records the complete frames offered to the process worker. */
    struct TestTransport final : ILspTransport {
        Result<void> Enqueue(std::string frame) override {
            if (reject)
                return Result<void>::Failure(MakeError(SourceDocumentErrors::Busy));
            writes.push_back(std::move(frame));
            return Result<void>::Success();
        }

        void Cancel() noexcept override {
            ++cancellations;
        }

        std::vector<std::string> writes;
        bool reject{};
        int cancellations{};
    };

    /** @brief Delivers a complete UTF-8 body over the same byte path used by the process. */
    Result<std::optional<LspMessage>> Deliver(LspConnection &connection, const std::string_view body) {
        const auto frame = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + std::string{body};
        auto bytes = std::string_view{frame};
        auto result = connection.Read(bytes, Now);
        CHECK(bytes.empty());
        return result;
    }

    /** @brief Opens a native source document for pending-work tests. */
    SourceDocumentSnapshot OpenSource(TestSupport::SourceProject &project, SourceFileOpenService &open) {
        project.Put("int speed;", "source.cpp");
        auto result = open.Open({.path = "source.cpp"});
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().sourceSnapshot.has_value());
        return *result.Value().sourceSnapshot;
    }

    TEST_CASE("LSP transport gates feature dispatch until initialized is queued and retires work during shutdown") {
        SourceLanguageSession source;
        TestTransport transport;
        LspConnection connection{source, transport};
        const auto stamp = connection.Start("file:///project", 21, Now);
        REQUIRE(stamp.HasValue());
        CHECK(source.State() == SourceLanguageSessionState::Initializing);
        REQUIRE(transport.writes.size() == 1);
        CHECK(transport.writes.back().find("\"method\":\"initialize\"") != std::string::npos);

        constexpr std::string_view notification = R"({"jsonrpc":"2.0","method":"textDocument/publishDiagnostics","params":{}})";
        const auto tooSoon = Deliver(connection, notification);
        REQUIRE(tooSoon.HasValue());
        CHECK_FALSE(tooSoon.Value());
        REQUIRE(Deliver(connection, InitializeResult).HasValue());
        REQUIRE(transport.writes.size() == 2);
        CHECK(transport.writes.back().find("\"method\":\"initialized\"") != std::string::npos);
        CHECK(source.State() == SourceLanguageSessionState::Ready);
        const auto eligible = Deliver(connection, notification);
        REQUIRE(eligible.HasValue());
        REQUIRE(eligible.Value().has_value());
        CHECK(eligible.Value()->kind == LspMessageKind::Notification);

        REQUIRE(connection.Shutdown(Now).HasValue());
        CHECK(source.State() == SourceLanguageSessionState::Disconnected);
        const auto tooLate = Deliver(connection, notification);
        REQUIRE(tooLate.HasValue());
        CHECK_FALSE(tooLate.Value());
        REQUIRE(Deliver(connection, ShutdownResult).HasValue());
        REQUIRE(transport.writes.size() == 4);
        CHECK(transport.writes.back().find("\"method\":\"exit\"") != std::string::npos);
        CHECK(connection.TransportStopped(true).HasValue());
        CHECK(transport.cancellations == 0);
        CHECK(connection.Shutdown(Now).HasValue());
    }

    TEST_CASE("Malformed LSP transport retires pending source requests without affecting its replacement") {
        TestSupport::SourceProject project;
        SourceFileOpenService open{project.directory};
        const auto snapshot = OpenSource(project, open);
        SourceLanguageSession source;
        TestTransport oldTransport;
        LspConnection old{source, oldTransport};
        const auto first = old.Start({}, std::nullopt, Now);
        REQUIRE(first.HasValue());
        REQUIRE(Deliver(old, InitializeResult).HasValue());
        REQUIRE(source.Sync(first.Value(), snapshot).HasValue());
        const auto pending = source.Request(first.Value(), SourceLanguageRequestKind::Hover, snapshot);
        REQUIRE(pending.HasValue());
        CHECK(Deliver(old, "{").HasError());
        CHECK(source.Complete(pending.Value()).HasError());
        CHECK(oldTransport.cancellations == 1);

        TestTransport newTransport;
        LspConnection replacement{source, newTransport};
        const auto second = replacement.Start({}, std::nullopt, Now);
        REQUIRE(second.HasValue());
        CHECK(second.Value() > first.Value());
        CHECK(old.TransportStopped(false).HasValue());
        CHECK(source.State() == SourceLanguageSessionState::Initializing);
        REQUIRE(Deliver(replacement, InitializeResult).HasValue());
        REQUIRE(source.Sync(second.Value(), snapshot).HasValue());
        const auto current = source.Request(second.Value(), SourceLanguageRequestKind::Hover, snapshot);
        REQUIRE(current.HasValue());
        CHECK(current.Value().request == pending.Value().request);
        CHECK(source.Complete(pending.Value()).HasError());
        CHECK(source.Complete(current.Value()).HasValue());
    }

    TEST_CASE("LSP transport refusal cannot publish readiness") {
        SourceLanguageSession source;
        TestTransport transport;
        LspConnection connection{source, transport};
        REQUIRE(connection.Start({}, std::nullopt, Now).HasValue());
        transport.reject = true;
        CHECK(Deliver(connection, InitializeResult).HasError());
        CHECK(source.State() == SourceLanguageSessionState::Disconnected);
        CHECK(transport.cancellations == 1);
        CHECK(connection.TransportStopped(false).HasValue());
    }

    TEST_CASE("LSP transport distinguishes expiry and truncated process output from clean shutdown") {
        SourceLanguageSession source;
        TestTransport timeoutTransport;
        LspConnection timeout{source, timeoutTransport};
        REQUIRE(timeout.Start({}, std::nullopt, Now).HasValue());
        CHECK(timeout.Expire(Now + LspLifecycle::InitializeTimeout));
        CHECK_FALSE(timeout.Expire(Now + LspLifecycle::InitializeTimeout));
        CHECK(source.State() == SourceLanguageSessionState::Disconnected);
        CHECK(timeoutTransport.cancellations == 1);

        TestTransport truncatedTransport;
        LspConnection truncated{source, truncatedTransport};
        REQUIRE(truncated.Start({}, std::nullopt, Now).HasValue());
        auto partial = std::string_view{"Content-Length: 3\r\n\r\n{"};
        REQUIRE(truncated.Read(partial, Now).HasValue());
        CHECK(truncated.TransportStopped(true).HasError());
        CHECK(source.State() == SourceLanguageSessionState::Disconnected);
        CHECK(truncatedTransport.cancellations == 0);
    }
}  // namespace
