#include "Horo/Debug/CommandRegistry.h"

#include "ConsoleParse.h"
#include "ConsoleText.h"
#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>

namespace Horo::Debug {
    namespace {
        /** @brief Admits canonical dotted ASCII identifiers without empty namespace segments. */
        bool Identifier(const std::string_view text) {
            bool start = true;
            for (const char c : text) {
                if (c == '.') {
                    if (start)
                        return false;
                    start = true;
                } else {
                    const bool letter = (c >= 'a' && c <= 'z') || c == '_';
                    if (!letter && (start || c < '0' || c > '9'))
                        return false;
                    start = false;
                }
            }
            return !start;
        }

        /** @brief Canonical engine prefixes and host-approved project prefixes are disjoint. */
        constexpr std::string_view EngineNamespaces[]{"sys", "log", "net", "wst", "rnd", "phys", "game", "diag"};

        /** @brief Rejects unregistered namespaces and arbitrary unqualified aliases. */
        bool RegisteredName(const std::string_view name, const RegistryPolicy &policy) {
            const auto dot = name.find('.');
            if (dot == std::string_view::npos)
                return name == "help" || name == "find" || name == "version" || name == "clear" || name == "screenshot";
            const auto prefix = name.substr(0, dot);
            return std::ranges::find(EngineNamespaces, prefix) != std::end(EngineNamespaces) ||
                   std::ranges::find(policy.projectNamespaces, prefix) != policy.projectNamespaces.end();
        }

        /** @brief Checks exact availability sets from ADR-018 before principal permissions. */
        bool Included(const DebugCommandDescriptor &command, const RegistryPolicy &policy) {
            switch (command.availability) {
                case CommandAvailability::DevelopmentOnly:
                    return policy.profile == ProductProfile::Editor || policy.profile == ProductProfile::Development;
                case CommandAvailability::DiagnosticsOnly:
                    return policy.profile == ProductProfile::Editor || policy.profile == ProductProfile::Development ||
                           policy.profile == ProductProfile::Diagnostics;
                case CommandAvailability::NonShipping:
                    return policy.profile != ProductProfile::Shipping;
                case CommandAvailability::ShippingAllowlist:
                    return policy.profile != ProductProfile::Shipping ||
                           std::ranges::find(policy.shippingAllowlist, command.id) != policy.shippingAllowlist.end();
            }
            return false;
        }

        /** @brief Requires every permission bit, orthogonal authentication, and cheat authority. */
        bool Authorized(const DebugCommandDescriptor &command, const DebugCommandContext &context, ProductProfile profile) {
            const auto permissions = static_cast<std::uint32_t>(command.permissions);
            if ((permissions & context.permissionMask) != permissions)
                return false;
            if ((permissions & 8U) != 0 && (!context.authenticated || profile == ProductProfile::Profile))
                return false;
            if ((permissions & 4U) != 0) {
                if (!context.hasServerAuthority)
                    return false;
                if (profile != ProductProfile::Editor && profile != ProductProfile::Development)
                    return false;
            }
            if (profile == ProductProfile::Shipping && permissions != 1U)
                return false;
            return true;
        }

        /** @brief Tokenizes the small language; unsupported shell operators fail outside quotes. */
        Result<std::vector<std::string>> Tokenize(std::string_view input, const ConsoleLimits &limits) {
            if (input.size() > limits.inputBytes)
                return Result<std::vector<std::string>>::Failure(MakeCommandError(CommandError::LimitExceeded));
            if (!IsValidUtf8ScalarSequence(input))
                return Result<std::vector<std::string>>::Failure(MakeCommandError(CommandError::Usage));
            std::vector<std::string> tokens;
            std::string token;
            char quote = 0;
            bool escaped = false;
            bool started = false;
            for (const char c : input) {
                if (c == '\0' || c == '\n' || c == '\r' || (static_cast<unsigned char>(c) < 32U && c != '\t'))
                    return Result<std::vector<std::string>>::Failure(MakeCommandError(CommandError::Usage));
                if (escaped) {
                    if (c != '\\' && c != '"' && c != '\'')
                        return Result<std::vector<std::string>>::Failure(MakeCommandError(CommandError::Usage));
                    token += c;
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                    started = true;
                } else if (quote != 0) {
                    if (c == quote)
                        quote = 0;
                    else
                        token += c;
                } else if (c == '"' || c == '\'') {
                    quote = c;
                    started = true;
                } else if (c == ' ' || c == '\t') {
                    if (started) {
                        tokens.push_back(std::move(token));
                        token.clear();
                        started = false;
                        if (tokens.size() > 1 + limits.arguments * 2)
                            return Result<std::vector<std::string>>::Failure(MakeCommandError(CommandError::LimitExceeded));
                    }
                } else {
                    if (c == '|' || c == ';' || c == '&' || c == '`' || c == '$' || c == '<' || c == '>')
                        return Result<std::vector<std::string>>::Failure(MakeCommandError(CommandError::Usage));
                    token += c;
                    started = true;
                }
            }
            if (escaped || quote != 0)
                return Result<std::vector<std::string>>::Failure(MakeCommandError(CommandError::Usage));
            if (started)
                tokens.push_back(std::move(token));
            if (tokens.empty() || tokens.size() > 1 + limits.arguments * 2)
                return Result<std::vector<std::string>>::Failure(MakeCommandError(CommandError::Usage));
            return Result<std::vector<std::string>>::Success(std::move(tokens));
        }

        /** @brief Enforces optional numeric schema bounds without narrowing an admitted integer. */
        bool InRange(const DebugArgumentDescriptor &argument, const long double value) {
            return (!argument.minimum || value >= *argument.minimum) && (!argument.maximum || value <= *argument.maximum);
        }

        /** @brief Parses a generation-qualified reference instead of an ambiguous current-scene slot. */
        Result<ArgumentValue> EntityArgument(const std::string_view text) {
            const auto first = text.find(':');
            if (first == std::string_view::npos)
                return Result<ArgumentValue>::Failure(MakeCommandError(CommandError::Usage));
            const auto second = text.find(':', first + 1);
            if (second == std::string_view::npos)
                return Result<ArgumentValue>::Failure(MakeCommandError(CommandError::Usage));
            DebugEntityReference entity;
            const auto parse = [](std::string_view part, auto &value) {
                if (part.empty())
                    return false;
                const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), value);
                return error == std::errc{} && end == part.data() + part.size();
            };
            if (!parse(text.substr(0, first), entity.scene) || !parse(text.substr(first + 1, second - first - 1), entity.index) ||
                !parse(text.substr(second + 1), entity.generation) || entity.scene == 0 || entity.generation == 0)
                return Result<ArgumentValue>::Failure(MakeCommandError(CommandError::Usage));
            return Result<ArgumentValue>::Success(entity);
        }

        /** @brief Converts a complete token without accepting partial, overflowing or non-finite numbers. */
        Result<ArgumentValue> Convert(const DebugArgumentDescriptor &argument, const std::string_view text) {
            if (text.empty() && argument.type != DebugArgumentType::String)
                return Result<ArgumentValue>::Failure(MakeCommandError(CommandError::Usage));
            switch (argument.type) {
                case DebugArgumentType::String:
                    return Result<ArgumentValue>::Success(std::string(text));
                case DebugArgumentType::Enum:
                    if (std::ranges::find(argument.enumeration, text) != argument.enumeration.end())
                        return Result<ArgumentValue>::Success(std::string(text));
                    break;
                case DebugArgumentType::Bool:
                    if (text == "true")
                        return Result<ArgumentValue>::Success(true);
                    if (text == "false")
                        return Result<ArgumentValue>::Success(false);
                    break;
                case DebugArgumentType::Int32:
                case DebugArgumentType::Int64: {
                    std::int64_t value{};
                    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
                    if (error == std::errc{} && end == text.data() + text.size() && InRange(argument, value) &&
                        (argument.type == DebugArgumentType::Int64 ||
                         (value >= std::numeric_limits<std::int32_t>::min() && value <= std::numeric_limits<std::int32_t>::max())))
                        return Result<ArgumentValue>::Success(value);
                    break;
                }
                case DebugArgumentType::Float: {
                    double value{};
                    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
                    if (error == std::errc{} && end == text.data() + text.size() && std::isfinite(value) && InRange(argument, value))
                        return Result<ArgumentValue>::Success(value);
                    break;
                }
                case DebugArgumentType::EntityId: {
                    return EntityArgument(text);
                }
            }
            return Result<ArgumentValue>::Failure(MakeCommandError(CommandError::Usage));
        }

        /** @brief Owns descriptor bytes; views are published only after all vectors stop growing. */
        struct OwnedArgument final {
            std::string name, description, defaultValue;
            std::vector<std::string> enumeration;
            std::vector<std::string_view> enumViews;
        };

        /** @brief Owns one immutable descriptor and its argument declarations. */
        struct OwnedCommand final {
            std::string name, summary, syntax;
            std::vector<OwnedArgument> storage;
            std::vector<DebugArgumentDescriptor> arguments;
            DebugCommandDescriptor descriptor;
        };

        /** @brief Rejects ambiguous schemas and invalid privacy/profile declarations before composition. */
        bool Valid(const DebugCommandDescriptor &command, const ConsoleLimits &limits) {
            const auto permission = static_cast<std::uint32_t>(command.permissions);
            if (!command.id || !Identifier(command.name) || command.name.size() > limits.metadataBytes ||
                command.summary.size() > limits.metadataBytes || command.syntax.size() > limits.metadataBytes ||
                command.arguments.size() > limits.arguments || (permission != 1 && permission != 2 && permission != 4 && permission != 8) ||
                static_cast<unsigned>(command.threadPolicy) > 3 || static_cast<unsigned>(command.availability) > 3 ||
                (static_cast<unsigned>(command.flags) & ~3U) != 0)
                return false;
            if (!IsValidUtf8ScalarSequence(command.summary) || !IsValidUtf8ScalarSequence(command.syntax))
                return false;
            if (command.availability == CommandAvailability::ShippingAllowlist && permission != 1U)
                return false;
            if ((permission & 4U) != 0 && command.availability != CommandAvailability::DevelopmentOnly)
                return false;
            bool optionalPositional = false;
            for (std::size_t i = 0; i < command.arguments.size(); ++i) {
                const auto &arg = command.arguments[i];
                if (!Identifier(arg.name) || arg.name.find('.') != std::string_view::npos || arg.name.size() > limits.metadataBytes ||
                    arg.description.size() > limits.metadataBytes || arg.defaultValue.size() > limits.inputBytes ||
                    arg.enumeration.size() > 256 || static_cast<unsigned>(arg.type) > 6)
                    return false;
                if (!IsValidUtf8ScalarSequence(arg.description) || !IsValidUtf8ScalarSequence(arg.defaultValue))
                    return false;
                if ((arg.minimum && !std::isfinite(*arg.minimum)) || (arg.maximum && !std::isfinite(*arg.maximum)) ||
                    (arg.minimum && arg.maximum && *arg.minimum > *arg.maximum))
                    return false;
                if ((arg.minimum || arg.maximum) && arg.type != DebugArgumentType::Int32 && arg.type != DebugArgumentType::Int64 &&
                    arg.type != DebugArgumentType::Float)
                    return false;
                for (std::size_t j = 0; j < i; ++j)
                    if (command.arguments[j].name == arg.name)
                        return false;
                if (!arg.option) {
                    if (arg.required && optionalPositional)
                        return false;
                    if (!arg.required)
                        optionalPositional = true;
                }
                for (std::size_t j = 0; j < arg.enumeration.size(); ++j) {
                    if (arg.enumeration[j].size() > limits.metadataBytes || arg.enumeration[j].empty() ||
                        !IsValidUtf8ScalarSequence(arg.enumeration[j]))
                        return false;
                    for (std::size_t k = 0; k < j; ++k)
                        if (arg.enumeration[j] == arg.enumeration[k])
                            return false;
                }
                if (arg.type == DebugArgumentType::Enum && arg.enumeration.empty())
                    return false;
                if (!arg.required && Convert(arg, arg.defaultValue).HasError())
                    return false;
            }
            return true;
        }

        /** @brief Rejects host budgets that would permit unbounded allocation before touching metadata. */
        bool ValidPolicy(const RegistryPolicy &policy, const std::size_t descriptorCount) {
            const auto &limits = policy.limits;
            return descriptorCount <= limits.commands && limits.commands <= 4096 && limits.arguments <= 128 && limits.inputBytes != 0 &&
                   limits.inputBytes <= 65536 && limits.metadataBytes <= 65536 && limits.historyEntries <= 10000 &&
                   limits.outputBytes <= 65536 && limits.totalMetadataBytes <= (16U << 20U) && limits.totalHistoryBytes <= (16U << 20U) &&
                   policy.shippingAllowlist.size() <= limits.commands && policy.startupAllowlist.size() <= limits.commands &&
                   policy.shippingVariableAllowlist.size() <= limits.commands && policy.projectNamespaces.size() <= limits.commands &&
                   static_cast<unsigned>(policy.profile) <= 5;
        }

        /** @brief Subtracts admitted bytes without overflow. */
        bool Charge(std::size_t &remaining, const std::size_t bytes) {
            if (bytes > remaining)
                return false;
            remaining -= bytes;
            return true;
        }

        /** @brief Checks unique project namespace ownership independently of command inventory order. */
        Result<void> ValidateNamespaces(const RegistryPolicy &policy, std::size_t &remaining) {
            for (std::size_t i = 0; i < policy.projectNamespaces.size(); ++i) {
                const auto &name = policy.projectNamespaces[i];
                if (!Charge(remaining, name.size()))
                    return Result<void>::Failure(MakeCommandError(CommandError::LimitExceeded));
                if (!Identifier(name) || name.find('.') != std::string::npos || name.size() > policy.limits.metadataBytes ||
                    std::ranges::find(EngineNamespaces, name) != std::end(EngineNamespaces))
                    return Result<void>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
                const auto earlier = std::span(policy.projectNamespaces).first(i);
                if (std::ranges::find(earlier, name) != earlier.end())
                    return Result<void>::Failure(MakeCommandError(CommandError::Duplicate));
            }
            return Result<void>::Success();
        }

        /** @brief Charges all nested descriptor text before any owned copy is created. */
        bool ChargeDescriptor(const DebugCommandDescriptor &descriptor, std::size_t &remaining) {
            if (!Charge(remaining, descriptor.name.size()) || !Charge(remaining, descriptor.summary.size()) ||
                !Charge(remaining, descriptor.syntax.size()))
                return false;
            for (const auto &argument : descriptor.arguments) {
                if (!Charge(remaining, argument.name.size()) || !Charge(remaining, argument.description.size()) ||
                    !Charge(remaining, argument.defaultValue.size()))
                    return false;
                for (const auto value : argument.enumeration)
                    if (!Charge(remaining, value.size()))
                        return false;
            }
            return true;
        }

        /** @brief A callable command cannot simultaneously own a nested namespace. */
        bool Collides(const DebugCommandDescriptor &left, const DebugCommandDescriptor &right) {
            const auto a = left.name;
            const auto b = right.name;
            return left.id == right.id || a == b || (a.starts_with(b) && a.size() > b.size() && a[b.size()] == '.') ||
                   (b.starts_with(a) && b.size() > a.size() && b[a.size()] == '.');
        }

        /** @brief Validates the entire batch, including excluded descriptors, before activation. */
        Result<void> ValidateDescriptorSet(std::span<const DebugCommandDescriptor> descriptors, const RegistryPolicy &policy,
                                           std::size_t &remaining) {
            for (std::size_t i = 0; i < descriptors.size(); ++i) {
                const auto &descriptor = descriptors[i];
                if (!Valid(descriptor, policy.limits) || !RegisteredName(descriptor.name, policy))
                    return Result<void>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
                if (!ChargeDescriptor(descriptor, remaining))
                    return Result<void>::Failure(MakeCommandError(CommandError::LimitExceeded));
                for (const auto &earlier : descriptors.first(i))
                    if (Collides(descriptor, earlier))
                        return Result<void>::Failure(MakeCommandError(CommandError::Duplicate));
            }
            return Result<void>::Success();
        }

        /** @brief Requires unique exact known IDs and safe Shipping inclusion declarations. */
        Result<void> ValidateAllowlist(std::span<const DebugCommandId> ids, std::span<const DebugCommandDescriptor> descriptors,
                                       const bool shipping) {
            for (std::size_t i = 0; i < ids.size(); ++i) {
                const auto found = std::ranges::find(descriptors, ids[i], &DebugCommandDescriptor::id);
                if (found == descriptors.end() || (shipping && found->availability != CommandAvailability::ShippingAllowlist))
                    return Result<void>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
                if (std::ranges::find(ids.first(i), ids[i]) != ids.first(i).end())
                    return Result<void>::Failure(MakeCommandError(CommandError::Duplicate));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc Detail::TokenizeConsole */
    Result<std::vector<std::string>> Detail::TokenizeConsole(std::string_view input, const ConsoleLimits &limits) {
        return Tokenize(input, limits);
    }

    /** @copydoc Detail::ConvertConsoleArgument */
    Result<ArgumentValue> Detail::ConvertConsoleArgument(const DebugArgumentDescriptor &argument, std::string_view text) {
        return Convert(argument, text);
    }

    struct CommandRegistry::State final {
        RegistryPolicy policy;
        std::vector<OwnedCommand> commands;
    };

    /** @copydoc IsConsoleDescriptorIncluded */
    bool IsConsoleDescriptorIncluded(const DebugCommandId id, const CommandAvailability availability, const RegistryPolicy &policy) {
        if (static_cast<unsigned>(policy.profile) > 5)
            return false;
        DebugCommandDescriptor descriptor;
        descriptor.id = id;
        descriptor.availability = availability;
        return Included(descriptor, policy);
    }

    /** @copydoc IsValidConsoleIdentifier */
    bool IsValidConsoleIdentifier(const std::string_view text) {
        return Identifier(text);
    }

    /** @copydoc IsConsolePermissionGranted */
    bool IsConsolePermissionGranted(const CommandPermission permission, const DebugCommandContext &context, const ProductProfile profile) {
        if (static_cast<unsigned>(profile) > 5)
            return false;
        const auto value = static_cast<std::uint32_t>(permission);
        if (value != 1U && value != 2U && value != 4U && value != 8U)
            return false;
        DebugCommandDescriptor descriptor;
        descriptor.permissions = permission;
        return Authorized(descriptor, context, profile);
    }

    namespace {
        /** @brief Single stable error catalog shared by typed failures and explicit module registration. */
        const std::array<ErrorCodeDescriptor, 12> &ErrorDescriptors() {
            static const auto descriptors = [] {
                constexpr std::string_view codes[]{"invalid_descriptor", "duplicate",          "usage",          "permission_denied",
                                                   "limit_exceeded",     "unknown_command",    "startup_denied", "wrong_thread",
                                                   "shutting_down",      "missing_capability", "handler_failed", "reentrant_dispatch"};
                std::array<ErrorCodeDescriptor, 12> result{};
                for (std::size_t i = 0; i < result.size(); ++i)
                    result[i] = {ErrorDomainId("horo.runtime.console"),
                                 ErrorCode("console." + std::string(codes[i])),
                                 ErrorSeverity::Error,
                                 "Console request was rejected.",
                                 "Review the allowed descriptor schema, grants, budgets and lifecycle.",
                                 false,
                                 true,
                                 {}};
                return result;
            }();
            return descriptors;
        }
    }  // namespace

    /** @copydoc MakeCommandError */
    Error MakeCommandError(const CommandError error) {
        const auto index = static_cast<std::size_t>(error);
        const auto &descriptors = ErrorDescriptors();
        return MakeError(descriptors[index < descriptors.size() ? index : 0]);
    }

    /** @copydoc ConsoleErrorDomain */
    ModuleErrorDomainDescriptor ConsoleErrorDomain() {
        ModuleErrorDomainDescriptor domain{ErrorDomainId("horo.runtime.console"), {}};
        for (const auto &descriptor : ErrorDescriptors())
            domain.descriptors.push_back(&descriptor);
        return domain;
    }

    CommandRegistry::CommandRegistry(std::unique_ptr<State> state) : state_(std::move(state)) {}

    CommandRegistry::~CommandRegistry() = default;
    CommandRegistry::CommandRegistry(CommandRegistry &&) noexcept = default;
    CommandRegistry &CommandRegistry::operator=(CommandRegistry &&) noexcept = default;

    /** @copydoc CommandRegistry::Create */
    Result<CommandRegistry> CommandRegistry::Create(const std::span<const DebugCommandDescriptor> descriptors,
                                                    const RegistryPolicy &policy) {
        if (!ValidPolicy(policy, descriptors.size()))
            return Result<CommandRegistry>::Failure(MakeCommandError(CommandError::LimitExceeded));
        std::size_t remaining = policy.limits.totalMetadataBytes;
        const auto namespaces = ValidateNamespaces(policy, remaining);
        if (namespaces.HasError())
            return Result<CommandRegistry>::Failure(namespaces.ErrorValue());
        const auto metadata = ValidateDescriptorSet(descriptors, policy, remaining);
        if (metadata.HasError())
            return Result<CommandRegistry>::Failure(metadata.ErrorValue());
        const auto shipping = ValidateAllowlist(policy.shippingAllowlist, descriptors, true);
        if (shipping.HasError())
            return Result<CommandRegistry>::Failure(shipping.ErrorValue());
        const auto startup = ValidateAllowlist(policy.startupAllowlist, descriptors, false);
        if (startup.HasError())
            return Result<CommandRegistry>::Failure(startup.ErrorValue());
        auto state = std::make_unique<State>();
        state->policy = policy;
        state->commands.reserve(descriptors.size());
        for (const auto &descriptor : descriptors) {
            if (!Included(descriptor, policy))
                continue;
            OwnedCommand command;
            command.name = descriptor.name;
            command.summary = descriptor.summary;
            command.syntax = descriptor.syntax;
            command.descriptor = descriptor;
            command.arguments.assign(descriptor.arguments.begin(), descriptor.arguments.end());
            for (const auto &arg : descriptor.arguments) {
                OwnedArgument owned;
                owned.name = arg.name;
                owned.description = arg.description;
                owned.defaultValue = arg.defaultValue;
                for (const auto value : arg.enumeration)
                    owned.enumeration.emplace_back(value);
                command.storage.push_back(std::move(owned));
            }
            state->commands.push_back(std::move(command));
        }
        std::ranges::sort(state->commands, {}, &OwnedCommand::name);
        for (auto &command : state->commands) {
            command.descriptor.name = command.name;
            command.descriptor.summary = command.summary;
            command.descriptor.syntax = command.syntax;
            for (std::size_t i = 0; i < command.arguments.size(); ++i) {
                auto &arg = command.arguments[i];
                auto &owned = command.storage[i];
                arg.name = owned.name;
                arg.description = owned.description;
                arg.defaultValue = owned.defaultValue;
                for (const auto &value : owned.enumeration)
                    owned.enumViews.push_back(value);
                arg.enumeration = owned.enumViews;
            }
            command.descriptor.arguments = command.arguments;
        }
        return Result<CommandRegistry>::Success(CommandRegistry(std::move(state)));
    }

    /** @copydoc CommandRegistry::Prepare */
    Result<CommandInvocation> CommandRegistry::Prepare(const std::string_view input, const DebugCommandContext &context,
                                                       const bool startup) const {
        if (!state_)
            return Result<CommandInvocation>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
        auto parsed = Tokenize(input, state_->policy.limits);
        if (parsed.HasError())
            return Result<CommandInvocation>::Failure(parsed.ErrorValue());
        const auto &tokens = parsed.Value();
        const auto match = std::ranges::find(state_->commands, tokens.front(), &OwnedCommand::name);
        if (match == state_->commands.end())
            return Result<CommandInvocation>::Failure(MakeCommandError(CommandError::UnknownCommand));
        const auto &command = match->descriptor;
        if (!Authorized(command, context, state_->policy.profile))
            return Result<CommandInvocation>::Failure(MakeCommandError(CommandError::PermissionDenied));
        if (startup && std::ranges::find(state_->policy.startupAllowlist, command.id) == state_->policy.startupAllowlist.end())
            return Result<CommandInvocation>::Failure(MakeCommandError(CommandError::StartupDenied));
        std::vector<std::optional<std::string_view>> values(command.arguments.size());
        std::size_t positional = 0;
        for (std::size_t i = 1; i < tokens.size(); ++i) {
            std::size_t index = command.arguments.size();
            while (positional < command.arguments.size() && command.arguments[positional].option)
                ++positional;
            const bool explicitOption = tokens[i].starts_with("--");
            if (explicitOption || positional == command.arguments.size()) {
                const std::string_view key = explicitOption ? std::string_view(tokens[i]).substr(2) : std::string_view(tokens[i]);
                for (std::size_t j = 0; j < command.arguments.size(); ++j)
                    if (command.arguments[j].option && command.arguments[j].name == key)
                        index = j;
                if (index == command.arguments.size() || ++i == tokens.size())
                    return Result<CommandInvocation>::Failure(MakeCommandError(CommandError::Usage));
            } else {
                index = positional++;
            }
            if (index >= values.size() || values[index])
                return Result<CommandInvocation>::Failure(MakeCommandError(CommandError::Usage));
            values[index] = tokens[i];
        }
        CommandInvocation invocation{command.id, {}, std::string(command.name)};
        for (std::size_t i = 0; i < values.size(); ++i) {
            const auto &arg = command.arguments[i];
            if (!values[i] && arg.required)
                return Result<CommandInvocation>::Failure(MakeCommandError(CommandError::Usage));
            const auto value = values[i].value_or(arg.defaultValue);
            auto converted = Convert(arg, value);
            if (converted.HasError())
                return Result<CommandInvocation>::Failure(converted.ErrorValue());
            invocation.arguments.push_back(std::move(converted).Value());
            invocation.redactedText += " ";
            invocation.redactedText += arg.name;
            invocation.redactedText += "=";
            if (arg.sensitive || (static_cast<unsigned>(command.flags) & 2U) != 0)
                invocation.redactedText += "[REDACTED]";
            else
                invocation.redactedText.append(value);
        }
        invocation.redactedText.resize(Detail::ConsoleTextPrefix(invocation.redactedText, state_->policy.limits.inputBytes).size());
        return Result<CommandInvocation>::Success(std::move(invocation));
    }

    /** @copydoc CommandRegistry::Discover */
    std::vector<DebugCommandDescriptor> CommandRegistry::Discover(const std::string_view text, const DebugCommandContext &context,
                                                                  const bool prefix) const {
        std::vector<DebugCommandDescriptor> result;
        if (!state_)
            return result;
        for (const auto &command : state_->commands) {
            if (!Authorized(command.descriptor, context, state_->policy.profile))
                continue;
            if (prefix ? command.name.starts_with(text)
                       : (command.name.find(text) != std::string::npos || command.summary.find(text) != std::string::npos))
                result.push_back(command.descriptor);
        }
        return result;
    }

    /** @copydoc CommandRegistry::Resolve */
    Result<DebugCommandDescriptor> CommandRegistry::Resolve(const DebugCommandId id, const DebugCommandContext &context) const {
        if (!state_)
            return Result<DebugCommandDescriptor>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
        for (const auto &command : state_->commands) {
            if (command.descriptor.id != id)
                continue;
            if (!Authorized(command.descriptor, context, state_->policy.profile))
                return Result<DebugCommandDescriptor>::Failure(MakeCommandError(CommandError::PermissionDenied));
            return Result<DebugCommandDescriptor>::Success(command.descriptor);
        }
        return Result<DebugCommandDescriptor>::Failure(MakeCommandError(CommandError::UnknownCommand));
    }

    /** @copydoc CommandRegistry::Limits */
    const ConsoleLimits &CommandRegistry::Limits() const noexcept {
        static constexpr ConsoleLimits unavailable{0, 0, 0, 0, 0, 0, 0, 0};
        if (!state_)
            return unavailable;
        return state_->policy.limits;
    }

    /** @copydoc CommandRegistry::Profile */
    std::optional<ProductProfile> CommandRegistry::Profile() const noexcept {
        if (!state_)
            return std::nullopt;
        return state_->policy.profile;
    }

    /** @copydoc ConsoleHistory::ConsoleHistory */
    ConsoleHistory::ConsoleHistory(ConsoleLimits limits) : limits_(limits) {
        limits_.historyEntries = std::min(limits_.historyEntries, std::size_t{10000});
        limits_.inputBytes = std::min(limits_.inputBytes, std::size_t{65536});
        limits_.outputBytes = std::min(limits_.outputBytes, std::size_t{65536});
        limits_.totalHistoryBytes = std::min(limits_.totalHistoryBytes, std::size_t{16U << 20U});
        entries_.reserve(limits_.historyEntries);
    }

    /** @copydoc ConsoleHistory::Append */
    void ConsoleHistory::Append(const CommandInvocation &invocation, const std::string_view output) {
        if (limits_.historyEntries == 0 || limits_.totalHistoryBytes == 0)
            return;
        const auto commandBytes =
            Detail::ConsoleTextPrefix(invocation.redactedText, std::min(limits_.inputBytes, limits_.totalHistoryBytes)).size();
        const auto outputBytes =
            Detail::ConsoleTextPrefix(output, std::min(limits_.outputBytes, limits_.totalHistoryBytes - commandBytes)).size();
        const auto bytes = commandBytes + outputBytes;
        while (!entries_.empty() && (entries_.size() == limits_.historyEntries || retainedBytes_ > limits_.totalHistoryBytes - bytes)) {
            retainedBytes_ -= entries_.front().command.size() + entries_.front().output.size();
            entries_.erase(entries_.begin());
        }
        entries_.push_back({invocation.redactedText.substr(0, commandBytes), std::string(output.substr(0, outputBytes))});
        retainedBytes_ += bytes;
    }

    /** @copydoc ConsoleHistory::Clear */
    void ConsoleHistory::Clear() noexcept {
        entries_.clear();
        retainedBytes_ = 0;
    }

    /** @copydoc ConsoleHistory::Entries */
    std::span<const HistoryEntry> ConsoleHistory::Entries() const noexcept {
        return entries_;
    }

    /** @copydoc ConsoleHistory::ReverseSearch */
    const HistoryEntry *ConsoleHistory::ReverseSearch(const std::string_view text, const std::size_t skip) const noexcept {
        if (skip >= entries_.size())
            return nullptr;
        for (std::size_t i = entries_.size() - skip; i > 0; --i)
            if (entries_[i - 1].command.find(text) != std::string::npos)
                return &entries_[i - 1];
        return nullptr;
    }
}  // namespace Horo::Debug
