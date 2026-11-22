#include "OpenXRPackageProbe.h"

#include "Horo/Foundation/Platform.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Platform/ExternalProcess.h"
#include "OpenXRPackageProbeWire.h"
#include "cli_host/CliHostInterrupts.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ostream>
#include <ranges>
#include <string>
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
        constexpr std::size_t ReceiptIdentityColumns = 6;
        constexpr std::size_t ReceiptCompatibilityColumns = 9;
        constexpr std::string_view Usage =
            "Usage: horo-engine --probe-xr-package <windows|linux> system-default <runtime-name> <hmd-system-name> "
            "<install-sha256> <editor-executable> <editor-sha256> <installation-root> <project> <trust-roots> <catalog> "
            "<catalog-signature> [--connection desktop-local [--timeout-seconds 1..90]]\n";

        struct ProbeOptions final {
            std::chrono::seconds timeout{90};
            bool qualifiedReport{};
        };

        /** @brief Reads the bounded explicit connection and deadline without forwarding arbitrary child arguments. */
        std::optional<ProbeOptions> ReadOptions(const std::span<char *> arguments) {
            ProbeOptions result;
            if (arguments.empty())
                return result;
            if ((arguments.size() != 2 && arguments.size() != 4) || std::string_view{arguments[0]} != "--connection" ||
                std::string_view{arguments[1]} != OpenXRProbeWire::LocalConnection)
                return std::nullopt;
            result.qualifiedReport = true;
            if (arguments.size() == 2)
                return result;
            const std::string_view value{arguments[3]};
            unsigned seconds{};
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds);
            if (std::string_view{arguments[2]} != "--timeout-seconds" || parsed.ec != std::errc{} ||
                parsed.ptr != value.data() + value.size() || seconds == 0 || seconds > 90)
                return std::nullopt;
            result.timeout = std::chrono::seconds{seconds};
            return result;
        }

        struct ProbeIdentity final {
            std::string_view install;
            std::string_view executable;
            std::string_view host;
            std::string target;
        };

        /** @brief Reports the exact package and target without disclosing child output or device labels. */
        void WriteQualifiedReport(std::ostream &destination, const ProbeIdentity &identity, std::string_view outcome,
                                  std::string_view facts = {}) {
            destination << "horo-xr-desktop-qualification-v1 " << identity.install << ' ' << identity.executable << ' ' << identity.host
                        << ' ' << OpenXRProbeWire::Route << ' ' << OpenXRProbeWire::Renderer << ' ' << identity.target << ' '
                        << OpenXRProbeWire::LocalConnection << ' ' << outcome;
            if (!facts.empty())
                destination << ' ' << facts;
            destination << '\n';
        }

        int FailProbe(std::ostream &errors, const ProbeOptions &options, const ProbeIdentity &identity, std::string_view reason,
                      std::string_view message) {
            if (options.qualifiedReport)
                WriteQualifiedReport(errors, identity, reason);
            else
                errors << message << '\n';
            return 1;
        }

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

        /** @brief Validates a successful row against the packaged projection profile's bounded evidence. */
        bool ValidCompatibilityFacts(const std::span<const std::string_view, ReceiptCompatibilityColumns> fields) noexcept {
            constexpr std::uint32_t maximumRoutes = (1U << OpenXRProbeWire::PackagedRouteCount) - 1U;
            constexpr std::array<std::uint32_t, ReceiptCompatibilityColumns> maxima{65'535,     65'535,     UINT32_MAX,
                                                                                    UINT32_MAX, UINT32_MAX, UINT32_MAX,
                                                                                    16,         128,        maximumRoutes};
            std::array<std::uint32_t, maxima.size()> values{};
            for (std::size_t index = 0; index < maxima.size(); ++index) {
                const auto field = fields[index];
                auto &value = values[index];
                const auto parsed = std::from_chars(field.data(), field.data() + field.size(), value);
                if (parsed.ec != std::errc{} || parsed.ptr != field.data() + field.size() || value > maxima[index])
                    return false;
            }
            const auto [runtimeMajor, runtimeMinor, runtimePatch, graphicsMajor, graphicsMinor, graphicsPatch, spaces, formats, routes] =
                values;
            constexpr auto required = OpenXRProbeWire::RequiredRouteMask;
            return runtimeMajor != 0 && graphicsMajor != 0 && spaces >= 2 && formats != 0 && (routes & required) == required;
        }

        /** @brief Validates one process-owned record and rejects any extra strings or keys. */
        ReceiptKind ReadReceipt(const std::string_view line, const std::string_view digest, const std::string_view host,
                                const std::string_view target) {
            std::array<std::string_view, ReceiptIdentityColumns + ReceiptCompatibilityColumns> columns{};
            std::size_t count{};
            for (auto word : std::views::split(line, ' ')) {
                if (count == columns.size() || word.empty())
                    return ReceiptKind::Invalid;
                columns[count++] = std::string_view{word.begin(), word.end()};
            }
            if (count < ReceiptIdentityColumns || columns[1] != digest || columns[2] != host || columns[3] != OpenXRProbeWire::Route ||
                columns[4] != OpenXRProbeWire::Renderer || columns[5] != target)
                return ReceiptKind::Invalid;
            if (columns[0] == OpenXRProbeWire::FailureRecord) {
                const auto &reasons = OpenXRProbeWire::FailureNames;
                return count == ReceiptIdentityColumns + 2 && columns[6] == OpenXRProbeWire::LocalConnection &&
                               std::ranges::find(reasons, columns[7]) != reasons.end()
                           ? ReceiptKind::Failure
                           : ReceiptKind::Invalid;
            }
            if (count != columns.size() || columns[0] != OpenXRProbeWire::Record)
                return ReceiptKind::Invalid;
            return ValidCompatibilityFacts(std::span{columns}.last<ReceiptCompatibilityColumns>()) ? ReceiptKind::Success
                                                                                                   : ReceiptKind::Invalid;
        }

        /** @brief Retains at most one well-formed receipt from the supervised child's standard output. */
        struct ProbeReceipt final {
            /** @brief Ignores ordinary output and records malformed or repeated receipts as a permanent failure. */
            void Observe(const ProcessOutputLine &line, std::string_view digest, std::string_view host, std::string_view target) noexcept {
                if (line.stream != ProcessOutputStream::StandardOutput ||
                    (!line.text.starts_with(OpenXRProbeWire::Record) && !line.text.starts_with(OpenXRProbeWire::FailureRecord)))
                    return;
                if (size || line.truncated || line.text.size() > bytes.size()) {
                    rejected = true;
                    return;
                }
                kind = ReadReceipt(line.text, digest, host, target);
                if (kind == ReceiptKind::Invalid) {
                    rejected = true;
                    return;
                }
                std::ranges::copy(line.text, bytes.begin());
                size = line.text.size();
            }

            /** @brief Borrows the exact accepted output line while the receipt is alive. */
            [[nodiscard]] std::string_view Text() const noexcept {
                return {bytes.data(), size};
            }

            /** @brief Projects only the validated fields after the child's fixed identity columns. */
            [[nodiscard]] std::string_view Facts() const noexcept {
                auto facts = Text();
                for (unsigned index = 0; index < ReceiptIdentityColumns; ++index)
                    facts.remove_prefix(facts.find(' ') + 1);
                if (kind == ReceiptKind::Failure)
                    facts.remove_prefix(OpenXRProbeWire::LocalConnection.size() + 1);
                return facts;
            }

            std::array<char, MaximumReceiptBytes> bytes{};
            std::size_t size{};
            bool rejected{};
            ReceiptKind kind{};
        };

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
        ExternalProcessRequest MakeRequest(const std::span<char *> arguments, const std::filesystem::path &installation,
                                           std::chrono::seconds timeout) {
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
            request.timeout = timeout;
            request.maximumLineBytes = MaximumReceiptBytes;
            request.maximumOutputBytes = 128U * 1024U;
            return request;
        }

        /** @brief Requires one current native receipt while the caller retains the installed-product lease. */
        int ProbeInstalledRuntime(const std::span<char *> arguments, const std::filesystem::path &installation,
                                  const std::filesystem::path &executable, const Sha256Digest &executableDigest,
                                  const ProbeIdentity &identity, const ProbeOptions &options, std::ostream &output, std::ostream &errors) {
            Cli::CliInvocationStopController stop{std::chrono::milliseconds::zero()};
            NativeCliInterrupts interrupts;
            if (interrupts.Start(stop).HasError())
                return FailProbe(errors, options, identity, "interrupt-handler-unavailable",
                                 "XR probe could not acquire native cancellation handling.");
            auto request = MakeRequest(arguments, installation, options.timeout);
            request.forceCancellation = stop.ForceToken();
            ProbeReceipt receipt;
            request.onOutput = [&](const ProcessOutputLine &line) noexcept {
                receipt.Observe(line, identity.install, identity.host, identity.target);
            };
            NativeExternalProcessRunner runner;
            const auto terminal = runner.Run(request, stop.Token());
            if (terminal.HasError())
                return FailProbe(errors, options, identity, "launch-or-retirement-failed", "XR probe launch or process cleanup failed.");
            if (terminal.Value().stopCause == ProcessStopCause::Timeout)
                return FailProbe(errors, options, identity, "timed-out", "XR probe timed out; the supervised process tree was retired.");
            if (terminal.Value().reason != ProcessTerminationReason::Exited || terminal.Value().stopCause != ProcessStopCause::None ||
                stop.Token().IsCancellationRequested())
                return FailProbe(errors, options, identity, "interrupted-or-retired",
                                 "XR probe was interrupted or required process cleanup.");
            const bool finishedSuccess = receipt.kind == ReceiptKind::Success && terminal.Value().exitCode == 0;
            const bool finishedFailure = receipt.kind == ReceiptKind::Failure && terminal.Value().exitCode == 1;
            if (!MatchesExecutable(executable, executableDigest))
                return FailProbe(errors, options, identity, "stale-package",
                                 "XR probe reported no current installed runtime/device receipt. "
                                 "Check the requested default runtime, HMD and package.");
            if (receipt.rejected || (!finishedSuccess && !finishedFailure))
                return FailProbe(errors, options, identity, "unattributed-receipt",
                                 "XR probe reported no current installed runtime/device receipt. Check the requested default runtime, HMD "
                                 "and package.");
            if (options.qualifiedReport) {
                WriteQualifiedReport(finishedSuccess ? output : errors, identity, finishedSuccess ? "preflight" : receipt.Facts(),
                                     finishedSuccess ? receipt.Facts() : std::string_view{});
                return finishedSuccess && output ? 0 : 1;
            }
            if (finishedFailure) {
                errors << receipt.Text() << '\n';
                return 1;
            }
            output << receipt.Text() << '\n';
            return output ? 0 : 1;
        }

    }  // namespace

    /** @copydoc RunOpenXRPackageProbe */
    int RunOpenXRPackageProbe(const std::span<char *> input, std::ostream &output, std::ostream &errors) {
        if (input.size() < Count || std::ranges::any_of(input, [](const char *value) {
            return value == nullptr;
        })) {
            errors << Usage;
            return 2;
        }
        const auto options = ReadOptions(input.subspan(Count));
        if (!options) {
            errors << Usage;
            return 2;
        }
        const auto arguments = input.first(Count);
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
        const ProbeIdentity identity{digest, arguments[ExecutableDigest], host,
                                     FormatSha256(OpenXRProbeWire::TargetDigest(arguments[Runtime], arguments[System]))};
        NativeDurableFileSystem files;
        const auto lease = files.TryAcquireProductLaunch(installation);
        if (lease.HasError() || !MatchesExecutable(executable, executableDigest.Value()))
            return FailProbe(errors, *options, identity, "stale-or-busy-package",
                             "XR probe installation is stale, busy, or differs from its expected executable digest.");

        return ProbeInstalledRuntime(arguments, installation, executable, executableDigest.Value(), identity, *options, output, errors);
    }
}  // namespace Horo::Application::Internal
