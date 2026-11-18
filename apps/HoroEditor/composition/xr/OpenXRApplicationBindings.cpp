#include "OpenXRApplicationBindings.h"

#include <algorithm>

namespace Horo::Application::Internal {
    using namespace XR;
    using namespace XR::OpenXRInternal;

    namespace {
        /** @brief Fixed lifecycle scratch; every spelling borrows the retained immutable installer catalog for this call only. */
        struct NativeBindingViews final {
            std::array<XRProfileControl, XRActionBindingLimits::MaximumBindings> identities{};
            std::array<NativeActionControlPath, XRActionBindingLimits::MaximumBindings> controls{};
            std::array<NativeInteractionProfilePath, XRActionBindingLimits::MaximumProfiles> profiles{};
            std::array<NativeActionLabel, XRActionBindingLimits::MaximumActions> labels{};
            std::array<NativeActionSetLabel, XRActionBindingLimits::MaximumSets> sets{};
            std::array<NativePoseActionPath, 4> poses{};
            std::array<NativeHapticActionPath, NativeHapticActionPath::MaximumRoutes> haptics{};

            /** @brief Copies only bounded views after the caller has validated every collection size. */
            explicit NativeBindingViews(const OpenXRApplicationBindings &source) {
                for (std::size_t index = 0; index < source.controls.size(); ++index) {
                    const auto &entry = source.controls[index];
                    identities[index] = entry.identity;
                    controls[index] = {entry.identity.profile, entry.identity.control, entry.identity.role, entry.path};
                }
                for (std::size_t index = 0; index < source.profiles.size(); ++index)
                    profiles[index] = {source.profiles[index].id, source.profiles[index].path};
                for (std::size_t index = 0; index < source.actionLabels.size(); ++index)
                    labels[index] = {source.actionLabels[index].action, source.actionLabels[index].text};
                for (std::size_t index = 0; index < source.setLabels.size(); ++index)
                    sets[index] = {source.setLabels[index].set, source.setLabels[index].text};
                for (std::size_t index = 0; index < source.poses.size(); ++index) {
                    const auto &entry = source.poses[index];
                    poses[index] = {entry.profile, entry.set, entry.role, entry.kind, entry.path, entry.label};
                }
                for (std::size_t index = 0; index < source.haptics.size(); ++index) {
                    const auto &entry = source.haptics[index];
                    haptics[index] = {entry.profile, entry.set, entry.role, entry.path, entry.label};
                }
            }
        };

        /** @brief Rejects unbounded declarations before constructing native view arrays or acquiring any child. */
        bool CountsFit(const OpenXRApplicationBindings &source) noexcept {
            return source.profiles.size() <= XRActionBindingLimits::MaximumProfiles &&
                   source.controls.size() <= XRActionBindingLimits::MaximumBindings &&
                   source.actionLabels.size() <= XRActionBindingLimits::MaximumActions &&
                   source.setLabels.size() <= XRActionBindingLimits::MaximumSets && source.poses.size() <= 4 &&
                   source.haptics.size() <= NativeHapticActionPath::MaximumRoutes &&
                   source.PreparedActionCount() <= XRActionBindingLimits::MaximumActions;
        }

        /** @brief Joins the immutable product declaration to the current canonical map. */
        XRActionBindingSchema DeclaredSchema(const OpenXRApplicationActionSchema &schema,
                                             const std::span<const Input::ActionDescriptor> registeredActions) noexcept {
            return {schema.version, schema.actions, schema.suggestions, schema.migrations, schema.genericFallback, registeredActions};
        }
    }  // namespace

    /** @copydoc OpenXRApplicationBindings::PreparedActionCount */
    std::size_t OpenXRApplicationBindings::PreparedActionCount() const noexcept {
        constexpr std::array roles{XRTrackedDeviceRole::LeftController, XRTrackedDeviceRole::RightController};
        const auto outputCount = std::ranges::count_if(roles, [this](const auto role) {
            return std::ranges::find(haptics, role, &OpenXRApplicationHaptic::role) != haptics.end();
        });
        return schema.actions.size() + poses.size() + static_cast<std::size_t>(outputCount);
    }

    /** @copydoc OpenXRApplicationBindings::ValidateRegisteredActions */
    Result<void> OpenXRApplicationBindings::ValidateRegisteredActions(
        const std::span<const Input::ActionDescriptor> registeredActions) const {
        if (!CountsFit(*this))
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        const NativeBindingViews views{*this};
        return ValidateXRActionBindings(DeclaredSchema(schema, registeredActions), std::span{views.identities}.first(controls.size()),
                                        schema.overrides);
    }

    /** @copydoc OpenXRApplicationBindings::Prepare */
    Result<XRResolvedActionBindings> OpenXRApplicationBindings::Prepare(OpenXRActionBindings &native,
                                                                        const NativeSessionPreparation &preparation,
                                                                        const OpenXRApplicationActionDispatch dispatch,
                                                                        const std::span<const Input::ActionDescriptor> registeredActions,
                                                                        const NativeTrackingSpaceRequest &tracking) const {
        using Outcome = Result<XRResolvedActionBindings>;
        if (dispatch.getProc == nullptr)
            return Outcome::Failure(MakeError(XRErrors::IdentityInvalid));
        if (!CountsFit(*this))
            return Outcome::Failure(MakeError(XRErrors::CapacityExceeded));
        const NativeBindingViews views{*this};
        const auto declared = DeclaredSchema(schema, registeredActions);
        const auto catalog = std::span{views.identities}.first(controls.size());
        if (auto valid = ValidateXRActionBindings(declared, catalog, schema.overrides); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        const NativeActionBindingRequest request{dispatch.getProc,
                                                 declared,
                                                 catalog,
                                                 schema.overrides,
                                                 std::span{views.profiles}.first(profiles.size()),
                                                 std::span{views.controls}.first(controls.size()),
                                                 {std::span{views.labels}.first(actionLabels.size()),
                                                  std::span{views.sets}.first(setLabels.size())},
                                                 std::span{views.poses}.first(poses.size()),
                                                 tracking,
                                                 std::span{views.haptics}.first(haptics.size())};
        const auto actionCount = PreparedActionCount() + static_cast<std::size_t>(tracking.consentedGazeSet.has_value());
        if (actionCount > XRActionBindingLimits::MaximumActions)
            return Outcome::Failure(MakeError(XRErrors::CapacityExceeded));
        const auto maximumActions = static_cast<std::uint32_t>(actionCount);
        if (auto prepared = native.PrepareAttempt(preparation, maximumActions, request); prepared.HasError())
            return Outcome::Failure(prepared.ErrorValue());
        auto observed = native.PreparedProfiles(preparation);
        if (observed.HasError())
            return Outcome::Failure(observed.ErrorValue());
        return ResolveXRActionBindings(declared, catalog, observed.Value(), schema.overrides);
    }

    /** @copydoc OpenXRApplicationBindings::Publish */
    Result<void> OpenXRApplicationBindings::Publish(XRActionBindingCoordinator &coordinator, const XRSessionId &session,
                                                    std::span<const Input::ActionDescriptor> registeredActions,
                                                    std::span<const XRActiveInteractionProfile> observed) const {
        if (!CountsFit(*this))
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        const NativeBindingViews views{*this};
        const auto declared = DeclaredSchema(schema, registeredActions);
        return coordinator.Update(session, declared, std::span{views.identities}.first(controls.size()), observed, schema.overrides);
    }
}  // namespace Horo::Application::Internal
