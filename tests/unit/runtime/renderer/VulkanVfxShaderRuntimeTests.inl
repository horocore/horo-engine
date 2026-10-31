/** @file @brief Exact VFX material owner/generation and selected-uniform admission cases using the existing Vulkan fixture. */
namespace {
    /** @brief Configures only the canonical VFX instance schema in the shared fixture. */
    void UseVfxParameters(Fixture &fixture) {
        fixture.deviceIncarnation = 11;
        fixture.manifest.bindings[0].stages = ShaderStageVisibility::Vertex;
        fixture.manifest.parameters = {{{1}, {1}, ShaderValueType::Float32, 4, 4, MaximumVfxDrawInstances},
                                       {{2}, {1}, ShaderValueType::Float32, 1, 4, MaximumVfxDrawInstances}};
        fixture.parameterOverride = {{{1}, {1}, ShaderValueType::Float32, 4, 4, MaximumVfxDrawInstances, 0, 64, 16, true, true},
                                     {{2}, {1}, ShaderValueType::Float32, 1, 4, MaximumVfxDrawInstances, 8192, 16, 0, true, true}};
        fixture.resolvedBufferBytes = 10240;
    }
}  // namespace

TEST_CASE("Vulkan VFX material admission retains exact native pipeline generation and selected uniform limits",
          "[vulkan][shader][material][vfx]") {
    Fixture f;
    UseVfxParameters(f);
    f.Prepare();
    const auto pipeline = f.Pipeline();
    REQUIRE(pipeline.HasValue());
    const auto material = f.Material();
    REQUIRE(VfxShaderParameterBytes(material.layout).Value() == f.resolvedBufferBytes);
    const std::array resources{MaterialResidentResource{MaterialResidentResourceKind::Buffer, 99, 0, f.resolvedBufferBytes}};
    auto table = f.runtime.RealizeResolved(material, pipeline.Value(), resources);
    REQUIRE(table.HasValue());
    auto *native = dynamic_cast<VulkanPreparedMaterialBinding *>(table.Value().get());
    REQUIRE(native != nullptr);
    REQUIRE(f.runtime.ValidateVfxMaterial(*native, pipeline.Value(), material.layout).HasValue());
    CHECK(native->resources[0].binding == VfxShaderParameterIds::Instances);
    CHECK(native->resources[0].offset == 0);
    CHECK(native->resources[0].byteCount == f.resolvedBufferBytes);
    SECTION("foreign selected device") {
        ++native->deviceIncarnation;
    }
    SECTION("retained pipeline with revoked registry generation") {
        f.runtime.DestroyGraphicsPipeline(pipeline.Value());
    }
    SECTION("another pipeline generation cannot replace the prepared one") {
        const auto replacement = f.Pipeline();
        REQUIRE(replacement.HasValue());
        CHECK(f.runtime.ValidateVfxMaterial(*native, replacement.Value(), material.layout).HasError());
        return;
    }
    SECTION("a forged owner cannot borrow an otherwise matching native pipeline") {
        static const VulkanShaderOwnerIdentity otherOwner{11};
        native->owner = &otherOwner;
    }
    CHECK(f.runtime.ValidateVfxMaterial(*native, pipeline.Value(), material.layout).HasError());
}

TEST_CASE("Vulkan rejects VFX reflection beyond the actual selected uniform range", "[vulkan][shader][material][vfx]") {
    Fixture f;
    UseVfxParameters(f);
    f.maximumUniformRange = 8192;
    f.Prepare();
    CHECK(f.Pipeline().HasError());
}
