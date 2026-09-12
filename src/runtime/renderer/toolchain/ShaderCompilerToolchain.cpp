#include "ShaderCompilerToolchain.h"

#include "Horo/Platform/ExternalProcess.h"
#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace Horo::Render {
    namespace {
        constexpr std::size_t HardMaximumToolBinaryBytes = 512U * 1024U * 1024U;
        constexpr std::size_t HardMaximumProcessOutputBytes = 8U * 1024U * 1024U;
        constexpr auto MaximumProcessTimeout = std::chrono::minutes{30};
        constexpr std::array<std::uint8_t, 8> PackageMagic{'H', 'O', 'R', 'O', 'S', 'H', 'D', 'R'};
        constexpr std::uint32_t PackageVersion = 1;

        // Distribution archives and executables were retrieved from the named upstream source and
        // reviewed on 2026-09-13. The companion lock document records runtime dependencies and licenses.
        constexpr std::array ApprovedTools{
            ApprovedShaderCompilerTool{ShaderCompilerTool::Dxc, "linux-x86_64-ubuntu-26.04", "v1.9.2607",
                                       "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2607/"
                                       "linux_dxc_2026_07_29.x86_x64.tar.gz",
                                       "sha256:55665c87824051ed4774ff3280a79ccbbb7d39243b9736ca5e98222134112d54",
                                       "sha256:b1bfa493d5c780b94c20b8b5f5aed50d1c4d03339cd55f496bd223eedeec1734", "NCSA-and-MIT"},
            ApprovedShaderCompilerTool{ShaderCompilerTool::SpirvTools, "linux-x86_64-ubuntu-26.04", "ubuntu-2026.1-1",
                                       "http://archive.ubuntu.com/ubuntu/pool/universe/s/spirv-tools/spirv-tools_2026.1-1_amd64.deb",
                                       "sha256:24e972ed4f2e92ada6f64b32ff40550fda02038385656736af871ca3dcb2b867",
                                       "sha256:85367fefdb7e93ae45654255ac2b7f8dc7056b6df78a6fdeb03ce395c7477239", "Apache-2.0"},
            ApprovedShaderCompilerTool{ShaderCompilerTool::SpirvCross, "linux-x86_64-ubuntu-26.04", "ubuntu-2021.01.15+1.4.335.0-1",
                                       "http://archive.ubuntu.com/ubuntu/pool/universe/s/spirv-cross/"
                                       "spirv-cross_2021.01.15+1.4.335.0-1_amd64.deb",
                                       "sha256:50d11b7efc263240d04b015fecfd419377a4a3e2a9cb59387f2229aae03e4e7f",
                                       "sha256:335caee5ce86daefc3dee5e13100c2118a1a1cccb183a8c6df817090e0cbb976", "Apache-2.0"},
            ApprovedShaderCompilerTool{ShaderCompilerTool::DxilValidator, "linux-x86_64-ubuntu-26.04", "v1.9.2607",
                                       "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2607/"
                                       "linux_dxc_2026_07_29.x86_x64.tar.gz",
                                       "sha256:55665c87824051ed4774ff3280a79ccbbb7d39243b9736ca5e98222134112d54",
                                       "sha256:87cc9c1e459a7d6a0dc52c7b319d627d0f6078a2be95621e5a98b3cf12f255cb", "NCSA-and-MIT"},
        };

        [[nodiscard]] bool IsSafeIdentity(const std::string_view value) noexcept {
            return !value.empty() && value.size() <= 128U && std::ranges::all_of(value, [](const char character) {
                const auto byte = static_cast<unsigned char>(character);
                return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte == '.' ||
                       byte == '_' || byte == '-' || byte == '+';
            });
        }

        [[nodiscard]] std::optional<Sha256Digest> ParseLockedDigest(const std::string_view value) {
            auto parsed = ParseSha256(value);
            if (parsed.HasError())
                return std::nullopt;
            return parsed.Value();
        }

        [[nodiscard]] const ApprovedShaderCompilerTool *FindApproved(const std::string_view hostPlatform,
                                                                     const ShaderCompilerToolIdentity &identity) {
            const auto found = std::ranges::find_if(ApprovedTools, [&](const ApprovedShaderCompilerTool &entry) {
                const auto archiveDigest = ParseLockedDigest(entry.archiveSha256);
                return archiveDigest.has_value() && entry.hostPlatform == hostPlatform && entry.tool == identity.tool &&
                       entry.release == identity.release && *archiveDigest == identity.buildDigest;
            });
            return found == ApprovedTools.end() ? nullptr : &*found;
        }

        [[nodiscard]] Result<std::vector<std::uint8_t>> ReadBoundedFile(const std::filesystem::path &path, const std::size_t maximumBytes,
                                                                        const ErrorCodeDescriptor &failure) {
            std::error_code error;
            const auto status = std::filesystem::symlink_status(path, error);
            if (error || !std::filesystem::is_regular_file(status))
                return Result<std::vector<std::uint8_t>>::Failure(MakeError(failure));
            const std::uintmax_t size = std::filesystem::file_size(path, error);
            if (error || size == 0 || size > maximumBytes || size > std::numeric_limits<std::size_t>::max())
                return Result<std::vector<std::uint8_t>>::Failure(MakeError(failure));
            try {
                std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
                std::ifstream input(path, std::ios::binary);
                if (!input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())) ||
                    input.peek() != std::char_traits<char>::eof())
                    return Result<std::vector<std::uint8_t>>::Failure(MakeError(failure));
                return Result<std::vector<std::uint8_t>>::Success(std::move(bytes));
            } catch (const std::bad_alloc &) {
                return Result<std::vector<std::uint8_t>>::Failure(MakeError(failure));
            }
        }

        [[nodiscard]] Result<void> WriteFile(const std::filesystem::path &path, const std::span<const std::uint8_t> bytes) {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())) || !output.flush())
                return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ScratchIoFailed));
            return Result<void>::Success();
        }

        template <typename ValueT> void AppendInteger(std::vector<std::uint8_t> &output, ValueT value) {
            using UnsignedT = std::make_unsigned_t<ValueT>;
            UnsignedT bits = static_cast<UnsignedT>(value);
            for (std::size_t index = 0; index < sizeof(UnsignedT); ++index) {
                output.push_back(static_cast<std::uint8_t>(bits & 0xffU));
                if constexpr (sizeof(UnsignedT) > 1U)
                    bits >>= 8U;
            }
        }

        void AppendBytes(std::vector<std::uint8_t> &output, const std::span<const std::uint8_t> bytes) {
            AppendInteger(output, static_cast<std::uint64_t>(bytes.size()));
            output.insert(output.end(), bytes.begin(), bytes.end());
        }

        struct ToolArtifactRecord final {
            ShaderStage stage{ShaderStage::Vertex};
            std::string entryPoint;
            std::vector<std::uint8_t> bytes;
        };

        [[nodiscard]] Result<std::vector<std::uint8_t>> PackageStages(const ShaderTargetBackend backend, const ShaderPayloadFormat format,
                                                                      const std::vector<ToolArtifactRecord> &stages,
                                                                      const std::size_t maximumBytes) {
            try {
                std::vector<std::uint8_t> output(PackageMagic.begin(), PackageMagic.end());
                AppendInteger(output, PackageVersion);
                AppendInteger(output, static_cast<std::uint8_t>(backend));
                AppendInteger(output, static_cast<std::uint8_t>(format));
                AppendInteger(output, static_cast<std::uint32_t>(stages.size()));
                for (const ToolArtifactRecord &record : stages) {
                    constexpr std::size_t RecordHeaderBytes = sizeof(std::uint8_t) + sizeof(std::uint32_t) + sizeof(std::uint64_t);
                    if (maximumBytes < RecordHeaderBytes || record.entryPoint.size() > std::numeric_limits<std::uint32_t>::max() ||
                        record.entryPoint.size() > maximumBytes - RecordHeaderBytes || record.bytes.size() > maximumBytes ||
                        output.size() > maximumBytes - RecordHeaderBytes ||
                        record.entryPoint.size() > maximumBytes - RecordHeaderBytes - output.size() ||
                        record.bytes.size() > maximumBytes - RecordHeaderBytes - output.size() - record.entryPoint.size())
                        return Result<std::vector<std::uint8_t>>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
                    AppendInteger(output, static_cast<std::uint8_t>(record.stage));
                    AppendInteger(output, static_cast<std::uint32_t>(record.entryPoint.size()));
                    output.insert(output.end(), record.entryPoint.begin(), record.entryPoint.end());
                    AppendBytes(output, record.bytes);
                    if (output.size() > maximumBytes)
                        return Result<std::vector<std::uint8_t>>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
                }
                return Result<std::vector<std::uint8_t>>::Success(std::move(output));
            } catch (const std::bad_alloc &) {
                return Result<std::vector<std::uint8_t>>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
            }
        }

        [[nodiscard]] std::string_view StageProfile(const ShaderStage stage) noexcept {
            switch (stage) {
                case ShaderStage::Vertex:
                    return "vs_6_0";
                case ShaderStage::Fragment:
                    return "ps_6_0";
                case ShaderStage::Compute:
                    return "cs_6_0";
            }
            return {};
        }

        [[nodiscard]] std::string_view StageName(const ShaderStage stage) noexcept {
            switch (stage) {
                case ShaderStage::Vertex:
                    return "vert";
                case ShaderStage::Fragment:
                    return "frag";
                case ShaderStage::Compute:
                    return "comp";
            }
            return {};
        }

        [[nodiscard]] bool IsSpirV(const std::span<const std::uint8_t> bytes) noexcept {
            return bytes.size() >= 20U && bytes.size() % 4U == 0U && bytes[0] == 0x03U && bytes[1] == 0x02U && bytes[2] == 0x23U &&
                   bytes[3] == 0x07U && bytes[4] == 0U && bytes[5] <= 0x06U && bytes[6] == 0x01U && bytes[7] == 0U;
        }

        [[nodiscard]] bool IsDxil(const std::span<const std::uint8_t> bytes) noexcept {
            return bytes.size() >= 32U && bytes[0] == 'D' && bytes[1] == 'X' && bytes[2] == 'B' && bytes[3] == 'C';
        }

        [[nodiscard]] bool IsGlsl410(const std::span<const std::uint8_t> bytes) {
            const std::string_view text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
            return text.starts_with("#version 410") && text.find('\0') == std::string_view::npos;
        }

        [[nodiscard]] std::string SanitizeLine(std::string line, const std::filesystem::path &scratch,
                                               const std::filesystem::path &source) {
            const auto replaceAll = [&](const std::string &needle) {
                if (needle.empty())
                    return;
                std::size_t offset = 0;
                while ((offset = line.find(needle, offset)) != std::string::npos) {
                    line.replace(offset, needle.size(), "<shader>");
                    offset += 8U;
                }
            };
            replaceAll(source.string());
            replaceAll(scratch.string());
            return line;
        }

        [[nodiscard]] ShaderCompilerDiagnostic MakeToolDiagnostic(std::string message, const bool truncated,
                                                                  const std::string_view sourceIdentity, const ProcessOutputStream stream) {
            ShaderCompilerDiagnostic diagnostic;
            diagnostic.category = ShaderCompilerDiagnosticCategory::Toolchain;
            diagnostic.severity = stream == ProcessOutputStream::StandardError ? ShaderCompilerDiagnosticSeverity::Warning
                                                                               : ShaderCompilerDiagnosticSeverity::Information;
            diagnostic.message = std::move(message);
            diagnostic.truncated = truncated;
            constexpr std::string_view SourcePrefix = "<shader>:";
            if (!diagnostic.message.starts_with(SourcePrefix))
                return diagnostic;
            const char *begin = diagnostic.message.data() + SourcePrefix.size();
            const char *end = diagnostic.message.data() + diagnostic.message.size();
            std::uint32_t line = 0;
            const auto parsedLine = std::from_chars(begin, end, line);
            if (parsedLine.ec != std::errc{} || parsedLine.ptr == end || *parsedLine.ptr != ':')
                return diagnostic;
            std::uint32_t column = 0;
            const auto parsedColumn = std::from_chars(parsedLine.ptr + 1, end, column);
            if (parsedColumn.ec != std::errc{} || line == 0)
                return diagnostic;
            diagnostic.category = ShaderCompilerDiagnosticCategory::Source;
            diagnostic.sourceIdentity = sourceIdentity;
            diagnostic.line = line;
            diagnostic.column = column;
            if (diagnostic.message.find("error:") != std::string::npos)
                diagnostic.severity = ShaderCompilerDiagnosticSeverity::Error;
            return diagnostic;
        }

        class ScratchDirectory final {
        public:
            explicit ScratchDirectory(std::filesystem::path path) : path_(std::move(path)) {}

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
        IExternalProcessRunner *processes{};
        std::atomic_uint64_t nextInvocation{1};
        std::mutex processMutex;
    };

    /** @copydoc ApprovedShaderCompilerTools */
    std::span<const ApprovedShaderCompilerTool> ApprovedShaderCompilerTools() noexcept {
        return ApprovedTools;
    }

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
                                                                                IExternalProcessRunner &processes) {
        if (!IsSafeIdentity(configuration.hostPlatform) || !configuration.scratchRoot.is_absolute() ||
            configuration.processTimeout <= std::chrono::milliseconds::zero() || configuration.processTimeout > MaximumProcessTimeout ||
            configuration.maximumToolBinaryBytes == 0 || configuration.maximumToolBinaryBytes > HardMaximumToolBinaryBytes ||
            configuration.maximumProcessOutputBytes == 0 || configuration.maximumProcessOutputBytes > HardMaximumProcessOutputBytes ||
            configuration.tools.size() > 5U)
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
            const ApprovedShaderCompilerTool *approved = FindApproved(configuration.hostPlatform, tool.identity);
            if (approved == nullptr)
                return Result<ExternalShaderCompilerAdapter>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolNotApproved));
            const auto approvedExecutable = ParseLockedDigest(approved->executableSha256);
            if (!approvedExecutable.has_value() || *approvedExecutable != tool.executableDigest)
                return Result<ExternalShaderCompilerAdapter>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolDigestMismatch));
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
            state->processes = &processes;
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

        const auto lookup = [&](const ShaderCompilerTool tool) -> const ShaderCompilerToolInstallation * {
            const auto expected = std::ranges::find_if(invocation.target.tools, [tool](const ShaderCompilerToolIdentity &identity) {
                return identity.tool == tool;
            });
            if (expected == invocation.target.tools.end())
                return nullptr;
            const auto installed = std::ranges::find_if(state_->configuration.tools, [&](const ShaderCompilerToolInstallation &candidate) {
                return candidate.identity.tool == tool && candidate.identity.release == expected->release &&
                       candidate.identity.buildDigest == expected->buildDigest;
            });
            return installed == state_->configuration.tools.end() ? nullptr : &*installed;
        };

        for (const ShaderCompilerToolIdentity &required : invocation.target.tools) {
            const ShaderCompilerToolInstallation *installed = lookup(required.tool);
            if (installed == nullptr)
                return Result<ShaderCompilerAdapterOutput>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolMissing));
            auto executable = ReadBoundedFile(installed->executable, state_->configuration.maximumToolBinaryBytes,
                                              ShaderCompilerPipelineErrors::ToolMissing);
            if (executable.HasError())
                return Result<ShaderCompilerAdapterOutput>::Failure(std::move(executable).ErrorValue());
            if (ComputeSha256(std::as_bytes(std::span{executable.Value()})) != installed->executableDigest)
                return Result<ShaderCompilerAdapterOutput>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolDigestMismatch));
        }

        const std::uint64_t sequence = state_->nextInvocation.fetch_add(1, std::memory_order_relaxed);
        if (sequence == 0)
            return Result<ShaderCompilerAdapterOutput>::Failure(MakeError(ShaderCompilerPipelineErrors::ScratchIoFailed));
        std::filesystem::path scratchPath = state_->configuration.scratchRoot / ("shader-" + std::to_string(sequence));
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

        std::vector<ShaderCompilerDiagnostic> diagnostics;
        std::size_t diagnosticBytes = 0;
        const auto run = [&](const ShaderCompilerToolInstallation &tool, std::vector<std::string> arguments) -> Result<void> {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::CancellationRequested));
            ExternalProcessRequest request;
            request.executable = tool.executable.string();
            request.arguments = std::move(arguments);
            request.workingDirectory = scratch.Path();
            request.environment.base = ProcessEnvironmentBase::Replace;
            request.timeout = state_->configuration.processTimeout;
            request.maximumLineBytes =
                std::min(invocation.limits.maximumDiagnosticMessageBytes, state_->configuration.maximumProcessOutputBytes);
            request.onOutput = [&](ProcessOutputLine line) {
                if (diagnostics.size() >= invocation.limits.maximumDiagnosticsPerTarget ||
                    diagnosticBytes >= state_->configuration.maximumProcessOutputBytes)
                    return;
                line.text = SanitizeLine(std::move(line.text), scratch.Path(), sourcePath);
                const std::size_t remaining = state_->configuration.maximumProcessOutputBytes - diagnosticBytes;
                if (line.text.size() > remaining) {
                    line.text.resize(remaining);
                    line.truncated = true;
                }
                diagnosticBytes += line.text.size();
                if (!line.text.empty())
                    diagnostics.push_back(
                        MakeToolDiagnostic(std::move(line.text), line.truncated, invocation.manifest.sourceIdentity, line.stream));
            };
            std::scoped_lock processLock(state_->processMutex);
            auto result = state_->processes->Run(request, cancellation);
            if (result.HasError())
                return Result<void>::Failure(WrapError(ShaderCompilerPipelineErrors::ToolProcessFailed, std::move(result).ErrorValue()));
            if (result.Value().reason == ProcessTerminationReason::Cancelled || cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::CancellationRequested));
            if (result.Value().reason != ProcessTerminationReason::Exited || result.Value().exitCode != 0) {
                Error failure = MakeError(ShaderCompilerPipelineErrors::ToolProcessFailed);
                try {
                    for (const ShaderCompilerDiagnostic &diagnostic : diagnostics) {
                        failure.diagnostics.push_back(
                            {.code = DiagnosticCode{diagnostic.category == ShaderCompilerDiagnosticCategory::Source
                                                        ? "render.shader_compiler.source"
                                                        : "render.shader_compiler.toolchain"},
                             .severity = diagnostic.severity == ShaderCompilerDiagnosticSeverity::Error     ? DiagnosticSeverity::Error
                                         : diagnostic.severity == ShaderCompilerDiagnosticSeverity::Warning ? DiagnosticSeverity::Warning
                                                                                                            : DiagnosticSeverity::Note,
                             .message = diagnostic.message,
                             .location = {diagnostic.sourceIdentity, diagnostic.line, diagnostic.column}});
                    }
                } catch (const std::bad_alloc &) {
                    failure.diagnostics.clear();
                }
                return Result<void>::Failure(std::move(failure));
            }
            return Result<void>::Success();
        };

        if (invocation.target.requirement.backend == ShaderTargetBackend::Null) {
            std::vector<ToolArtifactRecord> records;
            try {
                records.reserve(invocation.manifest.entryPoints.size());
                for (const ShaderEntryPoint &entry : invocation.manifest.entryPoints) {
                    std::vector<std::uint8_t> bytes;
                    bytes.insert(bytes.end(), invocation.artifactKey.bytes.begin(), invocation.artifactKey.bytes.end());
                    bytes.insert(bytes.end(), entry.name.begin(), entry.name.end());
                    records.push_back({entry.stage, entry.name, std::move(bytes)});
                }
            } catch (const std::bad_alloc &) {
                return Result<ShaderCompilerAdapterOutput>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
            }
            auto payload = PackageStages(invocation.target.requirement.backend, invocation.target.requirement.payloadFormat, records,
                                         invocation.limits.maximumPayloadBytes);
            if (payload.HasError())
                return Result<ShaderCompilerAdapterOutput>::Failure(std::move(payload).ErrorValue());
            return Result<ShaderCompilerAdapterOutput>::Success({invocation.target.requirement.backend,
                                                                 invocation.target.requirement.payloadFormat,
                                                                 std::move(payload).Value(),
                                                                 {},
                                                                 std::move(diagnostics)});
        }

        const ShaderCompilerToolInstallation *dxc = lookup(ShaderCompilerTool::Dxc);
        if (dxc == nullptr)
            return Result<ShaderCompilerAdapterOutput>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolMissing));

        std::vector<ToolArtifactRecord> payloadStages;
        std::vector<ToolArtifactRecord> debugStages;
        try {
            payloadStages.reserve(invocation.manifest.entryPoints.size());
            debugStages.reserve(invocation.manifest.entryPoints.size());
        } catch (const std::bad_alloc &) {
            return Result<ShaderCompilerAdapterOutput>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
        }

        for (std::size_t index = 0; index < invocation.manifest.entryPoints.size(); ++index) {
            const ShaderEntryPoint &entry = invocation.manifest.entryPoints[index];
            const std::string stem = "stage-" + std::to_string(index);
            const std::filesystem::path nativePath = scratch.Path() / (stem + ".native");
            const std::filesystem::path spirvPath = scratch.Path() / (stem + ".spv");
            const std::filesystem::path debugPath = scratch.Path() / (stem + ".debug");
            std::vector<std::string>
                dxcArguments{"-nologo", "-HV", "2021", "-Ges", "-Zpc", "-T", std::string{StageProfile(entry.stage)}, "-E", entry.name};
            dxcArguments.push_back(invocation.target.enableFastMath ? "-ffinite-math-only" : "-Gis");
            dxcArguments.push_back(invocation.target.optimization == ShaderOptimizationLevel::Disabled ? "-Od"
                                   : invocation.target.optimization == ShaderOptimizationLevel::Size   ? "-O1"
                                                                                                       : "-O3");
            for (const ShaderCompilerDefine &define : invocation.defines) {
                dxcArguments.emplace_back("-D");
                dxcArguments.push_back(define.name + "=" + define.value);
            }

            const bool spirvRoute = invocation.target.requirement.backend != ShaderTargetBackend::D3D12;
            if (spirvRoute) {
                dxcArguments.insert(dxcArguments.end(), {"-spirv", "-fspv-target-env=vulkan1.3", "-fvk-use-gl-layout"});
                dxcArguments.insert(dxcArguments.end(), {"-Fo", spirvPath.string(), sourcePath.string()});
            } else {
                dxcArguments.insert(dxcArguments.end(), {"-Fo", nativePath.string(), sourcePath.string()});
            }
            if (auto compiled = run(*dxc, std::move(dxcArguments)); compiled.HasError())
                return Result<ShaderCompilerAdapterOutput>::Failure(std::move(compiled).ErrorValue());

            if (spirvRoute) {
                const ShaderCompilerToolInstallation *validator = lookup(ShaderCompilerTool::SpirvTools);
                if (validator == nullptr)
                    return Result<ShaderCompilerAdapterOutput>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolMissing));
                if (auto validated = run(*validator, {"--target-env", "vulkan1.3", spirvPath.string()}); validated.HasError())
                    return Result<ShaderCompilerAdapterOutput>::Failure(std::move(validated).ErrorValue());
                auto spirv =
                    ReadBoundedFile(spirvPath, invocation.limits.maximumPayloadBytes, ShaderCompilerPipelineErrors::ToolOutputInvalid);
                if (spirv.HasError())
                    return Result<ShaderCompilerAdapterOutput>::Failure(std::move(spirv).ErrorValue());
                if (!IsSpirV(spirv.Value()))
                    return Result<ShaderCompilerAdapterOutput>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));

                if (invocation.target.requirement.backend == ShaderTargetBackend::Vulkan) {
                    payloadStages.push_back({entry.stage, entry.name, std::move(spirv).Value()});
                } else {
                    const ShaderCompilerToolInstallation *translator = lookup(ShaderCompilerTool::SpirvCross);
                    if (translator == nullptr)
                        return Result<ShaderCompilerAdapterOutput>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolMissing));
                    std::vector<std::string> crossArguments{spirvPath.string(),
                                                            "--output",
                                                            nativePath.string(),
                                                            "--entry",
                                                            entry.name,
                                                            "--stage",
                                                            std::string{StageName(entry.stage)}};
                    if (invocation.target.requirement.backend == ShaderTargetBackend::OpenGL)
                        crossArguments.insert(crossArguments.end(), {"--version", "410", "--no-es", "--no-420pack-extension"});
                    else
                        crossArguments.insert(crossArguments.end(), {"--msl", "--msl-version", "20400"});
                    if (auto translated = run(*translator, std::move(crossArguments)); translated.HasError())
                        return Result<ShaderCompilerAdapterOutput>::Failure(std::move(translated).ErrorValue());
                    auto native =
                        ReadBoundedFile(nativePath, invocation.limits.maximumPayloadBytes, ShaderCompilerPipelineErrors::ToolOutputInvalid);
                    if (native.HasError())
                        return Result<ShaderCompilerAdapterOutput>::Failure(std::move(native).ErrorValue());
                    if (invocation.target.requirement.backend == ShaderTargetBackend::OpenGL) {
                        if (!IsGlsl410(native.Value()))
                            return Result<ShaderCompilerAdapterOutput>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
                        payloadStages.push_back({entry.stage, entry.name, std::move(native).Value()});
                    } else {
                        const ShaderCompilerToolInstallation *metal = lookup(ShaderCompilerTool::AppleMetal);
                        if (metal == nullptr)
                            return Result<ShaderCompilerAdapterOutput>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolMissing));
                        const std::filesystem::path airPath = scratch.Path() / (stem + ".air");
                        if (auto metalCompile = run(*metal, {"-sdk", "macosx", "metal", "-std=macos-metal2.4", "-mmacosx-version-min=14.0",
                                                             "-c", nativePath.string(), "-o", airPath.string()});
                            metalCompile.HasError())
                            return Result<ShaderCompilerAdapterOutput>::Failure(std::move(metalCompile).ErrorValue());
                        const std::filesystem::path libraryPath = scratch.Path() / (stem + ".metallib");
                        if (auto linked = run(*metal, {"-sdk", "macosx", "metallib", airPath.string(), "-o", libraryPath.string()});
                            linked.HasError())
                            return Result<ShaderCompilerAdapterOutput>::Failure(std::move(linked).ErrorValue());
                        auto library = ReadBoundedFile(libraryPath, invocation.limits.maximumPayloadBytes,
                                                       ShaderCompilerPipelineErrors::ToolOutputInvalid);
                        if (library.HasError())
                            return Result<ShaderCompilerAdapterOutput>::Failure(std::move(library).ErrorValue());
                        if (library.Value().size() < 4U || library.Value()[0] != 'M' || library.Value()[1] != 'T' ||
                            library.Value()[2] != 'L' || library.Value()[3] != 'B')
                            return Result<ShaderCompilerAdapterOutput>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
                        payloadStages.push_back({entry.stage, entry.name, std::move(library).Value()});
                    }
                }

                if (invocation.target.emitDebugInformation) {
                    std::vector<std::string> debugArguments{"-nologo",
                                                            "-HV",
                                                            "2021",
                                                            "-Ges",
                                                            "-Zpc",
                                                            "-T",
                                                            std::string{StageProfile(entry.stage)},
                                                            "-E",
                                                            entry.name,
                                                            invocation.target.enableFastMath ? "-ffinite-math-only" : "-Gis",
                                                            "-Od",
                                                            "-Zi",
                                                            "-Qembed_debug",
                                                            "-spirv",
                                                            "-fspv-target-env=vulkan1.3",
                                                            "-fvk-use-gl-layout"};
                    for (const ShaderCompilerDefine &define : invocation.defines) {
                        debugArguments.emplace_back("-D");
                        debugArguments.push_back(define.name + "=" + define.value);
                    }
                    debugArguments.insert(debugArguments.end(), {"-Fo", debugPath.string(), sourcePath.string()});
                    if (auto debugCompiled = run(*dxc, std::move(debugArguments)); debugCompiled.HasError())
                        return Result<ShaderCompilerAdapterOutput>::Failure(std::move(debugCompiled).ErrorValue());
                    auto debug = ReadBoundedFile(debugPath, invocation.limits.maximumDebugPayloadBytes,
                                                 ShaderCompilerPipelineErrors::ToolOutputInvalid);
                    if (debug.HasError())
                        return Result<ShaderCompilerAdapterOutput>::Failure(std::move(debug).ErrorValue());
                    if (!IsSpirV(debug.Value()))
                        return Result<ShaderCompilerAdapterOutput>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
                    debugStages.push_back({entry.stage, entry.name, std::move(debug).Value()});
                }
            } else {
                const ShaderCompilerToolInstallation *validator = lookup(ShaderCompilerTool::DxilValidator);
                if (validator == nullptr)
                    return Result<ShaderCompilerAdapterOutput>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolMissing));
                if (auto validated = run(*validator, {nativePath.string()}); validated.HasError())
                    return Result<ShaderCompilerAdapterOutput>::Failure(std::move(validated).ErrorValue());
                auto native =
                    ReadBoundedFile(nativePath, invocation.limits.maximumPayloadBytes, ShaderCompilerPipelineErrors::ToolOutputInvalid);
                if (native.HasError())
                    return Result<ShaderCompilerAdapterOutput>::Failure(std::move(native).ErrorValue());
                if (!IsDxil(native.Value()))
                    return Result<ShaderCompilerAdapterOutput>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
                payloadStages.push_back({entry.stage, entry.name, std::move(native).Value()});
                if (invocation.target.emitDebugInformation) {
                    const std::filesystem::path debugDxilPath = scratch.Path() / (stem + ".debug.dxil");
                    const std::filesystem::path pdbPath = scratch.Path() / (stem + ".pdb");
                    std::vector<std::string> debugArguments{"-nologo",
                                                            "-HV",
                                                            "2021",
                                                            "-Ges",
                                                            "-Zpc",
                                                            "-T",
                                                            std::string{StageProfile(entry.stage)},
                                                            "-E",
                                                            entry.name,
                                                            invocation.target.enableFastMath ? "-ffinite-math-only" : "-Gis",
                                                            "-Od",
                                                            "-Zi"};
                    for (const ShaderCompilerDefine &define : invocation.defines) {
                        debugArguments.emplace_back("-D");
                        debugArguments.push_back(define.name + "=" + define.value);
                    }
                    debugArguments.insert(debugArguments.end(),
                                          {"-Fd", pdbPath.string(), "-Fo", debugDxilPath.string(), sourcePath.string()});
                    if (auto debugCompiled = run(*dxc, std::move(debugArguments)); debugCompiled.HasError())
                        return Result<ShaderCompilerAdapterOutput>::Failure(std::move(debugCompiled).ErrorValue());
                    auto pdb = ReadBoundedFile(pdbPath, invocation.limits.maximumDebugPayloadBytes,
                                               ShaderCompilerPipelineErrors::ToolOutputInvalid);
                    if (pdb.HasError())
                        return Result<ShaderCompilerAdapterOutput>::Failure(std::move(pdb).ErrorValue());
                    debugStages.push_back({entry.stage, entry.name, std::move(pdb).Value()});
                }
            }
        }

        auto payload = PackageStages(invocation.target.requirement.backend, invocation.target.requirement.payloadFormat, payloadStages,
                                     invocation.limits.maximumPayloadBytes);
        if (payload.HasError())
            return Result<ShaderCompilerAdapterOutput>::Failure(std::move(payload).ErrorValue());
        std::vector<std::uint8_t> debugPayload;
        if (!debugStages.empty()) {
            auto packagedDebug = PackageStages(invocation.target.requirement.backend, invocation.target.requirement.payloadFormat,
                                               debugStages, invocation.limits.maximumDebugPayloadBytes);
            if (packagedDebug.HasError())
                return Result<ShaderCompilerAdapterOutput>::Failure(std::move(packagedDebug).ErrorValue());
            debugPayload = std::move(packagedDebug).Value();
        }
        return Result<ShaderCompilerAdapterOutput>::Success({invocation.target.requirement.backend,
                                                             invocation.target.requirement.payloadFormat, std::move(payload).Value(),
                                                             std::move(debugPayload), std::move(diagnostics)});
    }
}  // namespace Horo::Render
