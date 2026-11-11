#include "Horo/PlatformServices/PlatformFriendsCoordinator.h"

#include <ostream>
#include <type_traits>

using namespace Horo::PlatformServices;
static_assert(!std::is_convertible_v<PlatformSocialSubjectHandle, PlatformSubjectHandle>);
static_assert(!std::is_constructible_v<PlatformSocialSubjectHandle, PlatformSubjectNonce>);
template <typename T>
concept StreamWritable = requires(std::ostream &stream, const T &value) { stream << value; };
static_assert(!StreamWritable<PlatformSocialSubjectHandle>);

int main() {
    const PlatformSocialSubjectHandle social;
    return social.IsValid() ? 1 : 0;
}
