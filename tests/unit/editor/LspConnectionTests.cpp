#include "../../support/SourceDocumentTestProject.h"
#include "Horo/Editor/SourceFileOpenService.h"
#include "Horo/Foundation/Platform.h"
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
    constexpr std::string_view FullSync = R"({"jsonrpc":"2.0","id":-1,"result":{"capabilities":{"textDocumentSync":1}}})";
    constexpr std::string_view IncrementalSync =
        R"({"jsonrpc":"2.0","id":-1,"result":{"capabilities":{"textDocumentSync":{"openClose":true,"change":2}}}})";
    constexpr std::string_view QuerySupport =
        R"({"jsonrpc":"2.0","id":-1,"result":{"capabilities":{"textDocumentSync":1,"hoverProvider":true,"referencesProvider":true,"codeActionProvider":{}}}})";

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
    Result<std::optional<LspIncoming>> Deliver(LspConnection &connection, const std::string_view body) {
        const auto frame = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + std::string{body};
        auto bytes = std::string_view{frame};
        auto result = connection.Read(bytes, Now);
        CHECK(bytes.empty());
        return result;
    }

    /** @brief Opens a native source document for pending-work tests. */
    SourceDocumentSnapshot OpenSource(TestSupport::SourceProject &project, SourceFileOpenService &open, const char *path = "source.cpp",
                                      const std::string_view text = "int speed;") {
        project.Put(text, path);
        auto result = open.Open({.path = path});
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().sourceSnapshot.has_value());
        return *result.Value().sourceSnapshot;
    }

    /** @brief Decodes a frame admitted by the transport through the actual wire codec. */
    LspMessage Inspect(std::string_view bytes) {
        LspFrameReader reader;
        auto frame = reader.Consume(bytes);
        REQUIRE(frame.HasValue());
        REQUIRE(frame.Value().has_value());
        CHECK(bytes.empty());
        auto decoded = DecodeLspMessage(*frame.Value());
        REQUIRE(decoded.HasValue());
        return std::move(decoded).Value();
    }

    /** @brief Generates a reply for an actual wire request ID. */
    std::string Reply(const SourceLanguageRequestId id, const std::string_view fields = R"("result":null)") {
        return "{\"jsonrpc\":\"2.0\",\"id\":\"" + std::to_string(id.request) + "\"," + std::string{fields} + "}";
    }

    /** @brief Owns one negotiated query connection with an initial synchronized source. */
    struct QueryConnection final {
        SourceLanguageSession source;
        TestTransport transport;
        LspConnection connection{source, transport};

        explicit QueryConnection(const SourceDocumentSnapshot &snapshot) {
            REQUIRE(connection.Start("file:///project", std::nullopt, Now).HasValue());
            REQUIRE(Deliver(connection, QuerySupport).HasValue());
            REQUIRE(connection.SyncSource(snapshot, "cpp").HasValue());
        }
    };

    /** @brief Reads the exact LSP version from a real outgoing open or change notification. */
    std::int32_t SynchronizedVersion(const TestTransport &transport) {
        const auto message = Inspect(transport.writes.back());
        return message.envelope.Get().at("params").at("textDocument").at("version").get<std::int32_t>();
    }

    /** @brief Forms a push notification with optional version and a valid or cleared diagnostic list. */
    std::string Publication(const std::string_view uri, const std::optional<std::int32_t> version, const bool empty = false) {
        nlohmann::json diagnostics = nlohmann::json::array();
        if (!empty)
            diagnostics.push_back({{"message", "test diagnostic"},
                                   {"range", {{"start", {{"line", 0}, {"character", 0}}}, {"end", {{"line", 0}, {"character", 3}}}}}});
        nlohmann::json params{{"uri", uri}, {"diagnostics", std::move(diagnostics)}};
        if (version)
            params["version"] = *version;
        return nlohmann::json{{"jsonrpc", "2.0"}, {"method", PublishDiagnosticsMethod}, {"params", std::move(params)}}.dump();
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

        constexpr std::string_view notification = R"({"jsonrpc":"2.0","method":"window/logMessage","params":{}})";
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
        CHECK(eligible.Value()->message.kind == LspMessageKind::Notification);

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

    TEST_CASE("LSP source synchronization sends full or UTF-16 incremental updates as negotiated") {
        for (const auto capability : {FullSync, IncrementalSync}) {
            CAPTURE(capability);
            TestSupport::SourceProject project;
            SourceFileOpenService open{project.directory};
            const std::string_view original = "one\r\n\xF0\x9F\x98\x80\xC4\xB1";
            project.Put(original, "source #%.cpp");
            const auto opened = open.Open({.path = "source #%.cpp"});
            REQUIRE(opened.HasValue());
            REQUIRE(opened.Value().sourceSnapshot.has_value());
            const auto snapshot = *opened.Value().sourceSnapshot;
            const auto instance = snapshot.Identity().instance;

            SourceLanguageSession source;
            TestTransport transport;
            LspConnection connection{source, transport};
            const auto stamp = connection.Start("file:///project%20root", std::nullopt, Now);
            REQUIRE(stamp.HasValue());
            CHECK(connection.SyncSource(snapshot, "cpp").HasError());
            REQUIRE(Deliver(connection, capability).HasValue());
            REQUIRE(connection.SyncSource(snapshot, "cpp").HasValue());
            const auto didOpen = Inspect(transport.writes.back());
            CHECK(didOpen.envelope.Get().at("method") == "textDocument/didOpen");
            const auto &document = didOpen.envelope.Get().at("params").at("textDocument");
            CHECK(document.at("uri") == "file:///project%20root/source%20%23%25.cpp");
            CHECK(document.at("languageId") == "cpp");
            CHECK(document.at("text").get_ref<const std::string &>() == original);
            CHECK(document.at("version") == snapshot.Revision());
            REQUIRE(connection.SyncSource(snapshot, "cpp").HasValue());
            CHECK(transport.writes.size() == 3);

            const auto request = source.Request(stamp.Value(), SourceLanguageRequestKind::Hover, snapshot);
            REQUIRE(request.HasValue());
            const auto changed = open.Documents().Edit(instance, {snapshot.Revision(), 0, original.size(), "new text"});
            REQUIRE(changed.HasValue());
            REQUIRE(connection.SyncSource(changed.Value(), "cpp").HasValue());
            const auto didChange = Inspect(transport.writes.back());
            CHECK(didChange.envelope.Get().at("method") == "textDocument/didChange");
            const auto &change = didChange.envelope.Get().at("params").at("contentChanges").at(0);
            CHECK(change.at("text") == "new text");
            if (capability == IncrementalSync) {
                CHECK(change.at("range").at("end").at("line") == 1);
                CHECK(change.at("range").at("end").at("character") == 3);
            } else {
                CHECK_FALSE(change.contains("range"));
            }
            CHECK(source.Complete(request.Value()).HasError());
        }
    }

    TEST_CASE("LSP rekeys close the previous URI before opening the new one") {
        TestSupport::SourceProject project;
        SourceFileOpenService open{project.directory};
        const auto snapshot = OpenSource(project, open);
        const auto instance = snapshot.Identity().instance;
        SourceLanguageSession source;
        TestTransport transport;
        LspConnection connection{source, transport};
        const auto stamp = connection.Start("file:///project%20root/", std::nullopt, Now);
        REQUIRE(stamp.HasValue());
        REQUIRE(Deliver(connection, FullSync).HasValue());
        REQUIRE(connection.SyncSource(snapshot, "cpp").HasValue());
        const auto request = source.Request(stamp.Value(), SourceLanguageRequestKind::Hover, snapshot);
        REQUIRE(request.HasValue());

        NativeDurableFileSystem files;
        REQUIRE(open.SaveAs({instance, snapshot.Revision()}, project.directory / "new name.cpp", files).HasValue());
        const auto renamed = open.Documents().Snapshot(instance);
        REQUIRE(renamed.HasValue());
        REQUIRE(connection.SyncSource(renamed.Value(), "cpp").HasValue());
        REQUIRE(transport.writes.size() == 5);
        const auto didRekeyClose = Inspect(transport.writes[3]);
        CHECK(didRekeyClose.envelope.Get().at("method") == "textDocument/didClose");
        CHECK(didRekeyClose.envelope.Get().at("params").at("textDocument").at("uri") == "file:///project%20root/source.cpp");
        const auto didRekeyOpen = Inspect(transport.writes[4]);
        CHECK(didRekeyOpen.envelope.Get().at("method") == "textDocument/didOpen");
        CHECK(didRekeyOpen.envelope.Get().at("params").at("textDocument").at("uri") == "file:///project%20root/new%20name.cpp");
        CHECK(source.Complete(request.Value()).HasError());

        REQUIRE(connection.CloseSource(instance).HasValue());
        CHECK_FALSE(source.SynchronizedSource(stamp.Value(), instance));
        CHECK(Inspect(transport.writes.back()).envelope.Get().at("method") == "textDocument/didClose");
        REQUIRE(connection.CloseSource(instance).HasValue());
        CHECK(transport.writes.size() == 6);
    }

    TEST_CASE("LSP synchronization leaves the peer baseline intact when a replacement exceeds the wire budget") {
        TestSupport::SourceProject project;
        SourceFileOpenService open{project.directory};
        const auto snapshot = OpenSource(project, open);
        SourceLanguageSession source;
        TestTransport transport;
        LspConnection connection{source, transport};
        const auto stamp = connection.Start("file:///project", std::nullopt, Now);
        REQUIRE(stamp.HasValue());
        REQUIRE(Deliver(connection, FullSync).HasValue());
        REQUIRE(connection.SyncSource(snapshot, "cpp").HasValue());
        const auto instance = snapshot.Identity().instance;
        const auto oversized = open.Documents().Edit(instance, {snapshot.Revision(), 0, snapshot.Text().size(),
                                                                std::string(MaximumLspBodyBytes / 2 + 1, '\t')});
        REQUIRE(oversized.HasValue());
        CHECK(connection.SyncSource(oversized.Value(), "cpp").HasError());
        const auto baseline = source.SynchronizedSource(stamp.Value(), instance);
        REQUIRE(baseline.has_value());
        CHECK(baseline->SharesStateWith(snapshot));
        CHECK(source.State() == SourceLanguageSessionState::Ready);
        CHECK(transport.writes.size() == 3);
        CHECK(transport.cancellations == 0);
    }

    TEST_CASE("LSP document requests use negotiated features and exact UTF-16 source positions") {
        TestSupport::SourceProject project;
        SourceFileOpenService open{project.directory};
        project.Put("x\r\n\xF0\x9F\x98\x80"
                    "yz",
                    "utf.cpp");
        auto opened = open.Open({.path = "utf.cpp"});
        REQUIRE(opened.HasValue());
        REQUIRE(opened.Value().sourceSnapshot.has_value());
        const auto snapshot = *opened.Value().sourceSnapshot;
        QueryConnection fixture{snapshot};
        auto &connection = fixture.connection;
        const auto &writes = fixture.transport.writes;
        using enum SourceLanguageRequestKind;
        CHECK(connection.Request(snapshot, Completion).HasError());
        CHECK(connection.Request(snapshot, Hover, {.offset = 2}).HasError());
        CHECK(connection.Request(snapshot, Hover, {.offset = 4}).HasError());
        CHECK(connection.Request(snapshot, CodeActions, {.offset = 7, .length = 9}).HasError());
        CHECK(writes.size() == 3);

        auto references = connection.Request(snapshot, References, {.offset = 7, .includeDeclaration = true});
        REQUIRE(references.HasValue());
        const auto wire = Inspect(writes.back());
        const auto &envelope = wire.envelope.Get();
        CHECK(envelope.at("method") == "textDocument/references");
        CHECK(envelope.at("id").get<std::string>() == std::to_string(references.Value().request));
        const auto &params = envelope.at("params");
        CHECK(params.at("position").at("line") == 1);
        CHECK(params.at("position").at("character") == 2);
        CHECK(params.at("context").at("includeDeclaration") == true);

        REQUIRE(connection.Request(snapshot, CodeActions, {.offset = 7, .length = 1}).HasValue());
        const auto actions = Inspect(writes.back());
        const auto &range = actions.envelope.Get().at("params").at("range");
        CHECK(range.at("start").at("character") == 2);
        CHECK(range.at("end").at("character") == 3);
    }

    TEST_CASE("LSP replies remain correlated across documents and discard cancelled duplicate and stale results") {
        TestSupport::SourceProject project;
        SourceFileOpenService open{project.directory};
        const auto first = OpenSource(project, open);
        const auto second = OpenSource(project, open, "second.cpp", "int second;");
        QueryConnection fixture{first};
        auto &connection = fixture.connection;
        REQUIRE(connection.SyncSource(second, "cpp").HasValue());
        using enum SourceLanguageRequestKind;
        auto stale = connection.Request(first, Hover);
        auto cancelled = connection.Request(second, Hover);
        auto live = connection.Request(second, References);
        REQUIRE(stale.HasValue());
        REQUIRE(cancelled.HasValue());
        REQUIRE(live.HasValue());

        REQUIRE(connection.CancelRequest(cancelled.Value()).HasValue());
        const auto cancellation = Inspect(fixture.transport.writes.back());
        CHECK(cancellation.envelope.Get().at("method") == "$/cancelRequest");
        CHECK(cancellation.envelope.Get().at("params").at("id").get<std::string>() == std::to_string(cancelled.Value().request));
        const auto instance = first.Identity().instance;
        REQUIRE(open.Documents().Edit(instance, {first.Revision(), 0, 3, "float"}).HasValue());
        CHECK_FALSE(Deliver(connection, Reply(stale.Value())).Value());
        CHECK_FALSE(Deliver(connection, Reply(cancelled.Value())).Value());
        CHECK_FALSE(Deliver(connection, R"({"jsonrpc":"2.0","id":"01","result":null})").Value());
        const auto result = Deliver(connection, Reply(live.Value(), R"("error":{"code":-32603,"message":"failed"})"));
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().has_value());
        REQUIRE(result.Value()->reply.has_value());
        CHECK(result.Value()->message.kind == LspMessageKind::Error);
        CHECK(result.Value()->reply->kind == References);
        CHECK(result.Value()->reply->source.SharesStateWith(second));
        CHECK_FALSE(Deliver(connection, Reply(live.Value())).Value());
    }

    TEST_CASE("LSP pushes only admit the current wire version and URI and support clearing diagnostics") {
        TestSupport::SourceProject project;
        SourceFileOpenService open{project.directory};
        const auto first = OpenSource(project, open);
        QueryConnection fixture{first};
        auto &connection = fixture.connection;
        const auto firstVersion = SynchronizedVersion(fixture.transport);
        const auto second = OpenSource(project, open, "second.cpp", "int second;");
        REQUIRE(connection.SyncSource(second, "cpp").HasValue());
        const auto secondVersion = SynchronizedVersion(fixture.transport);
        REQUIRE(secondVersion > firstVersion);
        constexpr std::string_view uri = "file:///project/source.cpp";
        CHECK_FALSE(Deliver(connection, Publication(uri, std::nullopt)).Value());
        CHECK_FALSE(Deliver(connection, Publication(uri, secondVersion)).Value());
        CHECK_FALSE(Deliver(connection, Publication("file:///outside/source.cpp", firstVersion)).Value());

        const auto routed = Deliver(connection, Publication(uri, firstVersion));
        REQUIRE(routed.HasValue());
        REQUIRE(routed.Value().has_value());
        const auto projected = connection.ProjectReply(*routed.Value());
        REQUIRE(projected.HasValue());
        CHECK(projected.Value().context.source.SharesStateWith(first));
        REQUIRE(std::get<LanguageDiagnostics>(projected.Value().payload).items.size() == 1);

        const auto cleared = Deliver(connection, Publication(uri, firstVersion, true));
        REQUIRE(cleared.HasValue());
        REQUIRE(cleared.Value().has_value());
        const auto clear = connection.ProjectReply(*cleared.Value());
        REQUIRE(clear.HasValue());
        CHECK(std::get<LanguageDiagnostics>(clear.Value().payload).items.empty());
        REQUIRE(open.Documents().Edit(first.Identity().instance, {first.Revision(), 0, 3, "float"}).HasValue());
        CHECK_FALSE(Deliver(connection, Publication(uri, firstVersion)).Value());
        CHECK(connection.ProjectReply(*routed.Value()).HasError());
    }

    TEST_CASE("LSP diagnostic wire versions are never reused after reopening the same URI") {
        TestSupport::SourceProject project;
        SourceFileOpenService open{project.directory};
        const auto first = OpenSource(project, open);
        QueryConnection fixture{first};
        const auto previous = SynchronizedVersion(fixture.transport);
        const auto firstInstance = first.Identity().instance;
        REQUIRE(fixture.connection.CloseSource(firstInstance).HasValue());
        REQUIRE(open.CloseDocument(firstInstance).HasValue());
        const auto reopened = OpenSource(project, open);
        REQUIRE(reopened.Revision() == first.Revision());
        REQUIRE(reopened.Identity().instance != firstInstance);
        REQUIRE(fixture.connection.SyncSource(reopened, "cpp").HasValue());
        const auto current = SynchronizedVersion(fixture.transport);
        REQUIRE(current > previous);
        constexpr std::string_view uri = "file:///project/source.cpp";
        CHECK_FALSE(Deliver(fixture.connection, Publication(uri, previous)).Value());
        const auto fresh = Deliver(fixture.connection, Publication(uri, current));
        REQUIRE(fresh.HasValue());
        REQUIRE(fresh.Value().has_value());
        const auto typed = fixture.connection.ProjectReply(*fresh.Value());
        REQUIRE(typed.HasValue());
        CHECK(typed.Value().context.source.SharesStateWith(reopened));
    }
}  // namespace
