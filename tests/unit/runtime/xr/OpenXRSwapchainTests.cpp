#include "OpenXRSwapchain.h"
#include "support/OpenXRSwapchainTestSupport.h"

#include <algorithm>
#include <bit>
#include <thread>

namespace Horo::Tests::OpenXR {
    namespace {
        /** @brief Tests only the native owner's handoff obligations, not concrete renderer completion. */
        class ImagePort final : public IOpenXRSwapchainImages {
        public:
            bool complete{};
            bool rejectImport{};
            bool rejectRetire{};
            bool stale{};
            bool rejectEnumeration{};
            bool selectAbsentFormat{};
            std::uint32_t imports{};
            std::uint32_t retirements{};

            Result<void> Validate(const NativeSwapchainRequest &) const override {
                return stale ? Result<void>::Failure(MakeError(XRErrors::CapabilityStale)) : Result<void>::Success();
            }

            Result<std::int64_t> SelectFormat(const NativeSwapchainRequest &, std::span<const std::int64_t>) const override {
                return Result<std::int64_t>::Success(selectAbsentFormat ? 99 : 37);
            }

            Result<void> Enumerate(const NativeSwapchainRequest &, std::uint32_t count,
                                   const IOpenXRSwapchainImageEnumeration &enumeration) override {
                CHECK(enumeration.IsAvailable());
                std::uint32_t written{};
                CHECK(enumeration.Enumerate(0, &written, nullptr) == XR_SUCCESS);
                CHECK(written == count);
                return rejectEnumeration ? Result<void>::Failure(MakeError(XRErrors::OperationUnavailable)) : Result<void>::Success();
            }

            Result<Render::RenderTextureHandle> Import(const NativeSwapchainRequest &, const XRSwapchainImageId &image,
                                                       std::uint32_t index) override {
                CHECK(image.slot.index == index);
                if (rejectImport)
                    return Result<Render::RenderTextureHandle>::Failure(MakeError(XRErrors::OperationUnavailable));
                ++imports;
                return Result<Render::RenderTextureHandle>::Success({{1}, 1, imports});
            }

            Result<void> Retire(Render::RenderTextureHandle) override {
                ++retirements;
                return rejectRetire ? Result<void>::Failure(MakeError(XRErrors::OperationUnavailable)) : Result<void>::Success();
            }

            Result<bool> RetirementComplete(Render::RenderTextureHandle) override {
                return Result<bool>::Success(complete);
            }

            Result<void> Reset() override {
                return Result<void>::Success();
            }
        };

        struct SwapchainFixture final {
            Fixture session;
            SwapchainScript script;
            ImagePort images;
            OpenXRSwapchain owner{session.owner, images};
            NativeSwapchainRequest request{.target = {session.Request().candidate, {.index = 1, .generation = 1}},
                                           .texture = {.extent = {800, 600},
                                                       .format = Render::RenderTextureFormat::Rgba8Unorm,
                                                       .layerCount = 2,
                                                       .usage = Render::RenderTextureUsage::RenderAttachment}};

            SwapchainFixture() {
                REQUIRE(session.owner.Create(session.Request()).HasValue());
            }

            ~SwapchainFixture() {
                script.fail = {};
                script.timeout = false;
                images.complete = true;
                images.rejectRetire = false;
                CHECK(owner.Close().HasValue());
            }

            void Create() {
                REQUIRE(owner.Create(request, &SwapchainScript::GetProc).HasValue());
            }

            NativeSwapchainAcquisition Import() {
                REQUIRE(owner.Acquire().HasValue());
                const auto waited = owner.Wait(0);
                REQUIRE(waited.HasValue());
                REQUIRE(waited.Value());
                const auto imported = owner.Import();
                REQUIRE(imported.HasValue());
                return imported.Value();
            }
        };
    }  // namespace

    TEST_CASE("OpenXR compositor borrow requires latest completed release and fences replacement", "[unit][xr][native][swapchain]") {
        SwapchainFixture f;
        f.Create();
        const auto acquired = f.Import();
        REQUIRE_FALSE(f.owner.BorrowReleased(acquired.image).HasValue());
        REQUIRE_FALSE(f.owner.PollRelease().Value());
        REQUIRE_FALSE(f.owner.BorrowReleased(acquired.image).HasValue());
        f.images.complete = true;
        REQUIRE(f.owner.PollRelease().Value());
        REQUIRE(f.owner.BorrowReleased(acquired.image).HasValue());
        REQUIRE(f.owner.Close().Value());
        REQUIRE_FALSE(f.owner.BorrowReleased(acquired.image).HasValue());
        ++f.request.target.slot.generation;
        f.Create();
        REQUIRE_FALSE(f.owner.BorrowReleased(acquired.image).HasValue());
    }

    TEST_CASE("OpenXR enumeration context forwards captured state and exact native output", "[unit][xr][native][swapchain]") {
        const auto native = NativeHandle<XrSwapchain>(1);
        std::uint32_t calls{};
        XrSwapchain observed{XR_NULL_HANDLE};
        XrSwapchainImageBaseHeader output{XR_TYPE_UNKNOWN};
        auto enumerate = [&](XrSwapchain swapchain, std::uint32_t capacity, std::uint32_t *written,
                             XrSwapchainImageBaseHeader *images) -> XrResult {
            ++calls;
            observed = swapchain;
            CHECK(capacity == 1);
            REQUIRE(images == &output);
            CHECK(images->type == XR_TYPE_UNKNOWN);
            *written = 1;
            return XR_ERROR_RUNTIME_FAILURE;
        };
        OpenXRSwapchainImageEnumeration context{native, enumerate};
        const IOpenXRSwapchainImageEnumeration &port = context;
        CHECK(port.IsAvailable());
        std::uint32_t written{};
        CHECK(port.Enumerate(1, &written, &output) == XR_ERROR_RUNTIME_FAILURE);
        CHECK(calls == 1);
        CHECK(observed == native);
        CHECK(written == 1);
    }

    TEST_CASE("OpenXR enumeration context rejects typed null and null literal", "[unit][xr][native][swapchain]") {
        PFN_xrEnumerateSwapchainImages nullFunction{};
        OpenXRSwapchainImageEnumeration typed{XR_NULL_HANDLE, nullFunction};
        auto nullLiteral = nullptr;
        OpenXRSwapchainImageEnumeration literal{XR_NULL_HANDLE, nullLiteral};
        std::uint32_t written = 17;
        CHECK_FALSE(typed.IsAvailable());
        CHECK_FALSE(literal.IsAvailable());
        CHECK(typed.Enumerate(0, &written, nullptr) == XR_ERROR_FUNCTION_UNSUPPORTED);
        CHECK(literal.Enumerate(0, &written, nullptr) == XR_ERROR_FUNCTION_UNSUPPORTED);
        CHECK(written == 17);
        std::uint32_t calls{};
        auto enumerate = [&](XrSwapchain, std::uint32_t, std::uint32_t *, XrSwapchainImageBaseHeader *) -> XrResult {
            ++calls;
            return XR_SUCCESS;
        };
        OpenXRSwapchainImageEnumeration invalid{XR_NULL_HANDLE, enumerate};
        CHECK_FALSE(invalid.IsAvailable());
        CHECK(invalid.Enumerate(0, &written, nullptr) == XR_ERROR_FUNCTION_UNSUPPORTED);
        CHECK(calls == 0);
    }

    TEST_CASE("OpenXR swapchain creation accepts a capturing resolver", "[unit][xr][native][swapchain]") {
        SwapchainFixture fixture;
        std::uint32_t calls{};
        auto resolver = [&](XrInstance instance, const char *name, PFN_xrVoidFunction *output) -> XrResult {
            ++calls;
            return SwapchainScript::GetProc(instance, name, output);
        };
        REQUIRE(fixture.owner.Create(fixture.request, resolver).HasValue());
        CHECK(calls == 7);
    }

    TEST_CASE("OpenXR swapchain creation rejects null resolver forms before dispatch", "[unit][xr][native][swapchain]") {
        SwapchainFixture fixture;
        PFN_xrGetInstanceProcAddr nullResolver{};
        CHECK(fixture.owner.Create(fixture.request, nullptr).HasError());
        CHECK(fixture.owner.Create(fixture.request, nullResolver).HasError());
        CHECK(fixture.script.calls.empty());
        CHECK(fixture.script.creates == 0);
    }

    TEST_CASE("OpenXR swapchain waits imports retires and releases in native order", "[unit][xr][native][swapchain]") {
        SwapchainFixture fixture;
        fixture.Create();
        CHECK(fixture.script.created.width == 800);
        CHECK(fixture.script.created.height == 600);
        CHECK(fixture.script.created.arraySize == 2);
        CHECK(fixture.script.created.usageFlags == XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT);
        const auto imported = fixture.Import();
        CHECK(imported.image.target == fixture.request.target);
        CHECK(fixture.owner.Current().HasValue());
        const auto pending = fixture.owner.PollRelease();
        REQUIRE(pending.HasValue());
        CHECK_FALSE(pending.Value());
        CHECK(fixture.owner.Current().HasError());
        CHECK(fixture.script.releases == 0);
        CHECK(fixture.owner.Acquire().HasError());
        fixture.images.complete = true;
        const auto released = fixture.owner.PollRelease();
        REQUIRE(released.HasValue());
        CHECK(released.Value());
        CHECK(fixture.images.retirements == 1);
        CHECK(fixture.script.releases == 1);
        CHECK(fixture.owner.PollRelease().HasError());
    }

    TEST_CASE("OpenXR wait timeout retains native ownership and finite retry", "[unit][xr][native][swapchain]") {
        SwapchainFixture fixture;
        fixture.Create();
        REQUIRE(fixture.owner.Acquire().HasValue());
        fixture.script.timeout = true;
        const auto wait = fixture.owner.Wait(1);
        REQUIRE(wait.HasValue());
        CHECK_FALSE(wait.Value());
        CHECK(fixture.owner.Wait(XR_INFINITE_DURATION).HasError());
        CHECK(fixture.owner.Import().HasError());
        CHECK(fixture.owner.PollRelease().HasError());
        const auto close = fixture.owner.Close();
        REQUIRE(close.HasValue());
        CHECK_FALSE(close.Value());
        CHECK(fixture.script.releases == 0);
        CHECK(fixture.script.destroys == 0);
        fixture.script.timeout = false;
        const auto closed = fixture.owner.Close();
        REQUIRE(closed.HasValue());
        CHECK(closed.Value());
        CHECK(fixture.script.releases == 1);
        CHECK(fixture.script.destroys == 1);
    }

    TEST_CASE("OpenXR reacquired native index never reuses the old acquisition identity", "[unit][xr][native][swapchain]") {
        SwapchainFixture fixture;
        fixture.Create();
        const auto first = fixture.Import();
        fixture.images.complete = true;
        REQUIRE(fixture.owner.PollRelease().Value());
        const auto second = fixture.Import();
        CHECK(second.image.slot.index == first.image.slot.index);
        CHECK(second.image.slot.generation > first.image.slot.generation);
        CHECK(second.texture != first.texture);
        CHECK(second.image != first.image);
    }

    TEST_CASE("OpenXR preparation failure rolls back every created swapchain", "[unit][xr][native][swapchain]") {
        SwapchainFixture fixture;
        SECTION("format enumeration") {
            fixture.script.fail = "formats";
        }
        SECTION("creation") {
            fixture.script.fail = "create";
        }
        SECTION("image enumeration") {
            fixture.script.fail = "enumerate";
        }
        SECTION("typed adapter enumeration") {
            fixture.images.rejectEnumeration = true;
        }
        SECTION("zero images") {
            fixture.script.imageCount = 0;
        }
        SECTION("too many images") {
            fixture.script.imageCount = 65;
        }
        SECTION("too many formats") {
            fixture.script.formatCount = 129;
        }
        SECTION("unadvertised selected format") {
            fixture.images.selectAbsentFormat = true;
        }
        SECTION("missing release dispatch") {
            fixture.session.script.absent = "xrReleaseSwapchainImage";
        }
        CHECK(fixture.owner.Create(fixture.request, &SwapchainScript::GetProc).HasError());
        CHECK(fixture.script.creates == fixture.script.destroys);
        CHECK(fixture.images.imports == 0);
    }

    TEST_CASE("OpenXR projection depth requires actual negotiated extension before native creation", "[unit][xr][native][swapchain]") {
        SwapchainFixture fixture;
        fixture.request.role = NativeSwapchainRole::Depth;
        fixture.request.texture.format = Render::RenderTextureFormat::Depth32Float;
        CHECK(fixture.owner.Create(fixture.request, &SwapchainScript::GetProc).HasError());
        CHECK(fixture.script.creates == 0);
    }

    TEST_CASE("OpenXR import failure and invalid native index keep release obligations", "[unit][xr][native][swapchain]") {
        SwapchainFixture fixture;
        fixture.Create();
        SECTION("import fails before renderer submission") {
            fixture.images.rejectImport = true;
            REQUIRE(fixture.owner.Acquire().HasValue());
        }
        SECTION("runtime index is corrupt") {
            fixture.script.imageIndex = 999;
            CHECK(fixture.owner.Acquire().HasError());
        }
        REQUIRE(fixture.owner.Wait(0).Value());
        CHECK(fixture.owner.Import().HasError());
        CHECK(fixture.images.imports == 0);
        REQUIRE(fixture.owner.PollRelease().Value());
        CHECK(fixture.script.releases == 1);
    }

    TEST_CASE("OpenXR render retirement failure revokes publication and remains retryable", "[unit][xr][native][swapchain]") {
        SwapchainFixture fixture;
        fixture.Create();
        fixture.Import();
        fixture.images.rejectRetire = true;
        CHECK(fixture.owner.PollRelease().HasError());
        CHECK(fixture.owner.Current().HasError());
        CHECK(fixture.script.releases == 0);
        fixture.images.rejectRetire = false;
        fixture.images.complete = true;
        REQUIRE(fixture.owner.PollRelease().Value());
        CHECK(fixture.images.retirements == 2);
        CHECK(fixture.script.releases == 1);
    }

    TEST_CASE("OpenXR release or destruction failure retains exact native resources for retry", "[unit][xr][native][swapchain]") {
        SwapchainFixture fixture;
        fixture.Create();
        fixture.Import();
        fixture.images.complete = true;
        SECTION("release") {
            fixture.script.fail = "release";
            CHECK(fixture.owner.Close().HasError());
            CHECK(fixture.script.releases == 0);
            CHECK(fixture.script.destroys == 0);
        }
        SECTION("destroy") {
            fixture.script.fail = "destroy";
            CHECK(fixture.owner.Close().HasError());
            CHECK(fixture.script.releases == 1);
            CHECK(fixture.script.destroys == 0);
        }
        fixture.script.fail = {};
        REQUIRE(fixture.owner.Close().Value());
        CHECK(fixture.script.releases == 1);
        CHECK(fixture.script.destroys == 1);
    }

    TEST_CASE("OpenXR recreation uses a fresh target and keeps old imports pinned during shutdown", "[unit][xr][native][swapchain]") {
        SwapchainFixture fixture;
        fixture.Create();
        fixture.Import();
        const auto pending = fixture.owner.Close();
        REQUIRE(pending.HasValue());
        CHECK_FALSE(pending.Value());
        CHECK(fixture.owner.Current().HasError());
        CHECK(fixture.owner.Create(fixture.request, &SwapchainScript::GetProc).HasError());
        CHECK(fixture.script.destroys == 0);
        fixture.images.complete = true;
        REQUIRE(fixture.owner.Close().Value());
        CHECK(fixture.owner.Create(fixture.request, &SwapchainScript::GetProc).HasError());
        ++fixture.request.target.slot.generation;
        REQUIRE(fixture.owner.Create(fixture.request, &SwapchainScript::GetProc).HasValue());
    }

    TEST_CASE("OpenXR actual native session loss permits destruction without fabricated image release", "[unit][xr][native][swapchain]") {
        SwapchainFixture fixture;
        fixture.Create();
        REQUIRE(fixture.owner.Acquire().HasValue());
        fixture.script.fail = "wait";
        fixture.script.failure = XR_ERROR_SESSION_LOST;
        CHECK(fixture.owner.Wait(0).HasError());
        CHECK(fixture.owner.Import().HasError());
        fixture.script.fail = {};
        REQUIRE(fixture.owner.Close().Value());
        CHECK(fixture.script.releases == 0);
        CHECK(fixture.script.destroys == 1);
    }

    TEST_CASE("OpenXR stale session device and wrong thread cannot publish imported images", "[unit][xr][native][swapchain]") {
        SwapchainFixture fixture;
        fixture.Create();
        fixture.Import();
        SECTION("device admission revoked") {
            fixture.images.stale = true;
            CHECK(fixture.owner.Current().HasError());
        }
        SECTION("session fence revoked") {
            fixture.session.script.stale = true;
            CHECK(fixture.owner.Current().HasError());
        }
        SECTION("wrong thread") {
            bool rejected{};
            std::thread worker([&] {
                rejected = fixture.owner.Acquire().HasError();
            });
            worker.join();
            CHECK(rejected);
        }
        CHECK(fixture.script.releases == 0);
    }
}  // namespace Horo::Tests::OpenXR
