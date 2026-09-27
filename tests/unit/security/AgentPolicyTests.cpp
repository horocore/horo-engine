#include "Horo/Security/AgentPolicy.h"

#include <catch2/catch_test_macros.hpp>
#include <type_traits>

namespace Horo::Security::Tests {
    static_assert(!std::is_copy_constructible_v<AgentPolicy>);
    static_assert(!std::is_move_constructible_v<AgentPolicy>);

    namespace {
        [[nodiscard]] AgentContextCandidate Visible(const std::uint64_t id, const AgentDataClass dataClass, std::string text,
                                                    const bool redacted = false) {
            return {id, dataClass, std::move(text), true, redacted};
        }
    }  // namespace

    TEST_CASE("Cloud context requires an exact visible one-request preview", "[Security][Agent]") {
        AgentPolicy policy;
        REQUIRE(policy.SetProjectTrust(true, 1));
        const auto selected = Visible(1, AgentDataClass::SceneData, "selected object: cube");
        const auto ignored = Visible(2, AgentDataClass::AssetMetadata, "not selected");

        CHECK(policy.BuildContext(AgentProviderResidence::Cloud, 10, {selected}, 1).second.reason == AgentPolicyReason::ConsentRequired);
        REQUIRE(policy.GrantContextConsent(AgentProviderResidence::Cloud, 10, {selected}, 1).Allowed());
        const auto [envelope, decision] = policy.BuildContext(AgentProviderResidence::Cloud, 10, {selected, ignored}, 1);
        REQUIRE(decision.Allowed());
        REQUIRE(envelope.items.size() == 1);
        CHECK(envelope.items.front().text == selected.text);
        CHECK(envelope.totalBytes == selected.text.size());
        CHECK(envelope.projectRevision == 1);
        CHECK(policy.AuthorizeContextDispatch(envelope).Allowed());
        CHECK(policy.AuthorizeContextDispatch(envelope).reason == AgentPolicyReason::ConsentRequired);
        CHECK(policy.BuildContext(AgentProviderResidence::Cloud, 10, {selected}, 1).second.reason == AgentPolicyReason::ConsentRequired);
        CHECK(policy.BuildContext(AgentProviderResidence::Cloud, 11, {selected}, 1).second.reason == AgentPolicyReason::ConsentRequired);
        REQUIRE(policy.GrantContextConsent(AgentProviderResidence::Local, 12, {selected}, 1).Allowed());
        CHECK(policy.BuildContext(AgentProviderResidence::Cloud, 12, {selected}, 1).second.reason == AgentPolicyReason::ConsentRequired);
        CHECK(policy.BuildContext(AgentProviderResidence::Local, 12, {selected}, 1).second.Allowed());
    }

    TEST_CASE("Context dispatch rechecks bytes, provider, and project trust", "[Security][Agent]") {
        AgentPolicy policy;
        REQUIRE(policy.SetProjectTrust(true, 1));
        const auto preview = Visible(1, AgentDataClass::ToolResult, "redacted result", true);
        REQUIRE(policy.GrantContextConsent(AgentProviderResidence::Cloud, 1, {preview}, 1).Allowed());
        auto [envelope, decision] = policy.BuildContext(AgentProviderResidence::Cloud, 1, {preview}, 1);
        REQUIRE(decision.Allowed());
        envelope.items.front().text = "changed after build";
        CHECK(policy.AuthorizeContextDispatch(envelope).reason == AgentPolicyReason::StaleAuthority);
        CHECK(policy.AuthorizeContextDispatch(envelope).reason == AgentPolicyReason::ConsentRequired);

        REQUIRE(policy.GrantContextConsent(AgentProviderResidence::Cloud, 2, {preview}, 1).Allowed());
        auto [pending, secondDecision] = policy.BuildContext(AgentProviderResidence::Cloud, 2, {preview}, 1);
        REQUIRE(secondDecision.Allowed());
        REQUIRE(policy.SetProjectTrust(false, 2));
        CHECK(policy.AuthorizeContextDispatch(pending).reason == AgentPolicyReason::StaleAuthority);
    }

    TEST_CASE("Dispatch rejects duplicate or changed context envelope metadata", "[Security][Agent]") {
        AgentPolicy policy;
        REQUIRE(policy.SetProjectTrust(true, 1));
        const auto first = Visible(1, AgentDataClass::EditorContext, "first");
        const auto second = Visible(2, AgentDataClass::AssetMetadata, "second");
        REQUIRE(policy.GrantContextConsent(AgentProviderResidence::Cloud, 1, {first, second}, 1).Allowed());
        auto [envelope, decision] = policy.BuildContext(AgentProviderResidence::Cloud, 1, {first, second}, 1);
        REQUIRE(decision.Allowed());
        auto wrongProvider = envelope;
        wrongProvider.provider = AgentProviderResidence::Local;
        CHECK(policy.AuthorizeContextDispatch(wrongProvider).reason == AgentPolicyReason::ConsentRequired);
        envelope.items[1] = envelope.items[0];
        CHECK(policy.AuthorizeContextDispatch(envelope).reason == AgentPolicyReason::StaleAuthority);

        REQUIRE(policy.GrantContextConsent(AgentProviderResidence::Cloud, 2, {first}, 1).Allowed());
        auto [secondEnvelope, secondDecision] = policy.BuildContext(AgentProviderResidence::Cloud, 2, {first}, 1);
        REQUIRE(secondDecision.Allowed());
        secondEnvelope.totalBytes += 1;
        CHECK(policy.AuthorizeContextDispatch(secondEnvelope).reason == AgentPolicyReason::StaleAuthority);
    }

    TEST_CASE("An incomplete context consumes consent without creating dispatch authority", "[Security][Agent]") {
        AgentPolicy policy;
        REQUIRE(policy.SetProjectTrust(true, 1));
        const auto first = Visible(1, AgentDataClass::EditorContext, "first");
        const auto second = Visible(2, AgentDataClass::AssetMetadata, "second");
        REQUIRE(policy.GrantContextConsent(AgentProviderResidence::Cloud, 3, {first, second}, 1).Allowed());
        auto [envelope, decision] = policy.BuildContext(AgentProviderResidence::Cloud, 3, {first}, 1);
        CHECK_FALSE(decision.Allowed());
        CHECK(decision.reason == AgentPolicyReason::ContextNotAdmitted);
        CHECK(envelope.items.empty());
        CHECK(policy.BuildContext(AgentProviderResidence::Cloud, 3, {first, second}, 1).second.reason ==
              AgentPolicyReason::ConsentRequired);
        envelope.provider = AgentProviderResidence::Cloud;
        envelope.requestId = 3;
        envelope.projectRevision = 1;
        CHECK(policy.AuthorizeContextDispatch(envelope).reason == AgentPolicyReason::ConsentRequired);
    }

    TEST_CASE("Consent cannot authorize changed, hidden, or newly sensitive context", "[Security][Agent]") {
        AgentPolicy policy;
        REQUIRE(policy.SetProjectTrust(true, 1));
        const auto preview = Visible(1, AgentDataClass::ProjectFile, "redacted source", true);
        REQUIRE(policy.GrantContextConsent(AgentProviderResidence::Cloud, 10, {preview}, 1).Allowed());
        auto changed = preview;
        changed.text = "different source";
        CHECK(policy.BuildContext(AgentProviderResidence::Cloud, 10, {changed}, 1).second.reason == AgentPolicyReason::StaleAuthority);
        REQUIRE(policy.GrantContextConsent(AgentProviderResidence::Cloud, 11, {preview}, 1).Allowed());
        changed = preview;
        changed.visibleToUser = false;
        CHECK(policy.BuildContext(AgentProviderResidence::Cloud, 11, {changed}, 1).second.reason == AgentPolicyReason::ContextNotVisible);
        REQUIRE(policy.GrantContextConsent(AgentProviderResidence::Cloud, 12, {preview}, 1).Allowed());
        changed = preview;
        changed.dataClass = AgentDataClass::Credential;
        CHECK(policy.BuildContext(AgentProviderResidence::Cloud, 12, {changed}, 1).second.reason ==
              AgentPolicyReason::SensitiveDataExcluded);
        CHECK(policy.GrantContextConsent(AgentProviderResidence::Cloud, 13, {Visible(2, AgentDataClass::Credential, "opaque ref")}, 1)
                  .reason == AgentPolicyReason::SensitiveDataExcluded);
        CHECK(
            policy.GrantContextConsent(AgentProviderResidence::Cloud, 14, {Visible(3, AgentDataClass::RawSensorData, "frame")}, 1).reason ==
            AgentPolicyReason::SensitiveDataExcluded);
        CHECK(policy.GrantContextConsent(AgentProviderResidence::Cloud, 15, {Visible(4, AgentDataClass::ToolResult, "unredacted")}, 1)
                  .reason == AgentPolicyReason::ContextNotRedacted);
        CHECK(
            policy
                .GrantContextConsent(AgentProviderResidence::Cloud, 16, {Visible(5, AgentDataClass::ProposedChange, "unredacted patch")}, 1)
                .reason == AgentPolicyReason::ContextNotRedacted);
    }

    TEST_CASE("Context limits, duplicate identities, and explicit denial fail closed", "[Security][Agent]") {
        AgentPolicyLimits limits;
        limits.maximumContextItems = 2;
        limits.maximumItemBytes = 8;
        limits.maximumContextBytes = 10;
        AgentPolicy policy{limits};
        REQUIRE(policy.SetProjectTrust(true, 1));
        const auto first = Visible(1, AgentDataClass::EditorContext, "12345678");
        const auto second = Visible(2, AgentDataClass::SceneData, "1234");
        CHECK(policy.GrantContextConsent(AgentProviderResidence::Cloud, 1, {first, second}, 1).reason ==
              AgentPolicyReason::ContextLimitExceeded);
        CHECK(policy.GrantContextConsent(AgentProviderResidence::Cloud, 1, {first, first}, 1).reason == AgentPolicyReason::InvalidRequest);
        CHECK(policy.GrantContextConsent(AgentProviderResidence::Cloud, 1, {Visible(1, AgentDataClass::EditorContext, "123456789")}, 1)
                  .reason == AgentPolicyReason::ContextLimitExceeded);
        REQUIRE(policy.GrantContextConsent(AgentProviderResidence::Cloud, 1, {first}, 1).Allowed());
        policy.DenyContext(1);
        CHECK(policy.BuildContext(AgentProviderResidence::Cloud, 1, {first}, 1).second.reason == AgentPolicyReason::ConsentRequired);
        REQUIRE(policy.GrantContextConsent(AgentProviderResidence::Cloud, 2, {first}, 1).Allowed());
        CHECK(policy.BuildContext(AgentProviderResidence::Cloud, 2, {first, first}, 1).second.reason == AgentPolicyReason::InvalidRequest);
    }

    TEST_CASE("Local and cloud mutations need the same exact single-use approval", "[Security][Agent]") {
        AgentPolicy policy;
        REQUIRE(policy.SetProjectTrust(true, 1));
        const AgentToolRequest mutation{20, 7, AgentToolRisk::Mutation};
        for (const AgentProviderResidence provider : {AgentProviderResidence::Local, AgentProviderResidence::Cloud}) {
            CHECK(policy.EvaluateTool(provider, mutation, 1).reason == AgentPolicyReason::ApprovalRequired);
            CHECK(policy.AuthorizeTool(provider, mutation, 1).reason == AgentPolicyReason::ApprovalRequired);
            REQUIRE(policy.GrantToolApproval(provider, mutation, 1).Allowed());
            const auto otherProvider =
                provider == AgentProviderResidence::Local ? AgentProviderResidence::Cloud : AgentProviderResidence::Local;
            CHECK(policy.AuthorizeTool(otherProvider, mutation, 1).reason == AgentPolicyReason::StaleAuthority);
            CHECK(policy.AuthorizeTool(provider, {20, 8, AgentToolRisk::Mutation}, 1).reason == AgentPolicyReason::StaleAuthority);
            CHECK(policy.AuthorizeTool(provider, {20, 7, AgentToolRisk::Destructive}, 1).reason == AgentPolicyReason::StaleAuthority);
            CHECK(policy.AuthorizeTool(provider, mutation, 1).Allowed());
            CHECK(policy.AuthorizeTool(provider, mutation, 1).reason == AgentPolicyReason::ApprovalRequired);
        }
        CHECK(policy.AuthorizeTool(AgentProviderResidence::Local, {21, 1, AgentToolRisk::ReadOnly}, 1).Allowed());
    }

    TEST_CASE("Every write, destructive, execution, credential, and network risk requires approval", "[Security][Agent]") {
        AgentPolicy policy;
        REQUIRE(policy.SetProjectTrust(true, 1));
        for (const AgentToolRisk risk : {AgentToolRisk::Mutation, AgentToolRisk::Destructive, AgentToolRisk::Execution,
                                         AgentToolRisk::CredentialAccess, AgentToolRisk::NetworkAccess}) {
            CHECK(policy.EvaluateTool(AgentProviderResidence::Local, {1, 1, risk}, 1).reason == AgentPolicyReason::ApprovalRequired);
            CHECK(policy.EvaluateTool(AgentProviderResidence::Cloud, {1, 1, risk}, 1).reason == AgentPolicyReason::ApprovalRequired);
        }
        REQUIRE(policy.SetProjectTrust(false, 2));
        for (const AgentToolRisk risk : {AgentToolRisk::Mutation, AgentToolRisk::Destructive, AgentToolRisk::Execution,
                                         AgentToolRisk::CredentialAccess, AgentToolRisk::NetworkAccess}) {
            CHECK(policy.AuthorizeTool(AgentProviderResidence::Local, {1, 1, risk}, 2).reason == AgentPolicyReason::ProjectUntrusted);
        }
    }

    TEST_CASE("Project trust revokes pending grants and blocks risky operations", "[Security][Agent]") {
        AgentPolicy policy;
        REQUIRE(policy.SetProjectTrust(true, 1));
        const auto context = Visible(1, AgentDataClass::EditorContext, "visible");
        const AgentToolRequest execution{4, 3, AgentToolRisk::Execution};
        REQUIRE(policy.GrantContextConsent(AgentProviderResidence::Cloud, 2, {context}, 1).Allowed());
        REQUIRE(policy.GrantToolApproval(AgentProviderResidence::Local, execution, 1).Allowed());
        REQUIRE(policy.SetProjectTrust(false, 2));
        CHECK_FALSE(policy.SetProjectTrust(true, 1));
        CHECK(policy.BuildContext(AgentProviderResidence::Cloud, 2, {context}, 1).second.reason == AgentPolicyReason::StaleAuthority);
        CHECK(policy.BuildContext(AgentProviderResidence::Cloud, 2, {context}, 2).second.reason == AgentPolicyReason::ProjectUntrusted);
        CHECK(policy.AuthorizeTool(AgentProviderResidence::Local, execution, 1).reason == AgentPolicyReason::StaleAuthority);
        CHECK(policy.GrantToolApproval(AgentProviderResidence::Local, execution, 2).reason == AgentPolicyReason::ProjectUntrusted);
        CHECK(policy.GrantContextConsent(AgentProviderResidence::Cloud, 3, {context}, 2).reason == AgentPolicyReason::ProjectUntrusted);
        CHECK(policy.BuildContext(AgentProviderResidence::Cloud, 3, {context}, 2).second.reason == AgentPolicyReason::ProjectUntrusted);
        CHECK(policy.AuthorizeTool(AgentProviderResidence::Cloud, execution, 2).reason == AgentPolicyReason::ProjectUntrusted);
        CHECK(policy.AuthorizeTool(AgentProviderResidence::Local, {5, 1, AgentToolRisk::ReadOnly}, 2).Allowed());
        CHECK(policy.AuthorizeTool(AgentProviderResidence::Cloud, {5, 1, AgentToolRisk::ReadOnly}, 2).reason ==
              AgentPolicyReason::ProjectUntrusted);
    }

    TEST_CASE("Denial and audit retain decisions but never raw context or arguments", "[Security][Agent]") {
        AgentPolicyLimits limits;
        limits.maximumAuditEntries = 3;
        AgentPolicy policy{limits};
        REQUIRE(policy.SetProjectTrust(true, 1));
        const AgentToolRequest operation{1, 1, AgentToolRisk::Destructive};
        REQUIRE(policy.GrantToolApproval(AgentProviderResidence::Local, operation, 1).Allowed());
        policy.DenyTool(operation.operationId);
        CHECK(policy.AuthorizeTool(AgentProviderResidence::Local, operation, 1).reason == AgentPolicyReason::ApprovalRequired);
        const auto history = policy.AuditSnapshot();
        REQUIRE(history.size() == 3);
        CHECK(history.front().reason == AgentPolicyReason::Denied);
        CHECK(history.front().subjectId == operation.operationId);
        CHECK(history.front().proposalRevision == operation.proposalRevision);
        CHECK(history.back().reason == AgentPolicyReason::ApprovalRequired);
        CHECK(history.back().toolRisk == AgentToolRisk::Destructive);
        CHECK(history.back().projectRevision == 1);
        CHECK(history.back().subjectId == operation.operationId);
        CHECK(history.back().proposalRevision == operation.proposalRevision);
        CHECK(history.back().timestamp.time_since_epoch().count() > 0);
        AgentPolicyLimits invalid;
        invalid.maximumContextItems = 0;
        AgentPolicy closed{invalid};
        CHECK_FALSE(closed.SetProjectTrust(true, 1));
    }

    TEST_CASE("Unknown classification, provider, and tool risk fail closed", "[Security][Agent]") {
        AgentPolicy policy;
        REQUIRE(policy.SetProjectTrust(true, 1));
        const auto unknownProvider = static_cast<AgentProviderResidence>(99);
        const auto unknownClass = static_cast<AgentDataClass>(99);
        const auto unknownRisk = static_cast<AgentToolRisk>(99);
        CHECK(policy.GrantContextConsent(unknownProvider, 1, {Visible(1, AgentDataClass::EditorContext, "text")}, 1).reason ==
              AgentPolicyReason::InvalidRequest);
        CHECK(policy.GrantContextConsent(AgentProviderResidence::Cloud, 1, {Visible(1, unknownClass, "text")}, 1).reason ==
              AgentPolicyReason::InvalidRequest);
        CHECK(policy.EvaluateTool(unknownProvider, {1, 1, AgentToolRisk::ReadOnly}, 1).reason == AgentPolicyReason::InvalidRequest);
        CHECK(policy.AuthorizeTool(AgentProviderResidence::Local, {1, 1, unknownRisk}, 1).reason == AgentPolicyReason::InvalidRequest);
        CHECK(policy.GrantToolApproval(AgentProviderResidence::Local, {1, 1, unknownRisk}, 1).reason == AgentPolicyReason::InvalidRequest);
    }
}  // namespace Horo::Security::Tests
