#include "Horo/Debug/ConsoleProduct.h"
#include "Horo/Debug/ConsoleTelemetryBindings.h"
#include "Horo/Foundation/Telemetry/Telemetry.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <mutex>

using namespace Horo;
using namespace Horo::Debug;

namespace {
    class EventSink final : public Telemetry::ISink {
    public:
        void Export(const Telemetry::Record &record, const Telemetry::InstrumentDescriptor *) override {
            std::lock_guard lock(mutex);
            records.push_back(record);
        }

        void Flush() override {}

        std::vector<Telemetry::Record> Snapshot() {
            std::lock_guard lock(mutex);
            return records;
        }

    private:
        std::mutex mutex;
        std::vector<Telemetry::Record> records;
    };

    struct TelemetryLifetime final {
        TelemetryLifetime() {
            (void)Telemetry::Runtime::Shutdown();
        }

        ~TelemetryLifetime() {
            (void)Telemetry::Runtime::Shutdown();
        }
    };

    class AuditedQuery final : public ICommandHandler {
    public:
        int calls{};

        Result<DebugCommandOutput> Execute(const DebugCommandContext &, std::span<const ArgumentValue>, const CancellationToken &,
                                           const JobExecutionContext *) override {
            ++calls;
            return Result<DebugCommandOutput>::Success({"safe"});
        }
    };
}  // namespace

TEST_CASE("Console audit uses real telemetry admission and excludes sensitive values and ambient context", "[console][telemetry]") {
    TelemetryLifetime lifetime;
    auto audit = CreateConsoleTelemetryAudit();
    const std::array<DebugArgumentDescriptor, 1> arguments{{{"token", {}, DebugArgumentType::String, true, {}, true}}};
    const std::array descriptors{DebugCommandDescriptor{1, "game.secret", "Test audit boundary", "game.secret <token>", arguments,
                                                        CommandPermission::Public, CommandThreadPolicy::ImmediateConsoleThread,
                                                        CommandAvailability::ShippingAllowlist, CommandFlags::AuditLogged}};
    auto created = CommandRegistry::Create(descriptors, {});
    REQUIRE(created.HasValue());
    auto registry = std::make_shared<CommandRegistry>(std::move(created).Value());
    auto handler = std::make_shared<AuditedQuery>();
    const std::array bindings{CommandBinding{1, handler}};
    JobSystem jobs(JobSystemConfig{.workerCount = 0});
    auto executor = CommandExecutor::Create(registry, bindings, jobs, std::make_shared<OperationStore>(2, 2), std::this_thread::get_id(),
                                            std::this_thread::get_id(), 2, audit);
    REQUIRE(executor.HasValue());
    CHECK(executor.Value()->Submit("game.secret private-token", {}).HasError());
    CHECK(handler->calls == 0);
#if HORO_ENABLE_TELEMETRY
    auto sink = std::make_shared<EventSink>();
    REQUIRE(Telemetry::Runtime::Initialize({}, sink));
    {
        Log::LogContext ambient("account", "private-account");
        REQUIRE(executor.Value()->Submit("game.secret private-token", {}).HasValue());
    }
    REQUIRE(Telemetry::Runtime::Flush(std::chrono::seconds{2}));
    CHECK(handler->calls == 1);
    const auto records = sink->Snapshot();
    REQUIRE(records.size() == 1);
    CHECK(records[0].context.Fields().empty());
    CHECK(records[0].subsystem == "runtime.console");
    const auto &event = std::get<Telemetry::DiagnosticEvent>(records[0].payload);
    CHECK(event.name == "command.authorized");
    REQUIRE(event.fields.size() == 2);
    CHECK(std::get<std::uint64_t>(event.fields[0].value) == 1);
    const auto &text = std::get<std::string>(event.fields[1].value);
    CHECK(text.find("private-token") == std::string::npos);
    CHECK(text.find("[REDACTED]") != std::string::npos);
    Telemetry::Runtime::SetEnabled(false);
    CHECK(executor.Value()->Submit("game.secret private-token", {}).HasError());
    CHECK(handler->calls == 1);
#endif
}

TEST_CASE("Console metrics queries authoritative availability without exposing instrument metadata", "[console][telemetry][profile]") {
    TelemetryLifetime lifetime;
    RegistryPolicy policy;
    policy.profile = CompiledConsoleProfile();
    policy.limits.outputBytes = 512;
    const auto catalog = ProductConsoleDescriptors();
    const auto descriptor = std::ranges::find(catalog, MetricsCommandId, &DebugCommandDescriptor::id);
    const auto inventory =
        descriptor == catalog.end() ? std::span<const DebugCommandDescriptor>{} : std::span<const DebugCommandDescriptor>{&*descriptor, 1};
    auto created = CommandRegistry::Create(inventory, policy);
    REQUIRE(created.HasValue());
    auto registry = std::make_shared<CommandRegistry>(std::move(created).Value());
    auto bindings = BindConsoleTelemetryCommands(registry);
    REQUIRE(bindings.HasValue());
    if (policy.profile == ProductProfile::Shipping) {
        CHECK(bindings.Value().empty());
        CHECK(registry->Prepare("sys.metrics", {3}).HasError());
        return;
    }
    REQUIRE(bindings.Value().size() == 1);
#if HORO_ENABLE_TELEMETRY
    auto sink = std::make_shared<EventSink>();
    REQUIRE(Telemetry::Runtime::Initialize({}, sink));
    auto instrument = Telemetry::Runtime::RegisterGauge({.name = "private_descriptor_name", .subsystem = "private_subsystem"});
    REQUIRE(Telemetry::Runtime::SetAvailability(instrument, Telemetry::MetricAvailabilityState::UnsupportedByBackend));
#endif
    JobSystem jobs(JobSystemConfig{.workerCount = 0});
    auto operations = std::make_shared<OperationStore>(2, 2);
    auto executor =
        CommandExecutor::Create(registry, bindings.Value(), jobs, operations, std::this_thread::get_id(), std::this_thread::get_id());
    REQUIRE(executor.HasValue());
    auto submitted = executor.Value()->Submit("sys.metrics", {3});
    REQUIRE(submitted.HasValue());
    REQUIRE(submitted.Value().operation);
    REQUIRE(executor.Value()->DrainOwner(1).HasValue());
    const auto result = operations->SnapshotIfChanged(0);
    REQUIRE(result);
    const auto operation = std::ranges::find(result->operations, *submitted.Value().operation, &OperationRecord::id);
    REQUIRE(operation != result->operations.end());
    REQUIRE(operation->state == OperationState::Succeeded);
    const auto &text = operation->message;
    CHECK(text.size() <= 512);
    CHECK(text.find("generation=") != std::string::npos);
    CHECK(text.find("private_descriptor_name") == std::string::npos);
    CHECK(text.find("private_subsystem") == std::string::npos);
#if HORO_ENABLE_TELEMETRY
    CHECK(text.find("instrument=" + std::to_string(instrument.InstrumentId())) != std::string::npos);
    CHECK(text.find("state=unsupported_by_backend") != std::string::npos);
#endif
}
