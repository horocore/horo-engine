#pragma once

/** @file
 * @brief Private graph, spatial and complete-row fixtures shared by transformation and noise regressions.
 */
#include "Horo/Foundation/JobSystem.h"
#include "Horo/PCG/PCGCpuEvaluator.h"
#include "Horo/PCG/PCGErrors.h"
#include "Horo/PCG/PCGPointGeneration.h"
#include "Horo/PCG/PCGPointTransformation.h"
#include "PCGCpuEvaluatorInternal.h"
#include "PCGPointTransformationInternal.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <memory>
#include <ranges>

namespace Horo::PCG {
    namespace {
        template <class T> T Id(std::uint64_t value) {
            auto id = T::Create(value);
            REQUIRE(id.HasValue());
            return id.Value();
        }

        inline PCGCapabilitySet Grant() {
            const std::array values{PCGCapability::Validation, PCGCapability::OfflineBake};
            return PCGCapabilitySet::Create(values).Value();
        }

        inline PCGGraphPin Pin(std::uint64_t value, PCGPinDirection direction) {
            return {Id<PinId>(value), direction, PCGPinType::PointSet,
                    direction == PCGPinDirection::Input ? PCGPinCardinality::Single : PCGPinCardinality::Multiple, std::nullopt};
        }

        inline PCGCookedPlan Compile(const PCGGraphSourceData &source, const PCGNodeCatalogSnapshot &catalog) {
            std::vector<PCGNodeTypeSupport> types;
            for (const auto &entry : catalog.Nodes())
                types.push_back({entry.type, entry.version, entry.version});
            const PCGGraphSourceContext context{.supportedNodeTypes = types};
            const auto graph = PCGGraphAsset::Create(source, context);
            REQUIRE(graph.HasValue());
            const auto bytes = SerializePCGGraphAsset(graph.Value());
            REQUIRE(bytes.HasValue());
            const auto restored = DeserializePCGGraphAsset(bytes.Value(), context);
            REQUIRE(restored.HasValue());
            CHECK(SerializePCGGraphAsset(restored.Value()).Value() == bytes.Value());
            auto created = PCGRegistry::Create(Id<PCGRegistryInstanceId>(1), catalog.Capabilities());
            REQUIRE(created.HasValue());
            auto registry = std::move(created).Value();
            for (const auto &entry : catalog.Nodes())
                REQUIRE(
                    registry.RegisterNodeRuntime({entry.type, entry.runtimeContractVersion, entry.determinism, entry.requiredCapabilities})
                        .HasValue());
            PCGGraphDescriptor descriptor;
            descriptor.generation = source.generation;
            descriptor.requiredCapabilities = Grant();
            for (const auto &node : source.nodes)
                descriptor.nodes.push_back({node.id, node.type});
            REQUIRE(registry.RegisterGraph(descriptor).HasValue());
            const auto snapshot = registry.Snapshot();
            REQUIRE(snapshot.HasValue());
            const auto validation = ValidatePCGGraph(graph.Value(), snapshot.Value(), Grant());
            REQUIRE(validation.HasValue());
            const auto plan = CompilePCGGraph(graph.Value(), validation.Value(), snapshot.Value(), catalog, Grant(),
                                              PCGGraphSourceHardLimits::SourceBytes);
            REQUIRE(plan.HasValue());
            return plan.Value();
        }

        inline PCGCookedPlan Plan(PCGCpuNodeKind kind, std::vector<std::uint8_t> payload) {
            auto created = PCGNodeCatalog::Create(ProjectPCGCapabilities(PCGHostProfile::Interactive, Grant()).Value());
            REQUIRE(created.HasValue());
            auto owner = std::move(created).Value();
            REQUIRE(owner.Register(PCGCpuNodeKind::SnapshotGrid).HasValue());
            REQUIRE(owner.Register(kind).HasValue());
            const auto catalog = owner.Snapshot().Value();
            PCGGraphSourceData source;
            source.generation = {Id<GraphId>(83), Id<GraphRevision>(1)};
            source.deterministicSeed = 71;
            source.nodes = {{Id<NodeId>(10),
                             PCGCpuNodeType(PCGCpuNodeKind::SnapshotGrid).Value(),
                             {1, 0},
                             {Pin(101, PCGPinDirection::Output)},
                             EncodePCGPointGenerationSettings(PCGGridGenerationSettings{Id<SpatialElementId>(8)}).Value()},
                            {Id<NodeId>(20),
                             PCGCpuNodeType(kind).Value(),
                             {1, 0},
                             {Pin(201, PCGPinDirection::Input), Pin(202, PCGPinDirection::Output)},
                             std::move(payload)}};
            source.edges = {{Id<EdgeId>(1), Id<NodeId>(10), Id<PinId>(101), Id<NodeId>(20), Id<PinId>(201)}};
            auto plan = Compile(source, catalog);
            REQUIRE(owner.Remove(PCGCpuNodeType(kind).Value()).HasValue());
            REQUIRE(owner.Close().HasValue());
            return plan;
        }

        template <class T> PCGCookedPlan Plan(PCGCpuNodeKind kind, const T &settings) {
            auto payload = EncodePCGPointTransformationSettings(settings);
            REQUIRE(payload.HasValue());
            return Plan(kind, payload.Value());
        }

        inline PCGSpatialSnapshot Spatial(Math::Vec3 origin = {}, bool surface = true, bool downward = false, bool tilted = false) {
            PCGSpatialSnapshotCandidate c;
            c.snapshot = Id<SpatialSnapshotId>(1);
            c.provenance = {Id<SpatialProviderId>(1), Id<SpatialSourceId>(2), Id<SpatialRevision>(1)};
            c.bounds = {{-10, -10, -10}, {10, 10, 10}};
            c.coverage = PCGSpatialCoverage::Complete;
            c.grids = {{Id<SpatialElementId>(8), origin, {0.25F, 0.25F, 0.25F}, {2, 1, 2}}};
            if (surface)
                c.surfaces = {{Id<SpatialElementId>(9),
                               {{{-5, 0, -5}, {-5, 0, 5}, {5, 0, -5}}},
                               downward ? Math::Vec3{0, -1, 0} : Math::Vec3{0, 1, 0}},
                              {Id<SpatialElementId>(10),
                               {{{5, 0, 5}, {5, 0, -5}, {-5, 0, 5}}},
                               downward ? Math::Vec3{0, -1, 0} : Math::Vec3{0, 1, 0}}};
            if (tilted)
                for (auto &triangle : c.surfaces) {
                    for (auto &vertex : triangle.vertices)
                        vertex.y = vertex.z;
                    triangle.normal = {0, 0.7071067811865475F, -0.7071067811865475F};
                }
            if (downward)
                for (auto &triangle : c.surfaces)
                    std::swap(triangle.vertices[1], triangle.vertices[2]);
            const auto captured = CapturePCGSpatialSnapshot(std::move(c));
            REQUIRE(captured.HasValue());
            return captured.Value();
        }

        inline std::shared_ptr<const PCGPointSchema> Schema(const PCGPointNoiseSettings *noise = nullptr) {
            std::vector<PCGAttributeDescriptor> fields;
            for (auto key : PCGSourceRootAttributes)
                fields.push_back({PCGAttributeKey::Create(key).Value(), PCGAttributeType::UnsignedInteger});
            fields.push_back({PCGAttributeKey::Create(PCGSourceSampleAttribute).Value(), PCGAttributeType::UnsignedInteger});
            const std::array names{"test.boolean", "test.signed",  "test.unsigned", "test.scalar",
                                   "test.vector2", "test.vector3", "test.vector4"};
            for (std::size_t i = 0; i < names.size(); ++i)
                fields.push_back({PCGAttributeKey::Create(names[i]).Value(), static_cast<PCGAttributeType>(i)});
            if (noise != nullptr && std::ranges::none_of(fields, [&](const auto &field) {
                return field.key == noise->attribute;
            }))
                fields.push_back({noise->attribute, noise->type});
            auto schema = PCGPointSchema::Capture({PCGOperationalTier::Baseline, fields});
            REQUIRE(schema.HasValue());
            return std::make_shared<const PCGPointSchema>(schema.Value());
        }

        inline std::array<PCGPointOutputBound, 2> Bounds(const PCGPointNoiseSettings *noise = nullptr) {
            return {{{0, Id<PinId>(101), Schema(), 4}, {1, Id<PinId>(202), Schema(noise), 4}}};
        }

        inline PCGCpuEvaluationLimits Limits(const PCGCookedPlan &plan) {
            const auto tier = LimitsForTier(plan.Tier()).Value();
            PCGCpuEvaluationLimits limits{tier.maximumScratchBytes, tier.maximumCandidateBytes};
            limits.grantedCapabilities = Grant();
            limits.numericProfile.bytes[0] = 1;
            limits.world = Id<PCGWorldId>(1);
            limits.providerContent.bytes[0] = 1;
            return limits;
        }

        inline PCGCpuCandidate Evaluate(const PCGCookedPlan &plan, const PCGSpatialSnapshot &spatial,
                                        const PCGPointNoiseSettings *noise = nullptr, std::uint32_t workers = 1,
                                        JobSystem *jobs = nullptr) {
            auto limits = Limits(plan);
            limits.workers = workers;
            limits.jobs = jobs;
            const auto bounds = Bounds(noise);
            const auto result = EvaluatePCGCpu(plan, spatial, bounds, {}, limits);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        const PCGPointStorage &Points(const PCGCpuCandidate &candidate) {
            REQUIRE(candidate.Outputs().size() == 1);
            return *candidate.Outputs()[0].points;
        }

        /** @brief Checks retained schema-1 identity against the original stored seed, independent of downstream plan changes. */
        inline void ValidIdentity(const PCGPointStorage &points) {
            const auto *samples = points.FindColumn<PCGUnsignedIntegerColumn>(PCGSourceSampleAttribute);
            REQUIRE(samples != nullptr);
            for (std::size_t i = 0; i < points.PointCount(); ++i) {
                Sha256Digest root;
                for (std::size_t word = 0; word < 4; ++word) {
                    const auto value = (*points.FindColumn<PCGUnsignedIntegerColumn>(PCGSourceRootAttributes[word]))[i];
                    for (std::size_t byte = 0; byte < 8; ++byte)
                        root.bytes[word * 8 + byte] = static_cast<std::uint8_t>(value >> ((7 - byte) * 8));
                }
                CHECK((*samples)[i] == i + 1);
                CHECK(DerivePCGSampleSeed(root, Id<SourceSampleId>((*samples)[i])).Value() == points.Seeds()[i]);
            }
        }

        /** @brief Populates one complete typed nonzero source fixture before the actual routed dispatch. */
        inline void PopulateColumns(const PCGPointWriteView &writer, const PCGProvenance &provenance) {
            REQUIRE(writer.SetColumnValue<PCGBoolColumn>("test.boolean", 0, 1).HasValue());
            REQUIRE(writer.SetColumnValue<PCGSignedIntegerColumn>("test.signed", 0, -7).HasValue());
            REQUIRE(
                writer.SetColumnValue<PCGUnsignedIntegerColumn>("test.unsigned", 0, std::numeric_limits<std::uint64_t>::max()).HasValue());
            REQUIRE(writer.SetColumnValue<PCGScalarColumn>("test.scalar", 0, 0.125).HasValue());
            REQUIRE(writer.SetColumnValue<PCGVector2Column>("test.vector2", 0, {1, 2}).HasValue());
            REQUIRE(writer.SetColumnValue<PCGVector3Column>("test.vector3", 0, {3, 4, 5}).HasValue());
            REQUIRE(writer.SetColumnValue<PCGVector4Column>("test.vector4", 0, {6, 7, 8, 9}).HasValue());
            for (std::size_t word = 0; word < 4; ++word) {
                std::uint64_t value{};
                for (std::size_t byte = 0; byte < 8; ++byte)
                    value = (value << 8) | provenance.Key().bytes[word * 8 + byte];
                REQUIRE(writer.SetColumnValue<PCGUnsignedIntegerColumn>(PCGSourceRootAttributes[word], 0, value).HasValue());
            }
            REQUIRE(writer.SetColumnValue<PCGUnsignedIntegerColumn>(PCGSourceSampleAttribute, 0, 1).HasValue());
        }

        /** @brief Compare all bounds components without requiring equality on the public carrier. */
        inline bool SameBounds(const Math::Aabb &a, const Math::Aabb &b) {
            return a.minimum == b.minimum && a.maximum == b.maximum;
        }

        inline void SameIdentity(const PCGPointStorage &a, const PCGPointStorage &b) {
            CHECK(std::ranges::equal(a.Seeds(), b.Seeds()));
            CHECK(std::ranges::equal(a.Densities(), b.Densities()));
            CHECK(std::ranges::equal(a.Bounds(), b.Bounds(), SameBounds));
            for (auto key : PCGSourceRootAttributes)
                CHECK(*a.FindColumn<PCGUnsignedIntegerColumn>(key) == *b.FindColumn<PCGUnsignedIntegerColumn>(key));
            CHECK(*a.FindColumn<PCGUnsignedIntegerColumn>(PCGSourceSampleAttribute) ==
                  *b.FindColumn<PCGUnsignedIntegerColumn>(PCGSourceSampleAttribute));
        }
    }  // namespace
}  // namespace Horo::PCG
