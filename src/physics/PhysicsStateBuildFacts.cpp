#include "HoroPhysicsStateBuildFacts.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <version>

#define HORO_STATE_STRINGIFY_IMPL(value) #value
#define HORO_STATE_STRINGIFY(value)      HORO_STATE_STRINGIFY_IMPL(value)

namespace Horo::Physics::Detail::StateBuildFacts {
    namespace {
#if defined(__clang__)
        constexpr std::string_view Compiler = "clang:" __clang_version__;
#elif defined(_MSC_VER)
        constexpr std::string_view Compiler = "msvc:" HORO_STATE_STRINGIFY(_MSC_FULL_VER);
#elif defined(__GNUC__)
        constexpr std::string_view Compiler = "gcc:" __VERSION__;
#else
#error Unsupported native compiler identity for Physics state compatibility
#endif
#if defined(_LIBCPP_VERSION)
        constexpr std::string_view Library =
            "libc++:" HORO_STATE_STRINGIFY(_LIBCPP_VERSION) ":abi:" HORO_STATE_STRINGIFY(_LIBCPP_ABI_VERSION);
#elif defined(__GLIBCXX__)
        constexpr std::string_view Library =
            "libstdc++:" HORO_STATE_STRINGIFY(__GLIBCXX__) ":cxx11:" HORO_STATE_STRINGIFY(_GLIBCXX_USE_CXX11_ABI);
#elif defined(_MSVC_STL_VERSION)
        constexpr std::string_view Library =
            "msvc-stl:" HORO_STATE_STRINGIFY(_MSVC_STL_VERSION) ":update:" HORO_STATE_STRINGIFY(_MSVC_STL_UPDATE);
#else
#error Unsupported native standard library identity for Physics state compatibility
#endif
#if defined(_MSC_VER)
        constexpr std::string_view CompilerAbi =
            "msvc-abi:" HORO_STATE_STRINGIFY(_MSC_FULL_VER) ":language:" HORO_STATE_STRINGIFY(_MSVC_LANG);
#elif defined(__GXX_ABI_VERSION)
        constexpr std::string_view CompilerAbi =
            "gxx-abi:" HORO_STATE_STRINGIFY(__GXX_ABI_VERSION) ":language:" HORO_STATE_STRINGIFY(__cplusplus);
#else
#error Unsupported native compiler ABI for Physics state compatibility
#endif
#if defined(_WIN32)
        constexpr std::string_view OperatingSystem = "windows";
#elif defined(__APPLE__)
        constexpr std::string_view OperatingSystem = "darwin";
#elif defined(__linux__)
        constexpr std::string_view OperatingSystem = "linux";
#else
#error Unsupported native operating system for Physics state compatibility
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
        constexpr std::string_view Architecture = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
        constexpr std::string_view Architecture = "x86_64";
#else
#error Unsupported native architecture for Physics state compatibility
#endif
        constexpr std::uint64_t CodeGenerationFeatures = 0
#if defined(__SSE2__)
                                                         | (1ULL << 0)
#endif
#if defined(__SSE3__)
                                                         | (1ULL << 1)
#endif
#if defined(__SSSE3__)
                                                         | (1ULL << 2)
#endif
#if defined(__SSE4_1__)
                                                         | (1ULL << 3)
#endif
#if defined(__SSE4_2__)
                                                         | (1ULL << 4)
#endif
#if defined(__AVX__)
                                                         | (1ULL << 5)
#endif
#if defined(__AVX2__)
                                                         | (1ULL << 6)
#endif
#if defined(__AVX512F__)
                                                         | (1ULL << 7)
#endif
#if defined(__FMA__)
                                                         | (1ULL << 8)
#endif
#if defined(__ARM_NEON)
                                                         | (1ULL << 9)
#endif
#if defined(__ARM_FEATURE_FMA)
                                                         | (1ULL << 10)
#endif
#if defined(__ARM_FEATURE_ATOMICS)
                                                         | (1ULL << 11)
#endif
#if defined(__FAST_MATH__)
                                                         | (1ULL << 12)
#endif
#if defined(__CHAR_UNSIGNED__)
                                                         | (1ULL << 13)
#endif
            ;
        static_assert(std::endian::native == std::endian::little || std::endian::native == std::endian::big);

        /** @brief Adds one domain-delimited bounded literal without heap allocation. */
        void AddText(Sha256Builder &builder, std::string_view value) noexcept {
            static_cast<void>(builder.Update(std::as_bytes(std::span(value.data(), value.size()))));
            constexpr std::array<std::byte, 1> delimiter{};
            static_cast<void>(builder.Update(delimiter));
        }

        /** @brief Encodes native ABI numeric facts canonically, independently of the target byte order. */
        void AddNumber(Sha256Builder &builder, std::uint64_t value) noexcept {
            std::array<std::byte, 8> bytes{};
            for (std::size_t index = 0; index < bytes.size(); ++index) {
                bytes[index] = static_cast<std::byte>(value & 0xffU);
                value >>= 8U;
            }
            static_cast<void>(builder.Update(bytes));
        }

        /** @brief Hashes the actual native TU's ABI, not a consumer-supplied library label or host runtime guess. */
        Sha256Digest NativePlatformAbi() noexcept {
            Sha256Builder builder;
            AddText(builder, "horo.physics.native-platform-abi.v1");
            static_cast<void>(builder.Update(std::as_bytes(std::span(ConfiguredPlatformInputs.bytes))));
            AddText(builder, Compiler);
            AddText(builder, Library);
            AddText(builder, CompilerAbi);
            AddText(builder, OperatingSystem);
            AddText(builder, Architecture);
            constexpr std::array<std::uint64_t, 17> layout{sizeof(void *),
                                                           alignof(void *),
                                                           sizeof(std::size_t),
                                                           alignof(std::size_t),
                                                           sizeof(long),
                                                           sizeof(wchar_t),
                                                           sizeof(float),
                                                           sizeof(double),
                                                           sizeof(long double),
                                                           alignof(std::max_align_t),
                                                           sizeof(std::string),
                                                           alignof(std::string),
                                                           sizeof(std::shared_ptr<void>),
                                                           alignof(std::shared_ptr<void>),
                                                           std::endian::native == std::endian::little ? 1ULL : 2ULL,
                                                           static_cast<std::uint64_t>(static_cast<char>(-1) < 0),
                                                           __cplusplus};
            for (const auto value : layout)
                AddNumber(builder, value);
            AddNumber(builder, CodeGenerationFeatures);
            return builder.Finalize();
        }
    }  // namespace

    // Pure bounded metadata initialization: no I/O, registration, allocation or service discovery.
    const Sha256Digest PlatformAbi = NativePlatformAbi();
}  // namespace Horo::Physics::Detail::StateBuildFacts

#undef HORO_STATE_STRINGIFY
#undef HORO_STATE_STRINGIFY_IMPL
