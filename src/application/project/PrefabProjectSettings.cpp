#include "Horo/Application/PrefabProjectSettings.h"

#include "Horo/Prefab/PrefabErrors.h"

#include <bit>
#include <new>

namespace Horo::Application {
    namespace {
        using Policy = Prefab::PrefabProjectPolicy;
        constexpr Policy Defaults;
        constexpr std::array Members{
            &Policy::maximumHierarchyDepth,
            &Policy::maximumObjectCount,
            &Policy::maximumSourcePayloadBytes,
            &Policy::maximumExpandedPayloadBytes,
            &Policy::maximumCookedPayloadBytes,
            &Policy::maximumComponentsPerObject,
            &Policy::maximumReferencedAssets,
            &Policy::maximumDirectNestedPlacements,
            &Policy::maximumVariantInheritanceDepth,
            &Policy::maximumNestedPrefabDepth,
            &Policy::maximumOverrideRecords,
            &Policy::maximumConflictAndOrphanRecords,
            &Policy::maximumPropertyPathSegments,
            &Policy::maximumOverrideValueBytes,
            &Policy::maximumOverrideSetBytes,
            &Policy::maximumBindingSlots,
            &Policy::maximumBindingUses,
            &Policy::maximumInstanceBindings,
            &Policy::maximumRuntimeSpawnDepth,
        };
        constexpr std::array Fields{
            PrefabLimitFieldDescriptor{PrefabLimitField::HierarchyDepth, "prefab.limits.hierarchy_depth",
                                       "settings.prefab.fields.hierarchy_depth", Prefab::PrefabHardLimits::SourceHierarchyDepth,
                                       Defaults.maximumHierarchyDepth},
            PrefabLimitFieldDescriptor{PrefabLimitField::ObjectCount, "prefab.limits.object_count", "settings.prefab.fields.object_count",
                                       Prefab::PrefabHardLimits::SourceObjectCount, Defaults.maximumObjectCount},
            PrefabLimitFieldDescriptor{PrefabLimitField::SourcePayloadBytes, "prefab.limits.source_payload_bytes",
                                       "settings.prefab.fields.source_payload_bytes", Prefab::PrefabHardLimits::SourcePayloadBytes,
                                       Defaults.maximumSourcePayloadBytes},
            PrefabLimitFieldDescriptor{PrefabLimitField::ExpandedPayloadBytes, "prefab.limits.expanded_payload_bytes",
                                       "settings.prefab.fields.expanded_payload_bytes", Prefab::PrefabHardLimits::ExpandedPayloadBytes,
                                       Defaults.maximumExpandedPayloadBytes},
            PrefabLimitFieldDescriptor{PrefabLimitField::CookedPayloadBytes, "prefab.limits.cooked_payload_bytes",
                                       "settings.prefab.fields.cooked_payload_bytes", Prefab::PrefabHardLimits::CookedPayloadBytes,
                                       Defaults.maximumCookedPayloadBytes},
            PrefabLimitFieldDescriptor{PrefabLimitField::ComponentsPerObject, "prefab.limits.components_per_object",
                                       "settings.prefab.fields.components_per_object", Prefab::PrefabHardLimits::ComponentsPerObject,
                                       Defaults.maximumComponentsPerObject},
            PrefabLimitFieldDescriptor{PrefabLimitField::ReferencedAssets, "prefab.limits.referenced_assets",
                                       "settings.prefab.fields.referenced_assets", Prefab::PrefabHardLimits::ReferencedAssets,
                                       Defaults.maximumReferencedAssets},
            PrefabLimitFieldDescriptor{PrefabLimitField::DirectNestedPlacements, "prefab.limits.direct_nested_placements",
                                       "settings.prefab.fields.direct_nested_placements", Prefab::PrefabHardLimits::DirectNestedPlacements,
                                       Defaults.maximumDirectNestedPlacements},
            PrefabLimitFieldDescriptor{PrefabLimitField::VariantInheritanceDepth, "prefab.limits.variant_depth",
                                       "settings.prefab.fields.variant_depth", Prefab::PrefabHardLimits::VariantInheritanceDepth,
                                       Defaults.maximumVariantInheritanceDepth},
            PrefabLimitFieldDescriptor{PrefabLimitField::NestedPrefabDepth, "prefab.limits.nested_depth",
                                       "settings.prefab.fields.nested_depth", Prefab::PrefabHardLimits::NestedPrefabDepth,
                                       Defaults.maximumNestedPrefabDepth},
            PrefabLimitFieldDescriptor{PrefabLimitField::OverrideRecords, "prefab.limits.override_records",
                                       "settings.prefab.fields.override_records", Prefab::PrefabHardLimits::OverrideRecords,
                                       Defaults.maximumOverrideRecords},
            PrefabLimitFieldDescriptor{PrefabLimitField::ConflictAndOrphanRecords, "prefab.limits.unresolved_records",
                                       "settings.prefab.fields.unresolved_records", Prefab::PrefabHardLimits::ConflictAndOrphanRecords,
                                       Defaults.maximumConflictAndOrphanRecords},
            PrefabLimitFieldDescriptor{PrefabLimitField::PropertyPathSegments, "prefab.limits.property_path_segments",
                                       "settings.prefab.fields.property_path_segments", Prefab::PrefabHardLimits::PropertyPathSegments,
                                       Defaults.maximumPropertyPathSegments},
            PrefabLimitFieldDescriptor{PrefabLimitField::OverrideValueBytes, "prefab.limits.override_value_bytes",
                                       "settings.prefab.fields.override_value_bytes", Prefab::PrefabHardLimits::OverrideValueBytes,
                                       Defaults.maximumOverrideValueBytes},
            PrefabLimitFieldDescriptor{PrefabLimitField::OverrideSetBytes, "prefab.limits.override_set_bytes",
                                       "settings.prefab.fields.override_set_bytes", Prefab::PrefabHardLimits::OverrideSetBytes,
                                       Defaults.maximumOverrideSetBytes},
            PrefabLimitFieldDescriptor{PrefabLimitField::BindingSlots, "prefab.limits.binding_slots",
                                       "settings.prefab.fields.binding_slots", Prefab::PrefabHardLimits::BindingSlots,
                                       Defaults.maximumBindingSlots},
            PrefabLimitFieldDescriptor{PrefabLimitField::BindingUses, "prefab.limits.binding_uses", "settings.prefab.fields.binding_uses",
                                       Prefab::PrefabHardLimits::BindingUses, Defaults.maximumBindingUses},
            PrefabLimitFieldDescriptor{PrefabLimitField::InstanceBindings, "prefab.limits.instance_bindings",
                                       "settings.prefab.fields.instance_bindings", Prefab::PrefabHardLimits::InstanceBindings,
                                       Defaults.maximumInstanceBindings},
            PrefabLimitFieldDescriptor{PrefabLimitField::RuntimeSpawnDepth, "prefab.limits.runtime_spawn_depth",
                                       "settings.prefab.fields.runtime_spawn_depth", Prefab::PrefabHardLimits::RuntimeSpawnDepth,
                                       Defaults.maximumRuntimeSpawnDepth},
        };

        /** @brief Canonical bounded module-project document path. */
        std::filesystem::path Path(const std::filesystem::path &root) {
            return root / ".horo/prefab.json";
        }

        /** @brief Return the existing typed prefab policy diagnostic without creating another error namespace. */
        Error Invalid() {
            return MakeError(Prefab::PrefabErrors::LimitProfileInvalid);
        }

        /** @brief Contain foreign host lock callbacks before any irreversible file publication. */
        Result<ExclusiveFileLock> AcquireWriter(DurableFileSystem &files, const std::filesystem::path &path) {
            try {
                return files.TryAcquireExclusive(path, "horo.prefab.settings");
            } catch (...) {
                return Result<ExclusiveFileLock>::Failure(
                    MakeError(Assets::AssetSourceErrors::Authority, "The prefab settings writer authority failed before publication."));
            }
        }
    }  // namespace

    /** @copydoc PrefabLimitFields */
    std::span<const PrefabLimitFieldDescriptor> PrefabLimitFields() noexcept {
        return Fields;
    }

    /** @copydoc PrefabPolicyFieldValues */
    PrefabPolicyValues PrefabPolicyFieldValues(const Policy &policy) noexcept {
        PrefabPolicyValues values;
        for (std::size_t i = 0; i < Members.size(); ++i)
            values[i] = static_cast<std::int64_t>(policy.*Members[i]);
        return values;
    }

    /** @copydoc ValidatePrefabPolicyValues */
    PrefabPolicyValidation ValidatePrefabPolicyValues(const PrefabPolicyValues &values) {
        PrefabPolicyValidation result;
        Policy policy;
        bool invalid{};
        for (std::size_t i = 0; i < Members.size(); ++i) {
            if (values[i] <= 0 || static_cast<std::uint64_t>(values[i]) > Fields[i].hardCeiling) {
                result.fields[i] = PrefabPolicyFinding::OutsideSafetyCeiling;
                invalid = true;
            } else
                policy.*Members[i] = static_cast<std::size_t>(values[i]);
        }
        constexpr auto value = static_cast<std::size_t>(PrefabLimitField::OverrideValueBytes);
        constexpr auto set = static_cast<std::size_t>(PrefabLimitField::OverrideSetBytes);
        if (values[value] > values[set]) {
            result.fields[value] = PrefabPolicyFinding::ValueExceedsSet;
            result.fields[set] = PrefabPolicyFinding::ValueExceedsSet;
            invalid = true;
        }
        if (!invalid) {
            auto profile = Prefab::PrefabLimitProfile::Create(policy);
            if (profile.HasValue())
                result.profile = std::move(profile).Value();
        }
        return result;
    }

    /** @copydoc CreatePrefabProjectSettingsSchema */
    Result<ConfigurationSchema> CreatePrefabProjectSettingsSchema() {
        ConfigurationSchema schema;
        for (const auto &field : Fields) {
            auto added = schema.Register({.key = SettingKey{std::string(field.key)},
                                          .type = SettingValueType::Integer,
                                          .defaultValue = static_cast<std::int64_t>(field.defaultValue),
                                          .scope = SettingScope::Project,
                                          .reloadPolicy = ReloadPolicy::NextOperation,
                                          .sensitivity = SettingSensitivity::Public,
                                          .sourcePolicy = ConfigurationSourcePolicy{ConfigurationSourceMask::Project}});
            if (added.HasError())
                return Result<ConfigurationSchema>::Failure(std::move(added).ErrorValue());
        }
        if (auto sealed = schema.Seal(); sealed.HasError())
            return Result<ConfigurationSchema>::Failure(std::move(sealed).ErrorValue());
        return Result<ConfigurationSchema>::Success(std::move(schema));
    }

    /** @copydoc PrefabProjectSettings::Resolve */
    Result<PrefabProjectSettings> PrefabProjectSettings::Resolve(const ConfigurationSourceMap &project,
                                                                 const ConfigurationRevision revision) {
        using Return = Result<PrefabProjectSettings>;
        auto schema = CreatePrefabProjectSettingsSchema();
        if (schema.HasError())
            return Return::Failure(std::move(schema).ErrorValue());
        auto resolved = ConfigurationResolver::Resolve(schema.Value(), {.project = project}, revision);
        if (resolved.HasError())
            return Return::Failure(std::move(resolved).ErrorValue());
        PrefabPolicyValues values;
        for (std::size_t i = 0; i < Fields.size(); ++i)
            values[i] = std::get<std::int64_t>(resolved.Value().Get(SettingKey{std::string(Fields[i].key)}));
        auto validated = ValidatePrefabPolicyValues(values);
        if (!validated.profile)
            return Return::Failure(Invalid());
        return Return::Success({std::move(resolved).Value(), std::move(*validated.profile)});
    }

    /** @copydoc PrefabProjectSettings::Create */
    Result<PrefabProjectSettings> PrefabProjectSettings::Create(const PrefabPolicyValues &values, const ConfigurationRevision revision) {
        ConfigurationSourceMap project;
        for (std::size_t i = 0; i < Fields.size(); ++i)
            project.emplace(SettingKey{std::string(Fields[i].key)}, ConfigurationInputValue{values[i], {}});
        return Resolve(project, revision);
    }

    /** @copydoc LoadPrefabProjectSettings */
    Result<PrefabProjectSettingsFile> LoadPrefabProjectSettings(const std::filesystem::path &root, const ConfigurationRevision revision) {
        using Return = Result<PrefabProjectSettingsFile>;
        try {
            if (!root.is_absolute() || root != root.lexically_normal())
                return Return::Failure(Invalid());
            auto captured = Assets::CaptureAssetSource(root, Path(root), 64U * 1024U);
            if (captured.HasError())
                return Return::Failure(std::move(captured).ErrorValue());
            ConfigurationSourceMap project;
            if (captured.Value().identity.exists) {
                auto schema = CreatePrefabProjectSettingsSchema();
                if (schema.HasError())
                    return Return::Failure(std::move(schema).ErrorValue());
                const auto &bytes = captured.Value().bytes;
                auto parsed =
                    ConfigurationResolver::ParseDocument(schema.Value(), {std::bit_cast<const char *>(bytes.data()), bytes.size()},
                                                         ".horo/prefab.json");
                if (parsed.HasError())
                    return Return::Failure(std::move(parsed).ErrorValue());
                project = std::move(parsed).Value();
            }
            auto settings = PrefabProjectSettings::Resolve(project, revision);
            if (settings.HasError())
                return Return::Failure(std::move(settings).ErrorValue());
            return Return::Success({std::make_shared<const PrefabProjectSettings>(std::move(settings).Value()), captured.Value().identity});
        } catch (const std::bad_alloc &) {
            return Return::Failure(Invalid());
        }
    }

    /** @copydoc SavePrefabProjectSettings */
    Result<PrefabProjectSettingsPublication> SavePrefabProjectSettings(const std::filesystem::path &root,
                                                                       const PrefabProjectSettingsFile &expected,
                                                                       const PrefabProjectSettings &candidate, DurableFileSystem &files) {
        using Return = Result<PrefabProjectSettingsPublication>;
        try {
            if (!root.is_absolute() || root != root.lexically_normal() || !expected.settings)
                return Return::Failure(Invalid());
            auto path = Assets::AssetSourcePublicationLockPath(root, Path(root));
            if (path.HasError())
                return Return::Failure(std::move(path).ErrorValue());
            auto lock = AcquireWriter(files, path.Value());
            if (lock.HasError())
                return Return::Failure(std::move(lock).ErrorValue());
            auto settings = std::make_shared<const PrefabProjectSettings>(candidate);
            const auto text = candidate.Configuration().ToJson();
            auto published = Assets::PublishAssetSource(root, Path(root), expected.identity,
                                                        std::as_bytes(std::span{text.data(), text.size()}), lock.Value(), files);
            if (published.HasError())
                return Return::Failure(std::move(published).ErrorValue());
            auto receipt = std::move(published).Value();
            return Return::Success({{std::move(settings), receipt.identity}, std::move(receipt.durabilityError)});
        } catch (const std::bad_alloc &) {
            return Return::Failure(Invalid());
        }
    }
}  // namespace Horo::Application
