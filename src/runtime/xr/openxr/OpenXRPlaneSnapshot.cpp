#include "host/OpenXRPlaneSnapshot.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace Horo::XR::OpenXRInternal {
    namespace {
        struct ResultShape final {
            std::uint32_t capacity;
            std::array<XrSpatialEntityIdEXT, XRPlaneSnapshot::MaximumPlanes> ids{};
            std::array<XrSpatialEntityTrackingStateEXT, XRPlaneSnapshot::MaximumPlanes> states{};

            /** @brief Describes exactly one caller-owned fixed-capacity component query. */
            XrSpatialComponentDataQueryResultEXT Result(void *next) noexcept {
                return {XR_TYPE_SPATIAL_COMPONENT_DATA_QUERY_RESULT_EXT, next, capacity, 0, ids.data(), capacity, 0, states.data()};
            }

            /** @brief Validates output counts and that the runtime preserved borrowed storage. */
            bool Matches(const XrSpatialComponentDataQueryResultEXT &value, const void *next) const noexcept {
                return value.type == XR_TYPE_SPATIAL_COMPONENT_DATA_QUERY_RESULT_EXT && value.next == next &&
                       value.entityIdCapacityInput == capacity && value.entityIds == ids.data() &&
                       value.entityStateCapacityInput == capacity && value.entityStates == states.data() &&
                       value.entityIdCountOutput == value.entityStateCountOutput && value.entityIdCountOutput <= capacity;
            }

            /** @brief Disallows unissued, repeated or reserved entity evidence. */
            bool ValidEntity(std::size_t index) const noexcept {
                const auto id = ids[index];
                const auto state = states[index];
                const auto preceding = std::span{ids}.first(index);
                return id != XR_NULL_SPATIAL_ENTITY_ID_EXT && std::ranges::find(preceding, id) == preceding.end() &&
                       state >= XR_SPATIAL_ENTITY_TRACKING_STATE_STOPPED_EXT && state <= XR_SPATIAL_ENTITY_TRACKING_STATE_TRACKING_EXT;
            }
        };

        /** @brief Translates status and errors without recording geometry or provider entity identifiers. */
        Error QueryError(XrResult result) {
            switch (result) {
                case XR_ERROR_PERMISSION_INSUFFICIENT:
                    return MakeError(XRErrors::PermissionDenied);
                case XR_ERROR_SIZE_INSUFFICIENT:
                    return MakeError(XRErrors::CapacityExceeded);
                case XR_SESSION_LOSS_PENDING:
                case XR_ERROR_SESSION_LOST:
                case XR_ERROR_INSTANCE_LOST:
                    return MakeError(XRErrors::IdentityStale);
                case XR_ERROR_FUNCTION_UNSUPPORTED:
                case XR_ERROR_FEATURE_UNSUPPORTED:
                    return MakeError(XRErrors::OperationUnsupported);
                default:
                    return MakeError(XRErrors::OperationUnavailable, "OpenXR spatial snapshot component query failed");
            }
        }

        /** @brief Normalizes OpenXR's X/Y rectangle with +Z normal to a Horo X/Z rectangle with +Y normal. */
        Result<NativeSpatialPlane> MakePlane(XrSpatialEntityIdEXT entity, const XrSpatialBounded2DDataEXT &rectangle) {
            using Outcome = Result<NativeSpatialPlane>;
            const auto &position = rectangle.center.position;
            const auto &orientation = rectangle.center.orientation;
            const Math::Vec3 center{position.x, position.y, position.z};
            const Math::Vec2 extent{rectangle.extents.width, rectangle.extents.height};
            if (!Math::IsFinite(center) || !std::isfinite(extent.x) || !std::isfinite(extent.y) || extent.x <= 0 || extent.y <= 0)
                return Outcome::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
            const auto rotation = Math::Quaternion{orientation.x, orientation.y, orientation.z, orientation.w}.TryNormalized();
            if (rotation.HasError())
                return Outcome::Failure(MakeError(XRErrors::PoseInvalid));
            const auto toNativeAxes = Math::Quaternion::FromAxisAngle({1.0F, 0.0F, 0.0F}, std::numbers::pi_v<float> / 2.0F);
            return Outcome::Success(
                {entity, center, rotation.Value() * toNativeAxes, extent, XRPlaneClassification::Unknown, XRTrackingConfidence::High});
        }

        /** @brief Projects only semantic claims supplied by the runtime. */
        XRPlaneClassification Classify(XrSpatialPlaneSemanticLabelEXT label) noexcept {
            using enum XRPlaneClassification;
            switch (label) {
                case XR_SPATIAL_PLANE_SEMANTIC_LABEL_FLOOR_EXT:
                    return Floor;
                case XR_SPATIAL_PLANE_SEMANTIC_LABEL_CEILING_EXT:
                    return Ceiling;
                case XR_SPATIAL_PLANE_SEMANTIC_LABEL_WALL_EXT:
                    return Wall;
                case XR_SPATIAL_PLANE_SEMANTIC_LABEL_TABLE_EXT:
                    return Platform;
                default:
                    return Unknown;
            }
        }

        /** @brief Discards native output if the exact grant changes across a runtime callback. */
        Result<void> QueryPlaneData(XrSpatialSnapshotEXT snapshot, PFN_xrQuerySpatialComponentDataEXT query,
                                    const XrSpatialComponentDataQueryConditionEXT &condition, XrSpatialComponentDataQueryResultEXT &result,
                                    NativePlaneReadAuthorization authorization) {
            if (!authorization.AllowsRead())
                return Result<void>::Failure(MakeError(XRErrors::PermissionDenied));
            const auto status = query(snapshot, &condition, &result);
            if (!authorization.AllowsRead())
                return Result<void>::Failure(MakeError(XRErrors::PermissionDenied));
            return status == XR_SUCCESS ? Result<void>::Success() : Result<void>::Failure(QueryError(status));
        }

        /** @brief Joins the optional semantic subset by native entity identity, independent of runtime list order. */
        Result<void> ReadLabels(XrSpatialSnapshotEXT snapshot, PFN_xrQuerySpatialComponentDataEXT query, std::uint32_t capacity,
                                std::span<NativeSpatialPlane> planes, NativePlaneReadAuthorization authorization) {
            constexpr std::array types{XR_SPATIAL_COMPONENT_TYPE_BOUNDED_2D_EXT, XR_SPATIAL_COMPONENT_TYPE_PLANE_SEMANTIC_LABEL_EXT};
            const XrSpatialComponentDataQueryConditionEXT condition{XR_TYPE_SPATIAL_COMPONENT_DATA_QUERY_CONDITION_EXT, nullptr,
                                                                    static_cast<std::uint32_t>(types.size()), types.data()};
            std::array<XrSpatialPlaneSemanticLabelEXT, XRPlaneSnapshot::MaximumPlanes> labels{};
            XrSpatialComponentPlaneSemanticLabelListEXT list{XR_TYPE_SPATIAL_COMPONENT_PLANE_SEMANTIC_LABEL_LIST_EXT, nullptr, capacity,
                                                             labels.data()};
            ResultShape shape{capacity};
            auto result = shape.Result(&list);
            if (const auto queried = QueryPlaneData(snapshot, query, condition, result, authorization); queried.HasError())
                return queried;
            if (!shape.Matches(result, &list) || list.type != XR_TYPE_SPATIAL_COMPONENT_PLANE_SEMANTIC_LABEL_LIST_EXT || list.next ||
                list.semanticLabelCount != capacity || list.semanticLabels != labels.data())
                return Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
            for (std::size_t index = 0; index < result.entityIdCountOutput; ++index) {
                if (!shape.ValidEntity(index))
                    return Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
                if (shape.states[index] != XR_SPATIAL_ENTITY_TRACKING_STATE_TRACKING_EXT)
                    continue;
                const auto plane = std::ranges::find(planes, shape.ids[index], &NativeSpatialPlane::entity);
                if (plane == planes.end())
                    return Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
                plane->classification = Classify(labels[index]);
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ReadNativeSpatialPlanes */
    Result<NativeSpatialPlaneBatch> ReadNativeSpatialPlanes(XrSpatialSnapshotEXT snapshot, PFN_xrQuerySpatialComponentDataEXT query,
                                                            NativePlaneCensus census, std::uint32_t maximumPlanes,
                                                            NativePlaneReadAuthorization authorization) {
        using Outcome = Result<NativeSpatialPlaneBatch>;
        if (census.state == XRCapabilityState::Unsupported || census.state == XRCapabilityState::DependencyMissing)
            return Outcome::Failure(MakeError(XRErrors::OperationUnsupported));
        if (census.state == XRCapabilityState::Denied)
            return Outcome::Failure(MakeError(XRErrors::PermissionDenied));
        if (census.state != XRCapabilityState::Available)
            return Outcome::Failure(MakeError(XRErrors::OperationUnavailable));
        if (!query || snapshot == XR_NULL_HANDLE || maximumPlanes == 0)
            return Outcome::Failure(MakeError(XRErrors::OperationInvalid));
        if (maximumPlanes > XRPlaneSnapshot::MaximumPlanes)
            return Outcome::Failure(MakeError(XRErrors::CapacityExceeded));

        constexpr auto bounds = XR_SPATIAL_COMPONENT_TYPE_BOUNDED_2D_EXT;
        const XrSpatialComponentDataQueryConditionEXT condition{XR_TYPE_SPATIAL_COMPONENT_DATA_QUERY_CONDITION_EXT, nullptr, 1, &bounds};
        std::array<XrSpatialBounded2DDataEXT, XRPlaneSnapshot::MaximumPlanes> rectangles{};
        XrSpatialComponentBounded2DListEXT list{XR_TYPE_SPATIAL_COMPONENT_BOUNDED_2D_LIST_EXT, nullptr, maximumPlanes, rectangles.data()};
        ResultShape shape{maximumPlanes};
        auto result = shape.Result(&list);
        if (const auto queried = QueryPlaneData(snapshot, query, condition, result, authorization); queried.HasError())
            return Outcome::Failure(queried.ErrorValue());
        if (!shape.Matches(result, &list) || list.type != XR_TYPE_SPATIAL_COMPONENT_BOUNDED_2D_LIST_EXT || list.next ||
            list.boundCount != maximumPlanes || list.bounds != rectangles.data())
            return Outcome::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));

        NativeSpatialPlaneBatch output;
        for (std::size_t index = 0; index < result.entityIdCountOutput; ++index) {
            if (!shape.ValidEntity(index))
                return Outcome::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
            if (shape.states[index] != XR_SPATIAL_ENTITY_TRACKING_STATE_TRACKING_EXT)
                continue;
            const auto plane = MakePlane(shape.ids[index], rectangles[index]);
            if (plane.HasError())
                return Outcome::Failure(plane.ErrorValue());
            output.planes_[output.count_++] = plane.Value();
        }
        auto planes = std::span{output.planes_}.first(output.count_);
        if (census.semanticLabels && !planes.empty()) {
            const auto semantic = ReadLabels(snapshot, query, maximumPlanes, planes, authorization);
            if (semantic.HasError())
                return Outcome::Failure(semantic.ErrorValue());
        }
        std::ranges::sort(planes, {}, &NativeSpatialPlane::entity);
        return Outcome::Success(std::move(output));
    }
}  // namespace Horo::XR::OpenXRInternal
