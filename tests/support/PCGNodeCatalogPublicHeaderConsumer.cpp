#include "Horo/PCG/PCGCpuEvaluator.h"
#include "Horo/PCG/PCGNodeCatalog.h"
#include "Horo/PCG/PCGPointGeneration.h"
#include "Horo/PCG/PCGPointTransformation.h"
#include "Horo/PCG/PCGProvenance.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::PCG::PCGNodeCatalog>);
static_assert(std::is_nothrow_move_constructible_v<Horo::PCG::PCGNodeCatalog>);
static_assert(std::is_copy_constructible_v<Horo::PCG::PCGNodeCatalogSnapshot>);
static_assert(std::is_same_v<decltype(&Horo::PCG::DerivePCGSampleSeed),
                             Horo::Result<std::uint64_t> (*)(const Horo::Sha256Digest &, Horo::PCG::SourceSampleId)>);
static_assert(
    std::is_same_v<decltype(std::declval<const Horo::PCG::PCGCookedPlan &>().Catalog()), const Horo::PCG::PCGNodeCatalogSnapshot &>);

static_assert(
    std::is_same_v<decltype(Horo::PCG::EncodePCGPointTransformationSettings(std::declval<const Horo::PCG::PCGPointNoiseSettings &>())),
                   Horo::Result<std::vector<std::uint8_t>>>);
static_assert(static_cast<unsigned>(Horo::PCG::PCGCpuNodeKind::Transform) == 12);
