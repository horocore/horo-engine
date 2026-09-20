#pragma once

/**
 * @file DistributionModel.h
 * @brief Backend-neutral distribution identities, package capabilities, and admission rules.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Release/ReleaseVersion.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace Horo::Release {
    /** @brief Maximum bytes admitted by a distribution identity component. */
    inline constexpr std::size_t MaximumDistributionIdentityBytes = 128;

    /** @brief Distinct products that may own distribution artifacts. */
    enum class DistributionProductKind : std::uint8_t {
        Editor,
        EngineCli,
        PackageToolCli,
        PublicSdk,
        RendererComponent,
        GameRuntime,
        GameDedicatedServer
    };

    /** @brief Product identity with a component ID reserved for independently installed renderers. */
    struct DistributionProductIdentity final {
        DistributionProductKind kind{DistributionProductKind::Editor};
        std::string componentId;
        bool operator==(const DistributionProductIdentity &) const noexcept = default;
    };

    /** @brief Supported operating-system families; package format is selected separately. */
    enum class DistributionPlatform : std::uint8_t {
        Windows,
        MacOS,
        Linux
    };

    /** @brief Supported CPU architecture identities. */
    enum class DistributionArchitecture : std::uint8_t {
        X64,
        Arm64
    };

    /** @brief Ordinary installable output or a deliberately separate supplemental artifact. */
    enum class DistributionArtifactClass : std::uint8_t {
        InstallableProduct,
        Symbols,
        Diagnostics
    };

    /** @brief Explicit package formats selected by release policy or profile. */
    enum class DistributionPackageFormat : std::uint8_t {
        WindowsMsi,
        WindowsExeInstaller,
        ZipArchive,
        MacDmg,
        MacPkg,
        MacAppBundle,
        LinuxAppImage,
        TarGzip,
        LinuxDeb,
        LinuxRpm,
        StorePackage
    };

    /** @brief Install layout behavior exposed without native installer types. */
    enum class DistributionInstallLayout : std::uint8_t {
        Portable,
        SystemManaged,
        ApplicationBundle,
        StoreManaged
    };

    /** @brief Capability availability or mandatory policy for a package format. */
    enum class DistributionCapability : std::uint8_t {
        Unsupported,
        Supported,
        Required
    };

    /** @brief Cross-platform behavior contract for one explicit package format. */
    struct DistributionPackageCapabilities final {
        DistributionInstallLayout installLayout{DistributionInstallLayout::Portable};
        DistributionCapability fileAssociations{DistributionCapability::Unsupported};
        DistributionCapability desktopIntegration{DistributionCapability::Unsupported};
        DistributionCapability uninstall{DistributionCapability::Unsupported};
        DistributionCapability updates{DistributionCapability::Unsupported};
        DistributionCapability signing{DistributionCapability::Unsupported};
        DistributionCapability rollback{DistributionCapability::Unsupported};
        bool operator==(const DistributionPackageCapabilities &) const noexcept = default;
    };

    /** @brief Strong immutable build identity. */
    struct DistributionBuildId final {
        std::string value;
        bool operator==(const DistributionBuildId &) const noexcept = default;
    };

    /** @brief Strong package identity shared by installable and supplemental artifacts. */
    struct DistributionPackageId final {
        std::string value;
        bool operator==(const DistributionPackageId &) const noexcept = default;
    };

    /** @brief Strong intended installation identity, absent for symbols and diagnostics. */
    struct DistributionInstallationId final {
        std::string value;
        bool operator==(const DistributionInstallationId &) const noexcept = default;
    };

    /** @brief Exact artifact identity submitted for package-format admission; its version must be canonical parsed SemVer data. */
    struct DistributionArtifactIdentity final {
        DistributionProductIdentity product;
        ReleaseProductVersion version;
        DistributionPlatform platform{DistributionPlatform::Windows};
        DistributionArchitecture architecture{DistributionArchitecture::X64};
        DistributionBuildId build;
        DistributionPackageId package;
        std::optional<DistributionInstallationId> installation;
        DistributionArtifactClass artifactClass{DistributionArtifactClass::InstallableProduct};
        bool operator==(const DistributionArtifactIdentity &) const noexcept = default;
    };

    /** @brief Validated explicit format selection and its behavior capabilities. */
    struct DistributionPackageSelection final {
        DistributionArtifactIdentity artifact;
        DistributionPackageFormat format{DistributionPackageFormat::ZipArchive};
        DistributionPackageCapabilities capabilities;
        bool operator==(const DistributionPackageSelection &) const noexcept = default;
    };

    /**
     * @brief Validates one portable distribution-domain identity component.
     * @param value Candidate identity text.
     * @return True only for the canonical bounded identity grammar.
     */
    [[nodiscard]] bool IsValidDistributionIdentity(std::string_view value) noexcept;

    /**
     * @brief Resolves the Horo-owned capability descriptor for an explicit format and platform.
     * @param format Package format selected by policy or profile.
     * @param platform Exact target platform.
     * @return Capabilities or an unsupported-combination error.
     */
    [[nodiscard]] Result<DistributionPackageCapabilities> DescribeDistributionPackageFormat(DistributionPackageFormat format,
                                                                                            DistributionPlatform platform);

    /**
     * @brief Validates product, supplemental-artifact class, platform, and format policy without an artifact instance.
     * @param product Typed product identity.
     * @param artifactClass Installable, symbol, or diagnostics output class.
     * @param platform Exact target platform.
     * @param format Explicit package format.
     * @return Authoritative format capabilities or an invalid/unsupported-combination error.
     */
    [[nodiscard]] Result<DistributionPackageCapabilities> ValidateDistributionProductPackageFormat(
        const DistributionProductIdentity &product, DistributionArtifactClass artifactClass, DistributionPlatform platform,
        DistributionPackageFormat format);

    /**
     * @brief Validates one explicit product/platform/format selection before external work begins.
     * @param artifact Complete typed artifact identity.
     * @param format Explicit format selected by policy or profile, never inferred from platform.
     * @return Validated immutable selection or a typed identity/combination error; malformed versions fail identity admission.
     */
    [[nodiscard]] Result<DistributionPackageSelection> ValidateDistributionPackageSelection(const DistributionArtifactIdentity &artifact,
                                                                                            DistributionPackageFormat format);
}  // namespace Horo::Release
