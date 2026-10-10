#include "Horo/PCG/PCGCpuEvaluator.h"
#include "Horo/PCG/PCGNodeCatalog.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::PCG::PCGNodeCatalog>);
static_assert(std::is_nothrow_move_constructible_v<Horo::PCG::PCGNodeCatalog>);
static_assert(std::is_copy_constructible_v<Horo::PCG::PCGNodeCatalogSnapshot>);
static_assert(
    std::is_same_v<decltype(std::declval<const Horo::PCG::PCGCookedPlan &>().Catalog()), const Horo::PCG::PCGNodeCatalogSnapshot &>);
