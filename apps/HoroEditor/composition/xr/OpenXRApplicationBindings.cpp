#include "OpenXRApplicationBindings.h"

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
            }
        };

        /** @brief Rejects unbounded declarations before constructing native view arrays or acquiring any child. */
        bool CountsFit(const OpenXRApplicationBindings &source) noexcept {
            return source.profiles.size() <= XRActionBindingLimits::MaximumProfiles &&
                   source.controls.size() <= XRActionBindingLimits::MaximumBindings &&
                   source.actionLabels.size() <= XRActionBindingLimits::MaximumActions &&
                   source.setLabels.size() <= XRActionBindingLimits::MaximumSets && source.poses.size() <= 4 &&
                   source.schema.actions.size() <= XRActionBindingLimits::MaximumActions - source.poses.size();
        }
    }  // namespace

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
        const XRActionBindingSchema declared{schema.version,    schema.actions,         schema.suggestions,
                                             schema.migrations, schema.genericFallback, registeredActions};
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
                                                 tracking};
        const auto maximumActions = static_cast<std::uint32_t>(schema.actions.size() + poses.size());
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
        const XRActionBindingSchema declared{schema.version,    schema.actions,         schema.suggestions,
                                             schema.migrations, schema.genericFallback, registeredActions};
        return coordinator.Update(session, declared, std::span{views.identities}.first(controls.size()), observed, schema.overrides);
    }
}  // namespace Horo::Application::Internal
