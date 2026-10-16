#include "Horo/Foundation/Platform.h"

#include <utility>

int main() {
    Horo::ExclusiveFileLock empty;
    if (empty || empty.ProtectsPath({}))
        return 1;
    Horo::ExclusiveFileLock moved{std::move(empty)};
    if (empty || moved || empty.ProtectsPath({}) || moved.ProtectsPath({}))
        return 2;
    empty = std::move(moved);
    return empty || moved || empty.ProtectsPath({}) || moved.ProtectsPath({}) ? 3 : 0;
}
