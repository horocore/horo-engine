/** @file VulkanCoverageReduction.inl
 * @brief Closed SPIRV-Cross evidence and cook admission for stereo coverage reduction.
 */
namespace {
    using CoverageJson = ShaderReflectionCookDetail::Json;
    constexpr std::string_view CoverageReductionSource{"assets/shaders/runtime/multiview_coverage_reduce.hlsl"};
    constexpr std::string_view CoverageReductionEntry{"CSReduceCoverage"};
    constexpr std::uint32_t CoverageGroupExtent = 8;
    constexpr std::uint32_t CoverageCounterStride = 8;
    constexpr std::uint32_t CoverageRectangleBytes = 16;
    constexpr std::size_t MaximumCoveragePayload = 16U * 1024U * 1024U;

    /** @brief Describes descriptor ABI in native binding order. */
    struct CoverageBinding {
        std::string_view name;
        ShaderResourceKind kind;
        ShaderResourceAccess access{ShaderResourceAccess::ReadOnly};
    };

    constexpr std::array CoverageBindings{CoverageBinding{"CoverageLeft", ShaderResourceKind::SampledTexture},
                                          CoverageBinding{"CoverageRight", ShaderResourceKind::SampledTexture},
                                          CoverageBinding{"CoverageCounts", ShaderResourceKind::StorageBuffer,
                                                          ShaderResourceAccess::ReadWrite},
                                          CoverageBinding{"type.CoverageRectangles", ShaderResourceKind::UniformBuffer}};
    constexpr std::array CoverageRectangles{std::string_view{"LeftRect"}, std::string_view{"RightRect"}};
    constexpr ShaderBindingId CoverageRectanglesId{static_cast<std::uint32_t>(CoverageBindings.size())};

    constexpr ShaderTargetRequirement CoverageReductionTarget{.backend = ShaderTargetBackend::Vulkan,
                                                              .payloadFormat = ShaderPayloadFormat::SpirV16,
                                                              .descriptorVersion = 1,
                                                              .interfaceSchemaVersion = 1,
                                                              .maximumBindings = static_cast<std::uint32_t>(CoverageBindings.size()),
                                                              .maximumInlineConstantBytes = 128,
                                                              .supportsCompute = true,
                                                              .supportsStorageResources = true};

    /** @brief Preserves a typed failure for foreign or incomplete compiler evidence. */
    Error InvalidCoverageReflection() {
        return MakeError(ShaderReflectionErrors::InvalidReflection, "Stereo coverage reduction requires its exact cooked compute ABI.");
    }

    /** @brief Rejects floating, signed or out-of-range numeric ABI fields. */
    bool CoverageUnsigned(const CoverageJson &value, const std::uint64_t expected) {
        return value.is_number_unsigned() && value.get<std::uint64_t>() == expected;
    }

    /** @brief Checks a literal ABI name without implicit JSON conversion. */
    bool CoverageString(const CoverageJson &value, const std::string_view expected) {
        return value.is_string() && std::string_view{value.get_ref<const std::string &>()} == expected;
    }

    /** @brief Compares every field of one selected Vulkan target. */
    auto CoverageTargetFields(const ShaderTargetRequirement &target) {
        return std::tie(target.backend, target.payloadFormat, target.descriptorVersion, target.interfaceSchemaVersion,
                        target.maximumBindings, target.maximumInlineConstantBytes, target.supportsCompute, target.supportsStorageResources);
    }

    /** @brief Checks exact logical identity, native target and authored names before parsing evidence. */
    bool CoverageRequestMatches(const ShaderManifest &manifest, const ShaderTargetRequirement &target,
                                const std::span<const ShaderReflectionBindingName> names) {
        const auto canonical = StereoCoverageReductionManifest();
        if (manifest.sourceRevision != canonical.sourceRevision || manifest.targets.size() != 1 ||
            CoverageTargetFields(manifest.targets.front()) != CoverageTargetFields(CoverageReductionTarget) ||
            CoverageTargetFields(target) != CoverageTargetFields(CoverageReductionTarget))
            return false;
        const auto actual = ComputeShaderInterfaceCompatibilityId(manifest);
        const auto expected = ComputeShaderInterfaceCompatibilityId(canonical);
        if (actual.HasError() || expected.HasError() || actual.Value() != expected.Value())
            return false;
        return std::ranges::equal(names, StereoCoverageReductionReflectionNames(), [](const auto &actualName, const auto &expectedName) {
            return actualName.id == expectedName.id && actualName.name == expectedName.name &&
                   std::ranges::equal(actualName.parameters, expectedName.parameters, [](const auto &a, const auto &b) {
                return a.id == b.id && a.name == b.name;
            });
        });
    }

    /** @brief Rejects specialized or differently sized native workgroups. */
    bool CoverageEntryMatches(const CoverageJson &entry) {
        const auto literal = [&](const char *key) {
            return !entry.contains(key) || entry.at(key) == CoverageJson::array({false, false, false});
        };
        const auto &size = entry.at("workgroup_size");
        if (!CoverageString(entry.at("name"), CoverageReductionEntry) || !CoverageString(entry.at("mode"), "comp") || !size.is_array() ||
            size.size() != 3)
            return false;
        return CoverageUnsigned(size[0], CoverageGroupExtent) && CoverageUnsigned(size[1], CoverageGroupExtent) &&
               CoverageUnsigned(size[2], 1) && literal("workgroup_size_is_spec_constant_id") && literal("workgroup_size_is_spec_constant");
    }

    /** @brief Admits one non-array descriptor exactly at its canonical binding. */
    bool CoverageDescriptorMatches(const CoverageJson &resource, const std::size_t slot) {
        return CoverageString(resource.at("name"), CoverageBindings[slot].name) && CoverageUnsigned(resource.at("set"), 0) &&
               CoverageUnsigned(resource.at("binding"), slot) && !resource.contains("array") && !resource.contains("physical_pointer") &&
               !resource.contains("buffer_reference") && !resource.value("writeonly", false);
    }

    /** @brief Checks both ordered float image descriptors without relying on translator enumeration order. */
    bool CoverageImagesMatch(const CoverageJson &images) {
        if (!images.is_array() || images.size() != 2)
            return false;
        for (std::size_t eye = 0; eye < 2; ++eye) {
            if (std::ranges::none_of(images, [&](const auto &image) {
                return CoverageDescriptorMatches(image, eye) && CoverageString(image.at("type"), "texture2D");
            }))
                return false;
        }
        return true;
    }

    /** @brief Requires the actual writable uint2 runtime array with no padding or hidden members. */
    bool CoverageCountersMatch(const CoverageJson &block, const CoverageJson &members) {
        if (!CoverageDescriptorMatches(block, 2) || !CoverageUnsigned(block.at("block_size"), 0) || block.value("readonly", false) ||
            !members.is_array() || members.size() != 1)
            return false;
        const auto &member = members.front();
        const auto &shape = member.at("array");
        return CoverageString(member.at("type"), "uvec2") && CoverageUnsigned(member.at("offset"), 0) && shape.is_array() &&
               shape.size() == 1 && CoverageUnsigned(shape.front(), 0) &&
               member.at("array_size_is_literal") == CoverageJson::array({true}) &&
               CoverageUnsigned(member.at("array_stride"), CoverageCounterStride) && !member.contains("physical_pointer");
    }

    /** @brief Requires both exact uint4 rectangle fields with fixed offsets. */
    bool CoverageRectanglesMatch(const CoverageJson &block, const CoverageJson &members) {
        if (!CoverageDescriptorMatches(block, 3) ||
            !CoverageUnsigned(block.at("block_size"), CoverageRectangles.size() * CoverageRectangleBytes) || !members.is_array() ||
            members.size() != CoverageRectangles.size())
            return false;
        for (std::size_t index = 0; index < CoverageRectangles.size(); ++index) {
            const auto &member = members.at(index);
            if (!CoverageString(member.at("name"), CoverageRectangles[index]) || !CoverageString(member.at("type"), "uvec4") ||
                !CoverageUnsigned(member.at("offset"), index * CoverageRectangleBytes) || member.contains("array") ||
                member.contains("array_stride") || member.contains("matrix_stride") || member.contains("physical_pointer"))
                return false;
        }
        return true;
    }

    /** @brief Every resource and layout comes from the exact five-key compiler record. */
    bool CoverageEvidenceMatches(const CoverageJson &json) {
        if (!json.is_object() || json.size() != 5)
            return false;
        const auto &entry = json.at("entryPoints");
        const auto &types = json.at("types");
        if (!entry.is_array() || entry.size() != 1 || !CoverageEntryMatches(entry.front()) || !types.is_object() || types.size() != 2 ||
            !CoverageImagesMatch(json.at("separate_images")))
            return false;
        const auto members = [&](const CoverageJson &resource) -> const CoverageJson & {
            return types.at(resource.at("type").get_ref<const std::string &>()).at("members");
        };
        const auto &uniform = json.at("ubos");
        const auto &storage = json.at("ssbos");
        if (!uniform.is_array() || uniform.size() != 1 || !storage.is_array() || storage.size() != 1)
            return false;
        return CoverageRectanglesMatch(uniform.front(), members(uniform.front())) &&
               CoverageCountersMatch(storage.front(), members(storage.front()));
    }

    /** @brief Publishes typed final-artifact evidence only after validating actual IDs, fields and packing. */
    Result<NormalizedShaderReflection> ReflectCoverageReduction(const ShaderManifest &manifest, const ShaderTargetRequirement &target,
                                                                const std::span<const SpirvReflectionStage> stages,
                                                                const std::span<const ShaderReflectionBindingName> names) {
        using Outcome = Result<NormalizedShaderReflection>;
        if (!CoverageRequestMatches(manifest, target, names) || stages.size() != 1 || stages.front().stage != ShaderStage::Compute ||
            stages.front().entryPoint != CoverageReductionEntry)
            return Outcome::Failure(InvalidCoverageReflection());
        const auto &source = stages.front().json;
        if (source.empty() || source.size() > ShaderReflectionCookDetail::MaximumMetadataBytes)
            return Outcome::Failure(InvalidCoverageReflection());
        Foundation::JsonParseGuard guard{24};
        const auto json = guard.Parse(source.begin(), source.end());
        if (guard.HasDuplicate() || guard.IsTooDeep() || !CoverageEvidenceMatches(json))
            return Outcome::Failure(InvalidCoverageReflection());

        ShaderReflectionCandidate candidate{.backend = target.backend, .interfaceSchemaVersion = target.interfaceSchemaVersion};
        for (std::size_t index = 0; index < CoverageBindings.size(); ++index) {
            const ShaderBindingId id{static_cast<std::uint32_t>(index + 1)};
            const auto &binding = CoverageBindings[index];
            candidate.bindings.push_back({id, binding.kind, binding.access, 1, ShaderStageVisibility::Compute});
            candidate.targetBindings.push_back({.id = id, .nativeBinding = static_cast<std::uint32_t>(index)});
        }
        for (std::size_t index = 0; index < CoverageRectangles.size(); ++index)
            candidate.parameters.push_back({{static_cast<std::uint32_t>(index + 1)},
                                            CoverageRectanglesId,
                                            ShaderValueType::Uint32,
                                            1,
                                            4,
                                            1,
                                            static_cast<std::uint32_t>(index * CoverageRectangleBytes)});
        return NormalizeShaderReflection(manifest, target, candidate);
    }

    /** @brief Rechecks the exact required active descriptor slots and parameter offsets at the consumer boundary. */
    bool CoverageSidecarMatches(const NormalizedShaderReflection &reflection) {
        if (!reflection.stageInterface.empty() || reflection.parameters.size() != CoverageRectangles.size() ||
            reflection.bindings.size() != CoverageBindings.size() || reflection.targetBindings.size() != CoverageBindings.size())
            return false;
        for (std::size_t index = 0; index < CoverageBindings.size(); ++index) {
            const auto &binding = reflection.bindings[index];
            const auto &native = reflection.targetBindings[index];
            if (!binding.active || !native.active || native.id != binding.id || native.generatedHelperIndex != 0 ||
                native.nativeSpace != 0 || native.nativeBinding != index || native.pairedSampler.IsValid())
                return false;
        }
        for (std::size_t index = 0; index < CoverageRectangles.size(); ++index) {
            const auto &parameter = reflection.parameters[index];
            if (!parameter.active || parameter.byteOffset != index * CoverageRectangleBytes || parameter.arrayStride != 0 ||
                parameter.matrixStride != 0)
                return false;
        }
        return true;
    }
}  // namespace

/** @copydoc StereoCoverageReductionManifest */
ShaderManifest StereoCoverageReductionManifest() {
    ShaderManifest manifest{.schemaVersion = 1,
                            .sourceIdentity = std::string{CoverageReductionSource},
                            .sourceRevision = 1,
                            .entryPoints = {{ShaderStage::Compute, std::string{CoverageReductionEntry}}},
                            .targets = {CoverageReductionTarget}};
    for (std::size_t index = 0; index < CoverageBindings.size(); ++index) {
        const auto &binding = CoverageBindings[index];
        manifest.bindings.push_back(
            {{static_cast<std::uint32_t>(index + 1)}, binding.kind, binding.access, 1, ShaderStageVisibility::Compute});
    }
    for (std::size_t index = 0; index < CoverageRectangles.size(); ++index)
        manifest.parameters.push_back({{static_cast<std::uint32_t>(index + 1)}, CoverageRectanglesId, ShaderValueType::Uint32, 1, 4, 1});
    return manifest;
}

/** @copydoc StereoCoverageReductionReflectionNames */
std::vector<ShaderReflectionBindingName> StereoCoverageReductionReflectionNames() {
    std::vector<ShaderReflectionBindingName> names;
    for (std::size_t index = 0; index < CoverageBindings.size(); ++index)
        names.push_back({{static_cast<std::uint32_t>(index + 1)}, std::string{CoverageBindings[index].name}, {}});
    for (std::size_t index = 0; index < CoverageRectangles.size(); ++index)
        names.back().parameters.push_back({{static_cast<std::uint32_t>(index + 1)}, std::string{CoverageRectangles[index]}});
    return names;
}

/** @copydoc ValidateStereoCoverageReductionCook */
Result<ReflectedShaderArtifact> ValidateStereoCoverageReductionCook(const std::span<const std::uint8_t> payload,
                                                                    const std::span<const std::uint8_t> metadata) {
    using Outcome = Result<ReflectedShaderArtifact>;
    const auto stages =
        DecodeShaderArtifactStages(payload, CoverageReductionTarget.backend, CoverageReductionTarget.payloadFormat, MaximumCoveragePayload);
    if (stages.HasError())
        return Outcome::Failure(stages.ErrorValue());
    if (stages.Value().stageCount != 1 || stages.Value().stages.front().stage != ShaderStage::Compute ||
        stages.Value().stages.front().entryPoint != CoverageReductionEntry)
        return Outcome::Failure(InvalidCoverageReflection());
    auto reflection = DecodeReflectedShaderArtifact(StereoCoverageReductionManifest(), CoverageReductionTarget, payload, metadata);
    if (reflection.HasError())
        return reflection;
    if (!CoverageSidecarMatches(reflection.Value().reflection))
        return Outcome::Failure(InvalidCoverageReflection());
    return reflection;
}
