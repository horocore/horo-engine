#include "Horo/XR/XRActionBindings.h"

#include <algorithm>
#include <limits>

namespace Horo::XR {
    namespace {
        /** @brief Creates a stable XR diagnostic while preserving actionable binding context. */
        Result<void> Invalid(const char *message) {
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid, message));
        }

        /** @brief Rejects semantic conflicts and incompatible persisted overrides. */
        Result<void> Incompatible(const char *message) {
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible, message));
        }

        /** @brief Restricts this initial binding contract to explicit controller roles. */
        bool Controller(const XRTrackedDeviceRole role) noexcept {
            return role == XRTrackedDeviceRole::LeftController || role == XRTrackedDeviceRole::RightController;
        }

        /** @brief Checks the existing canonical Input action value vocabulary. */
        bool ValueType(const Input::ActionValueType type) noexcept {
            return type == Input::ActionValueType::Digital || type == Input::ActionValueType::Axis1D ||
                   type == Input::ActionValueType::Axis2D;
        }

        /** @brief Performs bounded semantic action lookup without native name interpretation. */
        const XRActionDeclaration *Action(const XRActionBindingSchema &schema, const Input::ActionId &id) {
            const auto found = std::ranges::find(schema.actions, id, &XRActionDeclaration::action);
            return found == schema.actions.end() ? nullptr : &*found;
        }

        /** @brief Finds exact catalog type and role evidence for one persisted control identity. */
        const XRProfileControl *Control(const std::span<const XRProfileControl> catalog, const XRInteractionProfileId profile,
                                        const XRPhysicalControlId control, const XRActionDeclaration &action) {
            const auto found = std::ranges::find_if(catalog, [&](const XRProfileControl &candidate) {
                return candidate.profile == profile && candidate.control == control && candidate.role == action.role &&
                       candidate.valueType == action.valueType;
            });
            return found == catalog.end() ? nullptr : &*found;
        }

        /** @brief Applies only an explicit direct migration; zero means incompatible and is never a fallback. */
        XRPhysicalControlId MigratedControl(const XRActionBindingSchema &schema, const XRActionBindingOverride &override) {
            if (override.schemaVersion == schema.version)
                return override.control;
            const auto migration = std::ranges::find_if(schema.migrations, [&](const XRBindingMigration &candidate) {
                return candidate.fromVersion == override.schemaVersion && candidate.previous == override.control;
            });
            return migration == schema.migrations.end() ? XRPhysicalControlId{} : migration->current;
        }

        /** @brief Checks finite product action contracts and native grouping limits. */
        Result<void> ValidateActions(const XRActionBindingSchema &schema) {
            std::array<XRActionSetId, XRActionBindingLimits::MaximumSets> sets{};
            std::size_t setCount = 0;
            for (std::size_t index = 0; index < schema.actions.size(); ++index) {
                const auto &action = schema.actions[index];
                const auto registered = std::ranges::find(schema.registeredActions, action.action, &Input::ActionDescriptor::id);
                if (registered == schema.registeredActions.end() || registered->context != action.context ||
                    registered->valueType != action.valueType || registered->required != action.required ||
                    std::ranges::count(schema.registeredActions, action.action, &Input::ActionDescriptor::id) != 1)
                    return Incompatible(
                        "XR action metadata must match one exact registered Input action; Input remains the semantic authority.");
                if (!action.action.IsValid() || !action.context.IsValid() || action.set.value == 0 || !Controller(action.role) ||
                    !ValueType(action.valueType) || action.action.Value().size() > XRActionBindingLimits::MaximumNameBytes ||
                    action.context.Value().size() > XRActionBindingLimits::MaximumNameBytes)
                    return Invalid(
                        "XR actions require registered bounded action/context identities, controller roles and supported value types.");
                if (std::ranges::find(schema.actions.first(index), action.action, &XRActionDeclaration::action) !=
                    schema.actions.first(index).end())
                    return Incompatible("XR semantic action declarations must be unique; declare one explicit controller role per action.");
                if (std::find(sets.begin(), sets.begin() + setCount, action.set) == sets.begin() + setCount) {
                    if (setCount == sets.size())
                        return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded, "XR action-set capacity exceeded."));
                    sets[setCount++] = action.set;
                }
            }
            return Result<void>::Success();
        }

        /** @brief Checks exact catalog identities and duplicate profile/role/control facts. */
        Result<void> ValidateCatalog(const std::span<const XRProfileControl> catalog) {
            std::array<XRInteractionProfileId, XRActionBindingLimits::MaximumProfiles> profiles{};
            std::size_t profileCount = 0;
            for (std::size_t index = 0; index < catalog.size(); ++index) {
                const auto &control = catalog[index];
                if (control.profile.value == 0 || control.control.value == 0 || !Controller(control.role) || !ValueType(control.valueType))
                    return Invalid("XR catalogs require registered non-zero profile/control identities and supported roles/types.");
                if (std::ranges::any_of(catalog.first(index), [&](const XRProfileControl &previous) {
                    return previous.profile == control.profile && previous.control == control.control && previous.role == control.role;
                }))
                    return Incompatible("XR catalog contains duplicate profile/role/control facts.");
                if (std::find(profiles.begin(), profiles.begin() + profileCount, control.profile) == profiles.begin() + profileCount) {
                    if (profileCount == profiles.size())
                        return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded, "XR interaction-profile capacity exceeded."));
                    profiles[profileCount++] = control.profile;
                }
            }
            return Result<void>::Success();
        }

        /** @brief Validates product suggestions against exact registered action/control types. */
        Result<void> ValidateSuggestions(const XRActionBindingSchema &schema, const std::span<const XRProfileControl> catalog) {
            for (std::size_t index = 0; index < schema.suggestions.size(); ++index) {
                const auto &binding = schema.suggestions[index];
                const auto *action = Action(schema, binding.action);
                if (!action || !Control(catalog, binding.profile, binding.control, *action))
                    return Incompatible(
                        "XR suggested binding references an unregistered action or incompatible profile/role/control type.");
                if (std::ranges::any_of(schema.suggestions.first(index), [&](const XRSuggestedActionBinding &previous) {
                    return previous.action == binding.action && previous.profile == binding.profile;
                }))
                    return Incompatible("XR action has duplicate suggested bindings for one interaction profile.");
            }
            if (schema.genericFallback.value != 0 &&
                std::ranges::find(catalog, schema.genericFallback, &XRProfileControl::profile) == catalog.end())
                return Invalid("XR generic fallback must name a registered catalog profile.");
            return Result<void>::Success();
        }

        /** @brief Rejects ambiguous control renames and future/unknown persisted override schemas. */
        Result<void> ValidateOverrides(const XRActionBindingSchema &schema, const std::span<const XRProfileControl> catalog,
                                       const std::span<const XRActionBindingOverride> overrides) {
            for (std::size_t index = 0; index < schema.migrations.size(); ++index) {
                const auto &migration = schema.migrations[index];
                if (migration.fromVersion == 0 || migration.fromVersion >= schema.version || migration.previous.value == 0 ||
                    migration.current.value == 0)
                    return Invalid("XR migration must explicitly map a non-zero older schema control directly to the current schema.");
                if (std::ranges::any_of(schema.migrations.first(index), [&](const XRBindingMigration &previous) {
                    return previous.fromVersion == migration.fromVersion && previous.previous == migration.previous;
                }))
                    return Incompatible("XR binding migration has an ambiguous source control.");
            }
            for (std::size_t index = 0; index < overrides.size(); ++index) {
                const auto &override = overrides[index];
                const auto *action = Action(schema, override.action);
                const auto control = MigratedControl(schema, override);
                if (override.schemaVersion == 0 || override.schemaVersion > schema.version || !action ||
                    !Control(catalog, override.profile, control, *action))
                    return Incompatible(
                        "XR override cannot migrate with the same semantic action, role and value type; repair or remove it explicitly.");
                if (std::ranges::any_of(overrides.first(index), [&](const XRActionBindingOverride &previous) {
                    return previous.action == override.action && previous.profile == override.profile;
                }))
                    return Incompatible("XR overrides conflict for the same semantic action and interaction profile.");
            }
            return Result<void>::Success();
        }

        /** @brief Resolves one action on one exact profile, preferring a validated Input-owned override. */
        std::optional<XRResolvedActionBinding> ResolveOne(const XRActionBindingSchema &schema, const XRActionDeclaration &action,
                                                          const XRInteractionProfileId profile,
                                                          const std::span<const XRActionBindingOverride> overrides, const bool fallback) {
            const auto override = std::ranges::find_if(overrides, [&](const XRActionBindingOverride &candidate) {
                return candidate.action == action.action && candidate.profile == profile;
            });
            XRPhysicalControlId control;
            bool migrated = false;
            if (override != overrides.end()) {
                control = MigratedControl(schema, *override);
                migrated = override->schemaVersion != schema.version;
            } else {
                const auto suggestion = std::ranges::find_if(schema.suggestions, [&](const XRSuggestedActionBinding &candidate) {
                    return candidate.action == action.action && candidate.profile == profile;
                });
                if (suggestion == schema.suggestions.end())
                    return std::nullopt;
                control = suggestion->control;
            }
            return XRResolvedActionBinding{action.action, action.context, action.valueType, action.set, action.role,
                                           profile,       control,        fallback,         migrated};
        }

        /** @brief Canonicalizes per-role evidence so publication never depends on caller enumeration order. */
        Result<std::array<XRActiveInteractionProfile, 2>> CanonicalProfiles(const std::span<const XRActiveInteractionProfile> profiles) {
            std::array<XRActiveInteractionProfile, 2> canonical{XRActiveInteractionProfile{XRTrackedDeviceRole::LeftController, {}},
                                                                XRActiveInteractionProfile{XRTrackedDeviceRole::RightController, {}}};
            std::array<bool, 2> seen{};
            if (profiles.size() > canonical.size())
                return Result<decltype(canonical)>::Failure(MakeError(XRErrors::CapacityExceeded));
            for (const auto &profile : profiles) {
                if (!Controller(profile.role))
                    return Result<decltype(canonical)>::Failure(
                        MakeError(XRErrors::OperationUnsupported, "XR bindings support explicit controller roles."));
                const std::size_t index = profile.role == XRTrackedDeviceRole::LeftController ? 0 : 1;
                if (seen[index])
                    return Result<decltype(canonical)>::Failure(
                        MakeError(XRErrors::OperationIncompatible, "XR active profile roles must be unique."));
                seen[index] = true;
                canonical[index] = profile;
            }
            return Result<decltype(canonical)>::Success(canonical);
        }
    }  // namespace

    /** @copydoc ValidateXRActionBindings */
    Result<void> ValidateXRActionBindings(const XRActionBindingSchema &schema, const std::span<const XRProfileControl> catalog,
                                          const std::span<const XRActionBindingOverride> overrides) {
        if (schema.version == 0 || schema.actions.empty() || schema.registeredActions.empty())
            return Invalid("XR binding schema requires a non-zero version and registered actions.");
        if (schema.actions.size() > XRActionBindingLimits::MaximumActions || catalog.size() > XRActionBindingLimits::MaximumBindings ||
            schema.suggestions.size() > XRActionBindingLimits::MaximumBindings ||
            overrides.size() > XRActionBindingLimits::MaximumBindings ||
            schema.migrations.size() > XRActionBindingLimits::MaximumBindings ||
            schema.registeredActions.size() > XRActionBindingLimits::MaximumRegisteredActions)
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded, "XR action binding schema exceeds finite plan limits."));
        if (auto valid = ValidateActions(schema); valid.HasError())
            return valid;
        if (auto valid = ValidateCatalog(catalog); valid.HasError())
            return valid;
        if (auto valid = ValidateSuggestions(schema, catalog); valid.HasError())
            return valid;
        return ValidateOverrides(schema, catalog, overrides);
    }

    /** @copydoc ResolveXRActionBindings */
    Result<XRResolvedActionBindings> ResolveXRActionBindings(const XRActionBindingSchema &schema,
                                                             const std::span<const XRProfileControl> catalog,
                                                             const std::span<const XRActiveInteractionProfile> profiles,
                                                             const std::span<const XRActionBindingOverride> overrides) {
        if (const auto valid = ValidateXRActionBindings(schema, catalog, overrides); valid.HasError())
            return Result<XRResolvedActionBindings>::Failure(valid.ErrorValue());
        const auto canonical = CanonicalProfiles(profiles);
        if (canonical.HasError())
            return Result<XRResolvedActionBindings>::Failure(canonical.ErrorValue());
        XRResolvedActionBindings result{schema.version, {}};
        result.bindings.reserve(schema.actions.size());
        for (const auto &action : schema.actions) {
            const std::size_t role = action.role == XRTrackedDeviceRole::LeftController ? 0 : 1;
            auto binding = ResolveOne(schema, action, canonical.Value()[role].profile, overrides, false);
            if (!binding && schema.genericFallback.value != 0)
                binding = ResolveOne(schema, action, schema.genericFallback, overrides, true);
            if (!binding) {
                if (action.required)
                    return Result<XRResolvedActionBindings>::Failure(
                        MakeError(XRErrors::OperationUnsupported, "Required XR action is unbound for the active profile; declare a "
                                                                  "compatible generic fallback or repair bindings."));
                continue;
            }
            if (std::ranges::any_of(result.bindings, [&](const XRResolvedActionBinding &previous) {
                return previous.context == binding->context && previous.role == binding->role && previous.control == binding->control;
            }))
                return Result<XRResolvedActionBindings>::Failure(
                    MakeError(XRErrors::OperationIncompatible, "XR actions in the same Input context conflict on one physical control."));
            result.bindings.push_back(std::move(*binding));
        }
        std::ranges::sort(result.bindings, {}, [](const auto &binding) -> const std::string & {
            return binding.action.Value();
        });
        return Result<XRResolvedActionBindings>::Success(std::move(result));
    }

    /** @copydoc XRActionBindingCoordinator::XRActionBindingCoordinator */
    XRActionBindingCoordinator::XRActionBindingCoordinator(XRSessionId session, IXRBindingNeutralizer &neutralizer) noexcept
        : session_(session), neutralizer_(&neutralizer) {}

    /** @copydoc XRActionBindingCoordinator::~XRActionBindingCoordinator */
    XRActionBindingCoordinator::~XRActionBindingCoordinator() {
        Shutdown();
    }

    /** @copydoc XRActionBindingCoordinator::Update */
    Result<void> XRActionBindingCoordinator::Update(const XRSessionId &session, const XRActionBindingSchema &schema,
                                                    const std::span<const XRProfileControl> catalog,
                                                    const std::span<const XRActiveInteractionProfile> profiles,
                                                    const std::span<const XRActionBindingOverride> overrides) {
        if (auto valid = ValidateXRSession(session, session_); valid.HasError())
            return valid;
        try {
            auto candidate = ResolveXRActionBindings(schema, catalog, profiles, overrides);
            if (candidate.HasError()) {
                Retire();
                return Result<void>::Failure(candidate.ErrorValue());
            }
            const auto canonical = CanonicalProfiles(profiles).Value();
            if (current_ && *current_ == candidate.Value() && profiles_ == canonical)
                return Result<void>::Success();
            Retire();
            if (revision_ == std::numeric_limits<std::uint64_t>::max())
                return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded, "XR binding revision exhausted; replace the session."));
            ++revision_;
            current_ = std::move(candidate).Value();
            profiles_ = canonical;
            return Result<void>::Success();
        } catch (...) {
            Retire();
            throw;
        }
    }

    /** @copydoc XRActionBindingCoordinator::Current */
    const XRResolvedActionBindings *XRActionBindingCoordinator::Current() const noexcept {
        return current_ ? &*current_ : nullptr;
    }

    /** @copydoc XRActionBindingCoordinator::Revision */
    std::uint64_t XRActionBindingCoordinator::Revision() const noexcept {
        return revision_;
    }

    /** @brief Commits neutral Input state before dropping the previous publication. */
    void XRActionBindingCoordinator::Retire() noexcept {
        if (current_) {
            neutralizer_->Neutralize(session_, revision_);
            current_.reset();
        }
    }

    /** @copydoc XRActionBindingCoordinator::Shutdown */
    void XRActionBindingCoordinator::Shutdown() noexcept {
        Retire();
        session_ = {};
    }
}  // namespace Horo::XR
