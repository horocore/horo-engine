#include "../../../apps/HoroEditor/app/EditorLocalAgentContext.h"
#include "../../support/SourceDocumentTestProject.h"

#include <catch2/catch_test_macros.hpp>
#include <future>

namespace Horo::Editor {
    namespace {
        using namespace Agent;

        /** @brief Observes the exact request crossing the synchronous local provider boundary. */
        struct RecordingModel final : IModelProvider {
            std::vector<ModelMessage> sent;

            ProviderDiscovery Discover(std::stop_token) override {
                return {};
            }

            ModelOutcome Stream(const ModelRequest &request, const ModelEventSink &sink, std::stop_token) override {
                sent = request.messages;
                if (!sink({.kind = ModelEventKind::TextDelta, .text = "Local response"}) || !sink({.kind = ModelEventKind::Completed}))
                    return {ModelError{ModelErrorCode::Cancelled, "Stopped by host", {}}};
                return {};
            }
        };
    }  // namespace

    TEST_CASE("Local panel source consent dispatches only approved current editor bytes and leaves context out of history",
              "[unit][agent][context][editor_source]") {
        TestSupport::SourceProject source;
        source.Put("disk value\n");
        SourceDocumentService owner{source.directory};
        const auto identity = source.Identity();
        const auto opened = owner.Open(identity, source.directory / "code.cpp");
        REQUIRE(opened.HasValue());
        const auto edited = owner.Edit(identity.instance, {opened.Value().Revision(), 0, 11, "int visible;\nApi-Key: private-value\n"});
        REQUIRE(edited.HasValue());
        CancellationSource lifetime;
        auto prepared = EditorLocalAgentContext::Prepare(edited.Value(), "project", 2, lifetime.Token(), 77);
        REQUIRE(prepared.HasValue());
        auto context = std::move(prepared).Value();
        REQUIRE(context.Items().size() == 1);
        const auto preview = context.Items().front().candidate.text;
        CHECK(preview.find("int visible;") != std::string::npos);
        CHECK(preview.find("private-value") == std::string::npos);
        CHECK(preview.find("disk value") == std::string::npos);
        CHECK(context.Matches(edited.Value()));
        CHECK_FALSE(context.Matches(opened.Value()));
        CHECK(owner.Snapshot(identity.instance).Value().SharesStateWith(edited.Value()));
        bool shouldSend{};

        SECTION("Dismissed previews never grant dispatch") {}
        SECTION("An edit after approval revokes the worker's authority before the provider runs") {
            REQUIRE(context.Approve());
            REQUIRE(owner.Edit(identity.instance, {edited.Value().Revision(), 0, 0, "changed "}).HasValue());
        }
        SECTION("Project detachment after approval revokes the worker's authority") {
            REQUIRE(context.Approve());
            lifetime.RequestCancellation();
        }
        SECTION("An approved current preview reaches the worker exactly once") {
            REQUIRE(context.Approve());
            shouldSend = true;
        }
        RecordingModel provider;
        auto worker = std::async(std::launch::async, [context = std::move(context), &provider]() mutable {
            const auto dispatch = context.Dispatch();
            AgentTurnRuntime runtime;
            const ModelRequest request{.model = "local", .messages = {{.role = ModelRole::User, .text = "Explain the active source"}}};
            return runtime.Run(provider, nullptr, 77, request, {}, {}, &dispatch);
        });
        const auto result = worker.get();
        if (!shouldSend) {
            CHECK(result.status == AgentTurnStatus::ContextDenied);
            CHECK(provider.sent.empty());
            return;
        }
        REQUIRE(result.status == AgentTurnStatus::Completed);
        REQUIRE(provider.sent.size() == 2);
        CHECK(provider.sent.front().text == preview);
        REQUIRE(result.messages.size() == 2);
        CHECK(result.messages.front().text == "Explain the active source");
        CHECK(result.messages.back().text == "Local response");
    }

    TEST_CASE("Local panel context refuses oversized and absent source snapshots", "[unit][agent][context][editor_source]") {
        TestSupport::SourceProject source;
        source.Put(std::string(Security::AgentPolicyLimits{}.maximumItemBytes + 1, 'x'));
        SourceDocumentService owner{source.directory};
        auto opened = owner.Open(source.Identity(), source.directory / "code.cpp");
        REQUIRE(opened.HasValue());
        const auto oversized = EditorLocalAgentContext::Prepare(opened.Value(), "project", 3, {}, 12);
        REQUIRE(oversized.HasError());
        CHECK(oversized.ErrorValue().code.Value() == Application::CodeQueryErrors::Capacity.code.Value());
        const auto absent = EditorLocalAgentContext::Prepare({}, "project", 3, {}, 12);
        REQUIRE(absent.HasError());
        CHECK(absent.ErrorValue().code.Value() == Application::CodeQueryErrors::Stale.code.Value());
    }

    TEST_CASE("Local panel sends only the explicitly chosen bytes from an active source range", "[unit][agent][context][editor_source]") {
        TestSupport::SourceProject source;
        const std::string document = std::string(5000, ' ') + "\nNot selected.\nselected \xC3\xA9\r\nNot selected either.";
        const std::string_view chosen = "selected \xC3\xA9\r\n";
        const auto start = document.find(chosen);
        source.Put(document);
        SourceDocumentService owner{source.directory};
        const auto snapshot = owner.Open(source.Identity(), source.directory / "code.cpp");
        REQUIRE(snapshot.HasValue());
        const auto bytes = snapshot.Value().TextLease();
        REQUIRE(bytes);
        CHECK(bytes->data() == snapshot.Value().Text().data());
        const SourceContextRange range{start, chosen.size()};
        CancellationSource lifetime;
        auto prepared = EditorLocalAgentContext::Prepare(snapshot.Value(), "project", 3, lifetime.Token(), 94, range);
        REQUIRE(prepared.HasValue());
        auto context = std::move(prepared).Value();
        REQUIRE(context.Selection() == range);
        const auto &item = context.Items().front();
        CHECK(item.provenance.kind == AgentContextKind::SourceRange);
        CHECK(item.provenance.offset == range.offset);
        CHECK(item.provenance.capturedBytes == range.length);
        CHECK(item.candidate.text.ends_with(chosen));
        CHECK(item.candidate.text.find("Not selected") == std::string::npos);
        REQUIRE(context.Approve());
        RecordingModel provider;
        const auto dispatch = context.Dispatch();
        AgentTurnRuntime runtime;
        const auto result =
            runtime.Run(provider, nullptr, 94, {.model = "local", .messages = {{.role = ModelRole::User, .text = "Explain this range"}}},
                        {}, {}, &dispatch);
        REQUIRE(result.status == AgentTurnStatus::Completed);
        REQUIRE(provider.sent.size() == 2);
        CHECK(provider.sent.front().text == item.candidate.text);
        CHECK(result.messages.front().text == "Explain this range");
    }

    TEST_CASE("A source range cannot bypass redaction by clipping a sensitive line or private key header",
              "[unit][agent][context][editor_source]") {
        TestSupport::SourceProject source;
        const std::string document =
            "Password: example-password\n-----BEGIN PRIVATE KEY-----\nprivatebodyfixture\n-----END PRIVATE KEY-----\n";
        source.Put(document);
        SourceDocumentService owner{source.directory};
        const auto snapshot = owner.Open(source.Identity(), source.directory / "code.cpp");
        REQUIRE(snapshot.HasValue());
        for (const std::string_view fragment : {"example-password", "privatebodyfixture"}) {
            const SourceContextRange range{document.find(fragment), fragment.size()};
            const auto preview = EditorLocalAgentContext::Prepare(snapshot.Value(), "project", 3, {}, 95, range);
            REQUIRE(preview.HasValue());
            CHECK(preview.Value().Items().front().candidate.text.ends_with("[redacted]"));
            CHECK(preview.Value().Items().front().candidate.text.find(fragment) == std::string::npos);
        }
        const auto beyond =
            EditorLocalAgentContext::Prepare(snapshot.Value(), "project", 3, {}, 96, SourceContextRange{document.size(), 1});
        REQUIRE(beyond.HasError());
        CHECK(beyond.ErrorValue().code.Value() == Application::CodeQueryErrors::Invalid.code.Value());
    }
}  // namespace Horo::Editor
