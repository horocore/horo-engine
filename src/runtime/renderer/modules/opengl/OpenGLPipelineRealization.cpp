#include "OpenGLPipelineRealization.h"

#include "OpenGLRenderBackendErrors.h"

#include <algorithm>
#include <array>
#include <glad/gl.h>
#include <memory>
#include <string_view>

namespace Horo::Render::Detail {
    namespace {
        /** @brief Retains an unpublished program across native reflection and owned metadata failures. */
        struct ProgramDelete {
            void (*destroy)(std::uint32_t) noexcept;

            void operator()(const std::uint32_t *program) const noexcept {
                destroy(*program);
            }
        };

        /** @brief Returns an actionable typed final-artifact mismatch without hiding driver link diagnostics. */
        [[nodiscard]] Result<void> Mismatch(const char *message) {
            return Result<void>::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid, message));
        }

        /** @brief Maps portable scalar/vector/matrix shape to a private GL4.1 reflection type. */
        [[nodiscard]] GLenum ValueType(const ShaderValueType type, const std::uint8_t rows, const std::uint8_t columns) noexcept {
            if (rows < 1 || rows > 4 || columns < 1 || columns > 4)
                return 0;
            if (rows == 1 || columns == 1) {
                const auto components = std::max(rows, columns);
                constexpr std::array floats{GL_FLOAT, GL_FLOAT_VEC2, GL_FLOAT_VEC3, GL_FLOAT_VEC4};
                constexpr std::array integers{GL_INT, GL_INT_VEC2, GL_INT_VEC3, GL_INT_VEC4};
                constexpr std::array unsignedIntegers{GL_UNSIGNED_INT, GL_UNSIGNED_INT_VEC2, GL_UNSIGNED_INT_VEC3, GL_UNSIGNED_INT_VEC4};
                constexpr std::array booleans{GL_BOOL, GL_BOOL_VEC2, GL_BOOL_VEC3, GL_BOOL_VEC4};
                switch (type) {
                    case ShaderValueType::Float32:
                        return floats[components - 1];
                    case ShaderValueType::Int32:
                        return integers[components - 1];
                    case ShaderValueType::Uint32:
                        return unsignedIntegers[components - 1];
                    case ShaderValueType::Bool32:
                        return booleans[components - 1];
                }
            }
            if (type != ShaderValueType::Float32 || rows < 2 || columns < 2)
                return 0;
            constexpr std::array<std::array<GLenum, 3>, 3> matrices{{{GL_FLOAT_MAT2, GL_FLOAT_MAT2x3, GL_FLOAT_MAT2x4},
                                                                     {GL_FLOAT_MAT3x2, GL_FLOAT_MAT3, GL_FLOAT_MAT3x4},
                                                                     {GL_FLOAT_MAT4x2, GL_FLOAT_MAT4x3, GL_FLOAT_MAT4}}};
            return matrices[columns - 2][rows - 2];
        }

        /** @brief Checks actual active attributes against the established MeshVertex layout and cooked locations. */
        [[nodiscard]] Result<void> ValidateAttributes(const GLuint program, const NormalizedShaderReflection &reflection) {
            GLint count{}, maximumName{};
            glGetProgramiv(program, GL_ACTIVE_ATTRIBUTES, &count);
            glGetProgramiv(program, GL_ACTIVE_ATTRIBUTE_MAX_LENGTH, &maximumName);
            const auto expected = std::ranges::count_if(reflection.stageInterface, [](const auto &variable) {
                return variable.stage == ShaderStage::Vertex && variable.direction == ShaderInterfaceDirection::Input;
            });
            if (count != expected || count < 0 || count > 3 || maximumName > 257)
                return Mismatch("Linked vertex attributes differ from bounded cooked MeshVertex reflection.");
            for (GLint index = 0; index < count; ++index) {
                std::array<char, 257> name{};
                GLsizei written{};
                GLint size{};
                GLenum type{};
                glGetActiveAttrib(program, static_cast<GLuint>(index), static_cast<GLsizei>(name.size()), &written, &size, &type,
                                  name.data());
                if (written <= 0 || written >= static_cast<GLsizei>(name.size()))
                    return Mismatch("Linked vertex attribute name exceeds the finite target-name envelope.");
                const GLint location = glGetAttribLocation(program, name.data());
                const auto match = std::ranges::find_if(reflection.stageInterface, [location](const auto &variable) {
                    return variable.stage == ShaderStage::Vertex && variable.direction == ShaderInterfaceDirection::Input &&
                           location >= 0 && variable.location == static_cast<std::uint32_t>(location);
                });
                const GLenum meshType = location == 0 || location == 1 ? GL_FLOAT_VEC3 : location == 2 ? GL_FLOAT_VEC2 : 0;
                if (match == reflection.stageInterface.end() || size != 1 || match->arrayCount != 1 || type != meshType ||
                    type != ValueType(match->type, match->rows, match->columns))
                    return Mismatch("Linked vertex attribute type/location does not match cooked reflection and MeshVertex.");
            }
            return Result<void>::Success();
        }

        /** @brief GL4.1 has output location lookup, not GL4.3 program-interface enumeration. */
        [[nodiscard]] Result<void> ValidateOutputs(const GLuint program, const NormalizedShaderReflection &reflection) {
            for (const auto &variable : reflection.stageInterface) {
                if (variable.stage != ShaderStage::Fragment || variable.direction != ShaderInterfaceDirection::Output)
                    continue;
                if (variable.nativeName.empty() || variable.nativeName.size() > 256 || variable.location != 0 ||
                    ValueType(variable.type, variable.rows, variable.columns) != GL_FLOAT_VEC4 || variable.arrayCount != 1)
                    return Mismatch("GL4.1 color output requires a bounded actual native name and location-zero float4 reflection.");
                if (glGetFragDataLocation(program, variable.nativeName.c_str()) != static_cast<GLint>(variable.location))
                    return Mismatch("Linked fragment output location differs from the final cooked artifact.");
            }
            return Result<void>::Success();
        }

        /** @brief Fixed native packing evidence queried once per active member, independently of comparison count. */
        struct UniformPacking {
            GLint type{}, size{}, offset{}, arrayStride{}, matrixStride{}, rowMajor{};
        };

        /** @brief Queries exactly six finite native facts for one member. */
        [[nodiscard]] UniformPacking QueryPacking(const GLuint program, const GLuint index) noexcept {
            UniformPacking packing;
            glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_TYPE, &packing.type);
            glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_SIZE, &packing.size);
            glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_OFFSET, &packing.offset);
            glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_ARRAY_STRIDE, &packing.arrayStride);
            glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_MATRIX_STRIDE, &packing.matrixStride);
            glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_IS_ROW_MAJOR, &packing.rowMajor);
            return packing;
        }

        /** @brief Compares exact shape and packing without inventing logical-to-native member names. */
        [[nodiscard]] bool MatchesParameter(const UniformPacking &packing, const ShaderReflectedParameter &parameter) noexcept {
            return packing.type == static_cast<GLint>(ValueType(parameter.type, parameter.rows, parameter.columns)) && packing.size > 0 &&
                   static_cast<std::uint32_t>(packing.size) == parameter.arrayCount && packing.offset >= 0 &&
                   static_cast<std::uint32_t>(packing.offset) == parameter.byteOffset && packing.arrayStride >= 0 &&
                   packing.matrixStride >= 0 && static_cast<std::uint32_t>(packing.arrayStride) == parameter.arrayStride &&
                   static_cast<std::uint32_t>(packing.matrixStride) == parameter.matrixStride &&
                   (parameter.rows == 1 || parameter.columns == 1 || (packing.rowMajor == GL_FALSE) == parameter.columnMajor);
        }

        /** @brief Validates a linked uniform block's complete active packing before assigning its candidate binding. */
        [[nodiscard]] Result<GLint> ValidateBlock(const GLuint program, const ShaderTargetBindingMapEntry &mapping,
                                                  const NormalizedShaderReflection &reflection, const GLint maximumBindings,
                                                  const GLint maximumBytes) {
            if (mapping.nativeBinding >= static_cast<std::uint32_t>(maximumBindings))
                return Result<GLint>::Failure(
                    MakeError(OpenGLBackendErrors::UnsupportedResourceOperation, "Uniform binding exceeds actual device capacity."));
            const GLuint block = glGetUniformBlockIndex(program, mapping.nativeName.c_str());
            if (block == GL_INVALID_INDEX)
                return Result<GLint>::Failure(
                    MakeError(OpenGLBackendErrors::ResourceIdentityInvalid, "Active cooked uniform block is absent after linking."));
            GLint count{}, bytes{};
            glGetActiveUniformBlockiv(program, block, GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS, &count);
            glGetActiveUniformBlockiv(program, block, GL_UNIFORM_BLOCK_DATA_SIZE, &bytes);
            const auto expected = std::ranges::count_if(reflection.parameters, [&mapping](const auto &parameter) {
                return parameter.active && parameter.binding == mapping.id;
            });
            if (count < 0 || count > 512 || count != expected || bytes <= 0 || bytes > maximumBytes)
                return Result<GLint>::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid,
                                                        "Linked uniform block extent/member count differs from cooked packing."));
            std::array<GLint, 512> indices{};
            std::array<bool, 512> matched{};
            glGetActiveUniformBlockiv(program, block, GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES, indices.data());
            for (GLint index = 0; index < count; ++index) {
                if (indices[index] < 0)
                    return Result<GLint>::Failure(
                        MakeError(OpenGLBackendErrors::ResourceIdentityInvalid, "Linked uniform index is invalid."));
                const UniformPacking packing = QueryPacking(program, static_cast<GLuint>(indices[index]));
                bool found = false;
                for (std::size_t parameterIndex = 0; parameterIndex < reflection.parameters.size(); ++parameterIndex) {
                    const auto &parameter = reflection.parameters[parameterIndex];
                    if (!matched[parameterIndex] && parameter.active && parameter.binding == mapping.id &&
                        MatchesParameter(packing, parameter)) {
                        matched[parameterIndex] = true;
                        found = true;
                        break;
                    }
                }
                if (!found)
                    return Result<GLint>::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid,
                                                            "Linked uniform member type/array/offset/stride differs from cooked packing."));
            }
            glUniformBlockBinding(program, block, mapping.nativeBinding);
            return Result<GLint>::Success(count);
        }

        /** @brief Checks one actual combined sampler uniform and maps only the unpublished program. */
        [[nodiscard]] Result<void> ValidateTexture(const GLuint program, const ShaderTargetBindingMapEntry &mapping,
                                                   const NormalizedShaderReflection &reflection, const GLint maximumUnits) {
            const auto sampler = std::ranges::find_if(reflection.bindings, [&mapping](const auto &binding) {
                return binding.id == mapping.pairedSampler && binding.active && binding.kind == ShaderResourceKind::Sampler &&
                       binding.arrayCount == 1;
            });
            if (sampler == reflection.bindings.end() || mapping.nativeBinding >= static_cast<std::uint32_t>(maximumUnits))
                return Mismatch("Active sampled texture needs an exact paired sampler and an admitted native texture unit.");
            const char *name = mapping.nativeName.c_str();
            GLuint index{};
            glGetUniformIndices(program, 1, &name, &index);
            if (index == GL_INVALID_INDEX)
                return Mismatch("Active cooked sampled texture is absent from the linked program.");
            GLint type{}, size{}, block{};
            glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_TYPE, &type);
            glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_SIZE, &size);
            glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_BLOCK_INDEX, &block);
            const GLint location = glGetUniformLocation(program, name);
            if (location < 0 || block != -1 || size != 1 || (type != GL_SAMPLER_2D && type != GL_SAMPLER_2D_SHADOW))
                return Mismatch("Linked sampled texture has an unsupported or mismatched native uniform type/array/location.");
            const GLint unit = static_cast<GLint>(mapping.nativeBinding);
            glProgramUniform1iv(program, location, 1, &unit);
            return Result<void>::Success();
        }

        /** @brief Checks every active native resource; undeclared or silently omitted uniform resources fail closed. */
        [[nodiscard]] Result<void> ValidateBindings(const GLuint program, const RenderGraphicsPipelineDescriptor &descriptor) {
            GLint maximumBindings{}, maximumBytes{}, maximumUnits{}, actualBlocks{}, actualUniforms{};
            glGetIntegerv(GL_MAX_UNIFORM_BUFFER_BINDINGS, &maximumBindings);
            glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &maximumBytes);
            glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &maximumUnits);
            glGetProgramiv(program, GL_ACTIVE_UNIFORM_BLOCKS, &actualBlocks);
            glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &actualUniforms);
            if (maximumBindings <= 0 || maximumBytes <= 0 || maximumUnits <= 0)
                return Mismatch("Actual GL4.1 uniform/sampler capacities are unavailable.");
            GLint expectedBlocks{}, expectedUniforms{};
            for (const auto &mapping : descriptor.reflection.targetBindings) {
                if (!mapping.active)
                    continue;
                const auto binding = std::ranges::find(descriptor.reflection.bindings, mapping.id, &ShaderReflectedBinding::id);
                if (binding == descriptor.reflection.bindings.end() || !binding->active || mapping.generatedHelperIndex != 0 ||
                    mapping.nativeSpace != 0 || mapping.nativeName.empty() || mapping.nativeName.size() > 256 || binding->arrayCount != 1)
                    return Mismatch("GL4.1 target mapping requires bounded active scalar resources without generated helpers.");
                if (binding->kind == ShaderResourceKind::UniformBuffer) {
                    const auto checked = ValidateBlock(program, mapping, descriptor.reflection, maximumBindings, maximumBytes);
                    if (checked.HasError())
                        return Result<void>::Failure(checked.ErrorValue());
                    ++expectedBlocks;
                    expectedUniforms += checked.Value();
                } else if (binding->kind == ShaderResourceKind::SampledTexture) {
                    if (const auto checked = ValidateTexture(program, mapping, descriptor.reflection, maximumUnits); checked.HasError())
                        return checked;
                    ++expectedUniforms;
                } else if (binding->kind != ShaderResourceKind::Sampler) {
                    return Result<void>::Failure(MakeError(OpenGLBackendErrors::UnsupportedResourceOperation,
                                                           "GL4.1 graphics realization does not implement storage resources."));
                }
            }
            if (actualBlocks != expectedBlocks || actualUniforms != expectedUniforms)
                return Mismatch("Linked program contains missing, extra or unaccounted uniform resources.");
            return Result<void>::Success();
        }

        /** @brief Queries finite final-program evidence and mutates only candidate-local resource bindings. */
        [[nodiscard]] Result<void> ValidateAndMap(const std::uint32_t program, const RenderGraphicsPipelineDescriptor &descriptor) {
            if (const auto valid = ValidateRenderGraphicsPipelineDescriptor(descriptor); valid.HasError())
                return Result<void>::Failure(valid.ErrorValue());
            if (!descriptor.manifest.inlineConstants.empty() || !descriptor.manifest.specializationInputs.empty())
                return Result<void>::Failure(
                    MakeError(OpenGLBackendErrors::UnsupportedResourceOperation,
                              "GL4.1 graphics realization requires cooked specialization and explicit buffer parameters."));
            if (glGetError() != GL_NO_ERROR)
                return Result<void>::Failure(
                    MakeError(OpenGLBackendErrors::CommandFailed, "Outstanding native error before final program reflection."));
            if (const auto attributes = ValidateAttributes(program, descriptor.reflection); attributes.HasError())
                return attributes;
            if (const auto outputs = ValidateOutputs(program, descriptor.reflection); outputs.HasError())
                return outputs;
            if (const auto bindings = ValidateBindings(program, descriptor); bindings.HasError())
                return bindings;
            if (glGetError() != GL_NO_ERROR)
                return Result<void>::Failure(
                    MakeError(OpenGLBackendErrors::CommandFailed, "Native final-program reflection or candidate binding mapping failed."));
            return Result<void>::Success();
        }

        /** @brief Reads bounded facts from the selected current context without creating or binding native objects. */
        Result<RenderPipelineDeviceIdentity> QueryDeviceIdentity() {
            if (!glGetString || !glGetError)
                return Result<RenderPipelineDeviceIdentity>::Failure(MakeError(OpenGLBackendErrors::MissingRequiredEntryPoints));
            if (glGetError() != GL_NO_ERROR)
                return Result<RenderPipelineDeviceIdentity>::Failure(MakeError(OpenGLBackendErrors::CommandFailed));
            constexpr std::array names{GL_VENDOR, GL_RENDERER, GL_VERSION, GL_SHADING_LANGUAGE_VERSION};
            std::array<std::string_view, 4> facts;
            for (std::size_t index = 0; index < names.size(); ++index) {
                const auto *value = reinterpret_cast<const char *>(glGetString(names[index]));
                if (!value)
                    return Result<RenderPipelineDeviceIdentity>::Failure(MakeError(OpenGLBackendErrors::UnsupportedResourceOperation));
                std::size_t length = 0;
                while (length < 120 && value[length] != '\0')
                    ++length;
                if (length == 0 || length == 120)
                    return Result<RenderPipelineDeviceIdentity>::Failure(
                        MakeError(OpenGLBackendErrors::UnsupportedResourceOperation,
                                  "Selected OpenGL context identity is absent or exceeds the finite cache identity envelope."));
                facts[index] = {value, length};
            }
            if (glGetError() != GL_NO_ERROR)
                return Result<RenderPipelineDeviceIdentity>::Failure(MakeError(OpenGLBackendErrors::CommandFailed));
            Sha256Builder hash;
            for (const auto fact : facts) {
                const std::array length{static_cast<std::byte>(fact.size())};
                static_cast<void>(hash.Update(length));
                static_cast<void>(hash.Update(std::as_bytes(std::span{fact.data(), fact.size()})));
            }
            const auto digest = hash.Finalize();
            // The transitional presentation port selects its default context. This stable implementation identity is
            // scoped to that selection's compatibility facts; it is not a physical-device discovery ordinal or handle.
            return Result<RenderPipelineDeviceIdentity>::Success({.backend = RenderBackendId{"opengl"},
                                                                  .backendModuleVersion = 1,
                                                                  .adapter = RenderAdapterId{"opengl." + FormatSha256(digest).substr(7)},
                                                                  .deviceCompatibilityDigest = digest,
                                                                  .driverIdentity = std::string{facts[0]} + ":" + std::string{facts[1]},
                                                                  .driverVersion = std::string{facts[2]}});
        }

        /** @brief Queries the linked uniform block's complete ABI size, including native tail padding. */
        Result<std::size_t> UniformBlockSize(const std::uint32_t program, const std::string &name) {
            if (!glGetUniformBlockIndex || !glGetActiveUniformBlockiv || !glGetError)
                return Result<std::size_t>::Failure(MakeError(OpenGLBackendErrors::MissingRequiredEntryPoints));
            if (glGetError() != GL_NO_ERROR)
                return Result<std::size_t>::Failure(MakeError(OpenGLBackendErrors::CommandFailed));
            const auto block = glGetUniformBlockIndex(program, name.c_str());
            if (block == GL_INVALID_INDEX)
                return Result<std::size_t>::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid));
            GLint size = 0;
            glGetActiveUniformBlockiv(program, block, GL_UNIFORM_BLOCK_DATA_SIZE, &size);
            if (glGetError() != GL_NO_ERROR || size <= 0)
                return Result<std::size_t>::Failure(MakeError(OpenGLBackendErrors::CommandFailed));
            return Result<std::size_t>::Success(static_cast<std::size_t>(size));
        }

        /** @brief Distinguishes final sampler2D and sampler2DShadow without guessing from resource names. */
        Result<bool> TextureComparison(const std::uint32_t program, const std::string &name) {
            if (!glGetUniformIndices || !glGetActiveUniformsiv || !glGetError)
                return Result<bool>::Failure(MakeError(OpenGLBackendErrors::MissingRequiredEntryPoints));
            if (glGetError() != GL_NO_ERROR)
                return Result<bool>::Failure(MakeError(OpenGLBackendErrors::CommandFailed));
            const char *nativeName = name.c_str();
            GLuint index = GL_INVALID_INDEX;
            glGetUniformIndices(program, 1, &nativeName, &index);
            if (index == GL_INVALID_INDEX)
                return Result<bool>::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid));
            GLint type = 0;
            glGetActiveUniformsiv(program, 1, &index, GL_UNIFORM_TYPE, &type);
            if (glGetError() != GL_NO_ERROR || (type != GL_SAMPLER_2D && type != GL_SAMPLER_2D_SHADOW))
                return Result<bool>::Failure(MakeError(OpenGLBackendErrors::ResourceIdentityInvalid));
            return Result<bool>::Success(type == GL_SAMPLER_2D_SHADOW);
        }

        /** @brief Queries actual uniform range alignment before material admission. */
        Result<std::size_t> UniformOffsetAlignment() {
            if (!glGetIntegerv || !glGetError)
                return Result<std::size_t>::Failure(MakeError(OpenGLBackendErrors::MissingRequiredEntryPoints));
            if (glGetError() != GL_NO_ERROR)
                return Result<std::size_t>::Failure(MakeError(OpenGLBackendErrors::CommandFailed));
            GLint alignment = 0;
            glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &alignment);
            if (glGetError() != GL_NO_ERROR || alignment <= 0)
                return Result<std::size_t>::Failure(MakeError(OpenGLBackendErrors::CommandFailed));
            return Result<std::size_t>::Success(static_cast<std::size_t>(alignment));
        }

        bool IsAvailable() noexcept {
            return glGetProgramiv && glGetActiveAttrib && glGetAttribLocation && glGetFragDataLocation && glGetUniformBlockIndex &&
                   glGetActiveUniformBlockiv && glGetActiveUniformsiv && glUniformBlockBinding && glGetUniformIndices &&
                   glGetUniformLocation && glProgramUniform1iv && glGetIntegerv && glGetError;
        }
    }  // namespace

    /** @copydoc OpenGLReflectedValueType */
    std::uint32_t OpenGLReflectedValueType(const ShaderValueType type, const std::uint8_t rows, const std::uint8_t columns) noexcept {
        return ValueType(type, rows, columns);
    }

    /** @copydoc CreateOpenGLGraphicsProgram */
    Result<std::uint32_t> CreateOpenGLGraphicsProgram(const std::uint32_t vertex, const std::uint32_t fragment,
                                                      const RenderGraphicsPipelineDescriptor &descriptor,
                                                      const OpenGLShaderFunctions &shaders, const OpenGLPipelineFunctions &pipelines) {
        if (!pipelines.IsValid() || !pipelines.isAvailable())
            return Result<std::uint32_t>::Failure(MakeError(OpenGLBackendErrors::UnsupportedResourceOperation));
        const auto linked = LinkOpenGLCookedGraphicsProgram(vertex, fragment, shaders);
        if (linked.HasError())
            return linked;
        const std::uint32_t program = linked.Value();
        std::unique_ptr<const std::uint32_t, ProgramDelete> candidate{&program, ProgramDelete{shaders.deleteProgram}};
        if (const auto validated = pipelines.validateAndMap(program, descriptor); validated.HasError())
            return Result<std::uint32_t>::Failure(validated.ErrorValue());
        static_cast<void>(candidate.release());
        return Result<std::uint32_t>::Success(program);
    }

    /** @copydoc ProductionOpenGLPipelineFunctions */
    OpenGLPipelineFunctions ProductionOpenGLPipelineFunctions() noexcept {
        return {.isAvailable = &IsAvailable, .validateAndMap = &ValidateAndMap, .queryDeviceIdentity = &QueryDeviceIdentity};
    }

    /** @copydoc ProductionOpenGLMaterialFunctions */
    OpenGLMaterialFunctions ProductionOpenGLMaterialFunctions() noexcept {
        return {.uniformBlockSize = &UniformBlockSize,
                .textureComparison = &TextureComparison,
                .uniformOffsetAlignment = &UniformOffsetAlignment};
    }
}  // namespace Horo::Render::Detail
