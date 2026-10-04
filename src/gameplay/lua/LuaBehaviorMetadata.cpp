#include "LuaBehaviorMetadata.h"

#include "Horo/Gameplay/GameplayErrors.h"
#include "Horo/Gameplay/LuaBehavior.h"

#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <stdexcept>
extern "C" {
#include <lua.h>
}

namespace Horo::Gameplay::Detail {
    namespace {
        /** @brief Borrowed compiler key valid only for one protected metadata lookup. */
        struct TableKey final {
            const char *value;
            bool coerceString;
        };

        /** @brief Contains Lua key-interning allocation failures without crossing C++ object lifetimes. */
        int RawField(lua_State *state) {
            const auto *key = static_cast<const TableKey *>(lua_touserdata(state, 2));
            lua_pushstring(state, key->value);
            lua_rawget(state, 1);
            if (key->coerceString)
                lua_tolstring(state, -1, nullptr);
            return 1;
        }

        /** @brief Converts scalar behavior defaults into owned gameplay values. */
        [[nodiscard]] BehaviorFieldValue ReadDefault(lua_State *state, const int index) {
            switch (lua_type(state, index)) {
                case LUA_TBOOLEAN:
                    return lua_toboolean(state, index) != 0;
                case LUA_TNUMBER:
                    if (lua_isinteger(state, index))
                        return static_cast<std::int64_t>(lua_tointeger(state, index));
                    return static_cast<double>(lua_tonumber(state, index));
                case LUA_TSTRING:
                    return std::string{lua_tostring(state, index)};
                default:
                    return std::monostate{};
            }
        }

        /** @brief Reads bounded inert behavior fields without author metamethods. */
        Result<void> ReadBehaviorFields(lua_State *state, BehaviorDescriptor &descriptor) {
            ReadLuaMetadataField(state, "fields");
            if (lua_istable(state, -1)) {
                const auto count = static_cast<lua_Integer>(lua_rawlen(state, -1));
                if (count < 0 || count > static_cast<lua_Integer>(MaximumBehaviorFields))
                    return Result<void>::Failure(MakeError(GameplayErrors::InvalidBehaviorComponent));
                for (lua_Integer index = 1; index <= count; ++index) {
                    lua_rawgeti(state, -1, index);
                    if (!lua_istable(state, -1))
                        return Result<void>::Failure(MakeError(GameplayErrors::InvalidBehaviorComponent));
                    ReadLuaMetadataField(state, "name", true);
                    if (lua_type(state, -1) != LUA_TSTRING)
                        return Result<void>::Failure(MakeError(GameplayErrors::InvalidBehaviorComponent));
                    std::string name = lua_tostring(state, -1);
                    lua_pop(state, 1);
                    ReadLuaMetadataField(state, "default");
                    BehaviorFieldValue defaultValue = ReadDefault(state, -1);
                    lua_pop(state, 1);
                    descriptor.fields.emplace_back(std::move(name), std::move(defaultValue));
                    lua_pop(state, 1);
                }
            }
            lua_pop(state, 1);
            return Result<void>::Success();
        }

        /** @brief Reads the inert behavior identity and delegates its bounded field list. */
        [[nodiscard]] Result<BehaviorDescriptor> ReadDescriptor(lua_State *state, const BehaviorTypeId &canonicalTypeId) {
            if (!lua_istable(state, -1))
                return Result<BehaviorDescriptor>::Failure(
                    MakeError(GameplayErrors::InvalidBehaviorComponent, "Lua behavior source must return a descriptor table."));
            BehaviorDescriptor descriptor;
            descriptor.typeId = canonicalTypeId;
            ReadLuaMetadataField(state, "type_id", true);
            if (!lua_isnil(state, -1) && (lua_type(state, -1) != LUA_TSTRING || canonicalTypeId.Value() != lua_tostring(state, -1)))
                return Result<BehaviorDescriptor>::Failure(
                    MakeError(GameplayErrors::InvalidBehaviorComponent, "Lua source type_id does not match its sidecar identity."));
            lua_pop(state, 1);
            ReadLuaMetadataField(state, "display_name", true);
            if (lua_type(state, -1) != LUA_TSTRING)
                return Result<BehaviorDescriptor>::Failure(
                    MakeError(GameplayErrors::InvalidBehaviorComponent, "Lua behavior requires display_name."));
            descriptor.displayName = lua_tostring(state, -1);
            lua_pop(state, 1);
            ReadLuaMetadataField(state, "category", true);
            if (lua_type(state, -1) == LUA_TSTRING)
                descriptor.category = lua_tostring(state, -1);
            lua_pop(state, 1);
            ReadLuaMetadataField(state, "schema_version");
            if (lua_isinteger(state, -1))
                descriptor.schemaVersion = static_cast<std::uint32_t>(lua_tointeger(state, -1));
            lua_pop(state, 1);
            ReadLuaMetadataField(state, "allow_multiple");
            descriptor.allowMultiple = lua_toboolean(state, -1) != 0;
            lua_pop(state, 1);
            if (const auto fields = ReadBehaviorFields(state, descriptor); fields.HasError())
                return Result<BehaviorDescriptor>::Failure(fields.ErrorValue());
            descriptor.phases.push_back({BehaviorPhase::Gameplay, canonicalTypeId.Value(), {}, {}, {}});
            return Result<BehaviorDescriptor>::Success(std::move(descriptor));
        }

    }  // namespace

    /** @copydoc ReadLuaMetadataField */
    void ReadLuaMetadataField(lua_State *state, const char *key, const bool coerceString) {
        TableKey argument{key, coerceString};
        lua_pushcfunction(state, RawField);
        lua_pushvalue(state, -2);
        lua_pushlightuserdata(state, &argument);
        if (lua_pcall(state, 2, 1, 0) != LUA_OK)
            throw std::invalid_argument("Lua metadata exceeds the compilation memory budget.");
    }

    /** @copydoc ReadLuaBehaviorDescriptor */
    Result<BehaviorDescriptor> ReadLuaBehaviorDescriptor(lua_State *state, const BehaviorTypeId &canonicalTypeId) {
        try {
            return ReadDescriptor(state, canonicalTypeId);
        } catch (const std::invalid_argument &error) {
            return Result<BehaviorDescriptor>::Failure(MakeError(GameplayErrors::InvalidBehaviorComponent, error.what()));
        } catch (const std::bad_alloc &) {
            return Result<BehaviorDescriptor>::Failure(
                MakeError(GameplayErrors::InvalidBehaviorComponent, "Unable to allocate Lua behavior metadata."));
        }
    }
}  // namespace Horo::Gameplay::Detail

namespace Horo::Gameplay {
    namespace {
        /** @brief Resolves sidecar replication identities without inventing a module namespace. */
        struct SidecarReplicationIdentity {
            Network::ReplicationSchemaId schema;
            ModuleId moduleId;
        };

        /** @brief Rejects malformed typed identities before compiling a candidate. */
        Result<SidecarReplicationIdentity> ReadReplicationIdentity(const nlohmann::json &sidecar,
                                                                   const std::filesystem::path &sidecarPath) {
            Network::ReplicationSchemaId schema;
            ModuleId moduleId;
            if (sidecar.contains("replicationSchemaId")) {
                if (!sidecar["replicationSchemaId"].is_number_unsigned())
                    return Result<SidecarReplicationIdentity>::Failure(
                        MakeError(GameplayErrors::InvalidReplicationRegistration,
                                  sidecarPath.string() + ": replicationSchemaId must be a nonzero unsigned integer."));
                auto identity = Network::ReplicationSchemaId::Create(sidecar["replicationSchemaId"].get<std::uint64_t>());
                if (identity.HasError()) {
                    auto error = identity.ErrorValue();
                    error.message = sidecarPath.string() + ": " + error.message;
                    return Result<SidecarReplicationIdentity>::Failure(std::move(error));
                }
                schema = identity.Value();
                if (!sidecar.contains("replicationModuleId") || !sidecar["replicationModuleId"].is_string())
                    return Result<SidecarReplicationIdentity>::Failure(
                        MakeError(GameplayErrors::InvalidReplicationRegistration,
                                  sidecarPath.string() + ": replicationModuleId is required with replicationSchemaId."));
                moduleId.value = sidecar["replicationModuleId"].get<std::string>();
            }
            return Result<SidecarReplicationIdentity>::Success({schema, std::move(moduleId)});
        }

        /** @brief Parses one sidecar while rejecting duplicate root keys instead of silently overwriting identities. */
        nlohmann::json ReadSidecar(std::istream &sidecarInput, bool &duplicateKey) {
            std::set<std::string, std::less<>> keys;
            auto sidecar =
                nlohmann::json::parse(sidecarInput, [&keys, &duplicateKey](const int depth, const nlohmann::json::parse_event_t event,
                                                                           const nlohmann::json &value) {
                if (depth == 1 && event == nlohmann::json::parse_event_t::key && !keys.insert(value.get<std::string>()).second)
                    duplicateKey = true;
                return true;
            });
            return sidecar;
        }

        /** @brief Reads one bounded metadata input and rejects incomplete or concurrently resized files. */
        Result<std::string> ReadFile(const std::filesystem::path &path, const std::uintmax_t maximumBytes) {
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            if (error || size == 0 || size > maximumBytes)
                return Result<std::string>::Failure(
                    MakeError(GameplayErrors::InvalidBehaviorComponent, path.string() + ": Lua input is missing or oversized."));
            std::ifstream input(path, std::ios::binary);
            std::string contents(static_cast<std::size_t>(size), '\0');
            input.read(contents.data(), static_cast<std::streamsize>(size));
            if (input.gcount() != static_cast<std::streamsize>(size) || input.peek() != std::char_traits<char>::eof())
                return Result<std::string>::Failure(
                    MakeError(GameplayErrors::InvalidBehaviorComponent, path.string() + ": Lua input could not be read completely."));
            return Result<std::string>::Success(std::move(contents));
        }

        /** @brief Canonical sidecar identities owned independently from JSON storage. */
        struct SidecarMetadata {
            BehaviorTypeId type;
            SidecarReplicationIdentity replication;
        };

        /** @brief Checks the versioned behavior identity shape before typed parsing. */
        bool ValidBehaviorMetadata(const nlohmann::json &sidecar) {
            return sidecar.is_object() && sidecar.value("schemaVersion", 0) == 1 && sidecar.value("runtime", "") == "lua" &&
                   sidecar.contains("behaviorTypeId") && sidecar["behaviorTypeId"].is_string();
        }

        /** @brief Parses and resolves typed metadata with sidecar-specific diagnostics. */
        Result<SidecarMetadata> ReadSidecarMetadata(const std::string &contents, const std::filesystem::path &sidecarPath) {
            std::istringstream sidecarInput{contents};
            try {
                bool duplicateKey{};
                const auto sidecar = ReadSidecar(sidecarInput, duplicateKey);
                if (duplicateKey || (sidecar.contains("replicationModuleId") && !sidecar.contains("replicationSchemaId")))
                    return Result<SidecarMetadata>::Failure(
                        MakeError(GameplayErrors::InvalidReplicationRegistration,
                                  sidecarPath.string() + ": duplicate or incomplete replication identities."));
                if (!ValidBehaviorMetadata(sidecar))
                    return Result<SidecarMetadata>::Failure(
                        MakeError(GameplayErrors::InvalidBehaviorComponent, sidecarPath.string() + ": Invalid Lua sidecar metadata."));
                auto typeId = BehaviorTypeId::Parse(sidecar["behaviorTypeId"].get<std::string>());
                if (typeId.HasError()) {
                    auto error = typeId.ErrorValue();
                    error.message = sidecarPath.string() + ": " + error.message;
                    return Result<SidecarMetadata>::Failure(std::move(error));
                }
                auto identity = ReadReplicationIdentity(sidecar, sidecarPath);
                if (identity.HasError())
                    return Result<SidecarMetadata>::Failure(identity.ErrorValue());
                auto identities = std::move(identity).Value();
                return Result<SidecarMetadata>::Success({std::move(typeId).Value(), std::move(identities)});
            } catch (const nlohmann::json::exception &exception) {
                return Result<SidecarMetadata>::Failure(
                    MakeError(GameplayErrors::InvalidBehaviorComponent, sidecarPath.string() + ": " + exception.what()));
            }
        }

    }  // namespace

    /** @copydoc LuaBehaviorProgram::LoadFiles */
    Result<std::unique_ptr<LuaBehaviorProgram>> LuaBehaviorProgram::LoadFiles(const std::filesystem::path &sourcePath,
                                                                              const std::filesystem::path &sidecarPath,
                                                                              const LuaBehaviorLimits limits) {
        auto source = ReadFile(sourcePath, 2U * 1024U * 1024U);
        if (source.HasError())
            return Result<std::unique_ptr<LuaBehaviorProgram>>::Failure(source.ErrorValue());
        auto sidecar = ReadFile(sidecarPath, 64U * 1024U);
        if (sidecar.HasError())
            return Result<std::unique_ptr<LuaBehaviorProgram>>::Failure(sidecar.ErrorValue());
        auto metadata = ReadSidecarMetadata(sidecar.Value(), sidecarPath);
        if (metadata.HasError())
            return Result<std::unique_ptr<LuaBehaviorProgram>>::Failure(metadata.ErrorValue());
        auto identity = std::move(metadata).Value();
        return Compile(std::move(source).Value(), identity.type, sourcePath.string(), limits, identity.replication.schema,
                       identity.replication.moduleId);
    }

}  // namespace Horo::Gameplay
