#include "OpenXRPackageProbe.h"

#include "Horo/Foundation/Platform.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Platform/ExternalProcess.h"
#include "OpenXRPackageProbeWire.h"
#include "cli_host/CliHostInterrupts.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <ranges>
#include <string_view>

namespace Horo::Application::Internal {
    namespace {
        enum Argument : std::size_t {
            Host,
            Route,
            Runtime,
            System,
            InstallDigest,
            Executable,
            ExecutableDigest,
            Installation,
            Project,
            Roots,
            Catalog,
            CatalogSignature,
            Count
        };

        constexpr std::size_t MaximumReceiptBytes = 512;
        constexpr std::string_view Usage =
            "Usage: horo-engine --probe-xr-package <windows|linux> system-default <runtime-name> <hmd-system-name> "
            "<install-sha256> <editor-executable> <editor-sha256> <installation-root> <project> <trust-roots> <catalog> "
            "<catalog-signature>\n";

        /** @brief Requires an exact bounded readable file underneath the cooperating product lease. */
        bool MatchesExecutable(const std::filesystem::path &path, const Sha256Digest &expected) {
            constexpr std::uintmax_t MaximumExecutableBytes = 512U * 1024U * 1024U;
            std::error_code error;
            if (!std::filesystem::is_regular_file(path, error) || error)
                return false;
            const auto size = std::filesystem::file_size(path, error);
            if (error || size == 0 || size > MaximumExecutableBytes)
                return false;
            std::ifstream file(path, std::ios::binary);
            if (!file)
                return false;
            Sha256Builder digest;
            std::array<char, 64U * 1024U> buffer;
            std::uintmax_t remaining = size;
            while (remaining > 0) {
                const auto requested = static_cast<std::size_t>(std::min<std::uintmax_t>(remaining, buffer.size()));
                if (!file.read(buffer.data(), static_cast<std::streamsize>(requested)) ||
                    !digest.Update(std::as_bytes(std::span{buffer}.first(requested))))
                    return false;
                remaining -= requested;
            }
            return file.peek() == std::char_traits<char>::eof() && !file.bad() && digest.Finalize() == expected;
        }

        enum class ReceiptKind : std::uint8_t {
            Invalid,
            Success,
            Failure
        };

        /** @brief Validates one process-owned record and rejects any extra strings or keys. */
        ReceiptKind ReadReceipt(const std::string_view line, const std::string_view digest, const std::string_view host,
                                const std::string_view target) {
            constexpr std::size_t IdentityColumns = 6;
            constexpr std::array<std::uint32_t, 9> maxima{65'535, 65'535, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, 16, 128, 7};
            std::array<std::string_view, IdentityColumns + maxima.size()> columns{};
            std::size_t count{};
            for (auto word : std::views::split(line, ' ')) {
                if (count == columns.size() || word.empty())
                    return ReceiptKind::Invalid;
                columns[count++] = std::string_view{word.begin(), word.end()};
            }
            if (count < IdentityColumns || columns[1] != digest || columns[2] != host || columns[3] != OpenXRProbeWire::Route ||
                columns[4] != OpenXRProbeWire::Renderer || columns[5] != target)
                return ReceiptKind::Invalid;
            if (columns[0] == OpenXRProbeWire::FailureRecord) {
                const auto &reasons = OpenXRProbeWire::FailureNames;
                return count == IdentityColumns + 2 && columns[6] == OpenXRProbeWire::LocalConnection &&
                               std::ranges::find(reasons, columns[7]) != reasons.end()
                           ? ReceiptKind::Failure
                           : ReceiptKind::Invalid;
            }
            if (count != columns.size() || columns[0] != OpenXRProbeWire::Record)
                return ReceiptKind::Invalid;
            for (std::size_t index = 0; index < maxima.size(); ++index) {
                const auto field = columns[index + IdentityColumns];
                std::uint32_t value{};
                const auto parsed = std::from_chars(field.data(), field.data() + field.size(), value);
                if (parsed.ec != std::errc{} || parsed.ptr != field.data() + field.size() || value > maxima[index])
                    return ReceiptKind::Invalid;
            }
            return ReceiptKind::Success;
        }

        /** @brief Converts the terminal host's UTF-8 argv to a native path without losing non-ASCII bytes. */
        std::filesystem::path NativePath(std::string_view utf8) {
            return std::filesystem::path{std::u8string{utf8.begin(), utf8.end()}};
        }

        /** @brief Admits only absolute canonical paths with no redirects before launching the installed child. */
        bool ExactPath(const std::filesystem::path &path) {
            std::error_code error;
            return path.is_absolute() && std::filesystem::canonical(path, error) == path && !error;
        }

        /** @brief Checks that a resolved executable resides strictly inside the retained installation. */
        bool Contained(const std::filesystem::path &exe, const std::filesystem::path &installation) {
            const auto relative = exe.lexically_relative(installation);
            return !relative.empty() && relative != "." && !relative.is_absolute() && *relative.begin() != "..";
        }

        /** @brief Requests an exact fixed child mode. Only signed project and installed renderer flags are forwarded. */
        ExternalProcessRequest MakeRequest(const std::span<char *> arguments, const std::filesystem::path &installation) {
            ExternalProcessRequest request;
            request.executable = arguments[Executable];
            request.workingDirectory = installation;
            request.arguments = {"--renderer",
                                 std::string{OpenXRProbeWire::Renderer},
                                 "--project",
                                 arguments[Project],
                                 "--xr-host-roots",
                                 arguments[Roots],
                                 "--xr-host-catalog",
                                 arguments[Catalog],
                                 "--xr-host-catalog-signature",
                                 arguments[CatalogSignature],
                                 std::string{OpenXRProbeWire::Command},
                                 arguments[Host],
                                 arguments[Route],
                                 arguments[Runtime],
                                 arguments[System],
                                 arguments[InstallDigest]};
            request.environment.unset = {"XR_RUNTIME_JSON", "XR_ENABLE_API_LAYERS", "XR_API_LAYER_PATH", "XR_LOADER_DEBUG"};
            request.timeout = std::chrono::seconds{90};
            request.maximumLineBytes = MaximumReceiptBytes;
            request.maximumOutputBytes = 128U * 1024U;
            return request;
        }

        /** @brief Requires one current native receipt while the caller retains the installed-product lease. */
        int ProbeInstalledRuntime(const std::span<char *> arguments, const std::filesystem::path &installation,
                                  const std::filesystem::path &executable, const Sha256Digest &executableDigest, std::ostream &output,
                                  std::ostream &errors) {
            const std::string_view host{arguments[Host]};
            const std::string_view digest{arguments[InstallDigest]};
            Cli::CliInvocationStopController stop{std::chrono::milliseconds::zero()};
            NativeCliInterrupts interrupts;
            if (interrupts.Start(stop).HasError()) {
                errors << "XR probe could not acquire native cancellation handling.\n";
                return 1;
            }
            auto request = MakeRequest(arguments, installation);
            request.forceCancellation = stop.ForceToken();
            const auto target = FormatSha256(OpenXRProbeWire::TargetDigest(arguments[Runtime], arguments[System]));
            std::array<char, MaximumReceiptBytes> receipt{};
            std::size_t received{};
            bool rejected{};
            ReceiptKind kind{};
            request.onOutput = [&](const ProcessOutputLine &line) noexcept {
                if (line.stream != ProcessOutputStream::StandardOutput ||
                    (!line.text.starts_with(OpenXRProbeWire::Record) && !line.text.starts_with(OpenXRProbeWire::FailureRecord)))
                    return;
                if (received || line.truncated || line.text.size() > receipt.size()) {
                    rejected = true;
                    return;
                }
                kind = ReadReceipt(line.text, digest, host, target);
                if (kind == ReceiptKind::Invalid) {
                    rejected = true;
                    return;
                }
                std::ranges::copy(line.text, receipt.begin());
                received = line.text.size();
            };
            NativeExternalProcessRunner runner;
            const auto terminal = runner.Run(request, stop.Token());
            if (terminal.HasError()) {
                errors << "XR probe launch or process cleanup failed.\n";
                return 1;
            }
            if (terminal.Value().stopCause == ProcessStopCause::Timeout) {
                errors << "XR probe timed out; the supervised process tree was retired.\n";
                return 1;
            }
            if (terminal.Value().reason != ProcessTerminationReason::Exited || terminal.Value().stopCause != ProcessStopCause::None ||
                stop.Token().IsCancellationRequested()) {
                errors << "XR probe was interrupted or required process cleanup.\n";
                return 1;
            }
            const bool finishedSuccess = kind == ReceiptKind::Success && terminal.Value().exitCode == 0;
            const bool finishedFailure = kind == ReceiptKind::Failure && terminal.Value().exitCode == 1;
            if (rejected || (!finishedSuccess && !finishedFailure) || !MatchesExecutable(executable, executableDigest)) {
                errors << "XR probe reported no current installed runtime/device receipt. Check the requested default runtime, HMD and "
                          "package.\n";
                return 1;
            }
            if (finishedFailure) {
                errors << std::string_view{receipt.data(), received} << '\n';
                return 1;
            }
            output << std::string_view{receipt.data(), received} << '\n';
            return output ? 0 : 1;
        }

    }  // namespace

    /** @copydoc RunOpenXRPackageProbe */
    int RunOpenXRPackageProbe(const std::span<char *> arguments, std::ostream &output, std::ostream &errors) {
        if (arguments.size() != Count || std::ranges::any_of(arguments, [](const char *value) {
            return value == nullptr;
        })) {
            errors << Usage;
            return 2;
        }
        const std::string_view host{arguments[Host]};
        if (host.empty() || host != OpenXRProbeWire::DesktopHost() || std::string_view{arguments[Route]} != OpenXRProbeWire::Route) {
            errors << "XR probe unavailable for the declared host or connection route.\n";
            return 2;
        }
        const std::string_view digest{arguments[InstallDigest]};
        const auto executableDigest = ParseSha256(arguments[ExecutableDigest]);
        if (!OpenXRProbeWire::ValidTargetLabel(arguments[Runtime]) || !OpenXRProbeWire::ValidTargetLabel(arguments[System]) ||
            ParseSha256(digest).HasError() || executableDigest.HasError()) {
            errors << Usage;
            return 2;
        }

        std::filesystem::path executable;
        std::filesystem::path installation;
        try {
            executable = NativePath(arguments[Executable]);
            installation = NativePath(arguments[Installation]);
            if (!ExactPath(installation) || !ExactPath(executable) || !Contained(executable, installation) ||
                std::ranges::any_of(arguments.subspan(Project), [](const char *value) {
                return !ExactPath(NativePath(value));
            })) {
                errors << "XR probe paths must resolve exactly to the selected installed package and project/trust inputs.\n";
                return 2;
            }
        } catch (const std::filesystem::filesystem_error &) {
            errors << "XR probe paths use an invalid native encoding.\n";
            return 2;
        }
        NativeDurableFileSystem files;
        const auto lease = files.TryAcquireProductLaunch(installation);
        if (lease.HasError() || !MatchesExecutable(executable, executableDigest.Value())) {
            errors << "XR probe installation is stale, busy, or differs from its expected executable digest.\n";
            return 1;
        }

        return ProbeInstalledRuntime(arguments, installation, executable, executableDigest.Value(), output, errors);
    }
}  // namespace Horo::Application::Internal
