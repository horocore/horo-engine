#pragma once
/** @file OpenXRPlaneCaptureTestSupport.h @brief Scripted native plane futures shared by owner and signed Play tests. */
#include "OpenXRCensusTestSupport.h"

namespace Horo::Tests::OpenXR {
    struct PlaneSnapshotFixture final {
        static inline PlaneSnapshotFixture *current{};
        XrResult result{XR_SUCCESS};
        bool duplicate{};
        bool staleLabel{};
        bool malformed{};
        bool countMismatch{};
        bool noTracking{};
        std::uint32_t calls{};
        std::uint32_t labelCalls{};

        PlaneSnapshotFixture() {
            current = this;
        }

        ~PlaneSnapshotFixture() {
            current = nullptr;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Query(XrSpatialSnapshotEXT snapshot, const XrSpatialComponentDataQueryConditionEXT *condition,
                                                    XrSpatialComponentDataQueryResultEXT *output) {
            REQUIRE(snapshot != XR_NULL_HANDLE);
            REQUIRE(condition->componentTypes[0] == XR_SPATIAL_COMPONENT_TYPE_BOUNDED_2D_EXT);
            ++current->calls;
            if (current->result != XR_SUCCESS)
                return current->result;
            if (condition->componentTypeCount == 2) {
                ++current->labelCalls;
                REQUIRE(condition->componentTypes[1] == XR_SPATIAL_COMPONENT_TYPE_PLANE_SEMANTIC_LABEL_EXT);
                auto &labels = *static_cast<XrSpatialComponentPlaneSemanticLabelListEXT *>(output->next);
                REQUIRE(labels.semanticLabelCount >= 1);
                output->entityIdCountOutput = output->entityStateCountOutput = 1;
                output->entityIds[0] = current->staleLabel ? 42 : 5;
                output->entityStates[0] = XR_SPATIAL_ENTITY_TRACKING_STATE_TRACKING_EXT;
                labels.semanticLabels[0] = XR_SPATIAL_PLANE_SEMANTIC_LABEL_TABLE_EXT;
                return XR_SUCCESS;
            }
            auto &rectangles = *static_cast<XrSpatialComponentBounded2DListEXT *>(output->next);
            if (output->entityIdCapacityInput < 3)
                return XR_ERROR_SIZE_INSUFFICIENT;
            REQUIRE(rectangles.boundCount == output->entityIdCapacityInput);
            output->entityIdCountOutput = 3;
            output->entityStateCountOutput = current->countMismatch ? 2 : 3;
            output->entityIds[0] = 20;
            output->entityIds[1] = current->duplicate ? 20 : 11;
            output->entityIds[2] = 5;
            output->entityStates[0] =
                current->noTracking ? XR_SPATIAL_ENTITY_TRACKING_STATE_PAUSED_EXT : XR_SPATIAL_ENTITY_TRACKING_STATE_TRACKING_EXT;
            output->entityStates[1] = XR_SPATIAL_ENTITY_TRACKING_STATE_STOPPED_EXT;
            output->entityStates[2] = output->entityStates[0];
            rectangles.bounds[0] = {{.orientation = {0, 0, 0, 1}, .position = {1, 2, 3}}, {4, 5}};
            rectangles.bounds[2] = {{.orientation = {0, 0, 0, 1}, .position = {7, 8, 9}}, {current->malformed ? 0.0F : 4.0F, 5}};
            return XR_SUCCESS;
        }
    };

    struct SpatialCaptureScript final {
        static inline SpatialCaptureScript *current{};
        static inline const auto ContextFuture = NativeHandle<XrFutureEXT>(9);
        static inline const auto ScanFuture = NativeHandle<XrFutureEXT>(17);
        bool ready{};
        bool failDestroy{};
        XrResult completeResult{XR_SUCCESS};
        std::uint32_t contextStarts{};
        std::uint32_t scans{};
        std::uint32_t cancels{};
        bool hasContext{};
        bool hasSnapshot{};

        SpatialCaptureScript() {
            current = this;
        }

        ~SpatialCaptureScript() {
            current = nullptr;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Create(XrSession session, const XrSpatialContextCreateInfoEXT *info, XrFutureEXT *future) {
            CHECK(session != XR_NULL_HANDLE);
            REQUIRE(info->capabilityConfigCount == 1);
            REQUIRE(info->capabilityConfigs[0]->capability == XR_SPATIAL_CAPABILITY_PLANE_TRACKING_EXT);
            REQUIRE(info->capabilityConfigs[0]->enabledComponents[0] == XR_SPATIAL_COMPONENT_TYPE_BOUNDED_2D_EXT);
            ++current->contextStarts;
            *future = ContextFuture;
            return XR_SUCCESS;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Complete(XrSession, XrFutureEXT future, XrCreateSpatialContextCompletionEXT *value) {
            CHECK(future == ContextFuture);
            CHECK_FALSE(current->hasContext);
            current->hasContext = true;
            value->futureResult = XR_SUCCESS;
            value->spatialContext = NativeHandle<XrSpatialContextEXT>(33);
            return XR_SUCCESS;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Destroy(XrSpatialContextEXT) {
            CHECK(current->hasContext);
            CHECK_FALSE(current->hasSnapshot);
            if (current->failDestroy)
                return XR_ERROR_RUNTIME_FAILURE;
            current->hasContext = false;
            return XR_SUCCESS;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Poll(XrInstance, const XrFuturePollInfoEXT *info, XrFuturePollResultEXT *value) {
            CHECK(info->future == (current->hasContext ? ScanFuture : ContextFuture));
            value->state = current->ready ? XR_FUTURE_STATE_READY_EXT : XR_FUTURE_STATE_PENDING_EXT;
            return XR_SUCCESS;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Cancel(XrInstance, const XrFutureCancelInfoEXT *info) {
            CHECK(info->future == (current->hasContext ? ScanFuture : ContextFuture));
            ++current->cancels;
            return XR_SUCCESS;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL StartScan(XrSpatialContextEXT, const XrSpatialDiscoverySnapshotCreateInfoEXT *info,
                                                        XrFutureEXT *future) {
            CHECK(current->hasContext);
            CHECK(info->componentTypeCount == 1);
            ++current->scans;
            *future = ScanFuture;
            return XR_SUCCESS;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL CompleteScan(XrSpatialContextEXT,
                                                           const XrCreateSpatialDiscoverySnapshotCompletionInfoEXT *info,
                                                           XrCreateSpatialDiscoverySnapshotCompletionEXT *value) {
            CHECK(info->future == ScanFuture);
            CHECK(info->baseSpace != XR_NULL_HANDLE);
            CHECK(info->time > 0);
            value->futureResult = current->completeResult;
            if (value->futureResult == XR_SUCCESS) {
                current->hasSnapshot = true;
                value->snapshot = NativeHandle<XrSpatialSnapshotEXT>(44);
            }
            return XR_SUCCESS;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL DestroySnapshot(XrSpatialSnapshotEXT) {
            CHECK(current->hasSnapshot);
            current->hasSnapshot = false;
            return XR_SUCCESS;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL GetProc(XrInstance, const char *name, PFN_xrVoidFunction *function) {
            struct Entry {
                std::string_view name;
                PFN_xrVoidFunction function;
            };

            const std::array table{
                Entry{"xrCreateSpatialContextAsyncEXT", std::bit_cast<PFN_xrVoidFunction>(&Create)},
                Entry{"xrCreateSpatialContextCompleteEXT", std::bit_cast<PFN_xrVoidFunction>(&Complete)},
                Entry{"xrDestroySpatialContextEXT", std::bit_cast<PFN_xrVoidFunction>(&Destroy)},
                Entry{"xrPollFutureEXT", std::bit_cast<PFN_xrVoidFunction>(&Poll)},
                Entry{"xrCancelFutureEXT", std::bit_cast<PFN_xrVoidFunction>(&Cancel)},
                Entry{"xrCreateSpatialDiscoverySnapshotAsyncEXT", std::bit_cast<PFN_xrVoidFunction>(&StartScan)},
                Entry{"xrCreateSpatialDiscoverySnapshotCompleteEXT", std::bit_cast<PFN_xrVoidFunction>(&CompleteScan)},
                Entry{"xrDestroySpatialSnapshotEXT", std::bit_cast<PFN_xrVoidFunction>(&DestroySnapshot)},
                Entry{"xrQuerySpatialComponentDataEXT", std::bit_cast<PFN_xrVoidFunction>(&PlaneSnapshotFixture::Query)},
            };
            const auto found = std::ranges::find(table, std::string_view{name}, &Entry::name);
            if (found == table.end())
                return XR_ERROR_FUNCTION_UNSUPPORTED;
            *function = found->function;
            return XR_SUCCESS;
        }
    };

}  // namespace Horo::Tests::OpenXR
