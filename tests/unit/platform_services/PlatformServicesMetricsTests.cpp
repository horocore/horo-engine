#include "PlatformServicesFrontendTestSupport.h"

namespace Horo::PlatformServices {
    namespace {
        class MetricCaptureSink final : public Telemetry::ISink {
        public:
            void Export(const Telemetry::Record &record, const Telemetry::InstrumentDescriptor *descriptor) override {
                {
                    std::unique_lock lock(exportMutex_);
                    if (holdNextExport_) {
                        exportEntered_ = true;
                        exportReady_.notify_all();
                        exportReady_.wait(lock, [this] {
                            return !holdNextExport_;
                        });
                    }
                }
                if (record.Kind() != Telemetry::RecordKind::Metric || descriptor == nullptr)
                    return;

                const auto *metric = std::get_if<Telemetry::MetricRecord>(&record.payload);
                if (metric == nullptr)
                    return;

                MetricSeries observed{.name = descriptor->name, .value = metric->value};
                for (std::size_t index = 0; index < metric->dimensionCount; ++index) {
                    if (index >= descriptor->dimensions.size())
                        return;
                    const auto &dimension = descriptor->dimensions[index];
                    const std::uint16_t valueId = metric->dimensionValueIds[index];
                    if (valueId == 0 || valueId > dimension.allowedValues.size())
                        return;
                    observed.dimensions.emplace_back(dimension.key, dimension.allowedValues[valueId - 1]);
                }

                std::lock_guard lock(mutex_);
                const auto existing = std::ranges::find_if(series_, [&observed](const MetricSeries &candidate) {
                    return candidate.name == observed.name && candidate.dimensions == observed.dimensions;
                });
                if (existing == series_.end())
                    series_.push_back(std::move(observed));
                else
                    existing->value += observed.value;
            }

            void Flush() override {}

            void HoldNextExport() {
                std::lock_guard lock(exportMutex_);
                holdNextExport_ = true;
                exportEntered_ = false;
            }

            [[nodiscard]] bool WaitForHeldExport(const std::chrono::milliseconds timeout) {
                std::unique_lock lock(exportMutex_);
                return exportReady_.wait_for(lock, timeout, [this] {
                    return exportEntered_;
                });
            }

            void ReleaseExport() {
                {
                    std::lock_guard lock(exportMutex_);
                    holdNextExport_ = false;
                }
                exportReady_.notify_all();
            }

            [[nodiscard]] double Value(const std::string_view name, const std::string_view key = {},
                                       const std::string_view value = {}) const {
                std::lock_guard lock(mutex_);
                double total{};
                for (const MetricSeries &series : series_) {
                    if (series.name != name)
                        continue;
                    if (!key.empty() && std::ranges::none_of(series.dimensions, [key, value](const auto &dimension) {
                        return dimension.first == key && dimension.second == value;
                    }))
                        continue;
                    total += series.value;
                }
                return total;
            }

        private:
            struct MetricSeries final {
                std::string name;
                std::vector<std::pair<std::string, std::string>> dimensions;
                double value{};
            };

            mutable std::mutex mutex_;
            std::vector<MetricSeries> series_;
            std::mutex exportMutex_;
            std::condition_variable exportReady_;
            bool holdNextExport_{};
            bool exportEntered_{};
        };

        struct MetricExportRelease final {
            MetricCaptureSink &sink;

            ~MetricExportRelease() {
                sink.ReleaseExport();
            }
        };

        [[nodiscard]] bool HoldWriterAtFirstExport(MetricCaptureSink &sink) {
            sink.HoldNextExport();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            bool markerAccepted{};
            do {
                markerAccepted = Telemetry::Runtime::EmitRecord(
                    {.subsystem = "platform_services", .payload = Telemetry::LogRecord{.message = "test export gate"}});
                if (!markerAccepted)
                    std::this_thread::yield();
            } while (!markerAccepted && std::chrono::steady_clock::now() < deadline);
            return markerAccepted && sink.WaitForHeldExport(std::chrono::seconds{2});
        }

        struct TelemetryShutdown final {
            ~TelemetryShutdown() {
                static_cast<void>(Telemetry::Runtime::Shutdown());
            }
        };

    }  // namespace

    TEST_CASE("Platform Services metrics report bounded request, queue, capability, session and shutdown outcomes",
              "[platform-services][frontend][metrics]") {
        static_cast<void>(Telemetry::Runtime::Shutdown());
        const auto sink = std::make_shared<MetricCaptureSink>();
        REQUIRE(Telemetry::Runtime::Initialize({.queueCapacity = 256, .enabled = true}, sink));
        [[maybe_unused]] const TelemetryShutdown telemetryShutdown;
        [[maybe_unused]] const MetricExportRelease exportRelease{*sink};
        REQUIRE(HoldWriterAtFirstExport(*sink));

        auto backend = std::make_shared<RoutingBackend>(1);
        const auto session = Session();
        const auto subject = *session.Subject();
        auto frontend = Frontend(backend, session);

        auto successful = frontend.UnlockAchievement({subject, {1}});
        REQUIRE(successful.HasValue());
        auto handle = std::move(successful).Value();
        REQUIRE(backend->requests.MarkRunning(handle).HasValue());
        REQUIRE(backend->requests.RecordThrottled(handle).HasValue());
        REQUIRE(backend->requests.RecordRetryScheduled(handle).HasValue());
        CHECK(backend->requests.Query(handle).Value().state == PlatformRequestState::Running);
        REQUIRE(backend->requests.CompleteSuccess(handle).HasValue());

        RequireError(frontend.UnlockAchievement({subject, {}}), FrontendErrors::InvalidRequest);
        const auto oldSession = Session({7}, {4}, PlatformSessionPhase::Active, PlatformSessionAccessState::Granted, std::byte{2});
        RequireError(frontend.UnlockAchievement({*oldSession.Subject(), {1}}), PlatformSessionErrors::StaleSession);

        REQUIRE(backend->requests.Admit<int>().HasValue());
        RequireError(frontend.UnlockAchievement({subject, {1}}), RequestErrors::CapacityExceeded);
        REQUIRE(frontend.Close().HasValue());
        sink->ReleaseExport();
        REQUIRE(Telemetry::Runtime::Flush(std::chrono::seconds{2}));

        CHECK(sink->Value("horo.platform_services.request.lifecycle", "outcome", "accepted") == 1.0);
        CHECK(sink->Value("horo.platform_services.request.lifecycle", "outcome", "succeeded") == 1.0);
        CHECK(sink->Value("horo.platform_services.request.lifecycle", "outcome", "rejected") == 3.0);
        CHECK(sink->Value("horo.platform_services.request.lifecycle", "outcome", "shutdown") == 1.0);
        CHECK(sink->Value("horo.platform_services.request.queue_admission", "outcome", "accepted") == 2.0);
        CHECK(sink->Value("horo.platform_services.request.queue_admission", "outcome", "capacity_rejected") == 1.0);
        CHECK(sink->Value("horo.platform_services.request.retry_scheduled") == 1.0);
        CHECK(sink->Value("horo.platform_services.request.throttled") == 1.0);
        CHECK(sink->Value("horo.platform_services.capability.checks", "outcome", "available") == 3.0);
        CHECK(sink->Value("horo.platform_services.session.checks", "outcome", "allowed") == 2.0);
        CHECK(sink->Value("horo.platform_services.session.checks", "outcome", "stale_session") == 1.0);
        CHECK(sink->Value("horo.platform_services.frontend.shutdown", "outcome", "succeeded") == 1.0);
    }
}  // namespace Horo::PlatformServices
