#include "Horo/Runtime/Render/RenderBackendRegistry.h"
#include "Horo/Runtime/Render/VfxGpuKernel.h"
#include "VfxGpuCook.h"
#include "runtime/renderer/modules/metal/MetalBackendModule.h"

#import <QuartzCore/CAMetalLayer.h>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

    /** @brief Preserves the actual typed error at every public cold native boundary. */
    template <class T> T Value(Result<T> result) {
        INFO((result.HasError() ? result.ErrorValue().code.Value() + ": " + result.ErrorValue().message : "Actual operation accepted."));
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    /** @brief Requires the original operation without substituting a provider or capabilities. */
    void Accepted(const Result<void> &result) {
        INFO((result.HasError() ? result.ErrorValue().code.Value() + ": " + result.ErrorValue().message : "Actual operation accepted."));
        REQUIRE(result.HasValue());
    }

    /** @brief Supplies an actual unattached Metal layer; no drawable or window is acquired. */
    class ColdMetalPort final : public IMetalPresentationPort {
    public:
        Result<void> CreateSurface() override {
            layer_ = [CAMetalLayer layer];
            REQUIRE(layer_ != nil);
            return Result<void>::Success();
        }

        void *Layer() const noexcept override {
            return (__bridge void *)layer_;
        }

        void DestroySurface() noexcept override {
            layer_ = nil;
        }

    private:
        __strong CAMetalLayer *layer_{nil};
    };

    /** @brief Reads only a bounded existing cooked file; never compiles or synthesizes payloads. */
    std::vector<std::uint8_t> Read(const std::filesystem::path &path) {
        const auto size = std::filesystem::file_size(path);
        REQUIRE(size > 0);
        REQUIRE(size <= 16U * 1024U * 1024U);
        std::ifstream stream(path, std::ios::binary);
        REQUIRE(stream.is_open());
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        REQUIRE(stream.gcount() == static_cast<std::streamsize>(bytes.size()));
        REQUIRE(stream.peek() == std::char_traits<char>::eof());
        return bytes;
    }

    /** @brief Decodes actual canonical metadata and binds it to the exact HOROSHDR bytes. */
    CookedVfxGpuProgram Load(const std::filesystem::path &root, VfxGpuProgramRole role, std::string_view prefix) {
        const auto target = Editor::Offline::VfxGpuMetalCookTarget();
        auto manifest = MakeVfxGpuShaderManifest(role, target);
        auto payload = Read(root / (std::string{prefix} + ".horoshdr"));
        const auto metadata = Read(root / (std::string{prefix} + ".reflection.json"));
        auto bound = Value(DecodeReflectedShaderArtifact(manifest, target, payload, metadata));
        CompiledShaderArtifact artifact{.backend = target.backend,
                                        .payloadFormat = target.payloadFormat,
                                        .artifactKey = bound.compiledArtifactKey,
                                        .payload = std::move(payload),
                                        .reflection = std::make_shared<const ReflectedShaderArtifact>(std::move(bound))};
        const auto &r = artifact.reflection->reflection;
        ShaderReflectionCandidate candidate{r.backend,        r.interfaceSchemaVersion, r.bindings, r.parameters,
                                            r.stageInterface, r.targetBindings,         r.sourceMap};
        return {std::move(manifest), target, std::move(artifact), std::move(candidate)};
    }

    /** @brief Keeps native ownership alive until the actual public shutdown proves retirement. */
    void Drain(IRenderBackend &backend) {
        Accepted(backend.BeginShutdown());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline) {
            if (Value(backend.PollDrain()) == RenderBackendDrainProgress::Drained)
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        FAIL("Actual Metal backend did not drain within five seconds.");
    }

    /** @brief Proves real library/function/compute and draw pipeline ABI creation through IRenderBackend only. */
    void Realize(const std::filesystem::path &root) {
        const auto prefixes = Editor::Offline::VfxGpuCookPrefixes();
        CookedVfxGpuPrograms family{Load(root, VfxGpuProgramRole::Simulation, prefixes[0]),
                                    Load(root, VfxGpuProgramRole::Draw, prefixes[1])};
        Accepted(ValidateCookedVfxGpuPrograms(family, ShaderTargetBackend::Metal));
        ColdMetalPort port;
        MetalEditorGraphicsBridge bridge;
        RenderBackendRegistry registry;
        Accepted(RegisterMetalRenderBackend(registry, port, bridge));
        Accepted(registry.Seal());
        auto backend = Value(registry.Create(RenderBackendId{"metal"}));
        Accepted(backend->Initialize({.requirePresentation = false, .enableValidation = true, .maxFramesInFlight = 2}));
        auto realized = backend->RealizeVfxGpuPrograms(family);
        INFO((realized.HasError() ? realized.ErrorValue().code.Value() + ": " + realized.ErrorValue().message
                                  : "Actual native PSOs accepted."));
        bool resident = false;
        if (realized.HasValue()) {
            auto kernel = std::move(realized).Value();
            resident = kernel != nullptr;
            kernel.reset();
        }
        Drain(*backend);
        REQUIRE(resident);
    }
}  // namespace

TEST_CASE("Metal realizes the genuine GPU VFX compute and draw program family", "[integration][renderer][gpu][vfx][programs]") {
    const char *configured = std::getenv("HORO_VFX_GPU_COOKED_ROOT");
    if (configured == nullptr || *configured == '\0')
        SKIP("Set HORO_VFX_GPU_COOKED_ROOT to an authenticated real Metal cooked family; no runtime cooking is performed.");
    @autoreleasepool {
        Realize(std::filesystem::path{configured});
    }
}
