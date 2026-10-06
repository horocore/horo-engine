#include "Horo/Application/PrefabSceneCookHost.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <type_traits>

namespace Horo::Application {
    namespace {
        using Json = nlohmann::json;

        /** @brief Canonicalizes every supported behavior default without layout, addresses or implementation callbacks. */
        Json Default(const Gameplay::BehaviorFieldValue &value) {
            return std::visit([](const auto &field) -> Json {
                using T = std::decay_t<decltype(field)>;
                if constexpr (std::is_same_v<T, std::monostate>)
                    return nullptr;
                else if constexpr (std::is_same_v<T, Math::Vec2>)
                    return Json::array({field.x, field.y});
                else if constexpr (std::is_same_v<T, Math::Vec3>)
                    return Json::array({field.x, field.y, field.z});
                else if constexpr (std::is_same_v<T, Math::Quaternion>)
                    return Json::array({field.x, field.y, field.z, field.w});
                else
                    return Json(field);
            }, value);
        }

        /** @brief Commits the complete frozen component contract, including property kinds/requirements and migration edges. */
        Json ComponentIdentity(const Gameplay::ComponentDescriptor &descriptor) {
            Json properties = Json::array();
            for (const auto &property : descriptor.properties)
                properties.push_back({property.id.Value(), static_cast<unsigned>(property.kind), property.required});
            Json migrations = Json::array();
            for (const auto &migration : descriptor.migrations)
                migrations.push_back({migration.fromSchemaVersion, migration.toSchemaVersion});
            return Json::array({descriptor.typeId.Value(), descriptor.schemaVersion, properties, migrations});
        }

        /** @brief Bounds inert behavior metadata and commits defaults, phase/access/dependency settings as well as schema. */
        Result<Json> BehaviorIdentity(const Gameplay::BehaviorDescriptor &descriptor) {
            if (!descriptor.typeId.IsValid() || descriptor.schemaVersion == 0 || descriptor.fields.size() > 128 ||
                descriptor.phases.size() > 64)
                return Result<Json>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
            Json fields = Json::object();
            for (const auto &field : descriptor.fields) {
                if (field.name.empty() || field.name.size() > 96 || fields.contains(field.name))
                    return Result<Json>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
                if (const auto *text = std::get_if<std::string>(&field.defaultValue); text && text->size() > 4096)
                    return Result<Json>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
                const auto value = Default(field.defaultValue).dump();
                if (value.size() > 1024U * 1024U)
                    return Result<Json>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
                fields[field.name] = {field.defaultValue.index(), value};
            }
            Json phases = Json::array();
            for (const auto &phase : descriptor.phases) {
                if (phase.nodeId.size() > 160 || phase.access.reads.size() > 256 || phase.access.writes.size() > 256 ||
                    phase.after.size() > 256 || phase.before.size() > 256)
                    return Result<Json>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
                const auto bounded = [](const std::vector<std::string> &names) {
                    return std::ranges::all_of(names, [](const std::string &name) {
                        return name.size() <= 160;
                    });
                };
                if (!bounded(phase.access.reads) || !bounded(phase.access.writes) || !bounded(phase.after) || !bounded(phase.before))
                    return Result<Json>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
                phases.push_back(
                    {static_cast<unsigned>(phase.phase), phase.nodeId, phase.access.reads, phase.access.writes, phase.after, phase.before});
            }
            return Result<Json>::Success(
                Json::array({descriptor.typeId.Value(), descriptor.schemaVersion, descriptor.allowMultiple, fields, phases}));
        }
    }  // namespace

    /** @copydoc PrefabCookSchemaContext::Capture */
    Result<std::shared_ptr<const PrefabCookSchemaContext>> PrefabCookSchemaContext::Capture(
        const Gameplay::ComponentRegistry &components, const std::span<const Gameplay::BehaviorDescriptor> behaviors) {
        if (!components.IsFrozen() || components.Descriptors().size() > 1024 || behaviors.size() > 1024)
            return Result<std::shared_ptr<const PrefabCookSchemaContext>>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
        Json identity{{"contract", "horo.prefab.cook.schemas.v1"}, {"components", Json::array()}, {"behaviors", Json::object()}};
        std::size_t totalBytes{};
        constexpr std::size_t maximumBytes = 16U * 1024U * 1024U;
        for (const auto &component : components.Descriptors()) {
            auto metadata = ComponentIdentity(component);
            const auto bytes = metadata.dump().size();
            if (bytes > maximumBytes - totalBytes)
                return Result<std::shared_ptr<const PrefabCookSchemaContext>>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
            totalBytes += bytes;
            identity["components"].push_back(std::move(metadata));
        }
        for (const auto &behavior : behaviors) {
            auto metadata = BehaviorIdentity(behavior);
            if (metadata.HasError() || identity["behaviors"].contains(behavior.typeId.Value()))
                return Result<std::shared_ptr<const PrefabCookSchemaContext>>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
            const auto bytes = metadata.Value().dump().size();
            if (bytes > maximumBytes - totalBytes)
                return Result<std::shared_ptr<const PrefabCookSchemaContext>>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
            totalBytes += bytes;
            identity["behaviors"][behavior.typeId.Value()] = std::move(metadata).Value();
        }
        const auto bytes = identity.dump();
        if (bytes.size() > 16U * 1024U * 1024U)
            return Result<std::shared_ptr<const PrefabCookSchemaContext>>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
        const auto digest = ComputeSha256(std::as_bytes(std::span{bytes}));
        return Result<std::shared_ptr<const PrefabCookSchemaContext>>::Success(std::shared_ptr<const PrefabCookSchemaContext>{
            new PrefabCookSchemaContext{components, {behaviors.begin(), behaviors.end()}, digest}});
    }

    /** @copydoc PrefabCookSchemaContext::PrefabCookSchemaContext */
    PrefabCookSchemaContext::PrefabCookSchemaContext(Gameplay::ComponentRegistry components,
                                                     std::vector<Gameplay::BehaviorDescriptor> behaviors, const Sha256Digest digest)
        : components_(std::move(components)), behaviors_(std::move(behaviors)), digest_(digest) {}

    /** @copydoc PrefabCookSchemaContext::Digest */
    const Sha256Digest &PrefabCookSchemaContext::Digest() const noexcept {
        return digest_;
    }

    /** @copydoc PrefabCookSchemaContext::Validate */
    Result<void> PrefabCookSchemaContext::Validate(const std::span<const Gameplay::SerializedComponent> components,
                                                   const std::span<const Gameplay::BehaviorComponent> behaviors) const {
        if (auto valid = Gameplay::ValidateSerializedComponents(components); valid.HasError())
            return valid;
        for (const auto &component : components) {
            const auto inspected = components_.Inspect(component);
            if (inspected.HasError() || inspected.Value().status != Gameplay::ComponentInspectionStatus::Current)
                return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
        }
        for (const auto &behavior : behaviors) {
            const auto descriptor = std::ranges::find(behaviors_, behavior.typeId, &Gameplay::BehaviorDescriptor::typeId);
            if (Gameplay::ValidateBehaviorComponent(behavior).HasError() || descriptor == behaviors_.end() ||
                behavior.schemaVersion != descriptor->schemaVersion || behavior.fields.size() != descriptor->fields.size() ||
                (!descriptor->allowMultiple && std::ranges::count(behaviors, behavior.typeId, &Gameplay::BehaviorComponent::typeId) > 1))
                return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
            for (const auto &field : behavior.fields) {
                const auto expected = std::ranges::find(descriptor->fields, field.name, &Gameplay::BehaviorFieldDescriptor::name);
                if (expected == descriptor->fields.end() || expected->defaultValue.index() != field.value.index())
                    return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
            }
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Application
