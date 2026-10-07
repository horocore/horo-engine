#include "Horo/Mcp/McpController.h"
#include "Horo/Mcp/McpErrors.h"
#include "McpAuthorizationTestSupport.h"

#include <thread>
#include <type_traits>

static_assert(!std::is_default_constructible_v<Horo::Mcp::McpAuthority>);
static_assert(!std::is_copy_constructible_v<Horo::Mcp::McpAuthority>);
static_assert(!std::is_copy_constructible_v<Horo::Mcp::McpAuthorization>);

using namespace Horo;
using namespace Horo::Mcp;

namespace {
    template <typename Authority>
    concept CanForgeAuthority = requires { Authority({}, nullptr); };
    static_assert(!CanForgeAuthority<McpAuthority>);

    void RequireAuthorizationDenied(const Result<nlohmann::json> &outcome) {
        REQUIRE(outcome.HasError());
        CHECK(outcome.ErrorValue().code.Value() == McpErrors::AuthorizationDenied.code.Value());
        CHECK(SafeErrorData(outcome.ErrorValue()).dump().find("private-proof") == std::string::npos);
    }

    /** @brief Observes numeric-only progress and redacted terminal denial across revocation and expiry. */
    struct AsyncObservation final {
        unsigned progressUpdates{};
        unsigned completions{};

        McpRequestContext Context(McpRequestContext context) {
            context.reportProgress = [this](const double fraction, const std::string &phase) {
                ++progressUpdates;
                CHECK(fraction == 0.5);
                CHECK(phase.empty());
            };
            return context;
        }

        std::function<void(Result<nlohmann::json>)> Completion() {
            return [this](const Result<nlohmann::json> &outcome) {
                ++completions;
                RequireAuthorizationDenied(outcome);
            };
        }
    };

    class Application final : public IMcpToolAdapter {
    public:
        unsigned calls{};
        McpRequestContext saved;
        bool deferred{};
        std::function<void(Result<nlohmann::json>)> completion;

        Result<nlohmann::json> Invoke(const nlohmann::json &, const McpRequestContext &context) override {
            ++calls;
            saved = context;
            context.ReportProgress(0.5, "credential-and-private-argument");
            return Result<nlohmann::json>::Success(nlohmann::json::object());
        }

        void InvokeAsync(const nlohmann::json &arguments, const McpRequestContext &context,
                         const std::function<void(Result<nlohmann::json>)> &complete) override {
            if (!deferred) {
                IMcpToolAdapter::InvokeAsync(arguments, context, complete);
                return;
            }
            ++calls;
            saved = context;
            completion = complete;
        }
    };

    struct Fixture final {
        std::shared_ptr<McpAuthorization> policy = std::make_shared<McpAuthorization>(std::make_shared<Test::Entropy>());
        McpSessionAdmission admission{.clientIdentity = "local-client", .capabilities = {"project.read"}, .projectIdentity = "project"};
        std::shared_ptr<Application> app = std::make_shared<Application>();
        std::shared_ptr<McpToolRegistry> registry = std::make_shared<McpToolRegistry>(policy);
        std::shared_ptr<McpController> controller;
        std::shared_ptr<McpSessionManager> manager;

        explicit Fixture(const McpToolEffect effect = McpToolEffect::Mutation) {
            REQUIRE(policy->SetTrust("project", 1, true).HasValue());
            const auto schema = nlohmann::json{{"type", "object"}, {"additionalProperties", true}};
            REQUIRE(registry
                        ->Publish({{.descriptor = {.id = {"project.tool"},
                                                   .description = "Test application capability",
                                                   .inputSchema = schema,
                                                   .outputSchema = schema,
                                                   .effect = effect,
                                                   .requiredCapabilities = {"project.read"}},
                                    .adapter = app,
                                    .owner = McpOwnerContext::Editor}},
                                  admission.capabilities)
                        .HasValue());
            auto created = McpController::Create(registry, {}, policy);
            REQUIRE(created.HasValue());
            controller = std::move(created).Value();
            auto sessions = McpSessionManager::Create(controller, {}, policy);
            REQUIRE(sessions.HasValue());
            manager = std::move(sessions).Value();
            admission = Test::Authenticate(admission, policy);
        }

        McpRequestContext Context() const {
            return {.session = {1, 1},
                    .clientIdentity = admission.clientIdentity,
                    .capabilities = admission.capabilities,
                    .projectIdentity = admission.projectIdentity,
                    .authorizationRevision = admission.authorizationRevision,
                    .registryRevision = admission.registryRevision,
                    .deadline = std::chrono::steady_clock::now() + std::chrono::minutes{1},
                    .authority = admission.authority,
                    .authorization = policy,
                    .requestIdentity = 7};
        }

        McpRequest Call(const nlohmann::json &arguments = nlohmann::json::object()) const {
            return {.id = 7, .method = "tools/call", .params = {{"name", "project.tool"}, {"arguments", arguments}}};
        }

        std::uint64_t Approve(const McpRequest &request, const std::chrono::milliseconds lifetime = std::chrono::minutes{1}) const {
            const auto challenge = policy->Challenge(Context(), request, lifetime);
            REQUIRE(challenge.HasValue());
            REQUIRE(policy->Decide(challenge.Value(), true).HasValue());
            return challenge.Value();
        }

        void Pump() const {
            REQUIRE(controller->BindOwner(McpOwnerContext::Editor).HasValue());
            REQUIRE(controller->Pump(McpOwnerContext::Editor).HasValue());
        }

        void StartDeferred(AsyncObservation &observed) const {
            registry->Read()->InvokeAsync({"project.tool"}, nlohmann::json::object(), observed.Context(Context()), observed.Completion());
            REQUIRE(app->calls == 1);
            REQUIRE(observed.completions == 0);
        }
    };
}  // namespace

TEST_CASE("MCP authentication requires secure entropy and explicit trusted authority", "[mcp][security]") {
    McpAuthorization absent{nullptr};
    REQUIRE(absent.SetTrust({}, 1, true).HasValue());
    CHECK(absent.IssueCredential({.clientIdentity = "client"}, std::chrono::minutes{1}).HasError());
    Fixture f;
    auto unsignedAdmission = f.admission;
    unsignedAdmission.authority.reset();
    CHECK(f.manager->Open(unsignedAdmission).HasError());
    auto forged = f.Context();
    forged.authority.reset();
    CHECK(f.controller->Dispatch({.id = 1, .method = "tools/list"}, forged).HasError());
    CHECK(f.app->calls == 0);
}

TEST_CASE("MCP local credentials are one-use and reject principal and capability substitution", "[mcp][security]") {
    Fixture f;
    auto admission = f.admission;
    admission.authority.reset();
    const auto mint = [&] {
        return f.policy->IssueCredential(admission, std::chrono::minutes{1});
    };
    auto token = mint();
    REQUIRE(token.HasValue());
    Security::SecureBytes replay{token.Value().View()};
    REQUIRE(f.policy->Authenticate(admission, std::move(token).Value()).HasValue());
    CHECK(f.policy->Authenticate(admission, std::move(replay)).HasError());
    token = mint();
    REQUIRE(token.HasValue());
    auto substituted = admission;
    substituted.capabilities.push_back("process.execute");
    Security::SecureBytes retry{token.Value().View()};
    CHECK(f.policy->Authenticate(substituted, std::move(token).Value()).HasError());
    CHECK(f.policy->Authenticate(admission, std::move(retry)).HasError());
}

TEST_CASE("MCP credentials reject path-like or embedded-NUL authority identities", "[mcp][security]") {
    Fixture f;
    auto admission = f.admission;
    admission.authority.reset();
    for (const std::string identity :
         {std::string{"../escape"}, std::string{"/outside"}, std::string{"client\0evil", 11}, std::string(257, 'a')}) {
        admission.clientIdentity = identity;
        CHECK(f.policy->IssueCredential(admission, std::chrono::minutes{1}).HasError());
    }
}

TEST_CASE("MCP expired credentials and untrusted project policy never create principals", "[mcp][security]") {
    Fixture f;
    auto admission = f.admission;
    admission.authority.reset();
    auto token = f.policy->IssueCredential(admission, std::chrono::milliseconds{10});
    REQUIRE(token.HasValue());
    std::this_thread::sleep_for(std::chrono::milliseconds{15});
    CHECK(f.policy->Authenticate(admission, std::move(token).Value()).HasError());
    REQUIRE(f.policy->SetTrust("project", 2, false).HasValue());
    admission.authorizationRevision = 2;
    CHECK(f.policy->IssueCredential(admission, std::chrono::minutes{1}).HasError());
    CHECK(f.controller->Dispatch({.id = 1, .method = "tools/list"}, f.Context()).HasError());
}

TEST_CASE("MCP credential scope binds client project and registry revisions", "[mcp][security]") {
    Fixture f;
    auto admission = f.admission;
    admission.authority.reset();
    for (unsigned index = 0; index < 4; ++index) {
        auto token = f.policy->IssueCredential(admission, std::chrono::minutes{1});
        REQUIRE(token.HasValue());
        auto substituted = admission;
        if (index == 0)
            substituted.clientIdentity = "other-client";
        if (index == 1)
            substituted.projectIdentity = "other-project";
        if (index == 2)
            ++substituted.registryRevision;
        if (index == 3)
            ++substituted.authorizationRevision;
        CHECK(f.policy->Authenticate(substituted, std::move(token).Value()).HasError());
    }
}

TEST_CASE("MCP trusted query discovery and execution use authenticated production composition", "[mcp][security]") {
    Fixture f{McpToolEffect::Query};
    const auto session = f.manager->Open(f.admission);
    REQUIRE(session.HasValue());
    const auto list = f.manager->Dispatch(session.Value(), {.id = 1, .method = "tools/list"});
    REQUIRE(list.HasValue());
    CHECK(list.Value()["tools"].size() == 1);
    REQUIRE(f.manager->Dispatch(session.Value(), f.Call()).HasValue());
    f.Pump();
    CHECK(f.app->calls == 1);
}

TEST_CASE("MCP presentation and mutation effects require an exact local decision", "[mcp][security]") {
    for (const auto effect : {McpToolEffect::PresentationSideEffect, McpToolEffect::Mutation}) {
        Fixture f{effect};
        CHECK(f.controller->Dispatch(f.Call(), f.Context()).HasError());
        const auto id = f.Approve(f.Call());
        REQUIRE(f.controller->Dispatch(f.Call(), f.Context()).HasValue());
        f.Pump();
        CHECK(f.app->calls == 1);
        CHECK(f.policy->Decide(id, true).HasError());
        CHECK(f.controller->Dispatch(f.Call(), f.Context()).HasError());
    }
}

TEST_CASE("MCP approvals bind exact path and argument bytes rather than caller risk claims", "[mcp][security]") {
    Fixture f;
    const auto original = f.Call({{"path", "safe/file"}, {"args", {"--safe"}}});
    f.Approve(original);
    CHECK(f.controller->Dispatch(f.Call({{"path", "../escape"}, {"args", {"--safe"}}}), f.Context()).HasError());
    CHECK(f.controller->Dispatch(f.Call({{"path", "safe/file"}, {"args", {"--safe", ";injected"}}}), f.Context()).HasError());
    auto otherId = original;
    otherId.id = 8;
    CHECK(f.controller->Dispatch(otherId, f.Context()).HasError());
    REQUIRE(f.controller->Dispatch(original, f.Context()).HasValue());
    f.Pump();
    CHECK(f.app->calls == 1);
}

TEST_CASE("MCP denial revokes a pending or approved challenge without remote reapproval", "[mcp][security]") {
    Fixture f;
    const auto id = f.Approve(f.Call());
    REQUIRE(f.policy->Decide(id, false).HasValue());
    CHECK(f.policy->Decide(id, true).HasError());
    CHECK(f.controller->Dispatch(f.Call(), f.Context()).HasError());
    CHECK(f.app->calls == 0);
}

TEST_CASE("MCP approval expiry is enforced again at owner invocation", "[mcp][security]") {
    Fixture f;
    f.Approve(f.Call(), std::chrono::milliseconds{10});
    REQUIRE(f.controller->Dispatch(f.Call(), f.Context()).HasValue());
    std::this_thread::sleep_for(std::chrono::milliseconds{15});
    f.Pump();
    CHECK(f.app->calls == 0);
}

TEST_CASE("MCP project revision replacement invalidates queued approval and cancels authority", "[mcp][security]") {
    Fixture f;
    f.Approve(f.Call());
    REQUIRE(f.controller->Dispatch(f.Call(), f.Context()).HasValue());
    REQUIRE(f.policy->SetTrust("project", 2, true).HasValue());
    CHECK(f.admission.authority->IsStopRequested());
    CHECK(f.policy->SetTrust("project", 1, true).HasError());
    f.Pump();
    CHECK(f.app->calls == 0);
    CHECK(f.manager->Open(f.admission).HasError());
}

TEST_CASE("MCP revocation reaches application cancellation and blocks new discovery", "[mcp][security]") {
    Fixture f{McpToolEffect::Query};
    REQUIRE(f.controller->Dispatch(f.Call(), f.Context()).HasValue());
    f.Pump();
    REQUIRE_FALSE(f.app->saved.IsStopRequested());
    f.policy->Revoke(f.admission.authority);
    CHECK(f.app->saved.IsStopRequested());
    CHECK(f.controller->Dispatch({.id = 1, .method = "tools/list"}, f.Context()).HasError());
}

TEST_CASE("MCP rejects confused-deputy principal and foreign policy substitution", "[mcp][security]") {
    Fixture f{McpToolEffect::Query};
    Fixture other{McpToolEffect::Query};
    auto foreign = other.Context();
    CHECK(f.controller->Dispatch(f.Call(), foreign).HasError());
    foreign.authorization = f.policy;
    CHECK(f.controller->Dispatch(f.Call(), foreign).HasError());
    auto forged = f.Context();
    forged.clientIdentity = "another-client";
    CHECK(f.controller->Dispatch(f.Call(), forged).HasError());
    forged = f.Context();
    ++forged.registryRevision;
    CHECK(f.controller->Dispatch(f.Call(), forged).HasError());
    CHECK(f.app->calls == 0);
}

TEST_CASE("MCP progress and typed denial presentation omit sensitive argument text", "[mcp][security]") {
    Fixture f;
    const auto request = f.Call({{"secret", "private-proof"}});
    const auto denied = f.controller->Dispatch(request, f.Context());
    REQUIRE(denied.HasError());
    CHECK(SafeErrorData(denied.ErrorValue()).dump().find("private-proof") == std::string::npos);
    f.Approve(request);
    const auto accepted = f.controller->Dispatch(request, f.Context());
    REQUIRE(accepted.HasValue());
    f.Pump();
    const auto view =
        f.controller->Dispatch({.id = 9, .method = "operations/get", .params = {{"operationId", accepted.Value()["operationId"]}}},
                               f.Context());
    REQUIRE(view.HasValue());
    CHECK(view.Value().dump().find("private-proof") == std::string::npos);
    CHECK(view.Value().dump().find("credential-and-private-argument") == std::string::npos);
}

TEST_CASE("MCP shutdown rejects new authenticated work without invoking an application", "[mcp][security]") {
    Fixture f;
    f.Approve(f.Call());
    REQUIRE(f.controller->Dispatch(f.Call(), f.Context()).HasValue());
    REQUIRE(f.controller->Shutdown().HasValue());
    CHECK(f.controller->Dispatch(f.Call(), f.Context()).HasError());
    CHECK(f.app->calls == 0);
}

TEST_CASE("MCP bounded malformed approval requests cannot broaden an exact grant", "[mcp][security]") {
    Fixture f;
    const auto original = f.Call();
    f.Approve(original);
    auto injected = original;
    injected.params["approved"] = true;
    CHECK(f.policy->Challenge(f.Context(), injected, std::chrono::minutes{1}).HasError());
    CHECK(f.controller->Dispatch(injected, f.Context()).HasError());
    injected = f.Call({{"oversized", std::string(65537, 'x')}});
    CHECK(f.policy->Challenge(f.Context(), injected, std::chrono::minutes{1}).HasError());
    REQUIRE(f.controller->Dispatch(original, f.Context()).HasValue());
    f.Pump();
    CHECK(f.app->calls == 1);
}

TEST_CASE("MCP operation reads cannot cross authenticated principals with the same handle", "[mcp][security]") {
    Fixture f{McpToolEffect::Query};
    const auto accepted = f.controller->Dispatch(f.Call(), f.Context());
    REQUIRE(accepted.HasValue());
    auto freshAdmission = f.admission;
    freshAdmission.authority.reset();
    freshAdmission = Test::Authenticate(freshAdmission, f.policy);
    auto other = f.Context();
    other.authority = freshAdmission.authority;
    const auto read =
        f.controller->Dispatch({.id = 1, .method = "operations/get", .params = {{"operationId", accepted.Value()["operationId"]}}}, other);
    REQUIRE(read.HasError());
    CHECK(read.ErrorValue().code.Value() == McpErrors::OperationUnavailable.code.Value());
    f.Pump();
    CHECK(f.app->calls == 1);
}

TEST_CASE("MCP direct registry invocation cannot bypass exact mutation approval", "[mcp][security]") {
    Fixture f;
    CHECK(f.registry->Read()->Invoke({"project.tool"}, nlohmann::json::object(), f.Context()).HasError());
    CHECK(f.app->calls == 0);
    f.Approve(f.Call());
    REQUIRE(f.registry->Read()->Invoke({"project.tool"}, nlohmann::json::object(), f.Context()).HasValue());
    CHECK(f.registry->Read()->Invoke({"project.tool"}, nlohmann::json::object(), f.Context()).HasError());
    CHECK(f.app->calls == 1);
}

TEST_CASE("MCP retained snapshot rejects foreign approved policy before sync or async side effects", "[mcp][security]") {
    Fixture owner;
    Fixture foreign;
    foreign.Approve(foreign.Call());
    const auto snapshot = owner.registry->Read();
    const auto context = foreign.Context();
    CHECK(snapshot->Invoke({"project.tool"}, nlohmann::json::object(), context).HasError());
    unsigned completions{};
    snapshot->InvokeAsync({"project.tool"}, nlohmann::json::object(), context, [&](Result<nlohmann::json> outcome) {
        ++completions;
        RequireAuthorizationDenied(outcome);
    });
    CHECK(completions == 1);
    CHECK(owner.app->calls == 0);
    CHECK(foreign.app->calls == 0);
    CHECK(McpController::Create(owner.registry, {}, foreign.policy).HasError());
}

TEST_CASE("MCP direct async admission redacts progress and rejects revoked late completion exactly once", "[mcp][security]") {
    Fixture f;
    f.app->deferred = true;
    f.Approve(f.Call());
    AsyncObservation observed;
    f.StartDeferred(observed);
    f.app->saved.ReportProgress(0.5, "private-proof");
    CHECK(observed.progressUpdates == 1);
    f.policy->Revoke(f.admission.authority);
    CHECK(f.app->saved.IsStopRequested());
    f.app->saved.ReportProgress(0.5, "private-proof");
    CHECK(observed.progressUpdates == 1);
    f.app->completion(Result<nlohmann::json>::Failure(MakeError(McpErrors::ControllerFailed, "private-proof")));
    f.app->completion(Result<nlohmann::json>::Success({{"private-proof", true}}));
    CHECK(observed.completions == 1);
}

TEST_CASE("MCP deferred application work observes credential expiry without a token timer", "[mcp][security]") {
    Fixture f{McpToolEffect::Query};
    f.app->deferred = true;
    auto admission = f.admission;
    admission.authority.reset();
    auto credential = f.policy->IssueCredential(admission, std::chrono::milliseconds{100});
    REQUIRE(credential.HasValue());
    auto principal = f.policy->Authenticate(admission, std::move(credential).Value());
    REQUIRE(principal.HasValue());
    f.admission.authority = std::move(principal).Value();
    const auto token = f.admission.authority->Cancellation();
    AsyncObservation observed;
    f.StartDeferred(observed);
    CHECK(f.app->saved.deadline == f.admission.authority->ExpiresAt());
    f.app->saved.ReportProgress(0.5, "private-proof");
    CHECK(observed.progressUpdates == 1);

    std::this_thread::sleep_until(f.admission.authority->ExpiresAt());
    CHECK_FALSE(token.IsCancellationRequested());
    CHECK(f.app->saved.IsStopRequested());
    CHECK(f.policy->Validate(f.app->saved).HasError());
    f.app->saved.ReportProgress(0.5, "private-proof");
    CHECK(observed.progressUpdates == 1);
    f.app->completion(Result<nlohmann::json>::Success({{"private-proof", true}}));
    f.app->completion(Result<nlohmann::json>::Failure(MakeError(McpErrors::ControllerFailed, "private-proof")));
    CHECK(observed.completions == 1);
}
