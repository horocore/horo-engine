#include "AudioPreviewHostTestSupport.h"
#include "EditorAudioPreviewControlHost.h"
#include "Horo/Foundation/Telemetry/Operation.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace Horo::Editor::AudioPreviewTest {
    namespace {
        struct Gate final {
            std::mutex mutex;
            std::condition_variable changed;
            bool entered{}, released{};

            void Enter() {
                std::unique_lock lock(mutex);
                entered = true;
                changed.notify_one();
                changed.wait(lock, [this] {
                    return released;
                });
            }

            bool AwaitEntry() {
                std::unique_lock lock(mutex);
                return changed.wait_for(lock, std::chrono::seconds(3), [this] {
                    return entered;
                });
            }

            void Release() {
                std::lock_guard lock(mutex);
                released = true;
                changed.notify_all();
            }
        };

        /** @brief Unblock native-preparation simulation before bridge destruction even on a fatal assertion. */
        struct ReleaseOnExit final {
            std::shared_ptr<Gate> gate;

            ~ReleaseOnExit() {
                gate->Release();
            }
        };

        struct OwnershipEvidence final {
            std::atomic<bool> wrongThread{}, retired{};
            std::atomic<std::size_t> factories{}, advances{};
            std::atomic<float> peak{};
            std::atomic<std::uint64_t> retirementOperation{};
        };

        /** @brief Record code/dependency retirement on the same control owner that issued the actual output. */
        struct ControlLease final {
            std::shared_ptr<OwnershipEvidence> evidence;
            std::thread::id owner;

            ControlLease(std::shared_ptr<OwnershipEvidence> retained, const std::thread::id control)
                : evidence(std::move(retained)), owner(control) {}

            ~ControlLease() {
                if (std::this_thread::get_id() != owner)
                    evidence->wrongThread.store(true);
                evidence->retirementOperation.store(Telemetry::CaptureOperationIdentity().operationId);
                evidence->retired.store(true);
            }
        };

        /** @brief Compose the real Null driver while recording its owner and gating preparation, never render execution. */
        EditorAudioPreviewOutputFactory OwnedOutput(const std::shared_ptr<OutputEvidence> &output,
                                                    const std::shared_ptr<OwnershipEvidence> &evidence,
                                                    const std::shared_ptr<Gate> &gate = {}) {
            const auto ui = std::this_thread::get_id();
            return [output, evidence, gate, ui](const Audio::AudioRuntimeId owner) -> Result<EditorAudioPreviewBackend> {
                const auto control = std::this_thread::get_id();
                if (control == ui)
                    evidence->wrongThread.store(true);
                evidence->factories.fetch_add(1);
                if (gate)
                    gate->Enter();
                auto created = ObservedOutput(output)(owner);
                if (created.HasError())
                    return created;
                auto selected = std::move(created).Value();
                auto lease = std::make_shared<ControlLease>(evidence, control);
                const auto wrap = [output, evidence, control, lease](std::function<Result<void>()> advance) {
                    return [output, evidence, control, lease, advance = std::move(advance)]() -> Result<void> {
                        if (std::this_thread::get_id() != control)
                            evidence->wrongThread.store(true);
                        evidence->advances.fetch_add(1);
                        auto result = advance();
                        evidence->peak.store(output->peak);
                        return result;
                    };
                };
                selected.advanceControl = wrap(std::move(selected.advanceControl));
                selected.advanceCallback = wrap(std::move(selected.advanceCallback));
                return Result<EditorAudioPreviewBackend>::Success(std::move(selected));
            };
        }

        /** @brief Retry explicit bounded admission backpressure, never silently replace a queued request. */
        void Admit(EditorAudioPreviewControlHost &host, const AudioComponentPreviewRequest &request) {
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < end) {
                const auto admitted = host.Start(request);
                if (admitted.HasValue())
                    return;
                REQUIRE(admitted.ErrorValue().code.Value() == AudioComponentPreviewErrors::Busy.code.Value());
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            FAIL("Control-owner admission did not progress");
        }

        /** @brief Observe actual published lifecycle values without calling an inner frontend/backend on the UI thread. */
        template <typename Predicate> AudioComponentPreviewSnapshot Await(EditorAudioPreviewControlHost &host, Predicate predicate) {
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < end) {
                (void)host.Pump();
                auto snapshot = host.Snapshot();
                if (predicate(snapshot))
                    return snapshot;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            FAIL("Published Audio lifecycle did not progress");
            return {};
        }

        TEST_CASE("Audio control bridge copies preparation input and renders on one owned control thread",
                  "[audio][editor][preview][control-owner]") {
            ClipProject project;
            const auto capture = std::make_shared<OutputEvidence>();
            const auto evidence = std::make_shared<OwnershipEvidence>();
            const auto gate = std::make_shared<Gate>();
            Audio::Backend::AudioRuntimeOwnerIssuer owners;
            auto created = EditorAudioPreviewControlHost::Create(owners, OwnedOutput(capture, evidence, gate));
            REQUIRE(created.HasValue());
            auto host = std::move(created).Value();
            ReleaseOnExit release{gate};
            auto request = project.Request();
            {
                const Telemetry::OperationContext context{.operationId = 771, .parentOperationId = 770};
                Telemetry::ScopedOperationContext operation{context};
                Admit(*host, request);
            }
            REQUIRE(gate->AwaitEntry());
            request.object = 99;
            request.source.playback.gain = 0.0F;
            CHECK(host->Pump().HasValue());  // The preparation owner remains blocked in the gate.
            CHECK(host->Snapshot().object == 7);
            CHECK_FALSE(host->Snapshot().output);
            CHECK(host->Snapshot().preparation == AudioComponentPreviewPreparation::Pending);
            CHECK(host->Start(project.Request()).HasError());
            host->Stop(project.clip);  // A foreign workspace cannot revoke this request.
            gate->Release();
            const auto active = Await(*host, [&](const auto &snapshot) {
                return snapshot.output && snapshot.output->phase == Audio::AudioFrontendPhase::Active && evidence->peak.load() > 0.01F;
            });
            CHECK(active.object == 7);
            CHECK(active.preparation == AudioComponentPreviewPreparation::None);
            CHECK_FALSE(evidence->retired.load());
            // Retire an active lane through bridge shutdown itself; join synchronizes the fixture evidence.
            host.reset();
            CHECK(capture->destroyed);
            CHECK(capture->closedAtDestruction);
            CHECK(capture->renderingBlocks > 0);
            CHECK(capture->peak > 0.01F);
            CHECK_FALSE(evidence->wrongThread.load());
            CHECK(evidence->factories.load() == 1);
            CHECK(evidence->advances.load() > 0);
            CHECK(evidence->retired.load());
            CHECK(evidence->retirementOperation.load() == 771);
        }

        TEST_CASE("Audio control bridge retains Stop during blocked preparation and never activates the revoked lane",
                  "[audio][editor][preview][control-owner]") {
            ClipProject project;
            const auto capture = std::make_shared<OutputEvidence>();
            const auto evidence = std::make_shared<OwnershipEvidence>();
            const auto gate = std::make_shared<Gate>();
            Audio::Backend::AudioRuntimeOwnerIssuer owners;
            auto created = EditorAudioPreviewControlHost::Create(owners, OwnedOutput(capture, evidence, gate));
            REQUIRE(created.HasValue());
            auto host = std::move(created).Value();
            ReleaseOnExit release{gate};
            Admit(*host, project.Request());
            REQUIRE(gate->AwaitEntry());
            host->Stop(project.workspace);
            CHECK(host->Pump().HasValue());
            gate->Release();
            (void)Await(*host, [](const auto &snapshot) {
                return snapshot.output && snapshot.output->phase == Audio::AudioFrontendPhase::Closed;
            });
            host.reset();
            CHECK(capture->destroyed);
            CHECK(capture->closedAtDestruction);
            CHECK(capture->renderingBlocks == 0);
            CHECK_FALSE(evidence->wrongThread.load());
            CHECK(evidence->retired.load());
        }

        TEST_CASE("A completed Stop cannot revoke a later preview generation in the same workspace",
                  "[audio][editor][preview][control-owner]") {
            ClipProject project;
            const auto capture = std::make_shared<OutputEvidence>();
            const auto evidence = std::make_shared<OwnershipEvidence>();
            Audio::Backend::AudioRuntimeOwnerIssuer owners;
            auto created = EditorAudioPreviewControlHost::Create(owners, OwnedOutput(capture, evidence));
            REQUIRE(created.HasValue());
            auto host = std::move(created).Value();
            auto request = project.Request();
            Admit(*host, request);
            const auto first = Await(*host, [](const auto &snapshot) {
                return snapshot.output && snapshot.output->phase == Audio::AudioFrontendPhase::Active;
            });
            host->Stop(project.workspace);
            (void)Await(*host, [](const auto &snapshot) {
                return snapshot.output && snapshot.output->phase == Audio::AudioFrontendPhase::Closed;
            });
            ++request.revision;
            Admit(*host, request);
            const auto second = Await(*host, [&](const auto &snapshot) {
                return snapshot.revision == request.revision && snapshot.output &&
                       snapshot.output->phase == Audio::AudioFrontendPhase::Active;
            });
            REQUIRE(first.output);
            REQUIRE(second.output);
            CHECK(first.output->voice.owner != second.output->voice.owner);
            host.reset();
            CHECK(evidence->factories.load() == 2);
            CHECK_FALSE(evidence->wrongThread.load());
            CHECK(capture->closedAtDestruction);
        }

        TEST_CASE("Audio control bridge publishes original preparation failure and admits a later generation",
                  "[audio][editor][preview][control-owner]") {
            ClipProject project;
            const auto original = WithCause(MakeError(Audio::AudioErrors::BackendFailed, "Selected peer could not prepare"),
                                            MakeError(Audio::AudioErrors::DeviceUnavailable));
            Audio::Backend::AudioRuntimeOwnerIssuer owners;
            auto created = EditorAudioPreviewControlHost::Create(owners, [original](Audio::AudioRuntimeId) {
                return Result<EditorAudioPreviewBackend>::Failure(original);
            });
            REQUIRE(created.HasValue());
            auto host = std::move(created).Value();
            auto request = project.Request();
            Admit(*host, request);
            const auto failed = Await(*host, [](const auto &snapshot) {
                return snapshot.failure.has_value();
            });
            REQUIRE(failed.failure);
            CHECK(failed.preparation == AudioComponentPreviewPreparation::None);
            CHECK_FALSE(failed.output);
            CHECK(failed.failure->message == original.message);
            CHECK(failed.failure->cause.Get() == original.cause.Get());
            CHECK(host->Pump().HasError());
            ++request.revision;
            Admit(*host, request);
            const auto replacement = Await(*host, [&](const auto &snapshot) {
                return snapshot.revision == request.revision && snapshot.failure.has_value();
            });
            CHECK(replacement.failure->code.Value() == original.code.Value());
        }

        TEST_CASE("Audio control bridge captures and isolates diagnostic lineage for successive requests",
                  "[audio][editor][preview][control-owner]") {
            ClipProject project;
            auto contexts = std::make_shared<std::vector<Telemetry::OperationContext>>();
            Audio::Backend::AudioRuntimeOwnerIssuer owners;
            auto created = EditorAudioPreviewControlHost::Create(owners, [contexts](Audio::AudioRuntimeId) {
                contexts->push_back(Telemetry::CaptureOperationContext());  // Control owner only; read after actual join.
                return Result<EditorAudioPreviewBackend>::Failure(MakeError(AudioComponentPreviewErrors::ProviderUnavailable));
            });
            REQUIRE(created.HasValue());
            auto host = std::move(created).Value();
            auto request = project.Request();
            {
                const Telemetry::OperationContext captured{.operationId = 101,
                                                           .parentOperationId = 100,
                                                           .diagnosticContext = Log::LogContextSnapshot::Isolated({{"first", "one"}})};
                Telemetry::ScopedOperationContext operation{captured};
                Admit(*host, request);
            }
            (void)Await(*host, [](const auto &snapshot) {
                return snapshot.failure.has_value();
            });
            ++request.revision;
            {
                const Telemetry::OperationContext captured{.operationId = 201,
                                                           .parentOperationId = 200,
                                                           .diagnosticContext = Log::LogContextSnapshot::Isolated({{"second", "two"}})};
                Telemetry::ScopedOperationContext operation{captured};
                Admit(*host, request);
            }
            (void)Await(*host, [&](const auto &snapshot) {
                return snapshot.revision == request.revision && snapshot.failure.has_value();
            });
            host.reset();
            REQUIRE(contexts->size() == 2);
            CHECK((*contexts)[0].operationId == 101);
            CHECK((*contexts)[0].parentOperationId == 100);
            CHECK((*contexts)[1].operationId == 201);
            CHECK((*contexts)[1].parentOperationId == 200);
            const auto first = (*contexts)[0].diagnosticContext.Fields();
            const auto second = (*contexts)[1].diagnosticContext.Fields();
            REQUIRE(first.size() == 1);
            REQUIRE(second.size() == 1);
            CHECK(first[0].first == "first");
            CHECK(first[0].second == "one");
            CHECK(second[0].first == "second");
            CHECK(second[0].second == "two");
        }

        TEST_CASE("Audio control exceptions retain their reason after safe unwinding and do not become permanent Busy",
                  "[audio][editor][preview][control-owner]") {
            ClipProject project;
            Audio::Backend::AudioRuntimeOwnerIssuer owners;
            auto created = EditorAudioPreviewControlHost::Create(owners, [](Audio::AudioRuntimeId) -> Result<EditorAudioPreviewBackend> {
                throw std::runtime_error("Control preparation failed before acquiring an output");
            });
            REQUIRE(created.HasValue());
            auto host = std::move(created).Value();
            Admit(*host, project.Request());
            const auto failed = Await(*host, [](const auto &snapshot) {
                return snapshot.failure.has_value();
            });
            REQUIRE(failed.failure);
            CHECK(failed.failure->code.Value() == Audio::AudioErrors::BackendFailed.code.Value());
            CHECK(failed.failure->message == "Control preparation failed before acquiring an output");
            CHECK(failed.preparation == AudioComponentPreviewPreparation::None);
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            bool terminal{};
            while (!terminal && std::chrono::steady_clock::now() < end) {
                const auto retried = host->Start(project.Request());
                REQUIRE(retried.HasError());
                terminal = retried.ErrorValue().code.Value() == Audio::AudioErrors::BackendFailed.code.Value();
                if (!terminal) {
                    REQUIRE(retried.ErrorValue().code.Value() == AudioComponentPreviewErrors::Busy.code.Value());
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            CHECK(terminal);
        }

        TEST_CASE("Audio control bridge joins idle and partial startup without invoking the selected factory",
                  "[audio][editor][preview][control-owner]") {
            Audio::Backend::AudioRuntimeOwnerIssuer owners;
            CHECK(EditorAudioPreviewControlHost::Create(owners, {}).HasError());
            const auto calls = std::make_shared<std::atomic<std::size_t>>(0);
            for (std::size_t iteration = 0; iteration < 16; ++iteration) {
                auto created = EditorAudioPreviewControlHost::Create(owners, [calls](Audio::AudioRuntimeId) {
                    calls->fetch_add(1);
                    return Result<EditorAudioPreviewBackend>::Failure(MakeError(AudioComponentPreviewErrors::ProviderUnavailable));
                });
                REQUIRE(created.HasValue());
                auto host = std::move(created).Value();
                host.reset();
            }
            CHECK(calls->load() == 0);
        }

        TEST_CASE("Independent control owners issue unique Audio identities from one retained process authority",
                  "[audio][editor][preview][control-owner][identity]") {
            Audio::Backend::AudioRuntimeOwnerIssuer owners;
            std::array<std::vector<Audio::AudioRuntimeId>, 2> issued;
            const auto reserve = [&](const std::size_t lane) {
                auto &output = issued[lane];
                output.reserve(256);
                for (std::size_t generation = 0; generation < 256; ++generation) {
                    const auto owner = owners.Issue();
                    if (owner.HasValue())
                        output.push_back(owner.Value());
                }
            };
            std::jthread first{reserve, 0};
            std::jthread second{reserve, 1};
            first.join();
            second.join();
            REQUIRE(issued[0].size() == 256);
            REQUIRE(issued[1].size() == 256);
            issued[0].insert(issued[0].end(), issued[1].begin(), issued[1].end());
            std::ranges::sort(issued[0]);
            CHECK(std::ranges::adjacent_find(issued[0]) == issued[0].end());
            CHECK(issued[0].front().Value() == 1);
            CHECK(issued[0].back().Value() == 512);
        }
    }  // namespace
}  // namespace Horo::Editor::AudioPreviewTest
