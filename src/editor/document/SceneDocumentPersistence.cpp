#include "editor/document/SceneDocumentPersistence.h"

#include "Horo/Foundation/Sha256.h"
#include "Horo/Runtime/Scene/PrimitiveCatalog.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

namespace Horo::Editor {
    namespace {
        using Json = nlohmann::json;

        constexpr std::uint32_t kSceneSchemaVersion = 1;
        constexpr std::uintmax_t kMaximumProjectMetadataBytes = 64U * 1024U;
        constexpr std::uintmax_t kMaximumSceneBytes = 16U * 1024U * 1024U;
        constexpr std::uintmax_t kMaximumRecoveryBytes = 20U * 1024U * 1024U;
        constexpr std::size_t kMaximumSceneObjects = 100'000;

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

        [[nodiscard]] bool IsResolvedContainedBy(const std::filesystem::path &absoluteRoot,
                                                 const std::filesystem::path &absoluteCandidate) {
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

        [[nodiscard]] Json BehaviorFieldValueJson(const Gameplay::BehaviorFieldValue &value) {
            return std::visit([]<typename T>(const T &typed) -> Json {
                if constexpr (std::is_same_v<T, std::monostate>) {
                    return {{"type", "null"}, {"value", nullptr}};
                } else if constexpr (std::is_same_v<T, bool>) {
                    return {{"type", "bool"}, {"value", typed}};
                } else if constexpr (std::is_same_v<T, std::int64_t>) {
                    return {{"type", "int"}, {"value", typed}};
                } else if constexpr (std::is_same_v<T, double>) {
                    return {{"type", "number"}, {"value", typed}};
                } else if constexpr (std::is_same_v<T, std::string>) {
                    return {{"type", "string"}, {"value", typed}};
                } else if constexpr (std::is_same_v<T, Math::Vec2>) {
                    return {{"type", "vec2"}, {"value", Vec2Json(typed)}};
                } else if constexpr (std::is_same_v<T, Math::Vec3>) {
                    return {{"type", "vec3"}, {"value", Vec3Json(typed)}};
                } else {
                    return {{"type", "quaternion"}, {"value", QuaternionJson(typed)}};
                }
            }, value);
        }

        [[nodiscard]] Result<Gameplay::BehaviorFieldValue> ParseBehaviorFieldValue(const Json &value) {
            if (!value.is_object() || !value.contains("type") || !value["type"].is_string() || !value.contains("value")) {
                return Result<Gameplay::BehaviorFieldValue>::Failure(PersistenceError(SceneInvalid, "Behavior field is invalid."));
            }
            const std::string type = value["type"].get<std::string>();
            const Json &payload = value["value"];
            if (type == "null" && payload.is_null()) {
                return Result<Gameplay::BehaviorFieldValue>::Success(std::monostate{});
            }
            if (type == "bool" && payload.is_boolean()) {
                return Result<Gameplay::BehaviorFieldValue>::Success(payload.get<bool>());
            }
            if (type == "int" && payload.is_number_integer()) {
                return Result<Gameplay::BehaviorFieldValue>::Success(payload.get<std::int64_t>());
            }
            if (type == "number" && payload.is_number()) {
                return Result<Gameplay::BehaviorFieldValue>::Success(payload.get<double>());
            }
            if (type == "string" && payload.is_string()) {
                return Result<Gameplay::BehaviorFieldValue>::Success(payload.get<std::string>());
            }
            if (type == "vec2") {
                auto parsed = ParseVec2(payload);
                if (parsed.HasValue()) {
                    return Result<Gameplay::BehaviorFieldValue>::Success(parsed.Value());
                }
            }
            if (type == "vec3") {
                auto parsed = ParseVec3(payload);
                if (parsed.HasValue()) {
                    return Result<Gameplay::BehaviorFieldValue>::Success(parsed.Value());
                }
            }
            if (type == "quaternion") {
                auto parsed = ParseQuaternion(payload);
                if (parsed.HasValue()) {
                    return Result<Gameplay::BehaviorFieldValue>::Success(parsed.Value());
                }
            }
            return Result<Gameplay::BehaviorFieldValue>::Failure(PersistenceError(SceneInvalid, "Behavior field type is unsupported."));
        }

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
            return Result<Math::Transform>::Success(
                {.translation = translation.Value(), .rotation = rotation.Value(), .scale = scale.Value()});
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

        [[nodiscard]] Result<Runtime::PrimitiveMeshParameters> ParsePrimitiveParameters(const Runtime::PrimitiveMeshType meshType,
                                                                                        const Json &parameters) {
            const auto number = [&parameters](const char *key) {
                return parameters.contains(key) && parameters[key].is_number();
            };
            const auto unsignedNumber = [&parameters](const char *key) {
                return parameters.contains(key) && parameters[key].is_number_unsigned();
            };

            switch (meshType) {
                case Runtime::PrimitiveMeshType::Box: {
                    if (!parameters.contains("size")) {
                        return Result<Runtime::PrimitiveMeshParameters>::Failure(
                            PersistenceError(SceneInvalid, "Box primitive parameters are incomplete."));
                    }
                    auto size = ParseVec3(parameters["size"]);
                    if (size.HasError()) {
                        return Result<Runtime::PrimitiveMeshParameters>::Failure(size.ErrorValue());
                    }
                    return Result<Runtime::PrimitiveMeshParameters>::Success(Runtime::BoxMeshParameters{size.Value()});
                }
                case Runtime::PrimitiveMeshType::Sphere: {
                    if (!number("radius") || !unsignedNumber("slices") || !unsignedNumber("stacks")) {
                        return Result<Runtime::PrimitiveMeshParameters>::Failure(
                            PersistenceError(SceneInvalid, "Sphere primitive parameters are incomplete."));
                    }
                    return Result<Runtime::PrimitiveMeshParameters>::Success(
                        Runtime::SphereMeshParameters{parameters["radius"].get<float>(), parameters["slices"].get<std::uint32_t>(),
                                                      parameters["stacks"].get<std::uint32_t>()});
                }
                case Runtime::PrimitiveMeshType::Capsule: {
                    if (!number("radius") || !number("totalHeight") || !unsignedNumber("radialSegments") ||
                        !unsignedNumber("hemisphereRings")) {
                        return Result<Runtime::PrimitiveMeshParameters>::Failure(
                            PersistenceError(SceneInvalid, "Capsule primitive parameters are incomplete."));
                    }
                    return Result<Runtime::PrimitiveMeshParameters>::Success(
                        Runtime::CapsuleMeshParameters{parameters["radius"].get<float>(), parameters["totalHeight"].get<float>(),
                                                       parameters["radialSegments"].get<std::uint32_t>(),
                                                       parameters["hemisphereRings"].get<std::uint32_t>()});
                }
                case Runtime::PrimitiveMeshType::Cylinder: {
                    if (!number("radius") || !number("height") || !unsignedNumber("radialSegments")) {
                        return Result<Runtime::PrimitiveMeshParameters>::Failure(
                            PersistenceError(SceneInvalid, "Cylinder primitive parameters are incomplete."));
                    }
                    return Result<Runtime::PrimitiveMeshParameters>::Success(
                        Runtime::CylinderMeshParameters{parameters["radius"].get<float>(), parameters["height"].get<float>(),
                                                        parameters["radialSegments"].get<std::uint32_t>()});
                }
                case Runtime::PrimitiveMeshType::Cone: {
                    if (!number("radius") || !number("height") || !unsignedNumber("radialSegments")) {
                        return Result<Runtime::PrimitiveMeshParameters>::Failure(
                            PersistenceError(SceneInvalid, "Cone primitive parameters are incomplete."));
                    }
                    return Result<Runtime::PrimitiveMeshParameters>::Success(
                        Runtime::ConeMeshParameters{parameters["radius"].get<float>(), parameters["height"].get<float>(),
                                                    parameters["radialSegments"].get<std::uint32_t>()});
                }
                case Runtime::PrimitiveMeshType::Plane:
                case Runtime::PrimitiveMeshType::Quad: {
                    if (!parameters.contains("size")) {
                        return Result<Runtime::PrimitiveMeshParameters>::Failure(
                            PersistenceError(SceneInvalid, "Plane or quad primitive parameters are incomplete."));
                    }
                    auto size = ParseVec2(parameters["size"]);
                    if (size.HasError()) {
                        return Result<Runtime::PrimitiveMeshParameters>::Failure(size.ErrorValue());
                    }
                    if (meshType == Runtime::PrimitiveMeshType::Plane) {
                        return Result<Runtime::PrimitiveMeshParameters>::Success(Runtime::PlaneMeshParameters{size.Value()});
                    }
                    return Result<Runtime::PrimitiveMeshParameters>::Success(Runtime::QuadMeshParameters{size.Value()});
                }
            }
            return Result<Runtime::PrimitiveMeshParameters>::Failure(
                PersistenceError(SceneInvalid, "Primitive descriptor is unsupported."));
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
                return Result<PrimitiveMeshDescriptor>::Failure(
                    PersistenceError(SceneInvalid, "Primitive descriptor version is unsupported."));
            }

            auto parsedParams = ParsePrimitiveParameters(*catalog->meshType, value["parameters"]);
            if (parsedParams.HasError()) {
                return Result<PrimitiveMeshDescriptor>::Failure(parsedParams.ErrorValue());
            }
            descriptor.parameters = std::move(parsedParams).Value();
            return Result<PrimitiveMeshDescriptor>::Success(std::move(descriptor));
        }

        /** @brief Appends a navigation surface payload. */
        void AppendNavigationSurface(Json &value, const Runtime::NavigationSurfaceComponent &surface) {
            Json profiles = Json::array();
            for (const Navigation::NavigationAgentProfileId profile : surface.profiles)
                profiles.push_back(profile.Value());
            value["navigationSurface"] = {
                {"id", surface.id.Value()},
                {"definition", surface.definition.ToString()},
                {"schemaVersion", surface.schemaVersion},
                {"generation", surface.generation},
                {"bakeScope", surface.bakeScope == Runtime::NavigationBakeScope::ObjectSubtree ? "object_subtree" : "local_bounds"},
                {"localBounds", surface.localBounds ? Json{{"center", Vec3Json(surface.localBounds->center)},
                                                           {"halfExtents", Vec3Json(surface.localBounds->halfExtents)}}
                                                    : Json(nullptr)},
                {"profiles", std::move(profiles)},
                {"enabled", surface.enabled},
            };
        }

        /** @brief Appends a navigation region payload. */
        void AppendNavigationRegion(Json &value, const Runtime::NavigationRegionComponent &region) {
            value["navigationRegion"] = {
                {"id", region.id.Value()},
                {"surface", region.surface.Value()},
                {"schemaVersion", region.schemaVersion},
                {"generation", region.generation},
                {"localBounds",
                 {{"center", Vec3Json(region.localBounds.center)}, {"halfExtents", Vec3Json(region.localBounds.halfExtents)}}},
                {"sourceSelection", region.sourceSelection == Runtime::NavigationRegionSourceSelection::ExplicitContributors
                                        ? "explicit_contributors"
                                        : "static_collision_in_bounds"},
                {"mode", region.mode == Runtime::NavigationRegionMode::Include ? "include" : "exclude"},
                {"enabled", region.enabled},
            };
        }

        /** @brief Appends a navigation modifier payload. */
        void AppendNavigationModifier(Json &value, const Runtime::NavigationModifierComponent &modifier) {
            Json volume;
            if (const auto *box = std::get_if<Runtime::NavigationLocalBounds>(&modifier.volume)) {
                volume = {{"shape", "box"}, {"center", Vec3Json(box->center)}, {"halfExtents", Vec3Json(box->halfExtents)}};
            } else {
                const Runtime::NavigationCylinderVolume &cylinder = std::get<Runtime::NavigationCylinderVolume>(modifier.volume);
                volume = {{"shape", "cylinder"},
                          {"center", Vec3Json(cylinder.center)},
                          {"radius", cylinder.radius},
                          {"halfHeight", cylinder.halfHeight}};
            }
            using enum Runtime::NavigationModifierOperation;
            const char *operation = "exclude";
            if (modifier.operation == OverrideArea)
                operation = "override_area";
            else if (modifier.operation == OverrideAreaAndCost)
                operation = "override_area_and_cost";
            value["navigationModifier"] = {
                {"id", modifier.id.Value()},
                {"surface", modifier.surface.Value()},
                {"schemaVersion", modifier.schemaVersion},
                {"generation", modifier.generation},
                {"volume", std::move(volume)},
                {"operation", operation},
                {"area", modifier.area ? Json(modifier.area->Value()) : Json(nullptr)},
                {"traversalCost", modifier.traversalCost.has_value() ? Json(*modifier.traversalCost) : Json(nullptr)},
                {"enabled", modifier.enabled},
            };
        }

        /** @brief Appends a navigation link payload. */
        void AppendNavigationLink(Json &value, const Runtime::NavigationLinkComponent &link) {
            Json profiles = Json::array();
            for (const Navigation::NavigationAgentProfileId profile : link.profiles)
                profiles.push_back(profile.Value());
            using enum Runtime::NavigationLinkKind;
            const char *kind = "teleport";
            if (link.kind == Jump)
                kind = "jump";
            else if (link.kind == Ladder)
                kind = "ladder";
            else if (link.kind == Door)
                kind = "door";
            value["navigationLink"] = {
                {"id", link.id.Value()},
                {"schemaVersion", link.schemaVersion},
                {"generation", link.generation},
                {"start",
                 {{"surface", link.start.surface.Value()},
                  {"localPosition", Vec3Json(link.start.localPosition)},
                  {"connectionRadiusMeters", link.start.connectionRadiusMeters}}},
                {"end",
                 {{"surface", link.end.surface.Value()},
                  {"localPosition", Vec3Json(link.end.localPosition)},
                  {"connectionRadiusMeters", link.end.connectionRadiusMeters}}},
                {"kind", kind},
                {"direction", link.direction == Runtime::NavigationLinkDirection::StartToEnd ? "start_to_end" : "bidirectional"},
                {"profiles", std::move(profiles)},
                {"traversalCost", link.traversalCost},
                {"enabled", link.enabled},
            };
        }

        /** @brief Appends optional navigation authoring payloads without increasing the core component serializer's branching. */
        void AppendNavigationComponents(Json &value, const SceneObjectComponentSet &components) {
            if (components.navigationSurface)
                AppendNavigationSurface(value, *components.navigationSurface);
            if (components.navigationRegion)
                AppendNavigationRegion(value, *components.navigationRegion);
            if (components.navigationModifier)
                AppendNavigationModifier(value, *components.navigationModifier);
            if (components.navigationLink)
                AppendNavigationLink(value, *components.navigationLink);
        }

        [[nodiscard]] Json PhysicsPoseJson(const Runtime::AuthoredPhysicsPose &pose) {
            return {{"translation", Vec3Json(pose.translation)},
                    {"rotation", {pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w}}};
        }

        [[nodiscard]] Json PhysicsBodyReferenceJson(const Runtime::PhysicsBodyReference &reference) {
            return {{"object", reference.object.value}, {"body", reference.body.value}};
        }

        [[nodiscard]] Json PhysicsColliderSourceJson(const Runtime::PhysicsColliderSource &source) {
            if (const auto *asset = std::get_if<Runtime::PhysicsShapeAssetReference>(&source))
                return {{"kind", "asset"}, {"asset", asset->asset.ToString()}, {"subresource", asset->subresource.value}};
            const Runtime::PhysicsAnalyticCollider &analytic = std::get<Runtime::PhysicsAnalyticCollider>(source);
            if (const auto *box = std::get_if<Runtime::PhysicsBoxCollider>(&analytic))
                return {{"kind", "box"}, {"halfExtentsMeters", Vec3Json(box->halfExtentsMeters)}};
            if (const auto *sphere = std::get_if<Runtime::PhysicsSphereCollider>(&analytic))
                return {{"kind", "sphere"}, {"radiusMeters", sphere->radiusMeters}};
            if (const auto *capsule = std::get_if<Runtime::PhysicsCapsuleCollider>(&analytic))
                return {{"kind", "capsule"},
                        {"radiusMeters", capsule->radiusMeters},
                        {"cylindricalHalfHeightMeters", capsule->cylindricalHalfHeightMeters}};
            const auto &plane = std::get<Runtime::PhysicsStaticPlaneCollider>(analytic);
            return {{"kind", "static_plane"}, {"normal", Vec3Json(plane.normal)}, {"signedDistanceMeters", plane.signedDistanceMeters}};
        }

        void AppendRigidBody(Json &value, const Runtime::RigidBodyComponent &body) {
            const char *motion = body.motion == Runtime::AuthoredPhysicsMotionType::Static      ? "static"
                                 : body.motion == Runtime::AuthoredPhysicsMotionType::Kinematic ? "kinematic"
                                                                                                : "dynamic";
            Json mass = {{"kind", "none"}};
            if (const auto *explicitMass = std::get_if<Runtime::AuthoredPhysicsMass>(&body.mass))
                mass = {{"kind", "mass"}, {"kilograms", explicitMass->kilograms}};
            else if (const auto *density = std::get_if<Runtime::AuthoredPhysicsDensity>(&body.mass))
                mass = {{"kind", "density"}, {"kilogramsPerCubicMeter", density->kilogramsPerCubicMeter}};
            value["rigidBody"] = {{"id", body.id.value},
                                  {"body", body.body.value},
                                  {"schemaVersion", body.schemaVersion},
                                  {"generation", body.generation},
                                  {"motion", motion},
                                  {"mass", std::move(mass)},
                                  {"initialLinearVelocity", Vec3Json(body.initialLinearVelocity)},
                                  {"initialAngularVelocity", Vec3Json(body.initialAngularVelocity)},
                                  {"linearDampingPerSecond", body.linearDampingPerSecond},
                                  {"angularDampingPerSecond", body.angularDampingPerSecond},
                                  {"maximumLinearSpeed", body.maximumLinearSpeed},
                                  {"maximumAngularSpeed", body.maximumAngularSpeed},
                                  {"enabled", body.enabled}};
        }

        void AppendColliders(Json &value, const std::vector<Runtime::ColliderComponent> &components) {
            Json colliders = Json::array();
            for (const Runtime::ColliderComponent &collider : components) {
                Json materials = Json::array();
                for (const Runtime::PhysicsColliderMaterialBinding &binding : collider.materials)
                    materials.push_back({{"slot", binding.slot.Value()}, {"material", binding.material.ToString()}});
                colliders.push_back({{"id", collider.id.value},
                                     {"collider", collider.collider.value},
                                     {"schemaVersion", collider.schemaVersion},
                                     {"generation", collider.generation},
                                     {"body", PhysicsBodyReferenceJson(collider.body)},
                                     {"source", PhysicsColliderSourceJson(collider.source)},
                                     {"localPose", PhysicsPoseJson(collider.localPose)},
                                     {"scale", Vec3Json(collider.scale)},
                                     {"collisionProfile", collider.collisionProfile.ToString()},
                                     {"materials", std::move(materials)},
                                     {"sensor", collider.sensor},
                                     {"enabled", collider.enabled}});
            }
            if (!colliders.empty())
                value["colliders"] = std::move(colliders);
        }

        [[nodiscard]] Json PhysicsConstraintEndpointJson(const Runtime::PhysicsConstraintSecondEndpoint &endpoint) {
            if (const auto *body = std::get_if<Runtime::PhysicsConstraintBodyEndpoint>(&endpoint))
                return {{"kind", "body"}, {"body", PhysicsBodyReferenceJson(body->body)}, {"frame", PhysicsPoseJson(body->localFrame)}};
            return {{"kind", "world"}, {"frame", PhysicsPoseJson(std::get<Runtime::PhysicsConstraintWorldEndpoint>(endpoint).frame)}};
        }

        [[nodiscard]] Json PhysicsConstraintParametersJson(
            const std::variant<Runtime::PhysicsFixedConstraint, Runtime::PhysicsDistanceConstraint> &parameters) {
            if (const auto *distance = std::get_if<Runtime::PhysicsDistanceConstraint>(&parameters))
                return {{"kind", "distance"}, {"minimumMeters", distance->minimumMeters}, {"maximumMeters", distance->maximumMeters}};
            return {{"kind", "fixed"}};
        }

        void AppendPhysicsConstraints(Json &value, const std::vector<Runtime::PhysicsConstraintComponent> &components) {
            Json constraints = Json::array();
            for (const Runtime::PhysicsConstraintComponent &constraint : components) {
                constraints.push_back(
                    {{"id", constraint.id.value},
                     {"constraint", constraint.constraint.value},
                     {"schemaVersion", constraint.schemaVersion},
                     {"generation", constraint.generation},
                     {"first",
                      {{"body", PhysicsBodyReferenceJson(constraint.first.body)}, {"frame", PhysicsPoseJson(constraint.first.localFrame)}}},
                     {"second", PhysicsConstraintEndpointJson(constraint.second)},
                     {"parameters", PhysicsConstraintParametersJson(constraint.parameters)},
                     {"enabled", constraint.enabled}});
            }
            if (!constraints.empty())
                value["physicsConstraints"] = std::move(constraints);
        }

        void AppendPhysicsComponents(Json &value, const SceneObjectComponentSet &components) {
            if (components.rigidBody)
                AppendRigidBody(value, *components.rigidBody);
            AppendColliders(value, components.colliders);
            AppendPhysicsConstraints(value, components.physicsConstraints);
        }

        [[nodiscard]] Json ComponentsJson(const SceneObjectComponentSet &components) {
            Json value = Json::object();
            if (components.camera.has_value()) {
                const Runtime::CameraComponent &camera = *components.camera;
                value["camera"] = {
                    {"projection", camera.projection == Runtime::CameraProjection::Perspective ? "perspective" : "orthographic"},
                    {"verticalFieldOfViewRadians", camera.verticalFieldOfViewRadians},
                    {"orthographicHeight", camera.orthographicHeight},
                    {"nearPlane", camera.nearPlane},
                    {"farPlane", camera.farPlane},
                    {"enabled", camera.enabled},
                };
            }
            if (components.light.has_value()) {
                const Runtime::LightComponent &light = *components.light;
                using enum Runtime::LightKind;
                const char *kind = "spot";
                if (light.kind == Directional) {
                    kind = "directional";
                } else if (light.kind == Point) {
                    kind = "point";
                }
                value["light"] = {
                    {"kind", kind},
                    {"color", Vec3Json(light.color)},
                    {"intensity", light.intensity},
                    {"range", light.range},
                    {"innerConeRadians", light.innerConeRadians},
                    {"outerConeRadians", light.outerConeRadians},
                    {"enabled", light.enabled},
                };
            }
            if (components.triggerVolume.has_value()) {
                value["triggerVolume"] = {
                    {"shape", static_cast<std::uint8_t>(components.triggerVolume->shape)},
                    {"enabled", components.triggerVolume->enabled},
                };
            }
            if (components.audioSource.has_value()) {
                const Runtime::AudioSourceComponent &audio = *components.audioSource;
                value["audioSource"] = {
                    {"kind", audio.kind == Runtime::AudioSourceKind::NativeClip ? "native_clip" : "middleware_event"},
                    {"gain", audio.gain},
                    {"spatial", audio.spatial},
                    {"enabled", audio.enabled},
                };
            }
            AppendNavigationComponents(value, components);
            AppendPhysicsComponents(value, components);
            if (!components.behaviors.empty()) {
                Json behaviors = Json::array();
                for (const Gameplay::BehaviorComponent &behavior : components.behaviors) {
                    Json fields = Json::array();
                    for (const Gameplay::BehaviorField &field : behavior.fields) {
                        fields.push_back({{"name", field.name}, {"value", BehaviorFieldValueJson(field.value)}});
                    }
                    behaviors.push_back({
                        {"instanceId", behavior.instanceId.value},
                        {"typeId", behavior.typeId.Value()},
                        {"schemaVersion", behavior.schemaVersion},
                        {"enabled", behavior.enabled},
                        {"fields", std::move(fields)},
                    });
                }
                value["behaviors"] = std::move(behaviors);
            }
            return value;
        }

        [[nodiscard]] Result<Runtime::CameraComponent> ParseCameraComponent(const Json &camera) {
            if (!camera.is_object() || !camera.contains("projection") || !camera["projection"].is_string()) {
                return Result<Runtime::CameraComponent>::Failure(PersistenceError(SceneInvalid, "Camera is invalid."));
            }
            const std::string projection = camera["projection"].get<std::string>();
            if (projection != "perspective" && projection != "orthographic") {
                return Result<Runtime::CameraComponent>::Failure(PersistenceError(SceneInvalid, "Camera projection is invalid."));
            }
            return Result<Runtime::CameraComponent>::Success(Runtime::CameraComponent{
                .projection =
                    projection == "perspective" ? Runtime::CameraProjection::Perspective : Runtime::CameraProjection::Orthographic,
                .verticalFieldOfViewRadians = camera.at("verticalFieldOfViewRadians").get<float>(),
                .orthographicHeight = camera.at("orthographicHeight").get<float>(),
                .nearPlane = camera.at("nearPlane").get<float>(),
                .farPlane = camera.at("farPlane").get<float>(),
                .enabled = camera.value("enabled", true),
            });
        }

        [[nodiscard]] Result<Runtime::LightComponent> ParseLightComponent(const Json &light) {
            const std::string kind = light.at("kind").get<std::string>();
            auto color = ParseVec3(light.at("color"));
            if (color.HasError() || (kind != "directional" && kind != "point" && kind != "spot")) {
                return Result<Runtime::LightComponent>::Failure(PersistenceError(SceneInvalid, "Light is invalid."));
            }
            Runtime::LightKind lightKind = Runtime::LightKind::Spot;
            if (kind == "directional") {
                lightKind = Runtime::LightKind::Directional;
            } else if (kind == "point") {
                lightKind = Runtime::LightKind::Point;
            }
            return Result<Runtime::LightComponent>::Success(Runtime::LightComponent{
                .kind = lightKind,
                .color = color.Value(),
                .intensity = light.at("intensity").get<float>(),
                .range = light.at("range").get<float>(),
                .innerConeRadians = light.at("innerConeRadians").get<float>(),
                .outerConeRadians = light.at("outerConeRadians").get<float>(),
                .enabled = light.value("enabled", true),
            });
        }

        [[nodiscard]] Result<Runtime::TriggerVolumeComponent> ParseTriggerVolumeComponent(const Json &triggerVolume) {
            const std::uint8_t shape = triggerVolume.at("shape").get<std::uint8_t>();
            if (shape > static_cast<std::uint8_t>(Runtime::ColliderShapeType::StaticPlane)) {
                return Result<Runtime::TriggerVolumeComponent>::Failure(PersistenceError(SceneInvalid, "Trigger shape is invalid."));
            }
            return Result<Runtime::TriggerVolumeComponent>::Success(Runtime::TriggerVolumeComponent{
                .shape = static_cast<Runtime::ColliderShapeType>(shape),
                .enabled = triggerVolume.value("enabled", true),
            });
        }

        [[nodiscard]] Result<Runtime::AudioSourceComponent> ParseAudioSourceComponent(const Json &audio) {
            const std::string kind = audio.at("kind").get<std::string>();
            if (kind != "native_clip" && kind != "middleware_event") {
                return Result<Runtime::AudioSourceComponent>::Failure(PersistenceError(SceneInvalid, "Audio source is invalid."));
            }
            return Result<Runtime::AudioSourceComponent>::Success(Runtime::AudioSourceComponent{
                .kind = kind == "native_clip" ? Runtime::AudioSourceKind::NativeClip : Runtime::AudioSourceKind::MiddlewareEvent,
                .gain = audio.at("gain").get<float>(),
                .spatial = audio.at("spatial").get<bool>(),
                .enabled = audio.value("enabled", true),
            });
        }

        [[nodiscard]] Result<Runtime::NavigationLocalBounds> ParseNavigationBounds(const Json &value) {
            if (!value.is_object() || !value.contains("center") || !value.contains("halfExtents"))
                return Result<Runtime::NavigationLocalBounds>::Failure(PersistenceError(SceneInvalid, "Navigation bounds are invalid."));
            auto center = ParseVec3(value["center"]);
            auto halfExtents = ParseVec3(value["halfExtents"]);
            if (center.HasError() || halfExtents.HasError())
                return Result<Runtime::NavigationLocalBounds>::Failure(PersistenceError(SceneInvalid, "Navigation bounds are invalid."));
            return Result<Runtime::NavigationLocalBounds>::Success({center.Value(), halfExtents.Value()});
        }

        [[nodiscard]] bool HasNavigationComponentHeader(const Json &value) {
            return value.is_object() && value.contains("id") && value["id"].is_number_unsigned() && value.contains("schemaVersion") &&
                   value["schemaVersion"].is_number_unsigned() && value.contains("generation") && value["generation"].is_number_unsigned();
        }

        [[nodiscard]] bool HasNavigationSurfaceReference(const Json &value) {
            return HasNavigationComponentHeader(value) && value.contains("surface") && value["surface"].is_number_unsigned();
        }

        [[nodiscard]] Result<std::vector<Navigation::NavigationAgentProfileId>> ParseNavigationProfiles(const Json &values) {
            if (!values.is_array())
                return Result<std::vector<Navigation::NavigationAgentProfileId>>::Failure(
                    PersistenceError(SceneInvalid, "Navigation profiles must be an array."));
            std::vector<Navigation::NavigationAgentProfileId> profiles;
            profiles.reserve(values.size());
            for (const Json &value : values) {
                if (!value.is_number_unsigned())
                    return Result<std::vector<Navigation::NavigationAgentProfileId>>::Failure(
                        PersistenceError(SceneInvalid, "Navigation profile identity is invalid."));
                auto profile = Navigation::NavigationAgentProfileId::Create(value.get<std::uint64_t>());
                if (profile.HasError())
                    return Result<std::vector<Navigation::NavigationAgentProfileId>>::Failure(
                        PersistenceError(SceneInvalid, "Navigation profile identity is invalid."));
                profiles.push_back(profile.Value());
            }
            return Result<std::vector<Navigation::NavigationAgentProfileId>>::Success(std::move(profiles));
        }

        [[nodiscard]] Result<Runtime::NavigationSurfaceComponent> ParseNavigationSurface(const Json &value) {
            if (!HasNavigationComponentHeader(value) || !value.contains("definition") || !value["definition"].is_string() ||
                !value.contains("bakeScope") || !value["bakeScope"].is_string() || !value.contains("localBounds") ||
                !value.contains("profiles")) {
                return Result<Runtime::NavigationSurfaceComponent>::Failure(
                    PersistenceError(SceneInvalid, "Navigation surface schema is incomplete."));
            }
            auto id = Navigation::SurfaceId::Create(value["id"].get<std::uint64_t>());
            auto definition = Assets::AssetId::Parse(value["definition"].get<std::string>());
            auto profiles = ParseNavigationProfiles(value["profiles"]);
            const std::string scope = value["bakeScope"].get<std::string>();
            if (id.HasError() || definition.HasError() || profiles.HasError() || (scope != "object_subtree" && scope != "local_bounds"))
                return Result<Runtime::NavigationSurfaceComponent>::Failure(
                    PersistenceError(SceneInvalid, "Navigation surface identities or scope are invalid."));

            std::optional<Runtime::NavigationLocalBounds> bounds;
            if (!value["localBounds"].is_null()) {
                auto parsedBounds = ParseNavigationBounds(value["localBounds"]);
                if (parsedBounds.HasError())
                    return Result<Runtime::NavigationSurfaceComponent>::Failure(parsedBounds.ErrorValue());
                bounds = parsedBounds.Value();
            }
            Runtime::NavigationSurfaceComponent surface{
                .id = id.Value(),
                .definition = definition.Value(),
                .schemaVersion = value["schemaVersion"].get<std::uint32_t>(),
                .generation = value["generation"].get<std::uint64_t>(),
                .bakeScope =
                    scope == "object_subtree" ? Runtime::NavigationBakeScope::ObjectSubtree : Runtime::NavigationBakeScope::LocalBounds,
                .localBounds = bounds,
                .profiles = std::move(profiles).Value(),
                .enabled = value.value("enabled", true),
            };
            if (Runtime::ValidateNavigationSurfaceComponent(surface).HasError())
                return Result<Runtime::NavigationSurfaceComponent>::Failure(
                    PersistenceError(SceneInvalid, "Navigation surface payload is invalid."));
            return Result<Runtime::NavigationSurfaceComponent>::Success(std::move(surface));
        }

        [[nodiscard]] Result<Runtime::NavigationRegionComponent> ParseNavigationRegion(const Json &value) {
            if (!HasNavigationSurfaceReference(value) || !value.contains("localBounds") || !value.contains("sourceSelection") ||
                !value["sourceSelection"].is_string() || !value.contains("mode") || !value["mode"].is_string()) {
                return Result<Runtime::NavigationRegionComponent>::Failure(
                    PersistenceError(SceneInvalid, "Navigation region schema is incomplete."));
            }
            auto id = Navigation::NavigationRegionId::Create(value["id"].get<std::uint64_t>());
            auto surface = Navigation::SurfaceId::Create(value["surface"].get<std::uint64_t>());
            auto bounds = ParseNavigationBounds(value["localBounds"]);
            const std::string source = value["sourceSelection"].get<std::string>();
            const std::string mode = value["mode"].get<std::string>();
            if (id.HasError() || surface.HasError() || bounds.HasError() ||
                (source != "explicit_contributors" && source != "static_collision_in_bounds") || (mode != "include" && mode != "exclude")) {
                return Result<Runtime::NavigationRegionComponent>::Failure(
                    PersistenceError(SceneInvalid, "Navigation region identity, bounds, or policy is invalid."));
            }
            Runtime::NavigationRegionComponent region{
                .id = id.Value(),
                .surface = surface.Value(),
                .localBounds = bounds.Value(),
                .sourceSelection = source == "explicit_contributors" ? Runtime::NavigationRegionSourceSelection::ExplicitContributors
                                                                     : Runtime::NavigationRegionSourceSelection::StaticCollisionInBounds,
                .mode = mode == "include" ? Runtime::NavigationRegionMode::Include : Runtime::NavigationRegionMode::Exclude,
                .enabled = value.value("enabled", true),
            };
            region.schemaVersion = value["schemaVersion"].get<std::uint32_t>();
            region.generation = value["generation"].get<std::uint64_t>();
            if (Runtime::ValidateNavigationRegionComponent(region).HasError())
                return Result<Runtime::NavigationRegionComponent>::Failure(
                    PersistenceError(SceneInvalid, "Navigation region payload is invalid."));
            return Result<Runtime::NavigationRegionComponent>::Success(region);
        }

        [[nodiscard]] Result<Runtime::NavigationModifierVolume> ParseNavigationModifierVolume(const Json &volumeValue) {
            if (!volumeValue.contains("shape") || !volumeValue["shape"].is_string() || !volumeValue.contains("center"))
                return Result<Runtime::NavigationModifierVolume>::Failure(
                    PersistenceError(SceneInvalid, "Navigation modifier volume is invalid."));
            auto center = ParseVec3(volumeValue["center"]);
            const std::string shape = volumeValue["shape"].get<std::string>();
            if (center.HasError() || (shape != "box" && shape != "cylinder"))
                return Result<Runtime::NavigationModifierVolume>::Failure(
                    PersistenceError(SceneInvalid, "Navigation modifier volume is invalid."));
            if (shape == "box") {
                if (!volumeValue.contains("halfExtents"))
                    return Result<Runtime::NavigationModifierVolume>::Failure(
                        PersistenceError(SceneInvalid, "Navigation modifier box is incomplete."));
                auto bounds = ParseNavigationBounds(volumeValue);
                if (bounds.HasError())
                    return Result<Runtime::NavigationModifierVolume>::Failure(bounds.ErrorValue());
                return Result<Runtime::NavigationModifierVolume>::Success(bounds.Value());
            }
            if (!volumeValue.contains("radius") || !volumeValue["radius"].is_number() || !volumeValue.contains("halfHeight") ||
                !volumeValue["halfHeight"].is_number())
                return Result<Runtime::NavigationModifierVolume>::Failure(
                    PersistenceError(SceneInvalid, "Navigation modifier cylinder is incomplete."));
            return Result<Runtime::NavigationModifierVolume>::Success(
                Runtime::NavigationCylinderVolume{center.Value(), volumeValue["radius"].get<float>(),
                                                  volumeValue["halfHeight"].get<float>()});
        }

        struct ParsedNavigationModifierPolicy final {
            Runtime::NavigationModifierOperation operation;
            std::optional<Navigation::NavigationAreaId> area;
            std::optional<float> traversalCost;
        };

        [[nodiscard]] Result<ParsedNavigationModifierPolicy> ParseNavigationModifierPolicy(const Json &value) {
            const std::string operationName = value["operation"].get<std::string>();
            if (operationName != "exclude" && operationName != "override_area" && operationName != "override_area_and_cost")
                return Result<ParsedNavigationModifierPolicy>::Failure(
                    PersistenceError(SceneInvalid, "Navigation modifier operation is invalid."));
            std::optional<Navigation::NavigationAreaId> area;
            if (!value["area"].is_null()) {
                if (!value["area"].is_number_unsigned())
                    return Result<ParsedNavigationModifierPolicy>::Failure(
                        PersistenceError(SceneInvalid, "Navigation modifier area is invalid."));
                auto parsedArea = Navigation::NavigationAreaId::Create(value["area"].get<std::uint64_t>());
                if (parsedArea.HasError())
                    return Result<ParsedNavigationModifierPolicy>::Failure(
                        PersistenceError(SceneInvalid, "Navigation modifier area is invalid."));
                area = parsedArea.Value();
            }
            std::optional<float> traversalCost;
            if (!value["traversalCost"].is_null()) {
                if (!value["traversalCost"].is_number())
                    return Result<ParsedNavigationModifierPolicy>::Failure(
                        PersistenceError(SceneInvalid, "Navigation modifier traversal cost is invalid."));
                traversalCost = value["traversalCost"].get<float>();
            }
            Runtime::NavigationModifierOperation operation = Runtime::NavigationModifierOperation::Exclude;
            if (operationName == "override_area")
                operation = Runtime::NavigationModifierOperation::OverrideArea;
            else if (operationName == "override_area_and_cost")
                operation = Runtime::NavigationModifierOperation::OverrideAreaAndCost;
            return Result<ParsedNavigationModifierPolicy>::Success({operation, area, traversalCost});
        }

        [[nodiscard]] Result<Runtime::NavigationModifierComponent> ParseNavigationModifier(const Json &value) {
            if (!HasNavigationSurfaceReference(value) || !value.contains("volume") || !value["volume"].is_object() ||
                !value.contains("operation") || !value["operation"].is_string() || !value.contains("area") ||
                !value.contains("traversalCost"))
                return Result<Runtime::NavigationModifierComponent>::Failure(
                    PersistenceError(SceneInvalid, "Navigation modifier schema is incomplete."));
            auto id = Navigation::NavigationModifierId::Create(value["id"].get<std::uint64_t>());
            auto surface = Navigation::SurfaceId::Create(value["surface"].get<std::uint64_t>());
            auto volume = ParseNavigationModifierVolume(value["volume"]);
            auto policy = ParseNavigationModifierPolicy(value);
            if (id.HasError() || surface.HasError())
                return Result<Runtime::NavigationModifierComponent>::Failure(
                    PersistenceError(SceneInvalid, "Navigation modifier identity or surface is invalid."));
            if (volume.HasError())
                return Result<Runtime::NavigationModifierComponent>::Failure(volume.ErrorValue());
            if (policy.HasError())
                return Result<Runtime::NavigationModifierComponent>::Failure(policy.ErrorValue());
            Runtime::NavigationModifierComponent modifier{
                .id = id.Value(),
                .surface = surface.Value(),
                .volume = std::move(volume).Value(),
                .operation = policy.Value().operation,
                .area = policy.Value().area,
                .traversalCost = policy.Value().traversalCost,
                .enabled = value.value("enabled", true),
            };
            modifier.schemaVersion = value["schemaVersion"].get<std::uint32_t>();
            modifier.generation = value["generation"].get<std::uint64_t>();
            if (Runtime::ValidateNavigationModifierComponent(modifier).HasError())
                return Result<Runtime::NavigationModifierComponent>::Failure(
                    PersistenceError(SceneInvalid, "Navigation modifier payload is invalid."));
            return Result<Runtime::NavigationModifierComponent>::Success(std::move(modifier));
        }

        [[nodiscard]] Result<Runtime::NavigationLinkEndpoint> ParseNavigationLinkEndpoint(const Json &value) {
            if (!value.is_object() || !value.contains("surface") || !value["surface"].is_number_unsigned() ||
                !value.contains("localPosition") || !value.contains("connectionRadiusMeters") ||
                !value["connectionRadiusMeters"].is_number()) {
                return Result<Runtime::NavigationLinkEndpoint>::Failure(
                    PersistenceError(SceneInvalid, "Navigation link endpoint schema is incomplete."));
            }
            auto surface = Navigation::SurfaceId::Create(value["surface"].get<std::uint64_t>());
            auto position = ParseVec3(value["localPosition"]);
            if (surface.HasError() || position.HasError())
                return Result<Runtime::NavigationLinkEndpoint>::Failure(
                    PersistenceError(SceneInvalid, "Navigation link endpoint is invalid."));
            return Result<Runtime::NavigationLinkEndpoint>::Success(
                {surface.Value(), position.Value(), value["connectionRadiusMeters"].get<float>()});
        }

        [[nodiscard]] Result<Runtime::NavigationLinkKind> ParseNavigationLinkKind(const std::string_view name) {
            using enum Runtime::NavigationLinkKind;
            if (name == "jump")
                return Result<Runtime::NavigationLinkKind>::Success(Jump);
            if (name == "ladder")
                return Result<Runtime::NavigationLinkKind>::Success(Ladder);
            if (name == "door")
                return Result<Runtime::NavigationLinkKind>::Success(Door);
            if (name == "teleport")
                return Result<Runtime::NavigationLinkKind>::Success(Teleport);
            return Result<Runtime::NavigationLinkKind>::Failure(PersistenceError(SceneInvalid, "Navigation link kind is invalid."));
        }

        [[nodiscard]] Result<Runtime::NavigationLinkDirection> ParseNavigationLinkDirection(const std::string_view name) {
            if (name == "start_to_end")
                return Result<Runtime::NavigationLinkDirection>::Success(Runtime::NavigationLinkDirection::StartToEnd);
            if (name == "bidirectional")
                return Result<Runtime::NavigationLinkDirection>::Success(Runtime::NavigationLinkDirection::Bidirectional);
            return Result<Runtime::NavigationLinkDirection>::Failure(
                PersistenceError(SceneInvalid, "Navigation link direction is invalid."));
        }

        [[nodiscard]] Result<Runtime::NavigationLinkComponent> ParseNavigationLink(const Json &value) {
            if (!HasNavigationComponentHeader(value) || !value.contains("start") || !value.contains("end") || !value.contains("kind") ||
                !value["kind"].is_string() || !value.contains("direction") || !value["direction"].is_string() ||
                !value.contains("profiles") || !value.contains("traversalCost") || !value["traversalCost"].is_number()) {
                return Result<Runtime::NavigationLinkComponent>::Failure(
                    PersistenceError(SceneInvalid, "Navigation link schema is incomplete."));
            }
            auto id = Navigation::NavigationLinkId::Create(value["id"].get<std::uint64_t>());
            auto start = ParseNavigationLinkEndpoint(value["start"]);
            auto end = ParseNavigationLinkEndpoint(value["end"]);
            auto kind = ParseNavigationLinkKind(value["kind"].get<std::string>());
            auto direction = ParseNavigationLinkDirection(value["direction"].get<std::string>());
            auto profiles = ParseNavigationProfiles(value["profiles"]);
            if (id.HasError())
                return Result<Runtime::NavigationLinkComponent>::Failure(
                    PersistenceError(SceneInvalid, "Navigation link identity is invalid."));
            if (start.HasError())
                return Result<Runtime::NavigationLinkComponent>::Failure(start.ErrorValue());
            if (end.HasError())
                return Result<Runtime::NavigationLinkComponent>::Failure(end.ErrorValue());
            if (kind.HasError())
                return Result<Runtime::NavigationLinkComponent>::Failure(kind.ErrorValue());
            if (direction.HasError())
                return Result<Runtime::NavigationLinkComponent>::Failure(direction.ErrorValue());
            if (profiles.HasError())
                return Result<Runtime::NavigationLinkComponent>::Failure(profiles.ErrorValue());
            Runtime::NavigationLinkComponent link{
                .id = id.Value(),
                .schemaVersion = value["schemaVersion"].get<std::uint32_t>(),
                .generation = value["generation"].get<std::uint64_t>(),
                .start = start.Value(),
                .end = end.Value(),
                .kind = kind.Value(),
                .direction = direction.Value(),
                .profiles = std::move(profiles).Value(),
                .traversalCost = value["traversalCost"].get<float>(),
                .enabled = value.value("enabled", true),
            };
            if (Runtime::ValidateNavigationLinkComponent(link).HasError())
                return Result<Runtime::NavigationLinkComponent>::Failure(
                    PersistenceError(SceneInvalid, "Navigation link payload is invalid."));
            return Result<Runtime::NavigationLinkComponent>::Success(std::move(link));
        }

        [[nodiscard]] Result<Runtime::AuthoredPhysicsPose> ParsePhysicsPose(const Json &value) {
            if (!value.is_object() || !value.contains("translation") || !value.contains("rotation"))
                return Result<Runtime::AuthoredPhysicsPose>::Failure(PersistenceError(SceneInvalid, "Physics pose is incomplete."));
            auto translation = ParseVec3(value["translation"]);
            auto rotation = ParseQuaternion(value["rotation"]);
            if (translation.HasError() || rotation.HasError())
                return Result<Runtime::AuthoredPhysicsPose>::Failure(PersistenceError(SceneInvalid, "Physics pose is invalid."));
            return Result<Runtime::AuthoredPhysicsPose>::Success({translation.Value(), rotation.Value()});
        }

        [[nodiscard]] Result<Runtime::PhysicsBodyReference> ParsePhysicsBodyReference(const Json &value) {
            if (!value.is_object() || !value.contains("object") || !value["object"].is_number_unsigned() || !value.contains("body") ||
                !value["body"].is_number_unsigned())
                return Result<Runtime::PhysicsBodyReference>::Failure(
                    PersistenceError(SceneInvalid, "Physics body reference is incomplete."));
            Runtime::PhysicsBodyReference reference{{value["object"].get<std::uint64_t>()}, {value["body"].get<std::uint64_t>()}};
            if (!reference.object.IsValid() || !reference.body.IsValid())
                return Result<Runtime::PhysicsBodyReference>::Failure(PersistenceError(SceneInvalid, "Physics body reference is invalid."));
            return Result<Runtime::PhysicsBodyReference>::Success(reference);
        }

        [[nodiscard]] Result<Runtime::AuthoredPhysicsMotionType> ParsePhysicsMotion(const std::string_view name) {
            if (name == "static")
                return Result<Runtime::AuthoredPhysicsMotionType>::Success(Runtime::AuthoredPhysicsMotionType::Static);
            if (name == "kinematic")
                return Result<Runtime::AuthoredPhysicsMotionType>::Success(Runtime::AuthoredPhysicsMotionType::Kinematic);
            if (name == "dynamic")
                return Result<Runtime::AuthoredPhysicsMotionType>::Success(Runtime::AuthoredPhysicsMotionType::Dynamic);
            return Result<Runtime::AuthoredPhysicsMotionType>::Failure(PersistenceError(SceneInvalid, "Rigid body motion is invalid."));
        }

        [[nodiscard]] Result<Runtime::AuthoredPhysicsMassPolicy> ParsePhysicsMass(const Json &value) {
            const std::string kind = value.value("kind", "");
            if (kind == "none")
                return Result<Runtime::AuthoredPhysicsMassPolicy>::Success(Runtime::AuthoredPhysicsNoMass{});
            if (kind == "mass" && value.contains("kilograms") && value["kilograms"].is_number())
                return Result<Runtime::AuthoredPhysicsMassPolicy>::Success(Runtime::AuthoredPhysicsMass{value["kilograms"].get<float>()});
            if (kind == "density" && value.contains("kilogramsPerCubicMeter") && value["kilogramsPerCubicMeter"].is_number())
                return Result<Runtime::AuthoredPhysicsMassPolicy>::Success(
                    Runtime::AuthoredPhysicsDensity{value["kilogramsPerCubicMeter"].get<float>()});
            return Result<Runtime::AuthoredPhysicsMassPolicy>::Failure(
                PersistenceError(SceneInvalid, "Rigid body mass policy is invalid."));
        }

        [[nodiscard]] Result<Runtime::RigidBodyComponent> ParseRigidBody(const Json &value) {
            if (!HasFields(value, {"motion", "mass"}) || !HasUnsignedFields(value, {"id", "body", "schemaVersion", "generation"}) ||
                !value["motion"].is_string() || !value["mass"].is_object())
                return Result<Runtime::RigidBodyComponent>::Failure(PersistenceError(SceneInvalid, "Rigid body schema is incomplete."));
            auto motion = ParsePhysicsMotion(value["motion"].get<std::string>());
            auto mass = ParsePhysicsMass(value["mass"]);
            auto linearVelocity = ParseVec3(value.value("initialLinearVelocity", Json::array({0.0F, 0.0F, 0.0F})));
            auto angularVelocity = ParseVec3(value.value("initialAngularVelocity", Json::array({0.0F, 0.0F, 0.0F})));
            if (!AllSucceeded({motion.HasValue(), mass.HasValue(), linearVelocity.HasValue(), angularVelocity.HasValue()}))
                return Result<Runtime::RigidBodyComponent>::Failure(PersistenceError(SceneInvalid, "Rigid body payload is invalid."));
            Runtime::RigidBodyComponent body{.id = {value["id"].get<std::uint64_t>()},
                                             .body = {value["body"].get<std::uint64_t>()},
                                             .schemaVersion = value["schemaVersion"].get<std::uint32_t>(),
                                             .generation = value["generation"].get<std::uint64_t>(),
                                             .motion = motion.Value(),
                                             .mass = std::move(mass).Value(),
                                             .initialLinearVelocity = linearVelocity.Value(),
                                             .initialAngularVelocity = angularVelocity.Value(),
                                             .linearDampingPerSecond = value.value("linearDampingPerSecond", 0.0F),
                                             .angularDampingPerSecond = value.value("angularDampingPerSecond", 0.0F),
                                             .maximumLinearSpeed = value.value("maximumLinearSpeed", 500.0F),
                                             .maximumAngularSpeed = value.value("maximumAngularSpeed", 100.0F),
                                             .enabled = value.value("enabled", true)};
            if (Runtime::ValidateRigidBodyComponent(body).HasError())
                return Result<Runtime::RigidBodyComponent>::Failure(PersistenceError(SceneInvalid, "Rigid body payload is invalid."));
            return Result<Runtime::RigidBodyComponent>::Success(std::move(body));
        }

        [[nodiscard]] Result<Runtime::PhysicsColliderSource> ParsePhysicsShapeAssetSource(const Json &value) {
            if (!HasFields(value, {"asset"}) || !HasUnsignedFields(value, {"subresource"}) || !value["asset"].is_string())
                return Result<Runtime::PhysicsColliderSource>::Failure(PersistenceError(SceneInvalid, "Collider asset is incomplete."));
            auto asset = Assets::AssetId::Parse(value["asset"].get<std::string>());
            if (asset.HasError())
                return Result<Runtime::PhysicsColliderSource>::Failure(PersistenceError(SceneInvalid, "Collider asset is invalid."));
            return Result<Runtime::PhysicsColliderSource>::Success(
                Runtime::PhysicsShapeAssetReference{asset.Value(), {value["subresource"].get<std::uint64_t>()}});
        }

        [[nodiscard]] Result<Runtime::PhysicsColliderSource> ParsePhysicsBoxSource(const Json &value) {
            auto halfExtents = ParseVec3(value.value("halfExtentsMeters", Json{}));
            if (halfExtents.HasError())
                return Result<Runtime::PhysicsColliderSource>::Failure(halfExtents.ErrorValue());
            return Result<Runtime::PhysicsColliderSource>::Success(
                Runtime::PhysicsAnalyticCollider{Runtime::PhysicsBoxCollider{halfExtents.Value()}});
        }

        [[nodiscard]] Result<Runtime::PhysicsColliderSource> ParsePhysicsPlaneSource(const Json &value) {
            if (!HasFields(value, {"normal", "signedDistanceMeters"}))
                return Result<Runtime::PhysicsColliderSource>::Failure(PersistenceError(SceneInvalid, "Static plane is incomplete."));
            auto normal = ParseVec3(value["normal"]);
            if (normal.HasError())
                return Result<Runtime::PhysicsColliderSource>::Failure(normal.ErrorValue());
            return Result<Runtime::PhysicsColliderSource>::Success(Runtime::PhysicsAnalyticCollider{
                Runtime::PhysicsStaticPlaneCollider{normal.Value(), value["signedDistanceMeters"].get<float>()}});
        }

        [[nodiscard]] Result<Runtime::PhysicsColliderSource> ParsePhysicsSphereSource(const Json &value) {
            if (!HasFields(value, {"radiusMeters"}))
                return Result<Runtime::PhysicsColliderSource>::Failure(PersistenceError(SceneInvalid, "Sphere collider is incomplete."));
            return Result<Runtime::PhysicsColliderSource>::Success(
                Runtime::PhysicsAnalyticCollider{Runtime::PhysicsSphereCollider{value["radiusMeters"].get<float>()}});
        }

        [[nodiscard]] Result<Runtime::PhysicsColliderSource> ParsePhysicsCapsuleSource(const Json &value) {
            if (!HasFields(value, {"radiusMeters", "cylindricalHalfHeightMeters"}))
                return Result<Runtime::PhysicsColliderSource>::Failure(PersistenceError(SceneInvalid, "Capsule collider is incomplete."));
            return Result<Runtime::PhysicsColliderSource>::Success(Runtime::PhysicsAnalyticCollider{
                Runtime::PhysicsCapsuleCollider{value["radiusMeters"].get<float>(), value["cylindricalHalfHeightMeters"].get<float>()}});
        }

        [[nodiscard]] Result<Runtime::PhysicsColliderSource> ParsePhysicsColliderSource(const Json &value) {
            if (!value.is_object() || !value.contains("kind") || !value["kind"].is_string())
                return Result<Runtime::PhysicsColliderSource>::Failure(PersistenceError(SceneInvalid, "Collider source is incomplete."));
            const std::string kind = value["kind"].get<std::string>();
            if (kind == "asset")
                return ParsePhysicsShapeAssetSource(value);
            if (kind == "box")
                return ParsePhysicsBoxSource(value);
            if (kind == "sphere")
                return ParsePhysicsSphereSource(value);
            if (kind == "capsule")
                return ParsePhysicsCapsuleSource(value);
            if (kind == "static_plane")
                return ParsePhysicsPlaneSource(value);
            return Result<Runtime::PhysicsColliderSource>::Failure(PersistenceError(SceneInvalid, "Collider source kind is invalid."));
        }

        [[nodiscard]] Result<std::vector<Runtime::PhysicsColliderMaterialBinding>> ParsePhysicsMaterials(const Json &value) {
            std::vector<Runtime::PhysicsColliderMaterialBinding> materials;
            materials.reserve(value.size());
            for (const Json &entry : value) {
                if (!HasFields(entry, {"material"}) || !HasUnsignedFields(entry, {"slot"}) || !entry["material"].is_string())
                    return Result<std::vector<Runtime::PhysicsColliderMaterialBinding>>::Failure(
                        PersistenceError(SceneInvalid, "Collider material is incomplete."));
                auto material = Assets::AssetId::Parse(entry["material"].get<std::string>());
                if (material.HasError())
                    return Result<std::vector<Runtime::PhysicsColliderMaterialBinding>>::Failure(
                        PersistenceError(SceneInvalid, "Collider material is invalid."));
                materials.push_back({Physics::PhysicsMaterialSlotId::FromValue(entry["slot"].get<std::uint64_t>()), material.Value()});
            }
            return Result<std::vector<Runtime::PhysicsColliderMaterialBinding>>::Success(std::move(materials));
        }

        [[nodiscard]] Result<Runtime::ColliderComponent> ParseCollider(const Json &value) {
            if (!HasFields(value, {"body", "source", "localPose", "scale", "collisionProfile", "materials"}) ||
                !HasUnsignedFields(value, {"id", "collider", "schemaVersion", "generation"}) || !value["collisionProfile"].is_string() ||
                !value["materials"].is_array())
                return Result<Runtime::ColliderComponent>::Failure(PersistenceError(SceneInvalid, "Collider schema is incomplete."));
            auto body = ParsePhysicsBodyReference(value["body"]);
            auto source = ParsePhysicsColliderSource(value["source"]);
            auto pose = ParsePhysicsPose(value["localPose"]);
            auto scale = ParseVec3(value["scale"]);
            auto profile = Physics::CollisionProfileId::Parse(value["collisionProfile"].get<std::string>());
            if (!AllSucceeded({body.HasValue(), source.HasValue(), pose.HasValue(), scale.HasValue(), profile.HasValue()}))
                return Result<Runtime::ColliderComponent>::Failure(PersistenceError(SceneInvalid, "Collider payload is invalid."));
            auto materials = ParsePhysicsMaterials(value["materials"]);
            if (materials.HasError())
                return Result<Runtime::ColliderComponent>::Failure(materials.ErrorValue());
            Runtime::ColliderComponent collider{.id = {value["id"].get<std::uint64_t>()},
                                                .collider = {value["collider"].get<std::uint64_t>()},
                                                .schemaVersion = value["schemaVersion"].get<std::uint32_t>(),
                                                .generation = value["generation"].get<std::uint64_t>(),
                                                .body = body.Value(),
                                                .source = std::move(source).Value(),
                                                .localPose = pose.Value(),
                                                .scale = scale.Value(),
                                                .collisionProfile = profile.Value(),
                                                .materials = std::move(materials).Value(),
                                                .sensor = value.value("sensor", false),
                                                .enabled = value.value("enabled", true)};
            if (Runtime::ValidateColliderComponent(collider).HasError())
                return Result<Runtime::ColliderComponent>::Failure(PersistenceError(SceneInvalid, "Collider payload is invalid."));
            return Result<Runtime::ColliderComponent>::Success(std::move(collider));
        }

        [[nodiscard]] Result<std::vector<Runtime::ColliderComponent>> ParseColliders(const Json &value) {
            if (!value.is_array() || value.size() > Runtime::MaximumPhysicsCollidersPerBody)
                return Result<std::vector<Runtime::ColliderComponent>>::Failure(
                    PersistenceError(SceneInvalid, "Collider list is invalid."));
            std::vector<Runtime::ColliderComponent> result;
            result.reserve(value.size());
            for (const Json &entry : value) {
                auto collider = ParseCollider(entry);
                if (collider.HasError())
                    return Result<std::vector<Runtime::ColliderComponent>>::Failure(collider.ErrorValue());
                result.push_back(std::move(collider).Value());
            }
            return Result<std::vector<Runtime::ColliderComponent>>::Success(std::move(result));
        }

        [[nodiscard]] Result<Runtime::PhysicsConstraintBodyEndpoint> ParseConstraintBodyEndpoint(const Json &value) {
            if (!value.is_object() || !value.contains("body") || !value.contains("frame"))
                return Result<Runtime::PhysicsConstraintBodyEndpoint>::Failure(
                    PersistenceError(SceneInvalid, "Constraint endpoint is incomplete."));
            auto body = ParsePhysicsBodyReference(value["body"]);
            auto frame = ParsePhysicsPose(value["frame"]);
            if (body.HasError() || frame.HasError())
                return Result<Runtime::PhysicsConstraintBodyEndpoint>::Failure(
                    PersistenceError(SceneInvalid, "Constraint endpoint is invalid."));
            return Result<Runtime::PhysicsConstraintBodyEndpoint>::Success({body.Value(), frame.Value()});
        }

        [[nodiscard]] Result<Runtime::PhysicsConstraintSecondEndpoint> ParseConstraintSecondEndpoint(const Json &value) {
            const std::string kind = value.value("kind", "");
            if (kind == "body") {
                auto endpoint = ParseConstraintBodyEndpoint(value);
                if (endpoint.HasError())
                    return Result<Runtime::PhysicsConstraintSecondEndpoint>::Failure(endpoint.ErrorValue());
                return Result<Runtime::PhysicsConstraintSecondEndpoint>::Success(endpoint.Value());
            }
            if (kind == "world" && value.contains("frame")) {
                auto frame = ParsePhysicsPose(value["frame"]);
                if (frame.HasError())
                    return Result<Runtime::PhysicsConstraintSecondEndpoint>::Failure(frame.ErrorValue());
                return Result<Runtime::PhysicsConstraintSecondEndpoint>::Success(Runtime::PhysicsConstraintWorldEndpoint{frame.Value()});
            }
            return Result<Runtime::PhysicsConstraintSecondEndpoint>::Failure(
                PersistenceError(SceneInvalid, "Constraint second endpoint is invalid."));
        }

        [[nodiscard]] Result<std::variant<Runtime::PhysicsFixedConstraint, Runtime::PhysicsDistanceConstraint>> ParseConstraintParameters(
            const Json &value) {
            const std::string kind = value.value("kind", "");
            if (kind == "fixed")
                return Result<std::variant<Runtime::PhysicsFixedConstraint, Runtime::PhysicsDistanceConstraint>>::Success(
                    Runtime::PhysicsFixedConstraint{});
            if (kind == "distance" && HasFields(value, {"minimumMeters", "maximumMeters"}))
                return Result<std::variant<Runtime::PhysicsFixedConstraint, Runtime::PhysicsDistanceConstraint>>::Success(
                    Runtime::PhysicsDistanceConstraint{value["minimumMeters"].get<float>(), value["maximumMeters"].get<float>()});
            return Result<std::variant<Runtime::PhysicsFixedConstraint, Runtime::PhysicsDistanceConstraint>>::Failure(
                PersistenceError(SceneInvalid, "Constraint parameters are invalid."));
        }

        [[nodiscard]] Result<Runtime::PhysicsConstraintComponent> ParsePhysicsConstraint(const Json &value) {
            if (!HasFields(value, {"first", "second", "parameters"}) ||
                !HasUnsignedFields(value, {"id", "constraint", "schemaVersion", "generation"}) || !value["second"].is_object() ||
                !value["parameters"].is_object())
                return Result<Runtime::PhysicsConstraintComponent>::Failure(
                    PersistenceError(SceneInvalid, "Physics constraint schema is incomplete."));
            auto first = ParseConstraintBodyEndpoint(value["first"]);
            auto second = ParseConstraintSecondEndpoint(value["second"]);
            auto parameters = ParseConstraintParameters(value["parameters"]);
            if (!AllSucceeded({first.HasValue(), second.HasValue(), parameters.HasValue()}))
                return Result<Runtime::PhysicsConstraintComponent>::Failure(
                    PersistenceError(SceneInvalid, "Physics constraint payload is invalid."));
            Runtime::PhysicsConstraintComponent constraint{.id = {value["id"].get<std::uint64_t>()},
                                                           .constraint = {value["constraint"].get<std::uint64_t>()},
                                                           .schemaVersion = value["schemaVersion"].get<std::uint32_t>(),
                                                           .generation = value["generation"].get<std::uint64_t>(),
                                                           .first = first.Value(),
                                                           .second = std::move(second).Value(),
                                                           .parameters = std::move(parameters).Value(),
                                                           .enabled = value.value("enabled", true)};
            if (Runtime::ValidatePhysicsConstraintComponent(constraint).HasError())
                return Result<Runtime::PhysicsConstraintComponent>::Failure(
                    PersistenceError(SceneInvalid, "Physics constraint payload is invalid."));
            return Result<Runtime::PhysicsConstraintComponent>::Success(std::move(constraint));
        }

        [[nodiscard]] Result<std::vector<Runtime::PhysicsConstraintComponent>> ParsePhysicsConstraints(const Json &value) {
            if (!value.is_array() || value.size() > Runtime::MaximumPhysicsConstraintsPerObject)
                return Result<std::vector<Runtime::PhysicsConstraintComponent>>::Failure(
                    PersistenceError(SceneInvalid, "Physics constraint list is invalid."));
            std::vector<Runtime::PhysicsConstraintComponent> result;
            result.reserve(value.size());
            for (const Json &entry : value) {
                auto constraint = ParsePhysicsConstraint(entry);
                if (constraint.HasError())
                    return Result<std::vector<Runtime::PhysicsConstraintComponent>>::Failure(constraint.ErrorValue());
                result.push_back(std::move(constraint).Value());
            }
            return Result<std::vector<Runtime::PhysicsConstraintComponent>>::Success(std::move(result));
        }

        [[nodiscard]] Result<Gameplay::BehaviorComponent> ParseSingleBehavior(const Json &behavior) {
            if (!behavior.is_object() || !behavior.contains("instanceId") || !behavior["instanceId"].is_number_unsigned() ||
                !behavior.contains("typeId") || !behavior["typeId"].is_string() || !behavior.contains("schemaVersion") ||
                !behavior["schemaVersion"].is_number_unsigned() || !behavior.contains("enabled") || !behavior["enabled"].is_boolean() ||
                !behavior.contains("fields") || !behavior["fields"].is_array()) {
                return Result<Gameplay::BehaviorComponent>::Failure(PersistenceError(SceneInvalid, "Behavior entry is invalid."));
            }
            auto typeId = Gameplay::BehaviorTypeId::Parse(behavior["typeId"].get<std::string>());
            if (typeId.HasError()) {
                return Result<Gameplay::BehaviorComponent>::Failure(PersistenceError(SceneInvalid, "Behavior type ID is invalid."));
            }
            Gameplay::BehaviorComponent parsed{
                .instanceId = Gameplay::BehaviorInstanceId{behavior["instanceId"].get<std::uint64_t>()},
                .typeId = std::move(typeId).Value(),
                .schemaVersion = behavior["schemaVersion"].get<std::uint32_t>(),
                .enabled = behavior["enabled"].get<bool>(),
            };
            parsed.fields.reserve(behavior["fields"].size());
            for (const Json &fieldEntry : behavior["fields"]) {
                if (!fieldEntry.is_object() || !fieldEntry.contains("name") || !fieldEntry["name"].is_string() ||
                    !fieldEntry.contains("value")) {
                    return Result<Gameplay::BehaviorComponent>::Failure(PersistenceError(SceneInvalid, "Behavior field entry is invalid."));
                }
                auto field = ParseBehaviorFieldValue(fieldEntry["value"]);
                if (field.HasError()) {
                    return Result<Gameplay::BehaviorComponent>::Failure(field.ErrorValue());
                }
                parsed.fields.emplace_back(fieldEntry["name"].get<std::string>(), std::move(field).Value());
            }
            if (Gameplay::ValidateBehaviorComponent(parsed).HasError()) {
                return Result<Gameplay::BehaviorComponent>::Failure(PersistenceError(SceneInvalid, "Behavior payload is invalid."));
            }
            return Result<Gameplay::BehaviorComponent>::Success(std::move(parsed));
        }

        [[nodiscard]] Result<std::vector<Gameplay::BehaviorComponent>> ParseBehaviors(const Json &behaviors) {
            if (!behaviors.is_array() || behaviors.size() > 128) {
                return Result<std::vector<Gameplay::BehaviorComponent>>::Failure(
                    PersistenceError(SceneInvalid, "Behavior list is invalid."));
            }
            std::vector<Gameplay::BehaviorComponent> parsedList;
            parsedList.reserve(behaviors.size());
            for (const Json &behavior : behaviors) {
                auto parsed = ParseSingleBehavior(behavior);
                if (parsed.HasError()) {
                    return Result<std::vector<Gameplay::BehaviorComponent>>::Failure(parsed.ErrorValue());
                }
                parsedList.push_back(std::move(parsed).Value());
            }
            return Result<std::vector<Gameplay::BehaviorComponent>>::Success(std::move(parsedList));
        }

        /** @brief Parses an optional named component into its destination when present. */
        template <typename Component, typename Parser>
        [[nodiscard]] Result<void> ParseOptionalComponent(const Json &value, const std::string_view name,
                                                          std::optional<Component> &destination, Parser &&parser) {
            if (!value.contains(name))
                return Result<void>::Success();
            auto parsed = std::invoke(std::forward<Parser>(parser), value[name]);
            if (parsed.HasError())
                return Result<void>::Failure(parsed.ErrorValue());
            destination = std::move(parsed).Value();
            return Result<void>::Success();
        }

        [[nodiscard]] Result<SceneObjectComponentSet> ParseComponents(const Json &value) {
            if (!value.is_object()) {
                return Result<SceneObjectComponentSet>::Failure(PersistenceError(SceneInvalid, "Components must be an object."));
            }
            SceneObjectComponentSet components;
            const auto parse = [&]<typename Component, typename Parser>(const std::string_view name, std::optional<Component> &destination,
                                                                        Parser &&parser) -> Result<void> {
                return ParseOptionalComponent(value, name, destination, std::forward<Parser>(parser));
            };
            if (auto parsed = parse("camera", components.camera, ParseCameraComponent); parsed.HasError())
                return Result<SceneObjectComponentSet>::Failure(parsed.ErrorValue());
            if (auto parsed = parse("light", components.light, ParseLightComponent); parsed.HasError())
                return Result<SceneObjectComponentSet>::Failure(parsed.ErrorValue());
            if (auto parsed = parse("triggerVolume", components.triggerVolume, ParseTriggerVolumeComponent); parsed.HasError())
                return Result<SceneObjectComponentSet>::Failure(parsed.ErrorValue());
            if (auto parsed = parse("audioSource", components.audioSource, ParseAudioSourceComponent); parsed.HasError())
                return Result<SceneObjectComponentSet>::Failure(parsed.ErrorValue());
            if (auto parsed = parse("navigationSurface", components.navigationSurface, ParseNavigationSurface); parsed.HasError())
                return Result<SceneObjectComponentSet>::Failure(parsed.ErrorValue());
            if (auto parsed = parse("navigationRegion", components.navigationRegion, ParseNavigationRegion); parsed.HasError())
                return Result<SceneObjectComponentSet>::Failure(parsed.ErrorValue());
            if (auto parsed = parse("navigationModifier", components.navigationModifier, ParseNavigationModifier); parsed.HasError())
                return Result<SceneObjectComponentSet>::Failure(parsed.ErrorValue());
            if (auto parsed = parse("navigationLink", components.navigationLink, ParseNavigationLink); parsed.HasError())
                return Result<SceneObjectComponentSet>::Failure(parsed.ErrorValue());
            if (auto parsed = parse("rigidBody", components.rigidBody, ParseRigidBody); parsed.HasError())
                return Result<SceneObjectComponentSet>::Failure(parsed.ErrorValue());
            if (value.contains("colliders")) {
                auto colliders = ParseColliders(value["colliders"]);
                if (colliders.HasError())
                    return Result<SceneObjectComponentSet>::Failure(colliders.ErrorValue());
                components.colliders = std::move(colliders).Value();
            }
            if (value.contains("physicsConstraints")) {
                auto constraints = ParsePhysicsConstraints(value["physicsConstraints"]);
                if (constraints.HasError())
                    return Result<SceneObjectComponentSet>::Failure(constraints.ErrorValue());
                components.physicsConstraints = std::move(constraints).Value();
            }
            if (value.contains("behaviors")) {
                auto behaviors = ParseBehaviors(value["behaviors"]);
                if (behaviors.HasError()) {
                    return Result<SceneObjectComponentSet>::Failure(behaviors.ErrorValue());
                }
                components.behaviors = std::move(behaviors).Value();
            }
            return Result<SceneObjectComponentSet>::Success(std::move(components));
        }

        [[nodiscard]] Json SceneJson(const SceneDocumentSnapshot &snapshot) {
            Json objects = Json::array();
            for (const SceneObjectSnapshot &object : snapshot.objects) {
                Json value{
                    {"id", object.id.value},
                    {"parent", object.parent.has_value() ? Json(object.parent->value) : Json(nullptr)},
                    {"name", object.name},
                    {"transform", TransformJson(object.localTransform)},
                    {"components", ComponentsJson(object.components)},
                    {"editor", {{"visible", object.editorState.visible}, {"locked", object.editorState.locked}}},
                };
                value["primitiveMesh"] = object.primitiveMesh.has_value() ? PrimitiveJson(*object.primitiveMesh) : Json(nullptr);
                value["meshAsset"] = object.meshAsset.has_value() ? Json(object.meshAsset->ToString()) : Json(nullptr);
                objects.push_back(std::move(value));
            }
            Json prefabInstances = Json::array();
            for (const ScenePrefabInstance &instance : snapshot.prefabInstances) {
                prefabInstances.push_back({
                    {"instanceId", instance.instanceId.Value()},
                    {"sourceAsset", instance.sourcePrefab.Asset().ToString()},
                    {"parent", instance.parent.has_value() ? Json(instance.parent->value) : Json(nullptr)},
                    {"rootTransform", TransformJson(instance.rootTransform)},
                });
            }
            return Json{{"schemaVersion", kSceneSchemaVersion},
                        {"objects", std::move(objects)},
                        {"prefabInstances", std::move(prefabInstances)}};
        }

        [[nodiscard]] Result<SceneObjectSnapshot> ParseSceneObjectSnapshot(const Json &value) {
            if (!value.is_object() || !value.contains("id") || !value["id"].is_number_unsigned() || !value.contains("name") ||
                !value["name"].is_string() || !value.contains("parent") || !value.contains("transform") ||
                !value.contains("primitiveMesh") || !value.contains("components")) {
                return Result<SceneObjectSnapshot>::Failure(PersistenceError(SceneInvalid, "Scene object schema is incomplete."));
            }
            auto transform = ParseTransform(value["transform"]);
            auto components = ParseComponents(value["components"]);
            if (transform.HasError() || components.HasError()) {
                return Result<SceneObjectSnapshot>::Failure(PersistenceError(SceneInvalid, "Scene object values are invalid."));
            }

            std::optional<SceneObjectId> parent;
            if (!value["parent"].is_null()) {
                if (!value["parent"].is_number_unsigned()) {
                    return Result<SceneObjectSnapshot>::Failure(PersistenceError(SceneInvalid, "Scene object parent is invalid."));
                }
                parent = SceneObjectId{value["parent"].get<std::uint64_t>()};
            }
            std::optional<PrimitiveMeshDescriptor> primitive;
            if (!value["primitiveMesh"].is_null()) {
                auto parsed = ParsePrimitive(value["primitiveMesh"]);
                if (parsed.HasError()) {
                    return Result<SceneObjectSnapshot>::Failure(parsed.ErrorValue());
                }
                primitive = std::move(parsed).Value();
            }
            std::optional<Assets::AssetId> meshAsset;
            if (value.contains("meshAsset") && !value["meshAsset"].is_null()) {
                if (!value["meshAsset"].is_string()) {
                    return Result<SceneObjectSnapshot>::Failure(
                        PersistenceError(SceneInvalid, "Scene object mesh asset identity is invalid."));
                }
                auto parsed = Assets::AssetId::Parse(value["meshAsset"].get<std::string>());
                if (parsed.HasError()) {
                    return Result<SceneObjectSnapshot>::Failure(
                        PersistenceError(SceneInvalid, "Scene object mesh asset identity is invalid."));
                }
                meshAsset = std::move(parsed).Value();
            }
            SceneObjectEditorState editorState;
            if (value.contains("editor")) {
                const Json &editor = value["editor"];
                if (!editor.is_object() || !editor.contains("visible") || !editor["visible"].is_boolean() || !editor.contains("locked") ||
                    !editor["locked"].is_boolean()) {
                    return Result<SceneObjectSnapshot>::Failure(PersistenceError(SceneInvalid, "Scene object editor state is invalid."));
                }
                editorState = SceneObjectEditorState{
                    .visible = editor["visible"].get<bool>(),
                    .locked = editor["locked"].get<bool>(),
                };
            }
            return Result<SceneObjectSnapshot>::Success(SceneObjectSnapshot{
                .id = SceneObjectId{value["id"].get<std::uint64_t>()},
                .parent = parent,
                .name = value["name"].get<std::string>(),
                .localTransform = transform.Value(),
                .primitiveMesh = std::move(primitive),
                .components = components.Value(),
                .meshAsset = std::move(meshAsset),
                .editorState = editorState,
            });
        }

        struct ParsedScene final {
            std::vector<SceneObjectSnapshot> objects;
            std::vector<ScenePrefabInstance> prefabInstances;
        };

        [[nodiscard]] Result<ScenePrefabInstance> ParseScenePrefabInstance(const Json &value) {
            if (!value.is_object() || !value.contains("instanceId") || !value["instanceId"].is_number_unsigned() ||
                !value.contains("sourceAsset") || !value["sourceAsset"].is_string() || !value.contains("parent") ||
                !value.contains("rootTransform")) {
                return Result<ScenePrefabInstance>::Failure(PersistenceError(SceneInvalid, "Scene prefab instance schema is incomplete."));
            }
            auto instanceId = Prefab::PrefabInstanceId::Create(value["instanceId"].get<std::uint64_t>());
            auto assetId = Assets::AssetId::Parse(value["sourceAsset"].get<std::string>());
            auto rootTransform = ParseTransform(value["rootTransform"]);
            if (instanceId.HasError() || assetId.HasError() || rootTransform.HasError()) {
                return Result<ScenePrefabInstance>::Failure(
                    PersistenceError(SceneInvalid, "Scene prefab instance identity, source, or root transform is invalid."));
            }
            auto sourcePrefab = Prefab::PrefabAssetReference::Create(assetId.Value());
            if (sourcePrefab.HasError()) {
                return Result<ScenePrefabInstance>::Failure(
                    PersistenceError(SceneInvalid, "Scene prefab source asset identity is invalid."));
            }
            std::optional<SceneObjectId> parent;
            if (!value["parent"].is_null()) {
                if (!value["parent"].is_number_unsigned()) {
                    return Result<ScenePrefabInstance>::Failure(PersistenceError(SceneInvalid, "Scene prefab instance parent is invalid."));
                }
                parent = SceneObjectId{value["parent"].get<std::uint64_t>()};
            }
            return Result<ScenePrefabInstance>::Success(
                ScenePrefabInstance{instanceId.Value(), sourcePrefab.Value(), parent, rootTransform.Value()});
        }

        [[nodiscard]] Result<ParsedScene> ParseScene(const std::string &contents) {
            try {
                const Json document = Json::parse(contents);
                if (!document.is_object() || !document.contains("schemaVersion") || document["schemaVersion"] != kSceneSchemaVersion ||
                    !document.contains("objects") || !document["objects"].is_array() || document["objects"].size() > kMaximumSceneObjects ||
                    (document.contains("prefabInstances") &&
                     (!document["prefabInstances"].is_array() || document["prefabInstances"].size() > kMaximumSceneObjects))) {
                    return Result<ParsedScene>::Failure(PersistenceError(SceneInvalid, "Scene schema is unsupported or incomplete."));
                }

                ParsedScene scene;
                scene.objects.reserve(document["objects"].size());
                for (const Json &value : document["objects"]) {
                    auto object = ParseSceneObjectSnapshot(value);
                    if (object.HasError()) {
                        return Result<ParsedScene>::Failure(object.ErrorValue());
                    }
                    scene.objects.push_back(std::move(object).Value());
                }
                if (document.contains("prefabInstances")) {
                    scene.prefabInstances.reserve(document["prefabInstances"].size());
                    for (const Json &value : document["prefabInstances"]) {
                        auto instance = ParseScenePrefabInstance(value);
                        if (instance.HasError())
                            return Result<ParsedScene>::Failure(instance.ErrorValue());
                        scene.prefabInstances.push_back(std::move(instance).Value());
                    }
                }
                return Result<ParsedScene>::Success(std::move(scene));
            } catch (const Json::exception &exception) {
                return Result<ParsedScene>::Failure(PersistenceError(SceneInvalid, "Invalid scene JSON: " + std::string{exception.what()}));
            }
        }

        [[nodiscard]] std::vector<std::byte> Bytes(const std::string_view value) {
            const auto *begin = reinterpret_cast<const std::byte *>(value.data());
            return {begin, begin + value.size()};
        }

        [[nodiscard]] std::filesystem::path RecoveryPath(const std::filesystem::path &absoluteProjectRoot) {
            return absoluteProjectRoot / ".horo/local/recovery/default-scene.hororecovery";
        }

        [[nodiscard]] std::string SceneChecksum(const Json &scene) {
            const std::string canonical = scene.dump();
            return FormatSha256(
                ComputeSha256(std::span<const std::byte>{reinterpret_cast<const std::byte *>(canonical.data()), canonical.size()}));
        }

        [[nodiscard]] SceneFileFingerprint Fingerprint(const std::string_view bytes) {
            return SceneFileFingerprint{
                .exists = true,
                .byteSize = bytes.size(),
                .checksum = FormatSha256(
                    ComputeSha256(std::span<const std::byte>{reinterpret_cast<const std::byte *>(bytes.data()), bytes.size()})),
            };
        }
    }  // namespace

    /** @copydoc LoadProjectDefaultScene */
    Result<std::optional<LoadedProjectScene>> LoadProjectDefaultScene(const std::filesystem::path &absoluteProjectRoot) {
        const std::filesystem::path metadataPath = absoluteProjectRoot / ".horo/project.json";
        if (std::error_code error; !std::filesystem::exists(metadataPath, error)) {
            if (error) {
                return Result<std::optional<LoadedProjectScene>>::Failure(
                    PersistenceError(SceneReadFailed, "Unable to inspect '" + metadataPath.string() + "'."));
            }
            return Result<std::optional<LoadedProjectScene>>::Success(std::nullopt);
        }

        auto metadataBytes = ReadBoundedFile(metadataPath, kMaximumProjectMetadataBytes);
        if (metadataBytes.HasError()) {
            return Result<std::optional<LoadedProjectScene>>::Failure(metadataBytes.ErrorValue());
        }

        try {
            const Json metadata = Json::parse(metadataBytes.Value());
            if (!metadata.is_object() || !metadata.contains("settings") || !metadata["settings"].is_object() ||
                !metadata["settings"].contains("defaultScene") || !metadata["settings"]["defaultScene"].is_string()) {
                return Result<std::optional<LoadedProjectScene>>::Failure(
                    PersistenceError(ScenePathInvalid, "Project metadata does not contain settings.defaultScene."));
            }
            const std::string configuredScene = metadata["settings"]["defaultScene"].get<std::string>();
            if (configuredScene.empty()) {
                return Result<std::optional<LoadedProjectScene>>::Success(std::nullopt);
            }
            const std::filesystem::path relativeScene = std::filesystem::path{configuredScene}.lexically_normal();
            if (!IsSafeProjectRelativePath(relativeScene)) {
                return Result<std::optional<LoadedProjectScene>>::Failure(
                    PersistenceError(ScenePathInvalid, "Project defaultScene must be a safe project-relative path."));
            }
            const std::filesystem::path absoluteScene = (absoluteProjectRoot / relativeScene).lexically_normal();
            if (!absoluteScene.is_absolute() || !IsResolvedContainedBy(absoluteProjectRoot, absoluteScene)) {
                return Result<std::optional<LoadedProjectScene>>::Failure(
                    PersistenceError(ScenePathInvalid, "Resolved defaultScene is outside the project root."));
            }

            auto loaded = LoadProjectScene(absoluteProjectRoot, absoluteScene);
            if (loaded.HasError()) {
                return Result<std::optional<LoadedProjectScene>>::Failure(loaded.ErrorValue());
            }
            if (!loaded.Value().existed) {
                return Result<std::optional<LoadedProjectScene>>::Failure(
                    PersistenceError(SceneReadFailed, "Configured default scene does not exist at '" + absoluteScene.string() + "'."));
            }
            return Result<std::optional<LoadedProjectScene>>::Success(std::move(loaded).Value());
        } catch (const Json::exception &exception) {
            return Result<std::optional<LoadedProjectScene>>::Failure(
                PersistenceError(SceneInvalid, "Invalid project metadata JSON: " + std::string{exception.what()}));
        }
    }

    /** @copydoc LoadProjectScene */
    Result<LoadedProjectScene> LoadProjectScene(const std::filesystem::path &absoluteProjectRoot,
                                                const std::filesystem::path &absoluteScenePath) {
        if (!absoluteProjectRoot.is_absolute() || !absoluteScenePath.is_absolute() ||
            !IsResolvedContainedBy(absoluteProjectRoot, absoluteScenePath) || absoluteScenePath.extension() != ".horo") {
            return Result<LoadedProjectScene>::Failure(
                PersistenceError(ScenePathInvalid, "Scene load requires an absolute project-contained .horo path."));
        }

        if (std::error_code error; !std::filesystem::exists(absoluteScenePath, error)) {
            if (error) {
                return Result<LoadedProjectScene>::Failure(
                    PersistenceError(SceneReadFailed, "Unable to inspect '" + absoluteScenePath.string() + "'."));
            }
            return Result<LoadedProjectScene>::Success(LoadedProjectScene{absoluteScenePath, {}, {}, false, SceneFileFingerprint{}});
        }
        auto sceneBytes = ReadBoundedFile(absoluteScenePath, kMaximumSceneBytes);
        if (sceneBytes.HasError()) {
            return Result<LoadedProjectScene>::Failure(sceneBytes.ErrorValue());
        }
        auto scene = ParseScene(sceneBytes.Value());
        if (scene.HasError()) {
            return Result<LoadedProjectScene>::Failure(scene.ErrorValue());
        }
        ParsedScene parsed = std::move(scene).Value();
        return Result<LoadedProjectScene>::Success(LoadedProjectScene{absoluteScenePath, std::move(parsed.objects),
                                                                      std::move(parsed.prefabInstances), true,
                                                                      Fingerprint(sceneBytes.Value())});
    }

    /** @copydoc InspectProjectSceneFingerprint */
    Result<SceneFileFingerprint> InspectProjectSceneFingerprint(const std::filesystem::path &absoluteProjectRoot,
                                                                const std::filesystem::path &absoluteScenePath) {
        if (!absoluteProjectRoot.is_absolute() || !absoluteScenePath.is_absolute() ||
            !IsResolvedContainedBy(absoluteProjectRoot, absoluteScenePath)) {
            return Result<SceneFileFingerprint>::Failure(
                PersistenceError(ScenePathInvalid, "Scene fingerprint inspection requires an absolute project-contained path."));
        }

        if (std::error_code error; !std::filesystem::exists(absoluteScenePath, error)) {
            if (error) {
                return Result<SceneFileFingerprint>::Failure(
                    PersistenceError(SceneReadFailed, "Unable to inspect '" + absoluteScenePath.string() + "'."));
            }
            return Result<SceneFileFingerprint>::Success(SceneFileFingerprint{});
        }
        auto bytes = ReadBoundedFile(absoluteScenePath, kMaximumSceneBytes);
        if (bytes.HasError()) {
            return Result<SceneFileFingerprint>::Failure(bytes.ErrorValue());
        }
        return Result<SceneFileFingerprint>::Success(Fingerprint(bytes.Value()));
    }

    /** @copydoc SaveProjectScene */
    Result<ProjectSceneSaveResult> SaveProjectScene(const std::filesystem::path &absoluteProjectRoot,
                                                    const std::filesystem::path &absoluteScenePath, const SceneDocumentSnapshot &snapshot,
                                                    const SceneFileFingerprint &expectedFingerprint, const bool overwriteConflict,
                                                    const ProjectMutationCoordinator &mutations, DurableFileSystem &files) {
        if (!absoluteProjectRoot.is_absolute() || !absoluteScenePath.is_absolute() ||
            !IsResolvedContainedBy(absoluteProjectRoot, absoluteScenePath)) {
            return Result<ProjectSceneSaveResult>::Failure(
                PersistenceError(ScenePathInvalid, "Scene save destination must be an absolute path inside the project."));
        }

        if (auto lease = mutations.TryAcquire(ProjectMutationRequest{
                .projectRoot = absoluteProjectRoot,
                .owner = ProjectMutationOwner::Save,
                .operationId = std::format("scene-save-{}", snapshot.revision.value),
            });
            lease.HasError()) {
            return Result<ProjectSceneSaveResult>::Failure(lease.ErrorValue());
        }

        auto currentFingerprint = InspectProjectSceneFingerprint(absoluteProjectRoot, absoluteScenePath);
        if (currentFingerprint.HasError()) {
            return Result<ProjectSceneSaveResult>::Failure(currentFingerprint.ErrorValue());
        }
        if (!overwriteConflict && currentFingerprint.Value() != expectedFingerprint) {
            return Result<ProjectSceneSaveResult>::Success(ProjectSceneSaveResult{
                .status = ProjectSceneSaveStatus::Conflict,
                .fingerprint = std::move(currentFingerprint).Value(),
            });
        }

        const std::string serialized = SceneJson(snapshot).dump(2) + '\n';
        const std::vector<std::byte> bytes = Bytes(serialized);
        std::filesystem::path prepared = absoluteScenePath;
        prepared += ".save.tmp";

        if (Result<void> write = files.WriteDurable(prepared, bytes); write.HasError()) {
            static_cast<void>(files.RemoveDurable(prepared));
            return Result<ProjectSceneSaveResult>::Failure(write.ErrorValue());
        }
        currentFingerprint = InspectProjectSceneFingerprint(absoluteProjectRoot, absoluteScenePath);
        if (currentFingerprint.HasError()) {
            static_cast<void>(files.RemoveDurable(prepared));
            return Result<ProjectSceneSaveResult>::Failure(currentFingerprint.ErrorValue());
        }
        if (!overwriteConflict && currentFingerprint.Value() != expectedFingerprint) {
            static_cast<void>(files.RemoveDurable(prepared));
            return Result<ProjectSceneSaveResult>::Success(ProjectSceneSaveResult{
                .status = ProjectSceneSaveStatus::Conflict,
                .fingerprint = std::move(currentFingerprint).Value(),
            });
        }
        if (Result<void> replace = files.AtomicReplace(prepared, absoluteScenePath); replace.HasError()) {
            static_cast<void>(files.RemoveDurable(prepared));
            return Result<ProjectSceneSaveResult>::Failure(replace.ErrorValue());
        }
        return Result<ProjectSceneSaveResult>::Success(ProjectSceneSaveResult{
            .status = ProjectSceneSaveStatus::Saved,
            .fingerprint = Fingerprint(serialized),
        });
    }

    /** @copydoc SaveProjectSceneToPath */
    Result<ProjectSceneDestinationSaveResult> SaveProjectSceneToPath(const std::filesystem::path &absoluteProjectRoot,
                                                                     const std::filesystem::path &absoluteScenePath,
                                                                     const SceneDocumentSnapshot &snapshot, const bool overwriteExisting,
                                                                     const ProjectMutationCoordinator &mutations,
                                                                     DurableFileSystem &files) {
        if (!absoluteProjectRoot.is_absolute() || !absoluteScenePath.is_absolute() ||
            !IsResolvedContainedBy(absoluteProjectRoot, absoluteScenePath) || absoluteScenePath.extension() != ".horo") {
            return Result<ProjectSceneDestinationSaveResult>::Failure(
                PersistenceError(ScenePathInvalid, "Scene destination must be an absolute project-contained .horo path."));
        }

        auto initial = InspectProjectSceneFingerprint(absoluteProjectRoot, absoluteScenePath);
        if (initial.HasError()) {
            return Result<ProjectSceneDestinationSaveResult>::Failure(initial.ErrorValue());
        }
        if (initial.Value().exists && !overwriteExisting) {
            return Result<ProjectSceneDestinationSaveResult>::Success({
                .status = ProjectSceneDestinationSaveStatus::DestinationExists,
                .fingerprint = initial.Value(),
            });
        }

        auto saved = SaveProjectScene(absoluteProjectRoot, absoluteScenePath, snapshot, initial.Value(), false, mutations, files);
        if (saved.HasError()) {
            return Result<ProjectSceneDestinationSaveResult>::Failure(saved.ErrorValue());
        }
        return Result<ProjectSceneDestinationSaveResult>::Success({
            .status = saved.Value().status == ProjectSceneSaveStatus::Saved ? ProjectSceneDestinationSaveStatus::Saved
                                                                            : ProjectSceneDestinationSaveStatus::Conflict,
            .fingerprint = std::move(saved).Value().fingerprint,
        });
    }

    /** @copydoc WriteProjectSceneRecovery */
    Result<void> WriteProjectSceneRecovery(const std::filesystem::path &absoluteProjectRoot, const std::filesystem::path &absoluteScenePath,
                                           const SceneDocumentSnapshot &snapshot, const DocumentRevision savedRevision,
                                           const DocumentStateId savedState, const ProjectMutationCoordinator &mutations,
                                           DurableFileSystem &files) {
        if (!absoluteProjectRoot.is_absolute() || !absoluteScenePath.is_absolute() ||
            !IsContainedBy(absoluteProjectRoot, absoluteScenePath) || !savedState.IsValid()) {
            return Result<void>::Failure(
                PersistenceError(ScenePathInvalid, "Recovery destination must describe an absolute project scene."));
        }

        if (auto lease = mutations.TryAcquire(ProjectMutationRequest{
                .projectRoot = absoluteProjectRoot,
                .owner = ProjectMutationOwner::Autosave,
                .operationId = std::format("scene-autosave-{}", snapshot.revision.value),
            });
            lease.HasError()) {
            return Result<void>::Failure(lease.ErrorValue());
        }

        Json scene = SceneJson(snapshot);
        const Json record{
            {"recordVersion", 1},
            {"canonicalPath", absoluteScenePath.string()},
            {"savedRevision", savedRevision.value},
            {"savedState", savedState.value},
            {"recoveredRevision", snapshot.revision.value},
            {"recoveredState", snapshot.state.value},
            {"capturedAtUnixMilliseconds",
             std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()},
            {"sceneChecksum", SceneChecksum(scene)},
            {"scene", std::move(scene)},
        };
        const std::string serialized = record.dump(2) + '\n';
        if (serialized.size() > kMaximumRecoveryBytes) {
            return Result<void>::Failure(PersistenceError(SceneInvalid, "Recovery record exceeds the supported size limit."));
        }

        const std::filesystem::path destination = RecoveryPath(absoluteProjectRoot);
        std::filesystem::path prepared = destination;
        prepared += ".tmp";
        const std::vector<std::byte> bytes = Bytes(serialized);
        if (Result<void> write = files.WriteDurable(prepared, bytes); write.HasError()) {
            static_cast<void>(files.RemoveDurable(prepared));
            return write;
        }
        if (Result<void> replace = files.AtomicReplace(prepared, destination); replace.HasError()) {
            static_cast<void>(files.RemoveDurable(prepared));
            return replace;
        }
        return Result<void>::Success();
    }

    /** @copydoc InspectProjectSceneRecovery */
    Result<std::optional<ProjectSceneRecoveryRecord>> InspectProjectSceneRecovery(const std::filesystem::path &absoluteProjectRoot,
                                                                                  const std::filesystem::path &absoluteScenePath) {
        if (!absoluteProjectRoot.is_absolute() || !absoluteScenePath.is_absolute() ||
            !IsContainedBy(absoluteProjectRoot, absoluteScenePath)) {
            return Result<std::optional<ProjectSceneRecoveryRecord>>::Failure(
                PersistenceError(ScenePathInvalid, "Recovery inspection requires an absolute project scene."));
        }

        const std::filesystem::path recoveryPath = RecoveryPath(absoluteProjectRoot);
        if (std::error_code error; !std::filesystem::exists(recoveryPath, error)) {
            if (error) {
                return Result<std::optional<ProjectSceneRecoveryRecord>>::Failure(
                    PersistenceError(SceneReadFailed, "Unable to inspect recovery path '" + recoveryPath.string() + "'."));
            }
            return Result<std::optional<ProjectSceneRecoveryRecord>>::Success(std::nullopt);
        }

        auto bytes = ReadBoundedFile(recoveryPath, kMaximumRecoveryBytes);
        if (bytes.HasError()) {
            return Result<std::optional<ProjectSceneRecoveryRecord>>::Failure(bytes.ErrorValue());
        }
        try {
            const Json record = Json::parse(bytes.Value());
            if (!record.is_object() || record.value("recordVersion", 0) != 1 || !record.contains("canonicalPath") ||
                !record["canonicalPath"].is_string() || !record.contains("savedRevision") ||
                !record["savedRevision"].is_number_unsigned() || !record.contains("savedState") ||
                !record["savedState"].is_number_unsigned() || !record.contains("recoveredRevision") ||
                !record["recoveredRevision"].is_number_unsigned() || !record.contains("recoveredState") ||
                !record["recoveredState"].is_number_unsigned() || !record.contains("sceneChecksum") ||
                !record["sceneChecksum"].is_string() || !record.contains("scene") || !record["scene"].is_object()) {
                return Result<std::optional<ProjectSceneRecoveryRecord>>::Failure(
                    PersistenceError(SceneInvalid, "Recovery record schema is incomplete."));
            }
            if (std::filesystem::path{record["canonicalPath"].get<std::string>()}.lexically_normal() !=
                absoluteScenePath.lexically_normal()) {
                return Result<std::optional<ProjectSceneRecoveryRecord>>::Failure(
                    PersistenceError(SceneInvalid, "Recovery record belongs to a different canonical scene."));
            }
            if (record["sceneChecksum"].get<std::string>() != SceneChecksum(record["scene"])) {
                return Result<std::optional<ProjectSceneRecoveryRecord>>::Failure(
                    PersistenceError(SceneInvalid, "Recovery scene checksum does not match its payload."));
            }

            auto scene = ParseScene(record["scene"].dump());
            if (scene.HasError()) {
                return Result<std::optional<ProjectSceneRecoveryRecord>>::Failure(scene.ErrorValue());
            }
            ParsedScene parsed = std::move(scene).Value();
            ProjectSceneRecoveryRecord result{
                .absoluteCanonicalPath = absoluteScenePath,
                .savedRevision = DocumentRevision{record["savedRevision"].get<std::uint64_t>()},
                .savedState = DocumentStateId{record["savedState"].get<std::uint64_t>()},
                .recoveredRevision = DocumentRevision{record["recoveredRevision"].get<std::uint64_t>()},
                .recoveredState = DocumentStateId{record["recoveredState"].get<std::uint64_t>()},
                .objects = std::move(parsed.objects),
                .prefabInstances = std::move(parsed.prefabInstances),
            };
            if (!result.savedState.IsValid() || !result.recoveredState.IsValid() || result.recoveredRevision < result.savedRevision) {
                return Result<std::optional<ProjectSceneRecoveryRecord>>::Failure(
                    PersistenceError(SceneInvalid, "Recovery revision metadata is invalid."));
            }
            return Result<std::optional<ProjectSceneRecoveryRecord>>::Success(std::move(result));
        } catch (const Json::exception &exception) {
            return Result<std::optional<ProjectSceneRecoveryRecord>>::Failure(
                PersistenceError(SceneInvalid, "Invalid recovery JSON: " + std::string{exception.what()}));
        }
    }

    /** @copydoc DiscardProjectSceneRecovery */
    Result<void> DiscardProjectSceneRecovery(const std::filesystem::path &absoluteProjectRoot, const ProjectMutationCoordinator &mutations,
                                             DurableFileSystem &files) {
        if (!absoluteProjectRoot.is_absolute()) {
            return Result<void>::Failure(PersistenceError(ScenePathInvalid, "Recovery cleanup requires an absolute project root."));
        }
        const std::filesystem::path recoveryPath = RecoveryPath(absoluteProjectRoot);
        if (std::error_code error; !std::filesystem::exists(recoveryPath, error)) {
            if (error) {
                return Result<void>::Failure(PersistenceError(SceneReadFailed, "Unable to inspect recovery state."));
            }
            return Result<void>::Success();
        }
        if (auto lease = mutations.TryAcquire(ProjectMutationRequest{
                .projectRoot = absoluteProjectRoot,
                .owner = ProjectMutationOwner::Autosave,
                .operationId = "scene-recovery-discard",
            });
            lease.HasError()) {
            return Result<void>::Failure(lease.ErrorValue());
        }
        return files.RemoveDurable(recoveryPath);
    }
}  // namespace Horo::Editor
