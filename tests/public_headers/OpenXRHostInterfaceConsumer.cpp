#include "OpenXRHostBindings.h"
#include "OpenXRNativeSession.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::XR::OpenXRInternal::OpenXRNativeSession>);
static_assert(std::is_trivially_copyable_v<Horo::XR::OpenXRInternal::NativeSessionBorrow>);

// This deliberately native, non-installed host consumer gets only the dedicated
// interface include directory, never a repository-wide src usage requirement.
Horo::Result<Horo::XR::OpenXRInternal::NativeSessionBorrow> BorrowComposedNativeSession(
    const Horo::XR::OpenXRInternal::OpenXRNativeSession &owner, const Horo::XR::XRSessionId &session) {
    return owner.Borrow(session);
}
