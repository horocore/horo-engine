#include "Horo/Extensions/ExtensionCapabilityAdmission.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Extensions::Tests {
    namespace {
        [[nodiscard]] ExtensionAdmissionPolicy Policy() {
            return {
                .revision = 7,
                .knownPermissions = {{"project.read"}, {"project.write.generated"}, {"process.execute"}},
                .approvedPermissions = {{"project.read"}, {"project.write.generated"}},
                .availableCapabilities = {{"horo.assets.import"}, {"horo.project.validate"}},
            };
        }

        [[nodiscard]] ExtensionAdmissionRequest Request() {
            return {
                .extensionId = "com.example.asset-tools",
                .moduleId = "com.example.asset-tools.backend",
                .activationGeneration = 12,
                .capabilities =
                    {
                        {.capability = {"horo.project.validate"}, .requiredPermissions = {{"project.read"}}},
                        {.capability = {"horo.assets.import"}, .requiredPermissions = {{"project.read"}, {"project.write.generated"}}},
                    },
            };
        }

        void RequireErrorCode(const auto &result, const std::string &code) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == code);
        }
    }  // namespace

    TEST_CASE("Capability admission grants only the complete declared and approved set", "[Extensions][Capabilities][Admission]") {
        auto admitted = ExtensionCapabilityAdmission::Evaluate(Request(), Policy());
        REQUIRE(admitted.HasValue());
        CHECK(admitted.Value().PolicyRevision() == 7);
        REQUIRE(admitted.Value().Capabilities().size() == 2);
        CHECK(admitted.Value().Capabilities()[0].value == "horo.assets.import");
        CHECK(admitted.Value().Capabilities()[1].value == "horo.project.validate");

        auto importer = admitted.Value().Grant({"horo.assets.import"});
        REQUIRE(importer.HasValue());
        CHECK(importer.Value().Capability().value == "horo.assets.import");
        CHECK(importer.Value().Activation().ExtensionId() == "com.example.asset-tools");
        CHECK(importer.Value().Activation().ModuleId() == "com.example.asset-tools.backend");
        CHECK(importer.Value().Activation().Generation() == 12);
        auto use = importer.Value().AcquireUse("com.example.asset-tools", "com.example.asset-tools.backend", 12);
        REQUIRE(use.HasValue());
        CHECK(use.Value().Capability().value == "horo.assets.import");
        CHECK(use.Value().Activation().Generation() == 12);
        CHECK(use.Value().IsUsable());

        RequireErrorCode(admitted.Value().Grant({"horo.process.execute"}), "capability_unavailable");
    }

    TEST_CASE("Permission denial rejects the complete capability request before any grant exists",
              "[Extensions][Capabilities][Admission]") {
        auto request = Request();
        SECTION("known but unapproved permission") {
            request.capabilities.back().requiredPermissions.push_back({"process.execute"});
            RequireErrorCode(ExtensionCapabilityAdmission::Evaluate(request, Policy()), "permission_denied");
        }
        SECTION("unknown permission defaults to denial") {
            request.capabilities.back().requiredPermissions.push_back({"network.server"});
            RequireErrorCode(ExtensionCapabilityAdmission::Evaluate(request, Policy()), "permission_denied");
        }
        SECTION("unavailable capability rejects all siblings") {
            request.capabilities.push_back({.capability = {"horo.network.server"}});
            RequireErrorCode(ExtensionCapabilityAdmission::Evaluate(request, Policy()), "capability_unavailable");
        }
    }

    TEST_CASE("Capability admission rejects malformed policy and request authority", "[Extensions][Capabilities][Admission]") {
        SECTION("policy revision is mandatory") {
            auto policy = Policy();
            policy.revision = 0;
            RequireErrorCode(ExtensionCapabilityAdmission::Evaluate(Request(), policy), "capability_admission_invalid");
        }
        SECTION("approval must belong to sealed catalog") {
            auto policy = Policy();
            policy.approvedPermissions.push_back({"network.server"});
            RequireErrorCode(ExtensionCapabilityAdmission::Evaluate(Request(), policy), "capability_admission_invalid");
        }
        SECTION("duplicate policy identity") {
            auto policy = Policy();
            policy.availableCapabilities.push_back({"horo.assets.import"});
            RequireErrorCode(ExtensionCapabilityAdmission::Evaluate(Request(), policy), "capability_admission_invalid");
        }
        SECTION("invalid owner identity") {
            auto request = Request();
            request.moduleId = "Not Canonical";
            RequireErrorCode(ExtensionCapabilityAdmission::Evaluate(request, Policy()), "capability_admission_invalid");
        }
        SECTION("zero activation generation") {
            auto request = Request();
            request.activationGeneration = 0;
            RequireErrorCode(ExtensionCapabilityAdmission::Evaluate(request, Policy()), "capability_admission_invalid");
        }
        SECTION("duplicate capability request") {
            auto request = Request();
            request.capabilities.push_back(request.capabilities.front());
            RequireErrorCode(ExtensionCapabilityAdmission::Evaluate(request, Policy()), "capability_admission_invalid");
        }
        SECTION("duplicate permission dependency") {
            auto request = Request();
            request.capabilities.front().requiredPermissions.push_back({"project.read"});
            RequireErrorCode(ExtensionCapabilityAdmission::Evaluate(request, Policy()), "capability_admission_invalid");
        }
    }

    TEST_CASE("Capability handles enforce owner generation and revocation", "[Extensions][Capabilities][Admission]") {
        auto admitted = ExtensionCapabilityAdmission::Evaluate(Request(), Policy());
        REQUIRE(admitted.HasValue());
        ExtensionCapabilityAdmission admission = std::move(admitted).Value();
        auto granted = admission.Grant({"horo.project.validate"});
        REQUIRE(granted.HasValue());
        ExtensionCapabilityHandle handle = std::move(granted).Value();

        RequireErrorCode(handle.AcquireUse("com.example.other", "com.example.asset-tools.backend", 12), "permission_denied");
        RequireErrorCode(handle.AcquireUse("com.example.asset-tools", "com.example.asset-tools.frontend", 12), "permission_denied");
        RequireErrorCode(handle.AcquireUse("com.example.asset-tools", "com.example.asset-tools.backend", 13), "permission_denied");

        auto inFlight = handle.AcquireUse("com.example.asset-tools", "com.example.asset-tools.backend", 12);
        REQUIRE(inFlight.HasValue());
        admission.Revoke();
        CHECK(inFlight.Value().Capability().value == "horo.project.validate");
        CHECK_FALSE(handle.IsUsable());
        RequireErrorCode(handle.AcquireUse("com.example.asset-tools", "com.example.asset-tools.backend", 12), "capability_revoked");
        RequireErrorCode(admission.Grant({"horo.project.validate"}), "capability_revoked");
        admission.Revoke();
    }

    TEST_CASE("Activation leases expose only live activation evidence", "[Extensions][Capabilities][Admission]") {
        auto admitted = ExtensionCapabilityAdmission::Evaluate(Request(), Policy());
        REQUIRE(admitted.HasValue());
        ExtensionCapabilityAdmission admission = std::move(admitted).Value();

        ExtensionActivationLease lease = admission.ActivationLease();
        CHECK(lease.Activation().ExtensionId() == "com.example.asset-tools");
        CHECK(lease.Activation().ModuleId() == "com.example.asset-tools.backend");
        CHECK(lease.Activation().Generation() == 12);
        CHECK(lease.IsUsable());

        ExtensionActivationLease movedLease = std::move(lease);
        CHECK(lease.Activation().ExtensionId().empty());
        CHECK(movedLease.Activation().ExtensionId() == "com.example.asset-tools");

        admission.Revoke();
        CHECK_FALSE(movedLease.IsUsable());
    }

    TEST_CASE("Capability admission destruction revokes retained handles", "[Extensions][Capabilities][Admission]") {
        auto makeHandle = [] {
            auto admitted = ExtensionCapabilityAdmission::Evaluate(Request(), Policy());
            REQUIRE(admitted.HasValue());
            auto granted = admitted.Value().Grant({"horo.assets.import"});
            REQUIRE(granted.HasValue());
            return std::move(granted).Value();
        };

        const ExtensionCapabilityHandle handle = makeHandle();
        CHECK_FALSE(handle.IsUsable());
        RequireErrorCode(handle.AcquireUse("com.example.asset-tools", "com.example.asset-tools.backend", 12), "capability_revoked");
    }

    TEST_CASE("Capability admission permits an explicit empty-authority module", "[Extensions][Capabilities][Admission]") {
        auto request = Request();
        request.capabilities.clear();
        auto admitted = ExtensionCapabilityAdmission::Evaluate(request, Policy());
        REQUIRE(admitted.HasValue());
        CHECK(admitted.Value().Capabilities().empty());
        RequireErrorCode(admitted.Value().Grant({"horo.assets.import"}), "capability_unavailable");
    }
}  // namespace Horo::Extensions::Tests
