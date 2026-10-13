#include "Horo/PCG/PCGCpuEvaluator.h"
#include "Horo/PCG/PCGNodeCatalog.h"
#include "Horo/PCG/PCGPointFiltering.h"
#include "Horo/PCG/PCGPointGeneration.h"
#include "Horo/PCG/PCGProvenance.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::PCG::PCGNodeCatalog>);
static_assert(std::is_nothrow_move_constructible_v<Horo::PCG::PCGNodeCatalog>);
static_assert(std::is_copy_constructible_v<Horo::PCG::PCGNodeCatalogSnapshot>);
static_assert(std::is_same_v<decltype(&Horo::PCG::DerivePCGSampleSeed),
                             Horo::Result<std::uint64_t> (*)(const Horo::Sha256Digest &, Horo::PCG::SourceSampleId)>);
static_assert(
    std::is_same_v<decltype(std::declval<const Horo::PCG::PCGCookedPlan &>().Catalog()), const Horo::PCG::PCGNodeCatalogSnapshot &>);

static_assert(std::variant_size_v<Horo::PCG::PCGPointFilterSettings> == 5);
static_assert(std::variant_size_v<Horo::PCG::PCGAttributePredicateValue> == 7);
