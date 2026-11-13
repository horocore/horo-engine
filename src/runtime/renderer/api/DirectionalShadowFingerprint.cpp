#include "DirectionalShadowInternal.h"

#include <bit>
#include <string_view>

namespace Horo::Render::DirectionalShadowDetail {
    namespace {
        class Signature final {
        public:
            void Integer(std::uint64_t value) noexcept {
                for (unsigned byte = 0; byte < 8; ++byte) {
                    value_ ^= (value >> (byte * 8U)) & 255U;
                    value_ *= 1099511628211ULL;
                }
            }

            void Float(float value) noexcept {
                Integer(std::bit_cast<std::uint32_t>(value == 0.0F ? 0.0F : value));
            }

            void Vector(Math::Vec3 value) noexcept {
                Float(value.x);
                Float(value.y);
                Float(value.z);
            }

            void Text(std::string_view value) noexcept {
                Integer(value.size());
                for (unsigned char byte : value)
                    Integer(byte);
            }

            [[nodiscard]] std::uint64_t Value() const noexcept {
                return value_ == 0 ? 1 : value_;
            }

        private:
            std::uint64_t value_{14695981039346656037ULL};
        };

        void Profile(Signature &hash, const DirectionalShadowProfile &profile) noexcept {
            hash.Integer(profile.enabled);
            hash.Integer(profile.cascadeCount);
            hash.Integer(profile.tileResolution);
            hash.Float(profile.maximumDistance);
            hash.Integer(static_cast<unsigned>(profile.splitMode));
            hash.Float(profile.splitLambda);
            hash.Float(profile.blendFraction);
            hash.Float(profile.depthBias);
            hash.Float(profile.slopeBias);
            hash.Float(profile.normalBias);
            hash.Integer(profile.filterRadius);
            hash.Integer(profile.stabilize);
        }

        void Camera(Signature &hash, const RenderCameraView &camera) noexcept {
            hash.Vector(camera.position);
            hash.Vector(camera.target);
            hash.Vector(camera.up);
            hash.Integer(static_cast<unsigned>(camera.projection.kind));
            hash.Float(camera.projection.verticalFovRadians);
            hash.Float(camera.projection.orthographicHeight);
            hash.Float(camera.projection.nearPlane);
            hash.Float(camera.projection.farPlane);
        }

        void Casters(Signature &hash, const RenderSceneView &scene) noexcept {
            hash.Integer(scene.instances.size());
            for (const auto &instance : scene.instances) {
                hash.Integer(instance.mesh.id.value);
                hash.Integer(instance.mesh.generation);
                for (const auto value : instance.localToWorld.values)
                    hash.Float(value);
                hash.Vector(instance.localBounds.minimum);
                hash.Vector(instance.localBounds.maximum);
                hash.Text(instance.material.value);
            }
            hash.Integer(scene.meshResources.size());
            for (const auto &mesh : scene.meshResources) {
                hash.Integer(mesh.handle.id.value);
                hash.Integer(mesh.handle.generation);
                hash.Integer(mesh.vertices.size());
                hash.Integer(mesh.indices.size());
            }
        }

        bool GeometryFits(const RenderSceneView &scene) noexcept {
            std::size_t remaining = 4U * 1024U * 1024U;
            for (const auto &mesh : scene.meshResources) {
                if (mesh.vertices.size() > remaining / sizeof(MeshVertex))
                    return false;
                remaining -= mesh.vertices.size() * sizeof(MeshVertex);
                if (mesh.indices.size() > remaining / sizeof(std::uint32_t))
                    return false;
                remaining -= mesh.indices.size() * sizeof(std::uint32_t);
            }
            for (const auto &instance : scene.instances)
                if (std::ranges::find(scene.meshResources, instance.mesh, &RenderMeshResourceView::handle) == scene.meshResources.end())
                    return false;
            return true;
        }

        void Geometry(Signature &hash, const RenderSceneView &scene) noexcept {
            for (const auto &mesh : scene.meshResources) {
                for (const auto &vertex : mesh.vertices) {
                    hash.Vector(vertex.position);
                    hash.Vector(vertex.normal);
                    hash.Float(vertex.uv.x);
                    hash.Float(vertex.uv.y);
                }
                for (const auto index : mesh.indices)
                    hash.Integer(index);
                hash.Vector(mesh.localBounds.minimum);
                hash.Vector(mesh.localBounds.maximum);
            }
        }
    }  // namespace

    std::uint64_t Fingerprint(const RenderSceneView &scene, float aspect, Math::ClipDepthRange depthRange,
                              DirectionalShadowFrame &frame) noexcept {
        Signature hash;
        Camera(hash, scene.camera);
        Profile(hash, frame.profile);
        Casters(hash, scene);
        frame.cacheable = GeometryFits(scene);
        if (frame.cacheable)
            Geometry(hash, scene);
        hash.Float(aspect);
        hash.Integer(static_cast<unsigned>(depthRange));
        hash.Integer(frame.lightIndex);
        const auto &light = scene.lights[frame.lightIndex];
        hash.Vector(light.direction);
        hash.Vector(light.position);
        hash.Vector(light.color);
        hash.Float(light.intensity);
        return hash.Value();
    }
}  // namespace Horo::Render::DirectionalShadowDetail
