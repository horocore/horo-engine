#include "Horo/Runtime/Render/RenderResourceDescriptorErrors.h"
#include "OpenGLRenderBackendErrors.h"
#include "OpenGLShaderRealization.h"
#include "TypedIdentityTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace {
    struct SamplerState {
        bool available{true};
        std::uint32_t candidate{41};
        std::size_t generates{0};
        std::size_t destroys{0};
        std::size_t parameters{0};
        std::size_t failAt{0};
        std::array<std::pair<std::uint32_t, std::int32_t>, 7> integers{};
        std::size_t integerCount{0};
        std::array<float, 2> lods{};
        std::size_t floatCount{0};
    };

    thread_local SamplerState *currentSampler = nullptr;

    Horo::Render::Detail::OpenGLSamplerFunctions Functions(SamplerState &state) {
        currentSampler = &state;
        return {.isAvailable = +[]() noexcept {
            return currentSampler->available;
        }, .generate = +[](std::uint32_t &sampler) noexcept {
            ++currentSampler->generates;
            sampler = currentSampler->candidate;
        }, .destroy = +[](std::uint32_t) noexcept {
            ++currentSampler->destroys;
        }, .parameterInteger = +[](std::uint32_t, std::uint32_t name, std::int32_t value) noexcept {
            currentSampler->integers[currentSampler->integerCount++] = {name, value};
            ++currentSampler->parameters;
        }, .parameterFloat = +[](std::uint32_t, std::uint32_t, float value) noexcept {
            currentSampler->lods[currentSampler->floatCount++] = value;
            ++currentSampler->parameters;
        }, .error = +[]() noexcept -> std::uint32_t {
            return currentSampler->failAt != 0 && currentSampler->parameters >= currentSampler->failAt ? 0x0505 : 0;
        }};
    }
}  // namespace

TEST_CASE("OpenGL core sampler realizes the complete admitted policy without binding state", "[render][opengl][sampler]") {
    using namespace Horo::Render;
    SamplerState state;
    const auto functions = Functions(state);
    RenderSamplerDescriptor descriptor;
    descriptor.minFilter = RenderSamplerFilter::Linear;
    descriptor.magFilter = RenderSamplerFilter::Linear;
    descriptor.mipmapMode = RenderSamplerMipmapMode::Linear;
    descriptor.addressU = RenderSamplerAddressMode::Repeat;
    descriptor.addressV = RenderSamplerAddressMode::MirroredRepeat;
    descriptor.addressW = RenderSamplerAddressMode::ClampToBorder;
    descriptor.minimumLod = 2.0F;
    descriptor.maximumLod = 8.0F;
    descriptor.compareEnabled = true;
    descriptor.compare = RenderCompareFunction::Greater;
    const auto created = Detail::CreateOpenGLSampler(descriptor, functions);
    REQUIRE(created.HasValue());
    CHECK(created.Value() == 41);
    CHECK(state.generates == 1);
    CHECK(state.parameters == 9);
    CHECK(state.destroys == 0);
    CHECK(state.integers[0].second == 0x2703);  // GL_LINEAR_MIPMAP_LINEAR
    CHECK(state.integers[2].second == 0x2901);  // GL_REPEAT
    CHECK(state.integers[3].second == 0x8370);  // GL_MIRRORED_REPEAT
    CHECK(state.integers[4].second == 0x812D);  // GL_CLAMP_TO_BORDER
    CHECK(state.integers[5].second == 0x884E);  // GL_COMPARE_REF_TO_TEXTURE
    CHECK(state.integers[6].second == 0x0204);  // GL_GREATER
    CHECK(state.lods[0] == 2.0F);
    CHECK(state.lods[1] == 8.0F);
    functions.destroy(created.Value());
    CHECK(state.destroys == 1);
}

TEST_CASE("OpenGL sampler rejects unsupported and malformed policies before native mutation", "[render][opengl][sampler]") {
    using namespace Horo::Render;
    SamplerState state;
    const auto functions = Functions(state);
    RenderSamplerDescriptor descriptor;
    SECTION("Anisotropy is not silently clamped") {
        descriptor.maximumAnisotropy = 2.0F;
        Horo::Tests::RequireError(Detail::CreateOpenGLSampler(descriptor, functions), OpenGLBackendErrors::UnsupportedResourceOperation);
    }
    SECTION("Unavailable context dispatch") {
        state.available = false;
        Horo::Tests::RequireError(Detail::CreateOpenGLSampler(descriptor, functions), OpenGLBackendErrors::UnsupportedResourceOperation);
    }
    SECTION("Reversed finite LOD range") {
        descriptor.minimumLod = 2.0F;
        descriptor.maximumLod = 1.0F;
        Horo::Tests::RequireError(Detail::CreateOpenGLSampler(descriptor, functions), RenderResourceDescriptorErrors::SamplerInvalid);
    }
    CHECK(state.generates == 0);
    CHECK(state.parameters == 0);
}

TEST_CASE("OpenGL sampler failure releases every unpublished candidate exactly once", "[render][opengl][sampler]") {
    using namespace Horo::Render;
    for (std::size_t fault = 1; fault <= 9; ++fault) {
        SamplerState state;
        state.failAt = fault;
        Horo::Tests::RequireError(Detail::CreateOpenGLSampler({}, Functions(state)), OpenGLBackendErrors::CommandFailed);
        CHECK(state.generates == 1);
        CHECK(state.destroys == 1);
    }
    SamplerState absent;
    absent.candidate = 0;
    Horo::Tests::RequireError(Detail::CreateOpenGLSampler({}, Functions(absent)), OpenGLBackendErrors::ResourceCreationFailed);
    CHECK(absent.destroys == 0);
}
