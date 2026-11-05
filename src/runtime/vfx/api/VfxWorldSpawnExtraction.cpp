#include "VfxWorldCoordinates.h"
#include "VfxWorldSpawnInternal.h"

namespace Horo::Vfx {
    namespace {
        /** @brief Converts emitter-local coordinates through copied world placement without losing nearby large-world precision. */
        [[nodiscard]] Math::Vec3 Position(const VfxSpawnRequest &request, const Math::Vec3 local,
                                          const Math::WorldCoordinate64 &origin) noexcept {
            const auto scale = request.transform.scale * request.scale;
            const auto rotated = request.transform.orientation.Rotate({local.x * scale.x, local.y * scale.y, local.z * scale.z});
            return {Detail::VfxRelativeAxis(request.transform.position.X(), origin.X(), rotated.x),
                    Detail::VfxRelativeAxis(request.transform.position.Y(), origin.Y(), rotated.y),
                    Detail::VfxRelativeAxis(request.transform.position.Z(), origin.Z(), rotated.z)};
        }

        [[nodiscard]] Render::VfxBatchId Batch(const Detail::VfxSpawnAsset &asset, const Detail::VfxSpawnInstance &instance) noexcept {
            return {instance.instance.scope.Value(), instance.instance.slot, instance.instance.generation, asset.emitter.slot,
                    asset.emitter.generation};
        }

        /** @brief Rejects nonfinite transformed geometry before any frame becomes immutable/published. */
        [[nodiscard]] bool ValidInstance(const Math::Vec3 position, const Math::Vec2 size, const float rotation) noexcept {
            return Math::IsFinite(position) && Math::IsFinite(size) && size.x >= 0.0F && size.y >= 0.0F && std::isfinite(rotation);
        }
    }  // namespace

    bool VfxRenderExtractor::CopySpawnParticles(Detail::VfxSpawnInstance &instance, const CpuParticleExtractView &source,
                                                Render::RenderWorldFrameData &data, const Math::WorldCoordinate64 &origin) noexcept {
        const auto scale = instance.request.transform.scale * instance.request.scale;
        for (std::uint32_t particle = 0; particle < source.positionX.size(); ++particle) {
            const auto position =
                Position(instance.request, {source.positionX[particle], source.positionY[particle], source.positionZ[particle]}, origin);
            const Math::Vec2 size{source.sizeX[particle] * scale.x, source.sizeY[particle] * scale.y};
            if (!ValidInstance(position, size, source.rotation[particle]))
                return false;
            const auto identity = instance.simulation.HandleAtDenseIndex(particle);
            if (identity.HasError())
                return false;
            data.instances_.push_back(
                {identity.Value().particle.Value(), position, size, source.rotation[particle], source.packedColor[particle]});
        }
        return true;
    }

    bool VfxRenderExtractor::AppendSpawnEmitter(Detail::VfxSpawnAsset &asset, Detail::VfxSpawnInstance &instance,
                                                Render::RenderWorldFrameData &data, const Math::WorldCoordinate64 &origin,
                                                VfxExtractionReport &report) const noexcept {
        const auto source = instance.simulation.Extract();
        if (source.HasError())
            return false;
        const auto count = static_cast<std::uint32_t>(source.Value().positionX.size());
        if (count == 0)
            return true;
        if (report.admittedBatches == budget_.maximumBatches || count > budget_.maximumInstances - report.admittedInstances ||
            report.reservedDrawCalls == budget_.maximumDrawCalls) {
            ++report.omittedBatches;
            report.omittedInstances += count;
            if (report.firstOmitted.scene == 0)
                report.firstOmitted = Batch(asset, instance);
            report.status = VfxExtractionStatus::BudgetExceeded;
            return true;
        }
        const auto first = static_cast<std::uint32_t>(data.instances_.size());
        if (!CopySpawnParticles(instance, source.Value(), data, origin))
            return false;
        data.batches_.push_back({Batch(asset, instance),
                                 asset.material,
                                 {},
                                 Render::VfxParticlePass::Additive,
                                 Render::VfxPrimitiveTopology::CameraFacingBillboard,
                                 false,
                                 false,
                                 first,
                                 count});
        ++report.admittedBatches;
        report.admittedInstances += count;
        ++report.reservedDrawCalls;
        return true;
    }

    bool VfxWorld::AppendSpawnEmitters(Render::RenderWorldFrameData &data, const Math::WorldCoordinate64 &origin,
                                       VfxExtractionReport &report) const noexcept {
        if (!spawn_)
            return true;
        for (auto &asset : spawn_->assets)
            for (auto &instance : asset.instances)
                if (instance.live && !extractor_.AppendSpawnEmitter(asset, instance, data, origin, report))
                    return false;
        return true;
    }
}  // namespace Horo::Vfx
