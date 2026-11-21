#include "Horo/XR/XRErrors.h"
#include "OpenXRWindowsLoaderImage.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <utility>

namespace Horo::Application::Internal {
    namespace {
        /** @brief Minimal inert PE metadata fixture covering file/RVA boundaries. No executable or native loader is used. */
        struct PeImportFixture final {
            static constexpr std::size_t Pe = 0x80;
            static constexpr std::size_t Optional = Pe + 24;
            static constexpr std::size_t Directories = Optional + 112;
            static constexpr std::size_t Sections = Optional + 240;
            static constexpr std::size_t Raw = 0x200;
            static constexpr std::uint32_t Address = 0x1000;
            static constexpr std::uint32_t SectionSize = 0x200;
            std::array<std::byte, Raw + SectionSize> bytes{};
            std::array<std::string, 2> signedImports{"kernel32.dll", "SHLWAPI.DLL"};

            PeImportFixture() {
                Word(0, 0x5a4d, 2);
                Word(0x3c, Pe);
                Word(Pe, 0x00004550);
                Word(Pe + 4, 0x8664, 2);
                Word(Pe + 6, 1, 2);
                Word(Pe + 20, 240, 2);
                Word(Pe + 22, 0x2002, 2);
                Word(Optional, 0x20b, 2);
                Word(Optional + 56, 0x2000);
                Word(Optional + 60, Raw);
                Word(Optional + 108, 16);
                Word(Directories + 8, Address);
                Word(Directories + 12, 60);
                Word(Sections + 8, SectionSize);
                Word(Sections + 12, Address);
                Word(Sections + 16, SectionSize);
                Word(Sections + 20, Raw);
                Import(Raw, 0x60, 0x100, "KERNEL32.dll");
                Import(Raw + 20, 0x80, 0x120, "shlwapi.dll");
            }

            void Word(std::size_t offset, std::uint32_t value, std::size_t count = 4) {
                for (std::size_t index = 0; index < count; ++index)
                    bytes[offset + index] = std::byte((value >> (index * 8)) & 0xffU);
            }

            void Import(std::size_t entry, std::size_t name, std::size_t iat, std::string_view value) {
                Word(entry + 12, Address + static_cast<std::uint32_t>(name));
                Word(entry + 16, Address + static_cast<std::uint32_t>(iat));
                for (std::size_t i = 0; i < value.size(); ++i)
                    bytes[Raw + name + i] = std::byte(static_cast<unsigned char>(value[i]));
                bytes[Raw + name + value.size()] = std::byte{};
            }

            Result<void> Verify() const {
                return VerifyWindowsOpenXRLoaderImage(bytes, "x64", signedImports);
            }
        };
    }  // namespace

    TEST_CASE("Windows OpenXR preflight compares case-insensitive direct imports to signed ABI names", "[xr][windows-loader][pe]") {
        PeImportFixture fixture;
        REQUIRE(fixture.Verify().HasValue());
        std::swap(fixture.signedImports.front(), fixture.signedImports.back());
        REQUIRE(fixture.Verify().HasValue());

        SECTION("The authenticated set cannot substitute a different ABI") {
            fixture.signedImports.back() = "alternate.dll";
        }
        SECTION("An additional or omitted image import cannot hide behind the same digest policy") {
            CHECK(VerifyWindowsOpenXRLoaderImage(fixture.bytes, "x64", std::span{fixture.signedImports}.first(1)).HasError());
            fixture.Word(PeImportFixture::Directories + 12, 40);
        }
        SECTION("Repeating a signed basename with different case cannot cover a different import") {
            fixture.signedImports.front() = "KERNEL32.DLL";
        }
        SECTION("The address table must start in initialized file-backed bytes") {
            fixture.Word(PeImportFixture::Raw + 16, PeImportFixture::Address + PeImportFixture::SectionSize - 4);
        }
        SECTION("Repeating a case-insensitive import cannot satisfy two signed names") {
            fixture.Import(PeImportFixture::Raw + 20, 0x80, 0x120, "Kernel32.DLL");
        }
        SECTION("A name cannot contain a directory or data-stream selector") {
            fixture.Import(PeImportFixture::Raw + 20, 0x80, 0x120, "C:/shlwapi.dll");
            CHECK_FALSE(IsWindowsOpenXRAbiDll("shlwapi.dll:stream"));
        }
        SECTION("A descriptor name must point to terminated initialized file-backed bytes") {
            fixture.Word(PeImportFixture::Raw + 32, PeImportFixture::Address + PeImportFixture::SectionSize - 1);
            fixture.bytes.back() = std::byte{0x78};
        }
        SECTION("The table cannot conceal additional live bytes in its terminator") {
            fixture.Word(PeImportFixture::Raw + 40 + 16, PeImportFixture::Address + 0x140);
        }
        const auto rejected = fixture.Verify();
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == XR::XRErrors::RuntimeRejected.code.Value());
    }

    TEST_CASE("Windows OpenXR preflight rejects unsupported import mechanisms and ABI shapes", "[xr][windows-loader][pe]") {
        PeImportFixture fixture;
        const auto *expected = &XR::XRErrors::OperationUnsupported;
        SECTION("delay load") {
            fixture.Word(PeImportFixture::Directories + 13 * 8, PeImportFixture::Address);
        }
        SECTION("bound imports") {
            fixture.Word(PeImportFixture::Directories + 11 * 8 + 4, 8);
        }
        SECTION("managed image") {
            fixture.Word(PeImportFixture::Directories + 14 * 8, PeImportFixture::Address);
        }
        SECTION("host ABI is not x64") {
            const auto rejected = VerifyWindowsOpenXRLoaderImage(fixture.bytes, "arm64", fixture.signedImports);
            REQUIRE(rejected.HasError());
            CHECK(rejected.ErrorValue().code.Value() == XR::XRErrors::OperationUnsupported.code.Value());
            return;
        }
        SECTION("foreign COFF machine") {
            fixture.Word(PeImportFixture::Pe + 4, 0xaa64, 2);
            expected = &XR::XRErrors::LoaderIncompatible;
        }
        SECTION("aliased section ownership") {
            fixture.Word(PeImportFixture::Pe + 6, 2, 2);
            fixture.Word(PeImportFixture::Sections + 40 + 8, 4);
            fixture.Word(PeImportFixture::Sections + 40 + 12, PeImportFixture::Address + 0x80);
            expected = &XR::XRErrors::LoaderIncompatible;
        }
        const auto rejected = fixture.Verify();
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == expected->code.Value());
    }

    TEST_CASE("Windows OpenXR import preflight bounds truncated and over-capacity metadata", "[xr][windows-loader][pe]") {
        PeImportFixture fixture;
        const auto image = std::span{fixture.bytes};
        for (std::size_t size = 0; size < image.size(); ++size) {
            CAPTURE(size);
            CHECK(VerifyWindowsOpenXRLoaderImage(image.first(size), "x64", fixture.signedImports).HasError());
        }
        const std::array<std::string, MaximumOpenXRAbiDependencies + 1> excessive{};
        CHECK(VerifyWindowsOpenXRLoaderImage(image, "x64", excessive).HasError());
    }
}  // namespace Horo::Application::Internal
