#include "ProjectScenePerception.h"

#include "Horo/AI/AIErrors.h"
#include "Horo/AI/PerceptionSpatialBroadphase.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <utility>

namespace Horo::Application::Internal {
    namespace {
        using Json = nlohmann::json;
        constexpr std::size_t MaximumPolicyBytes = 64U * 1024U;
        constexpr std::size_t MaximumControllers = 128;
        constexpr std::size_t MaximumAncestors = 128;
        using Positions = Result<std::vector<AI::AiScenePerceptionPosition>>;

        /** @brief Reads a closed, bounded shape with duplicate object keys rejected before insertion. */
        Json ParsePolicy(const std::string &text) {
            std::array<std::set<std::string, std::less<>>, 5> keys;
            const auto callback = [&](const int depth, const Json::parse_event_t event, const Json &value) {
                if (depth < 0 || static_cast<std::size_t>(depth) >= keys.size())
                    throw std::invalid_argument("Perception policy nesting exceeded");
                const auto index = static_cast<std::size_t>(depth);
                if (event == Json::parse_event_t::object_start) {
                    if (index + 1 >= keys.size())
                        throw std::invalid_argument("Perception policy object nesting exceeded");
                    keys[index + 1].clear();
                }
                if (event == Json::parse_event_t::key && !keys[index].insert(value.get<std::string>()).second)
                    throw std::invalid_argument("Duplicate Perception policy key");
                return true;
            };
            return Json::parse(text, callback);
        }

        /** @brief Requires the complete closed wire shape. */
        void RequireFields(const Json &object, std::initializer_list<std::string_view> fields) {
            if (!object.is_object() || object.size() != fields.size() || std::ranges::any_of(fields, [&](const auto field) {
                return !object.contains(field);
            }))
                throw std::invalid_argument("Incomplete Perception policy object");
        }

        /** @brief Reads only lossless unsigned integer representations. */
        std::uint64_t Unsigned(const Json &value) {
            if (!value.is_number_unsigned() && (!value.is_number_integer() || value.get<std::int64_t>() < 0))
                throw std::invalid_argument("Unsigned Perception value required");
            return value.get<std::uint64_t>();
        }

        /** @brief Reads only lossless signed millimeter coordinates. */
        std::int64_t SignedMillimeters(const Json &value) {
            if (!value.is_number_integer() ||
                (value.is_number_unsigned() && value.get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX)))
                throw std::invalid_argument("Signed Perception coordinate required");
            return value.get<std::int64_t>();
        }

        /** @brief Admits ordered unique stable selectors, independent of policy declaration order. */
        template <typename Id> std::vector<Id> ReadSelectors(const Json &array) {
            if (!array.is_array() || array.size() > AI::AiControllerPerceptionEndpoints::MaximumSenses)
                throw std::invalid_argument("Perception selector capacity exceeded");
            std::vector<Id> values;
            values.reserve(array.size());
            for (const auto &value : array) {
                auto parsed = Id::Create(Unsigned(value));
                if (parsed.HasError())
                    throw std::invalid_argument("Invalid Perception selector");
                values.push_back(parsed.Value());
            }
            std::ranges::sort(values);
            if (std::ranges::adjacent_find(values) != values.end())
                throw std::invalid_argument("Duplicate Perception selector");
            return values;
        }

        /** @brief Projects inert durable controller fields without choosing any runtime implementation. */
        ProjectPerceptionControllerSelection ReadController(const Json &object) {
            RequireFields(object, {"controller", "listeners", "sources", "layers", "mask"});
            auto controller = AI::ControllerTypeId::Create(Unsigned(object["controller"]));
            if (controller.HasError())
                throw std::invalid_argument("Invalid controller identity");
            ProjectPerceptionControllerSelection result{.controller = controller.Value(),
                                                        .listeners = ReadSelectors<AI::PerceptionListenerTypeId>(object["listeners"]),
                                                        .sources = ReadSelectors<AI::SenseTypeId>(object["sources"]),
                                                        .participant = {.layers = Unsigned(object["layers"]),
                                                                        .mask = Unsigned(object["mask"])}};
            if (result.participant.layers == 0 || (result.listeners.empty() && result.sources.empty()))
                throw std::invalid_argument("Controller requires declared endpoint layers and a listener or source");
            return result;
        }

        /** @brief Validates explicit native listeners and a bounded execution window for schema two. */
        ProjectNativePerceptionPolicy ReadNative(const Json &object) {
            RequireFields(object, {"listeners", "radiusMillimeters", "visibleLayers", "workUnitsPerTick"});
            const auto &listeners = object["listeners"];
            const auto radius = Unsigned(object["radiusMillimeters"]);
            const auto layers = Unsigned(object["visibleLayers"]);
            const auto units = Unsigned(object["workUnitsPerTick"]);
            if (!listeners.is_array() || listeners.empty() ||
                listeners.size() > AI::PerceptionDescriptorRegistryHardLimits::ListenerTypes ||
                radius > AI::PerceptionSpatialLimits::RadiusMillimeters || layers == 0 || units == 0 ||
                units > AI::AiTaskSchedulerSettings::HardWorkUnits)
                throw std::invalid_argument("Native Perception execution policy exceeds its bounds");
            ProjectNativePerceptionPolicy policy{{}, radius, layers, static_cast<std::size_t>(units)};
            policy.listeners.reserve(listeners.size());
            for (const auto &entry : listeners) {
                RequireFields(entry, {"id", "sense"});
                const auto listener = AI::PerceptionListenerTypeId::Create(Unsigned(entry["id"]));
                const auto sense = AI::SenseTypeId::Create(Unsigned(entry["sense"]));
                if (listener.HasError() || sense.HasError())
                    throw std::invalid_argument("Invalid native Perception listener identity");
                policy.listeners.push_back({listener.Value(), sense.Value()});
            }
            constexpr auto key = &ProjectPerceptionListenerSelection::listener;
            std::ranges::sort(policy.listeners, {}, key);
            if (std::ranges::adjacent_find(policy.listeners, {}, key) != policy.listeners.end())
                throw std::invalid_argument("Duplicate native Perception listener");
            return policy;
        }

        /** @brief Quantizes canonical local meters once and checks exact signed addition to the authored origin. */
        Result<Math::WorldCoordinate64> GlobalPosition(const Math::WorldCoordinate64 origin, const Math::Vec3 local) {
            using Output = Result<Math::WorldCoordinate64>;
            const auto anchors = origin.Millimeters();
            const std::array meters{local.x, local.y, local.z};
            std::array<std::int64_t, 3> millimeters{};
            constexpr auto limit = static_cast<double>(std::uint64_t{1} << 63);
            for (std::size_t axis = 0; axis < meters.size(); ++axis) {
                const auto rounded = std::round(static_cast<double>(meters[axis]) * 1000.0);
                if (!std::isfinite(rounded) || rounded < -limit || rounded >= limit)
                    return Output::Failure(MakeError(Math::WorldCoordinateErrors::CoordinateOutOfRange));
                const auto offset = static_cast<std::int64_t>(rounded);
                if ((offset > 0 && anchors[axis] > INT64_MAX - offset) || (offset < 0 && anchors[axis] < INT64_MIN - offset))
                    return Output::Failure(MakeError(Math::WorldCoordinateErrors::CoordinateOutOfRange));
                millimeters[axis] = anchors[axis] + offset;
            }
            return Output::Success(Math::WorldCoordinate64::FromMillimeters(millimeters[0], millimeters[1], millimeters[2]));
        }

        /** @brief Composes the exact available hierarchy from live and prospective generation-fenced views. */
        Result<Math::WorldCoordinate64> CapturePosition(Math::WorldCoordinate64 origin, const Runtime::RuntimeSceneView &scene,
                                                        Runtime::RuntimeEntityView entity,
                                                        const std::span<const Runtime::RuntimeEntityView> prospective) {
            using Output = Result<Math::WorldCoordinate64>;
            auto matrix = Math::Mat4::Identity();
            for (std::size_t depth = 0; depth < MaximumAncestors; ++depth) {
                if (entity.entity.runtime != scene.RuntimeId() || !entity.localTransform)
                    return Output::Failure(MakeError(AI::AIErrors::PerceptionDependencyMissing));
                const auto transform = entity.localTransform->TryToMatrix();
                if (transform.HasError())
                    return Output::Failure(transform.ErrorValue());
                matrix = Math::Multiply(transform.Value(), matrix);
                if (!entity.parent) {
                    const auto point = Math::TryTransformPoint(matrix, {});
                    if (point.HasError())
                        return Output::Failure(point.ErrorValue());
                    return GlobalPosition(origin, point.Value());
                }
                const auto parent = std::ranges::lower_bound(prospective, *entity.parent, {}, &Runtime::RuntimeEntityView::entity);
                if (parent != prospective.end() && parent->entity == *entity.parent) {
                    entity = *parent;
                    continue;
                }
                const auto live = scene.Get(*entity.parent);
                if (live.HasError())
                    return Output::Failure(MakeError(AI::AIErrors::PerceptionDependencyMissing,
                                                     "Perception cannot capture a detached ancestor outside its admitted views."));
                entity = live.Value();
            }
            return Output::Failure(MakeError(AI::AIErrors::PerceptionSpatialLimitExceeded));
        }

        /** @brief Tests association with this exact definition and an explicitly selected endpoint controller. */
        bool Selects(const ProjectScenePerceptionPolicy &policy, const Runtime::RuntimeEntityView &entity) {
            if (!entity.components || !entity.components->aiController || !entity.components->aiController->enabled ||
                !entity.components->aiAgent || !entity.components->aiAgent->enabled)
                return false;
            return std::ranges::binary_search(policy.controllers, entity.components->aiController->controller, {},
                                              &ProjectPerceptionControllerSelection::controller);
        }

        /** @brief Rejects stale revisions and unimplemented placement evidence before reading transforms. */
        bool OwnsDefinition(const ProjectScenePerceptionPolicy &policy, const Runtime::RuntimeSceneView &scene,
                            const std::optional<ProjectPerceptionSceneProjection> &projection) {
            const auto expected = projection.value_or(ProjectPerceptionSceneProjection{policy.definition, policy.revision});
            return scene.IsCurrent() && scene.DefinitionId() == expected.definition && scene.DefinitionRevision() == expected.revision;
        }
    }  // namespace

    /** @copydoc CaptureProjectScenePerception */
    Result<std::shared_ptr<const ProjectScenePerceptionPolicy>> CaptureProjectScenePerception(
        const Platform::IProjectReadFiles &files, const Platform::ProjectReadContext &context, const std::string_view path,
        const Assets::AssetId asset, const Runtime::RuntimeSceneDefinition &definition) {
        using Output = Result<std::shared_ptr<const ProjectScenePerceptionPolicy>>;
        if (!asset.IsValid())
            return Output::Failure(MakeError(AI::AIErrors::PerceptionDependencyMissing, "Select an exact committed Scene for Perception."));
        const auto read = files.SourceText(path, context, {.maximumFileBytes = MaximumPolicyBytes});
        if (read.HasError())
            return Output::Failure(read.ErrorValue());
        try {
            const auto root = ParsePolicy(*read.Value().text);
            const auto schema = root.is_object() && root.contains("schema") ? Unsigned(root["schema"]) : 0;
            if (schema == 1)
                RequireFields(root, {"schema", "sceneAsset", "sceneRevision", "worldOriginMillimeters", "controllers"});
            else if (schema == 2)
                RequireFields(root, {"schema", "sceneAsset", "sceneRevision", "worldOriginMillimeters", "controllers", "native"});
            else
                throw std::invalid_argument("Unsupported Perception policy schema");
            if (!root["sceneAsset"].is_string())
                throw std::invalid_argument("Perception policy requires a Scene AssetId");
            const auto selected = Assets::AssetId::Parse(root["sceneAsset"].get<std::string>());
            if (!selected.HasValue())
                return Output::Failure(selected.ErrorValue());
            if (selected.Value() != asset || Unsigned(root["sceneRevision"]) != definition.Revision().value)
                return Output::Failure(
                    MakeError(AI::AIErrors::PerceptionSpatialStale, "Perception policy targets a different Scene revision."));
            const auto &origin = root["worldOriginMillimeters"];
            const auto &controllers = root["controllers"];
            if (!origin.is_array() || origin.size() != 3 || !controllers.is_array() || controllers.empty() ||
                controllers.size() > MaximumControllers)
                throw std::invalid_argument("Incomplete Perception origin or controller inventory");
            ProjectScenePerceptionPolicy policy{asset,
                                                definition.Id(),
                                                definition.Revision(),
                                                Math::WorldCoordinate64::FromMillimeters(SignedMillimeters(origin[0]),
                                                                                         SignedMillimeters(origin[1]),
                                                                                         SignedMillimeters(origin[2])),
                                                {}};
            policy.controllers.reserve(controllers.size());
            for (const auto &controller : controllers)
                policy.controllers.push_back(ReadController(controller));
            constexpr auto key = &ProjectPerceptionControllerSelection::controller;
            std::ranges::sort(policy.controllers, {}, key);
            if (std::ranges::adjacent_find(policy.controllers, {}, key) != policy.controllers.end())
                throw std::invalid_argument("Duplicate Perception controller");
            if (schema == 2)
                policy.native = ReadNative(root["native"]);
            return Output::Success(std::make_shared<const ProjectScenePerceptionPolicy>(std::move(policy)));
        } catch (const Json::exception &) {
            return Output::Failure(MakeError(AI::AIErrors::PerceptionDescriptorInvalid, "Malformed project Perception policy."));
        } catch (const std::invalid_argument &error) {
            return Output::Failure(MakeError(AI::AIErrors::PerceptionDescriptorInvalid, error.what()));
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(AI::AIErrors::AgentCapacityExceeded));
        }
    }

    /** @copydoc FreezeProjectNativePerceptionRegistry */
    Result<std::shared_ptr<const AI::PerceptionDescriptorRegistry>> FreezeProjectNativePerceptionRegistry(
        const ProjectNativePerceptionPolicy &policy, const std::span<const AI::PerceptionSenseContract> contracts) {
        using Output = Result<std::shared_ptr<const AI::PerceptionDescriptorRegistry>>;
        try {
            std::vector<AI::SenseTypeDescriptor> senses;
            std::vector<AI::StimulusTypeDescriptor> stimuli;
            std::vector<AI::PerceptionListenerDescriptor> listeners;
            senses.reserve(contracts.size());
            stimuli.reserve(contracts.size());
            listeners.reserve(policy.listeners.size());
            for (const auto &contract : contracts) {
                senses.push_back(contract.sense);
                const auto existing = std::ranges::find(stimuli, contract.stimulus.identity, &AI::StimulusTypeDescriptor::identity);
                if (existing == stimuli.end())
                    stimuli.push_back(contract.stimulus);
                else if (*existing != contract.stimulus)
                    return Output::Failure(MakeError(AI::AIErrors::PerceptionDescriptorConflict));
            }
            for (const auto &listener : policy.listeners) {
                const auto owner = std::ranges::find(contracts, listener.sense, [](const auto &item) {
                    return item.sense.identity;
                });
                if (owner == contracts.end())
                    return Output::Failure(MakeError(AI::AIErrors::PerceptionDependencyMissing,
                                                     "The selected native module did not declare a requested Perception sense."));
                listeners.push_back({.identity = listener.listener,
                                     .origin = owner->sense.origin,
                                     .displayName = owner->sense.displayName,
                                     .sense = owner->sense.identity,
                                     .stimuli = {{owner->stimulus.identity}},
                                     .stimulusCount = 1});
            }
            auto captured = AI::PerceptionDescriptorRegistry::Capture({senses, stimuli, listeners}, policy.capabilities);
            if (captured.HasError())
                return Output::Failure(captured.ErrorValue());
            return Output::Success(std::make_shared<const AI::PerceptionDescriptorRegistry>(std::move(captured).Value()));
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(AI::AIErrors::AgentCapacityExceeded));
        }
    }

    /** @copydoc BindProjectPerceptionEndpoints */
    Result<std::vector<AI::AiControllerDescriptor>> BindProjectPerceptionEndpoints(
        const ProjectScenePerceptionPolicy &policy, std::shared_ptr<const AI::PerceptionDescriptorRegistry> registry,
        const std::span<const AI::AiControllerDescriptor> catalog) {
        using Output = Result<std::vector<AI::AiControllerDescriptor>>;
        if (!registry)
            return Output::Failure(MakeError(AI::AIErrors::PerceptionDependencyMissing));
        try {
            std::vector<AI::AiControllerDescriptor> bound{catalog.begin(), catalog.end()};
            for (const auto &selection : policy.controllers) {
                const auto chosen = std::ranges::find(bound, selection.controller, &AI::AiControllerDescriptor::controller);
                if (chosen == bound.end())
                    return Output::Failure(MakeError(AI::AIErrors::PerceptionDependencyMissing, "Selected AI controller is unavailable."));
                if (chosen->perceptionEndpoints || std::ranges::find(std::next(chosen), bound.end(), selection.controller,
                                                                     &AI::AiControllerDescriptor::controller) != bound.end())
                    return Output::Failure(MakeError(AI::AIErrors::PerceptionDescriptorConflict));
                chosen->perceptionEndpoints = std::make_shared<const AI::AiControllerPerceptionEndpoints>(
                    AI::AiControllerPerceptionEndpoints{.registry = registry,
                                                        .listeners = selection.listeners,
                                                        .sources = selection.sources,
                                                        .participant = selection.participant});
                if (const auto valid = AI::ValidateAiControllerDescriptor(*chosen); valid.HasError())
                    return Output::Failure(valid.ErrorValue());
            }
            return Output::Success(std::move(bound));
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(AI::AIErrors::AgentCapacityExceeded));
        }
    }

    /** @copydoc CaptureProjectSceneWorldPosition */
    Result<Math::WorldCoordinate64> CaptureProjectSceneWorldPosition(const Runtime::RuntimeSceneView &scene, Runtime::EntityRef entity,
                                                                     Math::WorldCoordinate64 origin) {
        const auto live = scene.Get(entity);
        if (live.HasError())
            return Result<Math::WorldCoordinate64>::Failure(live.ErrorValue());
        return CapturePosition(origin, scene, live.Value(), {});
    }

    /** @copydoc MakeProjectPerceptionPositionSources */
    ProjectPerceptionPositionSources MakeProjectPerceptionPositionSources(std::shared_ptr<const ProjectScenePerceptionPolicy> policy,
                                                                          std::optional<ProjectPerceptionSceneProjection> projection) {
        return {[policy, projection](const Runtime::RuntimeSceneView &scene) {
            if (!policy || !OwnsDefinition(*policy, scene, projection))
                return Positions::Failure(MakeError(AI::AIErrors::PerceptionSpatialStale));
            std::vector<AI::AiScenePerceptionPosition> result;
            for (std::size_t index = 0; index < scene.SlotCount(); ++index) {
                const auto entity = scene.EntityAt(index);
                if (!entity || !Selects(*policy, *entity))
                    continue;
                constexpr auto maximumEndpoints = AI::PerceptionSpatialLimits::Listeners + AI::PerceptionSpatialLimits::Sources;
                if (result.size() == maximumEndpoints)
                    return Positions::Failure(MakeError(AI::AIErrors::PerceptionSpatialLimitExceeded));
                const auto position = CapturePosition(policy->origin, scene, *entity, {});
                if (position.HasError())
                    return Positions::Failure(position.ErrorValue());
                result.push_back({entity->entity, position.Value()});
            }
            return Positions::Success(std::move(result));
        }, [policy, projection](const Runtime::RuntimeSceneView &scene, const std::span<const Runtime::RuntimeEntityView> prospective) {
            if (!policy || !OwnsDefinition(*policy, scene, projection))
                return Positions::Failure(MakeError(AI::AIErrors::PerceptionSpatialStale));
            std::vector<AI::AiScenePerceptionPosition> result;
            result.reserve(prospective.size());
            for (const auto &entity : prospective) {
                if (!Selects(*policy, entity))
                    return Positions::Failure(MakeError(AI::AIErrors::PerceptionDependencyMissing));
                const auto position = CapturePosition(policy->origin, scene, entity, prospective);
                if (position.HasError())
                    return Positions::Failure(position.ErrorValue());
                result.push_back({entity.entity, position.Value()});
            }
            return Positions::Success(std::move(result));
        }};
    }
}  // namespace Horo::Application::Internal
