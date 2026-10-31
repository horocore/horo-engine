#include "Horo/Runtime/Save/RuntimeSaveSession.h"

#include <type_traits>
static_assert(!std::is_copy_constructible_v<Horo::Runtime::RuntimeSaveSession>);
static_assert(std::is_copy_constructible_v<Horo::Runtime::RuntimeSaveRequest>);

int main() {
    return 0;
}
