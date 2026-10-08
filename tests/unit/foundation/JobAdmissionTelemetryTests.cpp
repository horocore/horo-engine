#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/Logging/LogContext.h"
#include "Horo/Foundation/Telemetry/Telemetry.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

namespace {
    using namespace Horo;

    /** @brief Parks the dispatcher outside its ingestion lock so this test can verify both emitted records without contention drops. */
    class AdmissionSink final : public Telemetry::ISink {
    public:
        void Export(const Telemetry::Record &record, const Telemetry::InstrumentDescriptor *) override {
            std::lock_guard lock(mutex_);
            records_.push_back(record);
        }

        void Flush() override {
            std::unique_lock lock(mutex_);
            paused_ = true;
            changed_.notify_all();
            if (!changed_.wait_for(lock, std::chrono::seconds{10}, [this] {
                return released_;
            }))
                pauseExpired_ = true;
        }

        /** @brief Waits for actual dispatcher entry, not an assumed scheduling delay. */
        [[nodiscard]] bool WaitUntilPaused() {
            std::unique_lock lock(mutex_);
            return changed_.wait_for(lock, std::chrono::seconds{5}, [this] {
                return paused_;
            }) && !pauseExpired_;
        }

        /** @brief Releases all current/future sink flush calls before bounded telemetry shutdown. */
        [[nodiscard]] bool Release() {
            std::lock_guard lock(mutex_);
            released_ = true;
            changed_.notify_all();
            return !pauseExpired_;
        }

        [[nodiscard]] std::vector<Telemetry::Record> Records() const {
            std::lock_guard lock(mutex_);
            return records_;
        }

    private:
        mutable std::mutex mutex_;
        std::condition_variable changed_;
        bool paused_{};
        bool released_{};
        bool pauseExpired_{};
        std::vector<Telemetry::Record> records_;
    };

    /** @brief Owns the dispatcher pause and releases it even when a REQUIRE unwinds the test. */
    class AdmissionTelemetryFixture final {
    public:
        AdmissionTelemetryFixture() = default;
        AdmissionTelemetryFixture(const AdmissionTelemetryFixture &) = delete;
        AdmissionTelemetryFixture &operator=(const AdmissionTelemetryFixture &) = delete;
        AdmissionTelemetryFixture(AdmissionTelemetryFixture &&) = delete;
        AdmissionTelemetryFixture &operator=(AdmissionTelemetryFixture &&) = delete;

        ~AdmissionTelemetryFixture() {
            static_cast<void>(sink_->Release());
            static_cast<void>(Telemetry::Runtime::Shutdown());
        }

        /** @brief Starts the explicitly owned runtime and waits for its queue-lock-free dispatch boundary. */
        [[nodiscard]] bool Start() const {
            return Telemetry::Runtime::Initialize(Telemetry::Configuration{.sinkFlushInterval = std::chrono::milliseconds{1}}, sink_) &&
                   sink_->WaitUntilPaused();
        }

        /** @brief Resumes delivery and waits for all accepted records before inspection. */
        [[nodiscard]] bool Drain() const {
            const bool pauseHeld = sink_->Release();
            return Telemetry::Runtime::Flush() && pauseHeld;
        }

        [[nodiscard]] std::vector<Telemetry::Record> Records() const {
            return sink_->Records();
        }

    private:
        const std::shared_ptr<AdmissionSink> sink_{std::make_shared<AdmissionSink>()};
    };
}  // namespace

TEST_CASE("Overload counters and structured diagnostic events preserve submitter correlation", "[foundation][jobs][admission]") {
    AdmissionTelemetryFixture telemetry;
    REQUIRE(telemetry.Start());
    JobSystemConfig config{.workerCount = 0, .maxQueuedJobs = 0};
    config.priorityQueues[2].overloadPolicy = JobOverloadPolicy::Shed;
    JobSystem jobs{config};
    {
        const Log::LogContext context{"correlation.id", "admission-test"};
        const auto noOpJob = [](const CancellationToken &) {
            // Deliberately inert: both submissions must be rejected before callback execution.
        };
        REQUIRE(jobs.Submit({.priority = JobPriority::Background}, noOpJob).HasError());
        REQUIRE(jobs.Submit({.priority = JobPriority::Background, .requirement = JobRequirement::Optional}, noOpJob).HasError());
    }
    REQUIRE(telemetry.Drain());
    const auto records = telemetry.Records();
    REQUIRE(records.size() == 2);
    const std::array<std::string_view, 2> outcomes{"job.queue_full", "job.queue_shed"};
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto *event = std::get_if<Telemetry::DiagnosticEvent>(&records[index].payload);
        REQUIRE(event != nullptr);
        CHECK(event->name == "job.queue_overload");
        REQUIRE(event->fields.size() == 4);
        CHECK(std::get<std::uint64_t>(event->fields[0].value) == 2);
        CHECK(std::get<std::string>(event->fields[3].value) == outcomes[index]);
        REQUIRE(records[index].context.Fields().size() == 1);
        CHECK(records[index].context.Fields()[0] == Log::MdcField{"correlation.id", "admission-test"});
    }
    CHECK(jobs.AdmissionSnapshot().rejected[2] == 2);
    CHECK(jobs.AdmissionSnapshot().shed[2] == 1);
}
