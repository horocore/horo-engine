#include "Horo/Debug/ConsoleVariableSet.h"

#include "ConsoleParse.h"
#include "ConsoleText.h"

#include <algorithm>

namespace Horo::Debug {
    namespace {
        /** @brief Terminal descriptor names cannot simultaneously own a namespace prefix. */
        bool NamesCollide(std::string_view left, std::string_view right) {
            return left == right || (left.size() > right.size() && left.starts_with(right) && left[right.size()] == '.') ||
                   (right.size() > left.size() && right.starts_with(left) && right[left.size()] == '.');
        }

        /** @brief Checks namespace/ID ownership across the complete command and variable composition. */
        Result<void> ValidateInventory(std::span<const ConsoleVariableDescriptor> descriptors,
                                       std::span<const DebugCommandDescriptor> commands, const RegistryPolicy &policy) {
            if (descriptors.size() > policy.limits.commands || policy.limits.commands > 4096 ||
                policy.shippingVariableAllowlist.size() > policy.limits.commands || static_cast<unsigned>(policy.profile) > 5 ||
                policy.limits.totalMetadataBytes > (16U << 20U))
                return Result<void>::Failure(MakeCommandError(CommandError::LimitExceeded));
            std::size_t remaining = policy.limits.totalMetadataBytes;
            for (std::size_t i = 0; i < descriptors.size(); ++i) {
                const auto &descriptor = descriptors[i];
                std::size_t bytes = descriptor.name.size();
                if (const auto value = std::get_if<std::string>(&descriptor.defaultValue)) {
                    if (value->size() > remaining)
                        return Result<void>::Failure(MakeCommandError(CommandError::LimitExceeded));
                    remaining -= value->size();
                }
                if (descriptor.configurationKey) {
                    if (descriptor.configurationKey->Value().size() > remaining)
                        return Result<void>::Failure(MakeCommandError(CommandError::LimitExceeded));
                    remaining -= descriptor.configurationKey->Value().size();
                }
                if (bytes > remaining)
                    return Result<void>::Failure(MakeCommandError(CommandError::LimitExceeded));
                remaining -= bytes;
                for (const auto &earlier : descriptors.first(i))
                    if (earlier.id == descriptor.id || NamesCollide(earlier.name, descriptor.name))
                        return Result<void>::Failure(MakeCommandError(CommandError::Duplicate));
                for (const auto &command : commands)
                    if (command.id == descriptor.id || NamesCollide(command.name, descriptor.name))
                        return Result<void>::Failure(MakeCommandError(CommandError::Duplicate));
            }
            for (std::size_t i = 0; i < policy.shippingVariableAllowlist.size(); ++i) {
                const auto id = policy.shippingVariableAllowlist[i];
                const auto found = std::ranges::find(descriptors, id, &ConsoleVariableDescriptor::id);
                if (found == descriptors.end() || found->availability != CommandAvailability::ShippingAllowlist ||
                    found->permission != CommandPermission::Public)
                    return Result<void>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
                const auto earlier = std::span(policy.shippingVariableAllowlist).first(i);
                if (std::ranges::find(earlier, id) != earlier.end())
                    return Result<void>::Failure(MakeCommandError(CommandError::Duplicate));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ConsoleVariableSet::Create */
    Result<ConsoleVariableSet> ConsoleVariableSet::Create(std::span<const ConsoleVariableDescriptor> descriptors,
                                                          std::span<const DebugCommandDescriptor> commands, const RegistryPolicy &policy,
                                                          std::shared_ptr<ConfigurationService> configuration,
                                                          std::shared_ptr<IConsoleVariablePersistence> persistence) {
        const auto valid = ValidateInventory(descriptors, commands, policy);
        if (valid.HasError())
            return Result<ConsoleVariableSet>::Failure(valid.ErrorValue());
        ConsoleVariableSet set;
        set.profile_ = policy.profile;
        set.variables_.reserve(descriptors.size());
        for (const auto &descriptor : descriptors) {
            auto created = ConsoleVariable::Create(descriptor, policy, configuration, persistence);
            if (created.HasError()) {
                if (created.ErrorValue().code.Value() == "console.permission_denied")
                    continue;
                return Result<ConsoleVariableSet>::Failure(created.ErrorValue());
            }
            set.variables_.push_back(std::move(created).Value());
        }
        std::ranges::sort(set.variables_, [](const auto &left, const auto &right) {
            return left.Descriptor().name < right.Descriptor().name;
        });
        return Result<ConsoleVariableSet>::Success(std::move(set));
    }

    /** @copydoc ConsoleVariableSet::Read */
    Result<ConsoleVariableSnapshot> ConsoleVariableSet::Read(std::string_view name, const DebugCommandContext &principal) const {
        if (std::this_thread::get_id() != owner_)
            return Result<ConsoleVariableSnapshot>::Failure(MakeCommandError(CommandError::WrongThread));
        for (const auto &variable : variables_)
            if (variable.Descriptor().name == name)
                return variable.Read(principal);
        return Result<ConsoleVariableSnapshot>::Failure(MakeCommandError(CommandError::UnknownCommand));
    }

    /** @copydoc ConsoleVariableSet::Set */
    Result<void> ConsoleVariableSet::Set(std::string_view name, ConsoleValue value, const DebugCommandContext &principal) {
        if (std::this_thread::get_id() != owner_)
            return Result<void>::Failure(MakeCommandError(CommandError::WrongThread));
        for (auto &variable : variables_)
            if (variable.Descriptor().name == name)
                return variable.Set(std::move(value), principal);
        return Result<void>::Failure(MakeCommandError(CommandError::UnknownCommand));
    }

    /** @copydoc ConsoleVariableSet::Apply */
    Result<std::size_t> ConsoleVariableSet::Apply(ConsoleVariableApplyPolicy boundary) {
        if (std::this_thread::get_id() != owner_)
            return Result<std::size_t>::Failure(MakeCommandError(CommandError::WrongThread));
        std::size_t count{};
        for (auto &variable : variables_) {
            auto applied = variable.Apply(boundary);
            if (applied.HasError())
                return Result<std::size_t>::Failure(applied.ErrorValue());
            if (applied.Value())
                ++count;
        }
        return Result<std::size_t>::Success(count);
    }

    /** @copydoc ConsoleVariableSet::Prepare */
    Result<PreparedConsoleVariable> ConsoleVariableSet::Prepare(std::string_view input, const DebugCommandContext &principal,
                                                                const ConsoleLimits &limits) const {
        if (limits.inputBytes == 0 || limits.inputBytes > 65536 || limits.arguments > 128)
            return Result<PreparedConsoleVariable>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
        auto tokens = Detail::TokenizeConsole(input, limits);
        if (tokens.HasError())
            return Result<PreparedConsoleVariable>::Failure(tokens.ErrorValue());
        const auto &parts = tokens.Value();
        const auto found = std::ranges::find_if(variables_, [&](const auto &variable) {
            return variable.Descriptor().name == parts[0];
        });
        if (found == variables_.end())
            return Result<PreparedConsoleVariable>::Failure(MakeCommandError(CommandError::UnknownCommand));
        const auto &descriptor = found->Descriptor();
        if (!IsConsolePermissionGranted(descriptor.permission, principal, profile_) ||
            (parts.size() > 1 && descriptor.applyPolicy == ConsoleVariableApplyPolicy::ReadOnly))
            return Result<PreparedConsoleVariable>::Failure(MakeCommandError(CommandError::PermissionDenied));
        if (parts.size() > 2)
            return Result<PreparedConsoleVariable>::Failure(MakeCommandError(CommandError::Usage));
        PreparedConsoleVariable result{descriptor.name, {descriptor.id, {}, descriptor.name}};
        if (parts.size() == 2) {
            constexpr DebugArgumentType types[]{DebugArgumentType::Bool, DebugArgumentType::Int64, DebugArgumentType::Float,
                                                DebugArgumentType::String};
            DebugArgumentDescriptor argument{"value", {}, types[descriptor.defaultValue.index()]};
            argument.minimum = descriptor.minimum;
            argument.maximum = descriptor.maximum;
            if (argument.type == DebugArgumentType::String && parts[1].size() > descriptor.maximumStringBytes)
                return Result<PreparedConsoleVariable>::Failure(MakeCommandError(CommandError::Usage));
            auto converted = Detail::ConvertConsoleArgument(argument, parts[1]);
            if (converted.HasError())
                return Result<PreparedConsoleVariable>::Failure(converted.ErrorValue());
            result.invocation.arguments.push_back(std::move(converted).Value());
            result.invocation.redactedText += " value=" + parts[1];
            result.invocation.redactedText.resize(Detail::ConsoleTextPrefix(result.invocation.redactedText, limits.inputBytes).size());
        }
        return Result<PreparedConsoleVariable>::Success(std::move(result));
    }

    /** @copydoc ConsoleVariableSet::Discover */
    std::vector<ConsoleVariableDescriptor> ConsoleVariableSet::Discover(std::string_view text, const DebugCommandContext &principal,
                                                                        const bool prefix) const {
        std::vector<ConsoleVariableDescriptor> descriptors;
        for (const auto &variable : variables_) {
            const auto &descriptor = variable.Descriptor();
            if (!IsConsolePermissionGranted(descriptor.permission, principal, profile_))
                continue;
            if (prefix ? descriptor.name.starts_with(text) : descriptor.name.find(text) != std::string::npos)
                descriptors.push_back(descriptor);
        }
        return descriptors;
    }

    /** @copydoc ConsoleVariableSet::Profile */
    ProductProfile ConsoleVariableSet::Profile() const noexcept {
        return profile_;
    }

    /** @copydoc ConsoleVariableSet::OwnerThread */
    std::thread::id ConsoleVariableSet::OwnerThread() const noexcept {
        return owner_;
    }
}  // namespace Horo::Debug
