#include "AllocationProbe.h"
#include "ProcessImageIdentity.h"
#include "ProcessImageWindowsInternal.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace Horo::Platform {
    namespace {
        struct Handle final {
            HANDLE value{};
            Handle() = default;
            Handle(const Handle &) = delete;
            Handle &operator=(const Handle &) = delete;
            Handle(Handle &&) = delete;
            Handle &operator=(Handle &&) = delete;

            ~Handle() {
                Reset();
            }

            void Reset() noexcept {
                if (value && value != INVALID_HANDLE_VALUE)
                    CloseHandle(value);
                value = nullptr;
            }
        };

        struct View final {
            void *value{};
            View() = default;
            View(const View &) = delete;
            View &operator=(const View &) = delete;
            View(View &&) = delete;
            View &operator=(View &&) = delete;

            ~View() {
                if (value)
                    UnmapViewOfFile(value);
            }
        };

        /** @brief Locates this test image only; the provider independently verifies the backing file. */
        std::filesystem::path MainFile() {
            std::array<wchar_t, 32768> path{};
            const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            REQUIRE(length > 0);
            REQUIRE(length < path.size());
            return path.data();
        }

        /** @brief Counts genuine process handles after all scoped API users have settled. */
        DWORD HandleCount() {
            DWORD count{};
            REQUIRE(GetProcessHandleCount(GetCurrentProcess(), &count));
            return count;
        }

        /** @brief Supplies an actual executable address emitted into this test's main image. */
        int MainImageMarker() noexcept {
            return 0x53494D47;
        }

        struct FileView final {
            Handle file;
            Handle mapping;
            View view;
            FileView(const FileView &) = delete;
            FileView &operator=(const FileView &) = delete;
            FileView(FileView &&) = delete;
            FileView &operator=(FileView &&) = delete;
            ~FileView() = default;

            explicit FileView(const std::filesystem::path &path, bool writable = false) {
                file.value = CreateFileW(path.c_str(), writable ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ, nullptr,
                                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                REQUIRE(file.value != INVALID_HANDLE_VALUE);
                mapping.value = CreateFileMappingW(file.value, nullptr, writable ? PAGE_READWRITE : PAGE_READONLY, 0, 0, nullptr);
                REQUIRE(mapping.value);
                view.value = MapViewOfFile(mapping.value, writable ? FILE_MAP_READ | FILE_MAP_WRITE : FILE_MAP_READ, 0, 0, 0);
                REQUIRE(view.value);
            }
        };

        /** @brief Owns one exclusive sibling directory on the image volume; cleanup removes only fixed owned paths. */
        struct CopyFixture final {
            std::filesystem::path root;
            CopyFixture(const CopyFixture &) = delete;
            CopyFixture &operator=(const CopyFixture &) = delete;
            CopyFixture(CopyFixture &&) = delete;
            CopyFixture &operator=(CopyFixture &&) = delete;

            CopyFixture() {
                const auto parent = MainFile().parent_path();
                const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
                for (unsigned attempt = 0; attempt < 64; ++attempt) {
                    root = parent / (L"horo image identity \u00e9-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                     std::to_wstring(nonce) + L"-" + std::to_wstring(attempt));
                    if (std::filesystem::create_directory(root))
                        return;
                }
                FAIL("Cannot exclusively create image identity fixture directory");
            }

            ~CopyFixture() {
                std::error_code error;
                std::filesystem::remove(root / L"identical.bin", error);
                std::filesystem::remove(root / L"image.exe", error);
                std::filesystem::remove(root / L"renamed.exe", error);
                std::filesystem::remove(root, error);
            }

            std::filesystem::path Copy() const {
                return root / L"identical.bin";
            }
        };

        /** @brief Restricts inheritance to the two synchronization events, including on assertion unwinding. */
        struct Attributes final {
            std::vector<std::byte> storage;
            LPPROC_THREAD_ATTRIBUTE_LIST value{};
            Attributes(const Attributes &) = delete;
            Attributes &operator=(const Attributes &) = delete;
            Attributes(Attributes &&) = delete;
            Attributes &operator=(Attributes &&) = delete;

            explicit Attributes(std::array<HANDLE, 2> &events) : storage(std::size_t{0}) {
                SIZE_T bytes{};
                InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
                REQUIRE(bytes > 0);
                storage.resize(bytes);
                auto *list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
                REQUIRE(InitializeProcThreadAttributeList(list, 1, 0, &bytes));
                value = list;
                if (!UpdateProcThreadAttribute(value, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, events.data(), sizeof(events), nullptr,
                                               nullptr)) {
                    DeleteProcThreadAttributeList(value);
                    value = nullptr;
                    FAIL("Cannot restrict child handle inheritance");
                }
            }

            ~Attributes() {
                if (value)
                    DeleteProcThreadAttributeList(value);
            }
        };

        /** @brief Owns only a freshly created process handle; timeout cleanup never identifies processes by PID. */
        struct Process final {
            Handle handle;
            Process() = default;
            Process(const Process &) = delete;
            Process &operator=(const Process &) = delete;
            Process(Process &&) = delete;
            Process &operator=(Process &&) = delete;

            ~Process() {
                if (handle.value && WaitForSingleObject(handle.value, 0) == WAIT_TIMEOUT) {
                    TerminateProcess(handle.value, 99);
                    WaitForSingleObject(handle.value, 10000);
                }
            }
        };

        /** @brief Executes the reviewed helper without a console, shell or arbitrary inherited handles. */
        struct Child final {
            Handle ready;
            Handle resume;
            Process process;
            Child(const Child &) = delete;
            Child &operator=(const Child &) = delete;
            Child(Child &&) = delete;
            Child &operator=(Child &&) = delete;
            ~Child() = default;

            Child(const std::filesystem::path &image, const wchar_t *mode) {
                SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
                ready.value = CreateEventW(&security, TRUE, FALSE, nullptr);
                resume.value = CreateEventW(&security, TRUE, FALSE, nullptr);
                REQUIRE(ready.value);
                REQUIRE(resume.value);
                std::array<HANDLE, 2> events{ready.value, resume.value};
                const Attributes attributes(events);
                STARTUPINFOEXW startup{};
                startup.StartupInfo.cb = sizeof(startup);
                startup.lpAttributeList = attributes.value;
                auto command = L"\"" + image.native() + L"\" " + mode + L" " +
                               std::to_wstring(reinterpret_cast<std::uintptr_t>(ready.value)) + L" " +
                               std::to_wstring(reinterpret_cast<std::uintptr_t>(resume.value));
                PROCESS_INFORMATION created{};
                const bool started =
                    CreateProcessW(image.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                                   nullptr, image.parent_path().c_str(), &startup.StartupInfo, &created);
                process.handle.value = created.hProcess;
                Handle thread;
                thread.value = created.hThread;
                REQUIRE(started);
            }

            void AwaitReady() const {
                const std::array<HANDLE, 2> waits{ready.value, process.handle.value};
                REQUIRE(WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), FALSE, 10000) == WAIT_OBJECT_0);
            }

            DWORD Finish() const {
                REQUIRE(SetEvent(resume.value));
                REQUIRE(WaitForSingleObject(process.handle.value, 10000) == WAIT_OBJECT_0);
                DWORD status{};
                REQUIRE(GetExitCodeProcess(process.handle.value, &status));
                return status;
            }
        };

        /** @brief Replaces only the owned child pathname; its live mapped image remains the renamed original. */
        void Replacement(bool retained) {
            const CopyFixture fixture;
            const auto image = fixture.root / L"image.exe";
            const auto renamed = fixture.root / L"renamed.exe";
            const std::filesystem::path helper(HORO_PROCESS_IMAGE_CHILD_PATH);
            REQUIRE(std::filesystem::copy_file(helper, image));
            const Child child(image, retained ? L"retained" : L"before");
            child.AwaitReady();
            const bool moved = MoveFileExW(image.c_str(), renamed.c_str(), 0);
            if (const auto error = GetLastError(); retained) {
                REQUIRE_FALSE(moved);
                REQUIRE((error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED));
            } else {
                REQUIRE(moved);
                REQUIRE(std::filesystem::copy_file(helper, image));
            }
            const auto status = child.Finish();
            std::printf("owned image replacement: retained=%d renamed=%d child-status=%lu\n", retained, moved, status);
            REQUIRE(status == (retained ? 0U : 7U));
        }
    }  // namespace

    TEST_CASE("Windows process image retains actual kernel-backed complete-file identity", "[unit][platform][process-image]") {
        auto identity = CaptureCurrentProcessImageIdentity();
        INFO((identity.HasError() ? identity.ErrorValue().message : std::string{"captured"}));
        REQUIRE(identity.HasValue());
        REQUIRE(identity.Value().Verify().HasValue());
        REQUIRE(identity.Value().VerifyStaticCodeAddress(reinterpret_cast<const void *>(&MainImageMarker)).HasValue());
        std::ifstream stream(MainFile(), std::ios::binary);
        REQUIRE(stream);
        const std::string bytes{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
        REQUIRE_FALSE(bytes.empty());
        REQUIRE(identity.Value().Artifact().bytes == ComputeSha256(std::as_bytes(std::span(bytes))).bytes);
    }

    TEST_CASE("Windows process image rejects null heap image-data and foreign DLL code addresses", "[unit][platform][process-image]") {
        auto identity = CaptureCurrentProcessImageIdentity();
        REQUIRE(identity.HasValue());
        const int heapOrStack{};
        static const int imageData = 77;
        REQUIRE(identity.Value().VerifyStaticCodeAddress(nullptr).HasError());
        REQUIRE(identity.Value().VerifyStaticCodeAddress(&heapOrStack).HasError());
        REQUIRE(identity.Value().VerifyStaticCodeAddress(&imageData).HasError());
        REQUIRE(identity.Value().VerifyStaticCodeAddress(GetModuleHandleW(nullptr)).HasError());
        const auto foreign = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetCurrentProcessId");
        REQUIRE(foreign);
        REQUIRE(identity.Value().VerifyStaticCodeAddress(reinterpret_cast<const void *>(foreign)).HasError());
        REQUIRE(identity.Value().Verify().HasValue());
    }

    TEST_CASE("Windows main image file comparison rejects byte-identical copies in spaced Unicode paths",
              "[unit][platform][process-image]") {
        const CopyFixture fixture;
        REQUIRE(std::filesystem::copy_file(MainFile(), fixture.Copy()));
        const FileView original(MainFile());
        REQUIRE(Detail::VerifyWindowsMainImageFileView(original.view.value).HasValue());
        const FileView copied(fixture.Copy());
        REQUIRE(Detail::VerifyWindowsMainImageFileView(copied.view.value).HasError());
        REQUIRE(Detail::VerifyWindowsMainImageFileView(original.view.value).HasValue());
    }

    TEST_CASE("Windows main image file comparison rejects writable foreign and anonymous views", "[unit][platform][process-image]") {
        const CopyFixture fixture;
        REQUIRE(std::filesystem::copy_file(MainFile(), fixture.Copy()));
        const FileView writable(fixture.Copy(), true);
        REQUIRE(Detail::VerifyWindowsMainImageFileView(writable.view.value).HasError());
        const int value{};
        REQUIRE(Detail::VerifyWindowsMainImageFileView(&value).HasError());
        REQUIRE(Detail::VerifyWindowsMainImageFileView(nullptr).HasError());
        REQUIRE(Detail::VerifyWindowsMainImageFileView(GetModuleHandleW(nullptr)).HasError());
        const auto foreign = GetModuleHandleW(L"kernel32.dll");
        REQUIRE(foreign);
        REQUIRE(Detail::VerifyWindowsMainImageFileView(foreign).HasError());
    }

    TEST_CASE("Windows retained image denies write and delete opens without a permission change", "[unit][platform][process-image]") {
        auto identity = CaptureCurrentProcessImageIdentity();
        REQUIRE(identity.HasValue());
        const auto path = MainFile();
        Handle write;
        write.value = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(write.value == INVALID_HANDLE_VALUE);
        Handle remove;
        remove.value = CreateFileW(path.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(remove.value == INVALID_HANDLE_VALUE);
        REQUIRE(identity.Value().Verify().HasValue());
    }

    TEST_CASE("Windows process image leases retain native handles through foreign last-drop", "[unit][platform][process-image]") {
        {
            REQUIRE(CaptureCurrentProcessImageIdentity().HasValue());
        }
        const auto before = HandleCount();
        std::shared_ptr<const void> lease;
        std::weak_ptr<const void> observed;
        {
            auto identity = CaptureCurrentProcessImageIdentity();
            REQUIRE(identity.HasValue());
            lease = identity.Value().Lease();
            observed = lease;
            REQUIRE(HandleCount() >= before + 2);
        }
        REQUIRE_FALSE(observed.expired());
        REQUIRE(HandleCount() >= before + 2);
        std::thread releaser([owned = std::move(lease)]() mutable {
            owned.reset();
        });
        releaser.join();
        REQUIRE(observed.expired());
        REQUIRE(HandleCount() == before);
    }

    TEST_CASE("Windows process image allocation failure issues no authority and leaks no native handles",
              "[unit][platform][process-image]") {
        const auto before = HandleCount();
        auto failed = [] {
            Tests::AllocationProbe::ScopedFailure failure;
            return CaptureCurrentProcessImageIdentity();
        }();
        REQUIRE(failed.HasError());
        REQUIRE(HandleCount() == before);
        REQUIRE(CaptureCurrentProcessImageIdentity().HasValue());
    }

    TEST_CASE("Windows process image rejects an actual byte-identical pathname replacement before capture without handle leaks",
              "[unit][platform][process-image]") {
        // Settle Windows' first process-start initialization; repeated owned operations must then return to this exact baseline.
        Replacement(true);
        const auto before = HandleCount();
        Replacement(false);
        REQUIRE(HandleCount() == before);
    }

    TEST_CASE("Windows process image retention blocks actual child pathname replacement through verification",
              "[unit][platform][process-image]") {
        Replacement(true);
        const auto before = HandleCount();
        Replacement(true);
        REQUIRE(HandleCount() == before);
    }
}  // namespace Horo::Platform
