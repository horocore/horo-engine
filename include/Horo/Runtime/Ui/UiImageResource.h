#pragma once

/**
 * @file UiImageResource.h
 * @brief Bounded backend-neutral Runtime UI image, sprite, atlas, and residency contracts.
 */

#include "Horo/Runtime/Ui/UiAssetDependency.h"
#include "Horo/Runtime/Ui/UiIdentity.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Horo::Runtime::Ui {
    /** @brief Maximum image/atlas pages admitted by one immutable UI image resource. */
    inline constexpr std::uint32_t MaximumUiImagePages = 64;
    /** @brief Maximum sprite regions admitted by one immutable UI image resource. */
    inline constexpr std::uint32_t MaximumUiImageRegions = 16'384;
    /** @brief Maximum canonical asset dependencies admitted by one image resource. */
    inline constexpr std::uint32_t MaximumUiImageDependencies = 1'024;
    /** @brief Maximum image resources that one owner-thread registry can retain. */
    inline constexpr std::uint32_t MaximumUiImageResourceSlots = 4'096;
    /** @brief Sentinel selecting the complete standalone-image page. */
    inline constexpr std::uint32_t NoUiImageRegion = 0;

    /** @brief Positive source-pixel extent used by image pages and sprite regions. */
    struct UiImageExtent final {
        std::uint32_t width{};  /**< Positive pixel width. */
        std::uint32_t height{}; /**< Positive pixel height. */

        /** @brief Checks for a representable positive extent. @return True when both dimensions are non-zero. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return width > 0 && height > 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const UiImageExtent &) const noexcept = default;
    };

    /** @brief Closed color-space meaning of decoded image samples. */
    enum class UiImageColorSpace : std::uint8_t {
        Linear,
        Srgb,
    };

    /** @brief Closed filtering choice for one image sampling axis. */
    enum class UiImageFilter : std::uint8_t {
        Nearest,
        Linear,
    };

    /** @brief Closed mip-level selection policy. */
    enum class UiImageMipmapMode : std::uint8_t {
        None,
        Nearest,
        Linear,
    };

    /** @brief Explicit fallback policy when the source image is not resident. */
    enum class UiImageFallbackPolicy : std::uint8_t {
        Reject,
        Transparent,
        Checkerboard,
    };

    /** @brief Residency evidence captured at an image-resource publication boundary. */
    enum class UiImageResidencyState : std::uint8_t {
        Unresolved,
        Loading,
        Resident,
        MissingFallback,
        Failed,
    };

    /** @brief Backend-neutral image sampling semantics carried into an immutable render snapshot. */
    struct UiImageSampling final {
        UiImageFilter minFilter{UiImageFilter::Linear};
        UiImageFilter magFilter{UiImageFilter::Linear};
        UiImageMipmapMode mipmap{UiImageMipmapMode::None};

        /** @brief Checks the closed sampler vocabulary. @return True for a representable sampling policy. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return static_cast<std::uint8_t>(minFilter) <= static_cast<std::uint8_t>(UiImageFilter::Linear) &&
                   static_cast<std::uint8_t>(magFilter) <= static_cast<std::uint8_t>(UiImageFilter::Linear) &&
                   static_cast<std::uint8_t>(mipmap) <= static_cast<std::uint8_t>(UiImageMipmapMode::Linear);
        }

        [[nodiscard]] constexpr auto operator<=>(const UiImageSampling &) const noexcept = default;
    };

    /** @brief Normalized source rectangle for a sprite or atlas region. */
    struct UiImageUvRect final {
        float u0{};     /**< Inclusive left coordinate in [0, 1]. */
        float v0{};     /**< Inclusive top coordinate in [0, 1]. */
        float u1{1.0F}; /**< Exclusive right coordinate in [0, 1]. */
        float v1{1.0F}; /**< Exclusive bottom coordinate in [0, 1]. */

        /** @brief Validates finite ordered normalized coordinates. @return True for a non-empty normalized rectangle. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiImageUvRect &) const noexcept = default;
    };

    /** @brief Source-pixel borders for a nine-slice sprite region. */
    struct UiImageNineSliceInsets final {
        std::uint32_t left{};   /**< Left source-pixel inset. */
        std::uint32_t top{};    /**< Top source-pixel inset. */
        std::uint32_t right{};  /**< Right source-pixel inset. */
        std::uint32_t bottom{}; /**< Bottom source-pixel inset. */

        /** @brief Checks that borders fit inside the source extent. @param extent Source-pixel region extent.
         * @return True when all borders are representable and their opposing sums fit.
         */
        [[nodiscard]] constexpr bool IsValid(const UiImageExtent extent) const noexcept {
            return extent.IsValid() && left <= extent.width && right <= extent.width - left && top <= extent.height &&
                   bottom <= extent.height - top;
        }

        /** @brief Reports whether this is a regular unsliced rectangle. @return True when every inset is zero. */
        [[nodiscard]] constexpr bool IsEmpty() const noexcept {
            return left == 0 && top == 0 && right == 0 && bottom == 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const UiImageNineSliceInsets &) const noexcept = default;
    };

    /** @brief One immutable page backing an image or an atlas. */
    struct UiImagePage final {
        UiAssetDependency dependency;                          /**< Cook/residency identity for the page bytes. */
        UiImageExtent extent;                                  /**< Decoded source-pixel extent. */
        UiImageColorSpace colorSpace{UiImageColorSpace::Srgb}; /**< Sample interpretation. */
        UiImageSampling sampling;                              /**< Explicit sampling semantics. */

        /** @brief Validates page identity and typed image metadata. @return True for a complete page declaration. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiImagePage &) const noexcept = default;
    };

    /** @brief One stable atlas/sprite region resolved to a page and source rectangle. */
    struct UiSpriteRegion final {
        std::uint32_t id{};               /**< Non-zero stable region identity within the resource. */
        std::uint32_t page{};             /**< Zero-based page index. */
        UiImageUvRect uv;                 /**< Normalized source rectangle on the page. */
        UiImageExtent pixelExtent;        /**< Source-pixel extent represented by the region. */
        UiImageNineSliceInsets nineSlice; /**< Optional source-pixel nine-slice borders. */

        /** @brief Validates the region against its owning page. @param ownerPage Page selected by page.
         * @return True for a complete bounded region declaration.
         */
        [[nodiscard]] bool IsValid(const UiImagePage &ownerPage) const noexcept;
        [[nodiscard]] auto operator<=>(const UiSpriteRegion &) const noexcept = default;
    };

    /** @brief Descriptive alias for callers that model atlas pages explicitly. */
    using UiImageAtlasPage = UiImagePage;
    /** @brief Descriptive alias for callers that use image-region terminology. */
    using UiImageRegion = UiSpriteRegion;

    /** @brief Closed image-resource kind used by a descriptor and immutable resource. */
    enum class UiImageResourceKind : std::uint8_t {
        Image,
        Atlas,
    };

    /** @brief Non-owning load-time declaration copied into an immutable image resource. */
    struct UiImageResourceDescriptor final {
        UiImageResourceKind kind{UiImageResourceKind::Image};
        UiImageFallbackPolicy fallback{UiImageFallbackPolicy::Reject};
        std::span<const UiImagePage> pages;
        std::span<const UiSpriteRegion> regions;

        /** @brief Validates resource kind, page/region bounds, and fallback representation. @return True when creation may proceed. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Fully resolved source data for one standalone image or sprite region. */
    struct UiResolvedImageRegion final {
        std::uint32_t page{};             /**< Owning page index. */
        UiImageUvRect uv;                 /**< Normalized source rectangle. */
        UiImageExtent pixelExtent;        /**< Source-pixel rectangle extent. */
        UiImageNineSliceInsets nineSlice; /**< Source-pixel nine-slice borders. */

        [[nodiscard]] constexpr auto operator<=>(const UiResolvedImageRegion &) const noexcept = default;
    };

    /** @brief Generation-safe identity of one published UI image resource. */
    struct UiImageResourceHandleTag;
    using UiImageResourceHandle = UiRuntimeHandle<UiImageResourceHandleTag>;

    /** @brief Monotonic source revision associated with one image-resource publication. */
    struct UiImageResourceRevisionTag;
    using UiImageResourceRevision = UiRevision<UiImageResourceRevisionTag>;

    /**
     * @brief Immutable validated image or atlas resource independent of renderer residency.
     * @details Creation copies bounded page, region, and dependency data. It performs no provider lookup, path resolution,
     * native resource creation, or ambient registration. The returned dependency span is the cook/residency manifest to merge
     * into the owning UiDocumentBuilder transaction.
     */
    class UiImageResource final {
    public:
        /** @brief Constructs an invalid empty value. @note Only Create() yields a usable resource. */
        UiImageResource() noexcept = default;
        /** @brief Releases owned immutable resource data. */
        ~UiImageResource();
        UiImageResource(UiImageResource &&) noexcept;
        UiImageResource &operator=(UiImageResource &&) noexcept;
        UiImageResource(const UiImageResource &) = delete;
        UiImageResource &operator=(const UiImageResource &) = delete;

        /** @brief Validates and copies one load-time descriptor into an immutable resource.
         * @param descriptor Non-owning bounded page and region declaration.
         * @return Complete resource or typed validation/capacity failure.
         */
        [[nodiscard]] static Result<UiImageResource> Create(const UiImageResourceDescriptor &descriptor);
        /** @brief Reports whether this value owns a complete validated resource. @return True after successful Create(). */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Returns the resource kind. @return Image or Atlas; Image for an invalid value. */
        [[nodiscard]] UiImageResourceKind Kind() const noexcept;
        /** @brief Returns the explicit missing-residency policy. @return Resource fallback policy. */
        [[nodiscard]] UiImageFallbackPolicy FallbackPolicy() const noexcept;
        /** @brief Returns immutable pages in canonical source order. @return Borrowed page span. */
        [[nodiscard]] std::span<const UiImagePage> Pages() const noexcept;
        /** @brief Returns immutable atlas regions in ascending stable-ID order. @return Borrowed region span. */
        [[nodiscard]] std::span<const UiSpriteRegion> Regions() const noexcept;
        /** @brief Returns canonical page dependencies for cook/residency admission. @return Borrowed dependency span. */
        [[nodiscard]] std::span<const UiAssetDependency> Dependencies() const noexcept;
        /** @brief Looks up a page without allocation. @param index Zero-based page index. @return Page or nullptr. */
        [[nodiscard]] const UiImagePage *Page(std::uint32_t index) const noexcept;
        /** @brief Looks up a stable atlas region without allocation. @param id Non-zero region identity.
         * @return Region or nullptr.
         */
        [[nodiscard]] const UiSpriteRegion *FindRegion(std::uint32_t id) const noexcept;
        /** @brief Resolves a standalone image or atlas region without allocation.
         * @param id NoUiImageRegion for a complete standalone image, otherwise a stable atlas region ID.
         * @return Complete source rectangle or UiErrors::ImageRegionInvalid.
         */
        [[nodiscard]] Result<UiResolvedImageRegion> ResolveRegion(std::uint32_t id = NoUiImageRegion) const;

    private:
        struct Storage;
        explicit UiImageResource(std::unique_ptr<Storage> storage) noexcept;
        std::unique_ptr<Storage> storage_;
    };

    /** @brief Bounded owner identity and resource-slot capacity for one image registry. */
    struct UiImageResourceRegistryDescriptor final {
        UiOwnershipGeneration owner;      /**< Exact process-local owner incarnation. */
        std::uint32_t maximumResources{}; /**< Number of preallocated resource slots. */

        /** @brief Validates owner and bounded capacity. @return True when the registry can be created. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Explicit publication lifecycle for one owner-thread image registry. */
    enum class UiImageResourceRegistryState : std::uint8_t {
        Active,
        Closed,
    };

    class UiImageResourceRegistry;

    /**
     * @brief Immutable generation lease for one published image resource.
     * @details Copies retain the exact source generation through reload and shutdown. A snapshot is drawable only when its
     * publication state is Resident or an explicitly permitted MissingFallback state.
     */
    class UiImageResourceSnapshot final {
    public:
        /** @brief Constructs an invalid empty snapshot. @note Only Acquire() yields a live generation lease. */
        UiImageResourceSnapshot() noexcept = default;
        ~UiImageResourceSnapshot();
        UiImageResourceSnapshot(const UiImageResourceSnapshot &other) noexcept;
        UiImageResourceSnapshot &operator=(const UiImageResourceSnapshot &other) noexcept;
        UiImageResourceSnapshot(UiImageResourceSnapshot &&other) noexcept;
        UiImageResourceSnapshot &operator=(UiImageResourceSnapshot &&other) noexcept;

        /** @brief Reports whether this object retains a published generation. @return True for a live snapshot. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Returns the exact registry handle captured by this lease. @return Published handle evidence. */
        [[nodiscard]] UiImageResourceHandle Handle() const noexcept;
        /** @brief Returns the exact source revision captured by this lease. @return Published image revision. */
        [[nodiscard]] UiImageResourceRevision Revision() const noexcept;
        /** @brief Returns publication residency evidence. @return Immutable residency state. */
        [[nodiscard]] UiImageResidencyState Residency() const noexcept;
        /** @brief Reports whether this generation may be submitted for drawing. @return Resident or explicit fallback. */
        [[nodiscard]] bool IsDrawable() const noexcept;
        /** @brief Returns the immutable resource retained by this snapshot. @return Resource or nullptr for an invalid snapshot. */
        [[nodiscard]] const UiImageResource *Resource() const noexcept;
        /** @brief Returns the resource's canonical dependency manifest. @return Borrowed dependency span. */
        [[nodiscard]] std::span<const UiAssetDependency> Dependencies() const noexcept;

    private:
        struct Generation;
        struct LeaseTracker;
        friend class UiImageResourceRegistry;
        UiImageResourceSnapshot(std::shared_ptr<const Generation> generation, std::shared_ptr<LeaseTracker> tracker) noexcept;
        void Retain() const noexcept;
        void Release() noexcept;

        std::shared_ptr<const Generation> generation_;
        std::shared_ptr<LeaseTracker> tracker_;
    };

    /**
     * @brief Owner-thread bounded publication registry for image resources and hot-reload generations.
     * @details All slot storage is reserved at creation. Publish and Reload are load/residency boundary operations; Acquire is
     * allocation-free and does not resolve paths, block, choose a provider, or create native resources.
     */
    class UiImageResourceRegistry final {
    public:
        /** @brief Creates a registry with all resource slots reserved. @param descriptor Owner and slot capacity.
         * @return Active registry or typed identity/capacity/allocation failure.
         */
        [[nodiscard]] static Result<UiImageResourceRegistry> Create(const UiImageResourceRegistryDescriptor &descriptor);
        ~UiImageResourceRegistry();
        UiImageResourceRegistry(UiImageResourceRegistry &&) noexcept;
        UiImageResourceRegistry &operator=(UiImageResourceRegistry &&) noexcept;
        UiImageResourceRegistry(const UiImageResourceRegistry &) = delete;
        UiImageResourceRegistry &operator=(const UiImageResourceRegistry &) = delete;

        /** @brief Publishes a complete resource into a free preallocated slot. @return New generation-safe handle. */
        [[nodiscard]] Result<UiImageResourceHandle> Publish(UiImageResource resource, UiImageResourceRevision revision,
                                                            UiImageResidencyState residency);
        /** @brief Publishes a strictly newer complete generation and retires the supplied handle atomically.
         * @param current Exact currently resident handle.
         * @param resource Complete replacement resource.
         * @param revision Strictly newer source revision.
         * @param residency Replacement residency evidence.
         * @return New handle; old snapshots remain valid until their leases retire.
         */
        [[nodiscard]] Result<UiImageResourceHandle> Reload(const UiImageResourceHandle &current, UiImageResource resource,
                                                           UiImageResourceRevision revision, UiImageResidencyState residency);
        /** @brief Acquires an immutable generation without allocation or blocking. @param handle Exact current handle.
         * @return Snapshot lease or typed owner/residency/lifecycle failure.
         */
        [[nodiscard]] Result<UiImageResourceSnapshot> Acquire(const UiImageResourceHandle &handle) const;
        /** @brief Retires a current handle without invalidating existing snapshots. @param handle Exact current handle.
         * @return Success or typed owner/stale/lifecycle/generation failure.
         */
        [[nodiscard]] Result<void> Retire(const UiImageResourceHandle &handle);
        /** @brief Stops publication, reload, acquire, and retirement while preserving existing leases. */
        void Close() noexcept;
        /** @brief Reports whether every acquired image snapshot has retired. @return True when no snapshot lease remains. */
        [[nodiscard]] bool IsDrained() const noexcept;
        /** @brief Returns current admission lifecycle. @return Active or Closed. */
        [[nodiscard]] UiImageResourceRegistryState State() const noexcept;

    private:
        struct Storage;
        explicit UiImageResourceRegistry(std::unique_ptr<Storage> storage) noexcept;
        std::unique_ptr<Storage> storage_;
    };
}  // namespace Horo::Runtime::Ui
