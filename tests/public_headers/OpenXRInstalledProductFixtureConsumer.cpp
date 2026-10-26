#include "support/OpenXRInstalledProductTestFixture.h"

#include <type_traits>

// Parsing the actual inline fixture bodies checks their owning contracts; no loader is constructed or run.
static_assert(!std::is_copy_constructible_v<Horo::Tests::OpenXRProduct::InstalledProductFixture>);
