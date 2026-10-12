#pragma once
#include "Horo/PCG/PCGNodeCatalog.h"

namespace Horo::PCG::detail {
    using BuiltInExecution = Result<void> (*)(const NodeExecutionContext &, std::uint32_t);
    [[nodiscard]] BuiltInExecution BuiltInFunction(PCGCpuNodeKind kind) noexcept;
}  // namespace Horo::PCG::detail

namespace Horo::PCG {
    struct PCGNodeCatalogSnapshot::State final {
        std::uint64_t generation{1};
        PCGCapabilityProjection capabilities;
        std::vector<PCGBuiltInNodeDescriptor> descriptors;
        std::vector<detail::BuiltInExecution> functions;
    };
}  // namespace Horo::PCG
