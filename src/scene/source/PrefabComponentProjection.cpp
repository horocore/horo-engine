#include "Horo/Scene/PrefabComponentProjection.h"

#include "Horo/Prefab/PrefabErrors.h"
#include "JsonUtils.h"
#include "SceneSourceInternal.h"

#include <algorithm>
#include <array>
#include <functional>
#include <utility>

namespace Horo::SceneSource {
    namespace {
        using Json = nlohmann::json;
        using Kind = Runtime::BuiltinSceneComponentKind;

        /** @brief Names only actual canonical Scene codec fields; unavailable representations stay unsupported. */
        std::string_view Field(const Kind kind) noexcept {
            constexpr std::array<std::string_view, static_cast<std::size_t>(Kind::Count)> fields{"",
                                                                                                 "camera",
                                                                                                 "light",
                                                                                                 "audioSource",
                                                                                                 "",
                                                                                                 "",
                                                                                                 "navigationSurface",
                                                                                                 "navigationRegion",
                                                                                                 "navigationModifier",
                                                                                                 "navigationLink",
                                                                                                 "navigationAgent",
                                                                                                 "aiAgent",
                                                                                                 "aiController",
                                                                                                 "characterController",
                                                                                                 "rigidBody",
                                                                                                 "colliders",
                                                                                                 "physicsConstraints",
                                                                                                 "animation"};
            const auto index = static_cast<std::size_t>(kind);
            return index < fields.size() ? fields[index] : std::string_view{};
        }

        /** @brief Copies bounded encoded bytes into the parser's owned character input. */
        std::string PayloadBytes(const std::span<const std::byte> payload) {
            std::string bytes;
            bytes.reserve(payload.size());
            for (const auto byte : payload)
                bytes.push_back(static_cast<char>(byte));
            return bytes;
        }

        /** @brief Rejects duplicate singleton occurrences before moving any candidate field. */
        template <typename T> Result<void> Adopt(std::optional<T> &destination, std::optional<T> &&source) {
            if (!source || destination)
                return Result<void>::Failure(
                    MakeError(Prefab::PrefabErrors::ComponentTypeUnregistered, "Built-in prefab component is absent or duplicated."));
            destination = std::move(source);
            return Result<void>::Success();
        }

        /** @brief Appends one complete collection occurrence without admitting duplicate typed IDs. */
        template <typename T> Result<void> Append(std::vector<T> &destination, std::vector<T> &&source) {
            if (source.size() != 1 || std::ranges::any_of(destination, [&](const auto &existing) {
                return existing.id == source.front().id;
            }))
                return Result<void>::Failure(MakeError(Prefab::PrefabErrors::ComponentTypeUnregistered,
                                                       "Built-in prefab collection occurrence is absent or duplicated."));
            destination.push_back(std::move(source.front()));
            return Result<void>::Success();
        }

        /** @brief Type-erases only one existing typed optional-field adoption, never payload validation. */
        template <auto Destination, auto Source>
        Result<void> AdoptField(Runtime::RuntimeComponentSet &destination, SceneObjectComponentSet &source) {
            return Adopt(destination.*Destination, std::move(source.*Source));
        }

        /** @brief Moves exactly one decoded field using the finite canonical built-in schema table. */
        Result<void> Merge(const Kind kind, Runtime::RuntimeComponentSet &destination, SceneObjectComponentSet &&source) {
            using Merger = Result<void> (*)(Runtime::RuntimeComponentSet &, SceneObjectComponentSet &);
            constexpr std::array<Merger, static_cast<std::size_t>(Kind::Count)> mergers{
                nullptr,
                AdoptField<&Runtime::RuntimeComponentSet::camera, &SceneObjectComponentSet::camera>,
                AdoptField<&Runtime::RuntimeComponentSet::light, &SceneObjectComponentSet::light>,
                AdoptField<&Runtime::RuntimeComponentSet::audioSource, &SceneObjectComponentSet::audioSource>,
                nullptr,
                nullptr,
                AdoptField<&Runtime::RuntimeComponentSet::navigationSurface, &SceneObjectComponentSet::navigationSurface>,
                AdoptField<&Runtime::RuntimeComponentSet::navigationRegion, &SceneObjectComponentSet::navigationRegion>,
                AdoptField<&Runtime::RuntimeComponentSet::navigationModifier, &SceneObjectComponentSet::navigationModifier>,
                AdoptField<&Runtime::RuntimeComponentSet::navigationLink, &SceneObjectComponentSet::navigationLink>,
                AdoptField<&Runtime::RuntimeComponentSet::navigationAgent, &SceneObjectComponentSet::navigationAgent>,
                AdoptField<&Runtime::RuntimeComponentSet::aiAgent, &SceneObjectComponentSet::aiAgent>,
                AdoptField<&Runtime::RuntimeComponentSet::aiController, &SceneObjectComponentSet::aiController>,
                AdoptField<&Runtime::RuntimeComponentSet::characterController, &SceneObjectComponentSet::characterController>,
                AdoptField<&Runtime::RuntimeComponentSet::rigidBody, &SceneObjectComponentSet::rigidBody>,
                +[](Runtime::RuntimeComponentSet &target, SceneObjectComponentSet &decoded) {
                return Append(target.colliders, std::move(decoded.colliders));
            },
                +[](Runtime::RuntimeComponentSet &target, SceneObjectComponentSet &decoded) {
                return Append(target.physicsConstraints, std::move(decoded.physicsConstraints));
            },
                AdoptField<&Runtime::RuntimeComponentSet::animation, &SceneObjectComponentSet::animation>,
            };
            const auto index = static_cast<std::size_t>(kind);
            if (index >= mergers.size() || !mergers[index])
                return Result<void>::Failure(MakeError(Prefab::PrefabErrors::ComponentTypeUnregistered));
            return mergers[index](destination, source);
        }

        /** @brief Reads bounded canonical payload bytes through the shared duplicate/depth guard. */
        Result<Json> ReadPayload(const Prefab::RawComponentPayload &source) {
            if (source.instance.Value() == 0 || source.component.encoding != Gameplay::ComponentPayloadEncoding::CanonicalJson ||
                source.component.schemaVersion != PrefabBuiltinComponentPayloadSchemaVersion ||
                Gameplay::ValidateSerializedComponent(source.component).HasError())
                return Result<Json>::Failure(MakeError(Prefab::PrefabErrors::CorruptedPayload));
            Foundation::JsonParseGuard guard{64};
            auto payload = Json::parse(PayloadBytes(source.component.payload), std::ref(guard), false);
            if (payload.is_discarded() || !payload.is_object() || guard.HasDuplicate() || guard.IsTooDeep())
                return Result<Json>::Failure(MakeError(Prefab::PrefabErrors::CorruptedPayload));
            return Result<Json>::Success(std::move(payload));
        }

        /** @brief Invokes the existing typed component codec once for one exact built-in occurrence. */
        Result<SceneObjectComponentSet> Decode(const Prefab::RawComponentPayload &source, const Kind kind, const std::string_view field) {
            auto payload = ReadPayload(source);
            if (payload.HasError())
                return Result<SceneObjectComponentSet>::Failure(payload.ErrorValue());
            const bool collection = kind == Kind::Collider || kind == Kind::PhysicsConstraint;
            const Json value{{std::string(field), collection ? Json::array({payload.Value()}) : payload.Value()}};
            std::size_t recognized{};
            auto decoded = Detail::ParseComponents(value, recognized, Runtime::PhysicsValidationDetail::BodyReferenceDomain::PrefabLocal);
            if (decoded.HasError())
                return Result<SceneObjectComponentSet>::Failure(WrapError(Prefab::PrefabErrors::CorruptedPayload, decoded.ErrorValue()));
            if (recognized != 1)
                return Result<SceneObjectComponentSet>::Failure(MakeError(Prefab::PrefabErrors::ComponentTypeUnregistered));
            return decoded;
        }

        /** @brief Admits exact canonical primitive geometry without selecting or allocating a renderer. */
        Result<bool> ProjectPrimitive(const Prefab::RawComponentPayload &source,
                                      std::optional<Runtime::PrimitiveMeshDescriptor> &destination) {
            if (destination)
                return Result<bool>::Failure(MakeError(Prefab::PrefabErrors::ComponentTypeUnregistered));
            auto payload = ReadPayload(source);
            if (payload.HasError())
                return Result<bool>::Failure(payload.ErrorValue());
            auto primitive = Detail::ParsePrimitive(payload.Value());
            if (primitive.HasError())
                return Result<bool>::Failure(WrapError(Prefab::PrefabErrors::CorruptedPayload, primitive.ErrorValue()));
            destination = std::move(primitive).Value();
            return Result<bool>::Success(true);
        }
    }  // namespace

    Sha256Digest PrefabBuiltinComponentCodecFingerprint() {
        std::string identity{"horo.prefab.builtin-component-codec"};
        const auto append = [&](std::string_view value) {
            identity += std::to_string(value.size());
            identity += ':';
            identity += value;
        };
        for (const auto version :
             {PrefabBuiltinComponentCodecContractVersion, PrefabBuiltinComponentPayloadSchemaVersion, Detail::kSceneSchemaVersion,
              Detail::kCharacterSceneSchemaVersion, Detail::kNavigationSourceSceneSchemaVersion, Detail::kAnimationSceneSchemaVersion})
            append(std::to_string(version));
        const auto actual = Runtime::BuiltinSceneComponentSchemas();
        std::vector<Runtime::BuiltinSceneComponentSchema> schemas(actual.begin(), actual.end());
        std::ranges::sort(schemas, [](const auto &left, const auto &right) {
            if (left.kind != right.kind)
                return left.kind < right.kind;
            return left.typeId < right.typeId;
        });
        for (const auto &schema : schemas) {
            append(std::to_string(static_cast<unsigned>(schema.kind)));
            append(schema.typeId);
            append(Field(schema.kind));
        }
        return ComputeSha256(std::as_bytes(std::span{identity}));
    }

    /** @copydoc EncodePrefabNavigationComponent */
    Result<Prefab::RawComponentPayload> EncodePrefabNavigationComponent(const Kind kind, const Prefab::PrefabComponentInstanceId instance,
                                                                        const SceneObjectComponentSet &components) {
        using Return = Result<Prefab::RawComponentPayload>;
        if (kind < Kind::NavigationSurface || kind > Kind::NavigationAgent || instance.Value() == 0)
            return Return::Failure(MakeError(Prefab::PrefabErrors::ComponentTypeUnregistered));
        auto encoded = Detail::ComponentsJson(components);
        const auto field = std::string(Field(kind));
        const auto &fields = encoded.get_ref<Detail::WireJson::object_t &>();
        const auto selected = std::ranges::find(fields, field, &Detail::WireJson::object_t::value_type::first);
        if (selected == fields.end())
            return Return::Failure(MakeError(Prefab::PrefabErrors::CorruptedPayload));
        const auto schema = std::ranges::find(Runtime::BuiltinSceneComponentSchemas(), kind, &Runtime::BuiltinSceneComponentSchema::kind);
        const auto type = Gameplay::ComponentTypeId::Parse(schema->typeId);
        if (type.HasError())
            return Return::Failure(type.ErrorValue());
        Prefab::RawComponentPayload result{instance, {.typeId = type.Value(), .schemaVersion = PrefabBuiltinComponentPayloadSchemaVersion}};
        const auto bytes = selected->second.dump(0);
        result.component.payload.reserve(bytes.size());
        for (const auto byte : bytes)
            result.component.payload.push_back(static_cast<std::byte>(byte));
        Runtime::RuntimeComponentSet checked;
        std::optional<Runtime::PrimitiveMeshDescriptor> primitive;
        if (auto valid = ProjectPrefabBuiltinComponent(result, checked, primitive); valid.HasError())
            return Return::Failure(valid.ErrorValue());
        return Return::Success(std::move(result));
    }

    /** @copydoc ProjectPrefabBuiltinComponent */
    Result<bool> ProjectPrefabBuiltinComponent(const Prefab::RawComponentPayload &source, Runtime::RuntimeComponentSet &destination,
                                               std::optional<Runtime::PrimitiveMeshDescriptor> &primitiveMesh) {
        const auto kind = Runtime::FindBuiltinSceneComponent(source.component.typeId.Value());
        if (!kind)
            return Result<bool>::Success(false);
        try {
            if (*kind == Kind::PrimitiveMesh)
                return ProjectPrimitive(source, primitiveMesh);
            const auto field = Field(*kind);
            if (field.empty())
                return Result<bool>::Failure(MakeError(Prefab::PrefabErrors::ComponentTypeUnregistered,
                                                       "This built-in has no supported canonical cooked component representation."));
            auto decoded = Decode(source, *kind, field);
            if (decoded.HasError())
                return Result<bool>::Failure(decoded.ErrorValue());
            if (auto merged = Merge(*kind, destination, std::move(decoded).Value()); merged.HasError())
                return Result<bool>::Failure(merged.ErrorValue());
            return Result<bool>::Success(true);
        } catch (const nlohmann::json::exception &) {
            return Result<bool>::Failure(MakeError(Prefab::PrefabErrors::CorruptedPayload));
        }
    }
}  // namespace Horo::SceneSource
