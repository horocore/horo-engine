#include "host/OpenXRActionBindings.h"

#include <algorithm>
#include <bit>
#include <format>
#include <memory>
#include <new>

namespace Horo::XR::OpenXRInternal {
    namespace {
        /** @brief Preserves phase and native evidence while translating to Horo-owned error categories. */
        Error NativeFailure(const char *phase, const XrResult result) {
            const auto &descriptor = result == XR_ERROR_PATH_UNSUPPORTED || result == XR_ERROR_FUNCTION_UNSUPPORTED
                                         ? XRErrors::OperationUnsupported
                                         : XRErrors::OperationUnavailable;
            return MakeError(descriptor, std::format("OpenXR {} failed; native result={}", phase, static_cast<std::int32_t>(result)));
        }

        /** @brief Resolves only official instance-scoped functions before acquiring any action handle. */
        template <typename GetProc, typename Function>
        Result<void> Load(GetProc getProc, XrInstance instance, const char *name, Function &function) {
            PFN_xrVoidFunction raw{};
            if (const XrResult result = getProc(instance, name, &raw); XR_FAILED(result) || !raw)
                return Result<void>::Failure(NativeFailure(name, XR_FAILED(result) ? result : XR_ERROR_FUNCTION_UNSUPPORTED));
            function = std::bit_cast<Function>(raw);
            return Result<void>::Success();
        }

        /** @brief Validates bounded registered native paths without accepting arbitrary user-supplied runtime names. */
        bool Path(const std::string_view path, const std::string_view prefix) {
            return path.starts_with(prefix) && path.size() > prefix.size() && path.size() < XR_MAX_PATH_LENGTH &&
                   path.find('\0') == std::string_view::npos;
        }

        /** @brief Requires bounded host-localized presentation without deriving display text from implementation IDs. */
        Result<void> ValidateLabels(const XRActionBindingSchema &schema, const NativeActionLabels &labels) {
            if (labels.actions.size() != schema.actions.size() || labels.sets.empty() ||
                labels.sets.size() > XRActionBindingLimits::MaximumSets)
                return Result<void>::Failure(
                    MakeError(XRErrors::OperationInvalid, "Provide one localized native label for every action and set."));
            for (std::size_t index = 0; index < labels.sets.size(); ++index) {
                const auto &label = labels.sets[index];
                if (label.label.empty() || label.label.size() >= XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE ||
                    label.label.find('\0') != std::string_view::npos ||
                    std::ranges::find(schema.actions, label.set, &XRActionDeclaration::set) == schema.actions.end() ||
                    std::ranges::any_of(labels.sets.first(index), [&](const auto &previous) {
                    return previous.set == label.set || previous.label == label.label;
                }))
                    return Result<void>::Failure(
                        MakeError(XRErrors::OperationInvalid, "Native action-set labels must be bounded unique host-localized text."));
            }
            for (std::size_t index = 0; index < labels.actions.size(); ++index) {
                const auto &label = labels.actions[index];
                const auto action = std::ranges::find(schema.actions, label.action, &XRActionDeclaration::action);
                if (action == schema.actions.end() || label.label.empty() || label.label.size() >= XR_MAX_LOCALIZED_ACTION_NAME_SIZE ||
                    label.label.find('\0') != std::string_view::npos ||
                    std::ranges::find(labels.sets, action->set, &NativeActionSetLabel::set) == labels.sets.end())
                    return Result<void>::Failure(
                        MakeError(XRErrors::OperationInvalid,
                                  "Native action labels require registered action/set identities and bounded localized text."));
                if (std::ranges::any_of(labels.actions.first(index), [&](const auto &previous) {
                    const auto previousAction = std::ranges::find(schema.actions, previous.action, &XRActionDeclaration::action);
                    return previous.action == label.action || (previousAction->set == action->set && previous.label == label.label);
                }))
                    return Result<void>::Failure(
                        MakeError(XRErrors::OperationIncompatible, "Native action labels conflict within one action set."));
            }
            return Result<void>::Success();
        }

        /** @brief Checks native catalogs are a bijection with the accepted Horo control registry before native effects. */
        Result<void> ValidatePaths(const std::span<const XRProfileControl> catalog,
                                   const std::span<const NativeInteractionProfilePath> profiles,
                                   const std::span<const NativeActionControlPath> controls) {
            if (profiles.empty() || profiles.size() > XRActionBindingLimits::MaximumProfiles || controls.size() != catalog.size())
                return Result<void>::Failure(
                    MakeError(XRErrors::CapacityExceeded, "Native XR path catalog must exactly cover finite registered controls."));
            for (std::size_t index = 0; index < profiles.size(); ++index) {
                const auto &profile = profiles[index];
                if (!profile.profile.value || !Path(profile.path, "/interaction_profiles/") ||
                    std::ranges::find(catalog, profile.profile, &XRProfileControl::profile) == catalog.end() ||
                    std::ranges::any_of(profiles.first(index), [&](const auto &previous) {
                    return previous.profile == profile.profile || previous.path == profile.path;
                }))
                    return Result<void>::Failure(
                        MakeError(XRErrors::OperationInvalid, "Native XR profile paths must be unique registered catalog entries."));
            }
            for (std::size_t index = 0; index < controls.size(); ++index) {
                const auto &control = controls[index];
                const auto prefix =
                    control.role == XRTrackedDeviceRole::LeftController ? "/user/hand/left/input/" : "/user/hand/right/input/";
                if (!Path(control.path, prefix) ||
                    std::ranges::find(profiles, control.profile, &NativeInteractionProfilePath::profile) == profiles.end() ||
                    std::ranges::none_of(catalog,
                                         [&](const auto &registered) {
                    return registered.profile == control.profile && registered.control == control.control &&
                           registered.role == control.role;
                }) ||
                    std::ranges::any_of(controls.first(index), [&](const auto &previous) {
                    return previous.profile == control.profile &&
                           ((previous.control == control.control && previous.role == control.role) || previous.path == control.path);
                }))
                    return Result<void>::Failure(
                        MakeError(XRErrors::OperationInvalid,
                                  "Native XR component paths require unique exact profile/control/role mappings."));
            }
            return Result<void>::Success();
        }

        /** @brief Maps the admitted existing Input value vocabulary to native action types privately. */
        XrActionType NativeType(Input::ActionValueType type) {
            using enum Input::ActionValueType;
            switch (type) {
                case Digital:
                    return XR_ACTION_TYPE_BOOLEAN_INPUT;
                case Axis1D:
                    return XR_ACTION_TYPE_FLOAT_INPUT;
                case Axis2D:
                    return XR_ACTION_TYPE_VECTOR2F_INPUT;
            }
            return XR_ACTION_TYPE_MAX_ENUM;
        }
    }  // namespace

    /** @copydoc OpenXRActionBindings::OpenXRActionBindings */
    OpenXRActionBindings::OpenXRActionBindings(OpenXRNativeSession &session) noexcept : session_(&session) {}

    /** @copydoc OpenXRActionBindings::~OpenXRActionBindings */
    OpenXRActionBindings::~OpenXRActionBindings() {
        const auto retired = Close();
        HORO_INVARIANT_MSG(retired.HasValue(), "OpenXR action owner requires successful retirement before destruction");
    }

    /** @brief Resolves complete native action lifecycle dispatch before acquiring handles. */
    Result<void> OpenXRActionBindings::Resolve(const NativeActionBindingRequest &request, XrInstance instance) {
        if (!request.getProc)
            return Result<void>::Failure(MakeError(XRErrors::LoaderIncompatible));
        if (auto result = Load(request.getProc, instance, "xrDestroyActionSet", dispatch_.destroySet); result.HasError())
            return result;
        if (auto result = Load(request.getProc, instance, "xrDestroyAction", dispatch_.destroyAction); result.HasError())
            return result;
        if (auto result = Load(request.getProc, instance, "xrStringToPath", dispatch_.stringToPath); result.HasError())
            return result;
        if (auto result = Load(request.getProc, instance, "xrCreateActionSet", dispatch_.createSet); result.HasError())
            return result;
        if (auto result = Load(request.getProc, instance, "xrCreateAction", dispatch_.createAction); result.HasError())
            return result;
        if (auto result = Load(request.getProc, instance, "xrSuggestInteractionProfileBindings", dispatch_.suggest); result.HasError())
            return result;
        if (auto result = Load(request.getProc, instance, "xrAttachSessionActionSets", dispatch_.attach); result.HasError())
            return result;
        if (auto result = Load(request.getProc, instance, "xrGetCurrentInteractionProfile", dispatch_.profile); result.HasError())
            return result;
        return Load(request.getProc, instance, "xrSyncActions", dispatch_.sync);
    }

    /** @brief Creates canonical action sets/actions with generated native names, never user semantic strings. */
    Result<void> OpenXRActionBindings::CreateActions(const XRActionBindingSchema &schema, const NativeActionLabels &labels) {
        std::vector<const XRActionDeclaration *> ordered;
        ordered.reserve(schema.actions.size());
        for (const auto &action : schema.actions) {
            ordered.push_back(&action);
            if (std::ranges::find(sets_, action.set, &Set::id) == sets_.end())
                sets_.push_back({action.set, XR_NULL_HANDLE});
        }
        std::ranges::sort(sets_, {}, &Set::id);
        std::ranges::sort(ordered, {}, [](const auto *action) -> const std::string & {
            return action->action.Value();
        });
        for (auto &set : sets_) {
            XrActionSetCreateInfo info{XR_TYPE_ACTION_SET_CREATE_INFO};
            const auto name = std::format("horo_set_{}", set.id.value);
            std::ranges::copy(name, info.actionSetName);
            const auto label = std::ranges::find(labels.sets, set.id, &NativeActionSetLabel::set);
            std::ranges::copy(label->label, info.localizedActionSetName);
            const auto result = dispatch_.createSet(native_.instance, &info, &set.native);
            if (XR_FAILED(result) || set.native == XR_NULL_HANDLE)
                return Result<void>::Failure(NativeFailure("create action set", XR_FAILED(result) ? result : XR_ERROR_RUNTIME_FAILURE));
        }
        for (const auto *action : ordered) {
            const auto set = std::ranges::find(sets_, action->set, &Set::id);
            const std::size_t role = action->role == XRTrackedDeviceRole::LeftController ? 0 : 1;
            XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
            const auto name = std::format("horo_action_{}", actions_.size());
            std::ranges::copy(name, info.actionName);
            const auto label = std::ranges::find(labels.actions, action->action, &NativeActionLabel::action);
            std::ranges::copy(label->label, info.localizedActionName);
            info.actionType = NativeType(action->valueType);
            info.countSubactionPaths = 1;
            info.subactionPaths = &hands_[role];
            actions_.emplace_back(action->action, XR_NULL_HANDLE);
            const auto result = dispatch_.createAction(set->native, &info, &actions_.back().native);
            if (XR_FAILED(result) || actions_.back().native == XR_NULL_HANDLE)
                return Result<void>::Failure(NativeFailure("create action", XR_FAILED(result) ? result : XR_ERROR_RUNTIME_FAILURE));
        }
        return Result<void>::Success();
    }

    /** @brief Suggests each admitted profile in deterministic ID/action order with exact registered component paths. */
    Result<void> OpenXRActionBindings::Suggest(const XRActionBindingSchema &schema, const std::span<const XRProfileControl> catalog,
                                               const std::span<const XRActionBindingOverride> overrides,
                                               const std::span<const NativeInteractionProfilePath> profiles,
                                               const std::span<const NativeActionControlPath> controls) {
        for (auto &profile : profiles_) {
            const auto source = std::ranges::find(profiles, profile.id, &NativeInteractionProfilePath::profile);
            const std::string path{source->path};
            auto result = dispatch_.stringToPath(native_.instance, path.c_str(), &profile.native);
            if (XR_FAILED(result) || profile.native == XR_NULL_PATH)
                return Result<void>::Failure(
                    NativeFailure("resolve interaction profile path", XR_FAILED(result) ? result : XR_ERROR_PATH_INVALID));
            const std::array active{XRActiveInteractionProfile{XRTrackedDeviceRole::LeftController, profile.id},
                                    XRActiveInteractionProfile{XRTrackedDeviceRole::RightController, profile.id}};
            auto resolved = ResolveXRActionBindings(schema, catalog, active, overrides);
            if (resolved.HasError())
                return Result<void>::Failure(resolved.ErrorValue());
            std::array<XrActionSuggestedBinding, XRActionBindingLimits::MaximumActions> suggestions{};
            std::uint32_t count = 0;
            for (const auto &binding : resolved.Value().bindings) {
                if (binding.profile != profile.id)
                    continue;
                const auto control = std::ranges::find_if(controls, [&](const auto &candidate) {
                    return candidate.profile == binding.profile && candidate.control == binding.control && candidate.role == binding.role;
                });
                const auto action = std::ranges::find(actions_, binding.action, &Action::id);
                const std::string component{control->path};
                XrPath nativePath{};
                result = dispatch_.stringToPath(native_.instance, component.c_str(), &nativePath);
                if (XR_FAILED(result) || nativePath == XR_NULL_PATH)
                    return Result<void>::Failure(NativeFailure("resolve control path", XR_FAILED(result) ? result : XR_ERROR_PATH_INVALID));
                suggestions[count++] = {action->native, nativePath};
            }
            if (count == 0)
                continue;
            const XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr, profile.native, count,
                                                            suggestions.data()};
            result = dispatch_.suggest(native_.instance, &info);
            if (XR_FAILED(result))
                return Result<void>::Failure(NativeFailure("suggest bindings", result));
        }
        return Result<void>::Success();
    }

    /** @brief Prevalidates all profile plans, then creates, suggests and attaches the native candidate. */
    Result<void> OpenXRActionBindings::Prepare(const XRActionBindingSchema &schema, const std::span<const XRProfileControl> catalog,
                                               const std::span<const XRActionBindingOverride> overrides,
                                               const std::span<const NativeInteractionProfilePath> profiles,
                                               const std::span<const NativeActionControlPath> controls, const NativeActionLabels &labels) {
        if (auto valid = ValidateXRActionBindings(schema, catalog, overrides); valid.HasError())
            return valid;
        if (schema.actions.size() > native_.maximumActions)
            return Result<void>::Failure(
                MakeError(XRErrors::CapacityExceeded, "XR action schema exceeds this session's negotiated action capacity."));
        if (auto valid = ValidatePaths(catalog, profiles, controls); valid.HasError())
            return valid;
        if (auto valid = ValidateLabels(schema, labels); valid.HasError())
            return valid;
        for (const auto &profile : profiles) {
            const std::array active{XRActiveInteractionProfile{XRTrackedDeviceRole::LeftController, profile.profile},
                                    XRActiveInteractionProfile{XRTrackedDeviceRole::RightController, profile.profile}};
            if (auto result = ResolveXRActionBindings(schema, catalog, active, overrides); result.HasError())
                return Result<void>::Failure(result.ErrorValue());
            profiles_.push_back({profile.profile, XR_NULL_PATH});
        }
        std::ranges::sort(profiles_, {}, &Profile::id);
        constexpr std::array hands{"/user/hand/left", "/user/hand/right"};
        for (std::size_t index = 0; index < hands.size(); ++index) {
            const auto result = dispatch_.stringToPath(native_.instance, hands[index], &hands_[index]);
            if (XR_FAILED(result) || hands_[index] == XR_NULL_PATH)
                return Result<void>::Failure(NativeFailure("resolve controller role", XR_FAILED(result) ? result : XR_ERROR_PATH_INVALID));
        }
        if (auto result = CreateActions(schema, labels); result.HasError())
            return result;
        if (auto result = Suggest(schema, catalog, overrides, profiles, controls); result.HasError())
            return result;
        if (auto valid = session_->Validate(owner_); valid.HasError())
            return valid;
        std::array<XrActionSet, XRActionBindingLimits::MaximumSets> sets{};
        std::ranges::transform(sets_, sets.begin(), &Set::native);
        const XrSessionActionSetsAttachInfo info{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, nullptr, static_cast<std::uint32_t>(sets_.size()),
                                                 sets.data()};
        const auto result = dispatch_.attach(native_.session, &info);
        if (XR_FAILED(result))
            return Result<void>::Failure(NativeFailure("attach action sets", result));
        attachedSession_ = owner_;
        return session_->Validate(owner_);
    }

    /** @copydoc OpenXRActionBindings::Create */
    Result<void> OpenXRActionBindings::Create(const XRSessionId &session, const NativeActionBindingRequest &request) {
        if (owner_.IsValid() || session == attachedSession_)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible,
                                                   "OpenXR actions attach once per native session; close this owner and replace "
                                                   "the session before changing its action schema."));
        const auto borrowed = session_->Borrow(session);
        if (borrowed.HasError())
            return Result<void>::Failure(borrowed.ErrorValue());
        native_ = borrowed.Value();
        owner_ = session;
        const auto retire = [](OpenXRActionBindings *owner) {
            (void)owner->Close();
        };
        // Unsupported exceptions unwind through retirement without translating their identity.
        std::unique_ptr<OpenXRActionBindings, decltype(retire)> rollback{this, retire};
        const auto preparationFailure = [] {
            return Result<void>::Failure(
                MakeError(XRErrors::OperationUnavailable, "OpenXR action preparation failed; rollback requested."));
        };
        Result<void> prepared = Result<void>::Success();
        try {
            prepared = Resolve(request, native_.instance);
            if (prepared.HasValue())
                prepared = Prepare(request.schema, request.catalog, request.overrides, request.profiles, request.controls, request.labels);
        } catch (const std::bad_alloc &) {
            prepared = preparationFailure();
        } catch (const std::format_error &) {
            prepared = preparationFailure();
        }
        (void)rollback.release();
        if (prepared.HasError()) {
            const auto retired = Close();
            return retired.HasError() ? retired : prepared;
        }
        active_ = true;
        return Result<void>::Success();
    }

    /** @brief Fences retained identity and complete action publication before native access. */
    Result<NativeSessionBorrow> OpenXRActionBindings::Current(const XRSessionId &session) const {
        if (!active_)
            return Result<NativeSessionBorrow>::Failure(MakeError(XRErrors::OperationUnavailable));
        if (auto valid = ValidateXRSession(session, owner_); valid.HasError())
            return Result<NativeSessionBorrow>::Failure(valid.ErrorValue());
        return session_->Borrow(session);
    }

    /** @copydoc OpenXRActionBindings::Profiles */
    Result<std::array<XRActiveInteractionProfile, 2>> OpenXRActionBindings::Profiles(const XRSessionId &session) const {
        const auto native = Current(session);
        using Profiles = std::array<XRActiveInteractionProfile, 2>;
        if (native.HasError())
            return Result<Profiles>::Failure(native.ErrorValue());
        Profiles profiles{XRActiveInteractionProfile{XRTrackedDeviceRole::LeftController, {}},
                          XRActiveInteractionProfile{XRTrackedDeviceRole::RightController, {}}};
        for (std::size_t index = 0; index < profiles.size(); ++index) {
            XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
            const auto result = dispatch_.profile(native.Value().session, hands_[index], &state);
            if (XR_FAILED(result))
                return Result<Profiles>::Failure(NativeFailure("query interaction profile", result));
            if (state.type != XR_TYPE_INTERACTION_PROFILE_STATE || state.next != nullptr)
                return Result<Profiles>::Failure(
                    MakeError(XRErrors::OperationInvalid, "OpenXR interaction-profile publication is malformed."));
            const auto profile = std::ranges::find(profiles_, state.interactionProfile, &Profile::native);
            if (profile != profiles_.end())
                profiles[index].profile = profile->id;
        }
        if (auto valid = session_->Validate(session); valid.HasError())
            return Result<Profiles>::Failure(valid.ErrorValue());
        return Result<Profiles>::Success(profiles);
    }

    /** @copydoc OpenXRActionBindings::Sync */
    Result<void> OpenXRActionBindings::Sync(const XRSessionId &session) const {
        const auto native = Current(session);
        if (native.HasError())
            return Result<void>::Failure(native.ErrorValue());
        std::array<XrActiveActionSet, XRActionBindingLimits::MaximumSets> sets{};
        std::ranges::transform(sets_, sets.begin(), [](const Set &set) {
            return XrActiveActionSet{set.native, XR_NULL_PATH};
        });
        const XrActionsSyncInfo info{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, static_cast<std::uint32_t>(sets_.size()), sets.data()};
        if (const auto result = dispatch_.sync(native.Value().session, &info); result != XR_SUCCESS)
            return Result<void>::Failure(NativeFailure("synchronize action sets", result));
        return session_->Validate(session);
    }

    /** @copydoc OpenXRActionBindings::Close */
    Result<void> OpenXRActionBindings::Close() {
        active_ = false;
        for (auto action = actions_.rbegin(); action != actions_.rend(); ++action) {
            if (action->native == XR_NULL_HANDLE)
                continue;
            const auto result = dispatch_.destroyAction(action->native);
            if (XR_FAILED(result))
                return Result<void>::Failure(NativeFailure("destroy action", result));
            action->native = XR_NULL_HANDLE;
        }
        for (auto set = sets_.rbegin(); set != sets_.rend(); ++set) {
            if (set->native == XR_NULL_HANDLE)
                continue;
            const auto result = dispatch_.destroySet(set->native);
            if (XR_FAILED(result))
                return Result<void>::Failure(NativeFailure("destroy action set", result));
            set->native = XR_NULL_HANDLE;
        }
        actions_.clear();
        sets_.clear();
        profiles_.clear();
        hands_ = {};
        owner_ = {};
        native_ = {};
        dispatch_ = {};
        return Result<void>::Success();
    }
}  // namespace Horo::XR::OpenXRInternal
