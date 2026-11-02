#include "extensions/host/NativeExtensionServiceInternal.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::Extensions::Tests {
    namespace {
        HoroExtensionStatus NeverInvoked(void *, const HoroExtensionServiceRequest *, HoroExtensionServiceResult *) {
            FAIL("shape validation invoked provider code");
            return HORO_EXTENSION_ERROR_INIT_FAILED;
        }

        void NeverShutdown(void *) {
            FAIL("shape validation invoked shutdown");
        }

        HoroExtensionStringView Text(const std::string_view text) {
            return {text.data(), static_cast<std::uint32_t>(text.size())};
        }

        HoroExtensionServiceDescriptor Descriptor(const std::span<const HoroExtensionServiceMethod> methods) {
            return {.structSize = sizeof(HoroExtensionServiceDescriptor),
                    .schemaVersion = 1,
                    .service = Text("com.example.arithmetic"),
                    .contract = Text("horo.examples.checked-arithmetic"),
                    .version = Text("1.0.0"),
                    .methods = methods.data(),
                    .methodCount = static_cast<std::uint32_t>(methods.size()),
                    .invoke = NeverInvoked,
                    .shutdown = NeverShutdown};
        }
    }  // namespace

    TEST_CASE("Native scalar descriptor validates the exact closed shape without invoking module callbacks", "[extensions][sdk][abi]") {
        const std::array methods{HoroExtensionServiceMethod{Text("add"), 2, 0}};
        auto descriptor = Descriptor(methods);
        auto shape = CopyNativeServiceDescriptor(descriptor);
        REQUIRE(shape.HasValue());
        CHECK(shape.Value().methods.front().arguments == 2);
        ExtensionModuleManifest module{.id = "com.example.backend",
                                       .version = "1.0.0",
                                       .exports = {{"com.example.arithmetic", "horo.examples.checked-arithmetic", "1.0.0"}}};
        CHECK(CopyNativeServiceShape(descriptor, module).HasValue());
        module.exports.front().contract = "com.example.foreign";
        CHECK(CopyNativeServiceShape(descriptor, module).HasError());
    }

    TEST_CASE("Native scalar descriptor rejects reserved layout and unbounded counts before touching method memory",
              "[extensions][sdk][abi]") {
        const std::array methods{HoroExtensionServiceMethod{Text("add"), 2, 0}};
        auto descriptor = Descriptor(methods);
        SECTION("reserved") {
            descriptor.reserved = 1;
        }
        SECTION("unknown schema") {
            descriptor.schemaVersion = 2;
        }
        SECTION("truncated prefix") {
            --descriptor.structSize;
        }
        SECTION("unknown trailing fields") {
            ++descriptor.structSize;
        }
        SECTION("null methods") {
            descriptor.methods = nullptr;
        }
        SECTION("too many methods") {
            descriptor.methodCount = std::numeric_limits<std::uint32_t>::max();
        }
        CHECK(CopyNativeServiceDescriptor(descriptor).HasError());
    }

    TEST_CASE("Native scalar descriptor rejects duplicate foreign and narrowing-prone method signatures", "[extensions][sdk][abi]") {
        std::array methods{HoroExtensionServiceMethod{Text("add"), 2, 0}, HoroExtensionServiceMethod{Text("add"), 2, 0}};
        auto descriptor = Descriptor(methods);
        SECTION("duplicate identities") {}
        SECTION("argument overflow") {
            methods[1] = {Text("other"), 256, 0};
        }
        SECTION("method reserved") {
            methods[1] = {Text("other"), 2, 1};
        }
        SECTION("noncanonical method") {
            methods[1] = {Text("other method"), 2, 0};
        }
        SECTION("version overflow") {
            descriptor.version = Text("65536.0.0");
        }
        SECTION("noncanonical numeric version") {
            descriptor.version = Text("01.0.0");
        }
        CHECK(CopyNativeServiceDescriptor(descriptor).HasError());
    }
}  // namespace Horo::Extensions::Tests
