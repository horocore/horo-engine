#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"
#include "RuntimeUiPaintPackagedKernel.h"
#include "UiPaintPackageTestFixture.h"
#include "support/AllocationProbe.h"

#include <optional>

using namespace Horo;
using namespace Horo::Application::Internal;
using namespace Horo::Tests::UiPaintPackageTesting;

TEST_CASE("Application Metal package admits only the exact canonical payload and record", "[application][runtime_ui][package]") {
    const auto kernel = PackageMetadataKernel();
    const auto metadata = EncodeReflectedShaderArtifact(*kernel.artifact.reflection);
    REQUIRE(metadata.HasValue());
    auto payload = kernel.artifact.payload;
    auto record = metadata.Value();
    SECTION("Same issued record") {
        const auto decoded = DecodeProjectUiPaintMetalPackage(payload, record, ShaderTargetBackend::Metal);
        REQUIRE(decoded.HasValue());
        REQUIRE(ValidateCookedUiPaintKernel(*decoded.Value(), ShaderTargetBackend::Metal).HasValue());
        return;
    }
    SECTION("Changed payload") {
        payload.back() ^= 1;
    }
    SECTION("Truncated payload") {
        payload.pop_back();
    }
    SECTION("Trailing record") {
        record.push_back('x');
    }
    SECTION("Missing record") {
        record.clear();
    }
    SECTION("Oversized record") {
        record.resize(1024U * 1024U + 1);
    }
    REQUIRE(DecodeProjectUiPaintMetalPackage(payload, record, ShaderTargetBackend::Metal).HasError());
}

TEST_CASE("Application packaged UI explicitly denies unsupported backend before filesystem work", "[application][runtime_ui][package]") {
    CancellationToken cancellation;
    for (const auto backend : {ShaderTargetBackend::Vulkan, ShaderTargetBackend::OpenGL, ShaderTargetBackend::Null}) {
        const auto result = LoadPackagedProjectUiMetalKernel("/does-not-exist", backend, cancellation);
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == ShaderCompilerPipelineErrors::UnsupportedTarget.code.Value());
    }
}

TEST_CASE("Application packaged UI loader retains bounded immutable metadata without native qualification",
          "[application][runtime_ui][package]") {
    PackageDirectory package;
    package.PublishMetadataFixture();
    CancellationToken cancellation;
    const auto loaded = LoadPackagedProjectUiMetalKernel(package.root, ShaderTargetBackend::Metal, cancellation);
    REQUIRE(loaded.HasValue());
    std::filesystem::remove_all(package.root / "shaders");
    REQUIRE(ValidateCookedUiPaintKernel(*loaded.Value(), ShaderTargetBackend::Metal).HasValue());
}

TEST_CASE("Application packaged UI rejects missing partial tampered and escaping package inputs", "[application][runtime_ui][package]") {
    PackageDirectory package;
    package.PublishMetadataFixture();
    const auto payload = package.root / "shaders/runtime/ui-paint-metal.horoshdr";
    SECTION("Missing metadata") {
        std::filesystem::remove(package.root / "shaders/runtime/ui-paint-metal.reflection.json");
    }
    SECTION("Tampered payload") {
        std::ofstream output(payload, std::ios::binary | std::ios::app);
        output.put('x');
    }
    SECTION("Oversized payload") {
        std::filesystem::resize_file(payload, 16U * 1024U * 1024U + 1);
    }
    SECTION("Nonregular payload") {
        std::filesystem::remove(payload);
        std::filesystem::create_directory(payload);
    }
    SECTION("Symlink payload") {
        const auto alternate = package.root / "alternative";
        std::filesystem::rename(payload, alternate);
        std::error_code error;
        std::filesystem::create_symlink(alternate, payload, error);
        if (error) {
            SKIP("host does not permit test symlinks");
        }
    }
    CancellationToken cancellation;
    REQUIRE(LoadPackagedProjectUiMetalKernel(package.root, ShaderTargetBackend::Metal, cancellation).HasError());
}

TEST_CASE("Application packaged UI cancellation preserves the original typed boundary before IO", "[application][runtime_ui][package]") {
    CancellationSource source;
    source.RequestCancellation();
    const auto result = LoadPackagedProjectUiMetalKernel("/does-not-exist", ShaderTargetBackend::Metal, source.Token());
    REQUIRE(result.HasError());
    CHECK(result.ErrorValue().domain.Value() == ShaderCompilerPipelineErrors::CancellationRequested.domain.Value());
    CHECK(result.ErrorValue().code.Value() == ShaderCompilerPipelineErrors::CancellationRequested.code.Value());
}

TEST_CASE("Application Metal package allocation failure preserves typed exhaustion and retry",
          "[application][runtime_ui][package][allocation]") {
    const auto kernel = PackageMetadataKernel();
    const auto metadata = EncodeReflectedShaderArtifact(*kernel.artifact.reflection);
    REQUIRE(metadata.HasValue());
    REQUIRE(DecodeProjectUiPaintMetalPackage(kernel.artifact.payload, metadata.Value(), ShaderTargetBackend::Metal).HasValue());
    std::optional<Result<std::shared_ptr<const CookedUiPaintKernel>>> failed;
    {
        Tests::AllocationProbe::ScopedFailure failure;
        failed.emplace(DecodeProjectUiPaintMetalPackage(kernel.artifact.payload, metadata.Value(), ShaderTargetBackend::Metal));
    }
    REQUIRE(failed->HasError());
    CHECK(failed->ErrorValue().domain.Value() == ShaderCompilerPipelineErrors::AllocationFailed.domain.Value());
    CHECK(failed->ErrorValue().code.Value() == ShaderCompilerPipelineErrors::AllocationFailed.code.Value());
    REQUIRE(DecodeProjectUiPaintMetalPackage(kernel.artifact.payload, metadata.Value(), ShaderTargetBackend::Metal).HasValue());
}

TEST_CASE("Application packaged loader allocation failure cannot publish a partial kernel",
          "[application][runtime_ui][package][allocation]") {
    PackageDirectory package;
    package.PublishMetadataFixture();
    CancellationToken cancellation;
    std::optional<Result<std::shared_ptr<const CookedUiPaintKernel>>> failed;
    {
        Tests::AllocationProbe::ScopedFailure failure;
        failed.emplace(LoadPackagedProjectUiMetalKernel(package.root, ShaderTargetBackend::Metal, cancellation));
    }
    REQUIRE(failed->HasError());
    CHECK(failed->ErrorValue().domain.Value() == ShaderCompilerPipelineErrors::AllocationFailed.domain.Value());
    CHECK(failed->ErrorValue().code.Value() == ShaderCompilerPipelineErrors::AllocationFailed.code.Value());
    const auto retry = LoadPackagedProjectUiMetalKernel(package.root, ShaderTargetBackend::Metal, cancellation);
    REQUIRE(retry.HasValue());
    REQUIRE(ValidateCookedUiPaintKernel(*retry.Value(), ShaderTargetBackend::Metal).HasValue());
}
