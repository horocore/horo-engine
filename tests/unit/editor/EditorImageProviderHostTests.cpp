#include "EditorImageProviderHost.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Runtime/Render/NullBackendModule.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "HostSignatureDocument.h"
#include "support/OpenXRProductSigningTestSupport.h"
#include "support/renderer/RenderBackendDrainTestSupport.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <utility>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Render;
    using namespace Horo::Release;

    template <typename T> T RequireValue(Result<T> result) {
        INFO((result.HasError() ? result.ErrorValue().message : "admitted"));
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    /** @brief Test-only quiescent product root from which the real file owner reads the signed artifacts. */
    struct InstalledDirectory final {
        std::filesystem::path root = std::filesystem::temp_directory_path() /
                                     ("horo-image-provider-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        InstalledDirectory() {
            REQUIRE(std::filesystem::create_directory(root));
        }

        ~InstalledDirectory() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        void Write(const std::filesystem::path &relative, std::string_view bytes) {
            const auto path = root / relative;
            std::filesystem::create_directories(path.parent_path());
            std::ofstream output{path, std::ios::binary};
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            REQUIRE(output.good());
        }
    };

    /** @brief Parsed and signed by the same existing production inventory and cryptographic owners as the editor host. */
    ReleaseArtifactManifestData Inventory() {
        ReleaseArtifactManifestData data;
        data.candidate = ReleaseCandidateId{17};
        data.product = {DistributionProductKind::Editor, {}};
        data.version = EngineProductVersion{{1, 2, 3, {}, {}}};
        data.sourceRevision = {"test_source"};
        data.platform = DistributionPlatform::Linux;
        data.architecture = DistributionArchitecture::X64;
        data.configuration = ReleaseBuildConfiguration::Shipping;
        data.build = {"fixture_build"};
        data.toolchainId = "unit_test";
        data.artifacts = {{"notices/test.txt", ReleaseArtifactRole::Notice, 0, ComputeSha256({})}};
        return data;
    }

    std::span<const std::byte> Bytes(std::string_view text) {
        return std::as_bytes(std::span{text.data(), text.size()});
    }

    void WriteSigned(InstalledDirectory &installed, ReleaseArtifactManifestData data, Tests::OpenXRProduct::SigningKey &signer) {
        const auto inventory = RequireValue(ReleaseArtifactManifest::Create(std::move(data)));
        const auto &json = inventory.CanonicalJson();
        installed.Write("manifest.json", json);
        installed.Write("manifest.signature.json", Application::Internal::EncodeDetachedSignatureDocument(signer.Sign(Bytes(json))));
    }

    /** @brief Adds the exact offline cook output to an installed directory and its candidate inventory. */
    void AddCookedShaders(InstalledDirectory &installed, ReleaseArtifactManifestData &data, const std::filesystem::path &source) {
        for (const auto &entry : std::filesystem::recursive_directory_iterator{source}) {
            if (!entry.is_regular_file())
                continue;
            const auto relative = entry.path().lexically_relative(source).generic_string();
            std::ifstream input{entry.path(), std::ios::binary};
            REQUIRE(input.good());
            const std::string bytes{std::istreambuf_iterator<char>{input}, {}};
            REQUIRE_FALSE(bytes.empty());
            data.artifacts.emplace_back(relative, ReleaseArtifactRole::AssetArchive, bytes.size(), ComputeSha256(Bytes(bytes)));
            installed.Write(relative, bytes);
        }
    }

    struct RendererOwner final {
        RenderBackendRegistry registry;
        std::unique_ptr<RenderFrontend> frontend;

        RendererOwner() {
            REQUIRE(RegisterNullRenderBackend(registry).HasValue());
            REQUIRE(registry.Seal().HasValue());
            frontend = RequireValue(RenderFrontend::Create(registry, RenderBackendId{"null"}, {}));
        }

        ~RendererOwner() {
            if (frontend)
                Test::RequireDrainedOwner(*frontend);
        }
    };
}  // namespace

TEST_CASE("Editor image provider inspection accepts one explicit route and independent roots", "[renderer][image-provider][editor]") {
    const std::string exe = "HoroEditor";
    const auto root = std::filesystem::temp_directory_path().lexically_normal();
    std::array options{exe, std::string{"--inspect-image-provider=taau"}, "--image-provider-root=" + root.string(),
                       "--image-provider-trust=" + (root / "trust.json").string()};
    std::array<char *, 4> args{};
    for (std::size_t index = 0; index < args.size(); ++index)
        args[index] = options[index].data();
    REQUIRE_FALSE(RequireValue(ParseEditorImageProviderInspection(std::span{args}.first(1))).has_value());
    const auto selected = RequireValue(ParseEditorImageProviderInspection(args));
    REQUIRE(selected);
    CHECK(selected->mode == ReconstructionKind::Temporal);
    CHECK(selected->productRoot == root);
    CHECK(ParseEditorImageProviderInspection(std::span{args}.first(3)).HasError());
    args.back() = args[1];
    CHECK(ParseEditorImageProviderInspection(args).HasError());
}

TEST_CASE("Editor image provider activation explicitly selects spatial or same-extent temporal output",
          "[renderer][image-provider][editor]") {
    const auto root = std::filesystem::temp_directory_path().lexically_normal();
    std::array options{std::string{"HoroEditor"}, std::string{"--activate-image-provider=spatial"},
                       "--image-provider-root=" + root.string(), "--image-provider-trust=" + (root / "trust.json").string(),
                       std::string{"--inspect-image-provider=spatial"}};
    std::array<char *, 5> args{};
    const auto update = [&] {
        for (std::size_t i = 0; i < args.size(); ++i)
            args[i] = options[i].data();
    };
    update();
    const auto selected = RequireValue(ParseEditorImageProviderInspection(std::span{args}.first(4)));
    REQUIRE(selected);
    CHECK(selected->activateViewport);
    CHECK(selected->mode == ReconstructionKind::Spatial);
    CHECK(ParseEditorImageProviderInspection(args).HasError());
    options[1] = "--activate-image-provider=taa";
    update();
    const auto temporal = RequireValue(ParseEditorImageProviderInspection(std::span{args}.first(4)));
    REQUIRE(temporal);
    CHECK(temporal->activateViewport);
    CHECK(temporal->mode == ReconstructionKind::Temporal);
    options[1] = "--activate-image-provider=taau";
    update();
    CHECK(ParseEditorImageProviderInspection(std::span{args}.first(4)).HasError());
}

TEST_CASE("Editor image provider inspection admits only signed matching product inventory", "[renderer][image-provider][editor]") {
    InstalledDirectory installed;
    auto data = Inventory();
    EditorProviderProductIdentity expected{std::get<EngineProductVersion>(data.version), data.platform, data.architecture};
    Tests::OpenXRProduct::SigningKey signer;
    auto roots = std::make_shared<Security::TrustedRootStore>();
    REQUIRE(roots->Add(signer.root).HasValue());
    const auto crypto = Security::CreateMbedTlsSignatureProvider();
    REQUIRE(crypto);
    Security::ArtifactVerifier verifier{crypto, roots};
    const NativeDurableFileSystem files;
    const EditorImageProviderInspection request{ReconstructionKind::Spatial, installed.root, {}};
    RendererOwner renderer;

    SECTION("Trusted inventory with no requested cook reports real selected backend capability") {
        WriteSigned(installed, data, signer);
        const auto result = RequireValue(InspectEditorImageProvider(*renderer.frontend, request, expected, files, verifier));
        CHECK(result.route.mode == "spatial");
        CHECK(result.support.installation == ImageProviderInstallStatus::Linked);
        CHECK(result.support.readiness == ImageProviderReadiness::CapabilityUnavailable);
        CHECK(result.inventory != Sha256Digest{});
    }
    SECTION("Changing canonical bytes after signing is rejected before decoding") {
        WriteSigned(installed, data, signer);
        data.build = {"other_build"};
        const auto changed = RequireValue(ReleaseArtifactManifest::Create(data));
        installed.Write("manifest.json", changed.CanonicalJson());
        CHECK(InspectEditorImageProvider(*renderer.frontend, request, expected, files, verifier).HasError());
    }
    SECTION("Valid signature for the wrong editor platform cannot authorize content") {
        WriteSigned(installed, data, signer);
        expected.platform = DistributionPlatform::Windows;
        const auto rejected = InspectEditorImageProvider(*renderer.frontend, request, expected, files, verifier);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == "editor.image_provider.invalid_inventory");
    }
    SECTION("Activation refuses a scene material missing from the signed inventory") {
        WriteSigned(installed, data, signer);
        auto activate = request;
        activate.activateViewport = true;
        const auto rejected = LoadEditorSignedSpatialProvider(*renderer.frontend, activate, expected, files, verifier);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == "editor.image_provider.invalid_inventory");
        CHECK(InspectEditorImageProvider(*renderer.frontend, activate, expected, files, verifier).HasError());
    }
    SECTION("Activation refuses a substitution of the signed scene-linear payload") {
        const auto payload = "assets/shaders/editor/viewport-scene-linear-vulkan.horoshdr";
        const auto reflection = "assets/shaders/editor/viewport-scene-linear-vulkan.reflection.json";
        data.artifacts = {{payload, ReleaseArtifactRole::AssetArchive, 3, ComputeSha256(Bytes("abc"))},
                          {reflection, ReleaseArtifactRole::AssetArchive, 2, ComputeSha256(Bytes("{}"))}};
        WriteSigned(installed, data, signer);
        installed.Write(payload, "xyz");
        installed.Write(reflection, "{}");
        auto activate = request;
        activate.activateViewport = true;
        const auto rejected = LoadEditorSignedSpatialProvider(*renderer.frontend, activate, expected, files, verifier);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == "editor.image_provider.artifact_mismatch");
    }
    SECTION("A partial signed pair fails closed") {
        const auto path = "assets/shaders/runtime/native-reconstruction-vulkan/ReconstructSpatial.horoshdr";
        data.artifacts = {{path, ReleaseArtifactRole::AssetArchive, 3, ComputeSha256(Bytes("abc"))}};
        WriteSigned(installed, data, signer);
        installed.Write(path, "abc");
        const auto rejected = InspectEditorImageProvider(*renderer.frontend, request, expected, files, verifier);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == "editor.image_provider.invalid_inventory");
    }
    SECTION("A same-length modified shader is rejected before admission") {
        const auto payload = "assets/shaders/runtime/native-reconstruction-vulkan/ReconstructSpatial.horoshdr";
        const auto reflection = "assets/shaders/runtime/native-reconstruction-vulkan/ReconstructSpatial.reflection.json";
        data.artifacts = {{payload, ReleaseArtifactRole::AssetArchive, 3, ComputeSha256(Bytes("abc"))},
                          {reflection, ReleaseArtifactRole::AssetArchive, 2, ComputeSha256(Bytes("{}"))}};
        WriteSigned(installed, data, signer);
        installed.Write(payload, "xyz");
        installed.Write(reflection, "{}");
        const auto rejected = InspectEditorImageProvider(*renderer.frontend, request, expected, files, verifier);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == "editor.image_provider.artifact_mismatch");
    }
    SECTION("A signed inventory without temporal guides does not advertise them") {
        WriteSigned(installed, data, signer);
        auto temporal = request;
        temporal.mode = ReconstructionKind::Temporal;
        const auto report = RequireValue(InspectEditorImageProvider(*renderer.frontend, temporal, expected, files, verifier));
        CHECK(report.route.mode == "taau");
        CHECK_FALSE(report.temporalGuidesVerified);
    }
    SECTION("A declared temporal guide cannot omit its reflection") {
        const auto payload = "assets/shaders/editor/viewport-linear-depth-vulkan.horoshdr";
        data.artifacts = {{payload, ReleaseArtifactRole::AssetArchive, 3, ComputeSha256(Bytes("abc"))}};
        WriteSigned(installed, data, signer);
        installed.Write(payload, "abc");
        auto temporal = request;
        temporal.mode = ReconstructionKind::Temporal;
        const auto rejected = InspectEditorImageProvider(*renderer.frontend, temporal, expected, files, verifier);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == "editor.image_provider.invalid_inventory");
    }
    SECTION("A temporal guide with changed bytes is rejected under its real signed digest") {
        const auto payload = "assets/shaders/editor/viewport-linear-depth-vulkan.horoshdr";
        const auto reflection = "assets/shaders/editor/viewport-linear-depth-vulkan.reflection.json";
        data.artifacts = {{payload, ReleaseArtifactRole::AssetArchive, 3, ComputeSha256(Bytes("abc"))},
                          {reflection, ReleaseArtifactRole::AssetArchive, 2, ComputeSha256(Bytes("{}"))}};
        WriteSigned(installed, data, signer);
        installed.Write(payload, "xyz");
        installed.Write(reflection, "{}");
        auto temporal = request;
        temporal.mode = ReconstructionKind::Temporal;
        const auto rejected = InspectEditorImageProvider(*renderer.frontend, temporal, expected, files, verifier);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == "editor.image_provider.artifact_mismatch");
    }
}

TEST_CASE("Editor inspects and retains the complete signed temporal shader family", "[renderer][image-provider][editor][integration]") {
    const auto *stage = std::getenv("HORO_TEST_EDITOR_TEMPORAL_STAGE");
    if (!stage)
        SKIP("Set HORO_TEST_EDITOR_TEMPORAL_STAGE to the reviewed Vulkan temporal reconstruction cook output.");
    InstalledDirectory installed;
    auto data = Inventory();
    AddCookedShaders(installed, data, std::filesystem::path{stage});
    REQUIRE(data.artifacts.size() == 13U);
    const EditorProviderProductIdentity expected{std::get<EngineProductVersion>(data.version), data.platform, data.architecture};
    Tests::OpenXRProduct::SigningKey signer;
    auto roots = std::make_shared<Security::TrustedRootStore>();
    REQUIRE(roots->Add(signer.root).HasValue());
    const auto crypto = Security::CreateMbedTlsSignatureProvider();
    REQUIRE(crypto);
    const Security::ArtifactVerifier verifier{crypto, roots};
    RendererOwner renderer;
    const NativeDurableFileSystem files;
    const EditorImageProviderInspection temporal{ReconstructionKind::Temporal, installed.root, {}};
    SECTION("Preparation receives the same signed bytes and reports backend readiness separately") {
        WriteSigned(installed, data, signer);
        const auto report = RequireValue(InspectEditorImageProvider(*renderer.frontend, temporal, expected, files, verifier));
        CHECK(report.temporalGuidesVerified);
        const auto prepared = RequireValue(LoadEditorSignedTemporalProvider(*renderer.frontend, temporal, expected, files, verifier));
        CHECK(prepared.availability.inventory == report.inventory);
        CHECK(prepared.availability.support.readiness == ImageProviderReadiness::CapabilityUnavailable);
        CHECK(prepared.kernels[0].entry == NativeReconstructionEntry::TemporalSeed);
        CHECK(prepared.kernels[1].entry == NativeReconstructionEntry::TemporalHistory);
        const auto verify = [&data](std::string_view path, const std::vector<std::uint8_t> &bytes) {
            const auto record = std::ranges::find(data.artifacts, path, &ReleaseArtifactRecord::path);
            REQUIRE(record != data.artifacts.end());
            CHECK(ComputeSha256(std::as_bytes(std::span{bytes})) == record->digest);
        };
        verify("assets/shaders/editor/viewport-scene-linear-vulkan.horoshdr", prepared.sceneLinear.payload);
        verify("assets/shaders/editor/viewport-linear-depth-vulkan.reflection.json", prepared.linearDepth.reflection);
        verify("assets/shaders/editor/viewport-motion-vulkan.horoshdr", prepared.motion.payload);
    }
    SECTION("Preparation rejects a temporal family missing any required component") {
        for (const auto missing :
             {"assets/shaders/editor/viewport-scene-linear-vulkan.horoshdr", "assets/shaders/editor/viewport-motion-vulkan.reflection.json",
              "assets/shaders/runtime/native-reconstruction-vulkan/ReconstructTemporal.horoshdr"}) {
            CAPTURE(missing);
            auto partial = data;
            REQUIRE(std::erase_if(partial.artifacts, [missing](const auto &record) {
                return record.path == missing;
            }) == 1);
            WriteSigned(installed, std::move(partial), signer);
            CHECK(LoadEditorSignedTemporalProvider(*renderer.frontend, temporal, expected, files, verifier).HasError());
        }
    }
}
