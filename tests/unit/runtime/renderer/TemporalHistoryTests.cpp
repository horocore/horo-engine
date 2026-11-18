#include "Horo/Runtime/Render/NativeReconstructionKernel.h"
#include "Horo/Runtime/Render/ReconstructionFrame.h"
#include "Horo/Runtime/Render/TemporalHistory.h"
#include "Horo/Runtime/Render/TemporalHistoryErrors.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <string>
#include <thread>
#include <utility>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

    TemporalHistoryCompatibility Compatibility(const std::uint64_t colorGeneration = 1,
                                               const FramebufferExtent renderExtent = {1280, 720}) {
        return {
            .view = {17},
            .provider = {1},
            .mode = {1},
            .renderExtent = renderExtent,
            .targetExtent = {1920, 1080},
            .providerGeneration = 1,
            .modeGeneration = 1,
            .surfaceGeneration = 1,
            .rasterGeneration = 1,
            .colorGeneration = colorGeneration,
            .exposureGeneration = 1,
            .inputSchemaGeneration = 1,
            .deviceGeneration = 1,
            .projectionGeneration = 1,
            .jitterGeneration = 1,
            .sceneOriginGeneration = 1,
            .motionGeneration = 1,
            .recipeGeneration = 1,
        };
    }

    std::array<RenderTextureHandle, 2> Resources(const std::uint32_t generation = 1) {
        return {{{{9}, 1, generation}, {{9}, 2, generation}}};
    }

    TemporalHistoryStore Store(const TemporalHistoryLimits &limits = {.maxHistories = 2, .maxResourcesPerHistory = 2}) {
        auto created = TemporalHistoryStore::Create(limits);
        REQUIRE(created.HasValue());
        return std::move(created).Value();
    }

    TemporalHistoryHandle AddHistory(TemporalHistoryStore &store) {
        const auto resources = Resources();
        auto added = store.Add(Compatibility(), resources);
        REQUIRE(added.HasValue());
        return added.Value();
    }

    void RequireErrorCode(const Error &error, const ErrorCodeDescriptor &descriptor) {
        REQUIRE(error.domain.Value() == descriptor.domain.Value());
        REQUIRE(error.code.Value() == descriptor.code.Value());
        REQUIRE_FALSE(error.message.empty());
        REQUIRE_FALSE(descriptor.remediationHint.empty());
    }

    RenderCapabilitySnapshot ReconstructionCapabilities() {
        RenderCapabilitySnapshot capabilities{.deviceIncarnation = 1, .capabilityRevision = 1};
        capabilities.features.Enable(RenderCapability::TextureResources);
        capabilities.limits.maxTextureDimension2D = 4096;
        capabilities.formats.sampleCountMask = 1 << 1;
        capabilities.formats.usages.fill(RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment | RenderTextureUsage::Storage);
        return capabilities;
    }

    ReconstructionFrameRequest ReconstructionRequest(const std::uint64_t frameId = 1, const std::uint64_t predecessor = 0) {
        using enum RenderTextureFormat;
        const auto compatibility = Compatibility();
        const auto input = [&](std::uint32_t slot, RenderTextureFormat format) {
            return ReconstructionTextureInput{{{9}, slot, 1},
                                              {.extent = compatibility.renderExtent,
                                               .format = format,
                                               .usage = RenderTextureUsage::Sampled},
                                              compatibility.view,
                                              frameId};
        };
        return {.compatibility = compatibility,
                .resourceOwner = {9},
                .timing = {frameId, predecessor, 12, std::chrono::milliseconds{16}, 1},
                .currentJitterUv = {0.001F, 0.002F},
                .previousJitterUv = {-0.001F, 0.001F},
                .color = input(3, Rgba16Float),
                .positiveLinearDepth = input(4, R32Float),
                .motion = input(5, Rg16Float),
                .masks = {.reactive = ReconstructionMaskInput{input(6, R8Unorm), {{1}, {1}}}},
                .output = {.descriptor = {.extent = compatibility.targetExtent,
                                          .format = Rgba16Float,
                                          .usage = RenderTextureUsage::RenderAttachment}}};
    }
}  // namespace

TEST_CASE("Reconstruction accepts typed temporal and spatial input on the admitted device", "[runtime][renderer][reconstruction]") {
    const auto capabilities = ReconstructionCapabilities();
    auto request = ReconstructionRequest();
    REQUIRE(ValidateReconstructionFrame(request, capabilities).HasValue());
    request = ReconstructionRequest(2, 1);
    request.exposure = ReconstructionExposureBuffers{request.compatibility.view,
                                                     request.compatibility.exposureGeneration,
                                                     {{9}, 1, 1},
                                                     RenderBufferHandle{{9}, 2, 1}};
    REQUIRE(ValidateReconstructionFrame(request, capabilities).HasValue());
    request.kind = ReconstructionKind::Spatial;
    request.positiveLinearDepth.reset();
    request.motion.reset();
    request.colorStage = ReconstructionColorStage::Exposed;
    REQUIRE(ValidateReconstructionFrame(request, capabilities).HasValue());
    auto store = Store();
    const auto history = AddHistory(store);
    const auto rejected = BeginTemporalReconstructionFrame(request, capabilities, store, history);
    REQUIRE(rejected.HasError());
    RequireErrorCode(rejected.ErrorValue(), ReconstructionErrors::InvalidFrame);
}

TEST_CASE("Reconstruction rejects malformed or unsupported data independently of provider", "[runtime][renderer][reconstruction]") {
    auto capabilities = ReconstructionCapabilities();
    auto request = ReconstructionRequest(2, 1);
    const ErrorCodeDescriptor *expected = &ReconstructionErrors::IncompatibleInput;
    SECTION("Display transfer format cannot stand in for scene-linear color") {
        request.color.descriptor.format = RenderTextureFormat::Rgba8UnormSrgb;
    }
    SECTION("Color from another real frame") {
        request.color.frameId = 1;
    }
    SECTION("Same numeric frame in another view cannot supply a guide") {
        ++request.motion->view.value;
    }
    SECTION("Device depth cannot be inferred as linear meters") {
        request.positiveLinearDepth->descriptor.format = RenderTextureFormat::Depth32Float;
    }
    SECTION("Motion is required for a temporal mode") {
        request.motion.reset();
        expected = &ReconstructionErrors::MissingGuides;
    }
    SECTION("Mask requires valid producer evidence") {
        request.masks.reactive->producer = {};
    }
    SECTION("Distinct semantics cannot reuse a slot's stale or current generation") {
        request.masks.reactive->image.texture.slot = request.positiveLinearDepth->texture.slot;
        ++request.masks.reactive->image.texture.generation;
    }
    SECTION("Exposure rescale must be representable") {
        request.exposure = ReconstructionExposureScales{std::numeric_limits<float>::max(), 0.5F};
    }
    SECTION("Temporal GPU exposure requires exact previous input when history can be read") {
        request.exposure = ReconstructionExposureBuffers{request.compatibility.view, request.compatibility.exposureGeneration, {{9}, 1, 1}};
    }
    SECTION("Projection and jitter are explicit and finite") {
        request.currentJitterUv.x = std::numeric_limits<float>::infinity();
        expected = &ReconstructionErrors::InvalidFrame;
    }
    SECTION("A non-current device cannot admit this frame") {
        ++capabilities.deviceIncarnation;
        expected = &ReconstructionErrors::InvalidFrame;
    }
    SECTION("Negative source time is not a real-frame interval") {
        request.timing.realFrameDelta = std::chrono::nanoseconds{-1};
        expected = &ReconstructionErrors::InvalidFrame;
    }
    SECTION("An unknown reset cause cannot open a pending attempt") {
        request.reset = static_cast<TemporalHistoryResetCause>(255);
        expected = &TemporalHistoryErrors::InvalidResetCause;
    }
    SECTION("Output requires compatible graph write usage") {
        request.output.writeUsage = RenderGraphUsageKind::Storage;
        expected = &ReconstructionErrors::IncompatibleOutput;
    }
    SECTION("A resident output cannot alias a sampled input") {
        request.output.lifetime = RenderGraphResourceClass::Persistent;
        request.output.resident = request.color.texture;
        expected = &ReconstructionErrors::IncompatibleOutput;
    }
    SECTION("Well-formed output beyond the backend extent limit has no implicit fallback") {
        capabilities.limits.maxTextureDimension2D = request.compatibility.renderExtent.width;
        expected = &ReconstructionErrors::UnsupportedResource;
    }
    const auto rejected = ValidateReconstructionFrame(request, capabilities);
    REQUIRE(rejected.HasError());
    RequireErrorCode(rejected.ErrorValue(), *expected);
}

TEST_CASE("Reconstruction history rejects stale input and keeps an abandoned cut fenced", "[runtime][renderer][reconstruction]") {
    const auto capabilities = ReconstructionCapabilities();
    auto store = Store();
    const auto history = AddHistory(store);
    const auto first = BeginTemporalReconstructionFrame(ReconstructionRequest(), capabilities, store, history);
    REQUIRE(first.HasValue());
    CHECK(first.Value().resetCause == TemporalHistoryResetCause::FirstFrame);
    REQUIRE(store.Publish(first.Value()).HasValue());

    auto mismatched = ReconstructionRequest(2, 1);
    ++mismatched.compatibility.colorGeneration;
    const auto stale = BeginTemporalReconstructionFrame(mismatched, capabilities, store, history);
    REQUIRE(stale.HasError());
    RequireErrorCode(stale.ErrorValue(), TemporalHistoryErrors::ResetRequired);
    const std::array foreignResources{RenderTextureHandle{{10}, 1, 1}};
    const auto foreignHistory = store.Add(Compatibility(), foreignResources);
    REQUIRE(foreignHistory.HasValue());
    const auto foreign = BeginTemporalReconstructionFrame(ReconstructionRequest(2, 1), capabilities, store, foreignHistory.Value());
    REQUIRE(foreign.HasError());
    RequireErrorCode(foreign.ErrorValue(), TemporalHistoryErrors::ResetRequired);

    const auto second = BeginTemporalReconstructionFrame(ReconstructionRequest(2, 1), capabilities, store, history);
    REQUIRE(second.HasValue());
    CHECK(second.Value().canReadPrevious);
    REQUIRE(store.Publish(second.Value()).HasValue());

    auto cut = ReconstructionRequest(3, 2);
    cut.reset = TemporalHistoryResetCause::CameraCut;
    cut.exposure = ReconstructionExposureBuffers{cut.compatibility.view, cut.compatibility.exposureGeneration, {{9}, 1, 1}};
    const auto interrupted = BeginTemporalReconstructionFrame(cut, capabilities, store, history);
    REQUIRE(interrupted.HasValue());
    CHECK_FALSE(interrupted.Value().canReadPrevious);
    REQUIRE(store.Abandon(interrupted.Value()).HasValue());
    const auto next = BeginTemporalReconstructionFrame(ReconstructionRequest(4, 2), capabilities, store, history);
    REQUIRE(next.HasValue());
    CHECK(next.Value().resetCause == TemporalHistoryResetCause::CameraCut);
    CHECK(next.Value().contentGeneration == interrupted.Value().contentGeneration);
    CHECK_FALSE(next.Value().canReadPrevious);
    REQUIRE(store.Publish(next.Value()).HasValue());
    REQUIRE(store.Retire(history).HasValue());
    REQUIRE(store.Shutdown().HasValue());
    CHECK(BeginTemporalReconstructionFrame(ReconstructionRequest(5, 4), capabilities, store, history).HasError());
}

TEST_CASE("Native reconstruction constants follow the exact reset and exposure evidence", "[runtime][renderer][reconstruction][native]") {
    auto capabilities = ReconstructionCapabilities();
    capabilities.features.Enable(RenderCapability::Compute);
    capabilities.queues.compute = true;
    auto request = ReconstructionRequest();
    const auto enableNativeTarget = [](ReconstructionFrameRequest &frame) {
        frame.masks = {};
        frame.output.writeUsage = RenderGraphUsageKind::Storage;
        frame.output.descriptor.format = RenderTextureFormat::Rgba32Float;
        frame.output.descriptor.usage = RenderTextureUsage::Storage | RenderTextureUsage::Sampled;
    };
    enableNativeTarget(request);
    const NativeReconstructionTemporalSettings tuning{.historyWeight = 0.75F,
                                                      .absoluteDepthTolerance = 0.01F,
                                                      .relativeDepthTolerance = 0.02F};
    auto store = Store();
    const auto history = AddHistory(store);
    const auto first = BeginTemporalReconstructionFrame(request, capabilities, store, history);
    REQUIRE(first.HasValue());
    const auto firstConstants =
        PrepareNativeReconstructionDispatch(request, capabilities, tuning, NativeReconstructionHistory{store, first.Value()});
    REQUIRE(firstConstants.HasValue());
    CHECK(firstConstants.Value().entry == NativeReconstructionEntry::TemporalSeed);
    CHECK((firstConstants.Value().constants.extents == std::array<std::uint32_t, 4>{1280, 720, 1920, 1080}));
    CHECK((firstConstants.Value().constants.Groups() == FramebufferExtent{240, 135}));
    CHECK((firstConstants.Value().constants.sample ==
           std::array<float, 4>{request.currentJitterUv.x, request.currentJitterUv.y, 1.0F, 0.0F}));
    REQUIRE(store.Publish(first.Value()).HasValue());

    request = ReconstructionRequest(2, 1);
    enableNativeTarget(request);
    request.exposure = ReconstructionExposureScales{2.0F, 0.5F};
    const auto second = BeginTemporalReconstructionFrame(request, capabilities, store, history);
    REQUIRE(second.HasValue());
    const auto nextConstants =
        PrepareNativeReconstructionDispatch(request, capabilities, tuning, NativeReconstructionHistory{store, second.Value()});
    REQUIRE(nextConstants.HasValue());
    CHECK(nextConstants.Value().entry == NativeReconstructionEntry::TemporalHistory);
    CHECK(nextConstants.Value().constants.sample[2] == 4.0F);
    CHECK(nextConstants.Value().constants.sample[3] == tuning.historyWeight);
    auto noBlend = tuning;
    noBlend.historyWeight = 0.0F;
    const auto seed =
        PrepareNativeReconstructionDispatch(request, capabilities, noBlend, NativeReconstructionHistory{store, second.Value()});
    REQUIRE(seed.HasValue());
    CHECK(seed.Value().entry == NativeReconstructionEntry::TemporalSeed);
    CHECK(seed.Value().constants.sample[2] == 1.0F);
    CHECK(seed.Value().constants.sample[3] == 0.0F);
    const auto wrong =
        PrepareNativeReconstructionDispatch(request, capabilities, tuning, NativeReconstructionHistory{store, first.Value()});
    REQUIRE(wrong.HasError());
    RequireErrorCode(wrong.ErrorValue(), NativeReconstructionErrors::InvalidDispatch);

    capabilities.queues.compute = false;
    const auto unsupported =
        PrepareNativeReconstructionDispatch(request, capabilities, tuning, NativeReconstructionHistory{store, second.Value()});
    REQUIRE(unsupported.HasError());
    RequireErrorCode(unsupported.ErrorValue(), NativeReconstructionErrors::Unsupported);
    REQUIRE(store.Abandon(second.Value()).HasValue());
    capabilities.queues.compute = true;
    const auto retry = BeginTemporalReconstructionFrame(request, capabilities, store, history);
    REQUIRE(retry.HasValue());
    const auto stale =
        PrepareNativeReconstructionDispatch(request, capabilities, tuning, NativeReconstructionHistory{store, second.Value()});
    REQUIRE(stale.HasError());
    RequireErrorCode(stale.ErrorValue(), TemporalHistoryErrors::InvalidFrame);
    CHECK(PrepareNativeReconstructionDispatch(request, capabilities, tuning, NativeReconstructionHistory{store, retry.Value()}).HasValue());
    REQUIRE(store.Abandon(retry.Value()).HasValue());
    CHECK((NativeReconstructionDispatch{.extents = {1, 1, 1, 9}}.Groups() == FramebufferExtent{1, 2}));
}

TEST_CASE("Native reconstruction cook contracts name only the closed entry and binding family",
          "[runtime][renderer][reconstruction][native]") {
    ShaderTargetRequirement target{.backend = ShaderTargetBackend::Vulkan,
                                   .payloadFormat = ShaderPayloadFormat::SpirV16,
                                   .descriptorVersion = 1,
                                   .interfaceSchemaVersion = 1,
                                   .maximumBindings = 16,
                                   .supportsCompute = true,
                                   .supportsStorageResources = true};
    const auto spatial = DescribeNativeReconstructionShader(NativeReconstructionEntry::Spatial, target);
    REQUIRE(spatial.HasValue());
    CHECK(spatial.Value().manifest.entryPoints.front().name == "ReconstructSpatial");
    CHECK(spatial.Value().manifest.bindings.size() == 3);
    CHECK(spatial.Value().reflectionNames.back().parameters.size() == 3);

    const auto temporal = DescribeNativeReconstructionShader(NativeReconstructionEntry::TemporalHistory, target);
    REQUIRE(temporal.HasValue());
    CHECK(temporal.Value().manifest.entryPoints.front().name == "ReconstructTemporal");
    CHECK(temporal.Value().manifest.bindings.size() == 8);
    CHECK(temporal.Value().reflectionNames[6].name == "ReconstructedDepth");
    const auto seed = DescribeNativeReconstructionShader(NativeReconstructionEntry::TemporalSeed, target);
    REQUIRE(seed.HasValue());
    CHECK(seed.Value().manifest.entryPoints.front().name == "ReconstructTemporalSeed");
    constexpr std::array seedBindings{1U, 2U, 3U, 7U, 8U};
    REQUIRE(seed.Value().manifest.bindings.size() == seedBindings.size());
    for (std::size_t i = 0; i < seedBindings.size(); ++i)
        CHECK(seed.Value().manifest.bindings[i].id.value == seedBindings[i]);
    const auto missing = AdmitNativeReconstructionShader(NativeReconstructionEntry::TemporalHistory, target, {});
    REQUIRE(missing.HasError());
    RequireErrorCode(missing.ErrorValue(), NativeReconstructionErrors::InvalidArtifact);
    target.supportsCompute = false;
    const auto unsupported = DescribeNativeReconstructionShader(NativeReconstructionEntry::Spatial, target);
    REQUIRE(unsupported.HasError());
    RequireErrorCode(unsupported.ErrorValue(), NativeReconstructionErrors::Unsupported);
}

TEST_CASE("Temporal history identities and capacities are bounded", "[runtime][renderer][temporal-history]") {
    REQUIRE(TemporalHistoryOwnerId{1}.IsValid());
    REQUIRE_FALSE(TemporalHistoryOwnerId{}.IsValid());
    REQUIRE(RenderViewId{1}.IsValid());
    REQUIRE(TemporalHistoryProviderId{1}.IsValid());
    REQUIRE(TemporalHistoryModeId{1}.IsValid());
    REQUIRE(Compatibility().IsValid());
    REQUIRE(TemporalHistoryLimits{}.IsValid());

    auto invalidLimits = TemporalHistoryLimits{};
    invalidLimits.maxHistories = 0;
    auto rejected = TemporalHistoryStore::Create(invalidLimits);
    REQUIRE(rejected.HasError());
    RequireErrorCode(rejected.ErrorValue(), TemporalHistoryErrors::InvalidLimits);

    auto store = Store({.maxHistories = 1, .maxResourcesPerHistory = 2});
    AddHistory(store);
    const auto resources = Resources();
    auto full = store.Add(Compatibility(2), resources);
    REQUIRE(full.HasError());
    RequireErrorCode(full.ErrorValue(), TemporalHistoryErrors::CapacityExceeded);
}

TEST_CASE("Temporal compatibility requires every ADR-040 identity dimension", "[runtime][renderer][temporal-history]") {
    auto compatibility = Compatibility();
    compatibility.provider = {};
    REQUIRE_FALSE(compatibility.IsValid());
    compatibility = Compatibility();
    compatibility.mode = {};
    REQUIRE_FALSE(compatibility.IsValid());
    compatibility = Compatibility();
    compatibility.renderExtent = {};
    REQUIRE_FALSE(compatibility.IsValid());
    compatibility = Compatibility();
    compatibility.targetExtent = {};
    REQUIRE_FALSE(compatibility.IsValid());

    constexpr std::array generationMembers{
        &TemporalHistoryCompatibility::providerGeneration,    &TemporalHistoryCompatibility::modeGeneration,
        &TemporalHistoryCompatibility::surfaceGeneration,     &TemporalHistoryCompatibility::rasterGeneration,
        &TemporalHistoryCompatibility::colorGeneration,       &TemporalHistoryCompatibility::exposureGeneration,
        &TemporalHistoryCompatibility::inputSchemaGeneration, &TemporalHistoryCompatibility::deviceGeneration,
        &TemporalHistoryCompatibility::projectionGeneration,  &TemporalHistoryCompatibility::jitterGeneration,
        &TemporalHistoryCompatibility::sceneOriginGeneration, &TemporalHistoryCompatibility::motionGeneration,
        &TemporalHistoryCompatibility::recipeGeneration,
    };
    for (const auto member : generationMembers) {
        compatibility = Compatibility();
        compatibility.*member = 0;
        REQUIRE_FALSE(compatibility.IsValid());
    }
}

TEST_CASE("Invalid descriptors and texture identities are rejected", "[runtime][renderer][temporal-history]") {
    auto store = Store();
    const auto resources = Resources();
    auto invalidCompatibility = Compatibility();
    invalidCompatibility.deviceGeneration = 0;
    auto invalidDescriptor = store.Add(invalidCompatibility, resources);
    REQUIRE(invalidDescriptor.HasError());
    RequireErrorCode(invalidDescriptor.ErrorValue(), TemporalHistoryErrors::InvalidDescriptor);

    const std::array<RenderTextureHandle, 1> invalidResources{};
    auto invalidResource = store.Add(Compatibility(), invalidResources);
    REQUIRE(invalidResource.HasError());
    RequireErrorCode(invalidResource.ErrorValue(), TemporalHistoryErrors::InvalidDescriptor);
}

TEST_CASE("First publish establishes readable temporal history", "[runtime][renderer][temporal-history]") {
    auto store = Store();
    const auto handle = AddHistory(store);

    auto first = store.BeginFrame(handle, 100, 0);
    REQUIRE(first.HasValue());
    REQUIRE_FALSE(first.Value().canReadPrevious);
    REQUIRE(first.Value().resetCause == TemporalHistoryResetCause::FirstFrame);
    REQUIRE(first.Value().contentGeneration == 1);
    REQUIRE(store.Publish(first.Value()).HasValue());

    auto second = store.BeginFrame(handle, 101, 100);
    REQUIRE(second.HasValue());
    REQUIRE(second.Value().canReadPrevious);
    REQUIRE_FALSE(second.Value().resetCause.has_value());
    REQUIRE(second.Value().compatibility == Compatibility());
    REQUIRE(std::ranges::equal(second.Value().resources, Resources()));
    REQUIRE(store.Publish(second.Value()).HasValue());
}

TEST_CASE("Failed frames never advance temporal history", "[runtime][renderer][temporal-history]") {
    auto store = Store();
    const auto handle = AddHistory(store);
    auto first = store.BeginFrame(handle, 4, 0);
    REQUIRE(first.HasValue());
    REQUIRE(store.Abandon(first.Value()).HasValue());

    auto retry = store.BeginFrame(handle, 4, 0);
    REQUIRE(retry.HasValue());
    REQUIRE(retry.Value().contentGeneration == 1);
    REQUIRE(retry.Value().resetCause == TemporalHistoryResetCause::FirstFrame);
    REQUIRE(store.Publish(retry.Value()).HasValue());

    auto skipped = store.BeginFrame(handle, 6, 5);
    REQUIRE(skipped.HasValue());
    REQUIRE_FALSE(skipped.Value().canReadPrevious);
    REQUIRE(skipped.Value().resetCause == TemporalHistoryResetCause::MissingPredecessor);
}

TEST_CASE("Abandoned frame snapshots cannot complete a same-id retry", "[runtime][renderer][temporal-history]") {
    auto store = Store();
    const auto handle = AddHistory(store);
    auto abandoned = store.BeginFrame(handle, 4, 0);
    REQUIRE(abandoned.HasValue());
    REQUIRE(store.Abandon(abandoned.Value()).HasValue());

    auto retry = store.BeginFrame(handle, 4, 0);
    REQUIRE(retry.HasValue());
    REQUIRE(retry.Value().attempt != abandoned.Value().attempt);
    auto stalePublish = store.Publish(abandoned.Value());
    REQUIRE(stalePublish.HasError());
    RequireErrorCode(stalePublish.ErrorValue(), TemporalHistoryErrors::InvalidFrame);
    auto staleAbandon = store.Abandon(abandoned.Value());
    REQUIRE(staleAbandon.HasError());
    RequireErrorCode(staleAbandon.ErrorValue(), TemporalHistoryErrors::InvalidFrame);
    REQUIRE(store.Publish(retry.Value()).HasValue());
}

TEST_CASE("Mutated frame validation fields cannot publish", "[runtime][renderer][temporal-history]") {
    auto store = Store();
    const auto handle = AddHistory(store);
    auto begun = store.BeginFrame(handle, 1, 0);
    REQUIRE(begun.HasValue());

    auto mutated = begun.Value();
    mutated.resetCause = TemporalHistoryResetCause::CameraCut;
    auto rejected = store.Publish(mutated);
    REQUIRE(rejected.HasError());
    RequireErrorCode(rejected.ErrorValue(), TemporalHistoryErrors::InvalidFrame);
    REQUIRE(store.Publish(begun.Value()).HasValue());
}

TEST_CASE("Explicit reset replaces resources and reports exact cause", "[runtime][renderer][temporal-history]") {
    auto store = Store();
    const auto handle = AddHistory(store);
    auto first = store.BeginFrame(handle, 1, 0);
    REQUIRE(first.HasValue());
    REQUIRE(store.Publish(first.Value()).HasValue());

    const auto resizedResources = Resources(2);
    REQUIRE(store.Reset(handle, Compatibility(1, {1920, 1080}), resizedResources, TemporalHistoryResetCause::Resize).HasValue());
    auto resized = store.BeginFrame(handle, 2, 1);
    REQUIRE(resized.HasValue());
    REQUIRE_FALSE(resized.Value().canReadPrevious);
    REQUIRE(resized.Value().resetCause == TemporalHistoryResetCause::Resize);
    REQUIRE(std::ranges::equal(resized.Value().resources, resizedResources));
    REQUIRE(store.Publish(resized.Value()).HasValue());

    REQUIRE(store.Reset(handle, Compatibility(2, {1920, 1080}), resizedResources, TemporalHistoryResetCause::ColorPlanChange).HasValue());
    auto profile = store.BeginFrame(handle, 3, 2);
    REQUIRE(profile.HasValue());
    REQUIRE(profile.Value().resetCause == TemporalHistoryResetCause::ColorPlanChange);
}

TEST_CASE("Compatibility replacements publish exact reset provenance", "[runtime][renderer][temporal-history]") {
    auto store = Store();
    const auto handle = AddHistory(store);
    const auto resources = Resources();
    auto first = store.BeginFrame(handle, 1, 0);
    REQUIRE(first.HasValue());
    REQUIRE(store.Publish(first.Value()).HasValue());

    struct Replacement {
        TemporalHistoryCompatibility compatibility;
        TemporalHistoryResetCause cause;
    };

    auto provider = Compatibility();
    provider.provider = {2};
    auto mode = provider;
    mode.mode = {2};
    auto projection = mode;
    projection.projectionGeneration = 2;
    auto sceneMotion = projection;
    sceneMotion.sceneOriginGeneration = 2;
    sceneMotion.motionGeneration = 2;
    const std::array replacements{
        Replacement{provider, TemporalHistoryResetCause::ProviderReplacement},
        Replacement{mode, TemporalHistoryResetCause::ModeReplacement},
        Replacement{projection, TemporalHistoryResetCause::ProjectionChange},
        Replacement{sceneMotion, TemporalHistoryResetCause::SceneDiscontinuity},
    };

    std::uint64_t frameId = 2;
    for (const Replacement &replacement : replacements) {
        REQUIRE(store.Reset(handle, replacement.compatibility, resources, replacement.cause).HasValue());
        auto frame = store.BeginFrame(handle, frameId, frameId - 1);
        REQUIRE(frame.HasValue());
        REQUIRE(frame.Value().compatibility == replacement.compatibility);
        REQUIRE(frame.Value().resetCause == replacement.cause);
        REQUIRE_FALSE(frame.Value().canReadPrevious);
        REQUIRE(store.Publish(frame.Value()).HasValue());
        ++frameId;
    }
}

TEST_CASE("View replacement starts a new per-view real-frame sequence", "[runtime][renderer][temporal-history]") {
    auto store = Store();
    const auto handle = AddHistory(store);
    auto oldView = store.BeginFrame(handle, 100, 0);
    REQUIRE(oldView.HasValue());
    REQUIRE(store.Publish(oldView.Value()).HasValue());

    auto newViewCompatibility = Compatibility();
    newViewCompatibility.view = {18};
    const auto resources = Resources();
    REQUIRE(store.Reset(handle, newViewCompatibility, resources, TemporalHistoryResetCause::ViewReplacement).HasValue());
    auto newView = store.BeginFrame(handle, 1, 0);
    REQUIRE(newView.HasValue());
    REQUIRE(newView.Value().contentGeneration == 1);
    REQUIRE(newView.Value().resetCause == TemporalHistoryResetCause::ViewReplacement);
}

TEST_CASE("Camera cuts and invalid inputs are explicit without fallback", "[runtime][renderer][temporal-history]") {
    auto store = Store();
    const auto handle = AddHistory(store);
    const auto resources = Resources();
    REQUIRE(store.Reset(handle, Compatibility(), resources, TemporalHistoryResetCause::CameraCut).HasValue());

    auto frame = store.BeginFrame(handle, 9, 8);
    REQUIRE(frame.HasValue());
    REQUIRE(frame.Value().resetCause == TemporalHistoryResetCause::CameraCut);

    auto overlapping = store.BeginFrame(handle, 10, 9);
    REQUIRE(overlapping.HasError());
    RequireErrorCode(overlapping.ErrorValue(), TemporalHistoryErrors::FrameAlreadyPending);
    auto resetPending = store.Reset(handle, Compatibility(), resources, TemporalHistoryResetCause::ExplicitRequest);
    REQUIRE(resetPending.HasError());
    RequireErrorCode(resetPending.ErrorValue(), TemporalHistoryErrors::FrameAlreadyPending);

    auto retirePending = store.Retire(handle);
    REQUIRE(retirePending.HasError());
    RequireErrorCode(retirePending.ErrorValue(), TemporalHistoryErrors::FrameAlreadyPending);
}

TEST_CASE("Foreign handles and unknown reset causes are rejected", "[runtime][renderer][temporal-history]") {
    auto owner = Store();
    auto other = Store();
    const auto handle = AddHistory(owner);
    auto foreign = other.BeginFrame(handle, 1, 0);
    REQUIRE(foreign.HasError());
    RequireErrorCode(foreign.ErrorValue(), TemporalHistoryErrors::WrongOwner);

    const auto resources = Resources();
    auto invalidCause = owner.Reset(handle, Compatibility(), resources, static_cast<TemporalHistoryResetCause>(255));
    REQUIRE(invalidCause.HasError());
    RequireErrorCode(invalidCause.ErrorValue(), TemporalHistoryErrors::InvalidResetCause);
}

TEST_CASE("Retirement invalidates generations and shutdown is idempotent", "[runtime][renderer][temporal-history]") {
    auto store = Store();
    const auto stale = AddHistory(store);
    REQUIRE(store.Retire(stale).HasValue());
    const auto size = store.Size();
    REQUIRE(size.HasValue());
    REQUIRE(size.Value() == 0);

    const auto replacement = AddHistory(store);
    REQUIRE(replacement.slot == stale.slot);
    REQUIRE(replacement.generation != stale.generation);
    auto staleUse = store.BeginFrame(stale, 1, 0);
    REQUIRE(staleUse.HasError());
    RequireErrorCode(staleUse.ErrorValue(), TemporalHistoryErrors::InvalidHandle);

    REQUIRE(store.Shutdown().HasValue());
    REQUIRE(store.Shutdown().HasValue());
    auto stopped = store.BeginFrame(replacement, 1, 0);
    REQUIRE(stopped.HasError());
    RequireErrorCode(stopped.ErrorValue(), TemporalHistoryErrors::StoreStopped);
}

TEST_CASE("Temporal history mutation remains owner-thread affine", "[runtime][renderer][temporal-history]") {
    auto store = Store();
    const auto handle = AddHistory(store);
    std::string observedCode;
    std::thread worker([&] {
        const auto result = store.BeginFrame(handle, 1, 0);
        observedCode = result.HasError() ? result.ErrorValue().code.Value() : "unexpected.success";
    });
    worker.join();
    REQUIRE(observedCode == TemporalHistoryErrors::WrongThread.code.Value());

    std::string sizeCode;
    std::thread observer([&] {
        const auto result = store.Size();
        sizeCode = result.HasError() ? result.ErrorValue().code.Value() : "unexpected.success";
    });
    observer.join();
    REQUIRE(sizeCode == TemporalHistoryErrors::WrongThread.code.Value());
}

TEST_CASE("Temporal history errors expose stable actionable identities", "[runtime][renderer][temporal-history]") {
    const std::array descriptors{
        &TemporalHistoryErrors::AllocationFailed,  &TemporalHistoryErrors::CapacityExceeded,  &TemporalHistoryErrors::FrameAlreadyPending,
        &TemporalHistoryErrors::InvalidDescriptor, &TemporalHistoryErrors::InvalidFrame,      &TemporalHistoryErrors::InvalidHandle,
        &TemporalHistoryErrors::InvalidLimits,     &TemporalHistoryErrors::InvalidResetCause, &TemporalHistoryErrors::OwnerExhausted,
        &TemporalHistoryErrors::ResetRequired,     &TemporalHistoryErrors::StoreStopped,      &TemporalHistoryErrors::WrongOwner,
        &TemporalHistoryErrors::WrongThread,
    };
    for (const ErrorCodeDescriptor *descriptor : descriptors) {
        REQUIRE(descriptor->domain.Value() == "render.temporal_history");
        REQUIRE_FALSE(descriptor->code.Value().empty());
        REQUIRE_FALSE(descriptor->summary.empty());
        REQUIRE_FALSE(descriptor->remediationHint.empty());
    }
}
