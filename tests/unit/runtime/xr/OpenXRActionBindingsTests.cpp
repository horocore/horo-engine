#include "OpenXRActionBindings.h"
#include "support/OpenXRNativeTestSupport.h"

#include <catch2/matchers/catch_matchers_exception.hpp>
#include <format>
#include <new>
#include <stdexcept>

namespace {
    using namespace Horo;
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
            REQUIRE(info->actionType == XR_ACTION_TYPE_BOOLEAN_INPUT);
            REQUIRE(std::string_view{info->localizedActionName} == "Select");
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
            const auto result = Script::Record("destroy-action");
            if (XR_SUCCEEDED(result))
                --active->liveActions;
            return result;
        }

        static XRAPI_ATTR XrResult XRAPI_CALL Suggest(XrInstance, const XrInteractionProfileSuggestedBinding *info) {
            REQUIRE(info->type == XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING);
            REQUIRE(info->interactionProfile != XR_NULL_PATH);
            REQUIRE(info->countSuggestedBindings == 1);
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
                                     Entry{"xrSyncActions", reinterpret_cast<PFN_xrVoidFunction>(&Sync)}};
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

        ActionFixture() {
            REQUIRE(session.owner.Create(session.Request()).HasValue());
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
    };

    struct Neutralizer final : IXRBindingNeutralizer {
        unsigned calls{};

        void Neutralize(const XRSessionId &, std::uint64_t) noexcept override {
            ++calls;
        }
    };

    TEST_CASE("OpenXR action ownership creates suggests attaches synchronizes and retires deterministically", "[xr][openxr][actions]") {
        ActionFixture fixture;
        REQUIRE(fixture.Create().HasValue());
        REQUIRE(fixture.script.liveSets == 1);
        REQUIRE(fixture.script.liveActions == 1);
        REQUIRE(fixture.script.suggestions == 2);
        REQUIRE(fixture.script.attaches == 1);
        REQUIRE(fixture.owner.Sync(fixture.session.Request().candidate).HasValue());
        REQUIRE(fixture.script.syncs == 1);
        REQUIRE(fixture.owner.Close().HasValue());
        REQUIRE(fixture.owner.Close().HasValue());
        REQUIRE(fixture.script.liveSets == 0);
        REQUIRE(fixture.script.liveActions == 0);
        REQUIRE(fixture.owner.Sync(fixture.session.Request().candidate).HasError());
        REQUIRE(fixture.Create().HasError());  // OpenXR permits only one attachment per native session.
    }

    TEST_CASE("OpenXR active profile changes join the Input neutral boundary without exposing native paths", "[xr][openxr][actions]") {
        ActionFixture fixture;
        REQUIRE(fixture.Create().HasValue());
        Neutralizer neutralizer;
        XRActionBindingCoordinator coordinator{fixture.session.Request().candidate, neutralizer};
        fixture.script.currentProfiles[0] = fixture.script.Find("/interaction_profiles/oculus/touch_controller");
        auto profiles = fixture.owner.Profiles(fixture.session.Request().candidate);
        REQUIRE(profiles.HasValue());
        REQUIRE(profiles.Value()[0].profile == XRInteractionProfileId{2});
        REQUIRE(coordinator
                    .Update(fixture.session.Request().candidate, {1, fixture.actions, fixture.suggestions, {}, {1}, fixture.registered},
                            fixture.controls, profiles.Value())
                    .HasValue());
        fixture.script.currentProfiles[0] = 9999;
        profiles = fixture.owner.Profiles(fixture.session.Request().candidate);
        REQUIRE(profiles.Value()[0].profile == XRInteractionProfileId{});
        REQUIRE(coordinator
                    .Update(fixture.session.Request().candidate, {1, fixture.actions, fixture.suggestions, {}, {1}, fixture.registered},
                            fixture.controls, profiles.Value())
                    .HasValue());
        REQUIRE(neutralizer.calls == 1);
        REQUIRE(coordinator.Current()->bindings[0].fallback);
        coordinator.Shutdown();
        REQUIRE(neutralizer.calls == 2);
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR native action preparation rolls back every failed phase before publication", "[xr][openxr][actions]") {
        ActionFixture fixture;
        SECTION("set creation") {
            fixture.session.script.fail = "create-set";
        }
        SECTION("action creation") {
            fixture.session.script.fail = "create-action";
        }
        SECTION("suggestion") {
            fixture.session.script.fail = "suggest";
        }
        SECTION("attachment") {
            fixture.session.script.fail = "attach";
        }
        REQUIRE(fixture.Create().HasError());
        REQUIRE(fixture.script.liveSets == 0);
        REQUIRE(fixture.script.liveActions == 0);
        REQUIRE(fixture.owner.Profiles(fixture.session.Request().candidate).HasError());
        REQUIRE(fixture.owner.Close().HasValue());
    }

    TEST_CASE("OpenXR rejects invalid paths missing dispatch stale generations and unfocused synchronization", "[xr][openxr][actions]") {
        ActionFixture fixture;
        SECTION("wrong-hand component path") {
            fixture.controlPaths[0].path = "/user/hand/right/input/select/click";
            REQUIRE(fixture.Create().HasError());
            REQUIRE(fixture.script.liveSets == 0);
        }
        SECTION("missing destroy dispatch") {
            fixture.session.script.absent = "xrDestroyAction";
            REQUIRE(fixture.Create().HasError());
            REQUIRE(fixture.script.liveSets == 0);
        }
        SECTION("stale generation") {
            REQUIRE(fixture.Create().HasValue());
            auto stale = fixture.session.Request().candidate;
            ++stale.slot.generation;
            REQUIRE(fixture.owner.Sync(stale).HasError());
            REQUIRE(fixture.script.syncs == 0);
        }
        SECTION("focus loss is unavailable, never success") {
            REQUIRE(fixture.Create().HasValue());
            fixture.session.script.fail = "sync";
            fixture.session.script.failureResult = XR_SESSION_NOT_FOCUSED;
            REQUIRE(fixture.owner.Sync(fixture.session.Request().candidate).HasError());
        }
    }

    TEST_CASE("OpenXR action callback exceptions roll back acquired ownership before returning or propagating", "[xr][native][lifecycle]") {
        ActionFixture fixture;
        SECTION("supported format errors become typed preparation failure") {
            fixture.script.throwFormat = true;
            const auto result = fixture.Create();
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == "xr.operation.unavailable");
        }
        SECTION("supported allocation errors become typed preparation failure") {
            fixture.script.throwAllocation = true;
            const auto result = fixture.Create();
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == "xr.operation.unavailable");
        }
        SECTION("unrelated runtime exceptions preserve type and message") {
            fixture.script.throwRuntime = true;
            REQUIRE_THROWS_MATCHES(fixture.Create(), std::runtime_error,
                                   Catch::Matchers::Message("unsupported scripted runtime exception"));
        }
        SECTION("unsupported standard exceptions preserve type and message") {
            fixture.script.throwUnsupportedStandard = true;
            REQUIRE_THROWS_MATCHES(fixture.Create(), std::logic_error,
                                   Catch::Matchers::Message("unsupported scripted preparation exception"));
        }
        SECTION("foreign callback exceptions preserve their original identity") {
            fixture.script.throwForeign = true;
            REQUIRE_THROWS_AS(fixture.Create(), std::uint32_t);
        }
        REQUIRE(fixture.script.liveActions == 0);
        REQUIRE(fixture.script.liveSets == 0);
        REQUIRE(fixture.script.attaches == 0);
        const auto sync = fixture.owner.Sync(fixture.session.Request().candidate);
        REQUIRE(sync.HasError());
        REQUIRE(sync.ErrorValue().code.Value() == "xr.operation.unavailable");
    }

    TEST_CASE("OpenXR failed action retirement closes admission and retains handles for explicit retry", "[xr][openxr][actions]") {
        ActionFixture fixture;
        REQUIRE(fixture.Create().HasValue());
        fixture.session.script.fail = "destroy-action";
        REQUIRE(fixture.owner.Close().HasError());
        REQUIRE(fixture.script.liveActions == 1);
        REQUIRE(fixture.owner.Sync(fixture.session.Request().candidate).HasError());
        fixture.session.script.fail = {};
        REQUIRE(fixture.owner.Close().HasValue());
        REQUIRE(fixture.script.liveActions == 0);
        REQUIRE(fixture.script.liveSets == 0);
    }

    TEST_CASE("OpenXR foreign preparation exceptions preserve retryable retirement failure", "[xr][native][lifecycle]") {
        ActionFixture fixture;
        fixture.script.throwForeign = true;
        fixture.session.script.fail = "destroy-action";
        REQUIRE_THROWS_AS(fixture.Create(), std::uint32_t);
        REQUIRE(fixture.script.liveActions == 1);
        REQUIRE(fixture.script.liveSets == 1);
        REQUIRE(fixture.script.attaches == 0);
        REQUIRE(fixture.owner.Sync(fixture.session.Request().candidate).HasError());
        fixture.session.script.fail = {};
        REQUIRE(fixture.owner.Close().HasValue());
        REQUIRE(fixture.script.liveActions == 0);
        REQUIRE(fixture.script.liveSets == 0);
    }
}  // namespace
