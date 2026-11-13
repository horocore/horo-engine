#include "AllocationProbe.h"
#include "Horo/Gameplay/LuaPerceptionSense.h"
#include "PerceptionSenseTestSupport.h"

namespace Horo::AI {
    namespace {
        using TestSupport::ExpectError;
        using TestSupport::MakeIdentity;

        /** @brief Unit-only native admission fence; verified package/native image ownership is tested separately. */
        struct Fixture final : PerceptionSenseTestSupport::InputFixture {
            std::shared_ptr<std::atomic_bool> admission{std::make_shared<std::atomic_bool>(true)};
            std::shared_ptr<const PerceptionSenseGeneration> parent;
            PerceptionSenseContract contract;

            Fixture() {
                auto captured = PerceptionSenseGeneration::Capture({}, admission);
                REQUIRE(captured.HasValue());
                parent = captured.Value();
                const PerceptionDescriptorOrigin origin{PerceptionDescriptorSourceKind::Script, MakeIdentity<PerceptionProviderId>(22), 1};
                contract = {{MakeIdentity<SenseTypeId>(20), origin, "script sense", capabilities},
                            {MakeIdentity<StimulusTypeId>(21), origin, "script sample", 1, {}},
                            0,
                            4,
                            2};
            }

            std::unique_ptr<Gameplay::LuaPerceptionSenseProgram> Program(std::string source) const {
                auto compiled = Gameplay::LuaPerceptionSenseProgram::Compile(std::move(source), contract, parent);
                REQUIRE(compiled.HasValue());
                return std::move(compiled).Value();
            }

            std::shared_ptr<const PerceptionSenseGeneration> Generation(const Gameplay::LuaPerceptionSenseProgram &program) const {
                auto acquired = program.Acquire();
                REQUIRE(acquired.HasValue());
                return acquired.Value();
            }
        };

        TEST_CASE("Lua custom senses evaluate exact scalar candidate facts without mutation capabilities", "[unit][ai][sense-lua]") {
            Fixture fixture;
            auto program = fixture.Program(R"(
                assert(io == nil and os == nil and package == nil and debug == nil)
                assert(print == nil and warn == nil and load == nil)
                assert(string == nil and table == nil and utf8 == nil and horo == nil)
                assert(pairs == nil and pcall == nil and xpcall == nil and getmetatable == nil)
                assert(math.random == nil and math.randomseed == nil)
                return function(x,y,z,lx,ly,lz) return x-lx < 500 and y == ly and z == lz end
            )");
            auto generation = fixture.Generation(*program);
            CHECK(generation->Contracts()[0].sense.origin.kind == PerceptionDescriptorSourceKind::Script);
            auto instance = fixture.Instance(generation);
            AiWorkBudget budget{6};
            const auto result = instance->Evaluate(fixture.Input(), budget, budget);
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().count == 2);
            CHECK(result.Value().samples[0].sensed);
            CHECK_FALSE(result.Value().samples[1].sensed);
            CHECK(budget.Remaining() == 0);
        }

        TEST_CASE("Lua custom sense nil outcome omits an input without inventing a sensing loss", "[unit][ai][sense-lua]") {
            Fixture fixture;
            auto program = fixture.Program("return function(x) if x < 1500 then return true end end");
            auto instance = fixture.Instance(fixture.Generation(*program));
            AiWorkBudget budget{6};
            const auto result = instance->Evaluate(fixture.Input(), budget, budget);
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().count == 1);
            CHECK(result.Value().samples[0].candidate == 0);
        }

        TEST_CASE("Lua custom sense VM allocation instruction and output violations quarantine only their instance",
                  "[unit][ai][sense-lua]") {
            Fixture fixture;
            std::string source;
            SECTION("allocation") {
                source = "return function() local allocated = {} return allocated ~= nil end";
            }
            SECTION("instruction limit") {
                source = "return function() while true do end end";
            }
            SECTION("invalid scalar output") {
                source = "return function() return 7 end";
            }
            SECTION("string-metatable native callback") {
                source = "return function() return ('aaa'):match('a+') ~= nil end";
            }
            auto program = fixture.Program(source);
            auto instance = fixture.Instance(fixture.Generation(*program));
            AiWorkBudget budget{6};
            ExpectError(instance->Evaluate(fixture.Input(), budget, budget), AIErrors::PerceptionProviderFailed);
            AiWorkBudget retry{6};
            ExpectError(instance->Evaluate(fixture.Input(2), retry, retry), AIErrors::RuntimeUnavailable);
            auto healthy = fixture.Program("return function() return true end");
            auto independent = fixture.Instance(fixture.Generation(*healthy));
            AiWorkBudget independentBudget{6};
            CHECK(independent->Evaluate(fixture.Input(), independentBudget, independentBudget).HasValue());
        }

        TEST_CASE("Lua custom senses inherit native parent and program retirement fences", "[unit][ai][sense-lua]") {
            Fixture fixture;
            auto program = fixture.Program("return function() return true end");
            auto generation = fixture.Generation(*program);
            auto instance = fixture.Instance(generation);
            SECTION("native parent retired") {
                fixture.admission->store(false, std::memory_order_release);
            }
            SECTION("program retired") {
                program->Retire();
            }
            CHECK_FALSE(generation->CanAdmit());
            ExpectError(program->Acquire(), AIErrors::RuntimeUnavailable);
            AiWorkBudget budget{6};
            ExpectError(instance->Evaluate(fixture.Input(), budget, budget), AIErrors::RuntimeUnavailable);
            CHECK(budget.Remaining() == 6);
        }

        TEST_CASE("Lua custom senses discard an earlier candidate when a later predicate fails", "[unit][ai][sense-lua]") {
            Fixture fixture;
            auto program = fixture.Program(R"(
                return function(x)
                    if x < 1500 then return true end
                    local allocated = {}
                    return allocated ~= nil
                end
            )");
            auto instance = fixture.Instance(fixture.Generation(*program));
            AiWorkBudget budget{6};
            ExpectError(instance->Evaluate(fixture.Input(), budget, budget), AIErrors::PerceptionProviderFailed);
            AiWorkBudget retry{6};
            ExpectError(instance->Evaluate(fixture.Input(2), retry, retry), AIErrors::RuntimeUnavailable);
        }

        TEST_CASE("Lua custom sense compilation rejects absent authority malformed source and excessive policy", "[unit][ai][sense-lua]") {
            Fixture fixture;
            ExpectError(Gameplay::LuaPerceptionSenseProgram::Compile("return function() return true end", fixture.contract, {}),
                        AIErrors::RuntimeUnavailable);
            for (const std::string source : {"return {}", "while true do end", "not valid lua"})
                CHECK(Gameplay::LuaPerceptionSenseProgram::Compile(source, fixture.contract, fixture.parent).HasError());
            CHECK(Gameplay::LuaPerceptionSenseProgram::Compile("return function() return true end", fixture.contract, fixture.parent,
                                                               {4U * 1024U * 1024U, 4096})
                      .HasError());
        }

        TEST_CASE("Lua custom sense compilation reports storage failure without allocating its diagnostic", "[unit][ai][sense-lua]") {
            Fixture fixture;
            std::string source{"return function() return true end"};
            auto contract = fixture.contract;
            Tests::AllocationProbe::Measurement observed;
            auto failed = [&] {
                Tests::AllocationProbe::ScopedMeasurement measurement;
                Tests::AllocationProbe::ScopedFailure allocation;
                auto result = Gameplay::LuaPerceptionSenseProgram::Compile(std::move(source), std::move(contract), fixture.parent);
                observed = measurement.Snapshot();
                return result;
            }();
            ExpectError(failed, AIErrors::PerceptionRegistryStorageUnavailable);
            CHECK(failed.ErrorValue().message.empty());
            CHECK(observed.requests == 1);
            CHECK(fixture.parent->CanAdmit());
            auto healthy = fixture.Program("return function() return true end");
            CHECK(healthy->Acquire().HasValue());
        }

        TEST_CASE("Lua custom sense acquisition contains metadata allocation failure and preserves its program", "[unit][ai][sense-lua]") {
            Fixture fixture;
            fixture.contract.sense.displayName = "A custom script sense name requiring owned metadata storage";
            auto program = fixture.Program("return function() return true end");
            Tests::AllocationProbe::Measurement observed;
            auto failed = [&] {
                Tests::AllocationProbe::ScopedMeasurement measurement;
                Tests::AllocationProbe::ScopedFailure allocation;
                auto result = program->Acquire();
                observed = measurement.Snapshot();
                return result;
            }();
            ExpectError(failed, AIErrors::PerceptionRegistryStorageUnavailable);
            CHECK(failed.ErrorValue().message.empty());
            CHECK(observed.requests == 1);
            CHECK(fixture.parent->CanAdmit());
            auto instance = fixture.Instance(fixture.Generation(*program));
            AiWorkBudget budget{6};
            CHECK(instance->Evaluate(fixture.Input(), budget, budget).HasValue());
        }
    }  // namespace
}  // namespace Horo::AI
