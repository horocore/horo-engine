#include "ShaderCompilerToolchain.h"

#include "Horo/Platform/ExternalProcess.h"
#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"
#include "ShaderCompilerRoute.h"
#include "ShaderCompilerToolchainCatalog.h"
#include "ShaderCompilerToolchainSupport.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <iterator>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace Horo::Render {
    namespace {
        constexpr std::size_t HardMaximumToolBinaryBytes = 512U * 1024U * 1024U;
        constexpr std::size_t HardMaximumProcessOutputBytes = 8U * 1024U * 1024U;
        constexpr auto MaximumProcessTimeout = std::chrono::minutes{30};
        using ShaderCompilerToolchainDetail::IsApproved;
        using ShaderCompilerToolchainDetail::ReadBoundedFile;
        using ShaderCompilerToolchainDetail::WriteFile;

        [[nodiscard]] bool IsSafeIdentity(const std::string_view value) noexcept {
            return !value.empty() && value.size() <= 128U && std::ranges::all_of(value, [](const char character) {
                const auto byte = static_cast<unsigned char>(character);
                return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte == '.' ||
                       byte == '_' || byte == '-' || byte == '+';
            });
        }

        class ScratchDirectory final {
        public:
            explicit ScratchDirectory(std::filesystem::path path) : path_(std::move(path)) {}

            ScratchDirectory(const ScratchDirectory &) = delete;
            ScratchDirectory &operator=(const ScratchDirectory &) = delete;
            ScratchDirectory(ScratchDirectory &&) = delete;
            ScratchDirectory &operator=(ScratchDirectory &&) = delete;

            ~ScratchDirectory() noexcept {
                std::error_code ignored;
                std::filesystem::remove_all(path_, ignored);
            }

            [[nodiscard]] const std::filesystem::path &Path() const noexcept {
                return path_;
            }

        private:
            std::filesystem::path path_;
        };

    }  // namespace

    struct ExternalShaderCompilerAdapterState final {
        ShaderCompilerToolchainConfiguration configuration;
        std::shared_ptr<IExternalProcessRunner> processes;
        std::atomic_uint64_t nextInvocation{1};
    };

    ExternalShaderCompilerAdapter::ExternalShaderCompilerAdapter(std::shared_ptr<ExternalShaderCompilerAdapterState> state) noexcept
        : state_(std::move(state)) {}

    /** @copydoc ExternalShaderCompilerAdapter::~ExternalShaderCompilerAdapter */
    ExternalShaderCompilerAdapter::~ExternalShaderCompilerAdapter() noexcept = default;

    /** @copydoc ExternalShaderCompilerAdapter::ExternalShaderCompilerAdapter */
    ExternalShaderCompilerAdapter::ExternalShaderCompilerAdapter(ExternalShaderCompilerAdapter &&) noexcept = default;

    /** @copydoc ExternalShaderCompilerAdapter::operator= */
    ExternalShaderCompilerAdapter &ExternalShaderCompilerAdapter::operator=(ExternalShaderCompilerAdapter &&) noexcept = default;

    /** @copydoc ExternalShaderCompilerAdapter::Create */
    Result<ExternalShaderCompilerAdapter> ExternalShaderCompilerAdapter::Create(ShaderCompilerToolchainConfiguration configuration,
                                                                                std::shared_ptr<IExternalProcessRunner> processes) {
        if (!IsSafeIdentity(configuration.hostPlatform) || !configuration.scratchRoot.is_absolute() ||
            configuration.processTimeout <= std::chrono::milliseconds::zero() || configuration.processTimeout > MaximumProcessTimeout ||
            configuration.maximumToolBinaryBytes == 0 || configuration.maximumToolBinaryBytes > HardMaximumToolBinaryBytes ||
            configuration.maximumProcessOutputBytes == 0 || configuration.maximumProcessOutputBytes > HardMaximumProcessOutputBytes ||
            configuration.tools.size() > 5U || processes == nullptr)
            return Result<ExternalShaderCompilerAdapter>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolchainConfigurationInvalid));

        std::error_code error;
        std::filesystem::create_directories(configuration.scratchRoot, error);
        if (error || !std::filesystem::is_directory(configuration.scratchRoot, error))
            return Result<ExternalShaderCompilerAdapter>::Failure(MakeError(ShaderCompilerPipelineErrors::ScratchIoFailed));

        for (std::size_t index = 0; index < configuration.tools.size(); ++index) {
            const auto &tool = configuration.tools[index];
            if (!tool.executable.is_absolute() || (index > 0 && configuration.tools[index - 1].identity.tool >= tool.identity.tool))
                return Result<ExternalShaderCompilerAdapter>::Failure(
                    MakeError(ShaderCompilerPipelineErrors::ToolchainConfigurationInvalid));
            if (!IsApproved(configuration, tool))
                return Result<ExternalShaderCompilerAdapter>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolNotApproved));
            auto executable =
                ReadBoundedFile(tool.executable, configuration.maximumToolBinaryBytes, ShaderCompilerPipelineErrors::ToolMissing);
            if (executable.HasError())
                return Result<ExternalShaderCompilerAdapter>::Failure(std::move(executable).ErrorValue());
            const Sha256Digest actual = ComputeSha256(std::as_bytes(std::span{executable.Value()}));
            if (actual != tool.executableDigest)
                return Result<ExternalShaderCompilerAdapter>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolDigestMismatch));
        }

        try {
            auto state = std::make_shared<ExternalShaderCompilerAdapterState>();
            state->configuration = std::move(configuration);
            state->processes = std::move(processes);
            return Result<ExternalShaderCompilerAdapter>::Success(ExternalShaderCompilerAdapter(std::move(state)));
        } catch (const std::bad_alloc &) {
            return Result<ExternalShaderCompilerAdapter>::Failure(MakeError(ShaderCompilerPipelineErrors::AllocationFailed));
        }
    }

    /** @copydoc ExternalShaderCompilerAdapter::Compile */
    Result<ShaderCompilerAdapterOutput> ExternalShaderCompilerAdapter::Compile(const ShaderCompilerInvocation &invocation,
                                                                               const CancellationToken &cancellation) const {
        if (!state_)
            return Result<ShaderCompilerAdapterOutput>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolchainConfigurationInvalid));
        if (cancellation.IsCancellationRequested())
            return Result<ShaderCompilerAdapterOutput>::Failure(MakeError(ShaderCompilerPipelineErrors::CancellationRequested));

        const std::uint64_t sequence = state_->nextInvocation.fetch_add(1, std::memory_order_relaxed);
        if (sequence == 0)
            return Result<ShaderCompilerAdapterOutput>::Failure(MakeError(ShaderCompilerPipelineErrors::ScratchIoFailed));
        std::filesystem::path scratchPath = state_->configuration.scratchRoot / std::format("shader-{}", sequence);
        std::error_code ioError;
        if (!std::filesystem::create_directory(scratchPath, ioError) || ioError)
            return Result<ShaderCompilerAdapterOutput>::Failure(MakeError(ShaderCompilerPipelineErrors::ScratchIoFailed));
        ScratchDirectory scratch(std::move(scratchPath));
        const std::filesystem::path sourcePath = scratch.Path() / "source.hlsl";
        if (auto written = WriteFile(sourcePath, invocation.source); written.HasError())
            return Result<ShaderCompilerAdapterOutput>::Failure(std::move(written).ErrorValue());
        for (const ShaderCompilerDependency &dependency : invocation.dependencies) {
            const std::filesystem::path dependencyPath = scratch.Path() / dependency.logicalPath;
            std::filesystem::create_directories(dependencyPath.parent_path(), ioError);
            if (ioError)
                return Result<ShaderCompilerAdapterOutput>::Failure(MakeError(ShaderCompilerPipelineErrors::ScratchIoFailed));
            if (auto written = WriteFile(dependencyPath, dependency.content); written.HasError())
                return Result<ShaderCompilerAdapterOutput>::Failure(std::move(written).ErrorValue());
        }

        return ShaderCompilerToolchainDetail::CompileRoute(state_->configuration, *state_->processes, invocation, cancellation,
                                                           scratch.Path(), sourcePath);
    }
}  // namespace Horo::Render
