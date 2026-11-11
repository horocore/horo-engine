#include "Horo/Debug/CommandRegistry.h"

#include <array>

int main() {
    using namespace Horo::Debug;
    constexpr std::array commands{DebugCommandDescriptor{0x1000, "sample.inspect", "Inspect sample", "sample.inspect"}};
    RegistryPolicy policy;
    policy.projectNamespaces = {"sample"};
    auto registry = CommandRegistry::Create(commands, policy);
    if (registry.HasError())
        return 1;
    return registry.Value().Prepare("sample.inspect", {3}).HasValue() ? 0 : 2;
}
