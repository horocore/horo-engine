#include "Horo/Runtime/Render/UiPaintFrameUpload.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <vector>

namespace Horo::Render {
    namespace {
        /** @brief Mutates one selected uniform contract field; Catch retains the original named case/sections. */
        void CorruptUniforms(std::array<UiPaintParameters, 1> &parameters) {
            SECTION("unrepresentable target extent") {
                parameters[0].extent[0] = 0;
            }
            SECTION("unknown sampling interpretation") {
                parameters[0].sampling = 5;
            }
            SECTION("nonfinite rounded-mask extent") {
                parameters[0].maskExtent[0] = std::numeric_limits<float>::infinity();
            }
            SECTION("rounded-mask corners cannot overlap an edge") {
                parameters[0].sampling = static_cast<std::uint32_t>(UiPaintSampling::RoundedRectangle);
                parameters[0].maskExtent = {100, 40};
                parameters[0].cornerRadii = {30, 10, 10, 20};
            }
            SECTION("reserved extended mask ABI bytes") {
                parameters[0].maskReserved[0] = 1;
            }
            SECTION("unknown target-origin bit") {
                parameters[0].flags = 8;
            }
            SECTION("noncanonical reserved ABI bytes") {
                parameters[0].reserved[1] = 1;
            }
            SECTION("invalid group composite opacity") {
                parameters[0].opacity = 1.01F;
            }
        }
    }  // namespace

    TEST_CASE("UI reusable frame admission validates complete geometry before native mapped writes", "[renderer][ui-paint][upload]") {
        std::array<UiPaintVertex, 3> vertices{};
        for (auto &vertex : vertices)
            vertex.color = {1, 1, 1, 1};
        std::array<std::uint32_t, 3> indices{0, 1, 2};
        std::array parameters{UiPaintParameters{{640, 480}}};
        UiPaintFrameUpdate update{1, vertices, indices, parameters};
        REQUIRE(ValidateUiPaintFrameUpdate(update).HasValue());
        SECTION("unissued revision") {
            update.revision = 0;
        }
        SECTION("missing vertex payload") {
            update.vertices = {};
        }
        SECTION("index cannot address beyond the complete vertex payload") {
            indices.back() = 3;
        }
        SECTION("nonfinite physical point") {
            vertices[1].position[0] = std::numeric_limits<float>::infinity();
        }
        SECTION("nonfinite shader coordinate") {
            vertices[1].uv[1] = std::numeric_limits<float>::quiet_NaN();
        }
        SECTION("invalid linear source color") {
            vertices[0].color[3] = -1;
        }
        CorruptUniforms(parameters);
        SECTION("finite uniform inventory exhaustion") {
            const std::vector<UiPaintParameters> excessive(UiPaintLimits::MaximumDraws + 1, parameters[0]);
            update.parameters = excessive;
            const auto rejected = ValidateUiPaintFrameUpdate(update);
            REQUIRE(rejected.HasError());
            REQUIRE(rejected.ErrorValue().domain.Value() == UiPaintFrameErrors::InvalidInput.domain.Value());
            REQUIRE(rejected.ErrorValue().code.Value() == UiPaintFrameErrors::InvalidInput.code.Value());
            return;
        }
        const auto rejected = ValidateUiPaintFrameUpdate(update);
        REQUIRE(rejected.HasError());
        REQUIRE(rejected.ErrorValue().domain.Value() == UiPaintFrameErrors::InvalidInput.domain.Value());
        REQUIRE(rejected.ErrorValue().code.Value() == UiPaintFrameErrors::InvalidInput.code.Value());
    }

    TEST_CASE("UI rounded frame ABI preserves asymmetric local shape bounds", "[renderer][ui-paint][upload]") {
        const std::array<UiPaintVertex, 3> vertices{};
        const std::array<std::uint32_t, 3> indices{0, 1, 2};
        std::array parameters{UiPaintParameters{{640, 480}}};
        parameters[0].sampling = static_cast<std::uint32_t>(UiPaintSampling::RoundedRectangle);
        parameters[0].maskExtent = {100, 100};
        parameters[0].cornerRadii = {80, 0, 80, 0};
        const UiPaintFrameUpdate update{1, vertices, indices, parameters};
        REQUIRE(ValidateUiPaintFrameUpdate(update).HasValue());
        parameters[0].cornerRadii[1] = 30;
        const auto rejected = ValidateUiPaintFrameUpdate(update);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == UiPaintFrameErrors::InvalidInput.code.Value());
    }
}  // namespace Horo::Render
