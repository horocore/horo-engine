#include "Horo/Gameplay/LuaBehavior.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace {
    using namespace Horo;
    using namespace Horo::Gameplay;
    using namespace Horo::Network;

    BehaviorTypeId Owner() {
        return BehaviorTypeId::Parse("game.tests.replicated").Value();
    }

    ReplicationSchemaId Schema() {
        return ReplicationSchemaId::Create(42).Value();
    }

    std::string Source(const std::string_view fields = R"(
        { id=7, value_type=1, codec=2, kind="number", introduced_major=1, introduced_minor=0,
          condition="owner_only", requirement="required", maximum_bytes=8, maximum_elements=1 }
    )",
                       const unsigned minor = 0) {
        return std::string{R"(return horo.behavior {
            display_name="Replicated",
            replication={ major=1, minor=)"} +
               std::to_string(minor) + ", minimum_minor=0, maximum_minor=" + std::to_string(minor) +
               R"(, capture_phase="gameplay", apply_phase="pre_physics",
            fields={)" +
               std::string{fields} + R"(}, tombstones={} }
        })";
    }

    auto Compile(const std::string &source) {
        return LuaBehaviorProgram::Compile(source, Owner(), "replicated.horo_script", {}, Schema(), {.value = "game.tests"});
    }

    GameplayReplicationRegistration Native() {
        const auto value = ReplicationValueTypeId::Create(1).Value();
        const auto codec = ReplicationCodecId::Create(2).Value();
        GameplayReplicationRegistration registration;
        registration.owner = Owner();
        registration.schema.id = Schema();
        registration.schema.version = {1, 0};
        registration.schema.compatibility = {{1, 0}, {1, 0}};
        registration.schema.owner = {.value = "game.tests"};
        registration.schema.fields = {{.id = FieldId::Create(7).Value(),
                                       .valueType = value,
                                       .codec = codec,
                                       .introducedVersion = {1, 0},
                                       .condition = ReplicationCondition::OwnerOnly,
                                       .limits = {8, 1}}};
        registration.serializers.push_back(CanonicalScalarReplicationSerializer::Create({.valueType = value,
                                                                                         .codec = codec,
                                                                                         .owner = registration.schema.owner,
                                                                                         .valueKind = ReplicationValueKind::FloatingPoint,
                                                                                         .maximumEncodedBytes = 8,
                                                                                         .maximumElementCount = 1})
                                               .Value());
        return registration;
    }

    void Write(const std::filesystem::path &path, const std::string_view contents) {
        std::ofstream output{path};
        output << contents;
    }

    void CheckRejectedReload(const LuaBehaviorProgram &program, const Result<void> &result, const std::filesystem::path &diagnosticPath,
                             const GameplayReplicationLease &previous, const std::uint64_t revision) {
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().message.find(diagnosticPath.string()) != std::string::npos);
        CHECK(program.AcquireReplication().Value().Descriptors() == previous.Descriptors());
        CHECK(program.Revision() == revision);
    }

    struct Directory final {
        std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            ("horo lua replication " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        Directory() {
            std::filesystem::create_directories(path);
        }

        ~Directory() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    };

}  // namespace

TEST_CASE("script metadata and native declarations produce equivalent canonical registries", "[gameplay][replication][lua]") {
    auto program = Compile(Source());
    REQUIRE(program.HasValue());
    const std::array behaviors{program.Value()->Registration()};
    ReplicationRegistrationRegistry native{"game.tests"};
    REQUIRE(native.Register(Native()).HasValue());
    REQUIRE(native.Freeze({}, behaviors, {}).HasValue());
    const auto nativeLease = native.Acquire().Value();
    const auto scriptLease = program.Value()->AcquireReplication().Value();
    // Fixed v1 canonical wire vector, independently encoded in big-endian order (137 bytes).
    constexpr std::string_view fingerprint = "sha256:4baf64ed25af9cc8f64b622de9c6eb3f089244456252553b637a5787a15f72f7";
    CHECK(FormatSha256(nativeLease.Descriptors()->Fingerprint()) == fingerprint);
    CHECK(FormatSha256(scriptLease.Descriptors()->Fingerprint()) == fingerprint);
    CHECK(nativeLease.Descriptors()->Fingerprint() == scriptLease.Descriptors()->Fingerprint());
    CHECK(nativeLease.Descriptors()->Schemas().front() == scriptLease.Descriptors()->Schemas().front());
    CHECK(std::get<BehaviorTypeId>(scriptLease.Registrations().front().owner) == Owner());
    CHECK(nativeLease.Serializers().Encode(Schema(), FieldId::Create(7).Value(), 4.5).Value() ==
          scriptLease.Serializers().Encode(Schema(), FieldId::Create(7).Value(), 4.5).Value());

    ReplicationRegistrationRegistry composed{"game.tests"};
    REQUIRE(composed.Register(*program.Value()->ReplicationDeclaration()).HasValue());
    REQUIRE(composed.Register(*program.Value()->ReplicationDeclaration()).HasError());
    REQUIRE(composed.Freeze({}, behaviors, {}).HasValue());
    REQUIRE(composed.Register(Native()).HasError());
    auto clone = program.Value()->Clone();
    REQUIRE(clone.HasValue());
    CHECK(clone.Value()->AcquireReplication().Value().Descriptors()->Fingerprint() == scriptLease.Descriptors()->Fingerprint());
    auto ownedProgram = std::move(program).Value();
    ownedProgram.reset();
    CHECK(scriptLease.Serializers().Encode(Schema(), FieldId::Create(7).Value(), 4.5).HasValue());
}

TEST_CASE("script replication compile and reload failures retain the complete published generation", "[gameplay][replication][lua]") {
    auto program = Compile(Source()).Value();
    const auto previous = program->AcquireReplication().Value();
    REQUIRE(Compile("broken Lua source").HasError());
    CHECK(program->Revision() == 1);
    CHECK(program->AcquireReplication().Value().Descriptors() == previous.Descriptors());

    auto changed = Source();
    changed.replace(changed.find("id=7"), 4, "id=8");
    auto candidate = Compile(changed);
    REQUIRE(candidate.HasValue());
    const auto rejected = program->ReplaceCompatible(std::move(candidate).Value());
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().message.find("replicated.horo_script") != std::string::npos);
    CHECK(program->Revision() == 1);
    CHECK(program->AcquireReplication().Value().Descriptors() == previous.Descriptors());

    auto removed = LuaBehaviorProgram::Compile("return horo.behavior {display_name='Plain'}", Owner(), "removed.horo_script");
    REQUIRE(removed.HasValue());
    CHECK(program->ReplaceCompatible(std::move(removed).Value()).HasError());
    CHECK(program->ReplaceCompatible(nullptr).HasError());
    CHECK(program->AcquireReplication().Value().Descriptors() == previous.Descriptors());
}

TEST_CASE("script replication accepts compatible optional additions and pins old leases", "[gameplay][replication][lua]") {
    auto program = Compile(Source()).Value();
    const auto previous = program->AcquireReplication().Value();
    const auto fields = R"(
        { id=7, value_type=1, codec=2, kind="number", introduced_major=1, introduced_minor=0,
          condition="owner_only", requirement="required", maximum_bytes=8, maximum_elements=1 },
        { id=9, value_type=3, codec=4, kind="boolean", introduced_major=1, introduced_minor=1,
          condition="always", requirement="optional", default=false, maximum_bytes=1, maximum_elements=1 }
    )";
    auto candidate = Compile(Source(fields, 1));
    REQUIRE(candidate.HasValue());
    REQUIRE(program->ReplaceCompatible(std::move(candidate).Value()).HasValue());
    CHECK(FormatSha256(program->AcquireReplication().Value().Descriptors()->Fingerprint()) ==
          "sha256:c80416267e305c91974697b75e721236450d060e07b2b2fb038cd7f6908cced7");
    CHECK(program->Revision() == 2);
    CHECK(previous.Descriptors()->Schemas().front().fields.size() == 1);
    CHECK(program->AcquireReplication().Value().Descriptors()->Schemas().front().fields.size() == 2);
    CHECK(previous.Serializers().Encode(Schema(), FieldId::Create(7).Value(), 1.0).HasValue());
}

TEST_CASE("script replication rejects malformed and foreign declarations before publication", "[gameplay][replication][lua]") {
    const auto source = Source();
    for (const auto &[before, after] :
         std::array<std::pair<std::string_view, std::string_view>, 9>{{{"id=7", "id=0"},
                                                                       {"id=7", "id=-1"},
                                                                       {"id=7", "id=4294967296"},
                                                                       {"kind=\"number\"", "kind=\"native_memory\""},
                                                                       {"major=1", "major=65536"},
                                                                       {"maximum_bytes=8", "maximum_bytes=0"},
                                                                       {"requirement=\"required\"", "requirement=\"optional\""},
                                                                       {"capture_phase=\"gameplay\"", "capture_phase=\"presentation\""},
                                                                       {"fields={", "fields={[999]="}}}) {
        auto malformed = source;
        malformed.replace(malformed.find(before), before.size(), after);
        const auto rejected = Compile(malformed);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().message.find("replicated.horo_script") != std::string::npos);
    }
    CHECK(LuaBehaviorProgram::Compile(source, Owner(), "missing_identity.horo_script").HasError());
    auto mismatch = source;
    mismatch.replace(mismatch.find("replication={"), 13, "replication={schema_id=43,");
    CHECK(Compile(mismatch).HasError());
}

TEST_CASE("script sidecar reload preserves identity and source diagnostics for missing files", "[gameplay][replication][lua]") {
    Directory directory;

    const auto source = directory.path / "Replicated.horo_script";
    const auto sidecar = directory.path / "Replicated.horo_script.meta";
    Write(source, Source());
    Write(
        sidecar,
        R"({"schemaVersion":1,"runtime":"lua","behaviorTypeId":"game.tests.replicated","replicationSchemaId":42,"replicationModuleId":"game.tests"})");
    auto loaded = LuaBehaviorProgram::LoadFiles(source, sidecar);
    REQUIRE(loaded.HasValue());
    auto program = std::move(loaded).Value();
    const auto previous = program->AcquireReplication().Value();
    REQUIRE(program->ReloadFiles(source, sidecar).HasValue());
    const auto valid = program->AcquireReplication().Value();
    CHECK(valid.Descriptors()->Fingerprint() == previous.Descriptors()->Fingerprint());

    Write(source, "invalid Lua source");
    auto rejected = program->ReloadFiles(source, sidecar);
    CheckRejectedReload(*program, rejected, source, valid, 2);
    std::filesystem::remove(source);
    rejected = program->ReloadFiles(source, sidecar);
    CheckRejectedReload(*program, rejected, source, valid, 2);
    Write(source, Source());
    std::filesystem::remove(sidecar);
    rejected = program->ReloadFiles(source, sidecar);
    CheckRejectedReload(*program, rejected, sidecar, valid, 2);
    Write(
        sidecar,
        R"({"schemaVersion":1,"runtime":"lua","behaviorTypeId":"game.tests.replicated","replicationSchemaId":43,"replicationModuleId":"game.tests"})");
    REQUIRE(program->ReloadFiles(source, sidecar).HasError());
    CHECK(program->AcquireReplication().Value().Descriptors() == valid.Descriptors());
    for (const std::string_view identity : {"0", "-1", "\"42\"", "42,\"replicationSchemaId\":43"}) {
        {
            std::ofstream output{sidecar};
            output << "{\"schemaVersion\":1,\"runtime\":\"lua\","
                      "\"behaviorTypeId\":\"game.tests.replicated\",\"replicationModuleId\":\"game.tests\",\"replicationSchemaId\":"
                   << identity << "}";
        }
        rejected = program->ReloadFiles(source, sidecar);
        CheckRejectedReload(*program, rejected, sidecar, valid, 2);
    }
}

TEST_CASE("script field ordering is canonical and retired identities cannot be reused", "[gameplay][replication][lua]") {
    const std::string first = R"({ id=7, value_type=1, codec=2, kind="number", introduced_major=1, introduced_minor=0,
          condition="always", requirement="required", maximum_bytes=8, maximum_elements=1 })";
    const std::string second = R"({ id=9, value_type=1, codec=2, kind="number", introduced_major=1, introduced_minor=0,
          condition="always", requirement="required", maximum_bytes=8, maximum_elements=1 })";
    auto ordered = Compile(Source(first + "," + second));
    auto reordered = Compile(Source(second + "," + first));
    REQUIRE(ordered.HasValue());
    REQUIRE(reordered.HasValue());
    CHECK(ordered.Value()->AcquireReplication().Value().Descriptors()->Fingerprint() ==
          reordered.Value()->AcquireReplication().Value().Descriptors()->Fingerprint());
    CHECK(Compile(Source(first + "," + first)).HasError());
    auto retired = Source(first);
    retired.replace(retired.find("tombstones={}"), 13, "tombstones={7}");
    CHECK(Compile(retired).HasError());

    auto foreign = LuaBehaviorProgram::Compile(Source(), Owner(), "foreign.horo_script", {}, Schema(), {.value = "game.foreign"});
    REQUIRE(foreign.HasError());
    CHECK(foreign.ErrorValue().message.find("foreign.horo_script") != std::string::npos);
}

TEST_CASE("script reload rejects major changes and existing codec reinterpretation", "[gameplay][replication][lua]") {
    auto program = Compile(Source()).Value();
    const auto previous = program->AcquireReplication().Value();
    auto major = Source();
    major.replace(major.find("major=1"), 7, "major=2");
    major.replace(major.find("introduced_major=1"), 18, "introduced_major=2");
    auto candidate = Compile(major);
    REQUIRE(candidate.HasValue());
    CHECK(program->ReplaceCompatible(std::move(candidate).Value()).HasError());
    auto changedKind = Source();
    changedKind.replace(changedKind.find("kind=\"number\""), 13, "kind=\"integer\"");
    candidate = Compile(changedKind);
    REQUIRE(candidate.HasValue());
    CHECK(program->ReplaceCompatible(std::move(candidate).Value()).HasError());
    CHECK(program->AcquireReplication().Value().Descriptors() == previous.Descriptors());
    CHECK(program->Revision() == 1);
}

TEST_CASE("bounded Lua file inputs fail before replacing a published generation", "[gameplay][replication][lua]") {
    Directory directory;
    const auto source = directory.path / "Replicated.horo_script";
    const auto sidecar = directory.path / "Replicated.horo_script.meta";
    auto compiled = Compile(Source());
    REQUIRE(compiled.HasValue());
    auto program = std::move(compiled).Value();
    const auto previous = program->AcquireReplication().Value();
    Write(
        sidecar,
        R"({"schemaVersion":1,"runtime":"lua","behaviorTypeId":"game.tests.replicated","replicationSchemaId":42,"replicationModuleId":"game.tests"})");
    for (const std::string input : {std::string{}, std::string(2U * 1024U * 1024U + 1U, 'x')}) {
        Write(source, input);
        auto rejected = program->ReloadFiles(source, sidecar);
        CheckRejectedReload(*program, rejected, source, previous, 1);
    }
    Write(source, Source());
    Write(sidecar, std::string(64U * 1024U + 1U, 'x'));
    auto rejected = program->ReloadFiles(source, sidecar);
    CheckRejectedReload(*program, rejected, sidecar, previous, 1);
}

TEST_CASE("declaration compilation reads inert behavior metadata without author metamethods", "[gameplay][replication][lua]") {
    auto source = Source();
    const std::string prefix = "return horo.behavior {";
    source.replace(source.find(prefix), prefix.size(), "return setmetatable({");
    source += R"(, { __index=function() error("metadata reflection executed") end }) )";
    auto compiled = Compile(source);
    REQUIRE(compiled.HasValue());
    CHECK(compiled.Value()->Descriptor().fields.empty());
    CHECK(compiled.Value()->AcquireReplication().HasValue());

    const auto position = source.find("display_name=");
    source.insert(position, R"(fields=setmetatable({{name="speed",default=1}}, {
        __len=function() error("metadata length reflection executed") end }), )");
    compiled = Compile(source);
    REQUIRE(compiled.HasValue());
    REQUIRE(compiled.Value()->Descriptor().fields.size() == 1);
    CHECK(compiled.Value()->Descriptor().fields.front().name == "speed");
    const std::string display = "display_name=\"Replicated\"";
    source.replace(source.find(display), display.size(), "display_name=123");
    compiled = Compile(source);
    REQUIRE(compiled.HasValue());
    CHECK(compiled.Value()->Descriptor().displayName == "123");
}

TEST_CASE("script source sidecar and display renames retain canonical replication identity", "[gameplay][replication][lua]") {
    Directory directory;
    const auto source = directory.path / "Replicated.horo_script";
    const auto sidecar = directory.path / "Replicated.horo_script.meta";
    Write(source, Source());
    Write(
        sidecar,
        R"({"schemaVersion":1,"runtime":"lua","behaviorTypeId":"game.tests.replicated","replicationSchemaId":42,"replicationModuleId":"game.tests"})");
    auto loaded = LuaBehaviorProgram::LoadFiles(source, sidecar);
    REQUIRE(loaded.HasValue());
    auto program = std::move(loaded).Value();
    const auto prior = program->AcquireReplication().Value();
    const auto renamedSource = directory.path / "Renamed behavior.horo_script";
    const auto renamedSidecar = directory.path / "Renamed behavior.horo_script.meta";
    std::filesystem::rename(source, renamedSource);
    std::filesystem::rename(sidecar, renamedSidecar);
    auto renamed = Source();
    renamed.replace(renamed.find("display_name=\"Replicated\""), std::string_view{"display_name=\"Replicated\""}.size(),
                    "display_name=\"Renamed presentation\"");
    Write(renamedSource, renamed);
    REQUIRE(program->ReloadFiles(renamedSource, renamedSidecar).HasValue());
    const auto current = program->AcquireReplication().Value();
    CHECK(program->Registration().descriptor.displayName == "Renamed presentation");
    CHECK(current.Descriptors()->Fingerprint() == prior.Descriptors()->Fingerprint());
    CHECK(current.Registrations().front().owner == prior.Registrations().front().owner);
    CHECK(current.Serializers().Encode(Schema(), FieldId::Create(7).Value(), 4.5).Value() ==
          prior.Serializers().Encode(Schema(), FieldId::Create(7).Value(), 4.5).Value());
    std::filesystem::remove(renamedSource);
    CheckRejectedReload(*program, program->ReloadFiles(renamedSource, renamedSidecar), renamedSource, current, 2);
}
