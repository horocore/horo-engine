#include "RenderFrameInputInternal.h"

#include "RenderParallelWorkErrors.h"
#include "RenderPayloadCopyInternal.h"

#include <algorithm>
#include <new>
#include <stdexcept>
#include <utility>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Accounts for arrays without multiplying until overflow has been excluded. */
        class CaptureBudget final {
        public:
            explicit CaptureBudget(const std::size_t maximum) noexcept : remaining_(maximum) {}

            [[nodiscard]] bool Charge(const std::size_t count, const std::size_t elementBytes) noexcept {
                if (count > remaining_ / elementBytes)
                    return false;
                remaining_ -= count * elementBytes;
                return true;
            }

            [[nodiscard]] std::size_t Remaining() const noexcept {
                return remaining_;
            }

        private:
            std::size_t remaining_;
        };

        [[nodiscard]] Result<void> CapacityFailure() {
            return Result<void>::Failure(MakeError(ParallelWorkErrors::CapacityExceeded));
        }

        /** @brief Charges mesh payloads after their metadata has passed admission. */
        [[nodiscard]] Result<void> ChargeMeshPayloads(const std::span<const RenderMeshResourceView> meshes, CaptureBudget &budget,
                                                      const CancellationToken &cancellation) {
            for (const RenderMeshResourceView &mesh : meshes) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(ParallelWorkErrors::Cancelled));
                if (!budget.Charge(mesh.vertices.size(), sizeof(MeshVertex)) || !budget.Charge(mesh.indices.size(), sizeof(std::uint32_t)))
                    return CapacityFailure();
            }
            return Result<void>::Success();
        }

        /** @brief Charges owned material bytes including each string terminator. */
        [[nodiscard]] Result<void> ChargeMaterialPayloads(const std::span<const RenderStaticMeshInstance> instances, CaptureBudget &budget,
                                                          const CancellationToken &cancellation) {
            for (const RenderStaticMeshInstance &instance : instances) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(ParallelWorkErrors::Cancelled));
                if (!budget.Charge(instance.material.value.size(), sizeof(char)) || !budget.Charge(1, sizeof(char)))
                    return CapacityFailure();
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> CheckSceneBudget(const RenderSceneView &scene, CaptureBudget &budget,
                                                    const CancellationToken &cancellation) {
            if (!budget.Charge(scene.meshResources.size(), sizeof(CapturedRenderMesh) + sizeof(RenderMeshResourceView)) ||
                !budget.Charge(scene.instances.size(), sizeof(RenderStaticMeshInstance) + sizeof(std::string)) ||
                !budget.Charge(scene.lights.size(), sizeof(RenderLight)))
                return CapacityFailure();
            if (const auto charged = ChargeMeshPayloads(scene.meshResources, budget, cancellation); charged.HasError())
                return charged;
            return ChargeMaterialPayloads(scene.instances, budget, cancellation);
        }

        [[nodiscard]] bool ValidPassIdentity(const RenderPassDescriptor &pass) noexcept {
            return pass.id.IsValid() && !(pass.primaryOutput && pass.staticMesh);
        }

        /** @brief Admits one pass before traversing any of its payload. */
        [[nodiscard]] Result<void> ChargePass(const RenderPassDescriptor &pass, CaptureBudget &budget,
                                              const CancellationToken &cancellation) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(ParallelWorkErrors::Cancelled));
            if (!ValidPassIdentity(pass))
                return Result<void>::Failure(MakeError(ParallelWorkErrors::InvalidPass));
            if (pass.staticMesh)
                return CheckSceneBudget(pass.staticMesh->scene, budget, cancellation);
            return Result<void>::Success();
        }

        /** @brief Checks all metadata and bytes before any source payload is allocated or traversed. */
        [[nodiscard]] Result<std::size_t> MeasureInputs(const std::span<const RenderPassDescriptor> passes,
                                                        const RenderFrameInputLimits &limits, const CancellationToken &cancellation) {
            CaptureBudget budget{limits.maximumBytes};
            if (passes.size() > limits.maximumPasses || !budget.Charge(1, sizeof(CapturedRenderFrame)) ||
                !budget.Charge(passes.size(), sizeof(CapturedRenderPass) + sizeof(RenderPassDescriptor) + sizeof(RenderPassId)))
                return Result<std::size_t>::Failure(MakeError(ParallelWorkErrors::CapacityExceeded));
            for (const RenderPassDescriptor &pass : passes) {
                const Result<void> admitted = ChargePass(pass, budget, cancellation);
                if (admitted.HasError())
                    return Result<std::size_t>::Failure(admitted.ErrorValue());
            }
            return Result<std::size_t>::Success(limits.maximumBytes - budget.Remaining());
        }

        /** @brief Owns material bytes with the same cooperative chunk bound as geometry. */
        [[nodiscard]] Result<void> CopyMaterial(std::string &destination, const std::string_view source,
                                                const CancellationToken &cancellation) {
            return CopyCapturedPayload(destination, std::span{source.data(), source.size()}, cancellation);
        }

        /** @brief Rebinds every borrowed span to its frozen owner without changing producer ordering. */
        [[nodiscard]] Result<CapturedRenderPass> CopyPass(const RenderPassDescriptor &source, const CancellationToken &cancellation) {
            using CopyResult = Result<CapturedRenderPass>;
            CapturedRenderPass captured;
            captured.descriptor = source;
            if (!source.staticMesh)
                return CopyResult::Success(std::move(captured));
            const RenderSceneView &scene = source.staticMesh->scene;
            captured.meshes.reserve(scene.meshResources.size());
            captured.resources.reserve(scene.meshResources.size());
            for (const RenderMeshResourceView &mesh : scene.meshResources) {
                CapturedRenderMesh storage;
                if (const auto copied = CopyCapturedPayload(storage.vertices, mesh.vertices, cancellation); copied.HasError())
                    return CopyResult::Failure(copied.ErrorValue());
                if (const auto copied = CopyCapturedPayload(storage.indices, mesh.indices, cancellation); copied.HasError())
                    return CopyResult::Failure(copied.ErrorValue());
                captured.meshes.push_back(std::move(storage));
                captured.resources.push_back({.handle = mesh.handle, .localBounds = mesh.localBounds});
            }
            if (const auto copied = CopyCapturedPayload(captured.instances, scene.instances, cancellation); copied.HasError())
                return CopyResult::Failure(copied.ErrorValue());
            captured.materials.reserve(scene.instances.size());
            for (const RenderStaticMeshInstance &instance : scene.instances) {
                captured.materials.emplace_back();
                if (const auto copied = CopyMaterial(captured.materials.back(), instance.material.value, cancellation); copied.HasError())
                    return CopyResult::Failure(copied.ErrorValue());
            }
            if (const auto copied = CopyCapturedPayload(captured.lights, scene.lights, cancellation); copied.HasError())
                return CopyResult::Failure(copied.ErrorValue());
            captured.Rebind();
            return CopyResult::Success(std::move(captured));
        }

        /** @brief Validates vertex values without an uninterruptible geometry traversal. */
        [[nodiscard]] Result<void> ValidateVertices(const std::span<const MeshVertex> vertices, const CancellationToken &cancellation) {
            for (const MeshVertex &vertex : vertices) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(ParallelWorkErrors::Cancelled));
                if (!Math::IsFinite(vertex.position) || !Math::IsFinite(vertex.normal) || !Math::IsFinite(vertex.uv))
                    return Result<void>::Failure(MakeError(ParallelWorkErrors::InvalidGeometry));
            }
            return Result<void>::Success();
        }

        /** @brief Validates bounded geometry while allowing shutdown to interrupt long mesh traversal. */
        [[nodiscard]] Result<void> ValidateMesh(const RenderMeshResourceView &mesh, const CancellationToken &cancellation) {
            if (!mesh.handle.IsValid() || !mesh.localBounds.IsValid() || mesh.vertices.empty() || mesh.indices.empty() ||
                mesh.indices.size() % 3 != 0)
                return Result<void>::Failure(MakeError(ParallelWorkErrors::InvalidGeometry));
            if (const auto valid = ValidateVertices(mesh.vertices, cancellation); valid.HasError())
                return valid;
            for (const std::uint32_t index : mesh.indices) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(ParallelWorkErrors::Cancelled));
                if (index >= mesh.vertices.size())
                    return Result<void>::Failure(MakeError(ParallelWorkErrors::InvalidGeometry));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateInstances(const std::span<const RenderStaticMeshInstance> instances,
                                                     const CancellationToken &cancellation) {
            for (const RenderStaticMeshInstance &instance : instances) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(ParallelWorkErrors::Cancelled));
                if (!instance.IsValid())
                    return Result<void>::Failure(MakeError(ParallelWorkErrors::InvalidGeometry));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateLights(const std::span<const RenderLight> lights, const CancellationToken &cancellation) {
            for (const RenderLight &light : lights) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(ParallelWorkErrors::Cancelled));
                if (!light.IsValid())
                    return Result<void>::Failure(MakeError(ParallelWorkErrors::InvalidGeometry));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateScene(const StaticMeshPassDescriptor &pass, const CancellationToken &cancellation) {
            const RenderSceneView &scene = pass.scene;
            if (!pass.target.IsValid() || !pass.extent.IsValid() || !pass.clearColor.IsFinite() || !scene.camera.IsValid() ||
                scene.lights.size() > MaximumForwardLights)
                return Result<void>::Failure(MakeError(ParallelWorkErrors::InvalidGeometry));
            for (const RenderMeshResourceView &mesh : scene.meshResources) {
                const Result<void> valid = ValidateMesh(mesh, cancellation);
                if (valid.HasError())
                    return valid;
            }
            if (const auto valid = ValidateInstances(scene.instances, cancellation); valid.HasError())
                return valid;
            return ValidateLights(scene.lights, cancellation);
        }

        [[nodiscard]] bool ValidCaptureLimits(const FrameToken frame, const RenderFrameInputLimits &limits) noexcept {
            return frame.IsValid() && limits.maximumPasses != 0 && limits.maximumBytes != 0 &&
                   limits.maximumPasses <= RenderParallelWorkLimits::HardMaximumPasses &&
                   limits.maximumBytes <= RenderParallelWorkLimits::HardMaximumBytes;
        }

        /** @brief Freezes an admitted pass set; duplicate identity and cancellation prohibit publication. */
        [[nodiscard]] Result<void> FreezePasses(CapturedRenderFrame &captured, const std::span<const RenderPassDescriptor> passes,
                                                const CancellationToken &cancellation) {
            captured.passes.reserve(passes.size());
            captured.sortedPassIds.reserve(passes.size());
            for (const RenderPassDescriptor &pass : passes)
                captured.sortedPassIds.push_back(pass.id);
            std::ranges::sort(captured.sortedPassIds);
            if (std::ranges::adjacent_find(captured.sortedPassIds) != captured.sortedPassIds.end())
                return Result<void>::Failure(MakeError(ParallelWorkErrors::InvalidPass));
            for (const RenderPassDescriptor &pass : passes) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(ParallelWorkErrors::Cancelled));
                auto copied = CopyPass(pass, cancellation);
                if (copied.HasError())
                    return Result<void>::Failure(copied.ErrorValue());
                captured.passes.push_back(std::move(copied).Value());
            }
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(ParallelWorkErrors::Cancelled));
            return Result<void>::Success();
        }

        [[nodiscard]] bool SupportedPassKind(const RenderPassKind kind) noexcept {
            switch (kind) {
                case RenderPassKind::Graphics:
                case RenderPassKind::Compute:
                case RenderPassKind::Copy:
                    return true;
                default:
                    return false;
            }
        }

        [[nodiscard]] bool ValidWorkloadKind(const RenderPassDescriptor &pass) noexcept {
            if (pass.primaryOutput)
                return pass.kind == RenderPassKind::Graphics && pass.primaryOutput->IsValid();
            return !pass.staticMesh || pass.kind == RenderPassKind::Graphics;
        }
    }  // namespace

    /** @copydoc CapturedRenderPass::CapturedRenderPass */
    CapturedRenderPass::CapturedRenderPass(CapturedRenderPass &&other) noexcept
        : descriptor(std::move(other.descriptor)), meshes(std::move(other.meshes)), resources(std::move(other.resources)),
          instances(std::move(other.instances)), materials(std::move(other.materials)), lights(std::move(other.lights)) {
        Rebind();
        other.descriptor.staticMesh.reset();
    }

    /** @copydoc CapturedRenderPass::operator= */
    CapturedRenderPass &CapturedRenderPass::operator=(CapturedRenderPass &&other) noexcept {
        if (this != &other) {
            descriptor = std::move(other.descriptor);
            meshes = std::move(other.meshes);
            resources = std::move(other.resources);
            instances = std::move(other.instances);
            materials = std::move(other.materials);
            lights = std::move(other.lights);
            Rebind();
            other.descriptor.staticMesh.reset();
        }
        return *this;
    }

    /** @copydoc CapturedRenderPass::Rebind */
    void CapturedRenderPass::Rebind() noexcept {
        if (!descriptor.staticMesh)
            return;
        for (std::size_t index = 0; index < resources.size(); ++index) {
            resources[index].vertices = meshes[index].vertices;
            resources[index].indices = meshes[index].indices;
        }
        descriptor.staticMesh->scene.meshResources = resources;
        for (std::size_t index = 0; index < instances.size(); ++index)
            instances[index].material.value = materials[index];
        descriptor.staticMesh->scene.instances = instances;
        descriptor.staticMesh->scene.lights = lights;
    }

    /** @copydoc CaptureRenderFrameInputs */
    Result<std::shared_ptr<const CapturedRenderFrame>> CaptureRenderFrameInputs(const FrameToken frame,
                                                                                const std::span<const RenderPassDescriptor> passes,
                                                                                const RenderFrameInputLimits &limits,
                                                                                const CancellationToken &cancellation) {
        using CaptureResult = Result<std::shared_ptr<const CapturedRenderFrame>>;
        if (!ValidCaptureLimits(frame, limits))
            return CaptureResult::Failure(MakeError(ParallelWorkErrors::InvalidLimits));
        const Result<std::size_t> measured = MeasureInputs(passes, limits, cancellation);
        if (measured.HasError())
            return CaptureResult::Failure(measured.ErrorValue());
        try {
            auto captured = std::make_shared<CapturedRenderFrame>();
            captured->frame = frame;
            captured->chargedBytes = measured.Value();
            if (const auto frozen = FreezePasses(*captured, passes, cancellation); frozen.HasError())
                return CaptureResult::Failure(frozen.ErrorValue());
            return CaptureResult::Success(std::move(captured));
        } catch (const std::bad_alloc &) {
            return CaptureResult::Failure(MakeError(ParallelWorkErrors::CaptureFailed));
        } catch (const std::length_error &) {
            return CaptureResult::Failure(MakeError(ParallelWorkErrors::InvalidLimits));
        }
    }

    /** @copydoc ValidateCapturedRenderPass */
    Result<void> ValidateCapturedRenderPass(const CapturedRenderPass &pass, const CancellationToken &cancellation) {
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(ParallelWorkErrors::Cancelled));
        const RenderPassDescriptor &descriptor = pass.descriptor;
        if (!ValidPassIdentity(descriptor))
            return Result<void>::Failure(MakeError(ParallelWorkErrors::InvalidPass));
        if (!SupportedPassKind(descriptor.kind) || !ValidWorkloadKind(descriptor))
            return Result<void>::Failure(MakeError(ParallelWorkErrors::InvalidPass));
        if (descriptor.staticMesh) {
            return ValidateScene(*descriptor.staticMesh, cancellation);
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Render::Detail
