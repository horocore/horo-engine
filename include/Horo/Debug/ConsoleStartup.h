#pragma once

/** @file ConsoleStartup.h @brief Bounded console launch parsing and root-bound allowlisted script preparation. */
#include "Horo/Debug/ConsoleConfiguration.h"
#include "Horo/Platform/ProjectReadFiles.h"

namespace Horo::Debug {
    /** @brief Closed startup input kind; filesystem approval is separate from command admission. */
    enum class ConsoleStartupKind : std::uint8_t {
        Command,
        Script
    };

    /** @brief One owned ordered startup request; parsing/configuration projection never executes it. */
    struct ConsoleStartupRequest final {
        ConsoleStartupKind kind{ConsoleStartupKind::Command};
        std::string text; /**< Command line or safe portable script path according to kind. */
    };

    /** @brief Resolved feature request; parsing constructs no service, endpoint or UI. */
    struct ConsoleLaunchConfiguration final {
        bool console{};
        bool overlays{};
        std::vector<OverlaySource> panels;
        std::vector<ConsoleStartupRequest> startup; /**< Original argument order, including interleaved commands and scripts. */
        std::size_t historyLimit{500};
        ConfigurationRevision configurationRevision{};
    };

    /** @brief Immutable bounded capture; the host merges its typed Invocation inputs through the existing Foundation resolver. */
    class ConsoleLaunchArguments final {
    public:
        /** @brief Captures only the console-specific argument slice, without querying or changing configuration/services.
         * @param arguments Host-assigned slice. @return Owned capture or safe typed failure; remote endpoints remain unsupported.
         */
        [[nodiscard]] static Result<ConsoleLaunchArguments> Parse(std::span<const std::string_view> arguments);
        /** @brief Exposes the canonical typed source contribution, with no alternate precedence implementation.
         * @return Immutable inputs for the host's ConfigurationResolutionRequest::invocation; reject host key collisions.
         */
        [[nodiscard]] const ConfigurationSourceMap &ConfigurationInputs() const noexcept;
        /** @brief Returns bounded startup requests in original argument order.
         * @return Borrowed immutable requests; script approval and command allowlists remain separate.
         */
        [[nodiscard]] std::span<const ConsoleStartupRequest> Startup() const noexcept;

    private:
        ConsoleLaunchArguments() = default;
        ConfigurationSourceMap inputs_;
        std::vector<ConsoleStartupRequest> startup_;
    };

    /** @brief Projects launch requests only after the host's canonical source resolution.
     * @param arguments Immutable captured inputs. @param snapshot Canonical snapshot with those exact Invocation winners.
     * @param profile Fixed artifact identity. @param shippingUi Project-approved local Shipping surface.
     * @return Bounded profile-admitted request carrying its snapshot revision, or safe typed failure; starts no service/UI.
     */
    [[nodiscard]] Result<ConsoleLaunchConfiguration> ResolveConsoleLaunch(const ConsoleLaunchArguments &arguments,
                                                                          const ConfigurationSnapshotRef &snapshot, ProductProfile profile,
                                                                          bool shippingUi = false);
    /** @brief Captures an exact approved script via the existing no-follow rooted filesystem authority.
     * @param path Exact portable relative spelling, not a native or normalized prefix.
     * @param approvedPaths Exact project/user paths approved by the host; no wildcard or directory-prefix grants.
     * @param files Existing root-bound filesystem capability. @param readContext Original authority and deadline.
     * @param registry Same immutable registry used for interactive submissions. @param principal Host principal.
     * @return At most 256 prevalidated command lines from a bounded UTF-8 capture. No command is executed here.
     * @note Invoke during startup or from an I/O job, never during interactive frame rendering.
     */
    [[nodiscard]] Result<std::vector<std::string>> ReadConsoleScript(std::string_view path, std::span<const std::string_view> approvedPaths,
                                                                     const Platform::IProjectReadFiles &files,
                                                                     const Platform::ProjectReadContext &readContext,
                                                                     const CommandRegistry &registry, const DebugCommandContext &principal);
}  // namespace Horo::Debug
