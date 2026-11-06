#pragma once

/**
 * @file RenderScene.h
 * @brief Backend-neutral static-mesh scene submission contracts.
 */

#include "Horo/Runtime/Render/Mesh.h"
#include "Horo/Runtime/Render/RenderResource.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>

namespace Horo::Render {
    /** @brief Maximum punctual and directional lights accepted by the baseline forward viewport path. */
    inline constexpr std::size_t MaximumForwardLights = 16;

    /** @brief Complete immutable light-table envelope; per-draw Forward selection has its own smaller budget. */
    inline constexpr std::size_t MaximumSceneLights = 4096;

    /** @brief Backend-neutral light families supported by the baseline forward lighting path. */
    enum class RenderLightKind : std::uint8_t {
        Directional,
        Point,
        Spot,
    };

    /**
     * @brief One immutable world-space light value extracted for a render view.
     *
     * Direction points from the light into the scene. Spot cone values are stored
     * as cosines so concrete backends do not reinterpret authored angle units.
     */
    struct RenderLight {
        RenderLightKind kind{RenderLightKind::Directional};
        Math::Vec3 position{};
        Math::Vec3 direction{0.0F, 0.0F, -1.0F};
        Math::Vec3 color{1.0F, 1.0F, 1.0F}; /**< Canonical finite signed scene-linear ACEScg/AP1 RGB; never untagged source RGB. */
        float intensity{1.0F};
        float range{10.0F};
        float innerConeCosine{0.9396926F};
        float outerConeCosine{0.7071068F};
        /** @brief Producer-issued entity generation/index within the scene domain; zero denotes an anonymous legacy value.
         * Clustered/per-draw preparation requires a real nonzero identity before sorting or selection. */
        std::uint64_t stableIdentity{};

        /** @brief Reports finite signed canonical RGB, normalized pose and ordered physical bounds. @return Valid light. */
        [[nodiscard]] bool IsValid() const noexcept {
            using enum RenderLightKind;
            const bool kindValid = kind == Directional || kind == Point || kind == Spot;
            return kindValid && ValidPose() && ValidRadiometry() && ValidRangeAndCone();
        }

    private:
        /** @brief Checks finite world pose and normalized direction. @return Valid renderer pose. */
        [[nodiscard]] bool ValidPose() const noexcept {
            const float length = Math::Length(direction);
            return Math::IsFinite(position) && Math::IsFinite(direction) && std::isfinite(length) &&
                   Math::NearlyEqual(length, 1.0F, 0.001F);
        }

        /** @brief Preserves finite signed AP1 channels while retaining nonnegative physical intensity. @return Valid radiometric values. */
        [[nodiscard]] bool ValidRadiometry() const noexcept {
            return Math::IsFinite(color) && std::isfinite(intensity) && intensity >= 0.0F;
        }

        /** @brief Checks existing finite world range and ordered cone limits. @return Valid attenuation bounds. */
        [[nodiscard]] bool ValidRangeAndCone() const noexcept {
            return std::isfinite(range) && range >= 0.0F && std::isfinite(innerConeCosine) && std::isfinite(outerConeCosine) &&
                   innerConeCosine >= outerConeCosine && innerConeCosine <= 1.0F && outerConeCosine >= -1.0F;
        }
    };

    /** @brief Producer-owned identity of one synchronously borrowed CPU mesh source. */
    struct RenderMeshSourceHandle {
        MeshResourceId id;
        std::uint32_t generation{0};

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return id.IsValid() && generation != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderMeshSourceHandle &) const noexcept = default;
    };

    /** @brief Supported backend-neutral camera projection families. */
    enum class RenderProjectionKind : std::uint8_t {
        Perspective,
        Orthographic,
    };

    /** @brief Validated projection values converted to API clip depth by each backend. */
    struct RenderProjectionDescriptor {
        RenderProjectionKind kind{RenderProjectionKind::Perspective};
        float verticalFovRadians{0.9599311F};
        float orthographicHeight{8.0F};
        float nearPlane{0.1F};
        float farPlane{100.0F};

        [[nodiscard]] bool IsValid() const noexcept {
            if (const bool common = std::isfinite(nearPlane) && std::isfinite(farPlane) && nearPlane > 0.0F && farPlane > nearPlane;
                !common)
                return false;
            using enum RenderProjectionKind;
            if (kind == Perspective)
                return std::isfinite(verticalFovRadians) && verticalFovRadians > 0.0F && verticalFovRadians < std::numbers::pi_v<float>;
            return kind == Orthographic && std::isfinite(orthographicHeight) && orthographicHeight > 0.0F;
        }
    };

    /** @brief World-space camera values for one render view. */
    struct RenderCameraView {
        Math::Vec3 position{};
        Math::Vec3 target{0.0F, 0.0F, -1.0F};
        Math::Vec3 up{0.0F, 1.0F, 0.0F};
        RenderProjectionDescriptor projection{};

        [[nodiscard]] bool IsValid() const noexcept {
            return Math::IsFinite(position) && Math::IsFinite(target) && Math::IsFinite(up) &&
                   Math::Length(target - position) > Math::DefaultEpsilon && Math::Length(up) > Math::DefaultEpsilon &&
                   projection.IsValid();
        }
    };

    /** @brief Non-owning immutable CPU resource pinned by the producer's owning snapshot. */
    struct RenderMeshResourceView {
        RenderMeshSourceHandle handle;
        std::span<const MeshVertex> vertices;
        std::span<const std::uint32_t> indices;
        Math::Aabb localBounds;

        [[nodiscard]] bool IsValid() const noexcept {
            if (!handle.IsValid() || vertices.empty() || indices.empty() || indices.size() % 3 != 0 || !localBounds.IsValid())
                return false;
            if (const bool validVertices = std::ranges::all_of(vertices,
                                                               [](const MeshVertex &vertex) {
                return Math::IsFinite(vertex.position) && Math::IsFinite(vertex.normal) && Math::IsFinite(vertex.uv);
            });
                !validVertices)
                return false;

            return std::ranges::all_of(indices, [this](const std::uint32_t index) {
                return index < vertices.size();
            });
        }
    };

    /** @brief Generic per-instance presentation values resolved before backend execution. */
    struct RenderInstancePresentation {
        Math::Vec3 tint{1.0F, 1.0F, 1.0F};
        float tintStrength{0.0F};

        [[nodiscard]] bool IsValid() const noexcept {
            return Math::IsFinite(tint) && std::isfinite(tintStrength) && tintStrength >= 0.0F && tintStrength <= 1.0F;
        }
    };

    /** @brief One static-mesh draw instance with no authored-scene or editor identity. */
    struct RenderStaticMeshInstance {
        RenderMeshSourceHandle mesh;
        Math::Mat4 localToWorld{Math::Mat4::Identity()};
        Math::Aabb localBounds{{-0.5F, -0.5F, -0.5F}, {0.5F, 0.5F, 0.5F}};
        MaterialBindingId material{CoreDefaultMaterial};
        RenderInstancePresentation presentation{};

        [[nodiscard]] bool IsValid() const noexcept {
            return mesh.IsValid() && Math::IsFinite(localToWorld) && localBounds.IsValid() && material.IsValid() && presentation.IsValid();
        }
    };

    /** @brief Synchronous non-owning scene view consumed during one Execute call. */
    struct RenderSceneView {
        RenderCameraView camera;
        std::span<const RenderMeshResourceView> meshResources;
        std::span<const RenderStaticMeshInstance> instances;
        std::span<const RenderLight> lights;
        std::uint64_t lightSceneDomain{};      /**< Runtime scene identity of this complete table. */
        std::uint64_t lightSnapshotRevision{}; /**< Content-derived light revision, including parameter/transform previews. */

        [[nodiscard]] bool IsValid() const noexcept {
            if (!camera.IsValid() || lights.size() > MaximumSceneLights)
                return false;
            return std::ranges::all_of(meshResources, &RenderMeshResourceView::IsValid) &&
                   std::ranges::all_of(instances, &RenderStaticMeshInstance::IsValid) && std::ranges::all_of(lights, &RenderLight::IsValid);
        }
    };

}  // namespace Horo::Render
