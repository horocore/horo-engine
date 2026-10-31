/** @brief Fault-dispatch ownership tests; no real context, shader execution or GPU pixel evidence. */
namespace {
    PipelineFixture PrepareVfxPipeline(RenderFrontend &frontend, const RenderGraphicsPipelineState &state = {}) {
        const ShaderTargetRequirement target{.backend = ShaderTargetBackend::OpenGL,
                                             .payloadFormat = ShaderPayloadFormat::Glsl410,
                                             .descriptorVersion = 1,
                                             .interfaceSchemaVersion = 1,
                                             .maximumBindings = 16,
                                             .maximumInlineConstantBytes = 128};
        ShaderManifest manifest{.schemaVersion = 1,
                                .sourceIdentity = "tests/vfx-frame.hlsl",
                                .sourceRevision = 1,
                                .entryPoints = {{ShaderStage::Vertex, "VertexMain"}, {ShaderStage::Fragment, "FragmentMain"}},
                                .bindings = {{{1},
                                              ShaderResourceKind::UniformBuffer,
                                              ShaderResourceAccess::ReadOnly,
                                              1,
                                              ShaderStageVisibility::Vertex}},
                                .parameters = {{{1}, {1}, ShaderValueType::Float32, 4, 4, 128},
                                               {{2}, {1}, ShaderValueType::Float32, 1, 4, 128}},
                                .targets = {target}};
        auto reflection = MaterialReflection();
        reflection.bindings = {
            {{1}, ShaderResourceKind::UniformBuffer, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Vertex, true}};
        reflection.parameters = {{{1}, {1}, ShaderValueType::Float32, 4, 4, 128, 0, 64, 16, true, true},
                                 {{2}, {1}, ShaderValueType::Float32, 1, 4, 128, 8192, 16, 0, false, true}};
        reflection.targetBindings = {{{1}, 0, 0, 0, "VfxInstances", {}, true}};
        constexpr std::string_view vertex =
            "#version 410 core\nlayout(location=0) in vec3 Position;out vec2 UV;layout(std140) uniform VfxInstances{"
            "mat4 ClipFromLocal[128];vec4 InstanceColor[128];};void main(){UV=InstanceColor[gl_InstanceID].xy;"
            "gl_Position=ClipFromLocal[gl_InstanceID]*vec4(Position,1);}";
        constexpr std::string_view fragment =
            "#version 410 core\nin vec2 UV;layout(location=0) out vec4 Color;void main(){Color=vec4(UV,0,1);}";
        return PreparePipelineSources(frontend, target, manifest, reflection, vertex, fragment, state);
    }

    struct VfxSlotFixture final {
        PipelineFixture pipeline;
        RenderBufferHandle buffer;
        std::unique_ptr<MaterialBindingTable> table;
        std::shared_ptr<PreparedVfxFrameSlot> slot;
    };

    struct VfxMaterialFixture final {
        PipelineFixture pipeline;
        RenderBufferHandle buffer;
        std::unique_ptr<MaterialBindingTable> table;
        std::shared_ptr<const ResidentMaterialBinding> material;
    };

    VfxMaterialFixture PrepareVfxMaterial(RenderFrontend &frontend, const RenderGraphicsPipelineState &state = {}) {
        native.uniformBytes = sizeof(VfxDrawUniformBlock);
        auto pipeline = PrepareVfxPipeline(frontend, state);
        const auto buffer = frontend.CreateBuffer({.byteSize = native.uniformBytes,
                                                   .usage = RenderBufferUsage::Uniform,
                                                   .access = RenderBufferAccess::HostVisible},
                                                  {});
        REQUIRE(buffer.HasValue());
        REQUIRE(frontend.ProcessResourceRequests().HasValue());
        auto table = MaterialBindingTable::Create(pipeline.pipeline.owner, frontend);
        REQUIRE(table.HasValue());
        MaterialBindingDescriptor material{.material = MaterialRuntimeId{27},
                                           .revision = 1,
                                           .pipeline = pipeline.pipeline,
                                           .layout = pipeline.layout,
                                           .resources = {{{1}, 0, MaterialBufferBinding{buffer.Value().handle, 0, native.uniformBytes}}}};
        const auto generation = table.Value()->Publish(std::move(material));
        REQUIRE(generation.HasValue());
        auto acquired = table.Value()->Acquire(generation.Value().binding);
        REQUIRE(acquired.HasValue());
        return {std::move(pipeline), buffer.Value().handle, std::move(table).Value(), std::move(acquired).Value()};
    }

    VfxSlotFixture PrepareVfxSlot(RenderFrontend &frontend) {
        auto source = PrepareVfxMaterial(frontend);
        auto slot = frontend.PrepareVfxFrameSlot(std::move(source.material));
        REQUIRE(slot.HasValue());
        return {std::move(source.pipeline), source.buffer, std::move(source.table), std::move(slot).Value()};
    }
}  // namespace

TEST_CASE("VFX issued slot rejects rollback CPU borrowers and invalid data before native writes", "[renderer][opengl][vfx][unit]") {
    OpenGLResourceTests::ResourcePresentationPort port;
    auto frontend = Create(port);
    auto fixture = PrepareVfxSlot(*frontend);
    const std::array<VfxPackedDrawInstance, 1> instances{};
    REQUIRE(frontend->UpdateVfxFrame(fixture.slot, 1, instances).HasValue());
    REQUIRE(fixture.slot->Revision() == 1);
    CHECK(native.uploads == 1);
    CHECK(frontend->UpdateVfxFrame(fixture.slot, 1, instances).HasError());
    auto graphBorrower = fixture.slot;
    CHECK(frontend->UpdateVfxFrame(fixture.slot, 2, instances).HasError());
    graphBorrower.reset();
    auto malformed = instances;
    malformed[0].linearColor[2] = std::numeric_limits<float>::infinity();
    CHECK(frontend->UpdateVfxFrame(fixture.slot, 2, malformed).HasError());
    CancellationSource cancellation;
    cancellation.RequestCancellation();
    CHECK(frontend->UpdateVfxFrame(fixture.slot, 2, instances, cancellation.Token()).HasError());
    CHECK(native.uploads == 1);
    CHECK(fixture.slot->Revision() == 1);
    REQUIRE(frontend->ReleaseBuffer(fixture.buffer).HasValue());
    CHECK(frontend->UpdateVfxFrame(fixture.slot, 2, instances).HasError());
}

TEST_CASE("VFX uniform reuse honors ordinary material alias partial failure and native completion", "[renderer][opengl][vfx][unit]") {
    OpenGLResourceTests::ResourcePresentationPort port;
    auto frontend = Create(port);
    auto fixture = PrepareVfxSlot(*frontend);
    const auto mesh = PrepareMesh(*frontend);
    const auto target = PrepareTarget(*frontend);
    const std::array<VfxPackedDrawInstance, 1> instances{};
    REQUIRE(frontend->UpdateVfxFrame(fixture.slot, 1, instances).HasValue());
    OpenGLBackendTests::commandState.pollStatus = 0x911B;  // GL_TIMEOUT_EXPIRED
    native.failDraw = true;
    auto begun = frontend->BeginFrame({.frameNumber = 1, .outputExtent = {64, 32}});
    REQUIRE(begun.HasValue());
    auto frame = std::move(begun).Value();
    const std::array draws{RenderMaterialDraw{mesh, fixture.slot->Material()}};
    REQUIRE(frame.ExecuteMaterialPass({target.target, {64, 32}, {}, 1.0F, draws}).HasError());
    frame.Cancel();
    CHECK(OpenGLBackendTests::commandState.fenceCount > 0);
    const auto pending = frontend->UpdateVfxFrame(fixture.slot, 2, instances);
    REQUIRE(pending.HasError());
    CHECK(pending.ErrorValue().code.Value() == VfxFrameErrors::Pending.code.Value());
    CHECK(native.uploads == 1);
    CHECK(fixture.slot->Revision() == 1);
    SECTION("actual completion permits overwrite") {
        OpenGLBackendTests::commandState.pollStatus = 0x911C;  // GL_CONDITION_SATISFIED
        REQUIRE(frontend->UpdateVfxFrame(fixture.slot, 2, instances).HasValue());
        CHECK(native.uploads == 2);
        CHECK(fixture.slot->Revision() == 2);
    }
    SECTION("failed polling never permits overwrite") {
        OpenGLBackendTests::commandState.pollStatus = 0x911D;  // GL_WAIT_FAILED
        CHECK(frontend->UpdateVfxFrame(fixture.slot, 2, instances).HasError());
        OpenGLBackendTests::commandState.pollStatus = 0x911C;
        CHECK(frontend->UpdateVfxFrame(fixture.slot, 2, instances).HasError());
        CHECK(native.uploads == 1);
        CHECK(fixture.slot->Revision() == 1);
    }
}

TEST_CASE("VFX native write failure invalidates publication and shutdown revokes issued slots", "[renderer][opengl][vfx][unit]") {
    OpenGLResourceTests::ResourcePresentationPort port;
    auto frontend = Create(port);
    auto fixture = PrepareVfxSlot(*frontend);
    const std::array<VfxPackedDrawInstance, 1> instances{};
    REQUIRE(frontend->UpdateVfxFrame(fixture.slot, 1, instances).HasValue());
    native.failUpload = true;
    CHECK(frontend->UpdateVfxFrame(fixture.slot, 2, instances).HasError());
    CHECK(fixture.slot->Revision() == 0);
    native.failUpload = false;
    REQUIRE(frontend->UpdateVfxFrame(fixture.slot, 2, instances).HasValue());
    REQUIRE(frontend->BeginShutdown().HasValue());
    CHECK(frontend->UpdateVfxFrame(fixture.slot, 3, instances).HasError());
}

TEST_CASE("VFX slot issuance rejects ordinary material schemas and foreign frontend incarnations", "[renderer][opengl][vfx][unit]") {
    OpenGLResourceTests::ResourcePresentationPort port;
    auto frontend = Create(port);
    SECTION("ordinary material does not acquire a VFX slot by buffer size") {
        auto ordinary = Prepare(*frontend);
        CHECK(frontend->PrepareVfxFrameSlot(ordinary.material).HasError());
        CHECK(native.uploads == 0);
    }
    SECTION("another frontend cannot issue or update this slot") {
        auto fixture = PrepareVfxSlot(*frontend);
        OpenGLResourceTests::ResourcePresentationPort foreignPort;
        auto foreign = Create(foreignPort);
        CHECK(foreign->PrepareVfxFrameSlot(fixture.slot->Material()).HasError());
        const std::array<VfxPackedDrawInstance, 1> instances{};
        CHECK(foreign->UpdateVfxFrame(fixture.slot, 1, instances).HasError());
        CHECK(native.uploads == 0);
    }
}

TEST_CASE("VFX preparation revalidates native query reentry before issuing a slot", "[renderer][opengl][vfx][unit]") {
    OpenGLResourceTests::ResourcePresentationPort port;
    auto frontend = Create(port);
    auto source = PrepareVfxMaterial(*frontend);
    bool callbackSucceeded = true;
    bool expectSlot = false;
    SECTION("buffer metadata resize preserves the original ready generation") {
        expectSlot = true;
        native.onUniformQuery = [&] {
            for (std::size_t index = 0; index < 16; ++index)
                callbackSucceeded =
                    frontend->CreateBuffer({.byteSize = 16, .usage = RenderBufferUsage::Uniform}, {}).HasValue() && callbackSucceeded;
        };
    }
    SECTION("released buffer cannot acquire a new proof") {
        native.onUniformQuery = [&] {
            callbackSucceeded = frontend->ReleaseBuffer(source.buffer).HasValue();
        };
    }
    SECTION("released pipeline cannot acquire a new proof") {
        native.onUniformQuery = [&] {
            callbackSucceeded = frontend->ReleaseGraphicsPipeline(source.pipeline.pipeline).HasValue();
        };
    }
    SECTION("shutdown cannot recreate an owner marker") {
        native.onUniformQuery = [&] {
            callbackSucceeded = frontend->BeginShutdown().HasValue();
        };
    }
    const auto slot = frontend->PrepareVfxFrameSlot(source.material);
    CHECK(callbackSucceeded);
    CHECK(slot.HasValue() == expectSlot);
    CHECK(native.uploads == 0);
}

TEST_CASE("VFX publication rejects nested mutation and never republishes after callback shutdown", "[renderer][opengl][vfx][unit]") {
    OpenGLResourceTests::ResourcePresentationPort port;
    auto frontend = Create(port);
    auto fixture = PrepareVfxSlot(*frontend);
    const std::array<VfxPackedDrawInstance, 1> instances{};
    REQUIRE(frontend->UpdateVfxFrame(fixture.slot, 1, instances).HasValue());
    SECTION("same handle by reference cannot mutate outer borrowed staging") {
        bool nestedRejected = false;
        native.onUpload = [&] {
            const auto nested = frontend->UpdateVfxFrame(fixture.slot, 2, instances);
            nestedRejected = nested.HasError() && nested.ErrorValue().code.Value() == VfxFrameErrors::Pending.code.Value();
        };
        REQUIRE(frontend->UpdateVfxFrame(fixture.slot, 2, instances).HasValue());
        CHECK(nestedRejected);
        CHECK(native.uploads == 2);
        CHECK(fixture.slot->Revision() == 2);
        REQUIRE(frontend->UpdateVfxFrame(fixture.slot, 3, instances).HasValue());
    }
    SECTION("shutdown after write entry invalidates the candidate publication") {
        bool closed = false;
        native.onUpload = [&] {
            closed = frontend->BeginShutdown().HasValue();
        };
        CHECK(frontend->UpdateVfxFrame(fixture.slot, 2, instances).HasError());
        CHECK(closed);
        CHECK(native.uploads == 2);
        CHECK(fixture.slot->Revision() == 0);
        CHECK(frontend->UpdateVfxFrame(fixture.slot, 3, instances).HasError());
    }
    SECTION("releasing caller handle cannot destroy the active borrowed staging") {
        std::weak_ptr<PreparedVfxFrameSlot> lifetime = fixture.slot;
        native.onUpload = [&] {
            fixture.slot.reset();
        };
        REQUIRE(frontend->UpdateVfxFrame(fixture.slot, 2, instances).HasValue());
        CHECK(!fixture.slot);
        CHECK(lifetime.expired());
        CHECK(native.uploads == 2);
    }
}

TEST_CASE("VFX duplicate preparation shares one CPU authority for an unsubmitted graph borrower", "[renderer][opengl][vfx][unit]") {
    OpenGLResourceTests::ResourcePresentationPort port;
    auto frontend = Create(port);
    auto fixture = PrepareVfxSlot(*frontend);
    auto graphBorrower = fixture.slot;
    auto duplicateResult = frontend->PrepareVfxFrameSlot(fixture.slot->Material());
    REQUIRE(duplicateResult.HasValue());
    auto duplicate = std::move(duplicateResult).Value();
    CHECK(duplicate.get() == graphBorrower.get());
    const std::array<VfxPackedDrawInstance, 1> instances{};
    CHECK(frontend->UpdateVfxFrame(duplicate, 1, instances).HasError());
    CHECK(native.uploads == 0);
    duplicate.reset();
    graphBorrower.reset();
    REQUIRE(frontend->UpdateVfxFrame(fixture.slot, 1, instances).HasValue());
    auto conflicting = fixture.slot->Material()->Descriptor();
    conflicting.material = MaterialRuntimeId{28};
    const auto published = fixture.table->Publish(std::move(conflicting));
    REQUIRE(published.HasValue());
    auto material = fixture.table->Acquire(published.Value().binding);
    REQUIRE(material.HasValue());
    CHECK(frontend->PrepareVfxFrameSlot(std::move(material).Value()).HasError());
    CHECK(fixture.slot->Revision() == 1);
}

TEST_CASE("VFX expired slot claims cannot restart revisions until the canonical buffer generation changes",
          "[renderer][opengl][vfx][unit]") {
    OpenGLResourceTests::ResourcePresentationPort port;
    auto frontend = Create(port);
    auto source = PrepareVfxMaterial(*frontend);
    auto issued = frontend->PrepareVfxFrameSlot(source.material);
    REQUIRE(issued.HasValue());
    auto slot = std::move(issued).Value();
    const std::array<VfxPackedDrawInstance, 1> instances{};
    REQUIRE(frontend->UpdateVfxFrame(slot, 1, instances).HasValue());
    slot.reset();
    CHECK(frontend->PrepareVfxFrameSlot(source.material).HasError());
    auto replacement = source.material->Descriptor();
    REQUIRE(source.table->Shutdown().HasValue());
    source.table.reset();
    source.material.reset();
    REQUIRE(frontend->ReleaseBuffer(source.buffer).HasValue());
    REQUIRE(frontend->ProcessResourceRequests().HasValue());  // Request native retirement; this does not poll its fence.
    OpenGLBackendTests::commandState.pollStatus = 0x911AU;    // GL_ALREADY_SIGNALED
    auto begun = frontend->BeginFrame({.frameNumber = 1, .outputExtent = {64, 32}});
    REQUIRE(begun.HasValue());  // The canonical frame boundary polls the native retirement lease.
    auto frame = std::move(begun).Value();
    frame.Cancel();
    REQUIRE(frontend->ProcessResourceRequests().HasValue());  // Recycle only after the native lease has actually retired.
    const auto buffer = frontend->CreateBuffer({.byteSize = sizeof(VfxDrawUniformBlock),
                                                .usage = RenderBufferUsage::Uniform,
                                                .access = RenderBufferAccess::HostVisible},
                                               {});
    REQUIRE(buffer.HasValue());
    REQUIRE(frontend->ProcessResourceRequests().HasValue());
    CHECK(buffer.Value().handle != source.buffer);
    CHECK(buffer.Value().handle.slot == source.buffer.slot);
    replacement.resources[0].value = MaterialBufferBinding{buffer.Value().handle, 0, sizeof(VfxDrawUniformBlock)};
    auto table = MaterialBindingTable::Create(source.pipeline.pipeline.owner, *frontend);
    REQUIRE(table.HasValue());
    const auto generation = table.Value()->Publish(std::move(replacement));
    REQUIRE(generation.HasValue());
    auto material = table.Value()->Acquire(generation.Value().binding);
    REQUIRE(material.HasValue());
    auto prepared = frontend->PrepareVfxFrameSlot(std::move(material).Value());
    REQUIRE(prepared.HasValue());
    auto fresh = std::move(prepared).Value();
    REQUIRE(frontend->UpdateVfxFrame(fresh, 1, instances).HasValue());
    CHECK(fresh->Revision() == 1);
}

TEST_CASE("VFX native query cannot recursively prepare the same buffer", "[renderer][opengl][vfx][unit]") {
    OpenGLResourceTests::ResourcePresentationPort port;
    auto frontend = Create(port);
    auto source = PrepareVfxMaterial(*frontend);
    bool nestedRejected = false;
    native.onUniformQuery = [&] {
        const auto nested = frontend->PrepareVfxFrameSlot(source.material);
        nestedRejected = nested.HasError() && nested.ErrorValue().code.Value() == VfxFrameErrors::Pending.code.Value();
    };
    const auto queries = native.uniformQueries;
    const auto slot = frontend->PrepareVfxFrameSlot(source.material);
    REQUIRE(slot.HasValue());
    CHECK(nestedRejected);
    CHECK(native.uniformQueries == queries + 1);
}

TEST_CASE("VFX callback release cannot recycle a pinned native uniform generation", "[renderer][opengl][vfx][unit]") {
    OpenGLResourceTests::ResourcePresentationPort port;
    auto frontend = Create(port);
    auto fixture = PrepareVfxSlot(*frontend);
    const auto original = Detail::RenderFrontendResourceAccess::BackendInstance(*frontend, fixture.buffer);
    REQUIRE(original.HasValue());
    const auto deleted = OpenGLResourceTests::resourceCommandState.deletedBuffers;
    RenderBufferHandle replacement;
    bool released = false, retained = false, replacementDistinct = false;
    native.onUniformQuery = [&] {
        released = frontend->ReleaseBuffer(fixture.buffer).HasValue();
        const auto created = frontend->CreateBuffer({.byteSize = native.uniformBytes,
                                                     .usage = RenderBufferUsage::Uniform,
                                                     .access = RenderBufferAccess::HostVisible},
                                                    {});
        if (created.HasError() || frontend->ProcessResourceRequests().HasError())
            return;
        replacement = created.Value().handle;
        const auto instance = Detail::RenderFrontendResourceAccess::BackendInstance(*frontend, replacement);
        replacementDistinct = instance.HasValue() && instance.Value() != original.Value();
        retained = OpenGLResourceTests::resourceCommandState.deletedBuffers == deleted;
    };
    const std::array<VfxPackedDrawInstance, 1> instances{};
    // The adapter may finish writing the still-pinned original, but the frontend cannot publish a retired handle.
    CHECK(frontend->UpdateVfxFrame(fixture.slot, 1, instances).HasError());
    CHECK(released);
    CHECK(retained);
    CHECK(replacementDistinct);
    CHECK(fixture.slot->Revision() == 0);
    CHECK(OpenGLResourceTests::resourceCommandState.deletedBuffers == deleted);
    REQUIRE(replacement.IsValid());
    CHECK(frontend->ReleaseBuffer(replacement).HasValue());
}

TEST_CASE("VFX callback shutdown rejects upload and context reincarnation", "[renderer][opengl][vfx][unit]") {
    OpenGLResourceTests::ResourcePresentationPort port;
    auto frontend = Create(port);
    auto fixture = PrepareVfxSlot(*frontend);
    bool closed = false, restarted = false, drainDeferred = false;
    const auto close = [&] {
        closed = borrowedBackend->BeginShutdown().HasValue();
        const auto drained = borrowedBackend->PollDrain();
        drainDeferred = drained.HasValue() && drained.Value() == RenderBackendDrainProgress::Pending;
        restarted = borrowedBackend->Initialize({}).HasValue();
    };
    SECTION("Presentation reentry") {
        port.onMakeCurrent = close;
    }
    SECTION("Reflection reentry") {
        native.onUniformQuery = close;
    }
    const auto uploads = native.uploads;
    const std::array<VfxPackedDrawInstance, 1> instances{};
    CHECK(frontend->UpdateVfxFrame(fixture.slot, 1, instances).HasError());
    CHECK(closed);
    CHECK_FALSE(restarted);
    CHECK(drainDeferred);
    CHECK(native.uploads == uploads);
    CHECK(fixture.slot->Revision() == 0);
}
