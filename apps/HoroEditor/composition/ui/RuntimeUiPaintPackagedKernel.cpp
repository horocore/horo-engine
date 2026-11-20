#include "RuntimeUiPaintPackagedKernel.h"

#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"

#include <bit>
#include <fstream>
#include <new>

namespace Horo::Application::Internal {
    namespace {
        using Preparation = Result<std::shared_ptr<const Render::CookedUiPaintKernel>>;
        using PackageDecoder = Preparation (*)(std::span<const std::uint8_t>, std::span<const std::uint8_t>, Render::ShaderTargetBackend);

        /** @brief Reads only one fixed regular asset inside the explicit host-owned package, with finite preallocation. */
        Result<std::vector<std::uint8_t>> ReadPackageFile(const std::filesystem::path &root, const char *filename,
                                                          const std::size_t maximum, const CancellationToken &cancellation) {
            using Bytes = Result<std::vector<std::uint8_t>>;
            if (cancellation.IsCancellationRequested())
                return Bytes::Failure(MakeError(Render::ShaderCompilerPipelineErrors::CancellationRequested));
            std::error_code error;
            const auto canonicalRoot = std::filesystem::canonical(root, error);
            if (error)
                return Bytes::Failure(MakeError(Render::ShaderCompilerPipelineErrors::ScratchIoFailed));
            const auto directory = canonicalRoot / "shaders/runtime";
            const auto path = directory / filename;
            if (const auto resolved = std::filesystem::canonical(path, error); error || resolved != path)
                return Bytes::Failure(MakeError(Render::ShaderCompilerPipelineErrors::ScratchIoFailed));
            if (const auto status = std::filesystem::symlink_status(path, error); error || !std::filesystem::is_regular_file(status))
                return Bytes::Failure(MakeError(Render::ShaderCompilerPipelineErrors::ScratchIoFailed));
            const auto size = std::filesystem::file_size(path, error);
            if (error || size == 0 || size > maximum)
                return Bytes::Failure(MakeError(Render::ShaderCompilerPipelineErrors::ScratchIoFailed));
            std::ifstream stream(path, std::ios::binary);
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
            if (!stream.read(std::bit_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())) ||
                stream.peek() != std::char_traits<char>::eof())
                return Bytes::Failure(MakeError(Render::ShaderCompilerPipelineErrors::ScratchIoFailed));
            if (cancellation.IsCancellationRequested())
                return Bytes::Failure(MakeError(Render::ShaderCompilerPipelineErrors::CancellationRequested));
            return Bytes::Success(std::move(bytes));
        }

        /** @brief Reads and binds one exact caller-selected pair before publishing any native authority. */
        Preparation Load(const std::filesystem::path &root, Render::ShaderTargetBackend selected, Render::ShaderTargetBackend expected,
                         const char *payloadName, const char *metadataName, PackageDecoder decode, const CancellationToken &cancellation) {
            if (selected != expected)
                return Preparation::Failure(MakeError(Render::ShaderCompilerPipelineErrors::UnsupportedTarget));
            try {
                const auto payload = ReadPackageFile(root, payloadName, 16U * 1024U * 1024U, cancellation);
                if (payload.HasError())
                    return Preparation::Failure(payload.ErrorValue());
                const auto record = ReadPackageFile(root, metadataName, 1024U * 1024U, cancellation);
                if (record.HasError())
                    return Preparation::Failure(record.ErrorValue());
                return decode(payload.Value(), record.Value(), selected);
            } catch (const std::bad_alloc &) {
                return Preparation::Failure(MakeError(Render::ShaderCompilerPipelineErrors::AllocationFailed));
            }
        }
    }  // namespace

    /** @copydoc LoadPackagedProjectUiMetalKernel */
    Preparation LoadPackagedProjectUiMetalKernel(const std::filesystem::path &assetRoot, const Render::ShaderTargetBackend selectedBackend,
                                                 const CancellationToken &cancellation) {
        return Load(assetRoot, selectedBackend, Render::ShaderTargetBackend::Metal, "ui-paint-metal.horoshdr",
                    "ui-paint-metal.reflection.json", DecodeProjectUiPaintMetalPackage, cancellation);
    }

    /** @copydoc LoadPackagedProjectUiVulkanSceneKernel */
    Preparation LoadPackagedProjectUiVulkanSceneKernel(const std::filesystem::path &assetRoot,
                                                       const Render::ShaderTargetBackend selectedBackend,
                                                       const CancellationToken &cancellation) {
        return Load(assetRoot, selectedBackend, Render::ShaderTargetBackend::Vulkan, "ui-paint-vulkan.horoshdr",
                    "ui-paint-vulkan.reflection.json", DecodeProjectUiPaintVulkanScenePackage, cancellation);
    }
}  // namespace Horo::Application::Internal
