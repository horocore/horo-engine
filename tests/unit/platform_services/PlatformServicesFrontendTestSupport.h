#pragma once

#include "Horo/Foundation/Telemetry/Telemetry.h"
#include "Horo/PlatformServices/PlatformRequestErrors.h"
#include "Horo/PlatformServices/PlatformServicesFrontend.h"
#include "PlatformServicesTestSupport.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace Horo::PlatformServices {
    using TestSupport::AvailableCapabilities;
    using TestSupport::RequireError;

    namespace {
        constexpr std::size_t ServiceCount = static_cast<std::size_t>(PlatformServiceKind::Count);

        class RoutingBackend final : public IPlatformServicesBackend {
        public:
            explicit RoutingBackend(const std::size_t activeCapacity = 32)
                : requests{{.activeCapacity = activeCapacity, .terminalCapacity = 32, .observerCapacity = 32, .generation = {19}}} {}

            PlatformServiceCapabilitySnapshot snapshot;
            PlatformRequestStore requests;
            std::array<std::uint32_t, ServiceCount> calls{};
            mutable std::uint32_t inspectCalls{};
            std::uint32_t shutdownCalls{};
            std::uint32_t cancelCalls{};
            bool shutdownFails{};
            bool shutdownThrows{};
            bool shutdownThrowsNonStandard{};
            bool malformedHandle{};
            std::optional<LeaderboardRankedQuery> lastRankedQuery;
            std::optional<LeaderboardAroundSubjectQuery> lastAroundSubjectQuery;
            std::optional<LeaderboardFriendsQuery> lastFriendsLeaderboardQuery;

            Result<PlatformServiceCapabilitySnapshot> InspectCapabilities() const override {
                ++inspectCalls;
                return Result<PlatformServiceCapabilitySnapshot>::Success(snapshot);
            }

            Result<void> Activate(const PlatformServicesBackendConfig &) override {
                return Result<void>::Success();
            }

            Result<void> RequestCancel(const PlatformRequestId id, const PlatformRequestGeneration generation) override {
                ++cancelCalls;
                const auto result = requests.RequestCancel(id, generation);
                return result.HasError() ? Result<void>::Failure(result.ErrorValue()) : Result<void>::Success();
            }

            Result<void> Shutdown() override {
                ++shutdownCalls;
                requests.Shutdown();
                if (shutdownThrows)
                    throw std::runtime_error("test backend shutdown failure");
                if (shutdownThrowsNonStandard)
                    throw 17;
                if (shutdownFails)
                    return Result<void>::Failure(MakeError(BackendErrors::ServiceUnavailable));
                return Result<void>::Success();
            }

            Result<PlatformRequestHandle<void>> UnlockAchievement(AchievementUnlockRequest) override {
                return Admit<void>(PlatformServiceKind::Achievements);
            }

            Result<PlatformRequestHandle<void>> SubmitScore(LeaderboardScoreRequest) override {
                return Admit<void>(PlatformServiceKind::LeaderboardsAndStats);
            }

            Result<PlatformRequestHandle<LeaderboardEntriesPage>> QueryRankedLeaderboard(LeaderboardRankedQuery query) override {
                lastRankedQuery = query;
                return Admit<LeaderboardEntriesPage>(PlatformServiceKind::LeaderboardsAndStats);
            }

            Result<PlatformRequestHandle<LeaderboardAroundSubjectResult>> QueryLeaderboardAroundSubject(
                LeaderboardAroundSubjectQuery query) override {
                lastAroundSubjectQuery = query;
                return Admit<LeaderboardAroundSubjectResult>(PlatformServiceKind::LeaderboardsAndStats);
            }

            Result<PlatformRequestHandle<LeaderboardEntriesPage>> QueryFriendsLeaderboard(LeaderboardFriendsQuery query) override {
                lastFriendsLeaderboardQuery = query;
                return Admit<LeaderboardEntriesPage>(PlatformServiceKind::LeaderboardsAndStats);
            }

            Result<PlatformRequestHandle<void>> WriteStat(StatWriteRequest) override {
                return Admit<void>(PlatformServiceKind::LeaderboardsAndStats);
            }

            Result<PlatformRequestHandle<CloudReadResult>> ReadCloudObject(CloudReadRequest) override {
                return Admit<CloudReadResult>(PlatformServiceKind::Cloud);
            }

            Result<PlatformRequestHandle<void>> WriteCloudObject(CloudWriteRequest) override {
                return Admit<void>(PlatformServiceKind::Cloud);
            }

            Result<PlatformRequestHandle<void>> SetPresence(PresenceUpdateRequest) override {
                return Admit<void>(PlatformServiceKind::Presence);
            }

            Result<PlatformRequestHandle<void>> ClearPresence(PlatformSubjectHandle) override {
                return Admit<void>(PlatformServiceKind::Presence);
            }

            Result<PlatformRequestHandle<FriendsPage>> QueryFriends(FriendsQuery) override {
                return Admit<FriendsPage>(PlatformServiceKind::Friends);
            }

            Result<PlatformRequestHandle<PlatformSessionSnapshot>> QueryCurrentSession() override {
                return Admit<PlatformSessionSnapshot>(PlatformServiceKind::Session);
            }

            [[nodiscard]] std::uint32_t TotalCalls() const noexcept {
                std::uint32_t result{};
                for (const auto count : calls)
                    result += count;
                return result;
            }

        private:
            template <typename T> Result<PlatformRequestHandle<T>> Admit(const PlatformServiceKind service) {
                ++calls[static_cast<std::size_t>(service)];
                if (malformedHandle)
                    return Result<PlatformRequestHandle<T>>::Success({});
                return requests.Admit<T>();
            }
        };

        PlatformServiceCapabilitySnapshot Capabilities(const PlatformProviderGeneration generation = {7}) {
            return AvailableCapabilities(generation, {.maxConcurrentRequests = 16, .maxPageEntries = 4, .maxPayloadBytes = 4});
        }

        PlatformSessionSnapshot Session(const PlatformProviderGeneration provider = {7}, const PlatformSessionGeneration generation = {5},
                                        const PlatformSessionPhase phase = PlatformSessionPhase::Active,
                                        const PlatformSessionAccessState access = PlatformSessionAccessState::Granted,
                                        const std::byte nonce = std::byte{1}) {
            PlatformSessionCandidate candidate{.phase = phase,
                                               .generation = generation,
                                               .providerGeneration = provider,
                                               .accessRevision = {3},
                                               .reason = phase == PlatformSessionPhase::Active ? PlatformSessionReason::None
                                                                                               : PlatformSessionReason::UserSignedOut};
            candidate.capabilities.services.fill(phase == PlatformSessionPhase::Active ? access : PlatformSessionAccessState::Unavailable);
            if (phase == PlatformSessionPhase::Active) {
                PlatformSubjectNonce subject;
                subject.bytes.back() = nonce;
                candidate.subjectNonce = subject;
            }
            auto built = BuildPlatformSessionSnapshot(candidate);
            REQUIRE(built.HasValue());
            return std::move(built).Value();
        }

        PlatformServicesFrontend Frontend(const std::shared_ptr<RoutingBackend> &backend, PlatformSessionSnapshot session,
                                          PlatformServicesOperationPolicy policy = {}) {
            backend->snapshot = Capabilities(session.ProviderGeneration());
            auto result = PlatformServicesFrontend::Create(backend, backend->snapshot, std::move(session), std::move(policy));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }
    }  // namespace

}  // namespace Horo::PlatformServices
