#include "../../../apps/HoroEditor/app/EditorLocalAgentContext.h"
#include "../../support/SourceDocumentTestProject.h"
#include "Horo/Platform/NativeProjectReadFiles.h"
#include "McpAuthorizationTestSupport.h"

#include <array>
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

    TEST_CASE("The exact redacted clipboard copy becomes a one-turn note or report", "[unit][agent][context][clipboard]") {
        auto type = AgentContextAttachmentKind::Note;
        auto expected = AgentContextKind::Note;
        SECTION("Note remains a note") {}
        SECTION("Report remains a report") {
            type = AgentContextAttachmentKind::Report;
            expected = AgentContextKind::Report;
        }
        CancellationSource lifetime;
        std::string text{"Selected context\nApi-Key: clipboard-value\n"};
        auto prepared = EditorLocalAgentContext::PrepareClipboardAttachment(type, text, "project", 3, lifetime.Token(), 76);
        REQUIRE(prepared.HasValue());
        auto context = std::move(prepared).Value();
        auto different = EditorLocalAgentContext::PrepareClipboardAttachment(type, "Different copy", "project", 3, lifetime.Token(), 76);
        REQUIRE(different.HasValue());
        const auto &item = context.Items().front();
        CHECK(item.provenance.kind == expected);
        CHECK(item.provenance.origin == "clipboard");
        CHECK(item.provenance.capturedBytes == text.size());
        CHECK(item.provenance.revision != different.Value().Items().front().provenance.revision);
        CHECK_FALSE(context.RequiresOpenSource());
        CHECK_FALSE(context.HistoryReference());
        CHECK(item.candidate.text.find("Selected context") != std::string::npos);
        CHECK(item.candidate.text.find("clipboard-value") == std::string::npos);

        text = "Clipboard changed";
        REQUIRE(context.Approve());
        const auto dispatch = context.Dispatch();
        const ModelRequest prompt{.model = "local", .messages = {{.role = ModelRole::User, .text = "Explain this"}}};
        RecordingModel provider;
        AgentTurnRuntime runtime;
        REQUIRE(runtime.Run(provider, nullptr, 76, prompt, {}, {}, &dispatch).status == AgentTurnStatus::Completed);
        REQUIRE(provider.sent.size() == 2);
        CHECK(provider.sent.front().text == item.candidate.text);
        provider.sent.clear();
        CHECK(runtime.Run(provider, nullptr, 76, prompt, {}, {}, &dispatch).status == AgentTurnStatus::ContextDenied);
        CHECK(provider.sent.empty());
    }

    TEST_CASE("Clipboard consent rejects missing, binary, oversized and unsupported attachment input",
              "[unit][agent][context][clipboard]") {
        const auto prepare = [](std::string_view text, AgentContextAttachmentKind kind = AgentContextAttachmentKind::Note) {
            return EditorLocalAgentContext::PrepareClipboardAttachment(kind, text, "project", 3, {}, 76);
        };
        CHECK(prepare("").HasError());
        CHECK(prepare(std::string_view{"a\0b", 3}).HasError());
        CHECK(prepare(std::string(Security::AgentPolicyLimits{}.maximumItemBytes + 1, 'a')).HasError());
        CHECK(prepare("One item", AgentContextAttachmentKind::BuildOutput).HasError());
        CancellationSource workspace;
        auto prepared = EditorLocalAgentContext::PrepareClipboardAttachment(AgentContextAttachmentKind::Report, "Report", "project", 3,
                                                                            workspace.Token(), 76);
        REQUIRE(prepared.HasValue());
        workspace.RequestCancellation();
        CHECK_FALSE(std::move(prepared).Value().Approve());
    }

    TEST_CASE("Saved project files need exact review and a fresh rooted read before local inference",
              "[unit][agent][context][saved_project_file]") {
        TestSupport::SourceProject project;
        project.Put("int saved_value;\nApi-Key: private-value\n", "unopened file.cpp");
        auto files = Platform::CreateNativeProjectReadFiles(std::filesystem::canonical(project.directory), "workspace", 1);
        REQUIRE(files.HasValue());
        CancellationSource lifetime;
        const Platform::ProjectReadContext read{"workspace", 1, lifetime.Token(),
                                                std::chrono::steady_clock::now() + std::chrono::minutes{1}};
        auto prepared = EditorLocalAgentContext::PrepareSavedFile(files.Value(), "unopened file.cpp", read, 76);
        REQUIRE(prepared.HasValue());
        auto context = std::move(prepared).Value();
        REQUIRE(context.IsSavedFile());
        REQUIRE_FALSE(context.HistoryReference());
        const auto &item = context.Items().front();
        CHECK(item.provenance.origin == "unopened file.cpp");
        CHECK(item.candidate.text.find("int saved_value;") != std::string::npos);
        CHECK(item.candidate.text.find("private-value") == std::string::npos);

        bool shouldSend{};
        std::stop_source worker;
        SECTION("Matching saved bytes send only after consent") {
            REQUIRE(context.Approve());
            shouldSend = true;
        }
        SECTION("Reading a file does not itself grant consent") {}
        SECTION("Changes after consent deny dispatch") {
            REQUIRE(context.Approve());
            project.Put("int other_value;\n", "unopened file.cpp");
        }
        SECTION("Deletion after consent denies dispatch") {
            REQUIRE(context.Approve());
            REQUIRE(std::filesystem::remove(project.directory / "unopened file.cpp"));
        }
        SECTION("Project detachment after consent denies dispatch") {
            REQUIRE(context.Approve());
            lifetime.RequestCancellation();
        }
        SECTION("Cancelled worker denies dispatch") {
            REQUIRE(context.Approve());
            worker.request_stop();
        }
        RecordingModel provider;
        const auto dispatch = context.Dispatch(worker.get_token());
        const ModelRequest request{.model = "local", .messages = {{.role = ModelRole::User, .text = "Explain the saved file"}}};
        const auto result = AgentTurnRuntime{}.Run(provider, nullptr, 76, request, {}, {}, &dispatch);
        if (!shouldSend) {
            CHECK(result.status == AgentTurnStatus::ContextDenied);
            CHECK(provider.sent.empty());
            return;
        }
        REQUIRE(result.status == AgentTurnStatus::Completed);
        REQUIRE(provider.sent.size() == 2);
        CHECK(provider.sent.front().text == item.candidate.text);
    }

    TEST_CASE("Saved project file review rejects unsafe, missing, binary or oversized sources",
              "[unit][agent][context][saved_project_file]") {
        TestSupport::SourceProject project;
        auto files = Platform::CreateNativeProjectReadFiles(std::filesystem::canonical(project.directory), "workspace", 1);
        REQUIRE(files.HasValue());
        const Platform::ProjectReadContext read{"workspace", 1, {}, std::chrono::steady_clock::now() + std::chrono::minutes{1}};
        const auto prepare = [&](std::string_view path) {
            return EditorLocalAgentContext::PrepareSavedFile(files.Value(), path, read, 76);
        };
        CHECK(prepare("../outside.cpp").HasError());
        CHECK(prepare("code.cpp:alternate").HasError());
        CHECK(prepare("missing.cpp").HasError());
        project.Put(std::string_view{"a\0b", 3}, "binary.cpp");
        CHECK(prepare("binary.cpp").HasError());
        project.Put(std::string(Security::AgentPolicyLimits{}.maximumItemBytes + 1, 'a'), "large.cpp");
        CHECK(prepare("large.cpp").HasError());
        project.Put("int visible;");
        std::filesystem::copy_file(project.directory / "code.cpp", project.directory / std::filesystem::path{u8"örnek.cpp"});
        CHECK(prepare("örnek.cpp").HasValue());
        std::filesystem::create_hard_link(project.directory / "code.cpp", project.directory / "linked.cpp");
        CHECK(prepare("linked.cpp").HasError());
    }

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
        const auto reference = context.HistoryReference();
        REQUIRE(reference);
        CHECK(reference->kind == AgentHistoryReferenceKind::Document);
        CHECK(reference->identity == identity.instance.Value());
        CHECK(reference->revision == edited.Value().Revision());
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
        std::string observed;
        auto worker = std::async(std::launch::async, [context = std::move(context), &provider, &observed]() mutable {
            const auto dispatch = context.Dispatch();
            AgentTurnRuntime runtime;
            const ModelRequest request{.model = "local", .messages = {{.role = ModelRole::User, .text = "Explain the active source"}}};
            return runtime.Run(provider, nullptr, 77, request, {}, {}, &dispatch, [&observed](std::string_view delta) {
                observed.append(delta);
            });
        });
        const auto result = worker.get();
        if (!shouldSend) {
            CHECK(result.status == AgentTurnStatus::ContextDenied);
            CHECK(provider.sent.empty());
            CHECK(observed.empty());
            return;
        }
        REQUIRE(result.status == AgentTurnStatus::Completed);
        REQUIRE(provider.sent.size() == 2);
        CHECK(provider.sent.front().text == preview);
        REQUIRE(result.messages.size() == 2);
        CHECK(result.messages.front().text == "Explain the active source");
        CHECK(result.messages.back().text == "Local response");
        CHECK(observed == result.messages.back().text);
    }

    TEST_CASE("An explicitly selected open source keeps its own revision and loses approval when closed",
              "[unit][agent][context][editor_source]") {
        using namespace Agent;
        TestSupport::SourceProject project;
        project.Put("int foreground;");
        project.Put("int requested;", "other.cpp");
        SourceDocumentService owner{project.directory};
        const auto first = owner.Open(project.Identity(), project.directory / "code.cpp");
        const auto identity = project.Identity("other.cpp", 2);
        const auto chosen = owner.Open(identity, project.directory / "other.cpp");
        REQUIRE(first.HasValue());
        REQUIRE(chosen.HasValue());
        CancellationSource lifetime;
        auto preview = EditorLocalAgentContext::Prepare(chosen.Value(), "project", 2, lifetime.Token(), 7);
        REQUIRE(preview.HasValue());
        auto context = std::move(preview).Value();

        const std::array tabs{first.Value(), chosen.Value()};
        REQUIRE(context.MatchesOpenSource(tabs));
        CHECK_FALSE(context.MatchesOpenSource(std::span{tabs}.first(1)));
        REQUIRE(context.HistoryReference());
        CHECK(context.HistoryReference()->identity == identity.instance.Value());
        const auto &text = context.Items().front().candidate.text;
        CHECK(text.find("int requested;") != std::string::npos);
        CHECK(text.find("int foreground;") == std::string::npos);
        REQUIRE(context.Approve());
        REQUIRE(owner.Close(identity.instance).HasValue());
        CHECK_FALSE(context.MatchesOpenSource(tabs));
        RecordingModel provider;
        AgentTurnRuntime runtime;
        const auto dispatch = context.Dispatch();
        const ModelRequest request{.model = "local", .messages = {{.role = ModelRole::User, .text = "Explain the selected file"}}};
        CHECK(runtime.Run(provider, nullptr, 7, request, {}, {}, &dispatch).status == AgentTurnStatus::ContextDenied);
        CHECK(provider.sent.empty());
        CHECK(owner.Snapshot(project.Identity().instance).Value().SharesStateWith(first.Value()));
    }

    TEST_CASE("An approved Scene selection sends only the bounded IDs while its owner remains current",
              "[unit][agent][context][scene_selection]") {
        using namespace Agent;
        CancellationSource workspace;
        CancellationSource scene;
        const AgentSceneSelectionSnapshot selection{.documentEpoch = 2,
                                                    .documentRevision = 0,
                                                    .selectionRevision = 3,
                                                    .objects = {42, 815},
                                                    .objectCount = 2,
                                                    .truncated = true,
                                                    .lifetime = scene.Token()};
        auto prepared = EditorLocalAgentContext::PrepareSceneSelection(selection, "project", 1, workspace.Token(), 9);
        REQUIRE(prepared.HasValue());
        auto context = std::move(prepared).Value();
        CHECK(context.IsSceneSelection());
        CHECK_FALSE(context.HistoryReference());
        const auto &item = context.Items().front();
        CHECK(item.provenance.truncated);
        const auto approvedText = item.candidate.text;
        CHECK(approvedText.find("Selected object ID 42\nSelected object ID 815") != std::string::npos);
        REQUIRE(context.Approve());
        bool revoked{};
        SECTION("An exact lease supplies the approved bytes once") {}
        SECTION("A changed scene or selection revokes the pending dispatch") {
            scene.RequestCancellation();
            revoked = true;
        }
        RecordingModel provider;
        AgentTurnRuntime runtime;
        const auto dispatch = context.Dispatch();
        const ModelRequest request{.model = "local", .messages = {{.role = ModelRole::User, .text = "Review selected objects"}}};
        const auto result = runtime.Run(provider, nullptr, 9, request, {}, {}, &dispatch);
        if (revoked) {
            CHECK(result.status == AgentTurnStatus::ContextDenied);
            CHECK(provider.sent.empty());
        } else {
            REQUIRE(result.status == AgentTurnStatus::Completed);
            REQUIRE(provider.sent.size() == 2);
            CHECK(provider.sent.front().text == approvedText);
        }
    }

    TEST_CASE("Explicit viewport consent binds numeric camera values to the captured scene", "[unit][agent][context][viewport]") {
        using namespace Agent;
        CancellationSource workspace;
        CancellationSource viewport;
        const AgentSceneSelectionSnapshot snapshot{.documentEpoch = 2,
                                                   .documentRevision = 6,
                                                   .selectionRevision = 7,
                                                   .lifetime = viewport.Token(),
                                                   .viewport = AgentViewportOverview{.revision = 8,
                                                                                     .position = {1, 2, 3},
                                                                                     .target = {0, 0, 0},
                                                                                     .up = {0, 1, 0},
                                                                                     .verticalExtent = 8,
                                                                                     .nearPlane = 1,
                                                                                     .farPlane = 100,
                                                                                     .orthographic = true,
                                                                                     .tool = AgentViewportTool::Rotate}};
        auto prepared = EditorLocalAgentContext::PrepareSceneSelection(snapshot, "project", 1, workspace.Token(), 9);
        REQUIRE(prepared.HasValue());
        auto context = std::move(prepared).Value();
        const auto &item = context.Items().front();
        const auto expected = item.candidate.text;
        CHECK(item.provenance.revision == "scene:2:6:7:viewport:8");
        CHECK(expected.find("camera.position = [1, 2, 3]") != std::string::npos);
        CHECK(expected.find("camera.height = 8") != std::string::npos);
        CHECK(expected.find("viewport.tool = rotate") != std::string::npos);
        CHECK(expected.find("Selected object ID") == std::string::npos);
        REQUIRE(context.Approve());
        bool retired = false;
        SECTION("The exact preview is used") {}
        SECTION("The owner can revoke the viewport before send") {
            viewport.RequestCancellation();
            retired = true;
        }
        RecordingModel provider;
        AgentTurnRuntime runtime;
        const auto dispatch = context.Dispatch();
        const ModelRequest request{.model = "local", .messages = {{.role = ModelRole::User, .text = "Explain the view"}}};
        const auto result = runtime.Run(provider, nullptr, 9, request, {}, {}, &dispatch);
        if (retired) {
            CHECK(result.status == AgentTurnStatus::ContextDenied);
            CHECK(provider.sent.empty());
        } else {
            REQUIRE(result.status == AgentTurnStatus::Completed);
            REQUIRE(provider.sent.size() == 2);
            CHECK(provider.sent.front().text == expected);
        }
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

    TEST_CASE("Build output witnesses follow only their operation and its retained records", "[unit][agent][context][build_output]") {
        constexpr OperationId selected = 17;
        constexpr OperationId other = 23;
        BuildOutputOperationWitness afterDestruction;
        {
            BuildOutputStore output{3};
            output.Append({.operationId = selected, .message = "First selected result"});
            output.Append({.operationId = other, .message = "Other operation"});
            CHECK_FALSE(output.CaptureOperationTail(99, 2, 128));
            const auto initial = output.CaptureOperationTail(selected, 2, 128);
            REQUIRE(initial);
            REQUIRE(initial->messages == std::vector<std::string>{"First selected result"});
            CHECK(initial->witness.IsCurrent());
            output.Append({.operationId = other, .message = "New unrelated output"});
            CHECK(initial->witness.IsCurrent());
            output.Append({.operationId = selected, .message = "New selected result"});
            CHECK_FALSE(initial->witness.IsCurrent());
            const auto refreshed = output.CaptureOperationTail(selected, 2, 128);
            REQUIRE(refreshed);
            afterDestruction = refreshed->witness;
            REQUIRE(afterDestruction.IsCurrent());
            output.Append({.operationId = other, .message = "Evict other output"});
            CHECK(afterDestruction.IsCurrent());
        }
        CHECK_FALSE(afterDestruction.IsCurrent());

        BuildOutputStore evicting{2};
        evicting.Append({.operationId = selected, .message = "Selected for eviction"});
        evicting.Append({.operationId = other, .message = "Unrelated"});
        const auto retained = evicting.CaptureOperationTail(selected, 1, 128);
        REQUIRE(retained);
        evicting.Append({.operationId = other, .message = "Evicts selected output"});
        CHECK_FALSE(retained->witness.IsCurrent());
    }

    TEST_CASE("The MCP diagnostics preview sends only the approved warning and error tail for the selected build",
              "[unit][agent][context][build_output][mcp]") {
        using namespace Agent;
        constexpr OperationId selected = 31;
        BuildOutputStore output{12};
        output.Append({.operationId = selected, .severity = DiagnosticSeverity::Warning, .message = "Earlier diagnostic"});
        output.Append({.operationId = selected, .severity = DiagnosticSeverity::Error, .message = "Compiler error\nApi-Key: fake-secret"});
        output.Append({.operationId = 32, .severity = DiagnosticSeverity::Fatal, .message = "Unrelated operation"});
        output.Append({.operationId = selected, .severity = DiagnosticSeverity::Fatal, .message = "A linker failure"});
        output.Append({.operationId = selected, .message = "Informational output"});
        auto tail = output.CaptureOperationDiagnostics(selected, 2, 1024);
        REQUIRE(tail);
        CHECK(tail->truncated);
        CHECK(tail->messages.size() == 2);
        CHECK_FALSE(output.CaptureOperationDiagnostics(99, 2, 1024));

        CancellationSource workspace;
        const Platform::ProjectReadContext project{"project", 3, workspace.Token(),
                                                   std::chrono::steady_clock::now() + std::chrono::minutes{1}};
        auto preview =
            EditorLocalAgentContext::PrepareBuildDiagnostics(std::move(*tail), project, 51, std::make_shared<Mcp::Test::Entropy>());
        REQUIRE(preview.HasValue());
        auto context = std::move(preview).Value();
        const auto &item = context.Items().front();
        CHECK(item.provenance.kind == AgentContextKind::Diagnostic);
        CHECK(item.provenance.truncated);
        const auto text = item.candidate.text;
        CHECK(text.find("Compiler error") != std::string::npos);
        CHECK(text.find("[redacted]") != std::string::npos);
        REQUIRE(text.find("A linker failure") != std::string::npos);
        CHECK(text.find("Compiler error") < text.find("A linker failure"));
        for (const auto *excluded : {"Earlier diagnostic", "fake-secret", "Unrelated operation", "Informational output"})
            CHECK(text.find(excluded) == std::string::npos);

        REQUIRE(context.Approve());
        bool shouldSend{true};
        SECTION("A newer diagnostic for the selected build revokes consent") {
            output.Append({.operationId = selected, .severity = DiagnosticSeverity::Error, .message = "New compiler error"});
            shouldSend = false;
        }
        SECTION("A new diagnostic of another operation leaves the selected capture current") {
            output.Append({.operationId = 32, .severity = DiagnosticSeverity::Error, .message = "Still outside scope"});
        }
        SECTION("Closing the workspace revokes the owned diagnostic capture") {
            workspace.RequestCancellation();
            shouldSend = false;
        }
        RecordingModel model;
        AgentTurnRuntime runtime;
        const auto dispatch = context.Dispatch();
        const ModelRequest request{.model = "local", .messages = {{.role = ModelRole::User, .text = "Explain these diagnostics"}}};
        const auto result = runtime.Run(model, nullptr, 51, request, {}, {}, &dispatch);
        if (!shouldSend) {
            CHECK(result.status == AgentTurnStatus::ContextDenied);
            CHECK(model.sent.empty());
            return;
        }
        REQUIRE(result.status == AgentTurnStatus::Completed);
        REQUIRE(model.sent.size() == 2);
        CHECK(model.sent.front().text == text);
    }

    TEST_CASE("Active editor diagnostics remain scoped to the reviewed report and its owner witness",
              "[unit][agent][context][mcp][editor_diagnostics]") {
        CancellationSource report;
        CancellationSource workspace;
        const Platform::ProjectReadContext project{"project", 3, workspace.Token(),
                                                   std::chrono::steady_clock::now() + std::chrono::minutes{1}};
        const AgentEditorDiagnosticsSnapshot snapshot{.document = 70,
                                                      .observation = 2,
                                                      .messages = {"decision.invalid: Invalid edge", "Api-Key: preview-secret"},
                                                      .truncated = true,
                                                      .lifetime = report.Token()};
        const auto prepare = [&] {
            return EditorLocalAgentContext::PrepareEditorDiagnostics(snapshot, project, 51, std::make_shared<Mcp::Test::Entropy>(), {});
        };
        auto prepared = prepare();
        REQUIRE(prepared.HasValue());
        auto context = std::move(prepared).Value();
        const auto &item = context.Items().front();
        CHECK(item.provenance.kind == AgentContextKind::Diagnostic);
        CHECK(item.provenance.truncated);
        CHECK(item.candidate.text.find("Invalid edge") != std::string::npos);
        CHECK(item.candidate.text.find("preview-secret") == std::string::npos);
        CHECK_FALSE(context.HistoryReference());
        REQUIRE(context.Approve());
        bool current{true};
        SECTION("The chosen current report sends exactly the MCP preview") {}
        SECTION("A change in the selected editor document denies new captures and dispatch") {
            report.RequestCancellation();
            REQUIRE(prepare().HasError());
            current = false;
        }
        RecordingModel model;
        const auto dispatch = context.Dispatch();
        const ModelRequest request{.model = "local", .messages = {{.role = ModelRole::User, .text = "Explain these problems"}}};
        const auto result = AgentTurnRuntime{}.Run(model, nullptr, 51, request, {}, {}, &dispatch);
        if (!current) {
            CHECK(result.status == AgentTurnStatus::ContextDenied);
            CHECK(model.sent.empty());
            return;
        }
        REQUIRE(result.status == AgentTurnStatus::Completed);
        REQUIRE(model.sent.size() == 2);
        CHECK(model.sent.front().text == item.candidate.text);
    }

    TEST_CASE("Local panel build context sends the explicitly approved bounded project operation output without history",
              "[unit][agent][context][build_output]") {
        constexpr OperationId selected = 31;
        constexpr OperationId other = 32;
        BuildOutputStore output{12};
        output.Append({.operationId = selected, .message = "Omitted earlier selected detail"});
        output.Append({.operationId = other, .message = "Other project's build output"});
        output.Append({.operationId = selected, .message = "Visible compiler finding"});
        output.Append({.operationId = selected, .message = "Authorization: Bearer synthetic-build-value"});
        auto tail = output.CaptureOperationTail(selected, 2, 256);
        REQUIRE(tail);
        CHECK(tail->truncated);
        const auto selectedRevision = tail->lastSequence;
        CancellationSource workspace;
        auto captured = EditorLocalAgentContext::PrepareBuildOutput(std::move(*tail), "project", 3, workspace.Token(), 51);
        REQUIRE(captured.HasValue());
        auto context = std::move(captured).Value();
        CHECK(context.BuildOperation() == selected);
        const auto reference = context.HistoryReference();
        REQUIRE(reference);
        CHECK(reference->kind == AgentHistoryReferenceKind::BuildOutput);
        CHECK(reference->identity == selected);
        CHECK(reference->revision == selectedRevision);
        REQUIRE(context.Items().size() == 1);
        const auto &item = context.Items().front();
        REQUIRE(item.provenance.kind == AgentContextKind::BuildOutput);
        CHECK(item.provenance.truncated);
        const auto consented = item.candidate.text;
        CHECK(consented.find("Visible compiler finding") != std::string::npos);
        CHECK(consented.find("[redacted]") != std::string::npos);
        CHECK(consented.find("synthetic-build-value") == std::string::npos);
        CHECK(consented.find("Other project's") == std::string::npos);
        CHECK(consented.find("Omitted earlier selected detail") == std::string::npos);
        bool shouldSend{};

        SECTION("A preview without explicit consent cannot dispatch") {}
        SECTION("New output for the exact operation revokes dispatch even after consent") {
            REQUIRE(context.Approve());
            output.Append({.operationId = selected, .message = "New selected output"});
        }
        SECTION("Workspace detachment revokes approved output") {
            REQUIRE(context.Approve());
            workspace.RequestCancellation();
        }
        SECTION("Other operation output cannot change the exact approved evidence") {
            REQUIRE(context.Approve());
            output.Append({.operationId = other, .message = "Unrelated output after capture"});
            CHECK(context.IsCurrent());
            shouldSend = true;
        }
        RecordingModel provider;
        auto worker = std::async(std::launch::async, [context = std::move(context), &provider]() mutable {
            const auto dispatch = context.Dispatch();
            AgentTurnRuntime runtime;
            return runtime.Run(provider, nullptr, 51, {.model = "local", .messages = {{.role = ModelRole::User, .text = "Explain build"}}},
                               {}, {}, &dispatch);
        });
        const auto result = worker.get();
        if (!shouldSend) {
            CHECK(result.status == AgentTurnStatus::ContextDenied);
            CHECK(provider.sent.empty());
            return;
        }
        REQUIRE(result.status == AgentTurnStatus::Completed);
        REQUIRE(provider.sent.size() == 2);
        CHECK(provider.sent.front().text == consented);
        REQUIRE(result.messages.size() == 2);
        CHECK(result.messages.front().text == "Explain build");
    }
}  // namespace Horo::Editor
