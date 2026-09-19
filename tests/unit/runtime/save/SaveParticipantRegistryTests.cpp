#include "AllocationProbe.h"
#include "Horo/Runtime/Save/SaveCaptureSnapshot.h"
#include "Horo/Runtime/Save/SaveErrors.h"
#include "SaveCaptureSnapshotTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Runtime {
    namespace {
        SaveParticipantId Participant(const std::string_view value) {
            return SaveParticipantId::Parse(value).Value();
        }

        SaveRecordId Record(const std::string_view value) {
            return SaveRecordId::Parse(value).Value();
        }

        ParticipantSchemaVersion Schema(const std::uint32_t value = 1) {
            return ParticipantSchemaVersion::Create(value).Value();
        }

        CanonicalStateParticipantDescriptor Descriptor(const std::string_view participant,
                                                       const std::string_view record = "00112233-4455-6677-8899-aabbccddeeff") {
            return {
                .participant = Participant(participant),
                .schemaVersion = Schema(),
                .scope = SaveParticipantScope::RuntimeScene,
                .roles = SaveParticipantRole::Capture | SaveParticipantRole::Restore,
                .required = true,
                .limits = {.maximumPayloadBytes = 1024, .maximumRecordCount = 16, .maximumNestingDepth = 8},
                .dependencies = {},
                .ownedRecords = {Record(record)},
            };
        }

        std::shared_ptr<const ICanonicalStateAdapter> Adapter(const std::shared_ptr<int> &destructionCount) {
            return std::make_shared<CaptureTestSupport::CountingCaptureAdapter>(destructionCount);
        }

        std::vector<std::string> ParticipantIds(const std::span<const SaveParticipantBinding> bindings) {
            std::vector<std::string> identities;
            identities.reserve(bindings.size());
            for (const SaveParticipantBinding &binding : bindings)
                identities.push_back(binding.Descriptor().participant.Value());
            return identities;
        }

        /** @brief Verifies that registry admission rejects one invalid descriptor. */
        void RequireInvalidDescriptor(CanonicalStateParticipantRegistry &registry, CanonicalStateParticipantDescriptor descriptor,
                                      const std::shared_ptr<int> &destructionCount) {
            REQUIRE(registry.Register(std::move(descriptor), Adapter(destructionCount)).ErrorValue().code.Value() ==
                    SaveErrors::ParticipantDescriptorInvalid.code.Value());
        }

        TEST_CASE("Participant descriptors reject invalid metadata before snapshot publication", "[unit][save][registry]") {
            auto destructionCount = std::make_shared<int>();
            CanonicalStateParticipantRegistry registry;

            auto invalid = Descriptor("horo.test.invalid");
            invalid.schemaVersion = {};
            RequireInvalidDescriptor(registry, std::move(invalid), destructionCount);
            REQUIRE(registry.Register(Descriptor("horo.test.missing_adapter"), nullptr).ErrorValue().code.Value() ==
                    SaveErrors::ParticipantAdapterMissing.code.Value());

            invalid = Descriptor("horo.test.no_roles");
            invalid.roles = SaveParticipantRole::None;
            RequireInvalidDescriptor(registry, std::move(invalid), destructionCount);
            invalid = Descriptor("horo.test.unbounded");
            invalid.limits.maximumPayloadBytes = 0;
            RequireInvalidDescriptor(registry, std::move(invalid), destructionCount);
            invalid = Descriptor("horo.test.no_record_capacity");
            invalid.limits.maximumRecordCount = 0;
            RequireInvalidDescriptor(registry, std::move(invalid), destructionCount);
            invalid = Descriptor("horo.test.insufficient_record_capacity");
            invalid.ownedRecords.push_back(Record("10112233-4455-6677-8899-aabbccddeeff"));
            invalid.limits.maximumRecordCount = 1;
            RequireInvalidDescriptor(registry, std::move(invalid), destructionCount);
            invalid = Descriptor("horo.test.no_nesting");
            invalid.limits.maximumNestingDepth = 0;
            RequireInvalidDescriptor(registry, std::move(invalid), destructionCount);
            invalid = Descriptor("horo.test.no_participant");
            invalid.participant = {};
            RequireInvalidDescriptor(registry, std::move(invalid), destructionCount);
            invalid = Descriptor("horo.test.no_records");
            invalid.ownedRecords.clear();
            RequireInvalidDescriptor(registry, std::move(invalid), destructionCount);
            invalid = Descriptor("horo.test.duplicate_record");
            invalid.ownedRecords.push_back(invalid.ownedRecords.front());
            RequireInvalidDescriptor(registry, std::move(invalid), destructionCount);
        }

        TEST_CASE("Participant descriptors reject unsupported scope role and dependency metadata", "[unit][save][registry]") {
            auto destructionCount = std::make_shared<int>();
            CanonicalStateParticipantRegistry registry;
            auto invalid = Descriptor("horo.test.unknown_role");
            invalid.roles = static_cast<SaveParticipantRole>(0x80U);
            RequireInvalidDescriptor(registry, std::move(invalid), destructionCount);
            invalid = Descriptor("horo.test.unknown_scope");
            invalid.scope = static_cast<SaveParticipantScope>(0xffU);
            RequireInvalidDescriptor(registry, std::move(invalid), destructionCount);
            invalid = Descriptor("horo.test.invalid_dependency");
            invalid.dependencies = {SaveParticipantId{}};
            RequireInvalidDescriptor(registry, std::move(invalid), destructionCount);
            invalid = Descriptor("horo.test.self_dependency");
            invalid.dependencies = {invalid.participant};
            RequireInvalidDescriptor(registry, std::move(invalid), destructionCount);
            invalid = Descriptor("horo.test.duplicate_dependency");
            invalid.dependencies = {Participant("horo.test.provider"), Participant("horo.test.provider")};
            RequireInvalidDescriptor(registry, std::move(invalid), destructionCount);
            invalid = Descriptor("horo.test.unknown_dependency_requirement");
            invalid.dependencies = {{Participant("horo.test.provider"), static_cast<SaveParticipantDependencyRequirement>(0xffU)}};
            RequireInvalidDescriptor(registry, std::move(invalid), destructionCount);
            invalid = Descriptor("horo.test.unknown_dependency_phase");
            invalid.dependencies = {{Participant("horo.test.provider"), SaveParticipantDependencyRequirement::Required,
                                     static_cast<SaveParticipantDependencyPhase>(0xffU)}};
            RequireInvalidDescriptor(registry, std::move(invalid), destructionCount);
            invalid = Descriptor("horo.test.excess_dependencies");
            invalid.dependencies.assign(MaximumSaveParticipantCount + 1, Participant("horo.test.provider"));
            RequireInvalidDescriptor(registry, std::move(invalid), destructionCount);
        }

        TEST_CASE("Dependency uniqueness is scoped by provider and operation phase", "[unit][save][registry]") {
            auto destructionCount = std::make_shared<int>();
            CanonicalStateParticipantRegistry registry;
            auto consumer = Descriptor("horo.test.consumer");
            consumer.dependencies = {
                {Participant("horo.test.provider"), SaveParticipantDependencyRequirement::Optional,
                 SaveParticipantDependencyPhase::Capture},
                {Participant("horo.test.provider"), SaveParticipantDependencyRequirement::Required,
                 SaveParticipantDependencyPhase::Restore},
            };
            REQUIRE(registry.Register(std::move(consumer), Adapter(destructionCount)).HasValue());
            const auto missing = registry.Snapshot();
            REQUIRE(missing.HasError());
            CHECK(missing.ErrorValue().code.Value() == SaveErrors::ParticipantDependencyMissing.code.Value());
            CHECK(missing.ErrorValue().message.find("restore") != std::string::npos);

            auto provider = Descriptor("horo.test.provider", "10112233-4455-6677-8899-aabbccddeeff");
            REQUIRE(registry.Register(std::move(provider), Adapter(destructionCount)).HasValue());
            const auto snapshot = registry.Snapshot().Value();
            CHECK(ParticipantIds(snapshot.CaptureBindings()) == std::vector<std::string>{"horo.test.provider", "horo.test.consumer"});
            CHECK(ParticipantIds(snapshot.RestoreBindings()) == std::vector<std::string>{"horo.test.provider", "horo.test.consumer"});

            CanonicalStateParticipantRegistry overlapping;
            auto invalid = Descriptor("horo.test.overlap");
            invalid.dependencies = {
                {Participant("horo.test.provider"), SaveParticipantDependencyRequirement::Required,
                 SaveParticipantDependencyPhase::CaptureAndRestore},
                {Participant("horo.test.provider"), SaveParticipantDependencyRequirement::Optional,
                 SaveParticipantDependencyPhase::Capture},
            };
            RequireInvalidDescriptor(overlapping, std::move(invalid), destructionCount);
        }

        TEST_CASE("Participant registration is unique bounded and generation checked", "[unit][save][registry]") {
            auto destructionCount = std::make_shared<int>();
            CanonicalStateParticipantRegistry registry;
            const auto first = registry.Register(Descriptor("horo.test.scene"), Adapter(destructionCount));
            REQUIRE(first.HasValue());
            REQUIRE(first.Value().registryGeneration == 2);
            REQUIRE(registry.Generation() == first.Value().registryGeneration);
            REQUIRE(registry.Register(Descriptor("horo.test.scene"), Adapter(destructionCount)).ErrorValue().code.Value() ==
                    SaveErrors::ParticipantDuplicate.code.Value());

            REQUIRE(registry.Register(Descriptor("horo.test.other"), Adapter(destructionCount)).ErrorValue().code.Value() ==
                    SaveErrors::ParticipantRecordOwnershipDuplicate.code.Value());

            const auto snapshot = registry.Snapshot();
            REQUIRE(snapshot.HasValue());
            REQUIRE(snapshot.Value().Generation() == first.Value().registryGeneration);
            REQUIRE(snapshot.Value().Bindings().size() == 1);
            REQUIRE(snapshot.Value().Find(Participant("horo.test.scene")) != nullptr);
            REQUIRE(snapshot.Value().Find(Participant("horo.test.absent")) == nullptr);
            REQUIRE(SaveParticipantRegistrySnapshot{}.Bindings().empty());
            REQUIRE(SaveParticipantRegistrySnapshot{}.CaptureBindings().empty());
            REQUIRE(SaveParticipantRegistrySnapshot{}.RestoreBindings().empty());
            REQUIRE_FALSE(SaveParticipantRegistrySnapshot{}.IsValid());
            REQUIRE_FALSE(registry.Unregister(Participant("horo.test.absent")).Value());
        }

        TEST_CASE("Snapshot ordering and lookup share the canonical participant key", "[unit][save][registry]") {
            auto destructionCount = std::make_shared<int>();
            CanonicalStateParticipantRegistry registry;
            REQUIRE(registry.Register(Descriptor("horo.test.aa", "10112233-4455-6677-8899-aabbccddeeff"), Adapter(destructionCount))
                        .HasValue());
            REQUIRE(registry.Register(Descriptor("horo.test.a.z", "20112233-4455-6677-8899-aabbccddeeff"), Adapter(destructionCount))
                        .HasValue());
            const auto snapshot = registry.Snapshot().Value();
            CHECK(ParticipantIds(snapshot.Bindings()) == std::vector<std::string>{"horo.test.a.z", "horo.test.aa"});
            CHECK(snapshot.Find(Participant("horo.test.a.z")) != nullptr);
            CHECK(snapshot.Find(Participant("horo.test.aa")) != nullptr);
        }

        TEST_CASE("Allocation failure preserves registry membership generation and leases", "[unit][save][registry]") {
            bool reachedSuccessfulRegistration = false;
            for (std::size_t successfulAllocations = 0; successfulAllocations < 64 && !reachedSuccessfulRegistration;
                 ++successfulAllocations) {
#ifdef _WIN32
                std::fprintf(stderr, "[allocation-test] register budget %zu begin\n", successfulAllocations);
                std::fflush(stderr);
#endif
                auto destructionCount = std::make_shared<int>();
                auto adapter = Adapter(destructionCount);
                const auto descriptor = Descriptor("horo.alloc");
                CanonicalStateParticipantRegistry registry;
                const std::uint64_t generation = registry.Generation();
                auto registration = [&] {
                    Tests::AllocationProbe::ScopedFailure failure{successfulAllocations};
                    return registry.Register(descriptor, adapter);
                }();
#ifdef _WIN32
                std::fprintf(stderr, "[allocation-test] register budget %zu end value=%d\n", successfulAllocations,
                             registration.HasValue() ? 1 : 0);
                std::fflush(stderr);
#endif
                reachedSuccessfulRegistration = registration.HasValue();
                if (!reachedSuccessfulRegistration) {
                    CHECK(registration.ErrorValue().code.Value() == SaveErrors::ParticipantRegistryAllocationFailed.code.Value());
                    CHECK(registry.Generation() == generation);
                    CHECK(registry.Snapshot().Value().Bindings().empty());
                    CHECK(adapter.use_count() == 1);
                }
            }
            REQUIRE(reachedSuccessfulRegistration);

            auto destructionCount = std::make_shared<int>();
            auto adapter = Adapter(destructionCount);
            CanonicalStateParticipantRegistry registry;
            REQUIRE(registry.Register(Descriptor("horo.alloc"), adapter).HasValue());
            const std::uint64_t generation = registry.Generation();
            bool reachedSuccessfulSnapshot = false;
            for (std::size_t successfulAllocations = 0; successfulAllocations < 64 && !reachedSuccessfulSnapshot; ++successfulAllocations) {
                auto snapshot = [&] {
                    Tests::AllocationProbe::ScopedFailure failure{successfulAllocations};
                    return registry.Snapshot();
                }();
                reachedSuccessfulSnapshot = snapshot.HasValue();
                if (!reachedSuccessfulSnapshot) {
                    CHECK(snapshot.ErrorValue().code.Value() == SaveErrors::ParticipantRegistryAllocationFailed.code.Value());
                    CHECK(registry.Generation() == generation);
                    CHECK(adapter.use_count() == 2);
                }
            }
            REQUIRE(reachedSuccessfulSnapshot);
            CHECK(registry.Snapshot().Value().Find(Participant("horo.alloc")) != nullptr);
        }

        TEST_CASE("Participant registry enforces its explicit capacity", "[unit][save][registry]") {
            auto destructionCount = std::make_shared<int>();
            CanonicalStateParticipantRegistry registry;
            for (std::size_t index = 0; index < MaximumSaveParticipantCount; ++index) {
                auto descriptor = Descriptor("horo.test.placeholder");
                descriptor.participant = Participant("horo.test.p_" + std::to_string(index));
                SaveIdentityDetail::Bytes bytes{};
                bytes[0] = 1;
                bytes[14] = static_cast<std::uint8_t>(index >> 8U);
                bytes[15] = static_cast<std::uint8_t>(index);
                descriptor.ownedRecords = {SaveRecordId::FromBytes(bytes).Value()};
                REQUIRE(registry.Register(std::move(descriptor), Adapter(destructionCount)).HasValue());
            }
            auto overflow = Descriptor("horo.test.overflow", "ffffffff-ffff-ffff-ffff-ffffffffffff");
            REQUIRE(registry.Register(std::move(overflow), Adapter(destructionCount)).ErrorValue().code.Value() ==
                    SaveErrors::ParticipantRegistryCapacityExceeded.code.Value());
        }

        TEST_CASE("Registry snapshots are immutable and retain exact adapter leases", "[unit][save][registry]") {
            auto destructionCount = std::make_shared<int>();
            SaveParticipantRegistrySnapshot retainedSnapshot;
            {
                CanonicalStateParticipantRegistry registry;
                REQUIRE(registry.Register(Descriptor("horo.test.second", "10112233-4455-6677-8899-aabbccddeeff"), Adapter(destructionCount))
                            .HasValue());
                REQUIRE(registry.Register(Descriptor("horo.test.first"), Adapter(destructionCount)).HasValue());
                {
                    const SaveParticipantRegistrySnapshot sourceSnapshot = registry.Snapshot().Value();
                    retainedSnapshot = sourceSnapshot;
                    REQUIRE(sourceSnapshot.Bindings()[0].Descriptor().participant.Value() == "horo.test.first");
                    REQUIRE(sourceSnapshot.Bindings()[1].Descriptor().participant.Value() == "horo.test.second");
                }

                REQUIRE(registry.Unregister(Participant("horo.test.first")).Value());
                const auto secondSnapshot = registry.Snapshot().Value();
                REQUIRE(secondSnapshot.Generation() != retainedSnapshot.Generation());
                REQUIRE(secondSnapshot.Bindings().size() == 1);
                REQUIRE(retainedSnapshot.Bindings().size() == 2);
                registry.Close();
                REQUIRE(registry.IsClosed());
                REQUIRE(registry.Snapshot().HasError());
                REQUIRE(registry.Register(Descriptor("horo.test.late"), nullptr).ErrorValue().code.Value() ==
                        SaveErrors::ParticipantRegistryClosed.code.Value());
                REQUIRE(*destructionCount == 0);
            }
            REQUIRE(*destructionCount == 0);
            CHECK(ParticipantIds(retainedSnapshot.Bindings()) == std::vector<std::string>{"horo.test.first", "horo.test.second"});
            CHECK(ParticipantIds(retainedSnapshot.CaptureBindings()) == std::vector<std::string>{"horo.test.first", "horo.test.second"});
            CHECK(ParticipantIds(retainedSnapshot.RestoreBindings()) == std::vector<std::string>{"horo.test.first", "horo.test.second"});
            retainedSnapshot = {};
            REQUIRE(*destructionCount == 2);
        }

        TEST_CASE("Snapshot publication rejects missing and cyclic dependencies", "[unit][save][registry]") {
            auto destructionCount = std::make_shared<int>();
            CanonicalStateParticipantRegistry missing;
            auto dependent = Descriptor("horo.test.dependent");
            dependent.dependencies = {Participant("horo.test.provider")};
            REQUIRE(missing.Register(std::move(dependent), Adapter(destructionCount)).HasValue());
            const auto missingResult = missing.Snapshot();
            REQUIRE(missingResult.HasError());
            REQUIRE(missingResult.ErrorValue().code.Value() == SaveErrors::ParticipantDependencyMissing.code.Value());
            CHECK(missingResult.ErrorValue().message.find("horo.test.dependent") != std::string::npos);
            CHECK(missingResult.ErrorValue().message.find("horo.test.provider") != std::string::npos);

            CanonicalStateParticipantRegistry cyclic;
            auto first = Descriptor("horo.test.first");
            first.dependencies = {Participant("horo.test.second")};
            auto second = Descriptor("horo.test.second", "10112233-4455-6677-8899-aabbccddeeff");
            second.dependencies = {Participant("horo.test.first")};
            auto downstream = Descriptor("horo.test.downstream", "20112233-4455-6677-8899-aabbccddeeff");
            downstream.dependencies = {Participant("horo.test.first")};
            REQUIRE(cyclic.Register(std::move(first), Adapter(destructionCount)).HasValue());
            REQUIRE(cyclic.Register(std::move(second), Adapter(destructionCount)).HasValue());
            REQUIRE(cyclic.Register(std::move(downstream), Adapter(destructionCount)).HasValue());
            const auto cyclicResult = cyclic.Snapshot();
            REQUIRE(cyclicResult.HasError());
            REQUIRE(cyclicResult.ErrorValue().code.Value() == SaveErrors::ParticipantDependencyCycle.code.Value());
            CHECK(cyclicResult.ErrorValue().message.find("horo.test.first") != std::string::npos);
            CHECK(cyclicResult.ErrorValue().message.find("horo.test.second") != std::string::npos);
            CHECK(cyclicResult.ErrorValue().message.find("horo.test.downstream") == std::string::npos);

            CanonicalStateParticipantRegistry acyclic;
            auto consumer = Descriptor("horo.test.consumer");
            consumer.dependencies = {Participant("horo.test.provider")};
            REQUIRE(acyclic.Register(Descriptor("horo.test.provider", "20112233-4455-6677-8899-aabbccddeeff"), Adapter(destructionCount))
                        .HasValue());
            REQUIRE(acyclic.Register(std::move(consumer), Adapter(destructionCount)).HasValue());
            REQUIRE(acyclic.Snapshot().HasValue());
        }

        TEST_CASE("Capture and restore plans are stable across registration order", "[unit][save][registry]") {
            auto destructionCount = std::make_shared<int>();
            const auto buildPlan = [&destructionCount](const bool reverse) {
                CanonicalStateParticipantRegistry registry;
                auto consumer = Descriptor("horo.test.a_consumer", "10112233-4455-6677-8899-aabbccddeeff");
                consumer.dependencies = {Participant("horo.test.z_provider"), Participant("horo.test.m_independent")};
                auto independent = Descriptor("horo.test.m_independent", "20112233-4455-6677-8899-aabbccddeeff");
                auto provider = Descriptor("horo.test.z_provider", "30112233-4455-6677-8899-aabbccddeeff");
                if (reverse) {
                    REQUIRE(registry.Register(std::move(provider), Adapter(destructionCount)).HasValue());
                    REQUIRE(registry.Register(std::move(independent), Adapter(destructionCount)).HasValue());
                    REQUIRE(registry.Register(std::move(consumer), Adapter(destructionCount)).HasValue());
                } else {
                    REQUIRE(registry.Register(std::move(consumer), Adapter(destructionCount)).HasValue());
                    REQUIRE(registry.Register(std::move(independent), Adapter(destructionCount)).HasValue());
                    REQUIRE(registry.Register(std::move(provider), Adapter(destructionCount)).HasValue());
                }
                return registry.Snapshot().Value();
            };

            const auto first = buildPlan(false);
            const auto second = buildPlan(true);
            CHECK(ParticipantIds(first.Bindings()) == ParticipantIds(second.Bindings()));
            CHECK(ParticipantIds(first.CaptureBindings()) == ParticipantIds(second.CaptureBindings()));
            CHECK(ParticipantIds(first.RestoreBindings()) == ParticipantIds(second.RestoreBindings()));
            CHECK(ParticipantIds(first.CaptureBindings()) ==
                  std::vector<std::string>{"horo.test.m_independent", "horo.test.z_provider", "horo.test.a_consumer"});
            CHECK(ParticipantIds(first.RestoreBindings()) ==
                  std::vector<std::string>{"horo.test.m_independent", "horo.test.z_provider", "horo.test.a_consumer"});
            REQUIRE(first.Bindings().front().Descriptor().dependencies.size() == 2);
            CHECK(first.Bindings().front().Descriptor().dependencies[0].participant.Value() == "horo.test.m_independent");
            CHECK(first.Bindings().front().Descriptor().dependencies[1].participant.Value() == "horo.test.z_provider");
        }

        TEST_CASE("Phase-specific dependencies produce independent acyclic plans", "[unit][save][registry]") {
            auto destructionCount = std::make_shared<int>();
            CanonicalStateParticipantRegistry registry;
            auto alpha = Descriptor("horo.test.alpha");
            alpha.dependencies = {
                {Participant("horo.test.zeta"), SaveParticipantDependencyRequirement::Required, SaveParticipantDependencyPhase::Capture}};
            auto zeta = Descriptor("horo.test.zeta", "10112233-4455-6677-8899-aabbccddeeff");
            zeta.dependencies = {
                {Participant("horo.test.alpha"), SaveParticipantDependencyRequirement::Required, SaveParticipantDependencyPhase::Restore}};
            REQUIRE(registry.Register(std::move(zeta), Adapter(destructionCount)).HasValue());
            REQUIRE(registry.Register(std::move(alpha), Adapter(destructionCount)).HasValue());

            const auto snapshot = registry.Snapshot().Value();
            CHECK(ParticipantIds(snapshot.CaptureBindings()) == std::vector<std::string>{"horo.test.zeta", "horo.test.alpha"});
            CHECK(ParticipantIds(snapshot.RestoreBindings()) == std::vector<std::string>{"horo.test.alpha", "horo.test.zeta"});
        }

        TEST_CASE("Dependency plans break diamond ties by participant identity", "[unit][save][registry]") {
            auto destructionCount = std::make_shared<int>();
            CanonicalStateParticipantRegistry registry;
            auto left = Descriptor("horo.test.b_left", "10112233-4455-6677-8899-aabbccddeeff");
            left.dependencies = {Participant("horo.test.a_root")};
            auto right = Descriptor("horo.test.c_right", "20112233-4455-6677-8899-aabbccddeeff");
            right.dependencies = {Participant("horo.test.a_root")};
            auto join = Descriptor("horo.test.z_join", "30112233-4455-6677-8899-aabbccddeeff");
            join.dependencies = {Participant("horo.test.b_left"), Participant("horo.test.c_right")};
            REQUIRE(registry.Register(std::move(join), Adapter(destructionCount)).HasValue());
            REQUIRE(registry.Register(std::move(right), Adapter(destructionCount)).HasValue());
            REQUIRE(registry.Register(Descriptor("horo.test.a_root", "40112233-4455-6677-8899-aabbccddeeff"), Adapter(destructionCount))
                        .HasValue());
            REQUIRE(registry.Register(std::move(left), Adapter(destructionCount)).HasValue());

            const auto snapshot = registry.Snapshot().Value();
            const std::vector<std::string> expected{"horo.test.a_root", "horo.test.b_left", "horo.test.c_right", "horo.test.z_join"};
            CHECK(ParticipantIds(snapshot.CaptureBindings()) == expected);
            CHECK(ParticipantIds(snapshot.RestoreBindings()) == expected);
        }

        TEST_CASE("Optional absence and incompatible phases are explicit", "[unit][save][registry]") {
            auto destructionCount = std::make_shared<int>();
            CanonicalStateParticipantRegistry optional;
            auto consumer = Descriptor("horo.test.optional_consumer");
            consumer.dependencies = {
                {Participant("horo.test.absent"), SaveParticipantDependencyRequirement::Optional, SaveParticipantDependencyPhase::Capture}};
            REQUIRE(optional.Register(std::move(consumer), Adapter(destructionCount)).HasValue());
            REQUIRE(optional.Snapshot().HasValue());

            CanonicalStateParticipantRegistry exclusiveRoles;
            auto captureOnly = Descriptor("horo.test.capture_only", "30112233-4455-6677-8899-aabbccddeeff");
            captureOnly.roles = SaveParticipantRole::Capture;
            auto restoreOnly = Descriptor("horo.test.restore_only", "40112233-4455-6677-8899-aabbccddeeff");
            restoreOnly.roles = SaveParticipantRole::Restore;
            REQUIRE(exclusiveRoles.Register(std::move(captureOnly), Adapter(destructionCount)).HasValue());
            REQUIRE(exclusiveRoles.Register(std::move(restoreOnly), Adapter(destructionCount)).HasValue());
            const auto exclusiveSnapshot = exclusiveRoles.Snapshot().Value();
            CHECK(ParticipantIds(exclusiveSnapshot.CaptureBindings()) == std::vector<std::string>{"horo.test.capture_only"});
            CHECK(ParticipantIds(exclusiveSnapshot.RestoreBindings()) == std::vector<std::string>{"horo.test.restore_only"});

            CanonicalStateParticipantRegistry incompatible;
            auto provider = Descriptor("horo.test.capture_provider", "10112233-4455-6677-8899-aabbccddeeff");
            provider.roles = SaveParticipantRole::Capture;
            auto restoreConsumer = Descriptor("horo.test.restore_consumer", "20112233-4455-6677-8899-aabbccddeeff");
            restoreConsumer.dependencies = {
                {provider.participant, SaveParticipantDependencyRequirement::Optional, SaveParticipantDependencyPhase::Restore}};
            REQUIRE(incompatible.Register(std::move(provider), Adapter(destructionCount)).HasValue());
            REQUIRE(incompatible.Register(std::move(restoreConsumer), Adapter(destructionCount)).HasValue());
            const auto phaseFailure = incompatible.Snapshot();
            REQUIRE(phaseFailure.HasError());
            CHECK(phaseFailure.ErrorValue().code.Value() == SaveErrors::ParticipantDependencyPhaseIncompatible.code.Value());
            CHECK(phaseFailure.ErrorValue().message.find("horo.test.capture_provider") != std::string::npos);
            CHECK(phaseFailure.ErrorValue().message.find("horo.test.restore_consumer") != std::string::npos);

            CanonicalStateParticipantRegistry invalidDeclaration;
            auto invalidCaptureOnly = Descriptor("horo.test.capture_only", "50112233-4455-6677-8899-aabbccddeeff");
            invalidCaptureOnly.roles = SaveParticipantRole::Capture;
            invalidCaptureOnly.dependencies = {
                {Participant("horo.test.other"), SaveParticipantDependencyRequirement::Optional, SaveParticipantDependencyPhase::Restore}};
            const auto invalid = invalidDeclaration.Register(std::move(invalidCaptureOnly), Adapter(destructionCount));
            REQUIRE(invalid.HasError());
            CHECK(invalid.ErrorValue().code.Value() == SaveErrors::ParticipantDependencyPhaseIncompatible.code.Value());
        }

        TEST_CASE("Descriptors remain inert until explicit registry operations", "[unit][save][registry]") {
            auto destructionCount = std::make_shared<int>();
            auto adapter = Adapter(destructionCount);
            const CanonicalStateParticipantDescriptor descriptor = Descriptor("horo.test.inert");
            const auto copy = descriptor;
            REQUIRE(copy == descriptor);
            REQUIRE(adapter.use_count() == 1);
            REQUIRE(*destructionCount == 0);
        }
    }  // namespace
}  // namespace Horo::Runtime
