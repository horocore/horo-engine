#include "PlatformServicesMetrics.h"

#include "Horo/Foundation/Telemetry/Telemetry.h"
#include "Horo/PlatformServices/PlatformUserSession.h"

#include <array>
#include <memory>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::PlatformServices {
    namespace {
        using namespace Telemetry;

        constexpr std::array<std::string_view, static_cast<std::size_t>(PlatformServiceKind::Count) + 1>
            ServiceMetricValues{"achievements", "leaderboards_and_stats", "cloud", "presence", "friends", "session", "unknown"};
        constexpr std::array<std::string_view, 7> RequestMetricOutcomes{"accepted",  "rejected",  "succeeded", "failed",
                                                                        "cancelled", "timed_out", "shutdown"};
        constexpr std::array<std::string_view, 4> QueueMetricOutcomes{"accepted", "capacity_rejected", "shutdown_rejected",
                                                                      "invalid_configuration"};
        constexpr std::array<std::string_view, 6> CapabilityMetricOutcomes{"available",     "unavailable",     "null_provider",
                                                                           "policy_denied", "frontend_closed", "invalid_service"};
        constexpr std::array<std::string_view, 9> SessionMetricOutcomes{"allowed",          "stale_session",      "stale_access_policy",
                                                                        "consent_required", "access_denied",      "access_restricted",
                                                                        "access_revoked",   "access_unavailable", "inactive"};
        constexpr std::array<std::string_view, 2> ShutdownMetricOutcomes{"succeeded", "failed"};

        template <std::size_t Size>
        [[nodiscard]] DimensionDescriptor MetricDimension(std::string key, const std::array<std::string_view, Size> &values) {
            DimensionDescriptor dimension{.key = std::move(key)};
            dimension.allowedValues.reserve(values.size());
            for (const std::string_view value : values)
                dimension.allowedValues.emplace_back(value);
            return dimension;
        }

        [[nodiscard]] InstrumentDescriptor CounterDescriptor(std::string name, std::string description,
                                                             std::vector<DimensionDescriptor> dimensions = {},
                                                             const std::uint32_t maxSeries = 1) {
            return {.kind = InstrumentKind::Counter,
                    .name = std::move(name),
                    .subsystem = "platform_services",
                    .unit = MetricUnit::Count,
                    .description = std::move(description),
                    .dimensions = std::move(dimensions),
                    .maxSeries = maxSeries};
        }

        [[nodiscard]] std::string_view ServiceMetricName(const PlatformServiceKind service) noexcept {
            const std::size_t index = static_cast<std::size_t>(service);
            return index < static_cast<std::size_t>(PlatformServiceKind::Count) ? ServiceMetricValues[index] : "unknown";
        }

        struct PlatformMetricHandles final {
            Counter request;
            Counter queueAdmissions;
            Counter retries;
            Counter throttles;
            Counter capabilities;
            Counter sessions;
            Counter shutdown;
        };

        class PlatformMetricRegistry final {
        public:
            [[nodiscard]] PlatformMetricHandles CurrentHandles() {
                const auto currentGeneration = Runtime::GetDiagnosticSnapshot().runtimeGeneration;
                {
                    std::shared_lock lock(mutex_);
                    if (currentGeneration == generation_)
                        return handles_;
                }

                std::unique_lock lock(mutex_);
                const std::uint32_t confirmedGeneration = Runtime::GetDiagnosticSnapshot().runtimeGeneration;
                if (confirmedGeneration != generation_) {
                    generation_ = confirmedGeneration;
                    handles_ = {};
                    if (generation_ != 0)
                        RegisterHandles();
                }
                return handles_;
            }

        private:
            void RegisterHandles() {
                try {
                    handles_.request =
                        Runtime::RegisterCounter(CounterDescriptor("horo.platform_services.request.lifecycle",
                                                                   "Accepted, rejected, and terminal Platform Services requests.",
                                                                   {MetricDimension("outcome", RequestMetricOutcomes)},
                                                                   static_cast<std::uint32_t>(RequestMetricOutcomes.size())));
                    handles_.queueAdmissions =
                        Runtime::RegisterCounter(CounterDescriptor("horo.platform_services.request.queue_admission",
                                                                   "Accepted and rejected bounded request-store admissions.",
                                                                   {MetricDimension("outcome", QueueMetricOutcomes)},
                                                                   static_cast<std::uint32_t>(QueueMetricOutcomes.size())));
                    handles_.retries = Runtime::RegisterCounter(CounterDescriptor("horo.platform_services.request.retry_scheduled",
                                                                                  "Retries scheduled by the owning Horo policy."));
                    handles_.throttles = Runtime::RegisterCounter(
                        CounterDescriptor("horo.platform_services.request.throttled", "Normalized provider throttling outcomes."));
                    handles_.capabilities = Runtime::RegisterCounter(
                        CounterDescriptor("horo.platform_services.capability.checks", "Bounded Platform Services capability checks.",
                                          {MetricDimension("service", ServiceMetricValues),
                                           MetricDimension("outcome", CapabilityMetricOutcomes)},
                                          static_cast<std::uint32_t>(ServiceMetricValues.size() * CapabilityMetricOutcomes.size())));
                    handles_.sessions = Runtime::RegisterCounter(
                        CounterDescriptor("horo.platform_services.session.checks", "Bounded Platform Services session-access checks.",
                                          {MetricDimension("service", ServiceMetricValues),
                                           MetricDimension("outcome", SessionMetricOutcomes)},
                                          static_cast<std::uint32_t>(ServiceMetricValues.size() * SessionMetricOutcomes.size())));
                    handles_.shutdown = Runtime::RegisterCounter(
                        CounterDescriptor("horo.platform_services.frontend.shutdown", "Platform Services frontend shutdown outcomes.",
                                          {MetricDimension("outcome", ShutdownMetricOutcomes)},
                                          static_cast<std::uint32_t>(ShutdownMetricOutcomes.size())));
                } catch (const std::bad_alloc &) {
                    // Telemetry registration is best-effort and must not change service admission or shutdown.
                }
            }

            // Serializes process-lifetime handle registration/rebinding. Handles are copied under the lock and recorded after release.
            // Runtime-generation matching fences stale handles across telemetry restart; no request or provider state is retained here.
            std::shared_mutex mutex_;
            std::uint32_t generation_{};
            PlatformMetricHandles handles_;
        };

        [[nodiscard]] PlatformMetricRegistry &PlatformMetrics() {
            static PlatformMetricRegistry registry;
            return registry;
        }

        void AddBoundMetric(const Counter &root, const std::string_view key, const std::string_view value,
                            const std::uint64_t delta = 1) noexcept {
            const std::array selection{DimensionValue{.key = key, .value = value}};
            try {
                root.WithDimensions(selection).Add(delta);
            } catch (const std::bad_alloc &) {
                // A metric series allocation failure must not affect the owning request.
            }
        }

        void AddBoundMetric(const Counter &root, const std::string_view firstKey, const std::string_view firstValue,
                            const std::string_view secondKey, const std::string_view secondValue) noexcept {
            const std::array selection{DimensionValue{.key = firstKey, .value = firstValue},
                                       DimensionValue{.key = secondKey, .value = secondValue}};
            try {
                root.WithDimensions(selection).Add();
            } catch (const std::bad_alloc &) {
                // A metric series allocation failure must not affect the owning request.
            }
        }
    }  // namespace

    namespace Detail {
        void RegisterPlatformServicesMetricDescriptors() {
            static_cast<void>(PlatformMetrics().CurrentHandles());
        }

        void RecordPlatformRequestMetric(const PlatformRequestMetricOutcome outcome, const std::uint64_t delta) noexcept {
            constexpr std::array<std::string_view, 7> values{"accepted",  "rejected",  "succeeded", "failed",
                                                             "cancelled", "timed_out", "shutdown"};
            const std::size_t index = static_cast<std::size_t>(outcome);
            if (index >= values.size() || delta == 0)
                return;
            AddBoundMetric(PlatformMetrics().CurrentHandles().request, "outcome", values[index], delta);
        }

        void RecordPlatformRequestQueueMetric(const PlatformRequestQueueMetricOutcome outcome) noexcept {
            constexpr std::array<std::string_view, 4> values{"accepted", "capacity_rejected", "shutdown_rejected", "invalid_configuration"};
            const std::size_t index = static_cast<std::size_t>(outcome);
            if (index >= values.size())
                return;
            AddBoundMetric(PlatformMetrics().CurrentHandles().queueAdmissions, "outcome", values[index]);
        }

        void RecordPlatformRetryScheduledMetric() noexcept {
            PlatformMetrics().CurrentHandles().retries.Add();
        }

        void RecordPlatformThrottledMetric() noexcept {
            PlatformMetrics().CurrentHandles().throttles.Add();
        }

        void RecordPlatformCapabilityMetric(const PlatformServiceKind service, const PlatformCapabilityMetricOutcome outcome) noexcept {
            constexpr std::array<std::string_view, 6> values{"available",     "unavailable",     "null_provider",
                                                             "policy_denied", "frontend_closed", "invalid_service"};
            const std::size_t index = static_cast<std::size_t>(outcome);
            if (index >= values.size())
                return;
            AddBoundMetric(PlatformMetrics().CurrentHandles().capabilities, "service", ServiceMetricName(service), "outcome",
                           values[index]);
        }

        void RecordPlatformSessionMetric(const PlatformServiceKind service, const PlatformSessionMetricOutcome outcome) noexcept {
            constexpr std::array<std::string_view, 9> values{"allowed",          "stale_session",      "stale_access_policy",
                                                             "consent_required", "access_denied",      "access_restricted",
                                                             "access_revoked",   "access_unavailable", "inactive"};
            const std::size_t index = static_cast<std::size_t>(outcome);
            if (index >= values.size())
                return;
            AddBoundMetric(PlatformMetrics().CurrentHandles().sessions, "service", ServiceMetricName(service), "outcome", values[index]);
        }

        void RecordPlatformShutdownMetric(const PlatformShutdownMetricOutcome outcome) noexcept {
            constexpr std::array<std::string_view, 2> values{"succeeded", "failed"};
            const std::size_t index = static_cast<std::size_t>(outcome);
            if (index >= values.size())
                return;
            AddBoundMetric(PlatformMetrics().CurrentHandles().shutdown, "outcome", values[index]);
        }
    }  // namespace Detail
}  // namespace Horo::PlatformServices
