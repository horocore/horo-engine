#include "EditorConsoleExecution.h"
#include "EditorConsoleLaunch.h"
#include "Horo/Application/NetworkDebugger.h"
#include "Horo/Foundation/Telemetry/Telemetry.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <initializer_list>
#include <thread>

using namespace Horo;
using namespace Horo::Debug;
using namespace Horo::Application::Internal;

namespace {
    /** @brief Owns only this test's real process telemetry lifetime; no diagnostic store is introduced. */
    struct HostTelemetryLifetime final {
        HostTelemetryLifetime() {
            static_cast<void>(Telemetry::Runtime::Shutdown());
        }

        ~HostTelemetryLifetime() {
            static_cast<void>(Telemetry::Runtime::Shutdown());
        }
    };

    /** @brief Discards exported test records while the real telemetry runtime owns availability and instrument identities. */
    class HostTelemetrySink final : public Telemetry::ISink {
    public:
        void Export(const Telemetry::Record &, const Telemetry::InstrumentDescriptor *) override {}

        void Flush() override {}
    };

    /** @brief Keeps native argv storage alive while inspecting the host's intentionally borrowed token slice. */
    struct Arguments final {
        std::vector<std::string> storage;
        std::vector<char *> argv;

        explicit Arguments(std::initializer_list<std::string_view> input) {
            storage.reserve(input.size());
            for (const auto value : input)
                storage.emplace_back(value);
            for (auto &value : storage)
                argv.push_back(value.data());
        }
    };

    /** @brief Reuses the exact Editor-profile schema composed by the process root. */
    ConfigurationSchema HostSchema() {
        auto descriptors = ConsoleConfigurationDescriptors(ProductProfile::Editor);
        REQUIRE(descriptors.HasValue());
        ConfigurationSchema schema;
        for (const auto &descriptor : descriptors.Value())
            REQUIRE(schema.Register(descriptor).HasValue());
        REQUIRE(schema.Seal().HasValue());
        return schema;
    }
}  // namespace

TEST_CASE("Editor console startup payloads cannot become renderer lighting or XR host options", "[console][startup][host]") {
    Arguments arguments{"HoroEditor",       "--renderer",           "opengl",    "--exec",
                        "--renderer=metal", "--dev-overlay=jobs",   "--project", "C:/project with spaces/\xc3\xb6",
                        "--console-script", "scripts/boot.console", "--exec",    "--xr-host-roots",
                        "--console"};
    auto captured = CaptureEditorConsoleLaunch(arguments.argv);
    REQUIRE(captured.HasValue());
    const auto &host = captured.Value().hostArguments;
    REQUIRE(host.size() == 5);
    CHECK(host[0] == arguments.argv[0]);
    CHECK(std::string_view(host[1]) == "--renderer");
    CHECK(std::string_view(host[2]) == "opengl");
    CHECK(std::string_view(host[3]) == "--project");
    CHECK(std::string_view(host[4]) == "C:/project with spaces/\xc3\xb6");
    const auto startup = captured.Value().console.Startup();
    REQUIRE(startup.size() == 3);
    CHECK(startup[0].text == "--renderer=metal");
    CHECK(startup[1].kind == ConsoleStartupKind::Script);
    CHECK(startup[1].text == "scripts/boot.console");
    CHECK(startup[2].text == "--xr-host-roots");
    arguments.storage[4] = "replaced invocation storage";
    CHECK(captured.Value().console.Startup()[0].text == "--renderer=metal");
}

TEST_CASE("Editor-owned values and inline options preserve token ownership ahead of console parsing", "[console][startup][host]") {
    for (const auto option : {"--project", "--lighting-root", "--color-look-root", "--shadow-quality", "--xr-host-catalog-signature"}) {
        Arguments arguments{"HoroEditor", option, "--console", "--renderer=opengl", "--exec", "version"};
        auto captured = CaptureEditorConsoleLaunch(arguments.argv);
        REQUIRE(captured.HasValue());
        REQUIRE(captured.Value().hostArguments.size() == 4);
        CHECK(captured.Value().hostArguments[1] == arguments.argv[1]);
        CHECK(captured.Value().hostArguments[2] == arguments.argv[2]);
        CHECK(std::string_view(captured.Value().hostArguments[3]) == "--renderer=opengl");
        CHECK(captured.Value().console.ConfigurationInputs().empty());
        REQUIRE(captured.Value().console.Startup().size() == 1);
        CHECK(captured.Value().console.Startup()[0].text == "version");
    }
    Arguments payload{"HoroEditor", "--exec", "--remote-console=localhost"};
    auto captured = CaptureEditorConsoleLaunch(payload.argv);
    REQUIRE(captured.HasValue());
    CHECK(captured.Value().console.Startup()[0].text == "--remote-console=localhost");
}

TEST_CASE("Editor console capture fails closed before any host parser or source merge", "[console][startup][host]") {
    for (const auto flag :
         {"--exec", "--console-script", "--remote-console", "--remote-console=localhost", "--console=1", "--dev-overlay"}) {
        Arguments arguments{"HoroEditor", flag};
        CHECK(CaptureEditorConsoleLaunch(arguments.argv).HasError());
    }
    std::array<char *, 1> invalid{nullptr};
    CHECK(CaptureEditorConsoleLaunch(invalid).HasError());
    CHECK(CaptureEditorConsoleLaunch({}).HasError());
    Arguments root{"HoroEditor"};
    std::vector<char *> tooMany(4097, root.argv[0]);
    CHECK(CaptureEditorConsoleLaunch(tooMany).HasError());
    Arguments consoleLimit{"HoroEditor", "--console"};
    std::vector<char *> oversized(258, consoleLimit.argv[1]);
    oversized[0] = consoleLimit.argv[0];
    CHECK(CaptureEditorConsoleLaunch(oversized).HasError());
}

TEST_CASE("Editor console Invocation merge rejects collisions atomically and uses the existing canonical resolver",
          "[console][configuration][host]") {
    Arguments arguments{"HoroEditor", "--console", "--dev-overlays"};
    auto captured = CaptureEditorConsoleLaunch(arguments.argv);
    REQUIRE(captured.HasValue());
    const SettingKey consoleKey{std::string(ConsoleEnabledSettingName)};
    const SettingKey overlayKey{std::string(OverlaysEnabledSettingName)};
    ConfigurationResolutionRequest sources;
    sources.invocation.emplace(overlayKey, ConfigurationInputValue{false, {}});
    sources.user.emplace(consoleKey, ConfigurationInputValue{false, {}});
    CHECK(MergeEditorConsoleInputs(captured.Value().console, sources).HasError());
    REQUIRE(sources.invocation.size() == 1);
    CHECK_FALSE(std::get<bool>(sources.invocation.at(overlayKey).value));
    CHECK_FALSE(sources.invocation.contains(consoleKey));
    REQUIRE(sources.user.size() == 1);
    sources.invocation.clear();
    REQUIRE(MergeEditorConsoleInputs(captured.Value().console, sources).HasValue());
    auto descriptors = ConsoleConfigurationDescriptors(ProductProfile::Editor);
    REQUIRE(descriptors.HasValue());
    ConfigurationSchema schema;
    for (const auto &descriptor : descriptors.Value())
        REQUIRE(schema.Register(descriptor).HasValue());
    REQUIRE(schema.Seal().HasValue());
    ConfigurationService service(std::move(schema));
    REQUIRE(service.ResolveAndCommit(sources).HasValue());
    auto launch = ResolveConsoleLaunch(captured.Value().console, service.Snapshot(), ProductProfile::Editor);
    REQUIRE(launch.HasValue());
    CHECK(launch.Value().console);
    CHECK(launch.Value().overlays);
    const auto snapshot = service.Snapshot();
    const auto *winner = snapshot.FindResolved(consoleKey);
    REQUIRE(winner);
    CHECK(winner->source == ConfigurationSource::Invocation);
}

TEST_CASE("Editor core console startup uses the same process operations and real owner/render adapter", "[console][startup][host]") {
    Arguments arguments{"HoroEditor", "--exec", "version", "--exec", "clear"};
    auto captured = CaptureEditorConsoleLaunch(arguments.argv);
    REQUIRE(captured.HasValue());
    auto configurationOwner = std::make_shared<ConfigurationService>(HostSchema());
    auto &configuration = *configurationOwner;
    ConfigurationResolutionRequest sources;
    REQUIRE(MergeEditorConsoleInputs(captured.Value().console, sources).HasValue());
    REQUIRE(configuration.ResolveAndCommit(sources).HasValue());
    auto launch = ResolveConsoleLaunch(captured.Value().console, configuration.Snapshot(), ProductProfile::Editor);
    REQUIRE(launch.HasValue());
    auto prepared = PrepareEditorConsole(launch.Value(), configuration.Snapshot());
    REQUIRE(prepared.HasValue());
    JobSystem jobs(JobSystemConfig{.workerCount = 0});
    auto operations = std::make_shared<OperationStore>(8, 8);
    CHECK(EditorConsoleExecution::Start(prepared.Value(), jobs, {}, configurationOwner, "Product Editor").HasError());
    auto started = EditorConsoleExecution::Start(prepared.Value(), jobs, operations, configurationOwner, "Product Editor");
    REQUIRE(started.HasValue());
    auto execution = std::move(started).Value();
    REQUIRE(execution->Accepted().size() == 2);
    REQUIRE(prepared.Value().history->Entries().size() == 2);
    CHECK(prepared.Value().history->Entries()[0].output == "Product Editor");
    REQUIRE(execution->Accepted()[1].operation);
    const auto clear = *execution->Accepted()[1].operation;
    const auto pending = operations->SnapshotIfChanged(0);
    REQUIRE(pending);
    REQUIRE(pending->operations.size() == 1);
    CHECK(pending->operations[0].id == clear);
    CHECK(pending->operations[0].state == OperationState::Queued);
    bool wrongThread{};
    std::thread other([&] {
        wrongThread = execution->RenderSafePoint().HasError();
    });
    other.join();
    CHECK(wrongThread);
    REQUIRE(execution->RenderSafePoint().HasValue());
    CHECK(prepared.Value().history->Entries().size() == 2);
    REQUIRE(execution->OwnerSafePoint(std::chrono::steady_clock::now()).HasValue());
    CHECK(prepared.Value().history->Entries().empty());
    const auto terminal = operations->SnapshotIfChanged(0);
    REQUIRE(terminal);
    REQUIRE(terminal->operations.size() == 1);
    CHECK(terminal->operations[0].id == clear);
    CHECK(terminal->operations[0].state == OperationState::Succeeded);
    CHECK(execution->Accepted()[1].operation == clear);
    execution->Stop();
    execution->Stop();
    CHECK(execution->OwnerSafePoint(std::chrono::steady_clock::now()).HasError());
    CHECK(EditorConsoleExecution::Start(prepared.Value(), jobs, operations, configurationOwner, "Product Editor").HasError());
}

TEST_CASE("Editor history consumes canonical changes only at the live owner boundary", "[console][configuration][host]") {
    const std::array<std::string_view, 0> arguments{};
    auto captured = ConsoleLaunchArguments::Parse(arguments);
    REQUIRE(captured.HasValue());
    auto configurationOwner = std::make_shared<ConfigurationService>(HostSchema());
    auto &configuration = *configurationOwner;
    const auto frozen = configuration.Snapshot();
    auto launch = ResolveConsoleLaunch(captured.Value(), frozen, ProductProfile::Editor);
    REQUIRE(launch.HasValue());
    auto prepared = PrepareEditorConsole(launch.Value(), frozen);
    REQUIRE(prepared.HasValue());
    JobSystem jobs(JobSystemConfig{.workerCount = 0});
    auto operations = std::make_shared<OperationStore>(8, 8);
    auto started = EditorConsoleExecution::Start(prepared.Value(), jobs, operations, configurationOwner, "Product Editor");
    REQUIRE(started.HasValue());
    auto execution = std::move(started).Value();
    auto &history = *prepared.Value().history;
    for (const auto command : {"a", "b", "c"})
        history.Append({1, {}, command}, "result");
    const auto publish = [&](const std::int64_t limit) {
        ConfigurationResolutionRequest sources;
        sources.session.emplace(SettingKey(std::string(ConsoleHistorySettingName)), ConfigurationInputValue{limit, {}});
        REQUIRE(configuration.ResolveAndCommit(sources).HasValue());
    };
    publish(2);
    REQUIRE(history.Entries().size() == 3);
    REQUIRE(execution->RenderSafePoint().HasValue());
    CHECK(history.Entries().size() == 3);
    bool wrongOwner{};
    std::thread other([&] {
        wrongOwner = execution->OwnerSafePoint({}).HasError();
    });
    other.join();
    CHECK(wrongOwner);
    CHECK(history.Entries().size() == 3);
    REQUIRE(execution->OwnerSafePoint({}).HasValue());
    REQUIRE(history.Entries().size() == 2);
    CHECK(history.Entries().front().command == "b");
    CHECK(ReadConsoleHistoryLimit(frozen).Value() == 500);
    publish(4);
    REQUIRE(execution->OwnerSafePoint({}).HasValue());
    history.Append({1, {}, "d"}, "result");
    history.Append({1, {}, "e"}, "result");
    REQUIRE(history.Entries().size() == 4);
    publish(0);
    CHECK(execution->OwnerSafePoint({}).HasError());
    CHECK(history.Entries().size() == 4);
    publish(1);
    REQUIRE(execution->OwnerSafePoint({}).HasValue());
    REQUIRE(history.Entries().size() == 1);
    CHECK(history.Entries().front().command == "e");
    execution->Stop();
    publish(4);
    CHECK(execution->OwnerSafePoint({}).HasError());
    history.Append({1, {}, "f"}, "result");
    REQUIRE(history.Entries().size() == 1);
    CHECK(history.Entries().front().command == "f");
}

TEST_CASE("Editor lazy console activation uses the current canonical history preference", "[console][configuration][host]") {
    const std::array<std::string_view, 0> arguments{};
    auto captured = ConsoleLaunchArguments::Parse(arguments);
    REQUIRE(captured.HasValue());
    auto configurationOwner = std::make_shared<ConfigurationService>(HostSchema());
    auto &configuration = *configurationOwner;
    auto launch = ResolveConsoleLaunch(captured.Value(), configuration.Snapshot(), ProductProfile::Editor);
    REQUIRE(launch.HasValue());
    auto prepared = PrepareEditorConsole(launch.Value(), configuration.Snapshot());
    REQUIRE(prepared.HasValue());
    ConfigurationResolutionRequest sources;
    sources.session.emplace(SettingKey(std::string(ConsoleHistorySettingName)), ConfigurationInputValue{std::int64_t{2}, {}});
    REQUIRE(configuration.ResolveAndCommit(sources).HasValue());
    JobSystem jobs(JobSystemConfig{.workerCount = 0});
    auto operations = std::make_shared<OperationStore>(8, 8);
    auto started = EditorConsoleExecution::Start(prepared.Value(), jobs, operations, configurationOwner, "Product Editor");
    REQUIRE(started.HasValue());
    for (const auto command : {"a", "b", "c"})
        prepared.Value().history->Append({1, {}, command}, "result");
    REQUIRE(prepared.Value().history->Entries().size() == 2);
    CHECK(prepared.Value().history->Entries().front().command == "b");
}

TEST_CASE("Editor history variable uses common admission and remains recoverable after a stale pending write",
          "[console][configuration][host]") {
    const std::array<std::string_view, 0> arguments{};
    auto captured = ConsoleLaunchArguments::Parse(arguments);
    REQUIRE(captured.HasValue());
    auto configuration = std::make_shared<ConfigurationService>(HostSchema());
    auto launch = ResolveConsoleLaunch(captured.Value(), configuration->Snapshot(), ProductProfile::Editor);
    REQUIRE(launch.HasValue());
    auto prepared = PrepareEditorConsole(launch.Value(), configuration->Snapshot());
    REQUIRE(prepared.HasValue());
    JobSystem jobs(JobSystemConfig{.workerCount = 0});
    auto operations = std::make_shared<OperationStore>(8, 8);
    CHECK(EditorConsoleExecution::Start(prepared.Value(), jobs, operations, {}, "Product Editor").HasError());
    auto started = EditorConsoleExecution::Start(prepared.Value(), jobs, operations, configuration, "Product Editor");
    REQUIRE(started.HasValue());
    auto execution = std::move(started).Value();
    const DebugCommandContext developer{3, false, false};
    auto &dispatcher = execution->Dispatcher();
    auto help = dispatcher.Submit("help", developer);
    REQUIRE(help.HasValue());
    CHECK(help.Value().output.text.find("console.history.limit") != std::string::npos);
    help = dispatcher.Submit("help", {});
    REQUIRE(help.HasValue());
    CHECK(help.Value().output.text.find("console.history.limit") == std::string::npos);
    CHECK(dispatcher.Submit("console.history.limit 2", {}).HasError());
    CHECK(dispatcher.Submit("console.history.limit 0", developer).HasError());
    CHECK_FALSE(operations->SnapshotIfChanged(0));
    auto accepted = dispatcher.Submit("console.history.limit 2", developer);
    REQUIRE(accepted.HasValue());
    REQUIRE(accepted.Value().operation);
    auto &history = *prepared.Value().history;
    for (const auto command : {"a", "b", "c"})
        history.Append({1, {}, command}, "result");
    CHECK(ReadConsoleHistoryLimit(configuration->Snapshot()).Value() == 500);
    REQUIRE(execution->OwnerSafePoint({}).HasValue());
    auto variable = execution->Variables().Read("console.history.limit", developer);
    REQUIRE(variable.HasValue());
    CHECK(std::get<std::int64_t>(variable.Value().active) == 500);
    REQUIRE(variable.Value().pending);
    CHECK(std::get<std::int64_t>(*variable.Value().pending) == 2);
    CHECK(history.Entries().size() == 3);
    bool wrongOwner{};
    std::thread other([&] {
        wrongOwner = execution->OwnerSafePoint({}).HasError();
    });
    other.join();
    CHECK(wrongOwner);
    CHECK(ReadConsoleHistoryLimit(configuration->Snapshot()).Value() == 500);
    REQUIRE(execution->RenderSafePoint().HasValue());
    CHECK(ReadConsoleHistoryLimit(configuration->Snapshot()).Value() == 500);
    REQUIRE(execution->OwnerSafePoint({}).HasValue());
    CHECK(ReadConsoleHistoryLimit(configuration->Snapshot()).Value() == 2);
    REQUIRE(history.Entries().size() == 2);
    CHECK(history.Entries().front().command == "b");
    CHECK_FALSE(execution->VariableActivationError());
    REQUIRE(dispatcher.Submit("console.history.limit 3", developer).HasValue());
    REQUIRE(execution->OwnerSafePoint({}).HasValue());
    ConfigurationDraft concurrent;
    concurrent.baseRevision = configuration->Snapshot().Revision();
    concurrent.proposedValues.emplace(SettingKey(std::string(ConsoleEnabledSettingName)), true);
    REQUIRE(configuration->Commit(concurrent).HasValue());
    REQUIRE(execution->OwnerSafePoint({}).HasValue());
    REQUIRE(execution->VariableActivationError());
    CHECK(execution->VariableActivationError()->code.Value() == "configuration.draft_stale");
    variable = execution->Variables().Read("console.history.limit", developer);
    REQUIRE(variable.HasValue());
    REQUIRE(variable.Value().pending);
    CHECK(std::get<std::int64_t>(variable.Value().active) == 2);
    CHECK(std::get<std::int64_t>(*variable.Value().pending) == 3);
    REQUIRE(dispatcher.Submit("console.history.limit 4", developer).HasValue());
    REQUIRE(execution->OwnerSafePoint({}).HasValue());
    REQUIRE(execution->OwnerSafePoint({}).HasValue());
    CHECK_FALSE(execution->VariableActivationError());
    CHECK(ReadConsoleHistoryLimit(configuration->Snapshot()).Value() == 4);
    history.Append({1, {}, "d"}, "result");
    history.Append({1, {}, "e"}, "result");
    CHECK(history.Entries().size() == 4);
    REQUIRE(dispatcher.Submit("console.history.limit 1", developer).HasValue());
    REQUIRE(execution->OwnerSafePoint({}).HasValue());
    execution->Stop();
    CHECK(execution->OwnerSafePoint({}).HasError());
    CHECK(ReadConsoleHistoryLimit(configuration->Snapshot()).Value() == 4);
    const std::weak_ptr<ConfigurationService> lifetime = configuration;
    configuration.reset();
    CHECK_FALSE(lifetime.expired());
    execution.reset();
    CHECK(lifetime.expired());
}

TEST_CASE("Editor visibility uses the Public session variable and bounded common owner admission", "[console][configuration][host]") {
    for (const bool initial : {false, true}) {
        const std::array<std::string_view, 1> arguments{"--console"};
        auto captured =
            ConsoleLaunchArguments::Parse(initial ? std::span<const std::string_view>(arguments) : std::span<const std::string_view>{});
        REQUIRE(captured.HasValue());
        auto configuration = std::make_shared<ConfigurationService>(HostSchema());
        ConfigurationResolutionRequest sources;
        sources.invocation = captured.Value().ConfigurationInputs();
        REQUIRE(configuration->ResolveAndCommit(sources).HasValue());
        const auto frozen = configuration->Snapshot();
        auto launch = ResolveConsoleLaunch(captured.Value(), frozen, ProductProfile::Editor);
        REQUIRE(launch.HasValue());
        auto prepared = PrepareEditorConsole(launch.Value(), frozen);
        REQUIRE(prepared.HasValue());
        JobSystem jobs(JobSystemConfig{.workerCount = 0});
        auto operations = std::make_shared<OperationStore>(1, 4);
        auto started = EditorConsoleExecution::Start(prepared.Value(), jobs, operations, configuration, "Product Editor");
        REQUIRE(started.HasValue());
        auto execution = std::move(started).Value();
        REQUIRE(execution->ConsoleVisible().HasValue());
        CHECK(execution->ConsoleVisible().Value() == initial);
        auto admitted = execution->RequestConsoleVisibility(!initial);
        REQUIRE(admitted.HasValue());
        REQUIRE(admitted.Value().operation);
        CHECK(execution->ConsoleVisible().Value() == initial);
        CHECK(execution->RequestConsoleVisibility(initial).HasError());
        const auto queued = operations->SnapshotIfChanged(0);
        REQUIRE(queued);
        REQUIRE(queued->operations.size() == 1);
        CHECK(queued->operations[0].id == *admitted.Value().operation);
        CHECK(queued->operations[0].state == OperationState::Queued);
        REQUIRE(execution->RenderSafePoint().HasValue());
        CHECK(execution->ConsoleVisible().Value() == initial);
        bool wrongOwner{};
        std::thread other([&] {
            wrongOwner = execution->OwnerSafePoint({}).HasError();
        });
        other.join();
        CHECK(wrongOwner);
        CHECK(execution->ConsoleVisible().Value() == initial);
        REQUIRE(execution->OwnerSafePoint({}).HasValue());
        CHECK(execution->ConsoleVisible().Value() == !initial);
        CHECK(configuration->Snapshot().Revision() == frozen.Revision());
        REQUIRE(execution->Dispatcher().Submit("console.visible true", {}).HasValue());
        REQUIRE(execution->OwnerSafePoint({}).HasValue());
        CHECK(execution->ConsoleVisible().Value());
        REQUIRE(execution->RequestConsoleVisibility(false).HasValue());
        execution->Stop();
        CHECK(execution->OwnerSafePoint({}).HasError());
        CHECK(execution->RequestConsoleVisibility(false).HasError());
        CHECK(execution->ConsoleVisible().Value());
        CHECK(configuration->Snapshot().Revision() == frozen.Revision());
    }
}

TEST_CASE("Editor process-store panel controls use registered Developer variables without granting unavailable sources",
          "[console][host][overlays]") {
    const std::array<std::string_view, 0> arguments{};
    auto captured = ConsoleLaunchArguments::Parse(arguments);
    REQUIRE(captured.HasValue());
    auto configuration = std::make_shared<ConfigurationService>(HostSchema());
    const auto frozen = configuration->Snapshot();
    auto launch = ResolveConsoleLaunch(captured.Value(), frozen, ProductProfile::Editor);
    REQUIRE(launch.HasValue());
    auto prepared = PrepareEditorConsole(launch.Value(), frozen);
    REQUIRE(prepared.HasValue());
    JobSystem jobs(JobSystemConfig{.workerCount = 0});
    auto operations = std::make_shared<OperationStore>(8, 8);
    const std::array requested{OverlaySource::Logs, OverlaySource::Frame};
    auto started = EditorConsoleExecution::Start(prepared.Value(), jobs, operations, configuration, "Product Editor", requested);
    REQUIRE(started.HasValue());
    auto execution = std::move(started).Value();
    const DebugCommandContext developer{3, false, false};
    CHECK(execution->StoreOverlayVisible(OverlaySource::Logs, developer).Value());
    CHECK_FALSE(execution->StoreOverlayVisible(OverlaySource::Jobs, developer).Value());
    CHECK(execution->StoreOverlayVisible(OverlaySource::Logs, {}).HasError());
    CHECK(execution->StoreOverlayVisible(OverlaySource::Frame, developer).HasError());
    CHECK(execution->StoreOverlayVisible(OverlaySource::Network, developer).HasError());
    CHECK(execution->RequestStoreOverlayVisibility(OverlaySource::Jobs, true, {}).HasError());
    CHECK(execution->RequestStoreOverlayVisibility(OverlaySource::Frame, true, developer).HasError());
    CHECK(execution->Dispatcher().Submit("overlay.frame.visible true", developer).HasError());
    CHECK_FALSE(operations->SnapshotIfChanged(0));
    const auto publicHelp = execution->Dispatcher().Submit("help", {});
    REQUIRE(publicHelp.HasValue());
    CHECK(publicHelp.Value().output.text.find("overlay.logs.visible") == std::string::npos);
    const auto accepted = execution->RequestStoreOverlayVisibility(OverlaySource::Logs, false, developer);
    REQUIRE(accepted.HasValue());
    REQUIRE(accepted.Value().operation);
    CHECK(execution->StoreOverlayVisible(OverlaySource::Logs, developer).Value());
    REQUIRE(execution->RenderSafePoint().HasValue());
    CHECK(execution->StoreOverlayVisible(OverlaySource::Logs, developer).Value());
    REQUIRE(execution->OwnerSafePoint({}).HasValue());
    CHECK_FALSE(execution->StoreOverlayVisible(OverlaySource::Logs, developer).Value());
    const auto complete = operations->SnapshotIfChanged(0);
    REQUIRE(complete);
    REQUIRE(complete->operations.size() == 1);
    CHECK(complete->operations[0].id == *accepted.Value().operation);
    CHECK(complete->operations[0].state == OperationState::Succeeded);
    REQUIRE(execution->Dispatcher().Submit("overlay.jobs.visible true", developer).HasValue());
    REQUIRE(execution->OwnerSafePoint({}).HasValue());
    CHECK(execution->StoreOverlayVisible(OverlaySource::Jobs, developer).Value());
    CHECK(configuration->Snapshot().Revision() == frozen.Revision());
    REQUIRE(execution->RequestStoreOverlayVisibility(OverlaySource::Logs, true, developer).HasValue());
    execution->Stop();
    CHECK(execution->OwnerSafePoint({}).HasError());
    CHECK_FALSE(execution->StoreOverlayVisible(OverlaySource::Logs, developer).Value());
    CHECK(execution->RequestStoreOverlayVisibility(OverlaySource::Jobs, false, developer).HasError());
}

TEST_CASE("Editor Scene panel visibility uses the same Developer queue and NextFrame activation", "[console][host][overlays][scene]") {
    const std::array<std::string_view, 0> arguments{};
    auto captured = ConsoleLaunchArguments::Parse(arguments);
    REQUIRE(captured.HasValue());
    auto configuration = std::make_shared<ConfigurationService>(HostSchema());
    auto launch = ResolveConsoleLaunch(captured.Value(), configuration->Snapshot(), ProductProfile::Editor);
    REQUIRE(launch.HasValue());
    auto preparation = PrepareEditorConsole(launch.Value(), configuration->Snapshot());
    REQUIRE(preparation.HasValue());
    JobSystem jobs(JobSystemConfig{.workerCount = 0});
    auto operations = std::make_shared<OperationStore>(8, 8);
    const std::array panels{OverlaySource::Scene};
    auto started = EditorConsoleExecution::Start(preparation.Value(), jobs, operations, configuration, "Product Editor", panels);
    REQUIRE(started.HasValue());
    auto execution = std::move(started).Value();
    const DebugCommandContext developer{3, false, false};
    CHECK_FALSE(execution->StoreOverlayVisible(OverlaySource::Scene, developer).Value());
    REQUIRE(execution->OwnerSafePoint({}).HasValue());
    CHECK(execution->StoreOverlayVisible(OverlaySource::Scene, developer).Value());
    CHECK(execution->RequestStoreOverlayVisibility(OverlaySource::Scene, false, {}).HasError());
    CHECK_FALSE(operations->SnapshotIfChanged(0));
    const auto accepted = execution->RequestStoreOverlayVisibility(OverlaySource::Scene, false, developer);
    REQUIRE(accepted.HasValue());
    REQUIRE(accepted.Value().operation);
    REQUIRE(execution->OwnerSafePoint({}).HasValue());
    CHECK(execution->StoreOverlayVisible(OverlaySource::Scene, developer).Value());
    const auto pending = execution->Variables().Read(ConsoleSceneOverlayVariableName, developer);
    REQUIRE(pending.HasValue());
    REQUIRE(pending.Value().pending);
    CHECK_FALSE(std::get<bool>(*pending.Value().pending));
    REQUIRE(execution->OwnerSafePoint({}).HasValue());
    CHECK_FALSE(execution->StoreOverlayVisible(OverlaySource::Scene, developer).Value());
}

TEST_CASE("Stopping either shared console facade prevents pending host configuration activation",
          "[console][configuration][shutdown][host]") {
    for (const bool throughAdapter : {false, true}) {
        const std::array<std::string_view, 0> arguments{};
        auto captured = ConsoleLaunchArguments::Parse(arguments);
        REQUIRE(captured.HasValue());
        auto configuration = std::make_shared<ConfigurationService>(HostSchema());
        auto launch = ResolveConsoleLaunch(captured.Value(), configuration->Snapshot(), ProductProfile::Editor);
        REQUIRE(launch.HasValue());
        auto prepared = PrepareEditorConsole(launch.Value(), configuration->Snapshot());
        REQUIRE(prepared.HasValue());
        JobSystem jobs(JobSystemConfig{.workerCount = 0});
        auto operations = std::make_shared<OperationStore>(8, 8);
        auto started = EditorConsoleExecution::Start(prepared.Value(), jobs, operations, configuration, "Product Editor");
        REQUIRE(started.HasValue());
        auto execution = std::move(started).Value();
        REQUIRE(execution->Dispatcher().Submit("console.history.limit 1", {3, false, false}).HasValue());
        REQUIRE(execution->OwnerSafePoint({}).HasValue());
        auto &history = *prepared.Value().history;
        history.Append({1, {}, "first"}, "result");
        history.Append({1, {}, "second"}, "result");
        const auto before = configuration->Snapshot();
        if (throughAdapter)
            execution->Stop();
        else
            execution->Dispatcher().Stop();
        const auto boundary = execution->OwnerSafePoint({});
        REQUIRE(boundary.HasError());
        CHECK(boundary.ErrorValue().code.Value() == "console.shutting_down");
        CHECK(execution->OwnerSafePoint({}).HasError());
        CHECK(configuration->Snapshot().Revision() == before.Revision());
        CHECK(ReadConsoleHistoryLimit(configuration->Snapshot()).Value() == 500);
        CHECK(history.Entries().size() == 2);
        const auto variable = execution->Variables().Read("console.history.limit", {3, false, false});
        REQUIRE(variable.HasValue());
        CHECK(std::get<std::int64_t>(variable.Value().active) == 500);
        REQUIRE(variable.Value().pending);
        CHECK(std::get<std::int64_t>(*variable.Value().pending) == 1);
    }
}

TEST_CASE("Editor metric command binds the actual process availability owner without initializing it", "[console][host][telemetry]") {
    HostTelemetryLifetime lifetime;
    const std::array<std::string_view, 0> arguments{};
    auto captured = ConsoleLaunchArguments::Parse(arguments);
    REQUIRE(captured.HasValue());
    auto configuration = std::make_shared<ConfigurationService>(HostSchema());
    auto launch = ResolveConsoleLaunch(captured.Value(), configuration->Snapshot(), ProductProfile::Editor);
    REQUIRE(launch.HasValue());
    auto prepared = PrepareEditorConsole(launch.Value(), configuration->Snapshot());
    REQUIRE(prepared.HasValue());
    JobSystem jobs(JobSystemConfig{.workerCount = 0});
    auto operations = std::make_shared<OperationStore>(8, 8);
    const auto before = Telemetry::Runtime::GetDiagnosticSnapshot();
    auto started = EditorConsoleExecution::Start(prepared.Value(), jobs, operations, configuration, "Product Editor");
    REQUIRE(started.HasValue());
    auto execution = std::move(started).Value();
    CHECK(Telemetry::Runtime::GetDiagnosticSnapshot().runtimeGeneration == before.runtimeGeneration);
    CHECK(Telemetry::Runtime::GetDiagnosticSnapshot().runtimeEnabled == before.runtimeEnabled);
    CHECK(execution->Dispatcher().Submit("sys.metrics", {}).HasError());
    CHECK_FALSE(operations->SnapshotIfChanged(0));
#if HORO_ENABLE_TELEMETRY
    REQUIRE(Telemetry::Runtime::Initialize({}, std::make_shared<HostTelemetrySink>()));
    auto instrument = Telemetry::Runtime::RegisterGauge({.name = "private_host_metric", .subsystem = "private_host_subsystem"});
    REQUIRE(Telemetry::Runtime::SetAvailability(instrument, Telemetry::MetricAvailabilityState::UnsupportedByBackend));
#endif
    const auto submitted = execution->Dispatcher().Submit("sys.metrics", {3, false, false});
    REQUIRE(submitted.HasValue());
    REQUIRE(submitted.Value().operation);
    REQUIRE(execution->RenderSafePoint().HasValue());
    const auto pending = operations->SnapshotIfChanged(0);
    REQUIRE(pending);
    REQUIRE(pending->operations.size() == 1);
    CHECK(pending->operations[0].state == OperationState::Queued);
    REQUIRE(execution->OwnerSafePoint({}).HasValue());
    const auto completed = operations->SnapshotIfChanged(0);
    REQUIRE(completed);
    REQUIRE(completed->operations.size() == 1);
    const auto &record = completed->operations[0];
    CHECK(record.id == *submitted.Value().operation);
    CHECK(record.state == OperationState::Succeeded);
    CHECK(record.message.starts_with("telemetry generation="));
    CHECK(record.message.size() <= prepared.Value().registry->Limits().outputBytes);
    CHECK(record.message.find("private_host_metric") == std::string::npos);
    CHECK(record.message.find("private_host_subsystem") == std::string::npos);
#if HORO_ENABLE_TELEMETRY
    CHECK(record.message.find("instrument=" + std::to_string(instrument.InstrumentId())) != std::string::npos);
    CHECK(record.message.find("state=unsupported_by_backend") != std::string::npos);
#endif
    REQUIRE(execution->Dispatcher().Submit("sys.metrics", {3, false, false}).HasValue());
    execution->Stop();
    CHECK(execution->OwnerSafePoint({}).HasError());
    const std::array<std::string_view, 2> startupArguments{"--exec", "sys.metrics"};
    auto startup = ConsoleLaunchArguments::Parse(startupArguments);
    REQUIRE(startup.HasValue());
    auto startupLaunch = ResolveConsoleLaunch(startup.Value(), configuration->Snapshot(), ProductProfile::Editor);
    REQUIRE(startupLaunch.HasValue());
    CHECK(PrepareEditorConsole(startupLaunch.Value(), configuration->Snapshot()).HasError());
}

TEST_CASE("Editor Scene commands consume explicit weak views without retaining the uniquely owned Scene", "[console][host][scene]") {
    auto configuration = std::make_shared<ConfigurationService>(HostSchema());
    const std::array<std::string_view, 0> arguments{};
    auto captured = ConsoleLaunchArguments::Parse(arguments);
    REQUIRE(captured.HasValue());
    auto launch = ResolveConsoleLaunch(captured.Value(), configuration->Snapshot(), ProductProfile::Editor);
    REQUIRE(launch.HasValue());
    auto prepared = PrepareEditorConsole(launch.Value(), configuration->Snapshot());
    REQUIRE(prepared.HasValue());
    JobSystem jobs(JobSystemConfig{.workerCount = 0});
    auto operations = std::make_shared<OperationStore>(16, 16);
    auto started = EditorConsoleExecution::Start(prepared.Value(), jobs, operations, configuration, "Product Editor");
    REQUIRE(started.HasValue());
    auto execution = std::move(started).Value();
    auto scene = std::make_unique<Runtime::RuntimeSceneService>(Runtime::SceneRuntimeId{73});
    REQUIRE(scene->Startup({}).HasValue());
    Runtime::SceneDefinitionBuilder builder({1}, {1});
    builder.Add(Runtime::RuntimeEntityDefinition{.object = {1}});
    auto definition = std::move(builder).Build();
    REQUIRE(definition.HasValue());
    REQUIRE(scene->QueuePreparation(std::move(definition).Value()).HasValue());
    const Runtime::FrameContext frame{1, {}, 0.0, 0, {}, false, {}};
    REQUIRE(scene->OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
    const auto view = scene->ActiveScene();
    REQUIRE(view);
    const auto entity = view->EntityAt(0);
    REQUIRE(entity);
    const auto identity = std::to_string(entity->entity.runtime.value) + ":" + std::to_string(entity->entity.entity.index) + ":" +
                          std::to_string(entity->entity.entity.generation);
    CHECK(execution->Dispatcher().Submit("game.scene_tree", {}).HasError());
    CHECK_FALSE(operations->SnapshotIfChanged(0));

    auto tree = execution->Dispatcher().Submit("game.scene_tree", {3, false, false});
    REQUIRE(tree.HasValue());
    REQUIRE(tree.Value().operation);
    REQUIRE(execution->OwnerSafePoint({}, &*view).HasValue());
    auto completed = operations->SnapshotIfChanged(0);
    REQUIRE(completed);
    const auto success = std::ranges::find(completed->operations, *tree.Value().operation, &OperationRecord::id);
    REQUIRE(success != completed->operations.end());
    CHECK(success->state == OperationState::Succeeded);
    CHECK(success->message.find(identity) != std::string::npos);

    SECTION("Absent publication cannot reuse a previous current view") {
        REQUIRE(view->IsCurrent());
        auto missing = execution->Dispatcher().Submit("game.scene_tree", {3, false, false});
        REQUIRE(missing.HasValue());
        REQUIRE(missing.Value().operation);
        REQUIRE(execution->OwnerSafePoint({}).HasValue());
        completed = operations->SnapshotIfChanged(0);
        REQUIRE(completed);
        const auto failed = std::ranges::find(completed->operations, *missing.Value().operation, &OperationRecord::id);
        REQUIRE(failed != completed->operations.end());
        CHECK(failed->state == OperationState::Failed);
    }
    SECTION("Queued inspection survives Scene destruction and rejects its retired view") {
        auto inspect = execution->Dispatcher().Submit("game.inspect " + identity, {3, false, false});
        REQUIRE(inspect.HasValue());
        REQUIRE(inspect.Value().operation);
        scene->Shutdown();
        scene.reset();
        CHECK_FALSE(view->IsCurrent());
        REQUIRE(execution->OwnerSafePoint({}, &*view).HasValue());
        completed = operations->SnapshotIfChanged(0);
        REQUIRE(completed);
        const auto failed = std::ranges::find(completed->operations, *inspect.Value().operation, &OperationRecord::id);
        REQUIRE(failed != completed->operations.end());
        CHECK(failed->state == OperationState::Failed);
        REQUIRE(failed->error);
        CHECK(failed->error->code.Value() == MakeCommandError(CommandError::MissingCapability).code.Value());
    }
    SECTION("Replacement publication preserves the complete original reference rejection") {
        auto inspect = execution->Dispatcher().Submit("game.inspect " + identity, {3, false, false});
        REQUIRE(inspect.HasValue());
        REQUIRE(inspect.Value().operation);
        Runtime::SceneDefinitionBuilder replacementBuilder({1}, {2});
        replacementBuilder.Add(Runtime::RuntimeEntityDefinition{.object = {1}});
        auto replacementDefinition = std::move(replacementBuilder).Build();
        REQUIRE(replacementDefinition.HasValue());
        REQUIRE(scene->QueuePreparation(std::move(replacementDefinition).Value()).HasValue());
        REQUIRE(scene->OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
        const auto replacement = scene->ActiveScene();
        REQUIRE(replacement);
        CHECK_FALSE(view->IsCurrent());
        const auto stale = replacement->Get(entity->entity);
        REQUIRE(stale.HasError());
        REQUIRE(execution->OwnerSafePoint({}, &*replacement).HasValue());
        completed = operations->SnapshotIfChanged(0);
        REQUIRE(completed);
        const auto failed = std::ranges::find(completed->operations, *inspect.Value().operation, &OperationRecord::id);
        REQUIRE(failed != completed->operations.end());
        CHECK(failed->state == OperationState::Failed);
        REQUIRE(failed->error);
        CHECK(failed->error->code.Value() == stale.ErrorValue().code.Value());
        CHECK(failed->error->domain.Value() == stale.ErrorValue().domain.Value());
    }
    SECTION("Foreign owner entry cannot clear an already published capability") {
        bool rejected{};
        std::thread foreign([&] {
            rejected = execution->OwnerSafePoint({}).HasError();
        });
        foreign.join();
        REQUIRE(rejected);
        auto inspect = execution->Dispatcher().Submit("game.inspect " + identity, {3, false, false});
        REQUIRE(inspect.HasValue());
        REQUIRE(inspect.Value().operation);
        // Direct owner dispatch observes the previous capability; the rejected facade entry must not have cleared it.
        REQUIRE(execution->Dispatcher().DrainOwner(1).HasValue());
        completed = operations->SnapshotIfChanged(0);
        REQUIRE(completed);
        const auto inspected = std::ranges::find(completed->operations, *inspect.Value().operation, &OperationRecord::id);
        REQUIRE(inspected != completed->operations.end());
        CHECK(inspected->state == OperationState::Succeeded);
        CHECK(inspected->message.starts_with(identity));
    }
    execution->Stop();
    CHECK(execution->OwnerSafePoint({}, &*view).HasError());
    if (scene)
        scene->Shutdown();
    for (const auto command : {"game.scene_tree", "game.inspect 73:0:1"}) {
        const std::array<std::string_view, 2> startupArguments{"--exec", command};
        auto startup = ConsoleLaunchArguments::Parse(startupArguments);
        REQUIRE(startup.HasValue());
        auto startupLaunch = ResolveConsoleLaunch(startup.Value(), configuration->Snapshot(), ProductProfile::Editor);
        REQUIRE(startupLaunch.HasValue());
        CHECK(PrepareEditorConsole(startupLaunch.Value(), configuration->Snapshot()).HasError());
    }
}

TEST_CASE("Editor network inspection is explicitly composed before activation and remains excluded from startup",
          "[console][host][network]") {
    HostTelemetryLifetime lifetime;
    auto configuration = std::make_shared<ConfigurationService>(HostSchema());
    const std::array<std::string_view, 0> arguments{};
    auto captured = ConsoleLaunchArguments::Parse(arguments);
    REQUIRE(captured.HasValue());
    auto launch = ResolveConsoleLaunch(captured.Value(), configuration->Snapshot(), ProductProfile::Editor);
    REQUIRE(launch.HasValue());
    auto preparation = PrepareEditorConsole(launch.Value(), configuration->Snapshot(), {.networkInspection = true});
    REQUIRE(preparation.HasValue());
    JobSystem jobs(JobSystemConfig{.workerCount = 0});
    auto operations = std::make_shared<OperationStore>(8, 8);
    CHECK(EditorConsoleExecution::Start(preparation.Value(), jobs, operations, configuration, "Editor").HasError());
    CHECK_FALSE(operations->SnapshotIfChanged(0));
    auto query = std::make_shared<Application::NetworkDebuggerService>(std::chrono::hours{1});
    auto started = EditorConsoleExecution::Start(preparation.Value(), jobs, operations, configuration, "Editor", {}, query);
    REQUIRE(started.HasValue());
    auto execution = std::move(started).Value();
    CHECK_FALSE(query->Query().snapshot.attached);
    CHECK(execution->Dispatcher().Submit("net.debug_status", {}).HasError());
    CHECK(execution->Dispatcher().Submit("net.debug_status", {3}).HasError());
    CHECK_FALSE(operations->SnapshotIfChanged(0));
#if HORO_ENABLE_TELEMETRY
    REQUIRE(Telemetry::Runtime::Initialize({}, std::make_shared<HostTelemetrySink>()));
    const auto accepted = execution->Dispatcher().Submit("net.debug_status", {3});
    REQUIRE(accepted.HasValue());
    REQUIRE(accepted.Value().operation);
    REQUIRE(execution->OwnerSafePoint({}).HasValue());
    const auto snapshot = operations->SnapshotIfChanged(0);
    REQUIRE(snapshot);
    const auto record = std::ranges::find(snapshot->operations, *accepted.Value().operation, &OperationRecord::id);
    REQUIRE(record != snapshot->operations.end());
    CHECK(record->state == OperationState::Succeeded);
    CHECK(record->message.starts_with("network diagnostics state=detached"));
    CHECK_FALSE(query->Query().snapshot.attached);
#endif
    const std::array<std::string_view, 2> startupArguments{"--exec", "net.debug_status"};
    CHECK_FALSE(execution->StoreOverlayVisible(OverlaySource::Network, {3}).Value());
    CHECK(execution->RequestStoreOverlayVisibility(OverlaySource::Network, true, {}).HasError());
    const auto networkPanel = execution->RequestStoreOverlayVisibility(OverlaySource::Network, true, {3});
    REQUIRE(networkPanel.HasValue());
    REQUIRE(networkPanel.Value().operation);
    CHECK_FALSE(execution->StoreOverlayVisible(OverlaySource::Network, {3}).Value());
    REQUIRE(execution->OwnerSafePoint({}).HasValue());
    CHECK(execution->StoreOverlayVisible(OverlaySource::Network, {3}).Value());
    CHECK_FALSE(query->Query().snapshot.attached);
    auto startup = ConsoleLaunchArguments::Parse(startupArguments);
    REQUIRE(startup.HasValue());
    auto startupLaunch = ResolveConsoleLaunch(startup.Value(), configuration->Snapshot(), ProductProfile::Editor);
    REQUIRE(startupLaunch.HasValue());
    CHECK(PrepareEditorConsole(startupLaunch.Value(), configuration->Snapshot(), {.networkInspection = true}).HasError());
}

TEST_CASE("Editor core console rejects unbound standard capabilities and ungranted scripts before native startup",
          "[console][startup][host]") {
    for (const auto flag : {"--exec", "--console-script"}) {
        Arguments arguments{"HoroEditor", flag, flag == std::string_view("--exec") ? "sys.profile_capture" : "scripts/boot.console"};
        auto captured = CaptureEditorConsoleLaunch(arguments.argv);
        REQUIRE(captured.HasValue());
        auto configurationOwner = std::make_shared<ConfigurationService>(HostSchema());
        auto &configuration = *configurationOwner;
        auto launch = ResolveConsoleLaunch(captured.Value().console, configuration.Snapshot(), ProductProfile::Editor);
        REQUIRE(launch.HasValue());
        CHECK(PrepareEditorConsole(launch.Value(), configuration.Snapshot()).HasError());
    }
}

TEST_CASE("Editor startup partial admission cancels the accepted prefix through the original process store", "[console][startup][host]") {
    Arguments arguments{"HoroEditor", "--exec", "clear", "--exec", "clear"};
    auto captured = CaptureEditorConsoleLaunch(arguments.argv);
    REQUIRE(captured.HasValue());
    auto configurationOwner = std::make_shared<ConfigurationService>(HostSchema());
    auto &configuration = *configurationOwner;
    auto launch = ResolveConsoleLaunch(captured.Value().console, configuration.Snapshot(), ProductProfile::Editor);
    REQUIRE(launch.HasValue());
    auto prepared = PrepareEditorConsole(launch.Value(), configuration.Snapshot());
    REQUIRE(prepared.HasValue());
    JobSystem jobs(JobSystemConfig{.workerCount = 0});
    auto operations = std::make_shared<OperationStore>(1, 4);
    auto started = EditorConsoleExecution::Start(prepared.Value(), jobs, operations, configurationOwner, "Product Editor");
    REQUIRE(started.HasError());
    CHECK(started.ErrorValue().code.Value() == "console.limit_exceeded");
    REQUIRE(prepared.Value().history->Entries().size() == 1);
    CHECK(prepared.Value().history->Entries()[0].command == "clear");
    const auto snapshot = operations->SnapshotIfChanged(0);
    REQUIRE(snapshot);
    REQUIRE(snapshot->operations.size() == 1);
    CHECK(snapshot->operations[0].state == OperationState::Cancelled);
    CHECK(prepared.Value().history->Entries()[0].operation == snapshot->operations[0].id);
    auto replacement = std::make_shared<OperationStore>(8, 8);
    CHECK(EditorConsoleExecution::Start(prepared.Value(), jobs, replacement, configurationOwner, "Product Editor").HasError());
    CHECK_FALSE(replacement->SnapshotIfChanged(0));
}
