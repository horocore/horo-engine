#include "Horo/Debug/ConsoleVariable.h"

#include "Horo/Foundation/Utf8.h"

#include <cmath>
#include <type_traits>

namespace Horo::Debug {
    namespace {
        /** @brief Converts only Foundation-supported value types, never coercing a float into an integer/string. */
        Result<SettingValue> ConfigurationValue(const ConsoleValue &value) {
            return std::visit([](const auto &item) -> Result<SettingValue> {
                using Value = std::decay_t<decltype(item)>;
                if constexpr (std::is_same_v<Value, double>)
                    return Result<SettingValue>::Failure(MakeCommandError(CommandError::Usage));
                else
                    return Result<SettingValue>::Success(item);
            }, value);
        }

        /** @brief Copies the canonical configuration value into a typed presentation projection. */
        ConsoleValue Project(const SettingValue &value) {
            return std::visit([](const auto &item) -> ConsoleValue {
                return item;
            }, value);
        }
    }  // namespace

    ConsoleVariable::ConsoleVariable(ConsoleVariableDescriptor descriptor, const ProductProfile profile,
                                     std::shared_ptr<ConfigurationService> configuration,
                                     std::shared_ptr<IConsoleVariablePersistence> persistence)
        : descriptor_(std::move(descriptor)), profile_(profile), configuration_(std::move(configuration)),
          persistence_(std::move(persistence)), owner_(std::this_thread::get_id()) {
        if (!descriptor_.configurationKey)
            sessionValue_ = descriptor_.defaultValue;
    }

    /** @copydoc ConsoleVariable::Create */
    Result<ConsoleVariable> ConsoleVariable::Create(ConsoleVariableDescriptor descriptor, const RegistryPolicy &policy,
                                                    std::shared_ptr<ConfigurationService> configuration,
                                                    std::shared_ptr<IConsoleVariablePersistence> persistence) {
        const auto permission = static_cast<unsigned>(descriptor.permission);
        if (static_cast<unsigned>(policy.profile) > 5 || !descriptor.id || !IsValidConsoleIdentifier(descriptor.name) ||
            descriptor.name.size() > 128 || descriptor.name.find('.') == std::string::npos || descriptor.maximumStringBytes > 65536 ||
            static_cast<unsigned>(descriptor.applyPolicy) > 4 || static_cast<unsigned>(descriptor.persistence) > 2 ||
            static_cast<unsigned>(descriptor.availability) > 3 ||
            (descriptor.configurationKey &&
             (descriptor.configurationKey->Value().empty() || descriptor.configurationKey->Value().size() > 128)) ||
            (permission != 1 && permission != 2 && permission != 4 && permission != 8) ||
            (descriptor.minimum && !std::isfinite(*descriptor.minimum)) || (descriptor.maximum && !std::isfinite(*descriptor.maximum)) ||
            (descriptor.minimum && descriptor.maximum && *descriptor.minimum > *descriptor.maximum) ||
            (descriptor.availability == CommandAvailability::ShippingAllowlist && descriptor.permission != CommandPermission::Public) ||
            (descriptor.permission == CommandPermission::AdminCheat && descriptor.availability != CommandAvailability::DevelopmentOnly))
            return Result<ConsoleVariable>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
        const auto valid = ValidateValue(descriptor, descriptor.defaultValue);
        if (valid.HasError())
            return Result<ConsoleVariable>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
        RegistryPolicy variablePolicy = policy;
        variablePolicy.shippingAllowlist = policy.shippingVariableAllowlist;
        if (!IsConsoleDescriptorIncluded(descriptor.id, descriptor.availability, variablePolicy))
            return Result<ConsoleVariable>::Failure(MakeCommandError(CommandError::PermissionDenied));
        if ((descriptor.configurationKey && !configuration) ||
            (descriptor.persistence != ConsoleVariablePersistence::Session && (!descriptor.configurationKey || !persistence)))
            return Result<ConsoleVariable>::Failure(MakeCommandError(CommandError::MissingCapability));
        ConsoleVariable variable(std::move(descriptor), policy.profile, std::move(configuration), std::move(persistence));
        if (variable.descriptor_.configurationKey) {
            const auto snapshot = variable.configuration_->Snapshot();
            const auto *current = snapshot.FindResolved(*variable.descriptor_.configurationKey);
            if (!current || current->sensitivity != SettingSensitivity::Public || variable.Validate(Project(current->value)).HasError())
                return Result<ConsoleVariable>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
        }
        return Result<ConsoleVariable>::Success(std::move(variable));
    }

    /** @copydoc ConsoleVariable::ValidateValue */
    Result<void> ConsoleVariable::ValidateValue(const ConsoleVariableDescriptor &descriptor, const ConsoleValue &value) {
        if (value.valueless_by_exception() || descriptor.defaultValue.valueless_by_exception() ||
            value.index() != descriptor.defaultValue.index())
            return Result<void>::Failure(MakeCommandError(CommandError::Usage));
        if (const auto text = std::get_if<std::string>(&value)) {
            if (text->size() > descriptor.maximumStringBytes || !IsValidUtf8ScalarSequence(*text) || descriptor.minimum ||
                descriptor.maximum)
                return Result<void>::Failure(MakeCommandError(CommandError::Usage));
        } else if (std::holds_alternative<bool>(value)) {
            if (descriptor.minimum || descriptor.maximum)
                return Result<void>::Failure(MakeCommandError(CommandError::Usage));
        } else {
            const long double numeric = std::holds_alternative<double>(value) ? static_cast<long double>(std::get<double>(value))
                                                                              : static_cast<long double>(std::get<std::int64_t>(value));
            if (!std::isfinite(numeric) || (descriptor.minimum && numeric < *descriptor.minimum) ||
                (descriptor.maximum && numeric > *descriptor.maximum))
                return Result<void>::Failure(MakeCommandError(CommandError::Usage));
        }
        if (descriptor.configurationKey && ConfigurationValue(value).HasError())
            return Result<void>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
        return Result<void>::Success();
    }

    /** @copydoc ConsoleVariable::Validate */
    Result<void> ConsoleVariable::Validate(const ConsoleValue &value) const {
        return ValidateValue(descriptor_, value);
    }

    /** @copydoc ConsoleVariable::Read */
    Result<ConsoleVariableSnapshot> ConsoleVariable::Read(const DebugCommandContext &context) const {
        if (std::this_thread::get_id() != owner_)
            return Result<ConsoleVariableSnapshot>::Failure(MakeCommandError(CommandError::WrongThread));
        if (!IsConsolePermissionGranted(descriptor_.permission, context, profile_))
            return Result<ConsoleVariableSnapshot>::Failure(MakeCommandError(CommandError::PermissionDenied));
        ConsoleValue active;
        if (descriptor_.configurationKey) {
            const auto snapshot = configuration_->Snapshot();
            const auto *current = snapshot.FindResolved(*descriptor_.configurationKey);
            if (!current || current->sensitivity != SettingSensitivity::Public || Validate(Project(current->value)).HasError())
                return Result<ConsoleVariableSnapshot>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
            active = Project(current->value);
        } else
            active = *sessionValue_;
        auto pending = pending_;
        if (descriptor_.persistence != ConsoleVariablePersistence::Session) {
            const auto stored = persistence_->Pending(*descriptor_.configurationKey);
            if (stored && Validate(Project(*stored)).HasError())
                return Result<ConsoleVariableSnapshot>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
            if (stored)
                pending = Project(*stored);
        }
        return Result<ConsoleVariableSnapshot>::Success({std::move(active), std::move(pending), descriptor_.applyPolicy});
    }

    /** @copydoc ConsoleVariable::Commit */
    Result<void> ConsoleVariable::Commit(const ConsoleValue &value, const std::optional<ConfigurationRevision> expectedRevision) {
        if (!descriptor_.configurationKey) {
            sessionValue_ = value;
            return Result<void>::Success();
        }
        auto converted = ConfigurationValue(value);
        if (converted.HasError())
            return Result<void>::Failure(converted.ErrorValue());
        ConfigurationDraft draft;
        if (!expectedRevision)
            return Result<void>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
        draft.baseRevision = *expectedRevision;
        draft.proposedValues.emplace(*descriptor_.configurationKey, converted.Value());
        return configuration_->Commit(draft);
    }

    /** @copydoc ConsoleVariable::Set */
    Result<void> ConsoleVariable::Set(ConsoleValue value, const DebugCommandContext &context) {
        if (std::this_thread::get_id() != owner_)
            return Result<void>::Failure(MakeCommandError(CommandError::WrongThread));
        if (!IsConsolePermissionGranted(descriptor_.permission, context, profile_) ||
            descriptor_.applyPolicy == ConsoleVariableApplyPolicy::ReadOnly)
            return Result<void>::Failure(MakeCommandError(CommandError::PermissionDenied));
        const auto valid = Validate(value);
        if (valid.HasError())
            return valid;
        std::optional<ConfigurationRevision> revision;
        if (descriptor_.configurationKey) {
            const auto converted = ConfigurationValue(value);
            ConfigurationDraft draft{configuration_->Snapshot().Revision(), {{*descriptor_.configurationKey, converted.Value()}}};
            const auto admitted = configuration_->Validate(draft);
            if (admitted.HasError())
                return admitted;
            revision = draft.baseRevision;
            if (descriptor_.persistence != ConsoleVariablePersistence::Session) {
                return persistence_->Store(*descriptor_.configurationKey, converted.Value(), descriptor_.persistence,
                                           descriptor_.applyPolicy, draft.baseRevision);
            }
        }
        if (descriptor_.applyPolicy == ConsoleVariableApplyPolicy::Immediate)
            return Commit(value, revision);
        pending_ = std::move(value);
        pendingRevision_ = revision;
        return Result<void>::Success();
    }

    /** @copydoc ConsoleVariable::Apply */
    Result<bool> ConsoleVariable::Apply(const ConsoleVariableApplyPolicy boundary) {
        if (std::this_thread::get_id() != owner_)
            return Result<bool>::Failure(MakeCommandError(CommandError::WrongThread));
        if (descriptor_.persistence != ConsoleVariablePersistence::Session)
            return persistence_->Apply(*descriptor_.configurationKey, boundary);
        if (!pending_ || boundary != descriptor_.applyPolicy)
            return Result<bool>::Success(false);
        const auto committed = Commit(*pending_, pendingRevision_);
        if (committed.HasError())
            return Result<bool>::Failure(committed.ErrorValue());
        pending_.reset();
        pendingRevision_.reset();
        return Result<bool>::Success(true);
    }

    /** @copydoc ConsoleVariable::Descriptor */
    const ConsoleVariableDescriptor &ConsoleVariable::Descriptor() const noexcept {
        return descriptor_;
    }
}  // namespace Horo::Debug
