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
}  // namespace Horo::Editor
