#pragma once

/**
 * @file UiTemplateDependencyGraph.h
 * @brief Bounded, side-effect-free resolution of pinned Runtime UI template dependencies.
 */

#include "Horo/Assets/AssetId.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Packages/PackageLockfile.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Horo::UiTemplates {
    /** @brief Template-domain identity distinct from generic assets and document elements. */
    struct UiTemplateAssetId final {
        Assets::AssetId asset; /**< Stable Assets identity of this template. */

        /** @brief Checks the underlying persistent identity. @return Whether it is non-zero. */
        [[nodiscard]] bool IsValid() const noexcept {
            return asset.IsValid();
        }

        [[nodiscard]] auto operator<=>(const UiTemplateAssetId &) const noexcept = default;
    };

    /** @brief Public template interface version; major changes are incompatible. */
    struct UiTemplateInterfaceVersion final {
        std::uint16_t major{};
        std::uint16_t minor{};
        [[nodiscard]] auto operator<=>(const UiTemplateInterfaceVersion &) const noexcept = default;
    };

    /** @brief Source schema version accepted by this graph resolver. */
    struct UiTemplateSchemaVersion final {
        std::uint16_t major{};
        std::uint16_t minor{};
        [[nodiscard]] auto operator<=>(const UiTemplateSchemaVersion &) const noexcept = default;
    };

    inline constexpr UiTemplateSchemaVersion CurrentUiTemplateSchemaVersion{1, 0};

    /** @brief Exact accepted template revision and minimum compatible public interface. */
    struct UiTemplateReference final {
        UiTemplateAssetId asset;
        Sha256Digest revision; /**< Canonical semantic digest; all-zero is invalid. */
        UiTemplateInterfaceVersion minimumInterface;
    };

    /** @brief Required package identity and version range, checked against an already pinned lock. */
    struct UiTemplatePackageRequirement final {
        Packages::HoroPackageId package;
        Packages::PackageVersionRange versions;
    };

    /** @brief Owned source metadata supplied by the template domain after schema validation. */
    struct UiTemplateDependencyDescriptor final {
        UiTemplateAssetId asset;
        UiTemplateSchemaVersion schema{CurrentUiTemplateSchemaVersion};
        UiTemplateInterfaceVersion interfaceVersion;
        Sha256Digest revision;
        std::vector<UiTemplateReference> nested;
        std::vector<UiTemplatePackageRequirement> packages;
    };

    /** @brief Upper bounds for one catalog and one nested resolution. */
    struct UiTemplateGraphLimits final {
        std::size_t maximumTemplates{256};
        std::size_t maximumEdges{1024};
        std::size_t maximumDepth{32};
        std::size_t maximumPackages{256};

        /** @brief Checks positive bounds against compiled ceilings. @return Whether the limits are usable. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Detached dependency-first result that survives catalog reload or resolver shutdown. */
    struct UiTemplateResolvedDependencies final {
        std::vector<UiTemplateAssetId> templates;               /**< Each reached template exactly once, dependencies first. */
        std::vector<Packages::LockedPackageReference> packages; /**< Required pinned packages in canonical ID order. */
    };

    /** @brief Owner-thread resolver over an owned immutable snapshot; no I/O or ambient discovery. */
    class UiTemplateDependencyGraph final {
    public:
        /**
         * @brief Copies and validates a pre-pinned template catalog and package lock snapshot.
         * @param descriptors Complete candidate template metadata; missing nested assets remain a Resolve failure.
         * @param packages Exact package versions selected by the host's verified lock.
         * @param limits Bounded catalog and traversal policy.
         * @return Owned resolver or a typed malformed/duplicate/budget failure.
         */
        [[nodiscard]] static Result<UiTemplateDependencyGraph> Create(std::span<const UiTemplateDependencyDescriptor> descriptors,
                                                                      std::span<const Packages::LockedPackageReference> packages,
                                                                      const UiTemplateGraphLimits &limits = {});

        UiTemplateDependencyGraph(const UiTemplateDependencyGraph &) = delete;
        UiTemplateDependencyGraph &operator=(const UiTemplateDependencyGraph &) = delete;
        UiTemplateDependencyGraph(UiTemplateDependencyGraph &&) noexcept = default;
        UiTemplateDependencyGraph &operator=(UiTemplateDependencyGraph &&) noexcept = default;

        /**
         * @brief Resolves one exact accepted revision in deterministic dependency-first order.
         * @param root Exact root revision and minimum interface requirement.
         * @return Detached closure, or a typed missing/revision/interface/cycle/package/budget failure.
         * @pre No concurrent Shutdown call. Resolution is a load-time operation, never frame-hot.
         * @post Failure publishes no partial closure and does not change the resolver.
         */
        [[nodiscard]] Result<UiTemplateResolvedDependencies> Resolve(const UiTemplateReference &root) const;

        /** @brief Closes admission and releases the owned snapshot; repeated calls are safe. */
        void Shutdown() noexcept;

    private:
        /** @brief Adopts the validated copied catalog and lock without further I/O. */
        UiTemplateDependencyGraph(std::vector<UiTemplateDependencyDescriptor> descriptors,
                                  std::vector<Packages::LockedPackageReference> packages, const UiTemplateGraphLimits &limits) noexcept;

        std::vector<UiTemplateDependencyDescriptor> descriptors_;
        std::vector<Packages::LockedPackageReference> packages_;
        UiTemplateGraphLimits limits_;
        bool active_{true};
    };
}  // namespace Horo::UiTemplates
