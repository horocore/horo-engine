#include "Horo/Physics/PhysicsCollisionCooker.h"

#include "Horo/Foundation/JobSystem.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsCollisionArtifactInternal.h"

#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace Horo::Physics {
    namespace {
        constexpr std::size_t MaximumCookSourceContextBytes = 256;

        struct CollisionCookOutput final {
            PhysicsCookedShapeDescriptor descriptor;
            std::vector<std::uint8_t> payload;
        };

        /** @brief Dispatches owned geometry to its qualified cooker without extending borrowed input lifetimes. */
        template <typename Geometry>
        auto CookGeometry(const Geometry &geometry, const PhysicsCollisionSource &input, const Assets::CookSourceView &source,
                          const PhysicsShapeCookTargetDigest &target, const CancellationToken &cancellation) {
            const auto context = source.sourceContext.substr(0, MaximumCookSourceContextBytes);
            if constexpr (std::is_same_v<Geometry, PhysicsCollisionConvexSource>) {
                return CookPhysicsConvexHull({source.id, input.subresource, geometry.vertices, geometry.settings, target, context},
                                             cancellation);
            } else if constexpr (std::is_same_v<Geometry, PhysicsCollisionMeshSource>) {
                return CookPhysicsTriangleMesh({source.id, input.subresource, geometry.vertices, geometry.triangles, geometry.materialSlots,
                                                geometry.settings, target, context},
                                               cancellation);
            } else {
                return CookPhysicsHeightField({source.id, input.subresource, geometry.width, geometry.height, geometry.origin,
                                               geometry.spacingX, geometry.spacingZ, geometry.sampleScaleY, geometry.samples,
                                               geometry.cellHoles, geometry.cellMaterials, geometry.materialSlots, geometry.settings,
                                               target, context},
                                              cancellation);
            }
        }

        /** @brief Runs one qualified cooker while the owned normalized source remains alive. */
        Result<CollisionCookOutput> CookNormalizedSource(const PhysicsCollisionSource &input, const Assets::CookSourceView &source,
                                                         const PhysicsShapeCookTargetDigest &target,
                                                         const CancellationToken &cancellation) {
            return std::visit([&](const auto &geometry) -> Result<CollisionCookOutput> {
                auto cooked = CookGeometry(geometry, input, source, target, cancellation);
                if (cooked.HasError())
                    return Result<CollisionCookOutput>::Failure(cooked.ErrorValue());
                auto output = std::move(cooked).Value();
                return Result<CollisionCookOutput>::Success({std::move(output.descriptor), std::move(output.payload)});
            }, input.geometry);
        }

        /** @brief Binds immutable importer semantics and exact Physics target to generic Assets cache identity. */
        Sha256Digest ConfigurationDigest(const Assets::CookerCacheIdentity &identity, const PhysicsShapeCookTargetDigest &target) {
            std::vector<std::uint8_t> bytes{'P', 'C', 'C', '1'};
            Detail::AppendCollisionInteger(bytes, identity.version.size(), 4);
            bytes.insert(bytes.end(), identity.version.begin(), identity.version.end());
            Detail::AppendCollisionInteger(bytes, identity.settingsSchemaVersion, 4);
            Detail::AppendCollisionDigest(bytes, identity.settingsDigest);
            Detail::AppendCollisionDigest(bytes, target.digest);
            return ComputeSha256(std::as_bytes(std::span{bytes}));
        }

        /** @brief Preserves the Physics cancellation cause while exposing the shared job cancellation category. */
        Error CookFailure(Error error) {
            if (error.domain.Value() == PhysicsErrors::ShapeCookCancelled.domain.Value() &&
                error.code.Value() == PhysicsErrors::ShapeCookCancelled.code.Value()) {
                Diagnostic finding{DiagnosticCode{error.code.Value()}, DiagnosticSeverity::Warning, error.message};
                auto findings = std::move(error.diagnostics);
                auto cancelled = JobCancelled(std::move(error));
                Error result = cancelled.ErrorValue();
                result.diagnostics = std::move(findings);
                result.diagnostics.push_back(std::move(finding));
                return result;
            }
            return error;
        }

        class CollisionCookerStrategy final : public Assets::ICookerStrategy {
        public:
            CollisionCookerStrategy(Assets::AssetTypeId type, AssetCookTargetId target, const PhysicsShapeCookTargetDigest &physicsTarget,
                                    std::shared_ptr<const IPhysicsCollisionSourceImporter> importer, const Sha256Digest &configuration)
                : type_(std::move(type)), target_(std::move(target)), physicsTarget_(physicsTarget), importer_(std::move(importer)),
                  configuration_(configuration) {}

            [[nodiscard]] Assets::CookerCacheIdentity CacheIdentity() const noexcept override {
                return {.version = "physics.collision.1", .settingsDigest = configuration_, .settingsSchemaVersion = 1};
            }

            [[nodiscard]] Result<void> ValidateCookedPayload(const Assets::CookSourceView &source,
                                                             const std::span<const std::uint8_t> payload) const override {
                if (source.type != type_ || source.target != target_)
                    return Result<void>::Failure(MakeError(PhysicsErrors::ProfileUnsupported));
                auto inspected = InspectPhysicsCollisionArtifact(source.id, payload, physicsTarget_);
                if (inspected.HasError())
                    return Result<void>::Failure(inspected.ErrorValue());
                if (inspected.Value().sourceDigest != source.sourceDigest || inspected.Value().configurationDigest != configuration_)
                    return Result<void>::Failure(MakeError(PhysicsErrors::ShapeArtifactInvalid,
                                                           "Collision artifact source or import configuration changed; recook required"));
                return Result<void>::Success();
            }

            [[nodiscard]] Result<Assets::CookOutputSink> Cook(const Assets::CookSourceView &source,
                                                              const CancellationToken &cancellation) const noexcept override {
                // This owned adapter contains importer exceptions; constructing an error after exhausted memory may still be fatal.
                try {
                    if (cancellation.IsCancellationRequested())
                        return Result<Assets::CookOutputSink>::Failure(CookFailure(MakeError(PhysicsErrors::ShapeCookCancelled)));
                    if (auto admitted = ValidateSource(source); admitted.HasError())
                        return Result<Assets::CookOutputSink>::Failure(admitted.ErrorValue());
                    auto imported = importer_->Import(source, cancellation);
                    if (imported.HasError())
                        return Result<Assets::CookOutputSink>::Failure(CookFailure(imported.ErrorValue()));
                    auto cooked = CookNormalizedSource(imported.Value(), source, physicsTarget_, cancellation);
                    if (cooked.HasError())
                        return Result<Assets::CookOutputSink>::Failure(CookFailure(cooked.ErrorValue()));
                    if (cancellation.IsCancellationRequested())
                        return Result<Assets::CookOutputSink>::Failure(CookFailure(MakeError(PhysicsErrors::ShapeCookCancelled)));
                    auto output = std::move(cooked).Value();
                    Assets::CookOutputSink sink;
                    auto &bytes = sink.payload;
                    bytes.reserve(Detail::CollisionArtifactHeaderBytes + output.payload.size());
                    bytes.insert(bytes.end(), Detail::CollisionArtifactMagic.begin(), Detail::CollisionArtifactMagic.end());
                    bytes.push_back(static_cast<std::uint8_t>(output.descriptor.kind));
                    Detail::AppendCollisionInteger(bytes, output.descriptor.subresource.Value(), 8);
                    Detail::AppendCollisionDigest(bytes, *output.descriptor.cacheKeyDigest);
                    Detail::AppendCollisionDigest(bytes, *output.descriptor.payloadDigest);
                    Detail::AppendCollisionDigest(bytes, physicsTarget_.digest);
                    Detail::AppendCollisionDigest(bytes, source.sourceDigest);
                    Detail::AppendCollisionDigest(bytes, configuration_);
                    bytes.insert(bytes.end(), output.payload.begin(), output.payload.end());
                    return Result<Assets::CookOutputSink>::Success(std::move(sink));
                } catch (const std::bad_alloc &) {
                    return Result<Assets::CookOutputSink>::Failure(MakeError(PhysicsErrors::ShapeCookLimitExceeded));
                } catch (const std::runtime_error &) {
                    return Result<Assets::CookOutputSink>::Failure(
                        MakeError(PhysicsErrors::ShapeCookSourceInvalid, "Collision source importer failed before publication"));
                } catch (const std::exception &exception) {
                    return Result<Assets::CookOutputSink>::Failure(MakeError(PhysicsErrors::ShapeCookImporterFailed, exception.what()));
                } catch (...) {
                    return Result<Assets::CookOutputSink>::Failure(
                        MakeError(PhysicsErrors::ShapeCookImporterFailed, "The asset cooker threw before publication."));
                }
            }

        private:
            /** @brief Admits exact bounded captured bytes before invoking the retained importer. */
            [[nodiscard]] Result<void> ValidateSource(const Assets::CookSourceView &source) const {
                if (source.type != type_ || source.target != target_)
                    return Result<void>::Failure(MakeError(PhysicsErrors::ProfileUnsupported));
                if (source.bytes.size() > PhysicsConvexHullCookLimits::MaximumPayloadBytes)
                    return Result<void>::Failure(MakeError(PhysicsErrors::ShapeCookLimitExceeded));
                if (ComputeSha256(std::as_bytes(source.bytes)) != source.sourceDigest)
                    return Result<void>::Failure(
                        MakeError(PhysicsErrors::ShapeCookSourceInvalid, "Collision source bytes do not match the captured digest"));
                return Result<void>::Success();
            }

            Assets::AssetTypeId type_;
            AssetCookTargetId target_;
            PhysicsShapeCookTargetDigest physicsTarget_;
            std::shared_ptr<const IPhysicsCollisionSourceImporter> importer_;
            Sha256Digest configuration_;
        };
    }  // namespace

    /** @copydoc MakePhysicsCollisionCookerContribution */
    Result<Assets::CookerContribution> MakePhysicsCollisionCookerContribution(
        const Assets::AssetTypeId &type, AssetCookTargetId target, const PhysicsShapeCookTargetDigest &physicsTarget,
        std::shared_ptr<const IPhysicsCollisionSourceImporter> importer) {
        if (!importer || type.Value().empty() || target.Value().empty())
            return Result<Assets::CookerContribution>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        const auto identity = importer->CacheIdentity();
        if (identity.version.empty() || identity.version.size() > 128 || identity.settingsSchemaVersion == 0)
            return Result<Assets::CookerContribution>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "Collision importer requires a bounded version and settings schema"));
        const auto configuration = ConfigurationDigest(identity, physicsTarget);
        return Result<Assets::CookerContribution>::Success({
            .contributionId = "horo.physics.collision-cook",
            .assetType = type,
            .targets = {target},
            .strategy =
                std::make_shared<const CollisionCookerStrategy>(type, std::move(target), physicsTarget, std::move(importer), configuration),
        });
    }
}  // namespace Horo::Physics
