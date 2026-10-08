#include "LuaReplicationDeclaration.h"

#include "Horo/Gameplay/GameplayErrors.h"
#include "LuaBehaviorMetadata.h"

#include <algorithm>
#include <initializer_list>
#include <stdexcept>
#include <string_view>

extern "C" {
#include <lua.h>
}

namespace Horo::Gameplay::Detail {
    namespace {
        using namespace Network;

        /** @brief Restores the parser stack on every candidate validation path. */
        struct StackScope final {
            lua_State *state;
            int top;

            StackScope(lua_State *value, const int depth) noexcept : state(value), top(depth) {}

            StackScope(const StackScope &) = delete;
            StackScope &operator=(const StackScope &) = delete;
            StackScope(StackScope &&) = delete;
            StackScope &operator=(StackScope &&) = delete;

            ~StackScope() {
                lua_settop(state, top);
            }
        };

        /** @brief Rejects malformed compiler metadata with the exact declaration key. */
        [[noreturn]] void Invalid(const char *key) {
            throw std::invalid_argument(std::string{"Invalid Lua replication declaration: "} + key);
        }

        /** @brief Rejects misspelled or unsupported metadata instead of silently dropping authored policies. */
        void Keys(lua_State *state, std::initializer_list<std::string_view> allowed) {
            lua_pushnil(state);
            while (lua_next(state, -2) != 0) {
                if (lua_type(state, -2) != LUA_TSTRING)
                    Invalid("metadata key");
                std::size_t size{};
                if (const char *key = lua_tolstring(state, -2, &size);
                    std::ranges::find(allowed, std::string_view{key, size}) == allowed.end())
                    Invalid("unsupported metadata key");
                lua_pop(state, 1);
            }
        }

        /** @brief Reads a bounded integer without coercion, narrowing or defaulting missing metadata. */
        std::uint32_t Integer(lua_State *state, const char *key, const std::uint32_t maximum = UINT32_MAX) {
            ReadLuaMetadataField(state, key);
            if (!lua_isinteger(state, -1))
                Invalid(key);
            const lua_Integer value = lua_tointeger(state, -1);
            if (value < 0 || static_cast<lua_Unsigned>(value) > maximum)
                Invalid(key);
            lua_pop(state, 1);
            return static_cast<std::uint32_t>(value);
        }

        /** @brief Rejects reserved zero identities before accessing a successful typed result. */
        template <typename Identity> Identity Id(lua_State *state, const char *key) {
            const auto identity = Identity::Create(Integer(state, key));
            if (identity.HasError())
                Invalid(key);
            return identity.Value();
        }

        /** @brief Reads a closed compiler keyword without numeric-to-string coercion. */
        std::string Keyword(lua_State *state, const char *key) {
            ReadLuaMetadataField(state, key);
            if (lua_type(state, -1) != LUA_TSTRING)
                Invalid(key);
            std::size_t size{};
            const char *value = lua_tolstring(state, -1, &size);
            if (size > 64)
                Invalid(key);
            std::string result{value, size};
            lua_pop(state, 1);
            return result;
        }

        /** @brief Maps source-only scalar keywords to the closed typed codec representation. */
        ReplicationValueKind Kind(lua_State *state) {
            using enum ReplicationValueKind;
            const std::string value = Keyword(state, "kind");
            if (value == "boolean")
                return Boolean;
            if (value == "integer")
                return SignedInteger;
            if (value == "unsigned")
                return UnsignedInteger;
            if (value == "number")
                return FloatingPoint;
            Invalid("kind");
        }

        /** @brief Resolves recipient selection only; source metadata cannot grant write authority. */
        ReplicationCondition Condition(lua_State *state) {
            using enum ReplicationCondition;

            const std::string value = Keyword(state, "condition");
            if (value == "always")
                return Always;
            if (value == "initial_only")
                return InitialOnly;
            if (value == "owner_only")
                return OwnerOnly;
            if (value == "skip_owner")
                return SkipOwner;
            if (value == "simulated_only")
                return SimulatedOnly;
            Invalid("condition");
        }

        /** @brief Converts an explicitly optional default to owned typed values before canonical encoding. */
        ReplicationRuntimeValue Default(lua_State *state, const ReplicationValueKind kind) {
            using enum ReplicationValueKind;

            ReadLuaMetadataField(state, "default");
            ReplicationRuntimeValue result;
            switch (kind) {
                case Boolean:
                    if (lua_type(state, -1) != LUA_TBOOLEAN)
                        Invalid("default");
                    result = lua_toboolean(state, -1) != 0;
                    break;
                case SignedInteger:
                    if (!lua_isinteger(state, -1))
                        Invalid("default");
                    result = static_cast<std::int64_t>(lua_tointeger(state, -1));
                    break;
                case UnsignedInteger:
                    if (!lua_isinteger(state, -1) || lua_tointeger(state, -1) < 0)
                        Invalid("default");
                    result = static_cast<std::uint64_t>(lua_tointeger(state, -1));
                    break;
                case FloatingPoint:
                    if (lua_type(state, -1) != LUA_TNUMBER)
                        Invalid("default");
                    result = static_cast<double>(lua_tonumber(state, -1));
                    break;
                default:
                    Invalid("default");
            }
            lua_pop(state, 1);
            return result;
        }

        /** @brief Encodes optional defaults and rejects defaults on required fields. */
        Result<void> FieldDefault(lua_State *state, ReplicationFieldDescriptor &field, const ReplicationValueKind kind,
                                  const CanonicalScalarReplicationSerializer &serializer) {
            const auto requirement = Keyword(state, "requirement");
            if (requirement == "required") {
                ReadLuaMetadataField(state, "default");
                if (!lua_isnil(state, -1))
                    Invalid("required field default");
                lua_pop(state, 1);
            }
            if (requirement == "optional") {
                field.requirement = ReplicationFieldRequirement::Optional;
                auto encoded = serializer.Encode(Default(state, kind));
                if (encoded.HasError())
                    return Result<void>::Failure(encoded.ErrorValue());
                field.canonicalDefault = ReplicationFieldDefault{std::move(encoded).Value()};
            } else if (requirement != "required")
                Invalid("requirement");
            return Result<void>::Success();
        }

        /** @brief Generates one field and shares each exact codec identity across its declaration. */
        Result<void> Field(lua_State *state, GameplayReplicationRegistration &registration) {
            Keys(state, {"id", "value_type", "codec", "kind", "introduced_major", "introduced_minor", "condition", "requirement",
                         "maximum_bytes", "maximum_elements", "default"});
            ReplicationFieldDescriptor field{
                .id = Id<FieldId>(state, "id"),
                .valueType = Id<ReplicationValueTypeId>(state, "value_type"),
                .codec = Id<ReplicationCodecId>(state, "codec"),
                .introducedVersion = {static_cast<std::uint16_t>(Integer(state, "introduced_major", UINT16_MAX)),
                                      static_cast<std::uint16_t>(Integer(state, "introduced_minor", UINT16_MAX))},
                .condition = Condition(state),
                .limits = {Integer(state, "maximum_bytes"), Integer(state, "maximum_elements")},
            };
            const auto kind = Kind(state);
            auto serializer = CanonicalScalarReplicationSerializer::Create({.valueType = field.valueType,
                                                                            .codec = field.codec,
                                                                            .owner = registration.schema.owner,
                                                                            .valueKind = kind,
                                                                            .maximumEncodedBytes = field.limits.maximumEncodedBytes,
                                                                            .maximumElementCount = field.limits.maximumElementCount});
            if (serializer.HasError())
                return Result<void>::Failure(serializer.ErrorValue());
            if (const auto defaultValue = FieldDefault(state, field, kind, *serializer.Value()); defaultValue.HasError())
                return defaultValue;
            for (const auto &existing : registration.serializers) {
                if (existing->Descriptor().valueType == field.valueType && existing->Descriptor().codec == field.codec) {
                    if (existing->Descriptor() != serializer.Value()->Descriptor())
                        Invalid("conflicting codec metadata");
                    registration.schema.fields.push_back(std::move(field));
                    return Result<void>::Success();
                }
            }
            registration.serializers.push_back(std::move(serializer).Value());
            registration.schema.fields.push_back(std::move(field));
            return Result<void>::Success();
        }

        /** @brief Accepts only simulation safe points supported by the common gameplay registry. */
        GameplaySystemPhase Phase(lua_State *state, const char *key) {
            using enum GameplaySystemPhase;
            const auto value = Keyword(state, key);
            if (value == "gameplay")
                return Gameplay;
            if (value == "pre_physics")
                return PrePhysics;
            if (value == "post_physics")
                return PostPhysics;
            Invalid(key);
        }

        /** @brief Reads a finite dense list; dictionary keys and holes cannot hide declarations. */
        std::size_t Count(lua_State *state, const char *key) {
            ReadLuaMetadataField(state, key);
            if (!lua_istable(state, -1))
                Invalid(key);
            const auto count = lua_rawlen(state, -1);
            if (count > 256)
                Invalid(key);
            std::size_t actual{};
            lua_pushnil(state);
            while (lua_next(state, -2) != 0) {
                if (!lua_isinteger(state, -2) || lua_tointeger(state, -2) < 1 ||
                    static_cast<lua_Unsigned>(lua_tointeger(state, -2)) > count)
                    Invalid(key);
                ++actual;
                lua_pop(state, 1);
            }
            if (actual != count)
                Invalid(key);
            return count;
        }

        /** @brief Reads bounded typed field and retired-identity lists before common registry validation. */
        Result<void> Lists(lua_State *state, GameplayReplicationRegistration &registration) {
            const auto fields = Count(state, "fields");
            for (std::size_t index = 1; index <= fields; ++index) {
                lua_rawgeti(state, -1, static_cast<lua_Integer>(index));
                if (!lua_istable(state, -1))
                    Invalid("fields");
                if (const auto field = Field(state, registration); field.HasError())
                    return field;
                lua_pop(state, 1);
            }
            lua_pop(state, 1);
            const auto tombstones = Count(state, "tombstones");
            for (std::size_t index = 1; index <= tombstones; ++index) {
                lua_rawgeti(state, -1, static_cast<lua_Integer>(index));
                if (!lua_isinteger(state, -1) || lua_tointeger(state, -1) < 1 ||
                    static_cast<lua_Unsigned>(lua_tointeger(state, -1)) > UINT32_MAX)
                    Invalid("tombstones");
                registration.schema.tombstonedFields.push_back(
                    FieldId::Create(static_cast<std::uint32_t>(lua_tointeger(state, -1))).Value());
                lua_pop(state, 1);
            }
            return Result<void>::Success();
        }

        /** @brief Checks an optional source schema assertion against the sidecar identity. */
        void AssertSchema(lua_State *state, const ReplicationSchemaId schema) {
            ReadLuaMetadataField(state, "schema_id");
            if (!lua_isnil(state, -1) && (!lua_isinteger(state, -1) || lua_tointeger(state, -1) <= 0 ||
                                          static_cast<lua_Unsigned>(lua_tointeger(state, -1)) != schema.Value()))
                Invalid("schema_id differs from sidecar");
            lua_pop(state, 1);
        }
    }  // namespace

    /** @copydoc ReadLuaReplicationDeclaration */
    Result<std::optional<GameplayReplicationRegistration>> ReadLuaReplicationDeclaration(lua_State *state, const BehaviorTypeId &owner,
                                                                                         const Network::ReplicationSchemaId schema,
                                                                                         const ModuleId &moduleId) {
        const StackScope stack{state, lua_gettop(state)};
        try {
            ReadLuaMetadataField(state, "replication");
            if (lua_isnil(state, -1)) {
                if (schema.IsValid() || !moduleId.value.empty())
                    Invalid("sidecar schema has no source declaration");
                return Result<std::optional<GameplayReplicationRegistration>>::Success(std::nullopt);
            }
            if (!lua_istable(state, -1) || !schema.IsValid())
                Invalid("missing sidecar replicationSchemaId");
            Keys(state,
                 {"schema_id", "major", "minor", "minimum_minor", "maximum_minor", "capture_phase", "apply_phase", "fields", "tombstones"});
            GameplayReplicationRegistration registration;
            registration.owner = owner;
            registration.schema.id = schema;
            AssertSchema(state, schema);
            registration.schema.owner = moduleId;
            const auto major = static_cast<std::uint16_t>(Integer(state, "major", UINT16_MAX));
            registration.schema.version = {major, static_cast<std::uint16_t>(Integer(state, "minor", UINT16_MAX))};
            registration.schema.compatibility = {{major, static_cast<std::uint16_t>(Integer(state, "minimum_minor", UINT16_MAX))},
                                                 {major, static_cast<std::uint16_t>(Integer(state, "maximum_minor", UINT16_MAX))}};
            registration.schedule.capturePhase = Phase(state, "capture_phase");
            registration.schedule.applyPhase = Phase(state, "apply_phase");
            if (const auto lists = Lists(state, registration); lists.HasError())
                return Result<std::optional<GameplayReplicationRegistration>>::Failure(lists.ErrorValue());
            return Result<std::optional<GameplayReplicationRegistration>>::Success(std::move(registration));
        } catch (const std::invalid_argument &error) {
            return Result<std::optional<GameplayReplicationRegistration>>::Failure(
                MakeError(GameplayErrors::InvalidReplicationRegistration, error.what()));
        } catch (const std::bad_alloc &) {
            return Result<std::optional<GameplayReplicationRegistration>>::Failure(
                MakeError(GameplayErrors::InvalidReplicationRegistration, "Unable to allocate Lua replication metadata."));
        }
    }

    /** @copydoc BuildLuaReplicationRegistry */
    Result<std::unique_ptr<ReplicationRegistrationRegistry>> BuildLuaReplicationRegistry(const GameplayReplicationRegistration &declaration,
                                                                                         const BehaviorDescriptor &descriptor) {
        auto registry = std::make_unique<ReplicationRegistrationRegistry>(declaration.schema.owner.value);
        auto registered = registry->Register(declaration);
        const BehaviorRegistration behavior{.descriptor = descriptor};
        if (registered.HasValue())
            registered = registry->Freeze({}, {&behavior, 1}, {});
        if (registered.HasError())
            return Result<std::unique_ptr<ReplicationRegistrationRegistry>>::Failure(registered.ErrorValue());
        return Result<std::unique_ptr<ReplicationRegistrationRegistry>>::Success(std::move(registry));
    }

    /** @copydoc ValidateLuaReplicationCodecs */
    Result<void> ValidateLuaReplicationCodecs(const GameplayReplicationRegistration &active,
                                              const GameplayReplicationRegistration &replacement, const std::string &sourceName) {
        for (const auto &serializer : active.serializers) {
            const auto found = std::ranges::find_if(replacement.serializers, [&serializer](const auto &value) {
                return value->Descriptor().valueType == serializer->Descriptor().valueType &&
                       value->Descriptor().codec == serializer->Descriptor().codec;
            });
            if (found != replacement.serializers.end() && (*found)->Descriptor() != serializer->Descriptor())
                return Result<void>::Failure(MakeError(GameplayErrors::InvalidBehaviorComponent,
                                                       sourceName + ": Lua replication reload changed an existing codec."));
        }
        return Result<void>::Success();
    }

}  // namespace Horo::Gameplay::Detail
