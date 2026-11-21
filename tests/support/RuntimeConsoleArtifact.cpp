/** @file RuntimeConsoleArtifact.cpp @brief Linked profile-catalog fixture with real core execution; not a packaged graphical host. */
#include "Horo/Debug/ConsoleProduct.h"
#include "Horo/Debug/ConsoleVariableSet.h"

#include <array>
#include <cstdio>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Debug;

    /** @brief Resolves only the compiled artifact identity; unsupported values cannot produce a valid manifest. */
    std::string_view ProfileName(const ProductProfile profile) {
        constexpr std::array names{"Editor", "Development", "Profile", "Shipping", "Diagnostics", "Server"};
        const auto index = static_cast<std::size_t>(profile);
        return index < names.size() ? names[index] : "Invalid";
    }

    /** @brief Reports a stable fixture failure without printing private error payloads. */
    int Failed(const Error &error) {
        std::fprintf(stderr, "Console artifact fixture failed: %s\n", error.code.Value().c_str());
        return 1;
    }

    /** @brief Emits only descriptor identities, retaining no raw input or generated diagnostic payload. */
    void Entry(const char *kind, const std::string_view name) {
        std::printf("%s\t%.*s\n", kind, static_cast<int>(name.size()), name.data());
    }

    /** @brief Exercises the selected non-shipping history bridge from real catalog metadata through the same core dispatcher. */
    Result<void> CheckHistory(CommandExecutor &executor, ConsoleVariableSet &variables, ConsoleHistory &history,
                              const ConfigurationService &configuration, std::span<const ConsoleVariableDescriptor> controls) {
        if (controls.empty()) {
            Entry("omitted", "history");
            return Result<void>::Success();
        }
        const auto accepted = executor.Submit(controls.front().name + " 3", {3, false, false});
        if (accepted.HasError())
            return Result<void>::Failure(accepted.ErrorValue());
        const auto drained = executor.DrainOwner(1);
        if (drained.HasError())
            return Result<void>::Failure(drained.ErrorValue());
        const auto applied = variables.Apply(ConsoleVariableApplyPolicy::NextFrame);
        if (applied.HasError())
            return Result<void>::Failure(applied.ErrorValue());
        const auto limit = ReadConsoleHistoryLimit(configuration.Snapshot());
        if (limit.HasError())
            return Result<void>::Failure(limit.ErrorValue());
        if (limit.Value() != 3 || applied.Value() != 1 || !accepted.Value().operation || drained.Value() != 1)
            return Result<void>::Failure(MakeCommandError(CommandError::HandlerFailed));
        const auto resized = history.SetEntryLimit(limit.Value());
        if (resized.HasError())
            return Result<void>::Failure(resized.ErrorValue());
        for (int index = 0; index < 4; ++index)
            history.Append({VersionCommandId, {}, "fixture"}, "safe result");
        if (history.Entries().size() != 3)
            return Result<void>::Failure(MakeCommandError(CommandError::HandlerFailed));
        Entry("passed", "history");
        return Result<void>::Success();
    }
}  // namespace

/** @brief Runs the bounded qualification fixture without command-line input, native UI or fabricated subsystem handlers. */
int main() {
    using namespace Horo;
    using namespace Horo::Debug;
    const auto profile = CompiledConsoleProfile();
    std::puts("horo-console-artifact\t1");
    Entry("profile", ProfileName(profile));
    const auto catalog = ProductConsoleDescriptors();
    for (const auto &descriptor : catalog)
        Entry("command", descriptor.name);
    const auto variableCatalog = ProductConsoleVariableDescriptors();
    std::vector<ConsoleVariableDescriptor> controls;
    for (const auto &descriptor : variableCatalog) {
        Entry("variable", descriptor.name);
        if (descriptor.id == ConsoleHistoryLimitVariableId)
            controls.push_back(descriptor);
    }
    auto settings = ConsoleConfigurationDescriptors(profile);
    if (settings.HasError())
        return Failed(settings.ErrorValue());
    ConfigurationSchema schema;
    for (const auto &descriptor : settings.Value()) {
        const auto registered = schema.Register(descriptor);
        if (registered.HasError())
            return Failed(registered.ErrorValue());
    }
    const auto sealed = schema.Seal();
    if (sealed.HasError())
        return Failed(sealed.ErrorValue());
    auto configuration = std::make_shared<ConfigurationService>(std::move(schema));
    RegistryPolicy policy;
    policy.profile = profile;
    policy.shippingAllowlist = {HelpCommandId, FindCommandId, VersionCommandId, ClearCommandId};
    auto created = CommandRegistry::Create(CoreConsoleDescriptors(), policy);
    if (created.HasError())
        return Failed(created.ErrorValue());
    auto registry = std::make_shared<CommandRegistry>(std::move(created).Value());
    auto createdVariables = ConsoleVariableSet::Create(controls, CoreConsoleDescriptors(), policy, configuration);
    if (createdVariables.HasError())
        return Failed(createdVariables.ErrorValue());
    auto variables = std::make_shared<ConsoleVariableSet>(std::move(createdVariables).Value());
    auto history = std::make_shared<ConsoleHistory>();
    auto bindings = BindCoreConsoleCommands(registry, history, "Console qualification fixture", variables);
    if (bindings.HasError())
        return Failed(bindings.ErrorValue());
    JobSystem jobs(JobSystemConfig{.workerCount = 0});
    auto operations = std::make_shared<OperationStore>(8, 8);
    auto createdExecutor = CommandExecutor::Create(registry, bindings.Value(), jobs, operations, std::this_thread::get_id(),
                                                   std::this_thread::get_id(), 128, nullptr, variables);
    if (createdExecutor.HasError())
        return Failed(createdExecutor.ErrorValue());
    auto executor = std::move(createdExecutor).Value();
    history->Append({0, {}, "fixture.initial"}, "safe result");
    constexpr std::array requests{std::pair{"help", "help"}, std::pair{"find", "find version"}, std::pair{"version", "version"},
                                  std::pair{"clear", "clear"}};
    for (const auto &[command, input] : requests) {
        const auto submitted = executor->Submit(input, {});
        if (submitted.HasError())
            return Failed(submitted.ErrorValue());
        if (submitted.Value().operation) {
            const auto drained = executor->DrainOwner(1);
            if (drained.HasError())
                return Failed(drained.ErrorValue());
            const auto snapshot = operations->SnapshotIfChanged(0);
            if (!snapshot || snapshot->operations.size() != 1 || snapshot->operations.front().id != *submitted.Value().operation ||
                snapshot->operations.front().state != OperationState::Succeeded || !history->Entries().empty())
                return Failed(MakeCommandError(CommandError::HandlerFailed));
        } else if ((std::string_view(command) == "version" && submitted.Value().output.text != "Console qualification fixture") ||
                   (std::string_view(command) != "version" && submitted.Value().output.text.find("version") == std::string::npos)) {
            return Failed(MakeCommandError(CommandError::HandlerFailed));
        }
        Entry("passed", command);
    }
    const auto checked = CheckHistory(*executor, *variables, *history, *configuration, controls);
    if (checked.HasError())
        return Failed(checked.ErrorValue());
    executor->Stop();
    return 0;
}
