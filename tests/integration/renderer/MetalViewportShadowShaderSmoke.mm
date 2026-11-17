#include "MetalViewportShaderTypes.h"
#include "MetalViewportShaders.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <type_traits>

namespace Horo::Editor {
    namespace {
        struct NativePipeline final {
            __strong id<MTLRenderPipelineState> state;
            __strong MTLRenderPipelineReflection *reflection;
        };

        /** @brief Compiles the exact production bootstrap source without creating a window or renderer session. */
        id<MTLLibrary> CompileViewportLibrary(id<MTLDevice> device) {
            NSString *source = [NSString stringWithUTF8String:MetalViewportShaderSource()];
            REQUIRE(source != nil);
            NSError *error = nil;
            id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
            INFO((error == nil ? "Production MSL compiled." : error.localizedDescription.UTF8String));
            REQUIRE(library != nil);
            return library;
        }

        /** @brief Realizes the actual bootstrap pass attachment recipe and asks the driver for its argument ABI. */
        NativePipeline CompileViewportPipeline(id<MTLDevice> device, id<MTLLibrary> library, NSString *vertex, NSString *fragment) {
            MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
            descriptor.vertexFunction = [library newFunctionWithName:vertex];
            REQUIRE(descriptor.vertexFunction != nil);
            if (fragment != nil) {
                descriptor.fragmentFunction = [library newFunctionWithName:fragment];
                REQUIRE(descriptor.fragmentFunction != nil);
                descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            }
            descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
            NSError *error = nil;
            MTLRenderPipelineReflection *reflection = nil;
            id<MTLRenderPipelineState> pipeline =
                [device newRenderPipelineStateWithDescriptor:descriptor
                                                     options:MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo
                                                  reflection:&reflection
                                                       error:&error];
            INFO((error == nil ? "Production pass pipeline realized." : error.localizedDescription.UTF8String));
            REQUIRE(pipeline != nil);
            REQUIRE(reflection != nil);
            return {pipeline, reflection};
        }

        /** @brief Finds an actual used argument in the exact stage and native slot consumed by the production draw. */
        id<MTLBufferBinding> RequireBuffer(NSArray<id<MTLBinding>> *bindings, NSUInteger slot, std::size_t bytes) {
            for (id<MTLBinding> binding in bindings) {
                if (binding.used && binding.type == MTLBindingTypeBuffer && binding.index == slot) {
                    id<MTLBufferBinding> buffer = (id<MTLBufferBinding>)binding;
                    CHECK(buffer.bufferDataSize == bytes);
                    return buffer;
                }
            }
            FAIL("Actual viewport pipeline omitted the required constant buffer binding.");
            return nil;
        }

        /** @brief Compares an actual compiled member against the owning production C++ declaration. */
        MTLStructMember *RequireMember(MTLStructType *type, NSString *name, std::size_t offset) {
            REQUIRE(type != nil);
            MTLStructMember *member = [type memberByName:name];
            INFO(name.UTF8String);
            REQUIRE(member != nil);
            CHECK(member.offset == offset);
            return member;
        }

        /** @brief Checks the complete matrix/vector array packing used by cascades, instead of authored offset constants. */
        void RequireArray(MTLStructType *type, NSString *name, std::size_t offset, std::size_t count, std::size_t stride,
                          MTLDataType element) {
            MTLStructMember *member = RequireMember(type, name, offset);
            REQUIRE(member.dataType == MTLDataTypeArray);
            MTLArrayType *array = member.arrayType;
            REQUIRE(array != nil);
            CHECK(array.arrayLength == count);
            CHECK(array.stride == stride);
            CHECK(array.elementType == element);
        }

        /** @brief Verifies the real depth atlas and sampler slots used by the bootstrap encoder. */
        void RequireAtlasBindings(NSArray<id<MTLBinding>> *bindings) {
            bool textureFound{}, samplerFound{};
            for (id<MTLBinding> binding in bindings) {
                if (!binding.used || binding.index != 0)
                    continue;
                if (binding.type == MTLBindingTypeTexture) {
                    id<MTLTextureBinding> texture = (id<MTLTextureBinding>)binding;
                    CHECK(texture.textureType == MTLTextureType2D);
                    CHECK(texture.textureDataType == MTLDataTypeFloat);
                    CHECK(texture.depthTexture);
                    textureFound = true;
                } else if (binding.type == MTLBindingTypeSampler)
                    samplerFound = true;
            }
            CHECK(textureFound);
            CHECK(samplerFound);
        }

        /** @brief Covers every scalar and aggregate which drives actual cascade choice, blend, filter and atlas sampling. */
        void RequireLightingLayout(id<MTLBufferBinding> buffer) {
            static_assert(std::is_standard_layout_v<MetalSceneLighting>);
            MTLStructType *layout = buffer.bufferStructType;
            RequireArray(layout, @"lights", offsetof(MetalSceneLighting, lights), Render::MaximumForwardLights, sizeof(MetalLight),
                         MTLDataTypeStruct);
            MTLStructType *light = [layout memberByName:@"lights"].arrayType.elementStructType;
            RequireMember(light, @"positionKind", offsetof(MetalLight, positionKind));
            RequireMember(light, @"directionRange", offsetof(MetalLight, directionRange));
            RequireMember(light, @"colorIntensity", offsetof(MetalLight, colorIntensity));
            RequireMember(light, @"cone", offsetof(MetalLight, cone));
            RequireMember(layout, @"cameraPosition", offsetof(MetalSceneLighting, cameraPosition));
            RequireMember(layout, @"lightCount", offsetof(MetalSceneLighting, lightCount));
            RequireMember(layout, @"shadowEnabled", offsetof(MetalSceneLighting, shadowEnabled));
            RequireMember(layout, @"shadowLightIndex", offsetof(MetalSceneLighting, shadowLightIndex));
            RequireMember(layout, @"shadowCascadeCount", offsetof(MetalSceneLighting, shadowCascadeCount));
            RequireArray(layout, @"shadowViewProjection", offsetof(MetalSceneLighting, shadowViewProjection), 4, sizeof(Math::Mat4),
                         MTLDataTypeFloat4x4);
            RequireArray(layout, @"cascadeSplits", offsetof(MetalSceneLighting, cascadeSplits), 4, sizeof(Math::Vec4), MTLDataTypeFloat4);
            RequireArray(layout, @"atlasRects", offsetof(MetalSceneLighting, atlasRects), 4, sizeof(Math::Vec4), MTLDataTypeFloat4);
            RequireMember(layout, @"cameraForward", offsetof(MetalSceneLighting, cameraForward));
            RequireMember(layout, @"shadowBiasFilter", offsetof(MetalSceneLighting, shadowBiasFilter));
            RequireMember(layout, @"shadowTexel", offsetof(MetalSceneLighting, shadowTexel));
        }
    }  // namespace

    TEST_CASE("Production Metal viewport cascaded shadow shaders compile with the actual host uniform ABI",
              "[integration][renderer][metal][gpu][shadow][shader]") {
        @autoreleasepool {
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            REQUIRE(device != nil);
            id<MTLLibrary> library = CompileViewportLibrary(device);
            const auto main = CompileViewportPipeline(device, library, @"viewport_vertex", @"viewport_fragment");
            const auto transforms = RequireBuffer(main.reflection.vertexBindings, 1, sizeof(MetalObjectTransforms));
            RequireMember(transforms.bufferStructType, @"mvp", offsetof(MetalObjectTransforms, mvp));
            RequireMember(transforms.bufferStructType, @"model", offsetof(MetalObjectTransforms, model));
            RequireLightingLayout(RequireBuffer(main.reflection.fragmentBindings, 1, sizeof(MetalSceneLighting)));
            RequireAtlasBindings(main.reflection.fragmentBindings);
            const auto selection = RequireBuffer(main.reflection.fragmentBindings, 0, sizeof(MetalSelectionStyle));
            RequireMember(selection.bufferStructType, @"color", offsetof(MetalSelectionStyle, color));
            RequireMember(selection.bufferStructType, @"strength", offsetof(MetalSelectionStyle, strength));
            const auto grid = CompileViewportPipeline(device, library, @"viewport_grid_vertex", @"viewport_grid_fragment");
            RequireBuffer(grid.reflection.vertexBindings, 1, sizeof(Math::Mat4));
            RequireBuffer(grid.reflection.fragmentBindings, 0, sizeof(MetalGridStyle));
            const auto shadow = CompileViewportPipeline(device, library, @"viewport_shadow_vertex", nil);
            const auto matrix = RequireBuffer(shadow.reflection.vertexBindings, 1, sizeof(Math::Mat4));
            CHECK(matrix.bufferDataType == MTLDataTypeFloat4x4);
        }
    }
}  // namespace Horo::Editor
