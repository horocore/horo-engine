#include "Horo/Foundation/Platform.h"

#include <utility>

int main() {
    Horo::ExclusiveFileLock empty;
    if (empty || empty.ProtectsPath({}))
        return 1;
    Horo::ExclusiveFileLock moved{std::exchange(empty, Horo::ExclusiveFileLock{})};
    if (empty || moved || empty.ProtectsPath({}) || moved.ProtectsPath({}))
        return 2;
    empty = std::exchange(moved, Horo::ExclusiveFileLock{});
    return empty || moved || empty.ProtectsPath({}) || moved.ProtectsPath({}) ? 3 : 0;
}
