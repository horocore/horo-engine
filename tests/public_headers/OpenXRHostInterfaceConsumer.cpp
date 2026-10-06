#include "OpenXRHostBindings.h"
#include "OpenXRNativeNames.h"
#include "OpenXRNativeSession.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::XR::OpenXRInternal::OpenXRNativeSession>);
static_assert(std::is_trivially_copyable_v<Horo::XR::OpenXRInternal::NativeSessionBorrow>);
static_assert([] {
    char exact[3]{};
    Horo::XR::OpenXRInternal::CopyNativeLiteral(exact, "ab");
    return exact[0] == 'a' && exact[1] == 'b' && exact[2] == '\0';
}());
static_assert([] {
    char larger[4]{'x', 'x', 'x', 'z'};
    Horo::XR::OpenXRInternal::CopyNativeLiteral(larger, "ab");
    return larger[2] == '\0' && larger[3] == 'z';
}());

// This deliberately native, non-installed host consumer gets only the dedicated
// interface include directory, never a repository-wide src usage requirement.
Horo::Result<Horo::XR::OpenXRInternal::NativeSessionBorrow> BorrowComposedNativeSession(
    const Horo::XR::OpenXRInternal::OpenXRNativeSession &owner, const Horo::XR::XRSessionId &session) {
    return owner.Borrow(session);
}
