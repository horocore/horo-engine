/** @file Consumer.cpp @brief Standalone optional SDK consumer using only exported console/Foundation contracts. */
#include "Horo/Debug/ConsoleBuiltins.h"
#include "Horo/Debug/ConsoleConfiguration.h"
#include "Horo/Debug/ConsoleVariableSet.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Debug;
    constexpr DebugCommandId SampleCommandId = 0x1000;

    /** @brief Owns a project module's actual test action; its state is touched only by the fixed console owner. */
    class SampleHandler final : public ICommandHandler {
    public:
        std::size_t calls{};
        std::thread::id owner;

        /** @brief Executes the project-owned action without importing a concrete renderer, private service or editor type. */
        Result<DebugCommandOutput> Execute(const DebugCommandContext &, std::span<const ArgumentValue>, const CancellationToken &,
                                           const JobExecutionContext *) override {
            ++calls;
            owner = std::this_thread::get_id();
            return Result<DebugCommandOutput>::Success({"sample inspected"});
        }
    };

    /** @brief Uses the same public typed configuration contribution as the actual host, with one canonical service. */
    Result<std::shared_ptr<ConfigurationService>> MakeSdkConfiguration() {
        auto settings = ConsoleConfigurationDescriptors(ProductProfile::Development);
        if (settings.HasError())
            return Result<std::shared_ptr<ConfigurationService>>::Failure(settings.ErrorValue());
        ConfigurationSchema schema;
        for (const auto &descriptor : settings.Value()) {
            const auto registered = schema.Register(descriptor);
            if (registered.HasError())
                return Result<std::shared_ptr<ConfigurationService>>::Failure(registered.ErrorValue());
        }
        const auto sealed = schema.Seal();
        if (sealed.HasError())
            return Result<std::shared_ptr<ConfigurationService>>::Failure(sealed.ErrorValue());
        return Result<std::shared_ptr<ConfigurationService>>::Success(std::make_shared<ConfigurationService>(std::move(schema)));
    }

    /** @brief Checks the exported execution, configuration and operation contracts through their original public owners. */
    bool Exercise(CommandExecutor &executor, ConsoleVariableSet &variables, ConsoleHistory &history,
                  const std::shared_ptr<OperationStore> &operations, ConfigurationService &configuration, const SampleHandler &handler) {
        const auto fail = [](const char *stage) {
            std::fprintf(stderr, "SDK execution contract failed at %s\n", stage);
            return false;
        };
        const DebugCommandContext developer{3, false, false};
        if (executor.ValidateOwnerBoundary().HasError() || executor.Submit("sample.inspect", {}).HasValue() || handler.calls != 0)
            return fail("owner admission and permission denial");
        const auto admitted = executor.Submit("sample.inspect", developer);
        if (admitted.HasError() || !admitted.Value().operation || executor.DrainRender(8).HasError() || handler.calls != 0)
            return fail("project command queue admission");
        if (executor.DrainOwner(8).HasError() || handler.calls != 1 || handler.owner != std::this_thread::get_id())
            return fail("project owner dispatch");
        const auto completed = operations->SnapshotIfChanged(0);
        if (!completed || completed->operations.size() != 1 || completed->operations[0].id != *admitted.Value().operation ||
            completed->operations[0].state != OperationState::Succeeded || completed->operations[0].message != "sample inspected")
            return fail("project operation identity and result");
        const auto help = executor.Submit("help", developer);
        if (help.HasError() || help.Value().output.text.find("sample.inspect") == std::string::npos ||
            help.Value().output.text.find(ConsoleLogOverlayVariableName) == std::string::npos)
            return fail("shared command and variable discovery");
        const auto original = configuration.Snapshot();
        if (executor.Submit("console.visible true", {}).HasError() || executor.Submit("console.history.limit 2", developer).HasError() ||
            executor.DrainOwner(8).HasError())
            return fail("typed variable submission");
        const auto visible = variables.Read(ConsoleVisibleVariableName, {});
        const auto pending = variables.Read("console.history.limit", developer);
        if (visible.HasError() || !std::get<bool>(visible.Value().active) || pending.HasError() || !pending.Value().pending ||
            std::get<std::int64_t>(*pending.Value().pending) != 2 || configuration.Snapshot().Revision() != original.Revision())
            return fail("active versus pending configuration");
        if (executor.ValidateOwnerBoundary().HasError() || variables.Apply(ConsoleVariableApplyPolicy::NextFrame).HasError())
            return fail("next-frame activation");
        const auto limit = ReadConsoleHistoryLimit(configuration.Snapshot());
        if (limit.HasError() || limit.Value() != 2 || history.SetEntryLimit(limit.Value()).HasError())
            return fail("canonical history limit");
        for (const auto text : {"first", "second", "third"})
            history.Append({SampleCommandId, {}, text}, "safe result");
        if (history.Entries().size() != 2 || history.Entries().front().command != "second")
            return fail("bounded history consumption");
        const auto queued = executor.Submit("sample.inspect", developer);
        if (queued.HasError() || !queued.Value().operation)
            return fail("queued shutdown work");
        executor.Stop();
        if (executor.ValidateOwnerBoundary().HasValue() || executor.DrainOwner(8).HasValue() ||
            executor.Submit("sample.inspect", developer).HasValue() || handler.calls != 1)
            return fail("shared shutdown rejection");
        const auto stopped = operations->SnapshotIfChanged(0);
        if (!stopped)
            return fail("shutdown operation snapshot");
        const auto cancelled = std::ranges::find_if(stopped->operations, [&](const auto &operation) {
            return operation.id == *queued.Value().operation;
        });
        if (cancelled == stopped->operations.end() || cancelled->state != OperationState::Cancelled)
            return fail("queued cancellation");
        return true;
    }
}  // namespace

/** @brief Resolves, links and runs the optional public SDK without private headers or unrelated runtime components. */
int main() {
    using namespace Horo;
    using namespace Horo::Debug;
    const auto core = CoreConsoleDescriptors();
    std::vector<DebugCommandDescriptor> commands(core.begin(), core.end());
    commands.push_back({SampleCommandId, "sample.inspect", "Inspect sample", "sample.inspect"});
    RegistryPolicy policy;
    policy.profile = ProductProfile::Development;
    policy.projectNamespaces = {"sample"};
    auto created = CommandRegistry::Create(commands, policy);
    auto configuration = MakeSdkConfiguration();
    if (created.HasError() || configuration.HasError())
        return 1;
    auto registry = std::make_shared<CommandRegistry>(std::move(created).Value());
    const std::array controls{ConsoleHistoryVariableDescriptor(), ConsoleVisibilityVariableDescriptor(),
                              ConsoleLogOverlayVariableDescriptor(), ConsoleJobsOverlayVariableDescriptor()};
    auto inventory = ConsoleVariableSet::Create(controls, commands, policy, configuration.Value());
    if (inventory.HasError())
        return 2;
    auto variables = std::make_shared<ConsoleVariableSet>(std::move(inventory).Value());
    auto history = std::make_shared<ConsoleHistory>();
    auto bindings = BindCoreConsoleCommands(registry, history, "External SDK fixture", variables);
    if (bindings.HasError())
        return 3;
    auto ownedBindings = std::move(bindings).Value();
    auto handler = std::make_shared<SampleHandler>();
    ownedBindings.push_back({SampleCommandId, handler});
    JobSystem jobs(JobSystemConfig{.workerCount = 0});
    auto operations = std::make_shared<OperationStore>(16, 16);
    auto audit = CreateConsoleTelemetryAudit();
    if (!audit)
        return 4;
    auto executor = CommandExecutor::Create(registry, ownedBindings, jobs, operations, std::this_thread::get_id(),
                                            std::this_thread::get_id(), 16, std::move(audit), variables);
    if (executor.HasError())
        return 4;
    if (!Exercise(*executor.Value(), *variables, *history, operations, *configuration.Value(), *handler))
        return 5;
    std::puts("Runtime console external SDK contract passed");
    return 0;
}
