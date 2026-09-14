#include "ProjectMigrationTestFixture.h"

#include <array>
#include <atomic>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>

namespace Horo::Tests {
    namespace {
        class TestSha256 final {
        public:
            void Update(const std::string_view text) {
                for (const unsigned char value : text) {
                    buffer_[bufferSize_++] = value;
                    bitCount_ += 8;
                    if (bufferSize_ == buffer_.size()) {
                        Transform();
                        bufferSize_ = 0;
                    }
                }
            }

            [[nodiscard]] std::array<std::uint8_t, 32> Final() {
                const std::uint64_t originalBits = bitCount_;
                buffer_[bufferSize_++] = 0x80;
                if (bufferSize_ > 56) {
                    while (bufferSize_ < buffer_.size())
                        buffer_[bufferSize_++] = 0;
                    Transform();
                    bufferSize_ = 0;
                }
                while (bufferSize_ < 56)
                    buffer_[bufferSize_++] = 0;
                for (int shift = 56; shift >= 0; shift -= 8)
                    buffer_[bufferSize_++] = static_cast<std::uint8_t>(originalBits >> shift);
                Transform();

                std::array<std::uint8_t, 32> result{};
                for (std::size_t i = 0; i < state_.size(); ++i)
                    for (std::size_t j = 0; j < 4; ++j)
                        result[i * 4 + j] = static_cast<std::uint8_t>(state_[i] >> (24U - 8U * j));
                return result;
            }

        private:
            static constexpr std::array<std::uint32_t, 64> K{0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
                                                             0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
                                                             0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
                                                             0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
                                                             0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
                                                             0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
                                                             0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
                                                             0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
                                                             0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
                                                             0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
                                                             0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

            void Transform() {
                std::array<std::uint32_t, 64> words{};
                for (std::size_t i = 0; i < 16; ++i)
                    words[i] = (std::uint32_t(buffer_[i * 4]) << 24U) | (std::uint32_t(buffer_[i * 4 + 1]) << 16U) |
                               (std::uint32_t(buffer_[i * 4 + 2]) << 8U) | buffer_[i * 4 + 3];
                for (std::size_t i = 16; i < words.size(); ++i) {
                    const auto s0 = std::rotr(words[i - 15], 7) ^ std::rotr(words[i - 15], 18) ^ (words[i - 15] >> 3U);
                    const auto s1 = std::rotr(words[i - 2], 17) ^ std::rotr(words[i - 2], 19) ^ (words[i - 2] >> 10U);
                    words[i] = words[i - 16] + s0 + words[i - 7] + s1;
                }

                auto [a, b, c, d, e, f, g, h] = state_;
                for (std::size_t i = 0; i < words.size(); ++i) {
                    const auto s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
                    const auto choice = (e & f) ^ ((~e) & g);
                    const auto t1 = h + s1 + choice + K[i] + words[i];
                    const auto s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
                    const auto majority = (a & b) ^ (a & c) ^ (b & c);
                    const auto t2 = s0 + majority;
                    h = g;
                    g = f;
                    f = e;
                    e = d + t1;
                    d = c;
                    c = b;
                    b = a;
                    a = t1 + t2;
                }
                state_[0] += a;
                state_[1] += b;
                state_[2] += c;
                state_[3] += d;
                state_[4] += e;
                state_[5] += f;
                state_[6] += g;
                state_[7] += h;
            }

            std::array<std::uint32_t, 8> state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                                0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
            std::array<std::uint8_t, 64> buffer_{};
            std::size_t bufferSize_{};
            std::uint64_t bitCount_{};
        };

        [[nodiscard]] std::filesystem::path UniqueFixtureRoot() {
            static std::atomic<std::uint64_t> sequence{};
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            return std::filesystem::temp_directory_path() / ("horo-project-migration-integration-" + std::to_string(stamp) + "-" +
                                                             std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
        }

        [[nodiscard]] std::string ReadText(const std::filesystem::path &path) {
            std::ifstream input(path, std::ios::binary);
            REQUIRE((input.good()));
            return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        }
    }  // namespace

    ProjectMigrationTestFixture::ProjectMigrationTestFixture()
        : root_(UniqueFixtureRoot()), logRoot_(root_.parent_path() / (root_.filename().string() + "-logs")) {
        const std::filesystem::path fixture =
            std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "tests/fixtures/projects/horo_0_0_1_compression";
        std::error_code error;
        std::filesystem::copy(fixture, root_, std::filesystem::copy_options::recursive, error);
        REQUIRE_FALSE((error));
        nlohmann::json metadata = nlohmann::json::parse(ReadText(root_ / ".horo/project.json"));
        metadata["settings"]["defaultScene"] = "assets/scenes/main.horo";
        {
            std::ofstream output(root_ / ".horo/project.json", std::ios::binary | std::ios::trunc);
            REQUIRE((output.good()));
            output << metadata.dump(2) << '\n';
            REQUIRE((output.good()));
        }
        std::filesystem::create_directories(root_ / "assets/scenes", error);
        REQUIRE_FALSE((error));
        std::filesystem::create_directories(root_ / "assets/prefabs", error);
        REQUIRE_FALSE((error));
        {
            std::ofstream prefab(root_ / "assets/prefabs/player.prefab", std::ios::binary);
            REQUIRE((prefab.good()));
            prefab
                << R"({"projectVersion":"0.0.1","prefabId":"00112233-4455-6677-8899-aabbccddeeff","objects":[{"components":[{"type":"game.unknown","payload":{"bytes":[0,127,255]}}]}]})";
            REQUIRE((prefab.good()));
        }
        {
            std::ofstream sidecar(root_ / "assets/prefabs/player.prefab.horo", std::ios::binary);
            REQUIRE((sidecar.good()));
            sidecar << R"({"schemaVersion":1,"assetId":"00112233-4455-6677-8899-aabbccddeeff","assetType":"core.prefab"})";
            REQUIRE((sidecar.good()));
        }
        {
            std::ofstream scene(root_ / "assets/scenes/main.horo", std::ios::binary);
            REQUIRE((scene.good()));
            scene
                << R"({"schemaVersion":1,"objects":[],"prefabInstances":[{"instanceId":1,"sourcePath":"assets/prefabs/player.prefab","parent":null,"rootTransform":{"translation":[0.0,0.0,0.0],"rotation":[0.0,0.0,0.0,1.0],"scale":[1.0,1.0,1.0]}}]})";
            REQUIRE((scene.good()));
        }
        std::filesystem::create_directories(logRoot_, error);
        REQUIRE_FALSE((error));
    }

    ProjectMigrationTestFixture::~ProjectMigrationTestFixture() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
        std::filesystem::remove_all(logRoot_, ignored);
    }

    const std::filesystem::path &ProjectMigrationTestFixture::Root() const noexcept {
        return root_;
    }

    const std::filesystem::path &ProjectMigrationTestFixture::LogRoot() const noexcept {
        return logRoot_;
    }

    std::string ProjectMigrationTestFixture::ReadProjectBytes() const {
        return ReadText(root_ / ".horo/project.json");
    }

    std::string ProjectMigrationTestFixture::ReadHistoryBytes() const {
        return ReadText(root_ / ".horo/migration_history.json");
    }

    nlohmann::json ProjectMigrationTestFixture::ReadProjectJson() const {
        return nlohmann::json::parse(ReadProjectBytes());
    }

    nlohmann::json ProjectMigrationTestFixture::ReadHistoryJson() const {
        return nlohmann::json::parse(ReadHistoryBytes());
    }

    Editor::ProjectOpenProgressSnapshot PumpProjectOpenToTerminal(Editor::ProjectOpenService &service,
                                                                  const Editor::ProjectOpenOperationId operation,
                                                                  const std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        float previousProgress = 0.0F;
        while (std::chrono::steady_clock::now() < deadline) {
            service.PumpOwnerThread();
            const auto snapshot = service.Query(operation);
            REQUIRE((snapshot.has_value()));
            REQUIRE((snapshot->progress >= previousProgress));
            previousProgress = snapshot->progress;
            if (snapshot->outcome != Editor::ProjectOpenOutcome::Running)
                return *snapshot;
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        FAIL("Project open operation did not reach a terminal state before the timeout.");
    }

    std::string ComputeTestSha256(const std::string_view text) {
        TestSha256 sha;
        sha.Update(text);
        const auto digest = sha.Final();
        std::ostringstream formatted;
        formatted << "sha256:" << std::hex << std::setfill('0');
        for (const std::uint8_t byte : digest)
            formatted << std::setw(2) << static_cast<unsigned>(byte);
        return formatted.str();
    }
}  // namespace Horo::Tests
