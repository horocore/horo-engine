#pragma once
#include "OpenXRActionBindings.h"
#include "support/OpenXRNativeTestSupport.h"

#include <algorithm>
#include <bit>
#include <format>
#include <limits>
#include <new>
#include <stdexcept>

namespace Horo::Tests::OpenXRAction {
    using namespace Horo::XR;
    using namespace Horo::XR::OpenXRInternal;
    using namespace Horo::Tests::OpenXR;

    struct ActionScript final {
        static inline thread_local ActionScript *active{};
        std::vector<std::pair<std::string, XrPath>> paths;
        std::vector<std::string> actionNames;
        std::array<XrPath, 2> currentProfiles{};
        unsigned liveSets{};
        unsigned liveActions{};
        unsigned suggestions{};
        unsigned attaches{};
        unsigned syncs{};

        struct SampleState final {
            XrBool32 active{XR_TRUE};
            XrBool32 boolean{XR_TRUE};
            float scalar{0.5F};
            XrVector2f vector{0.25F, -0.5F};
            XrActionType expectedType{XR_ACTION_TYPE_BOOLEAN_INPUT};
            XRResolvedActionBindings *mutatePlan{};
            XRTrackedDeviceRecord *mutateDevice{};
            OpenXRActionBindings *reentrantOwner{};
            bool closeRejected{};
        } sample;

        struct TrackingState final {
            unsigned spaces{};
            std::vector<XrReferenceSpaceCreateInfo> references;
            bool enabled{};
            XrTime locatedTime{};
            XrSpaceLocationFlags flags{XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                                       XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT};
            XrSpaceVelocityFlags velocityFlags{XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT};
        } tracking;

        bool throwFormat{};
        bool throwRuntime{};
        bool throwAllocation{};
        bool throwUnsupportedStandard{};
        bool throwForeign{};

        ActionScript() {
            REQUIRE(active == nullptr);
            active = this;
        }

        ~ActionScript() {
            active = nullptr;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL StringToPath(XrInstance, const char *path, XrPath *result) {
            const auto status = Script::Record("path");
            if (XR_FAILED(status))
                return status;
            auto found = std::ranges::find(active->paths, std::string_view{path}, [](const auto &entry) {
                return std::string_view{entry.first};
            });
            if (found == active->paths.end()) {
                active->paths.emplace_back(path, active->paths.size() + 1);
                found = std::prev(active->paths.end());
            }
            *result = found->second;
            return XR_SUCCESS;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL CreateSet(XrInstance, const XrActionSetCreateInfo *info, XrActionSet *set) {
            REQUIRE(info->type == XR_TYPE_ACTION_SET_CREATE_INFO);
            REQUIRE(std::string_view{info->actionSetName}.starts_with("horo_set_"));
            REQUIRE(std::string_view{info->localizedActionSetName} == "Gameplay");
            const auto result = Script::Record("create-set");
            if (XR_SUCCEEDED(result))
                *set = NativeHandle<XrActionSet>(100 + ++active->liveSets);
            return result;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL DestroySet(XrActionSet) {
            const auto result = Script::Record("destroy-set");
            if (XR_SUCCEEDED(result))
                --active->liveSets;
            return result;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL CreateAction(XrActionSet, const XrActionCreateInfo *info, XrAction *action) {
            REQUIRE(info->type == XR_TYPE_ACTION_CREATE_INFO);
            REQUIRE(info->countSubactionPaths == 1);
            REQUIRE(info->subactionPaths[0] != XR_NULL_PATH);
            if (info->actionType == XR_ACTION_TYPE_POSE_INPUT) {
                REQUIRE(active->tracking.enabled);
                REQUIRE(std::string_view{info->localizedActionName} == "Grip");
            } else {
                REQUIRE(info->actionType == active->sample.expectedType);
                REQUIRE(std::string_view{info->localizedActionName} == "Select");
            }
            active->actionNames.emplace_back(info->actionName);
            const auto result = Script::Record("create-action");
            if (XR_SUCCEEDED(result))
                *action = NativeHandle<XrAction>(200 + ++active->liveActions);
            if (active->throwFormat)
                throw std::format_error("scripted action formatting exception");
            if (active->throwRuntime)
                throw std::runtime_error("unsupported scripted runtime exception");
            if (active->throwAllocation)
                throw std::bad_alloc{};
            if (active->throwUnsupportedStandard)
                throw std::logic_error("unsupported scripted preparation exception");
            if (active->throwForeign)
                throw std::uint32_t{7};
            return result;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL DestroyAction(XrAction) {
            REQUIRE(active->tracking.spaces == 0);
            const auto result = Script::Record("destroy-action");
            if (XR_SUCCEEDED(result))
                --active->liveActions;
            return result;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Suggest(XrInstance, const XrInteractionProfileSuggestedBinding *info) {
            REQUIRE(info->type == XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING);
            REQUIRE(info->interactionProfile != XR_NULL_PATH);
            REQUIRE(info->countSuggestedBindings == (active->tracking.enabled ? 2 : 1));
            REQUIRE(info->suggestedBindings[0].action != XR_NULL_HANDLE);
            ++active->suggestions;
            return Script::Record("suggest");
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Attach(XrSession, const XrSessionActionSetsAttachInfo *info) {
            REQUIRE(info->type == XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO);
            REQUIRE(info->countActionSets == 1);
            ++active->attaches;
            return Script::Record("attach");
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Profile(XrSession, XrPath hand, XrInteractionProfileState *state) {
            REQUIRE(state->type == XR_TYPE_INTERACTION_PROFILE_STATE);
            if (active->attaches == 0)
                return XR_ERROR_ACTIONSET_NOT_ATTACHED;
            const auto left = std::ranges::find(active->paths, std::string{"/user/hand/left"}, &std::pair<std::string, XrPath>::first);
            state->interactionProfile = active->currentProfiles[left->second == hand ? 0 : 1];
            return Script::Record("profile");
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Sync(XrSession, const XrActionsSyncInfo *info) {
            REQUIRE(info->type == XR_TYPE_ACTIONS_SYNC_INFO);
            REQUIRE(info->countActiveActionSets == 1);
            ++active->syncs;
            return Script::Record("sync");
        }

        static XRAPI_ATTR XrResult XRAPI_CALL BooleanState(XrSession, const XrActionStateGetInfo *info, XrActionStateBoolean *state) {
            REQUIRE(info->type == XR_TYPE_ACTION_STATE_GET_INFO);
            REQUIRE(info->action != XR_NULL_HANDLE);
            REQUIRE(info->subactionPath != XR_NULL_PATH);
            const auto result = Script::Record("sample-boolean");
            if (XR_FAILED(result))
                return result;
            state->isActive = active->sample.active;
            state->currentState = active->sample.boolean;
            if (active->sample.mutatePlan)
                active->sample.mutatePlan->bindings.clear();
            if (active->sample.mutateDevice)
                ++active->sample.mutateDevice->id.slot.generation;
            if (active->sample.reentrantOwner)
                active->sample.closeRejected = active->sample.reentrantOwner->Close().HasError();
            return XR_SUCCESS;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL FloatState(XrSession, const XrActionStateGetInfo *, XrActionStateFloat *state) {
            const auto result = Script::Record("sample-float");
            if (XR_FAILED(result))
                return result;
            state->isActive = active->sample.active;
            state->currentState = active->sample.scalar;
            return XR_SUCCESS;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL VectorState(XrSession, const XrActionStateGetInfo *, XrActionStateVector2f *state) {
            const auto result = Script::Record("sample-vector");
            if (XR_FAILED(result))
                return result;
            state->isActive = active->sample.active;
            state->currentState = active->sample.vector;
            return XR_SUCCESS;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL PoseState(XrSession, const XrActionStateGetInfo *info, XrActionStatePose *state) {
            REQUIRE(info->action != XR_NULL_HANDLE);
            REQUIRE(info->subactionPath != XR_NULL_PATH);
            const auto result = Script::Record("sample-pose");
            state->isActive = active->sample.active;
            return result;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL ReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo *info, XrSpace *space) {
            REQUIRE(info->type == XR_TYPE_REFERENCE_SPACE_CREATE_INFO);
            if (active->tracking.references.size() < 2) {
                REQUIRE(info->poseInReferenceSpace.orientation.w == 1.0F);
                REQUIRE(info->poseInReferenceSpace.position.x == 0.0F);
                REQUIRE(info->poseInReferenceSpace.position.y == 0.0F);
                REQUIRE(info->poseInReferenceSpace.position.z == 0.0F);
            }
            active->tracking.references.push_back(*info);
            const auto result = Script::Record("create-reference");
            if (XR_SUCCEEDED(result))
                *space = NativeHandle<XrSpace>(300 + ++active->tracking.spaces);
            return result;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL ActionSpace(XrSession, const XrActionSpaceCreateInfo *info, XrSpace *space) {
            REQUIRE(info->action != XR_NULL_HANDLE);
            REQUIRE(info->subactionPath != XR_NULL_PATH);
            REQUIRE(info->poseInActionSpace.orientation.w == 1.0F);
            const auto result = Script::Record("create-action-space");
            if (XR_SUCCEEDED(result))
                *space = NativeHandle<XrSpace>(300 + ++active->tracking.spaces);
            return result;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL DestroySpace(XrSpace) {
            const auto result = Script::Record("destroy-space");
            if (XR_SUCCEEDED(result))
                --active->tracking.spaces;
            return result;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL LocateSpace(XrSpace source, XrSpace base, XrTime time, XrSpaceLocation *location) {
            REQUIRE(source != base);
            active->tracking.locatedTime = time;
            location->locationFlags = active->tracking.flags;
            const float invalid = std::numeric_limits<float>::quiet_NaN();
            location->pose.position = {invalid, invalid, invalid};
            location->pose.orientation = {invalid, invalid, invalid, invalid};
            if ((location->locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0)
                location->pose.position = {1.0F, 2.0F, 3.0F};
            if ((location->locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0)
                location->pose.orientation = {0.0F, 0.0F, 0.0F, 1.0F};
            auto *velocity = static_cast<XrSpaceVelocity *>(location->next);
            velocity->velocityFlags = active->tracking.velocityFlags;
            velocity->linearVelocity = {4.0F, 0.0F, 0.0F};
            velocity->angularVelocity = {0.0F, 5.0F, 0.0F};
            if (active->sample.reentrantOwner)
                active->sample.closeRejected = active->sample.reentrantOwner->Close().HasError();
            return Script::Record("locate-space");
        }

        static XRAPI_ATTR XrResult XRAPI_CALL GetProc(XrInstance instance, const char *name, PFN_xrVoidFunction *function) {
            if (Script::active->absent == name) {
                *function = nullptr;
                return XR_ERROR_FUNCTION_UNSUPPORTED;
            }

            struct Entry {
                std::string_view name;
                PFN_xrVoidFunction function;
            };

            const std::array entries{Entry{"xrStringToPath", reinterpret_cast<PFN_xrVoidFunction>(&StringToPath)},
                                     Entry{"xrCreateActionSet", reinterpret_cast<PFN_xrVoidFunction>(&CreateSet)},
                                     Entry{"xrDestroyActionSet", reinterpret_cast<PFN_xrVoidFunction>(&DestroySet)},
                                     Entry{"xrCreateAction", reinterpret_cast<PFN_xrVoidFunction>(&CreateAction)},
                                     Entry{"xrDestroyAction", reinterpret_cast<PFN_xrVoidFunction>(&DestroyAction)},
                                     Entry{"xrSuggestInteractionProfileBindings", reinterpret_cast<PFN_xrVoidFunction>(&Suggest)},
                                     Entry{"xrAttachSessionActionSets", reinterpret_cast<PFN_xrVoidFunction>(&Attach)},
                                     Entry{"xrGetCurrentInteractionProfile", reinterpret_cast<PFN_xrVoidFunction>(&Profile)},
                                     Entry{"xrSyncActions", reinterpret_cast<PFN_xrVoidFunction>(&Sync)},
                                     Entry{"xrGetActionStateBoolean", std::bit_cast<PFN_xrVoidFunction>(&BooleanState)},
                                     Entry{"xrGetActionStateFloat", std::bit_cast<PFN_xrVoidFunction>(&FloatState)},
                                     Entry{"xrGetActionStateVector2f", std::bit_cast<PFN_xrVoidFunction>(&VectorState)},
                                     Entry{"xrGetActionStatePose", std::bit_cast<PFN_xrVoidFunction>(&PoseState)},
                                     Entry{"xrCreateReferenceSpace", std::bit_cast<PFN_xrVoidFunction>(&ReferenceSpace)},
                                     Entry{"xrCreateActionSpace", std::bit_cast<PFN_xrVoidFunction>(&ActionSpace)},
                                     Entry{"xrDestroySpace", std::bit_cast<PFN_xrVoidFunction>(&DestroySpace)},
                                     Entry{"xrLocateSpace", std::bit_cast<PFN_xrVoidFunction>(&LocateSpace)}};
            const auto found = std::ranges::find(entries, std::string_view{name}, &Entry::name);
            if (found == entries.end())
                return Script::GetProc(instance, name, function);
            *function = found->function;
            return XR_SUCCESS;
        }

        XrPath Find(std::string_view name) const {
            const auto found = std::ranges::find(paths, name, [](const auto &entry) {
                return std::string_view{entry.first};
            });
            REQUIRE(found != paths.end());
            return found->second;
        }
    };

    struct ActionFixture final {
        Fixture session;
        ActionScript script;
        OpenXRActionBindings owner{session.owner};
        std::array<Input::ActionDescriptor, 1> registered{Input::ActionDescriptor{Input::ActionId{"select"},
                                                                                  Input::ActionValueType::Digital,
                                                                                  Input::InputContextId{"gameplay"},
                                                                                  true,
                                                                                  {}}};
        std::array<XRActionDeclaration, 1> actions{XRActionDeclaration{Input::ActionId{"select"},
                                                                       Input::InputContextId{"gameplay"},
                                                                       Input::ActionValueType::Digital,
                                                                       {1},
                                                                       XRTrackedDeviceRole::LeftController,
                                                                       true}};
        std::array<XRProfileControl, 2>
            controls{XRProfileControl{{1}, {1}, XRTrackedDeviceRole::LeftController, Input::ActionValueType::Digital},
                     XRProfileControl{{2}, {1}, XRTrackedDeviceRole::LeftController, Input::ActionValueType::Digital}};
        std::array<XRSuggestedActionBinding, 2> suggestions{XRSuggestedActionBinding{Input::ActionId{"select"}, {1}, {1}},
                                                            XRSuggestedActionBinding{Input::ActionId{"select"}, {2}, {1}}};
        std::array<NativeInteractionProfilePath, 2>
            profilePaths{NativeInteractionProfilePath{{1}, "/interaction_profiles/khr/simple_controller"},
                         NativeInteractionProfilePath{{2}, "/interaction_profiles/oculus/touch_controller"}};
        std::array<NativeActionControlPath, 2>
            controlPaths{NativeActionControlPath{{1}, {1}, XRTrackedDeviceRole::LeftController, "/user/hand/left/input/select/click"},
                         NativeActionControlPath{{2}, {1}, XRTrackedDeviceRole::LeftController, "/user/hand/left/input/x/click"}};

        explicit ActionFixture(const bool activate = true) {
            if (activate) {
                REQUIRE(session.owner.Create(session.Request()).HasValue());
                return;
            }
            const auto &preflight = session.preflight;
            const NativeDiscoveryRequest discovery{.selection = {.contractVersion = preflight.ContractVersion(),
                                                                 .attempt = preflight.Attempt(),
                                                                 .backend = preflight.Backend(),
                                                                 .installRecord = preflight.InstallRecord(),
                                                                 .productProfile = preflight.ProductProfile(),
                                                                 .admittedLoaderVersions = {{1, 0, 0}, {1, 1, 63}}},
                                                   .requestedApi = preflight.LoaderApiVersion()};
            REQUIRE(session.owner.PrepareDiscovery(discovery).HasValue());
            REQUIRE(session.owner.PrepareSession({preflight, preflight.Attempt()}).HasValue());
        }

        XRResolvedActionBindings Plan() const {
            const std::array profiles{XRActiveInteractionProfile{XRTrackedDeviceRole::LeftController, {}},
                                      XRActiveInteractionProfile{XRTrackedDeviceRole::RightController, {}}};
            auto plan = ResolveXRActionBindings({1, actions, suggestions, {}, {1}, registered}, controls, profiles);
            REQUIRE(plan.HasValue());
            return std::move(plan).Value();
        }

        XRTrackedDeviceRecord Device() const {
            XRTrackedDeviceRecord record{.id = {session.Request().candidate, {1, 1}},
                                         .role = XRTrackedDeviceRole::LeftController,
                                         .state = XRTrackedDeviceState::TrackingLost};
            record.capabilities.values[static_cast<std::size_t>(XRTrackedDeviceCapability::GripPose)] = true;
            return record;
        }

        Result<void> Create() {
            const std::array labels{NativeActionLabel{Input::ActionId{"select"}, "Select"}};
            const std::array setLabels{NativeActionSetLabel{{1}, "Gameplay"}};
            return owner.Create(session.Request().candidate, {&ActionScript::GetProc,
                                                              {1, actions, suggestions, {}, {1}, registered},
                                                              controls,
                                                              {},
                                                              profilePaths,
                                                              controlPaths,
                                                              {labels, setLabels}});
        }

        Result<void> Prepare(const std::uint32_t maximumActions = 1) {
            const std::array labels{NativeActionLabel{Input::ActionId{"select"}, "Select"}};
            const std::array setLabels{NativeActionSetLabel{{1}, "Gameplay"}};
            return owner.PrepareAttempt({session.preflight, session.preflight.Attempt()}, maximumActions,
                                        {&ActionScript::GetProc,
                                         {1, actions, suggestions, {}, {1}, registered},
                                         controls,
                                         {},
                                         profilePaths,
                                         controlPaths,
                                         {labels, setLabels}});
        }
    };

}  // namespace Horo::Tests::OpenXRAction
