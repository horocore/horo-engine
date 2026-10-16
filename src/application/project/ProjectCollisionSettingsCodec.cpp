#include "ProjectCollisionCodecInternal.h"

#include <functional>
#include <new>

namespace Horo::Application {
    namespace {
        using namespace CollisionCodecInternal;
        const ErrorCodeDescriptor Invalid{.domain = ErrorDomainId{"horo.project.collision"},
                                          .code = ErrorCode{"collision.source.invalid"},
                                          .defaultSeverity = ErrorSeverity::Error,
                                          .summary = "The collision settings source is invalid.",
                                          .remediationHint = "Repair the versioned collision document before opening Physics settings.",
                                          .userActionable = true};

        /** @brief Empties a completed admitted tree before flattening destruction; checked-iterator proxy allocation remains STL-specific.
         */
        void ClearCollisionJson(Json &value) noexcept {
            if (value.is_structured())
                for (auto &child : value)
                    ClearCollisionJson(child);
            value.clear();
        }

        struct BoundedCollisionJson final {
            Json value;

            ~BoundedCollisionJson() {
                ClearCollisionJson(value);
            }
        };

        /** @brief Reads bounded presentation metadata; typed semantic validation remains in the document owner. */
        template <typename Id> std::vector<CollisionDisplayMetadata<Id>> Metadata(const Json &value, const std::size_t maximum) {
            Array(value, maximum);
            std::vector<CollisionDisplayMetadata<Id>> result;
            result.reserve(value.size());
            for (const auto &v : value) {
                Fields(v, {"id", "name", "description"});
                result.push_back({Identity<Id>(v["id"]), v["name"].get<std::string>(), v["description"].get<std::string>()});
            }
            return result;
        }

        /** @brief Writes identity-sorted display metadata, independent from immutable semantic fingerprinting. */
        template <typename Id> Json Metadata(const std::vector<CollisionDisplayMetadata<Id>> &values) {
            Json result = Json::array();
            for (const auto &v : values)
                result.push_back({{"id", v.id.ToString()}, {"name", v.name}, {"description", v.description}});
            return result;
        }
    }  // namespace

    /** @copydoc DecodeProjectCollisionSettings */
    Result<ProjectCollisionSettings> DecodeProjectCollisionSettings(const std::string_view source) {
        using namespace CollisionCodecInternal;
        try {
            if (source.empty() || source.size() > 4 * 1024 * 1024)
                return Result<ProjectCollisionSettings>::Failure(MakeError(Invalid, "Collision source exceeds the 4 MiB admission bound."));
            Foundation::JsonParseGuard guard{16};
            BoundedCollisionJson parsed{Json::parse(source, std::ref(guard))};
            const auto &root = parsed.value;
            if (guard.HasDuplicate() || guard.IsTooDeep())
                return Result<ProjectCollisionSettings>::Failure(
                    MakeError(Invalid, "Duplicate fields or excessive collision JSON nesting."));
            Fields(root, {"formatVersion", "projectId", "revision", "schema", "layerNames", "profileNames", "channelNames"});
            ProjectCollisionSettingsInput input;
            input.formatVersion = static_cast<std::uint32_t>(Unsigned(root["formatVersion"], UINT32_MAX));
            input.projectId = root["projectId"].get<std::string>();
            input.revision = Unsigned(root["revision"]);
            input.schema = ReadSchema(root["schema"]);
            input.layers = Metadata<Physics::CollisionLayerId>(root["layerNames"], 4096);
            input.profiles = Metadata<Physics::CollisionProfileId>(root["profileNames"], 1024);
            input.channels = Metadata<Physics::PhysicsQueryChannelId>(root["channelNames"], 4096);
            return ProjectCollisionSettings::Create(std::move(input));
        } catch (const std::bad_alloc &) {
            return Result<ProjectCollisionSettings>::Failure(MakeError(Invalid, "Collision source allocation failed."));
        } catch (const std::exception &) {
            return Result<ProjectCollisionSettings>::Failure(MakeError(Invalid, "Malformed or unsupported collision document value."));
        }
    }

    /** @copydoc EncodeProjectCollisionSettings */
    Result<std::string> EncodeProjectCollisionSettings(const ProjectCollisionSettings &settings) {
        try {
            const auto &input = settings.Input();
            BoundedCollisionJson encoded{Json{{"formatVersion", input.formatVersion},
                                              {"projectId", input.projectId},
                                              {"revision", input.revision},
                                              {"schema", CollisionCodecInternal::WriteSchema(settings)},
                                              {"layerNames", Metadata(input.layers)},
                                              {"profileNames", Metadata(input.profiles)},
                                              {"channelNames", Metadata(input.channels)}}};
            auto text = encoded.value.dump() + "\n";
            if (text.size() > 4 * 1024 * 1024)
                return Result<std::string>::Failure(MakeError(Invalid, "Collision source exceeds the 4 MiB admission bound."));
            return Result<std::string>::Success(std::move(text));
        } catch (const std::bad_alloc &) {
            return Result<std::string>::Failure(MakeError(Invalid, "Collision serialization allocation failed."));
        }
    }
}  // namespace Horo::Application
