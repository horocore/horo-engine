#include "Horo/Network/ReplicationDeclarationPolicy.h"
#include "ReplicationDescriptorTestSupport.h"

#include <algorithm>
#include <array>
#include <utility>

namespace Horo::Network {
    using namespace TestSupport;

    namespace {
        constexpr ReplicationDeclarationPolicyLimits PolicyLimits{Limits, 4, 32};
        const std::array<ModuleId, 1> Modules{{{"game.replication"}}};

        ReplicationDescriptorSnapshotPtr Registered(const ReplicationSchemaDescriptor &schema = Schema()) {
            const std::array schemas{schema};
            return BuildReplicationDescriptorSnapshot(schemas, Limits).Value();
        }

        ReplicationDeclarationAssessment Assessment(const ReplicationDeclaration &declaration,
                                                    const ReplicationDescriptorSnapshotPtr &registered = Registered(),
                                                    const ReplicationDeclarationUse use = ReplicationDeclarationUse::Cook) {
            return AssessReplicationDeclarations(std::span{&declaration, 1}, registered, Modules, use, PolicyLimits).Value();
        }

        bool HasProblem(const ReplicationDeclarationAssessment &assessment, const ReplicationDeclarationProblem problem,
                        const std::optional<FieldId> field = {}) {
            return std::ranges::any_of(assessment.Diagnostics(), [&](const auto &diagnostic) {
                return diagnostic.problem == problem && diagnostic.field == field;
            });
        }
    }  // namespace

    TEST_CASE("Replication declaration policy admits exact registered semantics for cook and both peer roles",
              "[unit][network][replication][declaration-policy]") {
        for (const auto use :
             {ReplicationDeclarationUse::Cook, ReplicationDeclarationUse::ServerActivation, ReplicationDeclarationUse::ClientActivation}) {
            const auto result = Assessment({Schema()}, Registered(), use);
            REQUIRE(result.Admitted());
            REQUIRE(result.RequireAdmission().HasValue());
            REQUIRE(result.AdmittedSchemas().size() == 1);
            REQUIRE(result.AdmittedSchemas().front() == SchemaId(10));
            REQUIRE(result.Diagnostics().empty());
            REQUIRE(result.OpaqueDeclarations().empty());
        }
        const auto inspection = Assessment({Schema()}, Registered(), ReplicationDeclarationUse::EditorInspection);
        REQUIRE_FALSE(inspection.Admitted());
        REQUIRE(inspection.AdmittedSchemas().empty());
        REQUIRE(inspection.RequireAdmission().HasError());
    }

    TEST_CASE("Replication declaration diagnostics retain every missing module and schema without partial admission",
              "[unit][network][replication][declaration-policy]") {
        const std::array declarations{ReplicationDeclaration{Schema(10)}, ReplicationDeclaration{Schema(20)},
                                      ReplicationDeclaration{Schema(30)}};
        const std::array<ModuleId, 0> none{};
        const auto missing = AssessReplicationDeclarations(declarations, {}, none, ReplicationDeclarationUse::Cook, PolicyLimits).Value();
        REQUIRE_FALSE(missing.Admitted());
        REQUIRE(missing.Diagnostics().size() == 3);
        REQUIRE(missing.RequireAdmission().ErrorValue().diagnostics.size() == 3);
        for (std::size_t index = 0; index < declarations.size(); ++index) {
            REQUIRE(missing.Diagnostics()[index].declarationIndex == index);
            REQUIRE(missing.Diagnostics()[index].schema == declarations[index].schema.id);
            REQUIRE(missing.Diagnostics()[index].owner == declarations[index].schema.owner);
            REQUIRE(missing.Diagnostics()[index].problem == ReplicationDeclarationProblem::MissingModule);
        }
        const auto unknown =
            AssessReplicationDeclarations(declarations, Registered(), Modules, ReplicationDeclarationUse::ServerActivation, PolicyLimits)
                .Value();
        REQUIRE_FALSE(unknown.Admitted());
        REQUIRE(unknown.Diagnostics().size() == 2);
        REQUIRE(unknown.AdmittedSchemas().empty());
        REQUIRE(unknown.Diagnostics()[0].schema == SchemaId(20));
        REQUIRE(unknown.Diagnostics()[1].schema == SchemaId(30));
        REQUIRE(HasProblem(unknown, ReplicationDeclarationProblem::UnknownSchema));
    }

    TEST_CASE("Replication declaration policy collects all field semantic changes and required omissions",
              "[unit][network][replication][declaration-policy]") {
        const auto expected = Schema(10, {Field(1), Field(2), Field(3)});
        auto actual = expected;
        actual.version = {1, 1};
        actual.compatibility.maximum = actual.version;
        actual.fields.erase(actual.fields.begin());
        actual.tombstonedFields.push_back(FieldIdValue(1));
        actual.fields[0].codec = Codec(2);
        actual.fields[1].valueType = ValueType(2);
        const auto assessment = Assessment({expected}, Registered(actual));
        REQUIRE_FALSE(assessment.Admitted());
        REQUIRE(assessment.Diagnostics().size() == 3);
        for (const auto id : {1U, 2U, 3U})
            REQUIRE(HasProblem(assessment, ReplicationDeclarationProblem::FieldIncompatible, FieldIdValue(id)));
        const auto rejected = assessment.RequireAdmission();
        REQUIRE(rejected.HasError());
        REQUIRE(rejected.ErrorValue().diagnostics.size() == 3);
        REQUIRE(rejected.ErrorValue().diagnostics.front().path == "replication[0].fields[1]");
        REQUIRE(rejected.ErrorValue().diagnostics.front().message.find("game.replication") != std::string::npos);
    }

    TEST_CASE("Replication declaration policy never guesses major versions or foreign ownership",
              "[unit][network][replication][declaration-policy]") {
        auto actual = Schema();
        actual.version = {2, 0};
        actual.compatibility = {{2, 0}, {2, 0}};
        actual.owner.value = "foreign.module";
        const auto rejected = Assessment({Schema()}, Registered(actual));
        REQUIRE_FALSE(rejected.Admitted());
        REQUIRE(HasProblem(rejected, ReplicationDeclarationProblem::OwnerMismatch));
        REQUIRE(HasProblem(rejected, ReplicationDeclarationProblem::VersionIncompatible));
        REQUIRE(rejected.Diagnostics().front().registeredVersion == actual.version);
    }

    TEST_CASE("Replication declaration policy accepts explicitly compatible optional minor additions only",
              "[unit][network][replication][declaration-policy]") {
        const auto expected = Schema();
        auto actual = expected;
        actual.version = {1, 1};
        actual.compatibility.maximum = actual.version;
        auto optional = Field(2, actual.version);
        optional.requirement = ReplicationFieldRequirement::Optional;
        optional.canonicalDefault = ReplicationFieldDefault{{std::byte{0}}};
        actual.fields.push_back(optional);
        REQUIRE(Assessment({expected}, Registered(actual)).Admitted());

        actual.compatibility.minimum = actual.version;
        REQUIRE(HasProblem(Assessment({expected}, Registered(actual)), ReplicationDeclarationProblem::VersionIncompatible));
        REQUIRE_FALSE(Assessment({actual}, Registered(expected)).Admitted());
    }

    TEST_CASE("Replication declaration tombstones cannot be reused or erased during compatibility admission",
              "[unit][network][replication][declaration-policy]") {
        auto expected = Schema();
        expected.tombstonedFields.push_back(FieldIdValue(2));
        auto actual = Schema(10, {Field(1), Field(2)});
        const auto rejected = Assessment({expected}, Registered(actual));
        REQUIRE_FALSE(rejected.Admitted());
        REQUIRE(HasProblem(rejected, ReplicationDeclarationProblem::FieldIncompatible, FieldIdValue(2)));
        actual = Schema();
        REQUIRE(HasProblem(Assessment({expected}, Registered(actual)), ReplicationDeclarationProblem::FieldIncompatible, FieldIdValue(2)));
    }

    TEST_CASE("Editor replication inspection owns unresolved metadata without acquiring runtime authority",
              "[unit][network][replication][declaration-policy]") {
        auto schema = Schema(20);
        schema.fields[0].requirement = ReplicationFieldRequirement::Optional;
        schema.fields[0].canonicalDefault = ReplicationFieldDefault{{std::byte{42}}};
        auto inspected = Assessment({schema}, Registered(), ReplicationDeclarationUse::EditorInspection);
        schema.fields[0].canonicalDefault->canonicalBytes[0] = std::byte{0};
        REQUIRE_FALSE(inspected.Admitted());
        REQUIRE(inspected.AdmittedSchemas().empty());
        REQUIRE(inspected.OpaqueDeclarations().size() == 1);
        REQUIRE(inspected.OpaqueDeclarations().front().schema.id == SchemaId(20));
        REQUIRE(inspected.OpaqueDeclarations().front().schema.owner.value == "game.replication");
        REQUIRE(inspected.OpaqueDeclarations().front().schema.fields[0].canonicalDefault->canonicalBytes.front() == std::byte{42});
        REQUIRE(inspected.RequireAdmission().HasError());
    }

    TEST_CASE("Explicit optional replication omission grants no schema authority and cannot hide malformed declarations",
              "[unit][network][replication][declaration-policy]") {
        ReplicationDeclaration optional{Schema(20), ReplicationDeclarationRequirement::Optional};
        const auto omitted = Assessment(optional);
        REQUIRE(omitted.Admitted());
        REQUIRE(omitted.AdmittedSchemas().empty());
        REQUIRE(omitted.OpaqueDeclarations().empty());
        REQUIRE_FALSE(omitted.Diagnostics().front().blocking);
        optional.schema.fields.push_back(optional.schema.fields.front());
        const auto malformed = Assessment(optional);
        REQUIRE_FALSE(malformed.Admitted());
        REQUIRE(HasProblem(malformed, ReplicationDeclarationProblem::MalformedDeclaration));
        REQUIRE(malformed.Diagnostics().front().blocking);
    }

    TEST_CASE("Replication declaration duplicate identities fail even when authored optional",
              "[unit][network][replication][declaration-policy]") {
        const std::array duplicates{ReplicationDeclaration{Schema(), ReplicationDeclarationRequirement::Optional},
                                    ReplicationDeclaration{Schema(), ReplicationDeclarationRequirement::Optional}};
        const auto result =
            AssessReplicationDeclarations(duplicates, Registered(), Modules, ReplicationDeclarationUse::Cook, PolicyLimits).Value();
        REQUIRE_FALSE(result.Admitted());
        REQUIRE(result.Diagnostics().size() == 2);
        REQUIRE(result.AdmittedSchemas().empty());
        REQUIRE(HasProblem(result, ReplicationDeclarationProblem::DuplicateSchema));
        REQUIRE(result.Diagnostics().front().blocking);
    }

    TEST_CASE("Replication declaration diagnostic overflow is explicit and always rejects publication",
              "[unit][network][replication][declaration-policy]") {
        const std::array declarations{ReplicationDeclaration{Schema(20), ReplicationDeclarationRequirement::Optional},
                                      ReplicationDeclaration{Schema(30), ReplicationDeclarationRequirement::Optional}};
        auto limits = PolicyLimits;
        limits.maximumDiagnostics = 1;
        const auto result =
            AssessReplicationDeclarations(declarations, Registered(), Modules, ReplicationDeclarationUse::ClientActivation, limits).Value();
        REQUIRE(result.DiagnosticsOverflowed());
        REQUIRE_FALSE(result.Admitted());
        REQUIRE(result.Diagnostics().size() == 1);
        REQUIRE(result.Diagnostics().front().schema == SchemaId(20));
        REQUIRE(result.AdmittedSchemas().empty());
        const auto error = result.RequireAdmission();
        REQUIRE(error.HasError());
        REQUIRE(error.ErrorValue().diagnostics.front().code.Value() == "replication.declaration.diagnostic_overflow");
    }

    TEST_CASE("Single declaration diagnostic overflow retains the first reason and cannot publish a compatible subset",
              "[unit][network][replication][declaration-policy]") {
        auto expected = Schema();
        auto actual = expected;
        actual.owner = ModuleId{"other.module"};
        actual.version = {2, 0};
        actual.compatibility = {actual.version, actual.version};
        actual.fields.front().codec = Codec(2);
        const std::array declarations{ReplicationDeclaration{expected}, ReplicationDeclaration{Schema(20)}};
        const std::array schemas{actual, Schema(20)};
        auto limits = PolicyLimits;
        limits.maximumDiagnostics = 1;
        const auto registered = BuildReplicationDescriptorSnapshot(schemas, limits.descriptors).Value();
        const auto result =
            AssessReplicationDeclarations(declarations, registered, Modules, ReplicationDeclarationUse::ServerActivation, limits).Value();
        REQUIRE(result.Diagnostics().size() == 1);
        REQUIRE(result.Diagnostics().front().problem == ReplicationDeclarationProblem::OwnerMismatch);
        REQUIRE(result.DiagnosticsOverflowed());
        REQUIRE_FALSE(result.Admitted());
        REQUIRE(result.AdmittedSchemas().empty());
        REQUIRE(result.RequireAdmission().HasError());
    }

    TEST_CASE("Replication declaration policy rejects invalid operation and capacity envelopes before publication",
              "[unit][network][replication][declaration-policy]") {
        const std::array declarations{ReplicationDeclaration{Schema()}};
        auto limits = PolicyLimits;
        limits.descriptors.maximumSchemas = 0;
        RequireError(AssessReplicationDeclarations(declarations, Registered(), Modules, ReplicationDeclarationUse::Cook, limits),
                     NetworkErrors::ReplicationDescriptorInvalid);
        limits = PolicyLimits;
        limits.maximumDiagnostics = 0;
        RequireError(AssessReplicationDeclarations(declarations, Registered(), Modules, ReplicationDeclarationUse::Cook, limits),
                     NetworkErrors::ReplicationDescriptorInvalid);
        RequireError(AssessReplicationDeclarations(declarations, Registered(), Modules, ReplicationDeclarationUse::Count, PolicyLimits),
                     NetworkErrors::ReplicationDescriptorInvalid);
        limits = PolicyLimits;
        limits.maximumModules = 1;
        const std::array<ModuleId, 2> modules{{{"game.replication"}, {"other.module"}}};
        RequireError(AssessReplicationDeclarations(declarations, Registered(), modules, ReplicationDeclarationUse::Cook, limits),
                     NetworkErrors::ReplicationCapacityExceeded);
    }

    TEST_CASE("Malformed declaration owner bytes never enter retained diagnostics or inspection metadata",
              "[unit][network][replication][declaration-policy]") {
        auto malformed = Schema();
        malformed.owner.value.assign(PolicyLimits.descriptors.maximumOwnerIdentityBytes + 1, 'x');
        const std::array declarations{ReplicationDeclaration{malformed}};
        const auto result =
            AssessReplicationDeclarations(declarations, {}, Modules, ReplicationDeclarationUse::EditorInspection, PolicyLimits).Value();
        REQUIRE_FALSE(result.Admitted());
        REQUIRE(result.Diagnostics().size() == 1);
        REQUIRE(result.Diagnostics().front().owner.value.empty());
        REQUIRE(result.OpaqueDeclarations().empty());
        REQUIRE(result.RequireAdmission().ErrorValue().diagnostics.front().message.find(malformed.owner.value) == std::string::npos);
    }
}  // namespace Horo::Network
