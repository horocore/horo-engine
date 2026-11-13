#include "AllocationProbe.h"
#include "Horo/Foundation/PathUtils.h"
#include "Horo/Platform/DynamicLibrary.h"
#include "OwnedTestDirectory.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
    using namespace Horo;
    namespace Probe = Tests::AllocationProbe;
    constexpr std::string_view Symbol{"Horo_Platform_DynamicLibrary_Allocation_Probe"};
    static_assert(Symbol.size() > 32);

    /** @brief Keeps an intentional fatal regression noninteractive without changing its abort signal or allocator. */
    class ScopedHeadlessAbort final {
    public:
        ScopedHeadlessAbort()
#if defined(_WIN32)
            : previous_(_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT))
#endif
        {
        }

        ~ScopedHeadlessAbort() {
#if defined(_WIN32)
            _set_abort_behavior(previous_, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
        }

        ScopedHeadlessAbort(const ScopedHeadlessAbort &) = delete;
        ScopedHeadlessAbort &operator=(const ScopedHeadlessAbort &) = delete;
        ScopedHeadlessAbort(ScopedHeadlessAbort &&) = delete;
        ScopedHeadlessAbort &operator=(ScopedHeadlessAbort &&) = delete;

    private:
#if defined(_WIN32)
        unsigned previous_{};
#endif
    };

    /** @brief Owns a distinct inert C image, never a previously loaded test module. */
    struct Image final {
        Tests::OwnedTestDirectory directory{"dl-probe"};
        std::filesystem::path path;

        Image() {
            const std::filesystem::path original{HORO_DYNAMIC_LIBRARY_FIXTURE_PATH};
            auto name = directory.Path().filename();
            name += original.extension();
            path = directory.Path() / name;
            REQUIRE(std::filesystem::copy_file(Foundation::Paths::NativeApiPath(original), Foundation::Paths::NativeApiPath(path)));
        }

        Image(const Image &) = delete;
        Image &operator=(const Image &) = delete;
        Image(Image &&) = delete;
        Image &operator=(Image &&) = delete;
    };

    /** @brief Observes actual native residency without retaining an extra image reference. */
    bool Loaded(const std::filesystem::path &path) {
#if defined(_WIN32)
        const auto name = path.filename();
        return GetModuleHandleW(name.c_str()) != nullptr;
#else
        void *const observed = dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD);
        if (!observed)
            return false;
        dlclose(observed);
        return true;
#endif
    }

    /** @brief Opens before measurement and keeps test/framework allocations outside every fault window. */
    std::unique_ptr<Platform::DynamicLibrary> Open(const Image &image) {
        auto opened = Platform::LoadDynamicLibraryAtPath(image.path);
        REQUIRE(opened.HasValue());
        return std::move(opened).Value();
    }

    /** @brief Measures C++ handoff allocations only after the actual native image is resident.
     * Path validation happens before acquisition and may use implementation-defined noexcept STL helpers.
     * Neither the residency query nor this observer allocates C++ storage.
     */
    struct HandoffAllocations final {
        const std::filesystem::path::value_type *imageKey{};
        std::size_t requests{};
        std::size_t residentRequests{};
        std::vector<std::size_t> *indices{};
        bool overflow{};

        static void Observe(std::size_t, void *context) noexcept {
            auto &self = *static_cast<HandoffAllocations *>(context);
#if defined(_WIN32)
            const bool resident = GetModuleHandleW(self.imageKey) != nullptr;
#else
            void *const image = dlopen(self.imageKey, RTLD_NOW | RTLD_NOLOAD);
            const bool resident = image != nullptr;
            if (image)
                dlclose(image);
#endif
            if (resident) {
                if (self.indices) {
                    if (self.residentRequests < self.indices->size())
                        (*self.indices)[self.residentRequests] = self.requests;
                    else
                        self.overflow = true;
                }
                ++self.residentRequests;
            }
            ++self.requests;
        }
    };

    thread_local bool injected{};

    /** @brief Records the actual one-shot fault after injection has already been disabled. */
    void Injection(std::size_t) noexcept {
        injected = true;
    }
}  // namespace

TEST_CASE("Native library admission releases its actual image at every handoff allocation failure", "[platform][native][allocation]") {
    ScopedHeadlessAbort headless;
    Image image;
    auto warm = Open(image);
    REQUIRE(Loaded(image.path));
    warm.reset();
    REQUIRE_FALSE(Loaded(image.path));

    std::unique_ptr<Platform::DynamicLibrary> retained;
    bool admitted{};
#if defined(_WIN32)
    const auto imageKey = image.path.filename().native();
#else
    const auto &imageKey = image.path.native();
#endif
    HandoffAllocations measured{imageKey.c_str()};
    {
        Probe::ScopedObserver observation{HandoffAllocations::Observe, &measured};
        auto opened = Platform::LoadDynamicLibraryAtPath(image.path);
        admitted = opened.HasValue();
        if (admitted)
            retained = std::move(opened).Value();
    }
    REQUIRE(admitted);
    REQUIRE(measured.requests > 0);
    REQUIRE(Loaded(image.path));
    retained.reset();
    REQUIRE_FALSE(Loaded(image.path));

    // Preallocate the complete measured handoff index set outside the observation window.
    std::vector<std::size_t> indices(measured.residentRequests);
    HandoffAllocations recorded{imageKey.c_str(), 0, 0, &indices};
    admitted = false;
    {
        Probe::ScopedObserver observation{HandoffAllocations::Observe, &recorded};
        auto opened = Platform::LoadDynamicLibraryAtPath(image.path);
        admitted = opened.HasValue();
        if (admitted)
            retained = std::move(opened).Value();
    }
    REQUIRE(admitted);
    REQUIRE_FALSE(recorded.overflow);
    REQUIRE(recorded.residentRequests == indices.size());
    REQUIRE(Loaded(image.path));
    retained.reset();
    REQUIRE_FALSE(Loaded(image.path));

    // Zero handoff requests is valid: the owner can be allocated before native acquisition.
    for (const auto index : indices) {
        std::fprintf(stderr, "native-admission-handoff-iteration %zu/%zu\n", index, recorded.requests);
        std::fflush(stderr);
        injected = false;
        try {
            Probe::ScopedFailure failure{index, Injection};
            auto opened = Platform::LoadDynamicLibraryAtPath(image.path);
            if (opened.HasValue())
                retained = std::move(opened).Value();
        } catch (const std::bad_alloc &) {
            // Allocation is permitted to fail; a loaded native reference must still have an owner.
        }
        retained.reset();
        CAPTURE(index, recorded.requests, recorded.residentRequests);
        REQUIRE(injected);
        std::fputs("native-admission-injected-residency-check\n", stderr);
        std::fflush(stderr);
        REQUIRE_FALSE(Loaded(image.path));
    }
}

TEST_CASE("Native symbol lookup contains allocation failure without terminating", "[platform][native][allocation]") {
    ScopedHeadlessAbort headless;
    Image image;
    auto library = Open(image);
    const std::string surrounded = "[" + std::string{Symbol} + "]";
    const std::string_view name{surrounded.data() + 1, Symbol.size()};
    void *const expected = library->GetSymbol(name);
    REQUIRE(expected != nullptr);

    std::size_t requests{};
    void *measured{};
    {
        Probe::ScopedMeasurement measurement;
        measured = library->GetSymbol(name);
        requests = measurement.Snapshot().requests;
    }
    REQUIRE(measured == expected);
    for (std::size_t index = 0; index < requests; ++index) {
        injected = false;
        void *symbol{};
        std::fputs("native-symbol-allocation-fault-call\n", stderr);
        std::fflush(stderr);
        {
            Probe::ScopedFailure failure{index, Injection};
            symbol = library->GetSymbol(name);
        }
        CAPTURE(index, requests);
        REQUIRE(injected);
        CHECK(symbol == nullptr);
        CHECK(Loaded(image.path));
        CHECK(library->GetSymbol(name) == expected);
    }
}

TEST_CASE("Native symbol lookup requires the complete exact exported name", "[platform][native][symbols]") {
    Image image;
    auto library = Open(image);
    const std::string surrounded = "[" + std::string{Symbol} + "]";
    const std::string_view name{surrounded.data() + 1, Symbol.size()};
    auto *const address = library->GetSymbol(name);
    REQUIRE(address != nullptr);
    CHECK(library->GetSymbol({}) == nullptr);
    std::string embedded{Symbol};
    embedded += '\0';
    embedded += "_different_symbol";
    std::fputs("native-exact-name-embedded-nul-check\n", stderr);
    std::fflush(stderr);
    CHECK(library->GetSymbol(embedded) == nullptr);
    CHECK(library->GetSymbol("Horo_Platform_DynamicLibrary_Allocation_Probe_Missing") == nullptr);
}
