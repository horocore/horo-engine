#include "Horo/Runtime/Render/MultiviewExtractionErrors.h"
#include "Horo/Runtime/Render/MultiviewMotionHistory.h"
#include "support/AllocationProbe.h"
#include "support/renderer/MultiviewTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <limits>
#include <optional>

namespace Horo::Render {
    namespace {
        constexpr MultiviewPublication Publication{1, 2, 3, 4, 5, 6};
        constexpr MultiviewCapabilities Capabilities{32, true, true, true};

        /** @brief Distinct instance data with exact resident generations and unit clip-space bounds. */
        [[nodiscard]] MultiviewSceneInstance Instance() {
            return {.mesh = {{1}, 2, 3}, .material = {{1}, 4}, .localBounds = {{-0.25F, -0.25F, 0.25F}, {0.25F, 0.25F, 0.75F}}};
        }

        /** @brief Shader view with an asymmetric horizontal clip offset. */
        [[nodiscard]] MultiviewSceneView View(const std::uint32_t index, const float offset = 0.0F) {
            auto matrix = Math::Mat4::Identity();
            matrix.values[12] = offset;
            return {index, matrix, Math::ClipDepthRange::ZeroToOne};
        }

        template <typename T> void RequireError(const Result<T> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().domain.Value() == expected.domain.Value());
            CHECK(result.ErrorValue().code.Value() == expected.code.Value());
        }

        TEST_CASE("Multiview motion pairs actual completed eyes and stable object identities", "[renderer][multiview][motion]") {
            MultiviewMotionHistory history;
            const std::array firstViews{View(0, 0.1F), View(1, -0.2F)};
            std::array firstObjects{Instance(), Instance()};
            firstObjects[0].localToWorld.values[12] = 0.2F;
            firstObjects[1].localToWorld.values[12] = -0.3F;
            const std::array keys{RenderMotionObjectId{31}, RenderMotionObjectId{22}};
            const auto first = MultiviewSceneSnapshot::Extract(Publication, Publication, firstViews, firstObjects, Capabilities,
                                                               MultiviewExecutionPolicy::Multipass);
            REQUIRE(first.HasValue());
            const MultiviewMotionFrame initial{Publication, 10, 1000};
            REQUIRE(history.Stage(initial, first.Value(), keys));
            CHECK_FALSE(history.Previous(initial, keys[0], 0));

            auto latest = Publication;
            latest.frame += 3;  // Desktop host can run independently of adjacent native projections.
            const std::array currentViews{View(0, -0.2F), View(1, 0.2F)};
            std::swap(firstObjects[0], firstObjects[1]);
            firstObjects[1].localToWorld.values[12] = 0.4F;
            const std::array reordered{keys[1], keys[0]};
            const auto current = MultiviewSceneSnapshot::Extract(latest, latest, currentViews, firstObjects, Capabilities,
                                                                 MultiviewExecutionPolicy::Multipass);
            REQUIRE(current.HasValue());
            const MultiviewMotionFrame next{latest, 11, 2000};
            CHECK_FALSE(history.Publish(next));
            REQUIRE(history.Publish(initial));
            CHECK_FALSE(history.Publish(initial));
            const auto start = Horo::Tests::AllocationProbe::Count();
            const auto staged = history.Stage(next, current.Value(), reordered);
            const auto left = history.Previous(next, keys[0], 0);
            const auto right = history.Previous(next, keys[0], 1);
            const auto end = Horo::Tests::AllocationProbe::Count();
            CHECK(start == end);
            REQUIRE(staged);
            REQUIRE(left);
            REQUIRE(right);
            CHECK(left->localToWorld.values[12] == 0.2F);
            CHECK(left->worldToClip == firstViews[0].worldToClip);
            CHECK(right->worldToClip == firstViews[1].worldToClip);
            CHECK_FALSE(history.Previous(initial, keys[0], 0));
            CHECK_FALSE(history.Previous(next, keys[0], 2));

            using namespace MultiviewShaderParameterIds;
            auto layout = MultiviewTest::Layout();
            layout.parameters = {{CurrentLocalToClip, Buffer, ShaderValueType::Float32, 4, 4, 2, 0, 64, 16, true, true},
                                 {PreviousLocalToClip, Buffer, ShaderValueType::Float32, 4, 4, 2, 128, 64, 16, true, true}};
            std::array<std::byte, 256> bytes{};
            const auto before = Horo::Tests::AllocationProbe::Count();
            const auto packed = PackMultiviewMotionShaderParameters(layout, history, next, 1, bytes);
            const auto after = Horo::Tests::AllocationProbe::Count();
            REQUIRE(packed.HasValue());
            CHECK(packed.Value() == bytes.size());
            CHECK(after == before);
            std::array<float, 64> values{};
            std::memcpy(values.data(), bytes.data(), bytes.size());
            CHECK(values[12] == 0.2F);
            CHECK(values[28] == 0.4F + 0.2F);
            CHECK(values[44] == 0.2F + 0.1F);
            CHECK(values[60] == 0.0F);
            const auto packedBytes = bytes;
            CHECK(PackMultiviewMotionShaderParameters(layout, history, initial, 1, bytes).HasError());
            CHECK(PackMultiviewMotionShaderParameters(layout, history, next, 2, bytes).HasError());
            CHECK(PackMultiviewMotionShaderParameters(layout, history, next, 1, std::span{bytes}.first(64)).HasError());
            CHECK(bytes == packedBytes);
        }

        TEST_CASE("Multiview motion omits culled replaced and discontinuous predecessors", "[renderer][multiview][motion]") {
            MultiviewMotionHistory history;
            const std::array firstViews{View(0), View(1, 4.0F)};
            const std::array objects{Instance(), Instance()};
            const std::array keys{RenderMotionObjectId{31}, RenderMotionObjectId{22}};
            const auto initialScene = MultiviewSceneSnapshot::Extract(Publication, Publication, firstViews, objects, Capabilities,
                                                                      MultiviewExecutionPolicy::Multipass);
            REQUIRE(initialScene.HasValue());
            const MultiviewMotionFrame initial{Publication, 1, 1000};
            REQUIRE(history.Stage(initial, initialScene.Value(), keys));
            REQUIRE(history.Publish(initial));
            auto publication = Publication;
            ++publication.frame;
            std::uint64_t sequence = 2;
            std::int64_t time = 2000;
            auto nextObjects = objects;
            bool ready = true;
            SECTION("new mesh on an existing source ID") {
                ++nextObjects[0].mesh.generation;
                ready = false;
            }
            SECTION("origin changed") {
                ++publication.originRevision;
                ready = false;
            }
            SECTION("projection skipped") {
                ++sequence;
                ready = false;
            }
            SECTION("nonmonotonic prediction") {
                time = 1000;
                ready = false;
            }
            SECTION("previous eye culled this object") {}
            const std::array presentViews{View(0), View(1)};
            const auto extracted = MultiviewSceneSnapshot::Extract(publication, publication, presentViews, nextObjects, Capabilities,
                                                                   MultiviewExecutionPolicy::Multipass);
            REQUIRE(extracted.HasValue());
            const MultiviewMotionFrame now{publication, sequence, time};
            const std::array duplicate{keys[0], keys[0]};
            CHECK_FALSE(history.Stage(now, extracted.Value(), duplicate));
            CHECK_FALSE(history.Publish(now));
            REQUIRE(history.Stage(now, extracted.Value(), keys));
            CHECK(history.Previous(now, keys[0], 0).has_value() == ready);
            CHECK_FALSE(history.Previous(now, keys[0], 1));
            CHECK_FALSE(history.ReadDraw(now, 0));
            history.Reset();
            CHECK_FALSE(history.Publish(now));
            CHECK_FALSE(history.Previous(now, keys[0], 0));
        }

        TEST_CASE("Multiview extraction copies scene authority once and preserves runtime order", "[unit][renderer][multiview]") {
            std::array views{View(0), View(1, 4.0F)};
            std::array instances{Instance(), Instance()};
            instances[1].localToWorld.values[12] = -4.0F;
            auto extracted = MultiviewSceneSnapshot::Extract(Publication, Publication, views, instances, Capabilities,
                                                             MultiviewExecutionPolicy::PreferInstanced);
            REQUIRE(extracted.HasValue());
            const auto snapshot = extracted.Value();
            REQUIRE(snapshot.Instances().size() == 2);
            REQUIRE(snapshot.Draws().size() == 2);
            CHECK(snapshot.Mode() == MultiviewExecutionMode::Instanced);
            CHECK(snapshot.Draws()[0].visibleViews == 1);
            CHECK(snapshot.Draws()[1].visibleViews == 2);
            CHECK(snapshot.ExpandedDrawCount() == 2);
            CHECK(snapshot.Invocation(1, 0, Publication).Value().viewIndex == 1);
            CHECK(snapshot.Invocation(1, 0, Publication).Value().instanceIndex == 1);
            instances[0].mesh.generation = 99;
            views[1].worldToClip = {};
            CHECK(snapshot.Instances()[0].mesh.generation == 3);
            CHECK(snapshot.Views()[1].worldToClip.values[12] == 4.0F);
            RequireError(snapshot.Invocation(0, 1, Publication), MultiviewExtractionErrors::InvalidInput);
            RequireError(snapshot.Invocation(2, 0, Publication), MultiviewExtractionErrors::InvalidInput);
        }

        TEST_CASE("Instanced and explicit multipass paths share the same visible shader mapping", "[unit][renderer][multiview]") {
            const std::array views{View(0), View(1)};
            const std::array instances{Instance()};
            auto optimized = MultiviewSceneSnapshot::Extract(Publication, Publication, views, instances, Capabilities,
                                                             MultiviewExecutionPolicy::RequireInstanced);
            auto fallbackCapabilities = Capabilities;
            fallbackCapabilities.instancedTargetRouting = false;
            auto fallback = MultiviewSceneSnapshot::Extract(Publication, Publication, views, instances, fallbackCapabilities,
                                                            MultiviewExecutionPolicy::PreferInstanced);
            REQUIRE(optimized.HasValue());
            REQUIRE(fallback.HasValue());
            CHECK(fallback.Value().Mode() == MultiviewExecutionMode::Multipass);
            CHECK(fallback.Value().ExpandedDrawCount() == optimized.Value().ExpandedDrawCount());
            for (std::uint32_t ordinal = 0; ordinal < 2; ++ordinal) {
                CHECK(fallback.Value().Invocation(0, ordinal, Publication).Value().viewIndex ==
                      optimized.Value().Invocation(0, ordinal, Publication).Value().viewIndex);
            }
            RequireError(MultiviewSceneSnapshot::Extract(Publication, Publication, views, instances, fallbackCapabilities,
                                                         MultiviewExecutionPolicy::RequireInstanced),
                         MultiviewExtractionErrors::Unsupported);
            fallbackCapabilities.multipass = false;
            RequireError(MultiviewSceneSnapshot::Extract(Publication, Publication, views, instances, fallbackCapabilities,
                                                         MultiviewExecutionPolicy::PreferInstanced),
                         MultiviewExtractionErrors::Unsupported);
        }

        TEST_CASE("Multiview culling preserves clip boundaries and handles both depth conventions", "[unit][renderer][multiview]") {
            std::array views{View(0)};
            std::array instances{Instance(), Instance(), Instance()};
            instances[0].localBounds = {{-1.0F, -1.0F, 0.0F}, {1.0F, 1.0F, 1.0F}};
            instances[1].localBounds = {{-0.1F, -0.1F, -0.9F}, {0.1F, 0.1F, -0.5F}};
            instances[2].localToWorld.values[12] = 10.0F;
            auto result = MultiviewSceneSnapshot::Extract(Publication, Publication, views, instances, Capabilities,
                                                          MultiviewExecutionPolicy::Multipass);
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().Draws().size() == 1);
            CHECK(result.Value().Draws()[0].instanceIndex == 0);
            views[0].depthRange = Math::ClipDepthRange::NegativeOneToOne;
            result = MultiviewSceneSnapshot::Extract(Publication, Publication, views, instances, Capabilities,
                                                     MultiviewExecutionPolicy::Multipass);
            REQUIRE(result.HasValue());
            CHECK(result.Value().Draws().size() == 2);
        }

        TEST_CASE("Full view masks and work bounds are admitted without shift overflow", "[unit][renderer][multiview]") {
            std::array<MultiviewSceneView, 32> views;
            for (std::uint32_t index = 0; index < views.size(); ++index)
                views[index] = View(index);
            const std::array instances{Instance()};
            MultiviewExtractionLimits limits{32, 128, 4096, 4096};
            auto result = MultiviewSceneSnapshot::Extract(Publication, Publication, views, instances, Capabilities,
                                                          MultiviewExecutionPolicy::PreferInstanced, limits);
            REQUIRE(result.HasValue());
            CHECK(result.Value().Draws()[0].visibleViews == std::numeric_limits<std::uint32_t>::max());
            CHECK(result.Value().Invocation(0, 31, Publication).Value().viewIndex == 31);
            CHECK(result.Value().ExpandedDrawCount() == 32);
            limits.maximumCullTests = 31;
            RequireError(MultiviewSceneSnapshot::Extract(Publication, Publication, views, instances, Capabilities,
                                                         MultiviewExecutionPolicy::PreferInstanced, limits),
                         MultiviewExtractionErrors::CapacityExceeded);
            limits.maximumCullTests = 32;
            limits.maximumExpandedDraws = 31;
            RequireError(MultiviewSceneSnapshot::Extract(Publication, Publication, views, instances, Capabilities,
                                                         MultiviewExecutionPolicy::PreferInstanced, limits),
                         MultiviewExtractionErrors::CapacityExceeded);
        }

        TEST_CASE("Extraction rejects every obsolete lifetime fence and shutdown", "[unit][renderer][multiview]") {
            const std::array views{View(0)};
            const std::array instances{Instance()};
            const auto result = MultiviewSceneSnapshot::Extract(Publication, Publication, views, instances, Capabilities,
                                                                MultiviewExecutionPolicy::Multipass);
            REQUIRE(result.HasValue());
            constexpr std::array fences{&MultiviewPublication::sceneIncarnation,  &MultiviewPublication::frame,
                                        &MultiviewPublication::originRevision,    &MultiviewPublication::viewRevision,
                                        &MultiviewPublication::deviceIncarnation, &MultiviewPublication::capabilityRevision};
            for (const auto fence : fences) {
                auto replacement = Publication;
                ++(replacement.*fence);
                RequireError(result.Value().Validate(replacement), MultiviewExtractionErrors::StalePublication);
                RequireError(result.Value().Invocation(0, 0, replacement), MultiviewExtractionErrors::StalePublication);
                RequireError(MultiviewSceneSnapshot::Extract(Publication, replacement, views, instances, Capabilities,
                                                             MultiviewExecutionPolicy::Multipass),
                             MultiviewExtractionErrors::StalePublication);
            }
            RequireError(result.Value().Validate({}), MultiviewExtractionErrors::InvalidInput);
            CHECK(result.Value().Instances()[0].mesh == instances[0].mesh);
        }

        TEST_CASE("Invalid late source cannot replace an existing immutable presentation", "[unit][renderer][multiview]") {
            std::array views{View(0), View(1)};
            std::array instances{Instance(), Instance()};
            const auto good = MultiviewSceneSnapshot::Extract(Publication, Publication, views, instances, Capabilities,
                                                              MultiviewExecutionPolicy::Multipass);
            REQUIRE(good.HasValue());
            SECTION("Wrong resource owner") {
                instances.back().material.owner.value = 9;
            }
            SECTION("Invalid generation") {
                instances.back().mesh.generation = 0;
            }
            SECTION("Invalid bounds") {
                instances.back().localBounds.minimum.x = 10.0F;
            }
            SECTION("Nonfinite model") {
                instances.back().localToWorld.values[0] = std::numeric_limits<float>::infinity();
            }
            SECTION("Non-affine model") {
                instances.back().localToWorld.values[3] = 1.0F;
            }
            SECTION("Non-dense view") {
                views.back().index = 4;
            }
            SECTION("Singular clip transform") {
                views.back().worldToClip = {};
            }
            SECTION("Invalid depth convention") {
                views.back().depthRange = static_cast<Math::ClipDepthRange>(99);
            }
            const auto rejected = MultiviewSceneSnapshot::Extract(Publication, Publication, views, instances, Capabilities,
                                                                  MultiviewExecutionPolicy::Multipass);
            RequireError(rejected, MultiviewExtractionErrors::InvalidInput);
            CHECK(good.Value().Instances().size() == 2);
            CHECK(good.Value().ExpandedDrawCount() == 4);
        }

        TEST_CASE("Successful extraction and invocation perform no C++ heap allocations", "[unit][renderer][multiview]") {
            const std::array views{View(0), View(1)};
            const std::array instances{Instance()};
            std::optional<MultiviewSceneSnapshot> retained;
            Tests::AllocationProbe::Measurement measured;
            bool succeeded = false;
            {
                Tests::AllocationProbe::ScopedMeasurement allocation;
                auto result = MultiviewSceneSnapshot::Extract(Publication, Publication, views, instances, Capabilities,
                                                              MultiviewExecutionPolicy::PreferInstanced);
                succeeded = result.HasValue();
                if (succeeded) {
                    retained = result.Value();
                    succeeded = retained->Invocation(0, 1, Publication).HasValue();
                }
                measured = allocation.Snapshot();
            }
            REQUIRE(succeeded);
            CHECK(measured.requests == 0);
        }

        TEST_CASE("Empty scene is valid while unsupported counts and exhausted budgets fail closed", "[unit][renderer][multiview]") {
            const std::array views{View(0), View(1)};
            auto result =
                MultiviewSceneSnapshot::Extract(Publication, Publication, views, {}, Capabilities, MultiviewExecutionPolicy::Multipass);
            REQUIRE(result.HasValue());
            CHECK(result.Value().Draws().empty());
            RequireError(MultiviewSceneSnapshot::Extract(Publication, Publication, {}, {}, Capabilities,
                                                         MultiviewExecutionPolicy::Multipass),
                         MultiviewExtractionErrors::InvalidInput);
            auto capabilities = Capabilities;
            capabilities.maximumViews = 1;
            RequireError(MultiviewSceneSnapshot::Extract(Publication, Publication, views, {}, capabilities,
                                                         MultiviewExecutionPolicy::PreferInstanced),
                         MultiviewExtractionErrors::Unsupported);
            std::array<MultiviewSceneInstance, 129> oversized;
            RequireError(MultiviewSceneSnapshot::Extract(Publication, Publication, views, oversized, Capabilities,
                                                         MultiviewExecutionPolicy::Multipass),
                         MultiviewExtractionErrors::CapacityExceeded);
        }
    }  // namespace
}  // namespace Horo::Render
