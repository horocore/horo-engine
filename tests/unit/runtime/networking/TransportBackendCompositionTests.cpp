#include "Horo/Network/DeterministicTransport.h"
#include "Horo/Network/NetworkErrors.h"
#include "Horo/Network/TransportBackendComposition.h"
#include "NetworkTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace Horo::Network {
    namespace {
        using TestSupport::RequireError;

        struct Counters final {
            std::size_t created{};
            std::size_t cancelled{};
            std::size_t shutdown{};
        };

        class CountingInstance final : public ITransportBackendInstance {
        public:
            explicit CountingInstance(Counters &counters) noexcept : counters_(counters) {}

            void RequestCancellation() noexcept override {
                ++counters_.cancelled;
            }

            void Shutdown() noexcept override {
                ++counters_.shutdown;
            }

        private:
            Counters &counters_;
        };

        TransportCapabilities Capabilities() {
            TransportCapabilities result{};
            result.revision = 1;
            result.delivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = TransportSupport::Available;
            result.maximumChannels = 2;
            result.maximumMessageBytes = 32;
            return result;
        }

        TransportBackendDescriptor Descriptor(const char *id, Counters &counters, const bool supported = true,
                                              const bool configured = true) {
            return {{id}, supported, configured, Capabilities(), [&counters]() -> Result<TransportBackendInstance> {
                ++counters.created;
                return Result<TransportBackendInstance>::Success(std::make_unique<CountingInstance>(counters));
            }};
        }

        DeterministicTransportDescriptor NullDescriptor() {
            return {.mode = DeterministicTransportMode::RejectAll,
                    .maximumScheduledDeliveries = 1,
                    .maximumPayloadBytes = 32,
                    .maximumChannels = 2,
                    .budgetCapacity = {1, 1, 32},
                    .budgetPolicy = {.revision = 1,
                                     .maximumActiveConnections = 1,
                                     .maximumQueuedMessages = 1,
                                     .maximumQueuedBytes = 32,
                                     .maximumQueuedMessagesPerConnection = 1,
                                     .maximumQueuedBytesPerConnection = 32,
                                     .maximumMessagesPerTick = 1,
                                     .maximumBytesPerTick = 32,
                                     .maximumMessagesPerConnectionPerTick = 1,
                                     .maximumBytesPerConnectionPerTick = 32,
                                     .saturationGraceTicks = 1},
                    .scenario = {.revision = 1, .seed = 1, .maximumFragmentBytes = 32}};
        }

        TEST_CASE("Transport composition selects only explicitly installed and configured backends",
                  "[unit][network][transport-composition]") {
            Counters nullCounters{};
            Counters productionCounters{};
            TransportBackendComposition composition;
            const TransportBackendId nullId{"null"};
            const TransportBackendId productionId{"gns"};
            REQUIRE(composition.Register(Descriptor("null", nullCounters)).HasValue());
            REQUIRE(composition.Seal().HasValue());
            REQUIRE(composition.InstalledIds().size() == 1);
            REQUIRE(composition.Status(nullId).Value().installed);
            REQUIRE_FALSE(composition.Status(productionId).Value().installed);
            RequireError(composition.Select(productionId), NetworkErrors::TransportBackendUnavailable);
            REQUIRE(nullCounters.created == 0);
            REQUIRE(productionCounters.created == 0);
            REQUIRE_FALSE(composition.Status(nullId).Value().selected);
            REQUIRE(composition.Select(nullId).HasValue());
            REQUIRE(composition.Status(nullId).Value().selected);
            REQUIRE_FALSE(composition.Status(nullId).Value().active);
            REQUIRE(nullCounters.created == 0);
            REQUIRE(composition.Activate().HasValue());
            REQUIRE(composition.Status(nullId).Value().active);
            REQUIRE(nullCounters.created == 1);
            RequireError(composition.Select(productionId), NetworkErrors::TransportBackendConflict);
            REQUIRE(productionCounters.created == 0);
            composition.Shutdown();
            REQUIRE(nullCounters.cancelled == 1);
            REQUIRE(nullCounters.shutdown == 1);
        }

        TEST_CASE("Transport composition reports distinct installed support and configuration states without factory calls",
                  "[unit][network][transport-composition][availability]") {
            Counters unsupported{};
            Counters unconfigured{};
            TransportBackendComposition composition;
            REQUIRE(composition.Register(Descriptor("unsupported", unsupported, false)).HasValue());
            REQUIRE(composition.Register(Descriptor("unconfigured", unconfigured, true, false)).HasValue());
            REQUIRE(composition.Seal().HasValue());
            const auto supportStatus = composition.Status({"unsupported"}).Value();
            REQUIRE(supportStatus.installed);
            REQUIRE_FALSE(supportStatus.hostSupported);
            REQUIRE(supportStatus.configured);
            const auto configurationStatus = composition.Status({"unconfigured"}).Value();
            REQUIRE(configurationStatus.installed);
            REQUIRE(configurationStatus.hostSupported);
            REQUIRE_FALSE(configurationStatus.configured);
            RequireError(composition.Select({"unsupported"}), NetworkErrors::TransportBackendUnsupported);
            RequireError(composition.Select({"unconfigured"}), NetworkErrors::TransportBackendNotConfigured);
            REQUIRE(unsupported.created == 0);
            REQUIRE(unconfigured.created == 0);
            REQUIRE(composition.Lifecycle() == TransportBackendCompositionState::Sealed);
        }

        TEST_CASE("Transport composition rejects malformed duplicate over-capacity and premature operations",
                  "[unit][network][transport-composition][failure]") {
            Counters counters{};
            TransportBackendComposition composition;
            RequireError(composition.Select({"null"}), NetworkErrors::TransportBackendInvalid);
            RequireError(composition.Activate(), NetworkErrors::TransportBackendInvalid);
            RequireError(composition.Register(Descriptor("INVALID", counters)), NetworkErrors::TransportBackendInvalid);
            auto invalidCapability = Descriptor("invalid", counters);
            invalidCapability.capabilities.revision = 0;
            RequireError(composition.Register(std::move(invalidCapability)), NetworkErrors::TransportBackendInvalid);
            REQUIRE(composition.InstalledIds().empty());
            REQUIRE(composition.Register(Descriptor("same", counters)).HasValue());
            RequireError(composition.Register(Descriptor("same", counters)), NetworkErrors::TransportBackendConflict);
            for (std::size_t index = 1; index < TransportBackendComposition::MaximumBackends; ++index) {
                const auto id = std::string{"backend-"} + std::to_string(index);
                REQUIRE(composition.Register(Descriptor(id.c_str(), counters)).HasValue());
            }
            RequireError(composition.Register(Descriptor("overflow", counters)), NetworkErrors::TransportBackendCapacityExceeded);
            REQUIRE(composition.InstalledIds().size() == TransportBackendComposition::MaximumBackends);
            REQUIRE(composition.Seal().HasValue());
            RequireError(composition.Register(Descriptor("late", counters)), NetworkErrors::TransportBackendConflict);
            RequireError(composition.Status({"BAD ID"}), NetworkErrors::TransportBackendInvalid);
            REQUIRE(counters.created == 0);
        }

        TEST_CASE("Transport composition activation failures preserve selected inactive state",
                  "[unit][network][transport-composition][failure]") {
            TransportBackendComposition composition;
            auto descriptor = TransportBackendDescriptor{{"failing"}, true, true, Capabilities(), []() -> Result<TransportBackendInstance> {
                return Result<TransportBackendInstance>::Success(TransportBackendInstance{});
            }};
            REQUIRE(composition.Register(std::move(descriptor)).HasValue());
            REQUIRE(composition.Seal().HasValue());
            REQUIRE(composition.Select({"failing"}).HasValue());
            RequireError(composition.Activate(), NetworkErrors::TransportBackendFactoryFailed);
            REQUIRE(composition.Status({"failing"}).Value().selected);
            REQUIRE_FALSE(composition.Status({"failing"}).Value().active);
            composition.BeginCancellation();
            RequireError(composition.Activate(), NetworkErrors::TransportBackendCancelled);
            composition.Shutdown();
            REQUIRE(composition.Lifecycle() == TransportBackendCompositionState::Closed);
        }

        TEST_CASE("Transport composition cancellation and shutdown are idempotent and terminal",
                  "[unit][network][transport-composition][lifecycle]") {
            Counters counters{};
            TransportBackendComposition composition;
            REQUIRE(composition.Register(Descriptor("null", counters)).HasValue());
            REQUIRE(composition.Seal().HasValue());
            REQUIRE(composition.Select({"null"}).HasValue());
            REQUIRE(composition.Activate().HasValue());
            composition.BeginCancellation();
            composition.BeginCancellation();
            REQUIRE(counters.cancelled == 1);
            RequireError(composition.Select({"null"}), NetworkErrors::TransportBackendCancelled);
            composition.Shutdown();
            composition.Shutdown();
            REQUIRE(counters.shutdown == 1);
            REQUIRE_FALSE(composition.Status({"null"}).Value().active);
            RequireError(composition.Seal(), NetworkErrors::TransportBackendShuttingDown);
            RequireError(composition.Register(Descriptor("late", counters)), NetworkErrors::TransportBackendShuttingDown);
            REQUIRE(counters.created == 1);
        }

        TEST_CASE("Headless Null factory creates a real bounded deterministic transport lifetime",
                  "[unit][network][transport-composition][null]") {
            TransportBackendComposition composition;
            const auto nullDescriptor = NullDescriptor();
            REQUIRE(composition
                        .Register({{"null"}, true, true, Capabilities(), [nullDescriptor] {
                return CreateDeterministicTransportBackend(nullDescriptor);
            }}).HasValue());
            REQUIRE(composition.Seal().HasValue());
            RequireError(composition.Select({"gns"}), NetworkErrors::TransportBackendUnavailable);
            REQUIRE(composition.Select({"null"}).HasValue());
            REQUIRE(composition.Activate().HasValue());
            composition.BeginCancellation();
            composition.Shutdown();
            REQUIRE(composition.Lifecycle() == TransportBackendCompositionState::Closed);
        }
    }  // namespace
}  // namespace Horo::Network
