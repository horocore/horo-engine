#include "AllocationProbe.h"
#include "Horo/Navigation/Backends/DeterministicCrowdProvider.h"
#include "Horo/Navigation/Backends/RecastDetourCrowdProvider.h"
#include "Horo/Navigation/NavigationCrowdDiagnostics.h"
#include "navigation/NavigationCrowdScaleFixture.h"

#include <algorithm>
#include <bit>
#include <iostream>
#include <string_view>

namespace Horo::Navigation {
    namespace {
        using namespace CrowdQualification;
        using TestSupport::RequireError;

        constexpr std::array Layouts{std::pair{Layout::Crossings, std::string_view{"crossings"}},
                                     std::pair{Layout::Bottleneck, std::string_view{"bottleneck"}},
                                     std::pair{Layout::Formation, std::string_view{"formation"}},
                                     std::pair{Layout::MixedPriorities, std::string_view{"mixed_priorities"}}};

        /** @brief Takes an ordered or deliberately shuffled sample of the scripted fixture. */
        NavigationCrowdSnapshot Capture(const CrowdScene &scene, const Layout layout, const std::uint64_t tick, const bool reverse,
                                        const NavigationCrowdQualification *qualified = nullptr) {
            const auto agents = scene.agents.Snapshot().Value();
            std::vector<NavigationCrowdMotionSample> motions;
            motions.reserve(agents.Agents().size());
            for (const auto &agent : agents.Agents()) {
                auto motion = Sample(agent, layout, tick);
                if (scene.teleported == agent.handle)
                    motion.position.z += 24.0F;
                motions.push_back(motion);
            }
            if (reverse)
                std::ranges::reverse(motions);
            return BuildCapture(scene, agents, motions, tick, qualified);
        }

        /** @brief Checks the shared publishable outcome contract for the full population. */
        std::size_t CheckOutcomes(const NavigationCrowdSnapshot &capture, const std::span<const NavigationCrowdBatchResult> results) {
            REQUIRE(results.size() == CrowdAgents);
            const auto metrics = SummarizeNavigationCrowd(capture);
            CHECK(metrics.activeAgents == CrowdAgents);
            CHECK(metrics.neighborFacts <= CrowdAgents * FactsPerAgent);
            CHECK(capture.BoundarySegments().size() <= FactsPerAgent);
            CHECK(metrics.neighborsOmitted == 0);
            CHECK(metrics.boundariesOmitted == 0);
            std::size_t advancing{};
            for (std::size_t index = 0; index < results.size(); ++index) {
                const auto &[agent, outcome] = results[index];
                const auto velocity = outcome.desiredVelocity;
                CHECK(agent == capture.CellAgentIndices()[index]);
                CHECK(outcome.captureTick == capture.CaptureTick());
                CHECK(outcome.binding == capture.Binding());
                CHECK(velocity.y == 0);
                if (outcome.disposition == NavigationAvoidanceDisposition::CollisionSafeStop)
                    CHECK(velocity == Math::Vec3{});
                else if (velocity.x * velocity.x + velocity.z * velocity.z > 0)
                    ++advancing;
            }
            return advancing;
        }

        /** @brief Records exact ordered outcomes while leaving timing outside the replay digest. */
        void HashOutcomes(const std::span<const NavigationCrowdBatchResult> results, Sha256Builder &digest) {
            for (const auto &[agent, outcome] : results) {
                const auto velocity = outcome.desiredVelocity;
                const std::array words{static_cast<std::uint32_t>(agent), static_cast<std::uint32_t>(outcome.stopReason),
                                       std::bit_cast<std::uint32_t>(velocity.x), std::bit_cast<std::uint32_t>(velocity.z)};
                REQUIRE(digest.Update(std::as_bytes(std::span{words})));
            }
        }

        /** @brief Replay the same layout in the native qualified owner with independently ordered inputs. */
        Sha256Digest RunQualifiedReplay(const Layout layout, const bool reverse) {
            CrowdScene scene;
            scene.CommitOverlay(layout);
            constexpr std::string_view identity = "NAV-006.12 CrowdSmall reference replay";
            const auto fixture = ComputeSha256(std::as_bytes(std::span{identity.data(), identity.size()}));
            const NavigationCrowdQualification
                qualification{DeterministicCrowdProviderFingerprint(), fixture, fixture, fixture, fixture, FixedStep, CrowdAgents};
            auto provider = CreateDeterministicCrowdBackend(qualification).Value();
            Sha256Builder digest;
            std::size_t advanced{};
            for (std::uint64_t tick = 1; tick <= Steps; ++tick) {
                const auto capture = Capture(scene, layout, tick, reverse, &qualification);
                auto requests = Requests(capture, layout);
                if (reverse)
                    std::ranges::reverse(requests);
                const auto results =
                    NavigationCrowdBatch::RunQualified(capture, requests, *provider, qualification, scene.activation.topology);
                REQUIRE(results.HasValue());
                advanced += CheckOutcomes(capture, results.Value());
                HashOutcomes(results.Value(), digest);
            }
            CHECK(advanced > 0);
            scene.Close();
            return digest.Finalize();
        }

        /** @brief Counts C++ allocations on the real Detour solve thread. */
        class MeasuredCrowdProvider final : public INavigationCrowdBackend {
        public:
            explicit MeasuredCrowdProvider(std::unique_ptr<INavigationCrowdBackend> backend) : backend_(std::move(backend)) {}

            /** @copydoc INavigationCrowdBackend::Solve */
            Result<NavigationAvoidanceOutcome> Solve(const NavigationCrowdSnapshot &capture,
                                                     const NavigationAvoidanceRequest &request) override {
                Tests::AllocationProbe::ScopedMeasurement worker;
                auto result = backend_->Solve(capture, request);
                const auto observed = worker.Snapshot();
                measurement_.requests += observed.requests;
                measurement_.requestedBytes += observed.requestedBytes;
                return result;
            }

            /** @brief Reads the solve totals after the only worker has joined. */
            [[nodiscard]] auto Measurement() const noexcept {
                return measurement_;
            }

        private:
            std::unique_ptr<INavigationCrowdBackend> backend_;
            Tests::AllocationProbe::Measurement measurement_;
        };

        /** @brief Measures a single real Detour worker per tick, then drains all work. */
        void RunBestEffort(const Layout layout, const std::string_view name) {
            CrowdScene scene;
            scene.CommitOverlay(layout);
            auto provider = std::make_shared<MeasuredCrowdProvider>(CreateRecastDetourCrowdBackend({FactsPerAgent, FactsPerAgent}).Value());
            const auto *observed = provider.get();
            const std::array<std::shared_ptr<INavigationCrowdBackend>, 1> providers{std::move(provider)};
            JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 1, .maxRetainedTerminalJobs = Steps});
            std::array<std::uint64_t, Steps> elapsed{};
            std::size_t advanced{};
            std::size_t maximumOwnerBytes{};
            for (std::uint64_t tick = 1; tick <= Steps; ++tick) {
                auto [capture, batch] = [&] {
                    Tests::AllocationProbe::ScopedMeasurement owner;
                    auto snapshot = Capture(scene, layout, tick, false);
                    auto submitted = NavigationCrowdBatch::Submit(jobs, snapshot, Requests(snapshot, layout), providers);
                    maximumOwnerBytes = std::max(maximumOwnerBytes, owner.Snapshot().requestedBytes);
                    return std::pair{std::move(snapshot), std::move(submitted)};
                }();
                REQUIRE(batch.HasValue());
                advanced += CheckOutcomes(capture, Collect(*batch.Value(), capture, elapsed[tick - 1]));
            }
            CHECK(advanced > 0);
            jobs.Shutdown(ShutdownPolicy::Drain);
            const auto workerAllocations = observed->Measurement();
            CHECK(workerAllocations.requests == 0);
            CHECK(workerAllocations.requestedBytes == 0);
            const auto journal = jobs.SnapshotIfChanged(0);
            REQUIRE(journal);
            REQUIRE(journal->jobs.size() == Steps);
            CHECK(std::ranges::all_of(journal->jobs, [](const auto &job) {
                return job.state == JobState::Succeeded;
            }));
            CHECK(providers.front().use_count() == 1);
            scene.Close();
            std::ranges::sort(elapsed);
            std::cout << "NAV-006.12 profile=CrowdSmall layout=" << name << " agents=" << CrowdAgents << " ticks=" << Steps
                      << " build=" << HORO_QUALIFICATION_BUILD_TYPE << " platform=" << HORO_QUALIFICATION_PLATFORM
                      << " compiler=" << HORO_QUALIFICATION_COMPILER << " solve_p50_ns=" << elapsed[elapsed.size() / 2]
                      << " solve_p95_ns=" << elapsed[(elapsed.size() - 1) * 95 / 100]
                      << " capture_submit_max_owner_cpp_requested_bytes=" << maximumOwnerBytes
                      << " detour_worker_cpp_allocations=" << workerAllocations.requests << '\n';
        }

    }  // namespace

    TEST_CASE("CrowdSmall layouts exercise both bounded Detour work and deterministic replay", "[integration][navigation][crowd][scale]") {
        for (const auto &[layout, name] : Layouts) {
            CAPTURE(name);
            CHECK(RunQualifiedReplay(layout, false) == RunQualifiedReplay(layout, true));
            RunBestEffort(layout, name);
        }
    }

    TEST_CASE("CrowdSmall overload withdraws old work across teleports and removal", "[integration][navigation][crowd][scale]") {
        Tests::AllocationProbe::ScopedMeasurement owner;
        const auto layout = Layout::Crossings;
        CrowdScene scene;
        scene.CommitOverlay(layout);
        auto provider = std::shared_ptr<INavigationCrowdBackend>(CreateRecastDetourCrowdBackend().Value());
        const std::array providers{provider};
        JobSystem jobs({.workerCount = 0, .maxQueuedJobs = 1, .maxRetainedTerminalJobs = 2});
        const auto original = Capture(scene, layout, 1, false);
        const auto requests = Requests(original, layout);
        auto submitted = NavigationCrowdBatch::Submit(jobs, original, requests, providers);
        REQUIRE(submitted.HasValue());
        auto oldWork = std::move(submitted).Value();
        const auto rejected = NavigationCrowdBatch::Submit(jobs, original, requests, providers);
        REQUIRE(rejected.HasError());
        CHECK(jobs.AdmissionSnapshot().rejected[static_cast<std::size_t>(JobPriority::Normal)] == 1);

        const auto &first = original.Agents().front();
        const auto removed = original.Agents().back().handle;
        scene.teleported = first.handle;
        REQUIRE(scene.agents.UnregisterAtSafePoint(removed).HasValue());
        const auto changed = Capture(scene, layout, 1, false);
        REQUIRE(changed.Agents().size() == CrowdAgents - 1);
        CHECK(changed.Agents().front().position.z > first.position.z + 20);
        REQUIRE(scene.agents.Find(removed).HasError());
        RequireError(oldWork->TryTake(changed, 1), NavigationErrors::StaleSnapshot);
        RequireError(oldWork->TryTake(original, 1), NavigationErrors::AdmissionRejected);
        oldWork.reset();
        jobs.Shutdown(ShutdownPolicy::Cancel);
        const auto journal = jobs.SnapshotIfChanged(0);
        REQUIRE(journal);
        REQUIRE(journal->jobs.size() == 1);
        CHECK(journal->jobs.front().state == JobState::Cancelled);
        CHECK(provider.use_count() == 2);
        scene.Close();
        const auto allocation = owner.Snapshot();
        CHECK(allocation.requestedBytes > 0);
        std::cout << "NAV-006.12 profile=CrowdSmall layout=overlap_and_retirement agents=" << CrowdAgents
                  << " build=" << HORO_QUALIFICATION_BUILD_TYPE << " platform=" << HORO_QUALIFICATION_PLATFORM
                  << " compiler=" << HORO_QUALIFICATION_COMPILER << " cumulative_owner_cpp_requested_bytes=" << allocation.requestedBytes
                  << '\n';
    }

}  // namespace Horo::Navigation
