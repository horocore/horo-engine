#include "Horo/Application/ProjectSurfaceSettings.h"
#include "JsonUtils.h"

#include <new>
#include <stdexcept>

namespace Horo::Application {
    namespace {
        using Foundation::Json;

        /** @brief Rejects unknown and missing fields before accessing values. */
        void Fields(const Json &value, std::initializer_list<std::string_view> fields) {
            if (!Foundation::HasAllowedFields(value, fields))
                throw std::invalid_argument("Surface document contains unknown or missing fields.");
        }

        /** @brief Reads exact unsigned integers without floating-point or signed narrowing. */
        std::uint64_t Unsigned(const Json &value, const std::uint64_t maximum = UINT64_MAX) {
            if (!value.is_number_unsigned() && (!value.is_number_integer() || value.get<std::int64_t>() < 0))
                throw std::invalid_argument("Surface document requires an unsigned integer.");
            const auto number = value.get<std::uint64_t>();
            if (number > maximum)
                throw std::invalid_argument("Surface integer exceeds its wire capacity.");
            return number;
        }

        /** @brief Parses one canonical typed semantic UUID without accepting an asset identity. */
        Physics::SurfaceMaterialId Identity(const Json &value) {
            const auto parsed = Physics::SurfaceMaterialId::Parse(value.get<std::string>());
            if (parsed.HasError())
                throw std::invalid_argument("Surface ID must be a nonzero canonical lowercase UUID.");
            return parsed.Value();
        }

        /** @brief Bounds the entry count before copying any display values. */
        std::vector<Physics::SurfaceCatalogEntry> Entries(const Json &value) {
            if (!value.is_array() || value.size() > 256)
                throw std::invalid_argument("Surface catalog requires an array of at most 256 entries.");
            std::vector<Physics::SurfaceCatalogEntry> entries;
            entries.reserve(value.size());
            for (const auto &entry : value) {
                Fields(entry, {"id", "displayName"});
                const auto id = Identity(entry.at("id"));
                entries.emplace_back(id, entry.at("displayName").get<std::string>());
            }
            return entries;
        }

        /** @brief Decodes only the closed v1 authoring shape frozen under HoroProjectVersion. */
        ProjectSurfaceSettingsInput ReadInput(const Json &root) {
            Fields(root, {"schemaVersion", "projectId", "revision", "defaultSurfaceId", "surfaces"});
            ProjectSurfaceSettingsInput input;
            input.schemaVersion = static_cast<std::uint32_t>(Unsigned(root.at("schemaVersion"), UINT32_MAX));
            input.projectId = root.at("projectId").get<std::string>();
            input.revision = Unsigned(root.at("revision"));
            if (!root.at("defaultSurfaceId").is_null())
                input.defaultSurfaceId = Identity(root.at("defaultSurfaceId"));
            input.surfaces = Entries(root.at("surfaces"));
            return input;
        }
    }  // namespace

    /** @copydoc DecodeProjectSurfaceSettings */
    Result<ProjectSurfaceSettings> DecodeProjectSurfaceSettings(const std::string_view source) {
        try {
            if (source.empty() || source.size() > MaximumProjectSurfaceBytes)
                return Result<ProjectSurfaceSettings>::Failure(MakeError(ProjectSurfaceErrors::Capacity));
            Foundation::JsonParseGuard guard{8};
            auto root = guard.Parse(source.begin(), source.end());
            if (guard.HasDuplicate() || guard.IsTooDeep())
                return Result<ProjectSurfaceSettings>::Failure(
                    MakeError(ProjectSurfaceErrors::Invalid, "Duplicate keys or excessive JSON nesting."));
            if (Unsigned(root.at("schemaVersion"), UINT32_MAX) != 1)
                return Result<ProjectSurfaceSettings>::Failure(MakeError(ProjectSurfaceErrors::Version));
            return ProjectSurfaceSettings::Create(ReadInput(root));
        } catch (const std::bad_alloc &) {
            return Result<ProjectSurfaceSettings>::Failure(MakeError(ProjectSurfaceErrors::Capacity));
        } catch (const Json::exception &) {
            return Result<ProjectSurfaceSettings>::Failure(
                MakeError(ProjectSurfaceErrors::Invalid, "Malformed surface catalog JSON or field type."));
        } catch (const std::invalid_argument &error) {
            return Result<ProjectSurfaceSettings>::Failure(MakeError(ProjectSurfaceErrors::Invalid, error.what()));
        }
    }

    /** @copydoc EncodeProjectSurfaceSettings */
    Result<std::string> EncodeProjectSurfaceSettings(const ProjectSurfaceSettings &settings) {
        try {
            const auto &input = settings.Input();
            Json entries = Json::array();
            for (const auto &entry : input.surfaces)
                entries.push_back({{"id", entry.id.ToString()}, {"displayName", entry.displayName}});
            const Json root{{"schemaVersion", input.schemaVersion},
                            {"projectId", input.projectId},
                            {"revision", input.revision},
                            {"defaultSurfaceId", input.defaultSurfaceId ? Json(input.defaultSurfaceId->ToString()) : Json(nullptr)},
                            {"surfaces", std::move(entries)}};
            auto source = root.dump() + "\n";
            if (source.size() > MaximumProjectSurfaceBytes)
                return Result<std::string>::Failure(MakeError(ProjectSurfaceErrors::Capacity));
            return Result<std::string>::Success(std::move(source));
        } catch (const std::bad_alloc &) {
            return Result<std::string>::Failure(MakeError(ProjectSurfaceErrors::Capacity));
        }
    }
}  // namespace Horo::Application
