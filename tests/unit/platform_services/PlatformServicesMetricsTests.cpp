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
                if (std::ranges::none_of(descriptors_, [descriptor](const auto &observedDescriptor) {
                    return observedDescriptor.name == descriptor->name;
                }))
                    descriptors_.push_back(*descriptor);
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

            [[nodiscard]] std::vector<Telemetry::InstrumentDescriptor> Descriptors() const {
                std::lock_guard lock(mutex_);
                return descriptors_;
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
            std::vector<Telemetry::InstrumentDescriptor> descriptors_;
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

        /** @brief Checks the exported privacy allowlist and finite series budget independently of recorded values. */
        void CheckPlatformMetricDescriptors(const MetricCaptureSink &sink) {
            constexpr std::array<std::string_view, 7> names{"horo.platform_services.request.lifecycle",
                                                            "horo.platform_services.request.queue_admission",
                                                            "horo.platform_services.request.retry_scheduled",
                                                            "horo.platform_services.request.throttled",
                                                            "horo.platform_services.capability.checks",
                                                            "horo.platform_services.session.checks",
                                                            "horo.platform_services.frontend.shutdown"};
            constexpr std::array<std::string_view, 7> services{"achievements", "leaderboards_and_stats",
                                                               "cloud",        "presence",
                                                               "friends",      "session",
                                                               "unknown"};
            constexpr std::array<std::string_view, 25> outcomes{"accepted",
                                                                "rejected",
                                                                "succeeded",
                                                                "failed",
                                                                "cancelled",
                                                                "timed_out",
                                                                "shutdown",
                                                                "capacity_rejected",
                                                                "shutdown_rejected",
                                                                "invalid_configuration",
                                                                "available",
                                                                "unavailable",
                                                                "null_provider",
                                                                "policy_denied",
                                                                "frontend_closed",
                                                                "invalid_service",
                                                                "allowed",
                                                                "stale_session",
                                                                "stale_access_policy",
                                                                "consent_required",
                                                                "access_denied",
                                                                "access_restricted",
                                                                "access_revoked",
                                                                "access_unavailable",
                                                                "inactive"};
            const auto descriptors = sink.Descriptors();
            REQUIRE(descriptors.size() == names.size());
            for (const auto &descriptor : descriptors) {
                CHECK(std::ranges::find(names, descriptor.name) != names.end());
                CHECK(descriptor.subsystem == "platform_services");
                CHECK(descriptor.kind == Telemetry::InstrumentKind::Counter);
                CHECK(descriptor.unit == Telemetry::MetricUnit::Count);
                REQUIRE(descriptor.dimensions.size() <= 2);
                std::size_t cardinality = 1;
                for (const auto &dimension : descriptor.dimensions) {
                    REQUIRE((dimension.key == "service" || dimension.key == "outcome"));
                    REQUIRE_FALSE(dimension.allowedValues.empty());
                    for (const auto &value : dimension.allowedValues) {
                        if (dimension.key == "service")
                            CHECK(std::ranges::find(services, value) != services.end());
                        else
                            CHECK(std::ranges::find(outcomes, value) != outcomes.end());
                    }
                    cardinality *= dimension.allowedValues.size();
                }
                CHECK(descriptor.maxSeries == cardinality);
                CHECK(descriptor.maxSeries <= 63);
            }
        }

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
        CheckPlatformMetricDescriptors(*sink);
    }

    TEST_CASE("Platform Services metrics rebind after the observability runtime restarts",
              "[platform-services][frontend][metrics][lifecycle]") {
        static_cast<void>(Telemetry::Runtime::Shutdown());
        std::uint32_t previousGeneration{};
        for (unsigned lifetime = 0; lifetime < 2; ++lifetime) {
            const auto sink = std::make_shared<MetricCaptureSink>();
            REQUIRE(Telemetry::Runtime::Initialize({.queueCapacity = 256, .enabled = true}, sink));
            [[maybe_unused]] const TelemetryShutdown telemetryShutdown;
            const auto generation = Telemetry::Runtime::GetDiagnosticSnapshot().runtimeGeneration;
            REQUIRE(generation != 0);
            CHECK(generation != previousGeneration);
            previousGeneration = generation;
            auto backend = std::make_shared<RoutingBackend>();
            const auto session = Session();
            auto frontend = Frontend(backend, session);
            const auto accepted = frontend.UnlockAchievement({*session.Subject(), {1}});
            REQUIRE(accepted.HasValue());
            REQUIRE(backend->requests.MarkRunning(accepted.Value()).HasValue());
            REQUIRE(backend->requests.CompleteSuccess(accepted.Value()).HasValue());
            REQUIRE(frontend.Close().HasValue());
            REQUIRE(Telemetry::Runtime::Flush(std::chrono::seconds{2}));
            CHECK(sink->Value("horo.platform_services.request.lifecycle", "outcome", "accepted") == 1.0);
            CHECK(sink->Value("horo.platform_services.request.lifecycle", "outcome", "succeeded") == 1.0);
            CHECK(sink->Value("horo.platform_services.frontend.shutdown", "outcome", "succeeded") == 1.0);
        }
    }
}  // namespace Horo::PlatformServices
