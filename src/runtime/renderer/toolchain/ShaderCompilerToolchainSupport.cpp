#include "ShaderCompilerToolchainSupport.h"

#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"

#include <charconv>
#include <fstream>
#include <limits>

namespace Horo::Render::ShaderCompilerToolchainDetail {
    Result<std::vector<std::uint8_t>> ReadBoundedFile(const std::filesystem::path &path, const std::size_t maximumBytes,
                                                      const ErrorCodeDescriptor &failure) {
        std::error_code error;
        if (const auto status = std::filesystem::symlink_status(path, error); error || !std::filesystem::is_regular_file(status))
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(failure));
        const std::uintmax_t size = std::filesystem::file_size(path, error);
        if (error || size == 0 || size > maximumBytes || size > std::numeric_limits<std::size_t>::max())
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(failure));
        try {
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
            if (std::ifstream input(path, std::ios::binary);
                !input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())) ||
                input.peek() != std::char_traits<char>::eof())
                return Result<std::vector<std::uint8_t>>::Failure(MakeError(failure));
            return Result<std::vector<std::uint8_t>>::Success(std::move(bytes));
        } catch (const std::bad_alloc &) {
            return Result<std::vector<std::uint8_t>>::Failure(MakeError(failure));
        }
    }

    Result<void> WriteFile(const std::filesystem::path &path, const std::span<const std::uint8_t> bytes) {
        if (std::ofstream output(path, std::ios::binary | std::ios::trunc);
            !output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())) || !output.flush())
            return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ScratchIoFailed));
        return Result<void>::Success();
    }

    Result<std::vector<std::uint8_t>> PackageStages(const ShaderTargetBackend backend, const ShaderPayloadFormat format,
                                                    const std::vector<ToolArtifactRecord> &stages, const std::size_t maximumBytes) {
        return PackageShaderArtifactStages(backend, format, stages, maximumBytes);
    }

    std::string_view StageProfile(const ShaderStage stage) noexcept {
        using enum ShaderStage;
        switch (stage) {
            case Vertex:
                return "vs_6_0";
            case Fragment:
                return "ps_6_0";
            case Compute:
                return "cs_6_0";
        }
        return {};
    }

    std::string_view StageName(const ShaderStage stage) noexcept {
        using enum ShaderStage;
        switch (stage) {
            case Vertex:
                return "vert";
            case Fragment:
                return "frag";
            case Compute:
                return "comp";
        }
        return {};
    }

    bool IsSpirV(const std::span<const std::uint8_t> bytes) noexcept {
        return bytes.size() >= 20U && bytes.size() % 4U == 0U && bytes[0] == 0x03U && bytes[1] == 0x02U && bytes[2] == 0x23U &&
               bytes[3] == 0x07U && bytes[4] == 0U && bytes[5] <= 0x06U && bytes[6] == 0x01U && bytes[7] == 0U;
    }

    bool IsDxil(const std::span<const std::uint8_t> bytes) noexcept {
        return bytes.size() >= 32U && bytes[0] == 'D' && bytes[1] == 'X' && bytes[2] == 'B' && bytes[3] == 'C';
    }

    bool IsGlsl410(const std::span<const std::uint8_t> bytes) {
        const std::string_view text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        return text.starts_with("#version 410") && text.find('\0') == std::string_view::npos;
    }

    std::string SanitizeLine(std::string line, const std::filesystem::path &scratch, const std::filesystem::path &source) {
        const auto replaceAll = [&](const std::string_view needle) {
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

    ShaderCompilerDiagnostic MakeToolDiagnostic(std::string message, const bool truncated, const std::string_view sourceIdentity,
                                                const ProcessOutputStream stream) {
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
        if (const auto parsedColumn = std::from_chars(parsedLine.ptr + 1, end, column); parsedColumn.ec != std::errc{} || line == 0)
            return diagnostic;
        diagnostic.category = ShaderCompilerDiagnosticCategory::Source;
        diagnostic.sourceIdentity = sourceIdentity;
        diagnostic.line = line;
        diagnostic.column = column;
        if (diagnostic.message.find("error:") != std::string::npos)
            diagnostic.severity = ShaderCompilerDiagnosticSeverity::Error;
        return diagnostic;
    }
}  // namespace Horo::Render::ShaderCompilerToolchainDetail
