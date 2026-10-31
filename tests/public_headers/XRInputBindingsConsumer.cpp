#include "Horo/XR/XRInputProjection.h"

#include <concepts>
#include <type_traits>

using Horo::XR::XRInputProjection;
static_assert(sizeof(Horo::XR::XRSimulationTime) == sizeof(std::int64_t));
static_assert(!Horo::XR::XRSimulationTime{}.IsValid());
static_assert(Horo::XR::XRSimulationTime{}.Nanoseconds() == -1);
static_assert(!Horo::XR::XRRuntimeTime{}.IsValid());
static_assert(!std::is_copy_constructible_v<XRInputProjection>);
static_assert(std::is_nothrow_move_constructible_v<XRInputProjection>);
static_assert(requires(const Horo::Input::InputActionCollection &port, const Horo::XR::XRResolvedActionBindings &plan,
                       XRInputProjection &projection, const Horo::XR::XRSessionId &candidate, std::uint64_t issuedRevision) {
    { XRInputProjection::Prepare(port, plan) } -> std::same_as<Horo::Result<XRInputProjection>>;
    { projection.Bind(candidate, issuedRevision) } -> std::same_as<Horo::Result<void>>;
    { XRInputProjection::Create(port, candidate, plan, issuedRevision) } -> std::same_as<Horo::Result<XRInputProjection>>;
});

static_assert(requires(const XRInputProjection &projection, const Horo::XR::XRTrackingSnapshot &snapshot,
                       const Horo::XR::XRSessionId &session, Horo::XR::XRTrackingSnapshotRevision revision,
                       Horo::XR::XRWorldOriginRevision origin, const Horo::Input::InputCollectionFrame &frame) {
    { projection.Append(snapshot, session, revision, origin, std::uint64_t{1}, frame) } -> std::same_as<Horo::Result<void>>;
});
