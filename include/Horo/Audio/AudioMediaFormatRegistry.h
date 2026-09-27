#pragma once

/**
 * @file AudioMediaFormatRegistry.h
 * @brief Typed container, codec and PCM capability metadata for audio import composition.
 */

#include "Horo/Audio/AudioFormat.h"
#include "Horo/Foundation/Result.h"

#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Horo::Audio {
    /** @brief Stable audio container identity; values are not file extensions or codec identities. */
    struct AudioContainerId final {
        std::uint32_t value{};

        /** @brief Reports whether the identity is usable. @return True when value is non-zero. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        constexpr auto operator<=>(const AudioContainerId &) const noexcept = default;
    };

    /** @brief Stable audio codec identity; values are independent of their possible containers. */
    struct AudioCodecId final {
        std::uint32_t value{};

        /** @brief Reports whether the identity is usable. @return True when value is non-zero. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        constexpr auto operator<=>(const AudioCodecId &) const noexcept = default;
    };

    namespace AudioContainerIds {
        inline constexpr AudioContainerId Wave{1};       /**< RIFF/WAVE container. */
        inline constexpr AudioContainerId Ogg{2};        /**< Ogg container. */
        inline constexpr AudioContainerId HoroCooked{3}; /**< Versioned Horo PCM cook payload container. */
    }  // namespace AudioContainerIds

    namespace AudioCodecIds {
        inline constexpr AudioCodecId Pcm{1};    /**< Pulse-code modulation payload. */
        inline constexpr AudioCodecId Vorbis{2}; /**< Vorbis compressed payload. */
        inline constexpr AudioCodecId Opus{3};   /**< Opus compressed payload. */
    }  // namespace AudioCodecIds

    /** @brief Codec payload families used to validate representation-specific metadata. */
    enum class AudioCodecPayloadKind : std::uint8_t {
        Pcm,
        Compressed
    };

    /** @brief Bounded display metadata for one container capability. */
    struct AudioContainerDescriptor final {
        AudioContainerId id;
        std::string displayName;
        bool operator==(const AudioContainerDescriptor &) const = default;
    };

    /** @brief Bounded display metadata and payload family for one codec capability. */
    struct AudioCodecDescriptor final {
        AudioCodecId id;
        std::string displayName;
        AudioCodecPayloadKind payloadKind{AudioCodecPayloadKind::Compressed};
        bool operator==(const AudioCodecDescriptor &) const = default;
    };

    /** @brief One exact admitted container/codec/PCM representation tuple. */
    struct AudioMediaFormatBinding final {
        AudioContainerId container;
        AudioCodecId codec;
        std::optional<AudioPcmFormat> pcm;
        bool operator==(const AudioMediaFormatBinding &) const = default;
    };

    /** @brief Hard construction bounds for an immutable audio format registry. */
    struct AudioMediaFormatRegistryLimits final {
        std::size_t maximumContainers{64};
        std::size_t maximumCodecs{256};
        std::size_t maximumBindings{1024};
        std::size_t maximumDisplayNameBytes{128};
    };

    /** @brief Borrowed metadata copied synchronously during registry creation. */
    struct AudioMediaFormatContributions final {
        std::span<const AudioContainerDescriptor> containers;
        std::span<const AudioCodecDescriptor> codecs;
        std::span<const AudioMediaFormatBinding> bindings;
    };

    /** @brief Owned inert snapshot for exact audio media capability discovery. */
    class AudioMediaFormatRegistry final {
    public:
        /**
         * @brief Copies and validates bounded container, codec and exact format metadata.
         * @param contributions Borrowed metadata valid for this call only.
         * @param limits Non-zero construction and display-name bounds.
         * @return Immutable registry or a typed invalid, capacity or conflict error.
         * @throws std::bad_alloc When copying contribution metadata fails.
         */
        [[nodiscard]] static Result<AudioMediaFormatRegistry> Create(const AudioMediaFormatContributions &contributions,
                                                                     const AudioMediaFormatRegistryLimits &limits = {});

        /** @brief Lists admitted containers. @return Stable view for the registry lifetime. */
        [[nodiscard]] std::span<const AudioContainerDescriptor> Containers() const noexcept;
        /** @brief Lists admitted codecs independently of containers. @return Stable view for the registry lifetime. */
        [[nodiscard]] std::span<const AudioCodecDescriptor> Codecs() const noexcept;
        /** @brief Lists exact admitted tuples. @return Stable view for the registry lifetime. */
        [[nodiscard]] std::span<const AudioMediaFormatBinding> Bindings() const noexcept;

        /**
         * @brief Resolves one exact tuple without inferring codec support from its container.
         * @param requested Exact container, codec and representation request.
         * @return The canonical binding or a typed invalid, unknown-identity or unsupported-combination error.
         * @throws std::bad_alloc When constructing an error value fails.
         */
        [[nodiscard]] Result<AudioMediaFormatBinding> Resolve(const AudioMediaFormatBinding &requested) const;

    private:
        std::vector<AudioContainerDescriptor> containers_;
        std::vector<AudioCodecDescriptor> codecs_;
        std::vector<AudioMediaFormatBinding> bindings_;
    };
}  // namespace Horo::Audio
