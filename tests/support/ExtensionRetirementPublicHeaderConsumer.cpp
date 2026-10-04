#include "Horo/Extensions/ExtensionRetirement.h"

#include <memory>

int main() {
    using namespace Horo::Extensions;
    ExtensionRetirement retirement{"consumer", {"consumer.native"}};
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
