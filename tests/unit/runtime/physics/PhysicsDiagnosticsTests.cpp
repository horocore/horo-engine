#include "Horo/Physics/PhysicsDiagnostics.h"
#include "Horo/Physics/PhysicsErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <string>

namespace Horo::Physics {
    namespace {
        /** @brief Creates one valid world identity for diagnostic fixtures. */
        PhysicsWorldId DiagnosticWorld(const std::uint64_t value = 17) {
            const auto world = PhysicsWorldId::Create(value);
            REQUIRE(world.HasValue());
            return world.Value();
        }

        /** @brief Creates one non-zero persistent asset identity without parsing text. */
        Assets::AssetId DiagnosticAsset() {
            auto bytes = std::array<std::uint8_t, 16>{};
            bytes.back() = 1;
            return Assets::AssetId::FromBytes(bytes);
        }

        /** @brief Requires stable descriptor failure identity. */
        void RequireDiagnosticFailure(const Result<PhysicsDiagnosticRecord> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == expected.code.Value());
        }

        TEST_CASE("Physics diagnostic categories expose stable derived presentation names", "[physics][diagnostics]") {
            const std::array cases{
                std::pair{PhysicsDiagnosticCategory::Configuration, std::string_view{"physics.configuration"}},
                std::pair{PhysicsDiagnosticCategory::Cook, std::string_view{"physics.cook"}},
                std::pair{PhysicsDiagnosticCategory::Runtime, std::string_view{"physics.runtime"}},
                std::pair{PhysicsDiagnosticCategory::Query, std::string_view{"physics.query"}},
                std::pair{PhysicsDiagnosticCategory::Event, std::string_view{"physics.event"}},
                std::pair{PhysicsDiagnosticCategory::Lifecycle, std::string_view{"physics.lifecycle"}},
            };
            for (const auto &[category, name] : cases)
                REQUIRE(PhysicsDiagnosticCategoryName(category) == name);
            REQUIRE(PhysicsDiagnosticCategoryName(static_cast<PhysicsDiagnosticCategory>(255)).empty());
        }

        TEST_CASE("Physics diagnostic records accept canonical mapping boundaries without accepting invented codes",
                  "[physics][diagnostics]") {
            const auto first = MakePhysicsDiagnosticRecord(PhysicsDiagnosticCategory::Runtime, MakeError(PhysicsErrors::WorldInvalid));
            REQUIRE(first.HasValue());
            REQUIRE(first.Value().code.Value() == PhysicsErrors::WorldInvalid.code.Value());
            const auto last =
                MakePhysicsDiagnosticRecord(PhysicsDiagnosticCategory::Lifecycle, MakeError(PhysicsErrors::SolverFatalCondition));
            REQUIRE(last.HasValue());
            REQUIRE(last.Value().code.Value() == PhysicsErrors::SolverFatalCondition.code.Value());

            auto invented = MakeError(PhysicsErrors::DescriptorInvalid);
            invented.code = ErrorCode{"physics.future.invented"};
            RequireDiagnosticFailure(MakePhysicsDiagnosticRecord(PhysicsDiagnosticCategory::Runtime, invented),
                                     PhysicsErrors::DescriptorInvalid);
            invented.domain = ErrorDomainId{"horo.asset"};
            RequireDiagnosticFailure(MakePhysicsDiagnosticRecord(PhysicsDiagnosticCategory::Runtime, invented),
                                     PhysicsErrors::DescriptorInvalid);
        }

        TEST_CASE("Physics diagnostic severity mapping is complete and preserves fatal evidence", "[physics][diagnostics]") {
            auto error = MakeError(PhysicsErrors::DescriptorInvalid);
            const std::array cases{
                std::pair{ErrorSeverity::Info, DiagnosticSeverity::Note},
                std::pair{ErrorSeverity::Warning, DiagnosticSeverity::Warning},
                std::pair{ErrorSeverity::Error, DiagnosticSeverity::Error},
                std::pair{ErrorSeverity::Critical, DiagnosticSeverity::Fatal},
            };
            for (const auto &[source, expected] : cases) {
                error.severity = source;
                const auto record = MakePhysicsDiagnosticRecord(PhysicsDiagnosticCategory::Runtime, error);
                REQUIRE(record.HasValue());
                REQUIRE(record.Value().severity == expected);
            }
            error.severity = static_cast<ErrorSeverity>(255);
            RequireDiagnosticFailure(MakePhysicsDiagnosticRecord(PhysicsDiagnosticCategory::Runtime, error),
                                     PhysicsErrors::DescriptorInvalid);
        }

        TEST_CASE("Physics diagnostic record owns ordered typed context and leaves sources unchanged", "[physics][diagnostics]") {
            const auto world = DiagnosticWorld();
            const BodyHandle body{world, {2, 3}};
            const ShapeHandle shape{world, {4, 5}};
            const ConstraintHandle constraint{world, {6, 7}};
            const auto asset = DiagnosticAsset();
            const std::array context{
                PhysicsDiagnosticContextEntry{PhysicsDiagnosticContextKey::World, world},
                PhysicsDiagnosticContextEntry{PhysicsDiagnosticContextKey::Body, body},
                PhysicsDiagnosticContextEntry{PhysicsDiagnosticContextKey::Shape, shape},
                PhysicsDiagnosticContextEntry{PhysicsDiagnosticContextKey::Constraint, constraint},
                PhysicsDiagnosticContextEntry{PhysicsDiagnosticContextKey::Asset, asset},
                PhysicsDiagnosticContextEntry{PhysicsDiagnosticContextKey::SceneGeneration, std::uint64_t{9}},
                PhysicsDiagnosticContextEntry{PhysicsDiagnosticContextKey::SimulationTick, std::uint64_t{10}},
                PhysicsDiagnosticContextEntry{PhysicsDiagnosticContextKey::Capacity, std::uint64_t{0}},
            };
            auto error = MakeError(PhysicsErrors::CapacityExceeded, "Bounded event capacity was exhausted.");
            const auto record = MakePhysicsDiagnosticRecord(PhysicsDiagnosticCategory::Event, error, context);
            REQUIRE(record.HasValue());
            REQUIRE(record.Value().schemaVersion == 1);
            REQUIRE(record.Value().category == PhysicsDiagnosticCategory::Event);
            REQUIRE(record.Value().contextCount == MaximumPhysicsDiagnosticContextEntries);
            REQUIRE(std::get<BodyHandle>(record.Value().context[1].value) == body);
            REQUIRE(std::get<Assets::AssetId>(record.Value().context[4].value) == asset);
            REQUIRE(record.Value().message == error.message);
            error.message[0] = 'X';
            REQUIRE(record.Value().message == "Bounded event capacity was exhausted.");
            REQUIRE(std::get<PhysicsWorldId>(context.front().value) == world);
        }

        TEST_CASE("Physics diagnostic construction rejects context overflow ordering and type mismatch transactionally",
                  "[physics][diagnostics]") {
            const auto error = MakeError(PhysicsErrors::DescriptorInvalid);
            std::array<PhysicsDiagnosticContextEntry, MaximumPhysicsDiagnosticContextEntries + 1> oversized{};
            RequireDiagnosticFailure(MakePhysicsDiagnosticRecord(PhysicsDiagnosticCategory::Configuration, error, oversized),
                                     PhysicsErrors::DescriptorInvalid);

            const std::array duplicate{
                PhysicsDiagnosticContextEntry{PhysicsDiagnosticContextKey::RequestedCount, std::uint64_t{1}},
                PhysicsDiagnosticContextEntry{PhysicsDiagnosticContextKey::RequestedCount, std::uint64_t{2}},
            };
            RequireDiagnosticFailure(MakePhysicsDiagnosticRecord(PhysicsDiagnosticCategory::Configuration, error, duplicate),
                                     PhysicsErrors::DescriptorInvalid);
            const std::array unordered{
                PhysicsDiagnosticContextEntry{PhysicsDiagnosticContextKey::Capacity, std::uint64_t{2}},
                PhysicsDiagnosticContextEntry{PhysicsDiagnosticContextKey::RequestedCount, std::uint64_t{1}},
            };
            RequireDiagnosticFailure(MakePhysicsDiagnosticRecord(PhysicsDiagnosticCategory::Configuration, error, unordered),
                                     PhysicsErrors::DescriptorInvalid);
            const std::array wrongType{
                PhysicsDiagnosticContextEntry{PhysicsDiagnosticContextKey::World, std::uint64_t{17}},
            };
            RequireDiagnosticFailure(MakePhysicsDiagnosticRecord(PhysicsDiagnosticCategory::Runtime, error, wrongType),
                                     PhysicsErrors::DescriptorInvalid);
            const std::array unknownKey{
                PhysicsDiagnosticContextEntry{static_cast<PhysicsDiagnosticContextKey>(255), std::uint64_t{17}},
            };
            RequireDiagnosticFailure(MakePhysicsDiagnosticRecord(PhysicsDiagnosticCategory::Runtime, error, unknownKey),
                                     PhysicsErrors::DescriptorInvalid);
        }

        TEST_CASE("Physics diagnostic construction rejects invalid and incoherent identity context", "[physics][diagnostics]") {
            const auto error = MakeError(PhysicsErrors::HandleStale);
            const auto world = DiagnosticWorld();
            const std::array malformed{
                PhysicsDiagnosticContextEntry{PhysicsDiagnosticContextKey::Body, BodyHandle{}},
            };
            RequireDiagnosticFailure(MakePhysicsDiagnosticRecord(PhysicsDiagnosticCategory::Runtime, error, malformed),
                                     PhysicsErrors::DescriptorInvalid);
            const std::array foreign{
                PhysicsDiagnosticContextEntry{PhysicsDiagnosticContextKey::World, world},
                PhysicsDiagnosticContextEntry{PhysicsDiagnosticContextKey::Shape, ShapeHandle{DiagnosticWorld(18), {1, 1}}},
            };
            RequireDiagnosticFailure(MakePhysicsDiagnosticRecord(PhysicsDiagnosticCategory::Runtime, error, foreign),
                                     PhysicsErrors::DescriptorInvalid);
            const std::array missingGeneration{
                PhysicsDiagnosticContextEntry{PhysicsDiagnosticContextKey::SimulationTick, std::uint64_t{0}},
            };
            RequireDiagnosticFailure(MakePhysicsDiagnosticRecord(PhysicsDiagnosticCategory::Event, error, missingGeneration),
                                     PhysicsErrors::DescriptorInvalid);
            const std::array invalidAsset{
                PhysicsDiagnosticContextEntry{PhysicsDiagnosticContextKey::Asset, Assets::AssetId{}},
            };
            RequireDiagnosticFailure(MakePhysicsDiagnosticRecord(PhysicsDiagnosticCategory::Cook, error, invalidAsset),
                                     PhysicsErrors::DescriptorInvalid);
        }

        TEST_CASE("Physics diagnostic messages enforce exact bounds without truncation or source mutation", "[physics][diagnostics]") {
            auto error = MakeError(PhysicsErrors::DescriptorInvalid);
            error.message.assign(MaximumPhysicsDiagnosticMessageBytes, 'a');
            const auto boundary = MakePhysicsDiagnosticRecord(PhysicsDiagnosticCategory::Configuration, error);
            REQUIRE(boundary.HasValue());
            REQUIRE(boundary.Value().message.size() == MaximumPhysicsDiagnosticMessageBytes);
            error.message.push_back('b');
            const auto original = error.message;
            RequireDiagnosticFailure(MakePhysicsDiagnosticRecord(PhysicsDiagnosticCategory::Configuration, error),
                                     PhysicsErrors::DescriptorInvalid);
            REQUIRE(error.message == original);
            error.message.clear();
            RequireDiagnosticFailure(MakePhysicsDiagnosticRecord(PhysicsDiagnosticCategory::Configuration, error),
                                     PhysicsErrors::DescriptorInvalid);
            RequireDiagnosticFailure(MakePhysicsDiagnosticRecord(static_cast<PhysicsDiagnosticCategory>(255),
                                                                 MakeError(PhysicsErrors::DescriptorInvalid)),
                                     PhysicsErrors::OperationUnsupported);
        }
    }  // namespace
}  // namespace Horo::Physics
