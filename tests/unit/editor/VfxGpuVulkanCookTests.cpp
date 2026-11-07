#include "AtomicShaderCookPackage.h"
#include "AuthenticatedMetalCookConfiguration.h"
#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"
#include "HostSignatureDocument.h"
#include "VfxGpuVulkanCook.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>
#include <mbedtls/bignum.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <type_traits>

namespace {
    using namespace Horo;
    using namespace Horo::Security;
    using namespace Horo::Render;
    using namespace Horo::Application::Internal;
    using namespace Horo::Editor::Offline;

    int TestRandom(void *state, unsigned char *output, const std::size_t length) {
        auto &value = *static_cast<std::uint32_t *>(state);
        for (std::size_t index = 0; index < length; ++index) {
            value = value * 1664525U + 1013904223U;
            output[index] = static_cast<unsigned char>(value >> 24U);
        }
        return 0;
    }

    struct SignedFixture {
        std::vector<std::byte> artifact;
        TrustedSigningKey trustedKey;
        DetachedSignatureEnvelope envelope;
    };

    /** @brief Test-only signing state; assertion unwinding releases every native crypto allocation. */
    struct SigningState final {
        mbedtls_ecp_group group;
        mbedtls_ecp_point publicPoint;
        mbedtls_mpi privateKey;
        mbedtls_mpi r;
        mbedtls_mpi s;

        SigningState() {
            mbedtls_ecp_group_init(&group);
            mbedtls_ecp_point_init(&publicPoint);
            mbedtls_mpi_init(&privateKey);
            mbedtls_mpi_init(&r);
            mbedtls_mpi_init(&s);
        }

        ~SigningState() {
            mbedtls_mpi_free(&s);
            mbedtls_mpi_free(&r);
            mbedtls_mpi_free(&privateKey);
            mbedtls_ecp_point_free(&publicPoint);
            mbedtls_ecp_group_free(&group);
        }

        SigningState(const SigningState &) = delete;
        SigningState &operator=(const SigningState &) = delete;
    };

    /** @brief Signs arbitrary test inputs with an explicitly test-only root, never a production catalog approval. */
    [[nodiscard]] SignedFixture MakeSignedFixture(const std::string_view Artifact) {
        SignedFixture fixture;
        fixture.artifact.assign(reinterpret_cast<const std::byte *>(Artifact.data()),
                                reinterpret_cast<const std::byte *>(Artifact.data() + Artifact.size()));
        fixture.envelope.publisherId = "com.horo.tests";
        fixture.envelope.keyId = "release-1";
        fixture.envelope.artifactDigest = ComputeSha256(fixture.artifact);
        const Sha256Digest payloadDigest = ComputeSignaturePayloadDigest(fixture.envelope);

        SigningState state;
        auto &group = state.group;
        auto &publicPoint = state.publicPoint;
        auto &privateKey = state.privateKey;
        auto &r = state.r;
        auto &s = state.s;
        REQUIRE(mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) == 0);
        REQUIRE(mbedtls_mpi_lset(&privateKey, 1) == 0);
        std::uint32_t randomState = 0x48305230U;
        REQUIRE(mbedtls_ecp_mul(&group, &publicPoint, &privateKey, &group.G, TestRandom, &randomState) == 0);
        REQUIRE(mbedtls_ecdsa_sign(&group, &r, &s, &privateKey, payloadDigest.bytes.data(), payloadDigest.bytes.size(), TestRandom,
                                   &randomState) == 0);

        fixture.trustedKey.publisherId = fixture.envelope.publisherId;
        fixture.trustedKey.keyId = fixture.envelope.keyId;
        fixture.trustedKey.publicKey.resize(65U);
        std::size_t publicKeySize{};
        REQUIRE(mbedtls_ecp_point_write_binary(&group, &publicPoint, MBEDTLS_ECP_PF_UNCOMPRESSED, &publicKeySize,
                                               reinterpret_cast<unsigned char *>(fixture.trustedKey.publicKey.data()),
                                               fixture.trustedKey.publicKey.size()) == 0);
        REQUIRE(publicKeySize == fixture.trustedKey.publicKey.size());
        fixture.envelope.signature.resize(64U);
        REQUIRE(mbedtls_mpi_write_binary(&r, reinterpret_cast<unsigned char *>(fixture.envelope.signature.data()), 32U) == 0);
        REQUIRE(mbedtls_mpi_write_binary(&s, reinterpret_cast<unsigned char *>(fixture.envelope.signature.data()) + 32U, 32U) == 0);
        return fixture;
    }

    struct OutputRoot {
        std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            ("horo vulkan vfx cook " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        OutputRoot() {
            std::filesystem::create_directory(path);
        }

        ~OutputRoot() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    };

    void Write(const std::filesystem::path &path, std::string_view text) {
        std::ofstream output(path, std::ios::binary);
        output << text;
        REQUIRE(output.good());
    }

    std::string Hex(std::span<const std::byte> bytes) {
        constexpr std::string_view digits = "0123456789abcdef";
        std::string result;
        for (const auto byte : bytes) {
            const auto value = std::to_integer<unsigned>(byte);
            result.push_back(digits[value >> 4]);
            result.push_back(digits[value & 15]);
        }
        return result;
    }

    std::string Catalog(bool apple) {
        std::string result = "{\"version\":1,\"host\":\"" + std::string{ActualShaderCookHost()} + "\",\"tools\":[";
        const std::array roles{"dxc", "spirv-val", "spirv-cross", "apple-metal"};
        for (std::size_t i = 0; i < (apple ? 4u : 3u); ++i) {
            if (i)
                result += ',';
            result += "{\"role\":\"" + std::string{roles[i]} +
                      "\",\"release\":\"test-only\",\"archive_sha256\":\"sha256:" + std::string(64, '1') +
                      "\",\"executable_sha256\":\"sha256:" + std::string(64, '2') + "\"}";
        }
        return result + "]}";
    }

    AuthenticatedSpirvCookInputs Inputs(const OutputRoot &root, bool apple = false) {
        auto fixture = MakeSignedFixture(Catalog(apple));
        const auto catalog = root.path / "catalog.json", signature = root.path / "signature.json", trust = root.path / "trust.json";
        Write(catalog, std::string_view{reinterpret_cast<const char *>(fixture.artifact.data()), fixture.artifact.size()});
        Write(signature, EncodeDetachedSignatureDocument(fixture.envelope));
        Write(trust, "{\"schemaVersion\":1,\"allowUnsigned\":false,\"publishers\":[{\"publisherId\":\"com.horo.tests\","
                     "\"keyId\":\"release-1\",\"algorithm\":\"ecdsa-p256-sha256\",\"publicKeyHex\":\"" +
                         Hex(fixture.trustedKey.publicKey) + "\",\"expiresAtUnixMilliseconds\":0,\"revoked\":false}]}");
        return {root.path / "scratch", catalog, signature, trust, {root.path / "dxc", root.path / "val", root.path / "cross"}};
    }
}  // namespace

TEST_CASE("Authenticated SPIR-V configuration needs only three signed tool roles and no Apple input", "[vfx][cook][vulkan][security]") {
    if (ActualShaderCookHost().empty())
        SKIP("This host has no authenticated offline cook host identity.");
    OutputRoot root;
    const auto inputs = Inputs(root);
    const auto result = MakeAuthenticatedSpirvCookConfiguration(inputs);
    REQUIRE(result.HasValue());
    REQUIRE(result.Value().configuration.tools.size() == 3);
    CHECK(result.Value().configuration.tools[2].identity.tool == ShaderCompilerTool::SpirvCross);
    const auto descriptor = VfxGpuVulkanCookDescriptor(result.Value().configuration);
    REQUIRE(descriptor.HasValue());
    REQUIRE(descriptor.Value().tools.size() == 2);
    CHECK(descriptor.Value().tools[0].tool == ShaderCompilerTool::Dxc);
    CHECK(descriptor.Value().tools[1].tool == ShaderCompilerTool::SpirvTools);
    CHECK(descriptor.Value().requirement.payloadFormat == ShaderPayloadFormat::SpirV16);
    CHECK_FALSE(std::filesystem::exists(inputs.scratchRoot));
    // Actual crypto over explicit test-only catalog tuples; no test executable is admitted or invoked.
    AuthenticatedMetalCookInputs metal{inputs.scratchRoot,
                                       inputs.catalog,
                                       inputs.detachedSignature,
                                       inputs.hostTrust,
                                       {inputs.executables[0], inputs.executables[1], inputs.executables[2], root.path / "xcrun"}};
    CHECK(MakeAuthenticatedMetalCookConfiguration(metal).HasError());
}

TEST_CASE("Metal authentication preserves its exact four-tool configuration and document bytes", "[vfx][cook][metal][security]") {
    STATIC_REQUIRE(std::is_same_v<AuthenticatedMetalCookConfiguration, AuthenticatedShaderCookConfiguration>);
    if (ActualShaderCookHost().empty())
        SKIP("This host has no authenticated offline cook host identity.");
    OutputRoot root;
    const auto inputs = Inputs(root, true);
    AuthenticatedMetalCookInputs metal{inputs.scratchRoot,
                                       inputs.catalog,
                                       inputs.detachedSignature,
                                       inputs.hostTrust,
                                       {inputs.executables[0], inputs.executables[1], inputs.executables[2], root.path / "xcrun"}};
    const auto configured = MakeAuthenticatedMetalCookConfiguration(metal);
    REQUIRE(configured.HasValue());
    REQUIRE(configured.Value().configuration.tools.size() == 4);
    CHECK(configured.Value().configuration.tools[3].identity.tool == ShaderCompilerTool::AppleMetal);
    CHECK(*configured.Value().catalogBytes == ReadShaderCookInput(inputs.catalog, 65536).Value());
    CHECK(*configured.Value().signatureBytes == ReadShaderCookInput(inputs.detachedSignature, 65536).Value());
    CHECK(ActualMetalCookHost() == ActualShaderCookHost());
}

TEST_CASE("SPIR-V authentication rejects tampered catalog and missing independent trust before tool use", "[vfx][cook][vulkan][security]") {
    if (ActualShaderCookHost().empty())
        SKIP("This host has no authenticated offline cook host identity.");
    OutputRoot root;
    const auto inputs = Inputs(root);
    SECTION("catalog bytes changed after signing") {
        Write(inputs.catalog, Catalog(true));
    }
    SECTION("detached signature invalid") {
        Write(inputs.detachedSignature, "{}");
    }
    SECTION("independent trust missing") {
        std::filesystem::remove(inputs.hostTrust);
    }
    SECTION("wrong host signature-valid bytes still not approved") {
        Write(inputs.catalog, "{\"version\":1,\"host\":\"foreign\",\"tools\":[]}");
    }
    CHECK(MakeAuthenticatedSpirvCookConfiguration(inputs).HasError());
    CHECK_FALSE(std::filesystem::exists(inputs.scratchRoot));
}

TEST_CASE("Vulkan producer rejects malformed family and preserves foreign publication paths", "[vfx][cook][vulkan][rollback]") {
    OutputRoot root;
    const auto scratch = root.path / "scratch";
    std::filesystem::create_directory(scratch);
    const auto foreign = root.path / ".vfx-gpu-vulkan-package-0";
    std::filesystem::create_directory(foreign);
    Write(foreign / "marker", "foreign");
    const auto output = root.path / "output";
    CHECK(PublishVfxGpuVulkanCook(output, {}, {}).HasError());
    CHECK_FALSE(std::filesystem::exists(output));
    CHECK(std::filesystem::file_size(foreign / "marker") == 7);
    CHECK(ValidateVfxGpuVulkanCookPaths(root.path / ".VFX-GPU-VULKAN-PACKAGE-other", scratch).HasError());
    CHECK(ValidateVfxGpuVulkanCookPaths(scratch / "output", scratch).HasError());
    CHECK(std::filesystem::is_empty(scratch));
}

TEST_CASE("Vulkan producer uses both canonical actual asset paths and complete target flags", "[vfx][cook][vulkan]") {
    OutputRoot root;
    CHECK(VfxGpuVulkanCookSourcePath(root.path, 0).Value() == root.path / "assets/shaders/runtime/vfx_simulation.hlsl");
    CHECK(VfxGpuVulkanCookSourcePath(root.path, 1).Value() == root.path / "assets/shaders/runtime/vfx_gpu_draw.hlsl");
    CHECK(VfxGpuVulkanCookSourcePath(root.path, 2).HasError());
    const auto target = VfxGpuVulkanCookTarget();
    CHECK(target.supportsCompute);
    CHECK(target.supportsStorageResources);
    CHECK(target.backend == ShaderTargetBackend::Vulkan);
}

TEST_CASE("Neutral publisher removes only its own Vulkan candidate after an invalid program write", "[vfx][cook][vulkan][rollback]") {
    OutputRoot root;
    const auto foreign = root.path / ".vfx-gpu-vulkan-package-0";
    std::filesystem::create_directory(foreign);
    Write(foreign / "marker", "foreign");
    const auto manifest = MakeVfxGpuShaderManifest(VfxGpuProgramRole::Simulation, VfxGpuVulkanCookTarget());
    const ShaderCompilationBatch missingArtifact;
    const std::array programs{AtomicShaderCookProgram{"shaders/runtime/vfx-simulation-vulkan", &manifest, &missingArtifact}};
    const AtomicShaderCookPackage package{programs, ".vfx-gpu-vulkan-package-", "provenance.json",
                                          [](const std::filesystem::path &, const void *) {
        return Result<void>::Success();
    }, nullptr};
    CHECK(PublishAtomicShaderCookPackage(root.path / "output", package, {}).HasError());
    CHECK_FALSE(std::filesystem::exists(root.path / "output"));
    CHECK_FALSE(std::filesystem::exists(root.path / ".vfx-gpu-vulkan-package-1"));
    CHECK(std::filesystem::file_size(foreign / "marker") == 7);
    CHECK(std::distance(std::filesystem::directory_iterator(root.path), std::filesystem::directory_iterator{}) == 1);
}
