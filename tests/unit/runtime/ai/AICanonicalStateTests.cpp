#include "AiTestSupport.h"
#include "Horo/AI/AICanonicalState.h"
#include "Horo/AI/BlackboardInstance.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <memory>

namespace Horo::AI {
    namespace {
        using TestSupport::ExpectError;
        using TestSupport::MakeIdentity;

        [[nodiscard]] BlackboardKeyDescriptor Key(const std::uint64_t id, const bool optional = false) {
            return {.key = MakeIdentity<BlackboardKeyId>(id),
                    .kind = BlackboardValueKind::Boolean,
                    .cardinality = BlackboardValueCardinality::Scalar,
                    .maximumCollectionElements = 1,
                    .presence = optional ? BlackboardKeyPresence::Optional : BlackboardKeyPresence::Required,
                    .access = BlackboardKeyAccess::ReadOnly,
                    .defaultValue = optional ? std::nullopt : std::optional<BlackboardValue>{BlackboardScalarValue{false}}};
        }

        [[nodiscard]] std::shared_ptr<const BlackboardSchema> Schema(const std::uint32_t version,
                                                                     const std::span<const BlackboardKeyDescriptor> keys) {
            auto schema = BlackboardSchema::Capture({.identity = MakeIdentity<BlackboardSchemaId>(10),
                                                     .version = version,
                                                     .unknownValuePolicy = BlackboardUnknownValuePolicy::Reject,
                                                     .keys = keys});
            REQUIRE(schema.HasValue());
            return std::make_shared<const BlackboardSchema>(std::move(schema).Value());
        }

        [[nodiscard]] BlackboardInstanceBinding Binding(const BlackboardSchema &schema) {
            return {.agent = {AiRuntimeIncarnation::Create(7).Value(), {0, 1}},
                    .schema = schema.Identity(),
                    .schemaVersion = schema.Version(),
                    .schemaGeneration = 1,
                    .instanceGeneration = 1};
        }

        [[nodiscard]] BlackboardCanonicalState Defaults(const std::shared_ptr<const BlackboardSchema> &schema) {
            auto instance = BlackboardInstance::Create(Binding(*schema), schema);
            REQUIRE(instance.HasValue());
            auto captured = instance.Value()->CaptureCanonical();
            REQUIRE(captured.HasValue());
            return std::move(captured).Value();
        }

        TEST_CASE("Canonical blackboard restores read-only values and explicitly absent optional keys", "[unit][ai][canonical]") {
            const auto schema = Schema(1, std::array{Key(1), Key(2, true)});
            auto state = Defaults(schema);
            state.entries.front().value = BlackboardValue{BlackboardScalarValue{true}};
            auto restored = BlackboardInstance::CreateFromCanonical(Binding(*schema), schema, state);
            REQUIRE(restored.HasValue());
            CHECK(restored.Value()->CaptureCanonical().Value() == state);
            const auto snapshot = restored.Value()->Snapshot();
            REQUIRE(snapshot.HasValue());
            CHECK(snapshot.Value().Read(MakeIdentity<BlackboardKeyId>(1)).Value() == state.entries.front().value);
            CHECK_FALSE(snapshot.Value().Read(MakeIdentity<BlackboardKeyId>(2)).Value().has_value());
            REQUIRE(restored.Value()->TeardownAtBlackboardSync().HasValue());
            CHECK(state.entries.front().value.has_value());
            ExpectError(restored.Value()->CaptureCanonical(), AIErrors::BlackboardInstanceStale);
        }

        TEST_CASE("Canonical blackboard requires exact source schema shape and valid typed values", "[unit][ai][canonical]") {
            const auto schema = Schema(1, std::array{Key(1), Key(2, true)});
            auto state = Defaults(schema);
            SECTION("missing key") {
                state.entries.pop_back();
            }
            SECTION("unknown key") {
                state.entries.back().key = MakeIdentity<BlackboardKeyId>(99);
            }
            SECTION("duplicate key") {
                state.entries.back().key = state.entries.front().key;
            }
            SECTION("unordered keys") {
                std::swap(state.entries.front(), state.entries.back());
            }
            SECTION("missing required value") {
                state.entries.front().value.reset();
            }
            ExpectError(ValidateCanonicalBlackboard(state, *schema), AIErrors::CanonicalStateInvalid);
        }

        TEST_CASE("Canonical blackboard rejects schema skew and malformed values before allocation", "[unit][ai][canonical]") {
            const auto schema = Schema(1, std::array{Key(1)});
            auto state = Defaults(schema);
            SECTION("unsupported source") {
                ++state.schemaVersion;
                ExpectError(MigrateCanonicalBlackboard(state, *schema, {}), AIErrors::CanonicalSchemaUnsupported);
            }
            SECTION("wrong scalar type") {
                state.entries.front().value = BlackboardValue{BlackboardScalarValue{std::int64_t{7}}};
                ExpectError(BlackboardInstance::CreateFromCanonical(Binding(*schema), schema, state),
                            AIErrors::BlackboardValueTypeMismatch);
            }
            SECTION("invalid destination fence") {
                auto binding = Binding(*schema);
                binding.schemaGeneration = 0;
                ExpectError(BlackboardInstance::CreateFromCanonical(binding, schema, state), AIErrors::BlackboardInstanceInvalid);
            }
            SECTION("null schema") {
                ExpectError(BlackboardInstance::CreateFromCanonical(Binding(*schema), nullptr, state), AIErrors::BlackboardInstanceInvalid);
            }
            SECTION("canonical entries ceiling") {
                state.entries.resize(MaximumBlackboardKeys + 1);
                ExpectError(MigrateCanonicalBlackboard(state, *schema, {}), AIErrors::CanonicalStateInvalid);
            }
            SECTION("migration catalog ceiling") {
                const std::vector<BlackboardSchemaMigration> migrations(MaximumAiSchemaMigrations + 1);
                ExpectError(MigrateCanonicalBlackboard(state, *schema, migrations), AIErrors::CanonicalStateInvalid);
            }
        }

        TEST_CASE("Explicit schema migration renames removes and adds default keys transactionally", "[unit][ai][canonical][migration]") {
            const auto source = Schema(1, std::array{Key(1), Key(2, true)});
            const auto destination = Schema(2, std::array{Key(3), Key(4)});
            auto state = Defaults(source);
            state.entries.front().value = BlackboardValue{BlackboardScalarValue{true}};
            const auto mappings = std::array{BlackboardKeyMigration{MakeIdentity<BlackboardKeyId>(1), MakeIdentity<BlackboardKeyId>(3)},
                                             BlackboardKeyMigration{MakeIdentity<BlackboardKeyId>(2), {}}};
            const BlackboardSchemaMigration migration{source, destination->Identity(), 2, mappings};
            auto migrated = MigrateCanonicalBlackboard(state, *destination, std::array{migration});
            REQUIRE(migrated.HasValue());
            CHECK(migrated.Value().entries.front().value == state.entries.front().value);
            CHECK(migrated.Value().entries.back().value == destination->Keys().back().defaultValue);
            CHECK(migrated.Value().schemaVersion == 2);
            CHECK(state.schemaVersion == 1);
            CHECK(state.entries.front().key == MakeIdentity<BlackboardKeyId>(1));
            CHECK(ValidateCanonicalBlackboard(migrated.Value(), *destination).HasValue());
        }

        TEST_CASE("Schema migration rejects loss ambiguity collisions and undeclared mappings", "[unit][ai][canonical][migration]") {
            const auto source = Schema(1, std::array{Key(1), Key(2, true)});
            auto targetKey = Key(1);
            SECTION("same key type changes") {
                targetKey.kind = BlackboardValueKind::SignedInteger;
                targetKey.defaultValue = BlackboardValue{BlackboardScalarValue{std::int64_t{0}}};
            }
            const auto destination = Schema(2, std::array{targetKey, Key(3)});
            const auto state = Defaults(source);
            auto original = state;
            std::vector<BlackboardKeyMigration> mappings{{MakeIdentity<BlackboardKeyId>(2), {}}};
            SECTION("implicit key removal") {
                mappings.clear();
            }
            SECTION("two sources collide") {
                mappings.front().destination = MakeIdentity<BlackboardKeyId>(1);
            }
            SECTION("mapping source missing") {
                mappings.front().source = MakeIdentity<BlackboardKeyId>(99);
            }
            SECTION("mapping target missing") {
                mappings.front().destination = MakeIdentity<BlackboardKeyId>(99);
            }
            SECTION("duplicate source mapping") {
                mappings.push_back(mappings.front());
            }
            SECTION("over-limit mapping") {
                mappings.resize(MaximumBlackboardKeys + 1);
            }
            const BlackboardSchemaMigration migration{source, destination->Identity(), 2, mappings};
            const auto failed = MigrateCanonicalBlackboard(state, *destination, std::array{migration});
            REQUIRE(failed.HasError());
            CHECK((failed.ErrorValue().code.Value() == AIErrors::CanonicalMigrationFailed.code.Value() ||
                   failed.ErrorValue().code.Value() == AIErrors::CanonicalStateInvalid.code.Value()));
            CHECK(state == original);
        }

        TEST_CASE("Forward migration requires explicit compatible source and complete target presence",
                  "[unit][ai][canonical][migration]") {
            const auto source = Schema(1, std::array{Key(1)});
            auto missingDefault = Key(2);
            missingDefault.defaultValue.reset();
            const auto destination = Schema(2, std::array{Key(1), missingDefault});
            const auto state = Defaults(source);
            const BlackboardSchemaMigration migration{source, destination->Identity(), 2, {}};
            const auto failed = MigrateCanonicalBlackboard(state, *destination, std::array{migration});
            ExpectError(failed, AIErrors::CanonicalMigrationFailed);
            REQUIRE(failed.ErrorValue().cause.Get() != nullptr);
            CHECK(failed.ErrorValue().cause.Get()->code.Value() == AIErrors::CanonicalStateInvalid.code.Value());
            const auto newer = Schema(3, std::array{Key(1)});
            const BlackboardSchemaMigration downgrade{newer, destination->Identity(), 2, {}};
            ExpectError(MigrateCanonicalBlackboard(Defaults(newer), *destination, std::array{downgrade}),
                        AIErrors::CanonicalSchemaUnsupported);
            ExpectError(MigrateCanonicalBlackboard(state, *destination, std::array{migration, migration}),
                        AIErrors::CanonicalSchemaUnsupported);
        }
    }  // namespace
}  // namespace Horo::AI
