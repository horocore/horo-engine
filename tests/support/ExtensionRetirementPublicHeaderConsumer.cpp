#include "Horo/Extensions/ExtensionRetirement.h"

#include <memory>
#include <type_traits>

static_assert(!std::is_default_constructible_v<Horo::Extensions::ExtensionExecutableLease::ConstructionKey>);
static_assert(!std::is_aggregate_v<Horo::Extensions::ExtensionExecutableLease::ConstructionKey>);
static_assert(!std::is_copy_constructible_v<Horo::Extensions::ExtensionExecutableLease>);
static_assert(!std::is_constructible_v<Horo::Extensions::ExtensionExecutableLease, std::shared_ptr<void>>);

int main() {
    using namespace Horo::Extensions;
    const ExtensionRetirement retirement{"consumer", {"consumer.native"}};
    auto code = std::make_shared<int>(0);
    if (!retirement.BindModuleCode("consumer.native", code))
        return 1;
    auto work = retirement.Acquire("consumer.native", ExtensionLeaseKind::Job, "consumer.job", code);
    if (!work)
        return 2;
    if (retirement.BeginRetirement().disposition != ExtensionRetirementDisposition::Draining)
        return 3;
    work.reset();
    return retirement.IsDrained() ? 0 : 4;
}
