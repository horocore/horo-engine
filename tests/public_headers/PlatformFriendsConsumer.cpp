#include "Horo/PlatformServices/PlatformFriendsCoordinator.h"
#include "Horo/PlatformServices/PlatformSessionBinding.h"

#include <ostream>
#include <type_traits>

using namespace Horo::PlatformServices;
static_assert(!std::is_convertible_v<PlatformSocialSubjectHandle, PlatformSubjectHandle>);
static_assert(!std::is_constructible_v<PlatformSocialSubjectHandle, PlatformSubjectNonce>);
template <typename T>
concept StreamWritable = requires(std::ostream &stream, const T &value) { stream << value; };
static_assert(!StreamWritable<PlatformSocialSubjectHandle>);
static_assert(!StreamWritable<ProviderAccountBindingId>);
static_assert(!StreamWritable<PlatformSessionBindingEvidence>);
static_assert(!std::is_convertible_v<ProviderAccountBindingId, PlatformSubjectHandle>);
static_assert(!std::is_copy_constructible_v<PlatformSessionBindingIssuer>);
static_assert(std::is_nothrow_move_constructible_v<PlatformSessionBindingIssuer>);

int main() {
    const PlatformSocialSubjectHandle social;
    const ProviderAccountBindingId binding;
    const PlatformSessionBindingEvidence evidence;
    return social.IsValid() || binding.IsValid() || evidence.IsValid() ? 1 : 0;
}
