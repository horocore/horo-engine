#include "Horo/PlatformServices/PlatformServicesBackend.h"
#include "PlatformServicesTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <type_traits>

namespace Horo::PlatformServices {
    using TestSupport::AvailableCapabilities;

    namespace {
        template <typename T> Result<PlatformRequestHandle<T>> Unavailable() {
            return Result<PlatformRequestHandle<T>>::Failure(MakeError(BackendErrors::ServiceUnavailable));
        }

        class TestBackend final : public IPlatformServicesBackend {
        public:
            PlatformServiceCapabilitySnapshot snapshot;
            bool inspectFails{};
            bool activateFails{};
            bool activated{};
            std::uint32_t shutdownCalls{};

            Result<PlatformServiceCapabilitySnapshot> InspectCapabilities() const override {
                if (inspectFails)
                    return Result<PlatformServiceCapabilitySnapshot>::Failure(MakeError(BackendErrors::InvalidCapabilitySnapshot));
                return Result<PlatformServiceCapabilitySnapshot>::Success(snapshot);
            }

            Result<void> Activate(const PlatformServicesBackendConfig &) override {
                activated = true;
                if (activateFails)
                    return Result<void>::Failure(MakeError(BackendErrors::ServiceUnavailable));
                return Result<void>::Success();
            }

            Result<void> RequestCancel(PlatformRequestId, PlatformRequestGeneration) override {
                return Result<void>::Success();
            }

            Result<void> Shutdown() override {
                ++shutdownCalls;
                return Result<void>::Success();
            }

            Result<PlatformRequestHandle<void>> UnlockAchievement(AchievementUnlockRequest) override {
                return Unavailable<void>();
            }

            Result<PlatformRequestHandle<void>> SubmitScore(LeaderboardScoreRequest) override {
                return Unavailable<void>();
            }

            Result<PlatformRequestHandle<void>> WriteStat(StatWriteRequest) override {
                return Unavailable<void>();
            }

            Result<PlatformRequestHandle<CloudReadResult>> ReadCloudObject(CloudReadRequest) override {
                return Unavailable<CloudReadResult>();
            }

            Result<PlatformRequestHandle<void>> SetPresence(PresenceUpdateRequest) override {
                return Unavailable<void>();
            }

            Result<PlatformRequestHandle<void>> ClearPresence(PlatformSubjectHandle) override {
                return Unavailable<void>();
            }

            Result<PlatformRequestHandle<FriendsPage>> QueryFriends(FriendsQuery) override {
                return Unavailable<FriendsPage>();
            }

            Result<PlatformRequestHandle<PlatformSessionSnapshot>> QueryCurrentSession() override {
                return Unavailable<PlatformSessionSnapshot>();
            }
        };

        PlatformServiceCapabilitySnapshot Snapshot() {
            return AvailableCapabilities();
        }
    }  // namespace

    TEST_CASE("Platform service identities and interfaces stay typed and backend-neutral", "[platform-services][backend]") {
        static_assert(std::is_abstract_v<IPlatformServicesBackend>);
        static_assert(std::has_virtual_destructor_v<IPlatformServicesBackend>);
        static_assert(!std::is_same_v<AchievementId, LeaderboardId>);
        static_assert(!std::is_same_v<CloudObjectId, PresenceStatusId>);
        REQUIRE_FALSE(AchievementId{}.IsValid());
        REQUIRE(AchievementId{1}.IsValid());
        REQUIRE_FALSE(PlatformSubjectHandle{}.IsValid());
    }

    TEST_CASE("Capability snapshots require one coherent entry for every service", "[platform-services][backend][capability]") {
        PlatformServicesBackendConfig config;
        auto snapshot = Snapshot();
        REQUIRE(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasValue());

        snapshot.services[1].service = snapshot.services[0].service;
        CHECK(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).ErrorValue().code.Value() ==
              BackendErrors::InvalidCapabilitySnapshot.code.Value());
        snapshot = Snapshot();
        snapshot.services[0].service = static_cast<PlatformServiceKind>(255);
        CHECK(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasError());
        snapshot = Snapshot();
        snapshot.services[0].availability = static_cast<PlatformServiceAvailability>(255);
        CHECK(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasError());
        snapshot = Snapshot();
        snapshot.services[0].binding = PlatformServiceBindingId{};
        CHECK(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasError());
    }

    TEST_CASE("Available and unavailable capability evidence cannot contradict itself", "[platform-services][backend][capability]") {
        PlatformServicesBackendConfig config;
        auto snapshot = Snapshot();
        snapshot.services[0].binding.reset();
        REQUIRE(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasError());
        snapshot = Snapshot();
        snapshot.services[0].unavailableReason = PlatformServiceUnavailableReason::ServiceUnsupported;
        REQUIRE(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasError());

        snapshot = Snapshot();
        auto &friends = snapshot.services[static_cast<std::size_t>(PlatformServiceKind::Friends)];
        friends.availability = PlatformServiceAvailability::Unavailable;
        friends.binding.reset();
        friends.unavailableReason = PlatformServiceUnavailableReason::HostPolicyDenied;
        friends.limits = {};
        REQUIRE(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasValue());
        friends.binding = PlatformServiceBindingId{9};
        REQUIRE(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasError());
        friends.binding.reset();
        friends.unavailableReason.reset();
        REQUIRE(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasError());
    }

    TEST_CASE("Cloud mutation capability must be finite and belong to the cloud service", "[platform-services][backend][cloud]") {
        PlatformServicesBackendConfig config;
        auto snapshot = Snapshot();
        CloudMutationCapability mutation{.atomicity = CloudMutationAtomicity::ConditionalAtomicObject,
                                         .createIfAbsent = true,
                                         .replaceIfRevision = true,
                                         .deleteIfRevision = true,
                                         .durableMutationDedupe = true,
                                         .maxNamespaceBytes = 32,
                                         .maxObjectCount = 4,
                                         .maxConcurrentMutations = 2};
        auto &cloud = snapshot.services[static_cast<std::size_t>(PlatformServiceKind::Cloud)];
        cloud.cloudMutation = mutation;
        REQUIRE(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasValue());
        cloud.cloudMutation->replaceIfRevision = false;
        REQUIRE(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasError());
        cloud.cloudMutation = mutation;
        cloud.cloudMutation->durableMutationDedupe = false;
        REQUIRE(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasError());
        cloud.cloudMutation = mutation;
        cloud.cloudMutation->maxConcurrentMutations = 0;
        REQUIRE(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasError());
        cloud.cloudMutation.reset();
        snapshot.services[0].cloudMutation = mutation;
        REQUIRE(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasError());
    }

    TEST_CASE("Capability validation rejects incompatible versions identities limits and required absence",
              "[platform-services][backend][capability]") {
        PlatformServicesBackendConfig config;
        auto snapshot = Snapshot();
        snapshot.interfaceVersion.major = PlatformServicesBackendInterfaceMajor + 1;
        CHECK(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).ErrorValue().code.Value() ==
              BackendErrors::IncompatibleInterfaceVersion.code.Value());
        snapshot = Snapshot();
        snapshot.interfaceVersion.minor = PlatformServicesBackendInterfaceMinor + 1;
        CHECK(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).ErrorValue().code.Value() ==
              BackendErrors::IncompatibleInterfaceVersion.code.Value());
        snapshot = Snapshot();
        snapshot.provider = {};
        REQUIRE(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasError());
        snapshot = Snapshot();
        snapshot.providerGeneration = {};
        REQUIRE(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasError());
        snapshot = Snapshot();
        snapshot.services[0].limits.maxConcurrentRequests = 0;
        REQUIRE(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasError());
        snapshot = Snapshot();
        snapshot.services[0].limits.maxPayloadBytes = (1ULL << 40U) + 1U;
        REQUIRE(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).HasError());

        snapshot = Snapshot();
        const auto cloudIndex = static_cast<std::size_t>(PlatformServiceKind::Cloud);
        snapshot.services[cloudIndex].availability = PlatformServiceAvailability::Unavailable;
        snapshot.services[cloudIndex].binding.reset();
        snapshot.services[cloudIndex].unavailableReason = PlatformServiceUnavailableReason::ServiceUnsupported;
        snapshot.services[cloudIndex].limits = {};
        config.requiredServices[cloudIndex] = true;
        CHECK(ValidatePlatformServiceCapabilitySnapshot(snapshot, config).ErrorValue().code.Value() ==
              BackendErrors::RequiredServiceUnavailable.code.Value());
    }

    TEST_CASE("Candidate validation precedes activation and preserves typed unsupported behavior",
              "[platform-services][backend][lifecycle]") {
        PlatformServicesBackendConfig config;
        TestBackend backend;
        backend.snapshot = Snapshot();
        backend.snapshot.interfaceVersion.major = 99;
        REQUIRE(ActivatePlatformServicesBackend(backend, config).HasError());
        CHECK_FALSE(backend.activated);

        backend.inspectFails = false;
        backend.activateFails = true;
        backend.snapshot = Snapshot();
        REQUIRE(ActivatePlatformServicesBackend(backend, config).HasError());
        CHECK(backend.activated);
        CHECK(backend.shutdownCalls == 1);

        backend.activateFails = false;
        backend.snapshot = Snapshot();
        const auto activated = ActivatePlatformServicesBackend(backend, config);
        REQUIRE(activated.HasValue());
        CHECK(backend.activated);
        CHECK(activated.Value().providerGeneration == PlatformProviderGeneration{7});
        const auto unsupported = backend.UnlockAchievement({{}, {5}});
        REQUIRE(unsupported.HasError());
        CHECK(unsupported.ErrorValue().code.Value() == BackendErrors::ServiceUnavailable.code.Value());

        backend.inspectFails = true;
        backend.activated = false;
        REQUIRE(ActivatePlatformServicesBackend(backend, config).HasError());
        CHECK_FALSE(backend.activated);
    }
}  // namespace Horo::PlatformServices
