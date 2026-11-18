#include "EditorImageProviderHost.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Runtime/Render/NullBackendModule.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "HostSignatureDocument.h"
#include "support/OpenXRProductSigningTestSupport.h"
#include "support/renderer/RenderBackendDrainTestSupport.h"

#include <array>
#include <chrono>
#include <fstream>
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
}
