#include "Horo/Editor/EditorConfiguration.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "editor/EditorServiceErrors.h"

#include <algorithm>
#include <charconv>

namespace Horo::Editor {
    namespace {
        /** @brief Formats only ordinary primitive settings for editing. */
        std::string FieldText(const SettingValue &value) {
            if (const auto *text = std::get_if<std::string>(&value))
                return *text;
            if (const auto *integer = std::get_if<std::int64_t>(&value))
                return std::to_string(*integer);
            if (const auto *boolean = std::get_if<bool>(&value))
                return *boolean ? "true" : "false";
            return {};
        }

        /** @brief Parses a bounded ordinary field without reflecting its input in errors. */
        Result<SettingValue> ParseField(const SettingDescriptor &descriptor, const std::string_view text) {
            if (descriptor.type == SettingValueType::String && text.size() <= ConfigurationLimits{}.maximumStringValueBytes)
                return Result<SettingValue>::Success(std::string{text});
            if (descriptor.type == SettingValueType::Boolean) {
                if (text == "true")
                    return Result<SettingValue>::Success(true);
                if (text == "false")
                    return Result<SettingValue>::Success(false);
            }
            if (descriptor.type == SettingValueType::Integer) {
                std::int64_t value{};
                const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
                if (error == std::errc{} && end == text.data() + text.size())
                    return Result<SettingValue>::Success(value);
            }
            return Result<SettingValue>::Failure(MakeError(SettingsErrors::ValidationFailed));
        }
    }  // namespace

    /** @copydoc ConfigurationSettingsSession::ConfigurationSettingsSession */
    ConfigurationSettingsSession::ConfigurationSettingsSession(ConfigurationService &configuration) : configuration_(configuration) {
        Reload();
    }

    /** @copydoc ConfigurationSettingsSession::ConfigurationSettingsSession */
    ConfigurationSettingsSession::ConfigurationSettingsSession(EditorSettingsService &settings)
        : configuration_(settings.Configuration()), settings_(&settings) {
        Reload();
    }

    /** @copydoc ConfigurationSettingsSession::ConfigurationSettingsSession */
    ConfigurationSettingsSession::ConfigurationSettingsSession(ConfigurationService &configuration,
                                                               ConfigurationSettingsPersistence persistence)
        : configuration_(configuration), persistence_(std::move(persistence)) {
        Reload();
    }

    /** @copydoc ConfigurationSettingsSession::CheckAvailable */
    Result<void> ConfigurationSettingsSession::CheckAvailable() const {
        if (!persistence_)
            return Result<void>::Success();
        if (!persistence_->checkAvailable || !persistence_->validate || !persistence_->persist || persistence_->keys.empty() ||
            persistence_->keys.size() > ConfigurationLimits{}.maximumKeysPerSource ||
            std::ranges::any_of(persistence_->keys, [this](const SettingKey &key) {
            return configuration_.Schema().FindDescriptor(key) == nullptr;
        }))
            return Result<void>::Failure(MakeError(SettingsErrors::ValidationFailed));
        return persistence_->checkAvailable();
    }

    /** @copydoc ConfigurationSettingsSession::ValidateSource */
    Result<void> ConfigurationSettingsSession::ValidateSource(const ConfigurationSnapshot &snapshot) const {
        auto available = CheckAvailable();
        if (available.HasError())
            return available;
        return persistence_ ? persistence_->validate(snapshot) : Result<void>::Success();
    }

    /** @copydoc ConfigurationSettingsSession::EditBuffer */
    std::string *ConfigurationSettingsSession::EditBuffer(const SettingKey &key) noexcept {
        const auto field = std::ranges::find(fields_, key, [](const ConfigurationSettingField &value) -> const SettingKey & {
            return value.descriptor.key;
        });
        return field == fields_.end() || !field->editable ? nullptr : &field->text;
    }

    /** @copydoc ConfigurationSettingsSession::Fields */
    std::span<const ConfigurationSettingField> ConfigurationSettingsSession::Fields() const noexcept {
        return fields_;
    }

    /** @copydoc ConfigurationSettingsSession::Draft */
    const ConfigurationDraft &ConfigurationSettingsSession::Draft() const noexcept {
        return draft_;
    }

    /** @copydoc ConfigurationSettingsSession::Preview */
    const std::optional<ConfigurationSnapshot> &ConfigurationSettingsSession::Preview() const noexcept {
        return preview_;
    }

    /** @copydoc ConfigurationSettingsSession::Failure */
    const std::optional<Error> &ConfigurationSettingsSession::Failure() const noexcept {
        return failure_;
    }

    /** @copydoc ConfigurationSettingsSession::Reload */
    void ConfigurationSettingsSession::Reload() {
        auto available = CheckAvailable();
        if (available.HasError()) {
            fields_.clear();
            draft_ = {};
            preview_.reset();
            failure_ = std::move(available).ErrorValue();
            return;
        }
        const auto snapshot = configuration_.Snapshot();
        const auto source = persistence_ ? persistence_->source : ConfigurationSource::User;
        draft_ = {.baseRevision = snapshot.Revision(), .source = source};
        fields_.clear();
        for (const auto &descriptor : configuration_.Schema().Descriptors()) {
            if (persistence_ && std::ranges::find(persistence_->keys, descriptor.key) == persistence_->keys.end())
                continue;
            const auto *resolved = snapshot.FindResolved(descriptor.key);
            const bool editable = !descriptor.readOnly && descriptor.sensitivity == SettingSensitivity::Public &&
                                  descriptor.scope != SettingScope::Invocation && ConfigurationSchema::AllowsSource(descriptor, source);
            const auto authored = std::ranges::find(resolved->candidates, source, &ConfigurationCandidate::Source);
            const SettingValue &value = authored == resolved->candidates.end() ? resolved->value : authored->Value();
            fields_.push_back({.descriptor = descriptor,
                               .resolved = *resolved,
                               .editable = editable,
                               .text = editable ? FieldText(value) : std::string{}});
        }
        preview_.reset();
        failure_.reset();
    }

    /** @copydoc ConfigurationSettingsSession::Edit */
    Result<void> ConfigurationSettingsSession::Edit(const SettingKey &key, std::string text) {
        auto available = CheckAvailable();
        if (available.HasError()) {
            preview_.reset();
            failure_ = available.ErrorValue();
            return available;
        }
        const auto field = std::ranges::find(fields_, key, [](const ConfigurationSettingField &value) -> const SettingKey & {
            return value.descriptor.key;
        });
        if (field == fields_.end() || !field->editable)
            return Result<void>::Failure(MakeError(SettingsErrors::ValidationFailed));
        if (text.size() > ConfigurationLimits{}.maximumStringValueBytes) {
            field->error = MakeError(SettingsErrors::ValidationFailed);
            preview_.reset();
            return Result<void>::Failure(*field->error);
        }
        field->text = std::move(text);
        auto value = ParseField(field->descriptor, field->text);
        field->error.reset();
        preview_.reset();
        failure_.reset();
        if (value.HasError()) {
            field->error = value.ErrorValue();
            return Result<void>::Failure(*field->error);
        }
        ConfigurationDraft candidate = draft_;
        candidate.proposedValues.insert_or_assign(key, std::move(value).Value());
        auto preview = configuration_.Preview(candidate);
        if (preview.HasError()) {
            const auto error = preview.ErrorValue();
            if (error.code.Value() == "configuration.draft_stale")
                Reload();
            else
                field->error = error;
            failure_ = error;
            return Result<void>::Failure(error);
        }
        draft_ = std::move(candidate);
        auto validated = ValidateSource(preview.Value());
        if (validated.HasError()) {
            failure_ = validated.ErrorValue();
            return validated;
        }
        if (std::ranges::none_of(fields_, [](const ConfigurationSettingField &value) {
            return value.error.has_value();
        }))
            preview_ = std::move(preview).Value();
        return Result<void>::Success();
    }

    /** @copydoc ConfigurationSettingsSession::Apply */
    Result<void> ConfigurationSettingsSession::Apply() {
        auto available = CheckAvailable();
        if (available.HasError()) {
            preview_.reset();
            failure_ = available.ErrorValue();
            return available;
        }
        for (const auto &field : fields_) {
            if (field.error)
                return Result<void>::Failure(*field.error);
        }
        if (draft_.proposedValues.empty())
            return Result<void>::Success();
        Result<void> result = Result<void>::Success();
        if (persistence_) {
            auto prepared = configuration_.Prepare(draft_);
            if (prepared.HasError()) {
                result = Result<void>::Failure(std::move(prepared).ErrorValue());
            } else {
                const auto candidate = prepared.Value().Candidate();
                result = ValidateSource(candidate);
                if (result.HasValue())
                    result = persistence_->persist(candidate);
                if (result.HasValue())
                    result = std::move(prepared).Value().Commit();
            }
        } else if (settings_) {
            const auto committed = settings_->CommitConfiguration(draft_);
            if (committed.HasError())
                result = Result<void>::Failure(committed.ErrorValue());
        } else {
            result = configuration_.Commit(draft_);
        }
        if (result.HasError()) {
            const auto error = result.ErrorValue();
            if (error.code.Value() == "configuration.draft_stale")
                Reload();
            failure_ = error;
            return Result<void>::Failure(error);
        }
        Reload();
        return result;
    }

    /** @copydoc ConfigurationSettingsSession::RestoreDefaults */
    Result<void> ConfigurationSettingsSession::RestoreDefaults() {
        Reload();
        if (failure_)
            return Result<void>::Failure(*failure_);
        for (auto &field : fields_) {
            if (!field.editable)
                continue;
            field.text = FieldText(field.descriptor.defaultValue);
            draft_.proposedValues.emplace(field.descriptor.key, field.descriptor.defaultValue);
        }
        auto preview = configuration_.Preview(draft_);
        if (preview.HasError()) {
            const auto error = preview.ErrorValue();
            Reload();
            failure_ = error;
            return Result<void>::Failure(error);
        }
        auto validated = ValidateSource(preview.Value());
        if (validated.HasError()) {
            failure_ = validated.ErrorValue();
            return validated;
        }
        preview_ = std::move(preview).Value();
        return Result<void>::Success();
    }

    /** @copydoc ConfigurationSettingsSession::Cancel */
    void ConfigurationSettingsSession::Cancel() {
        Reload();
    }
}  // namespace Horo::Editor
