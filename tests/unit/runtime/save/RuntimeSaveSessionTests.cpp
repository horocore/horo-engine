#include "Horo/Runtime/Save/RuntimeSaveSession.h"
#include "SaveCaptureSnapshotTestUtils.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace Horo::Runtime {
    namespace {
        using namespace CaptureTestSupport;

        struct Value final {
            std::byte value{0x31};
            std::thread::id owner{std::this_thread::get_id()};
            std::vector<std::string> published;
            int captures{};
        };

        struct Store final {
            std::mutex mutex;
            RuntimeSaveSnapshot snapshot;
        };

        class Writer final : public IRuntimeSaveWriter {
        public:
            explicit Writer(std::shared_ptr<Store> store) : store(std::move(store)) {}

            Result<void> Prepare(const RuntimeSaveSnapshot &snapshot, const CancellationToken &token) override {
                preparedThread = std::this_thread::get_id();
                if (started) {
                    started->set_value();
                    while (release.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready &&
                           !token.IsCancellationRequested()) {
                    }
                }
                if (failPrepare)
                    return Result<void>::Failure(Error{ErrorCode{"game.save.prepare_failed"}});
                candidate = snapshot;
                return Result<void>::Success();
            }

            Result<void> Commit() override {
                ++commits;
                if (failCommit)
                    return Result<void>::Failure(Error{ErrorCode{"game.save.commit_uncertain"}});
                std::lock_guard lock(store->mutex);
                store->snapshot = candidate;
                return Result<void>::Success();
            }

            std::shared_ptr<Store> store;
            RuntimeSaveSnapshot candidate;
            std::thread::id preparedThread;
            std::shared_ptr<std::promise<void>> started;
            std::shared_future<void> release;
            bool failPrepare{};
            bool failCommit{};
            int commits{};
        };

        class Prepared final : public ICanonicalRestorePreparedState {};

        class Receipt final : public IStagedRestoreParticipant {
        public:
            Receipt(StagedRestoreParticipantRequirement requirement, std::byte value, std::shared_ptr<Value> live, bool fail)
                : requirement_(std::move(requirement)), saved_(value), live_(std::move(live)), fail_(fail) {}

            const StagedRestoreParticipantRequirement &Requirement() const noexcept override {
                return requirement_;
            }

            Result<void> Decode(const StagedRestoreContext &) override {
                return Check();
            }

            Result<void> Validate(const StagedRestoreContext &) override {
                return Check();
            }

            Result<void> Instantiate(const StagedRestoreContext &) override {
                return Check();
            }

            const ICanonicalRestorePreparedState *PreparedState() const noexcept override {
                return &prepared_;
            }

            Result<void> ApplyState(const ICanonicalRestoreDependencyLookup &) override {
                if (fail_)
                    return Result<void>::Failure(Error{ErrorCode{"game.restore.invalid_state"}});
                return Check();
            }

            Result<void> FixupReferences(const ICanonicalRestoreDependencyLookup &) override {
                return Check();
            }

            void PublishPrepared() noexcept override {
                live_->value = saved_;
                live_->published.push_back(requirement_.participant.Value());
            }

            void RollbackPrepared() noexcept override {
                rolledBack = true;
            }

            bool rolledBack{};

        private:
            Result<void> Check() const {
                return std::this_thread::get_id() == live_->owner ? Result<void>::Success()
                                                                  : Result<void>::Failure(MakeError(SaveErrors::ThreadAffinityViolation));
            }

            StagedRestoreParticipantRequirement requirement_;
            std::byte saved_;
            std::shared_ptr<Value> live_;
            bool fail_{};
            Prepared prepared_;
        };

        class Source final : public IRuntimeSaveRestoreSource {
        public:
            Source(std::shared_ptr<Store> store, std::shared_ptr<Value> value) : store_(std::move(store)), value_(std::move(value)) {}

            Result<RuntimeSaveRestoreStaging> Stage(const StagedRestoreContext &, const SaveParticipantRegistrySnapshot &,
                                                    const CancellationToken &) override {
                stagedThread = std::this_thread::get_id();
                RuntimeSaveSnapshot snapshot;
                {
                    std::lock_guard lock(store_->mutex);
                    snapshot = store_->snapshot;
                }
                if (!snapshot.IsValid())
                    return Result<RuntimeSaveRestoreStaging>::Failure(MakeError(SaveErrors::CompositionObjectMissing));
                RuntimeSaveRestoreStaging staging;
                // Reverse order proves the existing canonical restore planner, not input ordering, controls publication.
                for (auto it = snapshot.Records().rbegin(); it != snapshot.Records().rend(); ++it) {
                    const auto &record = it->Record();
                    staging.participants.push_back(
                        std::make_unique<Receipt>(StagedRestoreParticipantRequirement{record.participant, record.schemaVersion,
                                                                                      SaveParticipantScope::RuntimeScene},
                                                  it->Segment(0)[0], value_, fail));
                }
                return Result<RuntimeSaveRestoreStaging>::Success(std::move(staging));
            }

            bool fail{};
            std::thread::id stagedThread;

        private:
            std::shared_ptr<Store> store_;
            std::shared_ptr<Value> value_;
        };

        class TestClock final : public Clock {
        public:
            std::function<void()> onSample;

            Duration MonotonicNow() const override {
                if (onSample)
                    onSample();
                return {};
            }
        };

        struct Fixture final {
            TestClock clock;
            JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 4}};
            CanonicalStateParticipantRegistry registry;
            std::shared_ptr<Value> value = std::make_shared<Value>();
            std::shared_ptr<Store> store = std::make_shared<Store>();
            std::unique_ptr<SaveCaptureBarrier> barrier;
            SaveParticipantRegistrySnapshot participants;
            RuntimeSaveCaptureProvenance provenance;
            SaveRuntimeGeneration generation;
            std::unique_ptr<RuntimeSaveSession> session;
            std::array<std::size_t, 5> owners{};

            Fixture() {
                barrier = SaveCaptureBarrier::Create(1390, clock, 8).Value();
                const std::array names{"game.b", "host.jobs", "host.scene", "host.subsystem", "game.a"};
                const std::array domains{SaveBarrierDomain::FixedSimulation, SaveBarrierDomain::Jobs, SaveBarrierDomain::SceneMutation,
                                         SaveBarrierDomain::Subsystem, SaveBarrierDomain::Subsystem};
                for (std::size_t i = 0; i < names.size(); ++i) {
                    owners[i] = barrier->Register(Participant(names[i]), domains[i]).Value();
                    REQUIRE(barrier->PublishReadiness(owners[i], {.value = 41}).HasValue());
                }
                for (const auto &[name, suffix] : std::array{std::pair{"game.b", 2}, std::pair{"game.a", 1}}) {
                    const auto record = Test::Id<SaveRecordId>(suffix);
                    auto descriptor = Descriptor(name, {record});
                    descriptor.roles = SaveParticipantRole::Capture | SaveParticipantRole::Restore;
                    Register(registry, descriptor,
                             std::make_shared<CallbackCaptureAdapter>(
                                 [live = value, record](const CanonicalCaptureContext &, ICanonicalCaptureSink &sink) {
                        if (std::this_thread::get_id() != live->owner)
                            return Result<CanonicalCaptureDisposition>::Failure(MakeError(SaveErrors::ThreadAffinityViolation));
                        ++live->captures;
                        auto written = sink.WriteCopied(record, std::span(&live->value, 1));
                        if (written.HasError())
                            return Result<CanonicalCaptureDisposition>::Failure(written.ErrorValue());
                        return Result<CanonicalCaptureDisposition>::Success(CanonicalCaptureDisposition::Captured);
                    }, std::make_shared<int>()));
                }
                participants = registry.Snapshot().Value();
                provenance = Provenance(participants);
                generation = {1, provenance.sceneIncarnation, participants.Generation()};
                session = RuntimeSaveSession::Create(jobs, *barrier, participants, generation).Value();
                value->published.reserve(8);
            }

            ~Fixture() {
                session.reset();
                jobs.Shutdown(ShutdownPolicy::Cancel);
            }

            SaveOperationDescriptor Operation(OperationId id, SaveOperationKind kind) {
                return {.operation = id, .kind = kind, .maximumCompletionCallbacks = 4};
            }

            Result<void> Pump() {
                return session->CommitAtSafePoint(RuntimePhase::CommitDeferredLifecycleChanges, generation, provenance);
            }

            void Finish(const RuntimeSaveRequest &request) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                while ((!request.Operation().Snapshot()->IsTerminal() || !session->IsSettled()) &&
                       std::chrono::steady_clock::now() < deadline) {
                    auto pumped = Pump();
                    if (pumped.HasError() && !request.Operation().Snapshot()->IsTerminal())
                        FAIL(pumped.ErrorValue().message);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                REQUIRE(request.Operation().Snapshot()->IsTerminal());
                REQUIRE(session->IsSettled());
            }

            RuntimeSaveRequest Save(OperationId id = 1) {
                auto saved = session->Save(Operation(id, SaveOperationKind::Save), std::make_shared<Writer>(store));
                REQUIRE(saved.HasValue());
                return std::move(saved).Value();
            }
        };

        TEST_CASE("Runtime save foundation composes real participants capture worker processing and owner-only restore",
                  "[save][session]") {
            Fixture fixture;
            auto writer = std::make_shared<Writer>(fixture.store);
            auto saved = fixture.session->Save(fixture.Operation(1, SaveOperationKind::Save), writer).Value();
            std::atomic<int> terminals{};
            REQUIRE(saved.Operation()
                        .OnCompletion([&](const SaveOperationSnapshot &) {
                ++terminals;
            }).HasValue());
            CHECK(fixture.value->captures == 0);
            CHECK(fixture.session->Save(fixture.Operation(2, SaveOperationKind::Save), writer).HasError());
            RequireError(fixture.session->CommitAtSafePoint(RuntimePhase::FixedUpdate, fixture.generation, fixture.provenance),
                         SaveErrors::SafePointInvalid);
            CHECK(fixture.value->captures == 0);
            fixture.Finish(saved);
            CHECK(saved.Operation().Snapshot()->state == SaveOperationState::Completed);
            CHECK(terminals == 1);
            CHECK(writer->preparedThread != std::this_thread::get_id());
            CHECK(writer->commits == 1);
            CHECK(fixture.value->captures == 2);
            REQUIRE(fixture.session->Acknowledge().HasValue());
            auto mutation = fixture.barrier->BeginMutation(fixture.owners[0]).Value();
            fixture.value->value = std::byte{0xff};
            REQUIRE(fixture.barrier->EndMutation(mutation, fixture.provenance.epoch).HasValue());
            auto source = std::make_shared<Source>(fixture.store, fixture.value);
            auto loaded = fixture.session->Load(fixture.Operation(2, SaveOperationKind::Load), source).Value();
            CHECK(fixture.value->value == std::byte{0xff});
            fixture.Finish(loaded);
            CHECK(loaded.Operation().Snapshot()->state == SaveOperationState::Completed);
            CHECK(source->stagedThread != std::this_thread::get_id());
            CHECK(fixture.value->value == std::byte{0x31});
            CHECK(fixture.value->published == std::vector<std::string>{"game.a", "game.b"});
        }

        TEST_CASE("Runtime save foundation defers actual mutation and cancels without capture", "[save][session]") {
            Fixture fixture;
            const auto mutation = fixture.barrier->BeginMutation(fixture.owners[0]).Value();
            auto request = fixture.Save();
            REQUIRE(fixture.Pump().HasValue());
            CHECK(fixture.value->captures == 0);
            CHECK(!request.Operation().Snapshot()->IsTerminal());
            CHECK(request.RequestCancellation() == SaveCancellationRequestResult::Requested);
            fixture.Finish(request);
            CHECK(request.Operation().Snapshot()->state == SaveOperationState::Cancelled);
            REQUIRE(fixture.session->Acknowledge().HasValue());
            REQUIRE(fixture.barrier->EndMutation(mutation, fixture.provenance.epoch).HasValue());
            auto successor = fixture.Save(2);
            fixture.Finish(successor);
            CHECK(successor.Operation().Snapshot()->state == SaveOperationState::Completed);
        }

        TEST_CASE("Runtime save foundation preserves preparation and uncertain commit errors exactly once", "[save][session]") {
            for (const bool commit : {false, true}) {
                Fixture fixture;
                auto writer = std::make_shared<Writer>(fixture.store);
                writer->failPrepare = !commit;
                writer->failCommit = commit;
                auto request = fixture.session->Save(fixture.Operation(1, SaveOperationKind::Save), writer).Value();
                std::atomic<int> terminals{};
                REQUIRE(request.Operation()
                            .OnCompletion([&](const SaveOperationSnapshot &) {
                    ++terminals;
                }).HasValue());
                fixture.Finish(request);
                const auto snapshot = request.Operation().Snapshot().value();
                CHECK(snapshot.state == SaveOperationState::Failed);
                CHECK(snapshot.terminalError->code.Value() == (commit ? "game.save.commit_uncertain" : "game.save.prepare_failed"));
                CHECK(snapshot.commit == (commit ? SaveOperationCommitOutcome::Unknown : SaveOperationCommitOutcome::NotCommitted));
                CHECK(terminals == 1);
            }
        }

        TEST_CASE("Runtime save foundation stages no live mutation on restore failure or generation retirement", "[save][session]") {
            for (const bool stale : {false, true}) {
                Fixture fixture;
                auto saved = fixture.Save();
                fixture.Finish(saved);
                REQUIRE(fixture.session->Acknowledge().HasValue());
                fixture.value->value = std::byte{0xfe};
                auto source = std::make_shared<Source>(fixture.store, fixture.value);
                source->fail = !stale;
                auto request = fixture.session->Load(fixture.Operation(2, SaveOperationKind::Load), source).Value();
                if (stale) {
                    auto next = fixture.generation;
                    ++next.scene;
                    RequireError(fixture.session->CommitAtSafePoint(RuntimePhase::CommitDeferredLifecycleChanges, next),
                                 SaveErrors::GenerationStale);
                    fixture.jobs.Shutdown(ShutdownPolicy::Cancel);
                } else
                    fixture.Finish(request);
                CHECK(request.Operation().Snapshot()->IsTerminal());
                CHECK(fixture.value->value == std::byte{0xfe});
                CHECK(fixture.value->published.empty());
                if (!stale)
                    CHECK(request.Operation().Snapshot()->terminalError->code.Value() == "game.restore.invalid_state");
            }
        }

        TEST_CASE("Runtime save foundation retirement pins worker inputs and settles without capturing its session", "[save][session]") {
            Fixture fixture;
            auto writer = std::make_shared<Writer>(fixture.store);
            writer->started = std::make_shared<std::promise<void>>();
            auto started = writer->started->get_future();
            std::promise<void> release;
            writer->release = release.get_future().share();
            auto request = fixture.session->Save(fixture.Operation(1, SaveOperationKind::Save), writer).Value();
            REQUIRE(fixture.Pump().HasValue());
            REQUIRE(started.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
            REQUIRE(fixture.session->Retire().HasValue());
            fixture.session.reset();
            release.set_value();
            fixture.jobs.Shutdown(ShutdownPolicy::Cancel);
            CHECK(request.Operation().Snapshot()->state == SaveOperationState::Cancelled);
            CHECK(writer->commits == 0);
        }

        TEST_CASE("Runtime save admission fences actual host clock reentry and resets after a clock failure", "[save][session]") {
            for (const bool clockFailure : {false, true}) {
                Fixture fixture;
                auto writer = std::make_shared<Writer>(fixture.store);
                int callbacks{};
                fixture.clock.onSample = [&] {
                    ++callbacks;
                    RequireError(fixture.session->Retire(), SaveErrors::LifecycleReentrant);
                    RequireError(fixture.session->Acknowledge(), SaveErrors::LifecycleReentrant);
                    RequireError(fixture.session->Save(fixture.Operation(9, SaveOperationKind::Save), writer),
                                 SaveErrors::LifecycleReentrant);
                    if (clockFailure)
                        throw std::runtime_error("injected host clock failure");
                };
                auto submitted = fixture.session->Save(fixture.Operation(1, SaveOperationKind::Save), writer);
                fixture.clock.onSample = {};
                REQUIRE(submitted.HasValue());
                auto request = std::move(submitted).Value();
                REQUIRE(request.Operation().IsValid());
                CHECK(callbacks == 1);
                if (clockFailure) {
                    REQUIRE(request.Operation().Snapshot()->state == SaveOperationState::Failed);
                    CHECK(request.Operation().Snapshot()->terminalError->code.Value() == SaveErrors::LifecycleCallbackFailed.code.Value());
                } else {
                    CHECK(fixture.barrier->Snapshot().Value().operation == request.Operation().Id());
                    CHECK(fixture.barrier->Snapshot().Value().state == SaveBarrierState::Pending);
                    fixture.Finish(request);
                    CHECK(request.Operation().Snapshot()->state == SaveOperationState::Completed);
                }
                REQUIRE(fixture.session->Acknowledge().HasValue());
                auto next = fixture.Save(2);
                fixture.Finish(next);
                CHECK(next.Operation().Snapshot()->state == SaveOperationState::Completed);
            }
        }

        TEST_CASE("Runtime save foundation null input and foreign admission use the same typed contract", "[save][session]") {
            Fixture fixture;
            auto source = std::make_shared<Source>(fixture.store, fixture.value);
            auto request = fixture.session->Load(fixture.Operation(1, SaveOperationKind::Load), source).Value();
            fixture.Finish(request);
            CHECK(request.Operation().Snapshot()->terminalError->code.Value() == SaveErrors::CompositionObjectMissing.code.Value());
            REQUIRE(fixture.session->Acknowledge().HasValue());
            std::optional<Error> error;
            std::thread foreign([&] {
                auto result = fixture.session->Load(fixture.Operation(2, SaveOperationKind::Load), source);
                if (result.HasError())
                    error = result.ErrorValue();
            });
            foreign.join();
            REQUIRE(error);
            CHECK(error->code.Value() == SaveErrors::ThreadAffinityViolation.code.Value());
        }
    }  // namespace
}  // namespace Horo::Runtime
