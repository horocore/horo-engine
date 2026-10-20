#include "Horo/Runtime/Render/MultiviewExtractionErrors.h"
#include "Horo/Runtime/Render/MultiviewSceneExtraction.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
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
