#include "Horo/Runtime/Scene/PrimitiveCatalog.h"
#include "editor/document/SceneDocumentPersistenceInternal.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <limits>
#include <system_error>
#include <type_traits>
#include <utility>

namespace Horo::Editor::ScenePersistenceDetail {
    const ErrorDomainId ScenePersistenceDomain{"horo.editor.scene_persistence"};
    const ErrorCodeDescriptor ScenePathInvalid{
        .domain = ScenePersistenceDomain,
        .code = ErrorCode{"scene_persistence.path_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The project default scene path is invalid.",
        .remediationHint = "Use a project-relative defaultScene path without parent traversal.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor SceneReadFailed{
        .domain = ScenePersistenceDomain,
        .code = ErrorCode{"scene_persistence.read_failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The scene document could not be read.",
        .remediationHint = "Check that the scene is readable and within the supported size limit.",
        .retryable = true,
        .userActionable = true,
    };
    const ErrorCodeDescriptor SceneInvalid{
        .domain = ScenePersistenceDomain,
        .code = ErrorCode{"scene_persistence.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The scene document is invalid.",
        .remediationHint = "Repair the scene JSON or restore a valid recovery copy.",
        .retryable = false,
        .userActionable = true,
    };

    [[nodiscard]] Error PersistenceError(const ErrorCodeDescriptor &descriptor, std::string message) {
        return MakeError(descriptor, std::move(message));
    }

    [[nodiscard]] Result<std::string> ReadBoundedFile(const std::filesystem::path &absolutePath, const std::uintmax_t maximumBytes) {
        std::error_code error;
        const std::uintmax_t size = std::filesystem::file_size(absolutePath, error);
        if (error || size == 0 || size > maximumBytes) {
            return Result<std::string>::Failure(
                PersistenceError(SceneReadFailed, "Unable to read bounded file '" + absolutePath.string() + "'."));
        }

        std::ifstream input(absolutePath, std::ios::binary);
        if (!input.is_open()) {
            return Result<std::string>::Failure(PersistenceError(SceneReadFailed, "Unable to open '" + absolutePath.string() + "'."));
        }
        std::string contents(size, '\0');
        input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (input.bad() || input.gcount() != static_cast<std::streamsize>(contents.size())) {
            return Result<std::string>::Failure(
                PersistenceError(SceneReadFailed, "Unable to read all bytes from '" + absolutePath.string() + "'."));
        }
        return Result<std::string>::Success(std::move(contents));
    }

    [[nodiscard]] bool IsSafeProjectRelativePath(const std::filesystem::path &path) {
        if (path.empty() || path.is_absolute() || path.has_root_path()) {
            return false;
        }
        for (const std::filesystem::path &component : path) {
            if (component == "..") {
                return false;
            }
        }
        return !path.filename().empty();
    }

    [[nodiscard]] bool IsContainedBy(const std::filesystem::path &absoluteRoot, const std::filesystem::path &absoluteCandidate) {
        const std::filesystem::path relative = absoluteCandidate.lexically_relative(absoluteRoot);
        return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
    }

    [[nodiscard]] bool IsResolvedContainedBy(const std::filesystem::path &absoluteRoot, const std::filesystem::path &absoluteCandidate) {
        std::error_code error;
        const std::filesystem::path resolvedRoot = std::filesystem::weakly_canonical(absoluteRoot, error);
        if (error) {
            return false;
        }
        const std::filesystem::path resolvedCandidate = std::filesystem::weakly_canonical(absoluteCandidate, error);
        return !error && IsContainedBy(resolvedRoot, resolvedCandidate);
    }

    [[nodiscard]] Json Vec2Json(const Math::Vec2 value) {
        return Json::array({value.x, value.y});
    }

    [[nodiscard]] Json Vec3Json(const Math::Vec3 value) {
        return Json::array({value.x, value.y, value.z});
    }

    [[nodiscard]] Json QuaternionJson(const Math::Quaternion value) {
        return Json::array({value.x, value.y, value.z, value.w});
    }

    [[nodiscard]] Json TransformJson(const Math::Transform &value) {
        return Json{
            {"translation", Vec3Json(value.translation)},
            {"rotation", QuaternionJson(value.rotation)},
            {"scale", Vec3Json(value.scale)},
        };
    }

    [[nodiscard]] Result<Math::Vec2> ParseVec2(const Json &value);
    [[nodiscard]] Result<Math::Vec3> ParseVec3(const Json &value);
    [[nodiscard]] Result<Math::Quaternion> ParseQuaternion(const Json &value);

    template <std::size_t Size> [[nodiscard]] bool IsNumberArray(const Json &value) {
        if (!value.is_array() || value.size() != Size) {
            return false;
        }
        return std::ranges::all_of(value, [](const Json &item) {
            return item.is_number();
        });
    }

    [[nodiscard]] bool HasFields(const Json &value, const std::initializer_list<std::string_view> fields) {
        return value.is_object() && std::ranges::all_of(fields, [&value](const std::string_view field) {
            return value.contains(field);
        });
    }

    [[nodiscard]] bool HasUnsignedFields(const Json &value, const std::initializer_list<std::string_view> fields) {
        return std::ranges::all_of(fields, [&value](const std::string_view field) {
            return value.contains(field) && value[field].is_number_unsigned();
        });
    }

    [[nodiscard]] bool AllSucceeded(const std::initializer_list<bool> results) {
        return std::ranges::all_of(results, std::identity{});
    }

    [[nodiscard]] Result<Math::Vec2> ParseVec2(const Json &value) {
        if (!IsNumberArray<2>(value)) {
            return Result<Math::Vec2>::Failure(PersistenceError(SceneInvalid, "Expected a two-component vector."));
        }
        return Result<Math::Vec2>::Success({value[0].get<float>(), value[1].get<float>()});
    }

    [[nodiscard]] Result<Math::Vec3> ParseVec3(const Json &value) {
        if (!IsNumberArray<3>(value)) {
            return Result<Math::Vec3>::Failure(PersistenceError(SceneInvalid, "Expected a three-component vector."));
        }
        return Result<Math::Vec3>::Success({value[0].get<float>(), value[1].get<float>(), value[2].get<float>()});
    }

    [[nodiscard]] Result<Math::Quaternion> ParseQuaternion(const Json &value) {
        if (!IsNumberArray<4>(value)) {
            return Result<Math::Quaternion>::Failure(PersistenceError(SceneInvalid, "Expected a quaternion."));
        }
        return Result<Math::Quaternion>::Success(
            {value[0].get<float>(), value[1].get<float>(), value[2].get<float>(), value[3].get<float>()});
    }

    [[nodiscard]] Result<Math::Transform> ParseTransform(const Json &value) {
        if (!value.is_object() || !value.contains("translation") || !value.contains("rotation") || !value.contains("scale")) {
            return Result<Math::Transform>::Failure(PersistenceError(SceneInvalid, "Scene transform is incomplete."));
        }
        auto translation = ParseVec3(value["translation"]);
        auto rotation = ParseQuaternion(value["rotation"]);
        auto scale = ParseVec3(value["scale"]);
        if (translation.HasError() || rotation.HasError() || scale.HasError()) {
            return Result<Math::Transform>::Failure(PersistenceError(SceneInvalid, "Scene transform is invalid."));
        }
        return Result<Math::Transform>::Success({.translation = translation.Value(), .rotation = rotation.Value(), .scale = scale.Value()});
    }

    template <typename Parameters> [[nodiscard]] Json SerializePrimitiveParameters(const Parameters &parameters) {
        if constexpr (std::is_same_v<Parameters, Runtime::BoxMeshParameters>) {
            return Json{{"size", Vec3Json(parameters.size)}};
        } else if constexpr (std::is_same_v<Parameters, Runtime::SphereMeshParameters>) {
            return Json{{"radius", parameters.radius}, {"slices", parameters.slices}, {"stacks", parameters.stacks}};
        } else if constexpr (std::is_same_v<Parameters, Runtime::CapsuleMeshParameters>) {
            return Json{{"radius", parameters.radius},
                        {"totalHeight", parameters.totalHeight},
                        {"radialSegments", parameters.radialSegments},
                        {"hemisphereRings", parameters.hemisphereRings}};
        } else if constexpr (std::is_same_v<Parameters, Runtime::CylinderMeshParameters>) {
            return Json{{"radius", parameters.radius}, {"height", parameters.height}, {"radialSegments", parameters.radialSegments}};
        } else if constexpr (std::is_same_v<Parameters, Runtime::ConeMeshParameters>) {
            return Json{{"radius", parameters.radius}, {"height", parameters.height}, {"radialSegments", parameters.radialSegments}};
        } else if constexpr (std::is_same_v<Parameters, Runtime::PlaneMeshParameters> ||
                             std::is_same_v<Parameters, Runtime::QuadMeshParameters>) {
            return Json{{"size", Vec2Json(parameters.size)}};
        } else {
            return Json::object();
        }
    }

    [[nodiscard]] Json PrimitiveParametersJson(const PrimitiveMeshDescriptor &descriptor) {
        return std::visit([]<typename T>(const T &parameters) {
            return SerializePrimitiveParameters(parameters);
        }, descriptor.parameters);
    }

    [[nodiscard]] Json PrimitiveJson(const PrimitiveMeshDescriptor &descriptor) {
        const Runtime::PrimitiveDescriptor *catalog = Runtime::PrimitiveCatalog::Find(descriptor.type);
        return Json{
            {"id", catalog == nullptr ? "" : std::string{catalog->id.value}},
            {"version", descriptor.version.value},
            {"parameters", PrimitiveParametersJson(descriptor)},
        };
    }

    [[nodiscard]] bool HasNumber(const Json &parameters, const char *key) {
        return parameters.contains(key) && parameters[key].is_number();
    }

    [[nodiscard]] bool HasUnsignedNumber(const Json &parameters, const char *key) {
        return parameters.contains(key) && parameters[key].is_number_unsigned();
    }

    [[nodiscard]] Result<Runtime::PrimitiveMeshParameters> ParseBoxParameters(const Json &parameters) {
        if (!parameters.contains("size"))
            return Result<Runtime::PrimitiveMeshParameters>::Failure(
                PersistenceError(SceneInvalid, "Box primitive parameters are incomplete."));
        auto size = ParseVec3(parameters["size"]);
        if (size.HasError())
            return Result<Runtime::PrimitiveMeshParameters>::Failure(size.ErrorValue());
        return Result<Runtime::PrimitiveMeshParameters>::Success(Runtime::BoxMeshParameters{size.Value()});
    }

    [[nodiscard]] Result<Runtime::PrimitiveMeshParameters> ParseSphereParameters(const Json &parameters) {
        if (!HasNumber(parameters, "radius") || !HasUnsignedNumber(parameters, "slices") || !HasUnsignedNumber(parameters, "stacks"))
            return Result<Runtime::PrimitiveMeshParameters>::Failure(
                PersistenceError(SceneInvalid, "Sphere primitive parameters are incomplete."));
        return Result<Runtime::PrimitiveMeshParameters>::Success(Runtime::SphereMeshParameters{parameters["radius"].get<float>(),
                                                                                               parameters["slices"].get<std::uint32_t>(),
                                                                                               parameters["stacks"].get<std::uint32_t>()});
    }

    [[nodiscard]] Result<Runtime::PrimitiveMeshParameters> ParseCapsuleParameters(const Json &parameters) {
        if (!HasNumber(parameters, "radius") || !HasNumber(parameters, "totalHeight") || !HasUnsignedNumber(parameters, "radialSegments") ||
            !HasUnsignedNumber(parameters, "hemisphereRings"))
            return Result<Runtime::PrimitiveMeshParameters>::Failure(
                PersistenceError(SceneInvalid, "Capsule primitive parameters are incomplete."));
        return Result<Runtime::PrimitiveMeshParameters>::Success(
            Runtime::CapsuleMeshParameters{parameters["radius"].get<float>(), parameters["totalHeight"].get<float>(),
                                           parameters["radialSegments"].get<std::uint32_t>(),
                                           parameters["hemisphereRings"].get<std::uint32_t>()});
    }

    [[nodiscard]] Result<Runtime::PrimitiveMeshParameters> ParseCylinderParameters(const Json &parameters) {
        if (!HasNumber(parameters, "radius") || !HasNumber(parameters, "height") || !HasUnsignedNumber(parameters, "radialSegments"))
            return Result<Runtime::PrimitiveMeshParameters>::Failure(
                PersistenceError(SceneInvalid, "Cylinder primitive parameters are incomplete."));
        return Result<Runtime::PrimitiveMeshParameters>::Success(
            Runtime::CylinderMeshParameters{parameters["radius"].get<float>(), parameters["height"].get<float>(),
                                            parameters["radialSegments"].get<std::uint32_t>()});
    }

    [[nodiscard]] Result<Runtime::PrimitiveMeshParameters> ParseConeParameters(const Json &parameters) {
        if (!HasNumber(parameters, "radius") || !HasNumber(parameters, "height") || !HasUnsignedNumber(parameters, "radialSegments"))
            return Result<Runtime::PrimitiveMeshParameters>::Failure(
                PersistenceError(SceneInvalid, "Cone primitive parameters are incomplete."));
        return Result<Runtime::PrimitiveMeshParameters>::Success(
            Runtime::ConeMeshParameters{parameters["radius"].get<float>(), parameters["height"].get<float>(),
                                        parameters["radialSegments"].get<std::uint32_t>()});
    }

    [[nodiscard]] Result<Runtime::PrimitiveMeshParameters> ParsePlanarParameters(const Runtime::PrimitiveMeshType meshType,
                                                                                 const Json &parameters) {
        if (!parameters.contains("size"))
            return Result<Runtime::PrimitiveMeshParameters>::Failure(
                PersistenceError(SceneInvalid, "Plane or quad primitive parameters are incomplete."));
        auto size = ParseVec2(parameters["size"]);
        if (size.HasError())
            return Result<Runtime::PrimitiveMeshParameters>::Failure(size.ErrorValue());
        if (meshType == Runtime::PrimitiveMeshType::Plane)
            return Result<Runtime::PrimitiveMeshParameters>::Success(Runtime::PlaneMeshParameters{size.Value()});
        return Result<Runtime::PrimitiveMeshParameters>::Success(Runtime::QuadMeshParameters{size.Value()});
    }

    [[nodiscard]] Result<Runtime::PrimitiveMeshParameters> ParsePrimitiveParameters(const Runtime::PrimitiveMeshType meshType,
                                                                                    const Json &parameters) {
        using enum Runtime::PrimitiveMeshType;
        switch (meshType) {
            case Box:
                return ParseBoxParameters(parameters);
            case Sphere:
                return ParseSphereParameters(parameters);
            case Capsule:
                return ParseCapsuleParameters(parameters);
            case Cylinder:
                return ParseCylinderParameters(parameters);
            case Cone:
                return ParseConeParameters(parameters);
            case Plane:
            case Quad:
                return ParsePlanarParameters(meshType, parameters);
        }
        return Result<Runtime::PrimitiveMeshParameters>::Failure(PersistenceError(SceneInvalid, "Primitive descriptor is unsupported."));
    }

    [[nodiscard]] Result<PrimitiveMeshDescriptor> ParsePrimitive(const Json &value) {
        if (!value.is_object() || !value.contains("id") || !value["id"].is_string() || !value.contains("version") ||
            !value["version"].is_number_unsigned() || !value.contains("parameters") || !value["parameters"].is_object()) {
            return Result<PrimitiveMeshDescriptor>::Failure(PersistenceError(SceneInvalid, "Primitive descriptor is incomplete."));
        }
        const Runtime::PrimitiveDescriptor *catalog = Runtime::PrimitiveCatalog::Find(value["id"].get_ref<const std::string &>());
        if (catalog == nullptr || !catalog->meshType.has_value()) {
            return Result<PrimitiveMeshDescriptor>::Failure(
                PersistenceError(SceneInvalid, "Primitive descriptor ID is not a mesh primitive."));
        }

        PrimitiveMeshDescriptor descriptor = PrimitiveMeshDescriptor::Defaults(*catalog->meshType);
        descriptor.version.value = value["version"].get<std::uint32_t>();
        if (descriptor.version.value != 1) {
            return Result<PrimitiveMeshDescriptor>::Failure(PersistenceError(SceneInvalid, "Primitive descriptor version is unsupported."));
        }

        auto parsedParams = ParsePrimitiveParameters(*catalog->meshType, value["parameters"]);
        if (parsedParams.HasError()) {
            return Result<PrimitiveMeshDescriptor>::Failure(parsedParams.ErrorValue());
        }
        descriptor.parameters = std::move(parsedParams).Value();
        return Result<PrimitiveMeshDescriptor>::Success(std::move(descriptor));
    }
}  // namespace Horo::Editor::ScenePersistenceDetail
