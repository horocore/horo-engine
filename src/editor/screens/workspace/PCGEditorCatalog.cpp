#include "Horo/PCG/PCGPointFiltering.h"
#include "Horo/PCG/PCGPointGeneration.h"
#include "Horo/PCG/PCGPointTransformation.h"
#include "editor/screens/workspace/PCGWorkspace.h"

namespace Horo::Editor {
    namespace {
        /** @brief Creates canonical initial settings through their owning codecs; no canvas payload parser exists. */
        Result<std::vector<std::uint8_t>> InitialSettings(PCG::PCGCpuNodeKind kind) {
            using namespace PCG;
            using enum PCGCpuNodeKind;
            // These are authored defaults, not provider availability or preview admission.
            const auto spatial = SpatialElementId::Create(1).Value();
            switch (kind) {
                case SnapshotGrid:
                    return EncodePCGPointGenerationSettings(PCGGridGenerationSettings{spatial});
                case GridJitter:
                    return EncodePCGPointGenerationSettings(PCGJitterGenerationSettings{spatial});
                case Stratified:
                    return EncodePCGPointGenerationSettings(PCGStratifiedGenerationSettings{spatial});
                case PoissonScatter:
                    return EncodePCGPointGenerationSettings(PCGPoissonGenerationSettings{.volume = spatial});
                case DensityRangeFilter:
                    return EncodePCGPointFilterSettings(PCGDensityRangeFilterSettings{});
                case BoundsFilter:
                    return EncodePCGPointFilterSettings(PCGBoundsFilterSettings{{{-1, -1, -1}, {1, 1, 1}}});
                case SlopeFilter:
                    return EncodePCGPointFilterSettings(PCGSlopeFilterSettings{});
                case DistanceFilter:
                    return EncodePCGPointFilterSettings(PCGDistanceFilterSettings{});
                case AttributeFilter:
                    return EncodePCGPointFilterSettings(PCGAttributeFilterSettings{PCGAttributeKey::Create("pcg.density").Value(),
                                                                                   PCGAttributeComparison::GreaterEqual, 0.0});
                case Transform:
                    return EncodePCGPointTransformationSettings(PCGPointTransformSettings{});
                case SurfaceAlignment:
                    return EncodePCGPointTransformationSettings(PCGPointAlignmentSettings{});
                case RandomOffset:
                    return EncodePCGPointTransformationSettings(PCGPointRandomOffsetSettings{});
                case PerlinNoise:
                case SimplexNoise:
                    return EncodePCGPointTransformationSettings(
                        PCGPointNoiseSettings{.attribute = PCGAttributeKey::Create("pcg.noise").Value()});
                case DensityFilter:
                case Merge:
                case Forward:
                    return Result<std::vector<std::uint8_t>>::Success({});
            }
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(PCGDocumentError(PCGDocumentCommandFailureReason::Invalid)));
        }
    }  // namespace

    /** @copydoc CreatePCGEditorCatalog() */
    Result<PCGEditorCatalog> CreatePCGEditorCatalog() {
        const std::array grants{PCG::PCGCapability::Validation};
        return CreatePCGEditorCatalog(PCG::PCGCapabilitySet::Create(grants).Value());
    }

    /** @copydoc CreatePCGEditorCatalog(PCG::PCGCapabilitySet) */
    Result<PCGEditorCatalog> CreatePCGEditorCatalog(const PCG::PCGCapabilitySet grantedCapabilities) {
        using Return = Result<PCGEditorCatalog>;
        auto created = PCG::PCGNodeCatalog::Create({PCG::PCGHostProfile::Interactive, grantedCapabilities});
        if (created.HasError())
            return Return::Failure(created.ErrorValue());
        auto owner = std::move(created).Value();
        constexpr std::array keys{"grid",      "density_filter", "merge",  "forward", "jitter",   "stratified",
                                  "poisson",   "density_range",  "bounds", "slope",   "distance", "attribute",
                                  "transform", "alignment",      "offset", "perlin",  "simplex"};
        for (std::size_t index = 0; index < keys.size(); ++index) {
            const auto registered = owner.Register(static_cast<PCG::PCGCpuNodeKind>(index));
            if (registered.HasError())
                return Return::Failure(registered.ErrorValue());
        }
        const auto captured = owner.Snapshot();
        if (captured.HasError())
            return Return::Failure(captured.ErrorValue());
        PCGEditorCatalog result;
        result.schemas = captured.Value();

        result.document.revision = captured.Value().Generation();
        for (const auto &descriptor : captured.Value().Nodes()) {
            const auto payload = InitialSettings(descriptor.kind);
            if (payload.HasError())
                return Return::Failure(payload.ErrorValue());
            PCGPaletteEntry entry{descriptor.kind,
                                  {.type = descriptor.type, .version = descriptor.version, .payload = payload.Value()},
                                  std::string{"workspace.pcg.nodes."} + keys[static_cast<std::size_t>(descriptor.kind)]};
            for (std::size_t index = 0; index < descriptor.pinCount; ++index) {
                const auto &pin = descriptor.pins[index];
                entry.prototype.pins.push_back({PCG::PinId::Create(index + 1).Value(), pin.direction, pin.type, pin.cardinality, {}});
            }
            result.document.types.push_back({descriptor.type, descriptor.version, descriptor.version});
            result.palette.push_back(std::move(entry));
        }
        return Return::Success(std::move(result));
    }
}  // namespace Horo::Editor
