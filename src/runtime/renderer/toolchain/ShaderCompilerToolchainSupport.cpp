#include "ShaderCompilerToolchainSupport.h"

#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <limits>
#include <ranges>
#include <type_traits>
#include <utility>

namespace Horo::Render::ShaderCompilerToolchainDetail {
    namespace {
        constexpr std::array<std::uint8_t, 8> PackageMagic{'H', 'O', 'R', 'O', 'S', 'H', 'D', 'R'};
        constexpr std::uint32_t PackageVersion = 1;
        constexpr std::size_t RecordHeaderBytes = sizeof(std::uint8_t) + sizeof(std::uint32_t) + sizeof(std::uint64_t);

        template <typename ValueT> void AppendInteger(std::vector<std::uint8_t> &output, ValueT value) {
            using UnsignedT = std::make_unsigned_t<ValueT>;
            auto bits = static_cast<UnsignedT>(value);
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
    }  // namespace

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
        try {
            std::vector<std::uint8_t> output(PackageMagic.begin(), PackageMagic.end());
            AppendInteger(output, PackageVersion);
            AppendInteger(output, static_cast<std::uint8_t>(backend));
            AppendInteger(output, static_cast<std::uint8_t>(format));
            AppendInteger(output, static_cast<std::uint32_t>(stages.size()));
            for (const ToolArtifactRecord &record : stages) {
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
        // Keep native codes separate from severity and the stable Horo category.
        for (const auto &[word, severity] : {std::pair{std::string_view{"error"}, ShaderCompilerDiagnosticSeverity::Error},
                                             std::pair{std::string_view{"warning"}, ShaderCompilerDiagnosticSeverity::Warning},
                                             std::pair{std::string_view{"note"}, ShaderCompilerDiagnosticSeverity::Information}}) {
            const auto offset = diagnostic.message.find(word);
            if (offset == std::string::npos || (offset != 0 && diagnostic.message[offset - 1] != ' '))
                continue;
            const std::size_t start = offset + word.size();
            if (start >= diagnostic.message.size())
                continue;
            if (diagnostic.message[start] == ':') {
                diagnostic.severity = severity;
                break;
            }
            if (diagnostic.message[start] != ' ')
                continue;
            const auto colon = diagnostic.message.find(':', start + 1);
            if (colon == std::string::npos || colon == start + 1 || colon - start > 128U)
                continue;
            const auto code = diagnostic.message.substr(start + 1, colon - start - 1);
            if (!std::ranges::all_of(code, [](const unsigned char character) {
                return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
                       (character >= '0' && character <= '9') || character == '_' || character == '-';
            }))
                continue;
            diagnostic.toolCode = code;
            diagnostic.severity = severity;
            break;
        }
        constexpr std::string_view SourcePrefix = "<shader>:";
        const std::string logicalPrefix = std::string{sourceIdentity} + ":";
        const std::size_t prefixBytes = diagnostic.message.starts_with(SourcePrefix)    ? SourcePrefix.size()
                                        : diagnostic.message.starts_with(logicalPrefix) ? logicalPrefix.size()
                                                                                        : 0U;
        if (prefixBytes == 0U)
            return diagnostic;
        const char *begin = diagnostic.message.data() + prefixBytes;
        const char *end = diagnostic.message.data() + diagnostic.message.size();
        std::uint32_t line = 0;
        const auto parsedLine = std::from_chars(begin, end, line);
        if (parsedLine.ec != std::errc{} || parsedLine.ptr == end || *parsedLine.ptr != ':')
            return diagnostic;
        std::uint32_t column = 0;
        if (const auto parsedColumn = std::from_chars(parsedLine.ptr + 1, end, column);
            parsedColumn.ec != std::errc{} || parsedColumn.ptr == end || *parsedColumn.ptr != ':' || line == 0)
            return diagnostic;
        diagnostic.category = ShaderCompilerDiagnosticCategory::Source;
        diagnostic.sourceIdentity = sourceIdentity;
        diagnostic.line = line;
        diagnostic.column = column;
        return diagnostic;
    }
}  // namespace Horo::Render::ShaderCompilerToolchainDetail
