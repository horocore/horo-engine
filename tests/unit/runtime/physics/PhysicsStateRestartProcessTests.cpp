#include "PhysicsSceneStateTestSupport.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Physics::StateTest {
    namespace {
        constexpr std::size_t MaximumWire = 8U * 1024U * 1024U;

        std::vector<std::byte> ReadState(const std::filesystem::path &path) {
            const auto count = std::filesystem::file_size(path);
            REQUIRE(count > 0);
            REQUIRE(count <= MaximumWire);
            std::vector<std::byte> bytes(static_cast<std::size_t>(count));
            std::ifstream input(path, std::ios::binary);
            input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            REQUIRE(input.good());
            CHECK(input.peek() == std::char_traits<char>::eof());
            return bytes;
        }

        void WriteState(const std::filesystem::path &path, std::span<const std::byte> bytes) {
            REQUIRE_FALSE(bytes.empty());
            REQUIRE(bytes.size() <= MaximumWire);
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            output.flush();
            REQUIRE(output.good());
        }

        std::unique_ptr<PhysicsRuntime> BoundRuntime() {
            auto runtime = Take(PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical));
            // Each child independently captures and verifies its OWN actually loaded executable, adapter and Jolt image.
            Check(runtime->BindStateSnapshotBuild(ActualStaticBinding(), StateRuntimeBudget(32, 1024U * 1024U * 1024U)));
            return runtime;
        }

        void CheckNonzeroOriginalIntent(const PhysicsStateSnapshot &checkpoint) {
            CHECK(std::ranges::any_of(checkpoint.Solver().bodies, [](const auto &body) {
                return body.hasPolicyIntent && body.lastLinearIntent == std::array<float, 3>{0.75F, 0.125F, -0.375F} &&
                       body.motion.linearVelocity != body.lastLinearIntent;
            }));
        }

        void CaptureProcess(const std::filesystem::path &directory) {
            auto runtime = BoundRuntime();
            SceneOwnerFixture source{*runtime, 41, 1, true};
            REQUIRE(source.view.Find({1})->entity.index !=
                    0);  // Source native/entity creation order differs from receiving logical-key order.
            source.Publish();
            source.Advance(5);
            auto capture = source.PrepareCapture();
            {
                auto checkpoint = Take(capture->Capture());
                CheckNonzeroOriginalIntent(*checkpoint.Get());
                REQUIRE_FALSE(checkpoint->World().publishedEvents.empty());  // Real undrained contact/exit delivery must survive restart.
                WriteState(directory / "checkpoint.bin", checkpoint->Bytes());
            }
            for (unsigned tick = 6; tick <= 10; ++tick) {
                source.Advance();
                auto continuation = Take(capture->Capture());
                CHECK(continuation->World().completedTick == tick);
                WriteState(directory / ("continuation-" + std::to_string(tick) + ".bin"), continuation->Bytes());
            }
        }

        void ChurnLocalOwners(PhysicsRuntime &runtime) {
            for (std::uint64_t index = 0; index < 4; ++index) {
                SceneOwnerFixture unrelated{runtime, 100 + index, 3};
                unrelated.Publish();
                unrelated.Advance();
            }  // Actual old Scene/native/asset owners retire; they are never reused as durable identity evidence.
        }

        void RestoreProcess(const std::filesystem::path &directory) {
            auto runtime = BoundRuntime();
            ChurnLocalOwners(*runtime);
            SceneOwnerFixture receiver{*runtime, 211, 73, false};
            REQUIRE(receiver.view.Find({1})->entity.index == 0);
            const auto bytes = ReadState(directory / "checkpoint.bin");
            auto restored = receiver.Restore(bytes);
            Check(restored->ValidateAndArm());
            receiver.Publish();
            const auto pair = Take(receiver.host.Borrow());
            REQUIRE(pair.physics.Identity().Value() > 4);
            REQUIRE(pair.scene.value == 211);
            REQUIRE(pair.physics.PublishedTick().completedTick == 5);
            REQUIRE(pair.character.PublishedTick().completedTick == 5);
            restored.reset();  // The actual last-point owner is no longer queried after publication.
            auto capture = receiver.PrepareCapture();
            {
                const auto recaptured = Take(capture->Capture());
                CHECK(std::ranges::equal(recaptured->Bytes(), bytes));  // All solver/world/semantic-event-ordinal fields, not a DTO subset.
            }
            for (unsigned tick = 6; tick <= 10; ++tick) {
                receiver.Advance();
                const auto actual = Take(capture->Capture());
                const auto expected = ReadState(directory / ("continuation-" + std::to_string(tick) + ".bin"));
                INFO("first actual receiving Character/Physics tick and full canonical continuation tick=" << tick);
                CHECK(std::ranges::equal(actual->Bytes(), expected));
            }
        }
    }  // namespace

    TEST_CASE("Physics canonical restart executes in separate verified same-binary processes", "[.][physics][native][restart][child]") {
        const auto *mode = std::getenv("HORO_PHYSICS_RESTART_MODE");
        const auto *path = std::getenv("HORO_PHYSICS_RESTART_DIRECTORY");
        REQUIRE(mode);
        REQUIRE(path);
        const auto directory = std::filesystem::path{path};
        REQUIRE(directory.is_absolute());
        REQUIRE(std::filesystem::is_directory(directory));
        const auto selected = std::string_view{mode};
        REQUIRE((selected == "capture" || selected == "restore"));
        if (selected == "capture")
            CaptureProcess(directory);
        else
            RestoreProcess(directory);
    }
}  // namespace Horo::Physics::StateTest
