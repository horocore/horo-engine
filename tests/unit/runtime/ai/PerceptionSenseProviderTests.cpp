#include "AllocationProbe.h"
#include "Horo/AI/PerceptionSenseProvider.h"
#include "PerceptionSenseTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <stdexcept>
#include <thread>

namespace Horo::AI {
    namespace {
        using TestSupport::ExpectError;
        using TestSupport::MakeIdentity;

        enum class Fault {
            None,
            Throw,
            Typed,
            Duplicate,
            OutOfRange,
            OutOfOrder,
            Overrun
        };

        /** @brief Actual callback-state owner; its admission alias must survive every provider destructor. */
        struct ProviderOwner final {
            std::array<std::size_t, 4> &counters;
            std::atomic_bool admission{true};
            Fault fault{};
            std::function<void()> onCreate;
            std::function<void()> onEvaluate;

            explicit ProviderOwner(std::array<std::size_t, 4> &observed) : counters(observed) {}

            ~ProviderOwner() {
                ++counters[3];
            }
        };

        /** @brief Bounded native test sense producing binary outcomes solely from frozen candidate positions. */
        class SampleSense final : public IPerceptionSenseProvider {
        public:
            explicit SampleSense(ProviderOwner &owner) : owner_(owner) {}

            ~SampleSense() override {
                ++owner_.counters[2];
            }

            Result<std::size_t> Evaluate(const PerceptionSenseInput &input, const std::span<PerceptionSenseSample> output,
                                         AiWorkBudget &budget) override {
                ++owner_.counters[1];
                if (owner_.onEvaluate)
                    owner_.onEvaluate();
                if (owner_.fault == Fault::Throw)
                    throw std::runtime_error("custom sense failed");
                if (owner_.fault == Fault::Typed)
                    return Result<std::size_t>::Failure(MakeError(AIErrors::PerceptionCapabilityUnavailable));
                for (std::size_t index = 0; index < input.candidates.size(); ++index) {
                    if (!budget.Consume())
                        return Result<std::size_t>::Failure(MakeError(AIErrors::SchedulerBudgetExhausted));
                    output[index] = {static_cast<std::uint32_t>(index), input.candidates[index].position.Millimeters()[0] < 1500};
                }
                if (owner_.fault == Fault::Duplicate)
                    output[1] = output[0];
                if (owner_.fault == Fault::OutOfRange)
                    output[0].candidate = 128;
                if (owner_.fault == Fault::OutOfOrder)
                    std::swap(output[0], output[1]);
                if (owner_.fault == Fault::Overrun)
                    static_cast<void>(budget.Consume(4096));
                return Result<std::size_t>::Success(input.candidates.size());
            }

        private:
            ProviderOwner &owner_;
        };

        /** @brief Module-owned factory callback whose state is protected by the generation's actual owner alias. */
        IPerceptionSenseProvider *Create(void *raw) {
            auto &owner = *static_cast<ProviderOwner *>(raw);
            ++owner.counters[0];
            if (owner.onCreate)
                owner.onCreate();
            return new SampleSense{owner};
        }

        /** @brief Releases test provider storage before its callback-state owner can retire. */
        void Destroy(void *, IPerceptionSenseProvider *provider) noexcept {
            delete provider;
        }

        /** @brief Captures real typed registry and agent/listener fences for native core qualification. */
        struct Fixture final : PerceptionSenseTestSupport::InputFixture {
            std::array<std::size_t, 4> counters{};
            std::shared_ptr<ProviderOwner> owner{std::make_shared<ProviderOwner>(counters)};
            PerceptionSenseRegistration registration{{{MakeIdentity<SenseTypeId>(20),
                                                       {PerceptionDescriptorSourceKind::Native, MakeIdentity<PerceptionProviderId>(22), 1},
                                                       "custom sense",
                                                       capabilities},
                                                      {MakeIdentity<StimulusTypeId>(21),
                                                       {PerceptionDescriptorSourceKind::Native, MakeIdentity<PerceptionProviderId>(22), 1},
                                                       "custom observation",
                                                       1,
                                                       {}},
                                                      sizeof(SampleSense),
                                                      4},
                                                     {owner.get(), Create, Destroy}};

            Fixture() {
                registration.contract.maximumCandidates = candidates.size();
            }

            PerceptionDescriptorRegistry Registry() const {
                return InputFixture::Registry(registration.contract);
            }

            std::shared_ptr<const PerceptionSenseGeneration> Generation() const {
                std::shared_ptr<const std::atomic_bool> admission{owner, &owner->admission};
                auto captured = PerceptionSenseGeneration::Capture(std::span{&registration, 1}, std::move(admission));
                REQUIRE(captured.HasValue());
                return std::move(captured).Value();
            }
        };

        TEST_CASE("Custom perception capture is inert and rejects unavailable capabilities before factories",
                  "[unit][ai][sense-provider]") {
            Fixture fixture;
            auto generation = fixture.Generation();
            CHECK(fixture.counters[0] == 0);
            auto registry = fixture.Registry();
            ExpectError(generation->Create(generation, fixture.registration.contract.sense.identity, registry, {}, fixture.binding),
                        AIErrors::PerceptionCapabilityUnavailable);
            CHECK(fixture.counters[0] == 0);
            auto instance = fixture.Instance(generation);
            CHECK(fixture.counters[0] == 1);
        }

        TEST_CASE("Custom perception metadata copy failure reports storage exhaustion without another allocation",
                  "[unit][ai][sense-provider]") {
            Fixture fixture;
            fixture.registration.contract.sense.displayName = "A custom native sense name requiring owned metadata storage";
            Tests::AllocationProbe::Measurement observed;
            auto failed = [&] {
                Tests::AllocationProbe::ScopedMeasurement measurement;
                Tests::AllocationProbe::ScopedFailure allocation{1};
                auto result = ValidatePerceptionSenseRegistration(fixture.registration);
                observed = measurement.Snapshot();
                return result;
            }();
            ExpectError(failed, AIErrors::PerceptionRegistryStorageUnavailable);
            CHECK(failed.ErrorValue().message.empty());
            CHECK(observed.requests == 2);
            CHECK(fixture.counters[0] == 0);
            REQUIRE(ValidatePerceptionSenseRegistration(fixture.registration).HasValue());
        }

        TEST_CASE("Custom perception capture storage failure allocates no replacement diagnostic", "[unit][ai][sense-provider]") {
            Fixture fixture;
            std::shared_ptr<const std::atomic_bool> admission{fixture.owner, &fixture.owner->admission};
            Tests::AllocationProbe::Measurement observed;
            auto failed = [&] {
                Tests::AllocationProbe::ScopedMeasurement measurement;
                Tests::AllocationProbe::ScopedFailure allocation;
                auto result = PerceptionSenseGeneration::Capture(std::span{&fixture.registration, 1}, admission);
                observed = measurement.Snapshot();
                return result;
            }();
            ExpectError(failed, AIErrors::PerceptionRegistryStorageUnavailable);
            CHECK(failed.ErrorValue().message.empty());
            CHECK(observed.requests == 1);
            CHECK(fixture.counters[0] == 0);
            auto healthy = fixture.Instance(fixture.Generation());
            CHECK(healthy->CanEvaluate());
        }

        TEST_CASE("Custom perception instance storage failure preserves the generation without diagnostic allocation",
                  "[unit][ai][sense-provider]") {
            Fixture fixture;
            auto generation = fixture.Generation();
            auto registry = fixture.Registry();
            Tests::AllocationProbe::Measurement observed;
            auto failed = [&] {
                Tests::AllocationProbe::ScopedMeasurement measurement;
                Tests::AllocationProbe::ScopedFailure allocation;
                auto result = generation->Create(generation, fixture.registration.contract.sense.identity, registry, fixture.capabilities,
                                                 fixture.binding);
                observed = measurement.Snapshot();
                return result;
            }();
            ExpectError(failed, AIErrors::PerceptionRegistryStorageUnavailable);
            CHECK(failed.ErrorValue().message.empty());
            CHECK(observed.requests == 1);
            CHECK(fixture.counters[0] == 0);
            CHECK(generation->CanAdmit());
            auto healthy = fixture.Instance(generation);
            CHECK(healthy->CanEvaluate());
        }

        TEST_CASE("Custom perception preserves optional listener capability-unavailable admission", "[unit][ai][sense-provider]") {
            Fixture fixture;
            auto generation = fixture.Generation();
            auto admitted = fixture.Registry();
            auto listener = admitted.FindListener(fixture.binding.listenerType).Value().descriptor;
            listener.dependencyPolicy = PerceptionDependencyPolicy::Optional;
            const auto &contract = fixture.registration.contract;
            auto registry = PerceptionDescriptorRegistry::Capture({std::span{&contract.sense, 1}, std::span{&contract.stimulus, 1},
                                                                   std::span{&listener, 1}},
                                                                  {});
            REQUIRE(registry.HasValue());
            ExpectError(generation->Create(generation, contract.sense.identity, registry.Value(), fixture.capabilities, fixture.binding),
                        AIErrors::PerceptionCapabilityUnavailable);
            CHECK(fixture.counters[0] == 0);
        }

        TEST_CASE("Custom perception cannot override built-in event authority or install unsupported script callbacks",
                  "[unit][ai][sense-provider]") {
            Fixture fixture;
            auto registration = fixture.registration;
            registration.contract.sense.identity = SenseTypeIds::Hearing;
            ExpectError(ValidatePerceptionSenseRegistration(registration), AIErrors::PerceptionDescriptorInvalid);
            registration = fixture.registration;
            registration.contract.stimulus.identity = StimulusTypeIds::Damage;
            ExpectError(ValidatePerceptionSenseRegistration(registration), AIErrors::PerceptionDescriptorInvalid);
            registration = fixture.registration;
            registration.contract.sense.origin.kind = PerceptionDescriptorSourceKind::Script;
            ExpectError(ValidatePerceptionSenseRegistration(registration), AIErrors::PerceptionDescriptorInvalid);
            CHECK(fixture.counters[0] == 0);
        }

        TEST_CASE("Custom perception reserves complete distinct budgets and publishes stable frozen candidate outcomes",
                  "[unit][ai][sense-provider]") {
            Fixture fixture;
            auto instance = fixture.Instance(fixture.Generation());
            AiWorkBudget small{5}, global{6};
            ExpectError(instance->Evaluate(fixture.Input(), small, global), AIErrors::SchedulerBudgetExhausted);
            CHECK(small.Remaining() == 5);
            CHECK(global.Remaining() == 6);
            CHECK(fixture.counters[1] == 0);
            AiWorkBudget agent{6};
            const auto evaluated = instance->Evaluate(fixture.Input(), agent, global);
            REQUIRE(evaluated.HasValue());
            REQUIRE(evaluated.Value().count == 2);
            CHECK(evaluated.Value().samples[0].sensed);
            CHECK_FALSE(evaluated.Value().samples[1].sensed);
            CHECK(agent.Remaining() == 0);
            CHECK(global.Remaining() == 0);
            AiWorkBudget sameTick{6};
            ExpectError(instance->Evaluate(fixture.Input(), sameTick, sameTick), AIErrors::PerceptionMemoryTimeInvalid);
            CHECK(sameTick.Remaining() == 6);
            AiWorkBudget nextTick{6};
            REQUIRE(instance->Evaluate(fixture.Input(2), nextTick, nextTick).HasValue());
            CHECK(nextTick.Remaining() == 0);
        }

        TEST_CASE("Custom perception bounds candidate declarations and diagnostic source metadata before factories",
                  "[unit][ai][sense-provider]") {
            Fixture fixture;
            auto registration = fixture.registration;
            registration.contract.maximumCandidates = 0;
            ExpectError(ValidatePerceptionSenseRegistration(registration), AIErrors::PerceptionDescriptorInvalid);
            registration.contract.maximumCandidates = PerceptionSenseResult::MaximumCandidates + 1;
            ExpectError(ValidatePerceptionSenseRegistration(registration), AIErrors::PerceptionDescriptorInvalid);
            registration = fixture.registration;
            registration.contract.source = {"fixture.cpp", 0, 0};
            ExpectError(ValidatePerceptionSenseRegistration(registration), AIErrors::PerceptionDescriptorInvalid);
            registration.contract.source = {std::string(PerceptionSenseGeneration::MaximumSourceBytes + 1, 'x'), 1, 1};
            ExpectError(ValidatePerceptionSenseRegistration(registration), AIErrors::PerceptionDescriptorInvalid);
            CHECK(fixture.counters[0] == 0);
        }

        TEST_CASE("Custom perception rejects excessive input and older spatial revisions without invoking providers",
                  "[unit][ai][sense-provider]") {
            Fixture fixture;
            auto instance = fixture.Instance(fixture.Generation());
            const std::array<PerceptionSenseCandidate, 3> tooMany{fixture.candidates[0], fixture.candidates[1],
                                                                  PerceptionSenseCandidate{{77, 3, 1}, {}}};
            auto input = fixture.Input();
            input.candidates = tooMany;
            AiWorkBudget budget{6};
            ExpectError(instance->Evaluate(input, budget, budget), AIErrors::PerceptionSpatialLimitExceeded);
            CHECK(budget.Remaining() == 6);
            CHECK(fixture.counters[1] == 0);
            input = fixture.Input();
            input.spatialRevision = 2;
            REQUIRE(instance->Evaluate(input, budget, budget).HasValue());
            AiWorkBudget stale{6};
            ExpectError(instance->Evaluate(fixture.Input(2), stale, stale), AIErrors::PerceptionSpatialStale);
            CHECK(stale.Remaining() == 6);
            CHECK(fixture.counters[1] == 1);
        }

        TEST_CASE("Custom perception owner affinity rejects worker invocation before reading mutable instance state",
                  "[unit][ai][sense-provider]") {
            Fixture fixture;
            auto generation = fixture.Generation();
            auto instance = fixture.Instance(generation);
            bool rejected{};
            std::thread worker([&] {
                AiWorkBudget budget{6};
                auto evaluated = instance->Evaluate(fixture.Input(), budget, budget);
                rejected = evaluated.HasError() && evaluated.ErrorValue().code.Value() == AIErrors::RuntimeUnavailable.code.Value();
            });
            worker.join();
            CHECK(rejected);
            CHECK(fixture.counters[1] == 0);
            AiWorkBudget owner{6};
            REQUIRE(instance->Evaluate(fixture.Input(), owner, owner).HasValue());
        }

        TEST_CASE("Custom perception isolates malformed output exceptions and overrun from another instance",
                  "[unit][ai][sense-provider]") {
            for (const auto fault : {Fault::Throw, Fault::Typed, Fault::Duplicate, Fault::OutOfRange, Fault::OutOfOrder, Fault::Overrun}) {
                Fixture fixture;
                auto generation = fixture.Generation();
                auto instance = fixture.Instance(generation);
                fixture.owner->fault = fault;
                AiWorkBudget budget{6};
                const auto evaluated = instance->Evaluate(fixture.Input(), budget, budget);
                REQUIRE(evaluated.HasError());
                const auto &expected = fault == Fault::Typed     ? AIErrors::PerceptionCapabilityUnavailable
                                       : fault == Fault::Overrun ? AIErrors::SchedulerBudgetExhausted
                                                                 : AIErrors::PerceptionProviderFailed;
                CHECK(evaluated.ErrorValue().code.Value() == expected.code.Value());
                fixture.owner->fault = Fault::None;
                AiWorkBudget retry{6};
                if (fault == Fault::Typed)
                    REQUIRE(instance->Evaluate(fixture.Input(2), retry, retry).HasValue());
                else
                    ExpectError(instance->Evaluate(fixture.Input(2), retry, retry), AIErrors::RuntimeUnavailable);
                auto healthy = fixture.Instance(generation);
                AiWorkBudget independent{6};
                REQUIRE(healthy->Evaluate(fixture.Input(), independent, independent).HasValue());
            }
        }

        TEST_CASE("Custom perception rejects nested callbacks without consuming a later simulation tick", "[unit][ai][sense-provider]") {
            Fixture fixture;
            auto instance = fixture.Instance(fixture.Generation());
            fixture.owner->onEvaluate = [&] {
                CHECK_FALSE(instance->CanEvaluate());
                AiWorkBudget nested{6};
                ExpectError(instance->Evaluate(fixture.Input(2), nested, nested), AIErrors::RuntimeUnavailable);
                CHECK(nested.Remaining() == 6);
            };
            AiWorkBudget first{6};
            REQUIRE(instance->Evaluate(fixture.Input(), first, first).HasValue());
            CHECK(fixture.counters[1] == 1);
            CHECK(instance->CanEvaluate());
            fixture.owner->onEvaluate = {};
            AiWorkBudget next{6};
            REQUIRE(instance->Evaluate(fixture.Input(2), next, next).HasValue());
            CHECK(fixture.counters[1] == 2);
        }

        TEST_CASE("Custom perception diagnostic allocation failure remains isolated to the failing instance",
                  "[unit][ai][sense-provider]") {
            struct ProviderException final : std::exception {
                const char *what() const noexcept override {
                    return "A provider failed with a diagnostic long enough to require allocated string storage on the native host.";
                }
            };

            Fixture fixture;
            auto generation = fixture.Generation();
            auto instance = fixture.Instance(generation);
            SECTION("standard exception diagnostic") {
                fixture.owner->onEvaluate = [] {
                    throw ProviderException{};
                };
            }
            SECTION("untyped exception default diagnostic") {
                fixture.owner->onEvaluate = [] {
                    throw 42;
                };
            }
            AiWorkBudget budget{6};
            auto failed = [&] {
                Tests::AllocationProbe::ScopedFailure allocation;
                return instance->Evaluate(fixture.Input(), budget, budget);
            }();
            ExpectError(failed, AIErrors::PerceptionProviderFailed);
            CHECK(failed.ErrorValue().message.empty());
            CHECK_FALSE(instance->CanEvaluate());
            fixture.owner->onEvaluate = {};
            auto healthy = fixture.Instance(generation);
            AiWorkBudget independent{6};
            REQUIRE(healthy->Evaluate(fixture.Input(), independent, independent).HasValue());
            CHECK(fixture.counters[1] == 2);
        }

        TEST_CASE("Custom perception retains executable callback state through provider destruction", "[unit][ai][sense-provider]") {
            Fixture fixture;
            std::weak_ptr<ProviderOwner> observed = fixture.owner;
            auto generation = fixture.Generation();
            auto instance = fixture.Instance(generation);
            fixture.owner.reset();
            generation.reset();
            CHECK_FALSE(observed.expired());
            CHECK(fixture.counters[2] == 0);
            instance.reset();
            CHECK(fixture.counters[2] == 1);
            CHECK(fixture.counters[3] == 1);
            CHECK(observed.expired());
        }

        TEST_CASE("Custom perception rechecks retirement after factories and throwing evaluation callbacks", "[unit][ai][sense-provider]") {
            Fixture fixture;
            auto generation = fixture.Generation();
            SECTION("retirement during creation destroys the unexposed instance") {
                fixture.owner->onCreate = [&] {
                    fixture.owner->admission.store(false, std::memory_order_release);
                };
                auto registry = fixture.Registry();
                ExpectError(generation->Create(generation, fixture.registration.contract.sense.identity, registry, fixture.capabilities,
                                               fixture.binding),
                            AIErrors::RuntimeUnavailable);
                CHECK(fixture.counters[2] == 1);
            }
            SECTION("retirement takes precedence over a callback exception") {
                auto instance = fixture.Instance(generation);
                fixture.owner->onEvaluate = [&] {
                    fixture.owner->admission.store(false, std::memory_order_release);
                    throw std::runtime_error("retiring sense failed");
                };
                AiWorkBudget budget{6};
                ExpectError(instance->Evaluate(fixture.Input(), budget, budget), AIErrors::RuntimeUnavailable);
                CHECK_FALSE(generation->CanAdmit());
            }
        }
    }  // namespace
}  // namespace Horo::AI
