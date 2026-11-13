#include "Horo/Debug/ConsoleBuiltins.h"

#include "ConsoleText.h"
#include "Horo/Debug/ConsoleVariableSet.h"

#include <algorithm>

namespace Horo::Debug {
    namespace {
        constexpr DebugArgumentDescriptor HelpArguments[]{{"command", "Exact allowed command name", DebugArgumentType::String, false, ""}};
        constexpr DebugArgumentDescriptor FindArguments[]{{"text", "Descriptor name or summary substring", DebugArgumentType::String}};
        constexpr DebugCommandDescriptor Descriptors[]{{HelpCommandId, "help", "List allowed commands or describe one command",
                                                        "help [command]", HelpArguments, CommandPermission::Public,
                                                        CommandThreadPolicy::ImmediateConsoleThread,
                                                        CommandAvailability::ShippingAllowlist},
                                                       {FindCommandId, "find", "Search allowed descriptors", "find <text>", FindArguments,
                                                        CommandPermission::Public, CommandThreadPolicy::ImmediateConsoleThread,
                                                        CommandAvailability::ShippingAllowlist},
                                                       {VersionCommandId,
                                                        "version",
                                                        "Print product build identity",
                                                        "version",
                                                        {},
                                                        CommandPermission::Public,
                                                        CommandThreadPolicy::ImmediateConsoleThread,
                                                        CommandAvailability::ShippingAllowlist},
                                                       {ClearCommandId,
                                                        "clear",
                                                        "Clear visible console records",
                                                        "clear",
                                                        {},
                                                        CommandPermission::Public,
                                                        CommandThreadPolicy::OwnerThreadNextFrame,
                                                        CommandAvailability::ShippingAllowlist}};

        /** @brief Owns only immutable registry/build identity and the explicit visible history capability. */
        class CoreHandler final : public ICommandHandler {
        public:
            CoreHandler(DebugCommandId id, std::shared_ptr<const CommandRegistry> registry, std::shared_ptr<ConsoleHistory> history,
                        std::string version, std::shared_ptr<const ConsoleVariableSet> variables)
                : id_(id), registry_(std::move(registry)), history_(std::move(history)), version_(std::move(version)),
                  variables_(std::move(variables)) {}

            /** @copydoc ICommandHandler::Execute */
            Result<DebugCommandOutput> Execute(const DebugCommandContext &context, std::span<const ArgumentValue> arguments,
                                               const CancellationToken &, const JobExecutionContext *) override {
                if (id_ == VersionCommandId)
                    return Result<DebugCommandOutput>::Success({version_});
                if (id_ == ClearCommandId) {
                    history_->Clear();
                    return Result<DebugCommandOutput>::Success({});
                }
                const auto &query = std::get<std::string>(arguments.front());
                const auto matches = registry_->Discover(query, context);
                std::string output;
                const auto limit = registry_->Limits().outputBytes;
                const auto append = [&](const std::string_view text) {
                    output.append(Detail::ConsoleTextPrefix(text, limit - output.size()));
                };
                for (const auto &descriptor : matches) {
                    if (id_ == HelpCommandId && !query.empty() && descriptor.name != query)
                        continue;
                    append(descriptor.syntax);
                    append(" — ");
                    append(descriptor.summary);
                    append("\n");
                    if (output.size() == limit)
                        break;
                }
                if (variables_) {
                    constexpr std::string_view types[]{"bool", "int64", "float", "string"};
                    for (const auto &descriptor : variables_->Discover(query, context)) {
                        if (id_ == HelpCommandId && !query.empty() && descriptor.name != query)
                            continue;
                        append(descriptor.name);
                        append(" (variable, ");
                        append(types[descriptor.defaultValue.index()]);
                        append(")\n");
                        if (output.size() == limit)
                            break;
                    }
                }
                return Result<DebugCommandOutput>::Success({std::move(output)});
            }

        private:
            DebugCommandId id_;
            std::shared_ptr<const CommandRegistry> registry_;
            std::shared_ptr<ConsoleHistory> history_;
            std::string version_;
            std::shared_ptr<const ConsoleVariableSet> variables_;
        };
    }  // namespace

    /** @copydoc CoreConsoleDescriptors */
    std::span<const DebugCommandDescriptor> CoreConsoleDescriptors() noexcept {
        return Descriptors;
    }

    /** @copydoc BindCoreConsoleCommands */
    Result<std::vector<CommandBinding>> BindCoreConsoleCommands(std::shared_ptr<const CommandRegistry> registry,
                                                                std::shared_ptr<ConsoleHistory> history, std::string version,
                                                                std::shared_ptr<const ConsoleVariableSet> variables) {
        if (!registry || !registry->Profile() || !history || version.size() > registry->Limits().outputBytes ||
            (variables && variables->Profile() != *registry->Profile()))
            return Result<std::vector<CommandBinding>>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
        std::vector<CommandBinding> bindings;
        for (const auto &descriptor : Descriptors) {
            const auto resolved = registry->Resolve(descriptor.id, {15, true, true});
            if (resolved.HasError())
                continue;
            const auto &actual = resolved.Value();
            if (actual.name != descriptor.name || actual.threadPolicy != descriptor.threadPolicy ||
                actual.permissions != descriptor.permissions || actual.availability != descriptor.availability ||
                actual.arguments.size() != descriptor.arguments.size())
                return Result<std::vector<CommandBinding>>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
            for (std::size_t i = 0; i < actual.arguments.size(); ++i) {
                if (actual.arguments[i].type != DebugArgumentType::String ||
                    actual.arguments[i].required != descriptor.arguments[i].required || actual.arguments[i].option ||
                    actual.arguments[i].defaultValue != descriptor.arguments[i].defaultValue)
                    return Result<std::vector<CommandBinding>>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
            }
            bindings.push_back({descriptor.id, std::make_shared<CoreHandler>(descriptor.id, registry, history, version, variables)});
        }
        return Result<std::vector<CommandBinding>>::Success(std::move(bindings));
    }
}  // namespace Horo::Debug
