#include "Horo/Runtime/Scene/SceneIdentity.h"
static_assert(sizeof(Horo::Runtime::SceneRuntimeId) == sizeof(std::uint64_t));

int main() {
    return Horo::Runtime::SceneRuntimeId{42}.IsValid() ? 0 : 1;
}
