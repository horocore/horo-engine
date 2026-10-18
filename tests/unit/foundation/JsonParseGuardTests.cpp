#include "JsonUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Horo::Foundation {
    TEST_CASE("Guarded JSON preserves callback depth and value semantics", "[foundation][json][scene]") {
        const std::vector<std::string> documents{"null",
                                                 "true",
                                                 "false",
                                                 "-42",
                                                 "18446744073709551615",
                                                 "1.25",
                                                 R"("text")",
                                                 R"({"left":{"same":1},"right":{"same":2}})",
                                                 R"([{"key":1},{"key":2}])",
                                                 R"([[],{},null,1])",
                                                 R"({"escaped":"\uD83D\uDE00"})"};
        for (const auto &document : documents) {
            for (const auto depth : {0U, 1U, 2U, 3U, 5U, 8U}) {
                CAPTURE(document, depth);
                JsonParseGuard previous{depth};
                const auto expected = Json::parse(document, std::ref(previous));
                JsonParseGuard current{depth};
                const auto actual = current.Parse(document.begin(), document.end());
                CHECK(current.HasDuplicate() == previous.HasDuplicate());
                CHECK(current.IsTooDeep() == previous.IsTooDeep());
                if (current.IsTooDeep())
                    CHECK(actual.is_discarded());
                else
                    CHECK(actual == expected);
            }
        }
    }

    TEST_CASE("Guarded JSON rejects duplicate decoded keys in every object scope", "[foundation][json][scene]") {
        for (const std::string document :
             {R"({"key":1,"key":2})", R"({"a":0,"\u0061":1})", R"([{"key":1},{"nested":{"key":1,"key":2}}])"}) {
            CAPTURE(document);
            JsonParseGuard guard{16};
            CHECK(guard.Parse(document.begin(), document.end()).is_discarded());
            CHECK(guard.HasDuplicate());
            CHECK_FALSE(guard.IsTooDeep());
        }
    }

    TEST_CASE("Guarded JSON retains original syntax exceptions and strict complete-input admission", "[foundation][json][scene]") {
        for (const std::string document : {"", "[", "null true", "{\"key\":}", "\"unterminated", "\"\\uD800\""}) {
            CAPTURE(document);
            JsonParseGuard guard{16};
            CHECK_THROWS_AS(guard.Parse(document.begin(), document.end()), Json::parse_error);
            CHECK_FALSE(guard.HasDuplicate());
            CHECK_FALSE(guard.IsTooDeep());
        }
        const std::string overflow = "1e1000";
        JsonParseGuard guard{16};
        CHECK_THROWS_AS(guard.Parse(overflow.begin(), overflow.end()), Json::out_of_range);
    }

    TEST_CASE("Guarded JSON stops excessive nesting before DOM construction", "[foundation][json][scene]") {
        const std::string document = std::string(4096, '[') + "0" + std::string(4096, ']');
        JsonParseGuard guard{64};
        CHECK(guard.Parse(document.begin(), document.end()).is_discarded());
        CHECK(guard.IsTooDeep());
        CHECK_FALSE(guard.HasDuplicate());
    }

    TEST_CASE("Guarded byte parsing retains large sibling object arrays and catches a late duplicate", "[foundation][json][scene]") {
        std::string document = "[";
        for (std::size_t index = 0; index < 8192; ++index)
            document += R"({"key":1},)";
        document += R"({"key":2}])";
        const std::vector<std::uint8_t> bytes{document.begin(), document.end()};
        JsonParseGuard valid{8};
        const auto parsed = valid.Parse(bytes.begin(), bytes.end());
        REQUIRE(parsed.is_array());
        CHECK(parsed.size() == 8193);
        CHECK(parsed.front()["key"] == 1);
        CHECK(parsed.back()["key"] == 2);
        document.insert(document.size() - 2, R"(,"key":3)");
        JsonParseGuard invalid{8};
        CHECK(invalid.Parse(document.begin(), document.end()).is_discarded());
        CHECK(invalid.HasDuplicate());
    }
}  // namespace Horo::Foundation
