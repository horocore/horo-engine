#include "Horo/PlatformServices/PlatformProviderAdmission.h"

// Preserve the reverse include order as a public composition regression.
#include "Horo/Foundation/Platform.h"

#include <type_traits>

static_assert(std::is_constructible_v<Horo::HostPlatformServices, Horo::FileSystem &, Horo::Clock &, Horo::ProcessService &,
                                      Horo::UserDirectories &>);
static_assert(!std::is_copy_constructible_v<Horo::PlatformServices::PlatformProviderLifecycleHost>);
