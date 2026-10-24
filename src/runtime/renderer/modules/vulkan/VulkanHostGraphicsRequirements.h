#pragma once
/** @file VulkanHostGraphicsRequirements.h
 * @brief Private immutable pre-device host requirements; no backend selection or native allocation.
 */
#include "Horo/Foundation/Result.h"

#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Horo::Render {
    /** @brief Vulkan API version reported independently by the loader, runtime requirements and adapter. */
    struct VulkanApiVersion {
        std::uint32_t major{};
        std::uint32_t minor{};
        std::uint32_t patch{};
        [[nodiscard]] constexpr auto operator<=>(const VulkanApiVersion &) const noexcept = default;
    };

    /** @brief Inclusive exact native interoperability API interval, supplied by verified host preflight. */
    struct VulkanHostApiRange {
        VulkanApiVersion minimum;
        VulkanApiVersion maximum;
    };

    /** @brief Owned bounded graphics requirements sampled before instance/device creation and immutable thereafter. */
    class VulkanHostGraphicsRequirements final {
    public:
        /** @brief Validates finite exact names and API bounds without querying a runtime.
         * @param instance Required instance extensions, at most 32 unique names.
         * @param device Required device extensions, at most 32 unique names.
         * @param api Inclusive runtime API interval, absent for explicit non-XR composition.
         * @return Immutable requirements or typed invalid input. Explicit preflight failure must never be replaced with empty lists.
         */
        [[nodiscard]] static Result<VulkanHostGraphicsRequirements> Create(std::span<const std::string> instance,
                                                                           std::span<const std::string> device,
                                                                           std::optional<VulkanHostApiRange> api = {});
        /** @brief Returns exact owned instance names. @return Borrow valid through this value. */
        [[nodiscard]] std::span<const std::string> Instance() const noexcept;
        /** @brief Returns exact owned device names. @return Borrow valid through this value. */
        [[nodiscard]] std::span<const std::string> Device() const noexcept;
        /** @brief Tests the exact requested API against any explicit runtime interval. */
        [[nodiscard]] bool Allows(VulkanApiVersion api) const noexcept;

    private:
        /** @brief Retains only completely validated immutable preparation facts. */
        VulkanHostGraphicsRequirements(std::vector<std::string> instance, std::vector<std::string> device,
                                       std::optional<VulkanHostApiRange> api);
        std::vector<std::string> instance_;
        std::vector<std::string> device_;
        std::optional<VulkanHostApiRange> api_;
    };
}  // namespace Horo::Render
