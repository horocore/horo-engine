#include "Horo/Extensions/EditorSvgIcon.h"

#include "Horo/Extensions/ExtensionErrors.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <lunasvg.h>
#include <span>
#include <string>
#include <vector>

namespace Horo::Extensions {
    namespace {
        constexpr std::size_t kMaximumBytes = 64U * 1024U;
        constexpr std::uint32_t kIconSize = 48;
        constexpr std::size_t kMaximumElements = 128;
        constexpr std::size_t kMaximumNumbers = 2048;
        constexpr std::size_t kMaximumPathCommands = 256;
        constexpr std::size_t kMaximumDepth = 8;

        /** @brief Returns whether the byte is SVG/XML whitespace without consulting a process locale. */
        [[nodiscard]] bool Space(const char value) noexcept {
            return value == ' ' || value == '\t' || value == '\r' || value == '\n';
        }

        /** @brief Consumes an optional bounded decimal exponent after the mantissa. */
        [[nodiscard]] bool Exponent(const std::string_view value, std::size_t &cursor, const std::size_t start,
                                    int &decimalExponent) noexcept {
            if (cursor < value.size() && (value[cursor] == 'e' || value[cursor] == 'E')) {
                ++cursor;
                bool exponentNegative = false;
                if (cursor < value.size() && (value[cursor] == '+' || value[cursor] == '-'))
                    exponentNegative = value[cursor++] == '-';
                const std::size_t exponentStart = cursor;
                int exponent = 0;
                while (cursor < value.size() && (value[cursor] >= '0' && value[cursor] <= '9')) {
                    if (cursor - start >= 32 || exponent > 32)
                        return false;
                    exponent = exponent * 10 + value[cursor++] - '0';
                }
                if (cursor == exponentStart || exponent > 32)
                    return false;
                decimalExponent += exponentNegative ? -exponent : exponent;
            }
            return true;
        }

        /** @brief Parses a bounded ASCII decimal without locale state or platform floating-point conversion APIs. */
        [[nodiscard]] bool Number(const std::string_view value, std::size_t &cursor, double &output) noexcept {
            const std::size_t start = cursor;
            bool negative = false;
            if (cursor < value.size() && (value[cursor] == '+' || value[cursor] == '-'))
                negative = value[cursor++] == '-';
            double magnitude = 0.0;
            std::size_t digits = 0;
            int decimalExponent = 0;
            const auto digit = [](const char c) {
                return c >= '0' && c <= '9';
            };
            while (cursor < value.size() && digit(value[cursor])) {
                if (cursor - start >= 32)
                    return false;
                magnitude = magnitude * 10.0 + static_cast<double>(value[cursor++] - '0');
                ++digits;
            }
            if (cursor < value.size() && value[cursor] == '.') {
                ++cursor;
                while (cursor < value.size() && digit(value[cursor])) {
                    if (cursor - start >= 32)
                        return false;
                    magnitude = magnitude * 10.0 + static_cast<double>(value[cursor++] - '0');
                    --decimalExponent;
                    ++digits;
                }
            }
            if (digits == 0)
                return false;
            if (!Exponent(value, cursor, start, decimalExponent))
                return false;
            if (cursor - start > 32)
                return false;
            output = magnitude * std::pow(10.0, decimalExponent);
            if (negative)
                output = -output;
            return std::isfinite(output);
        }

        /** @brief Consumes finite geometry numbers and bounds subdivision workload before decoder allocation. */
        [[nodiscard]] bool Geometry(const std::string_view value, std::size_t &numbers, std::size_t &commands, const bool path) {
            std::size_t cursor = 0;
            while (cursor < value.size()) {
                const char c = value[cursor];
                if (Space(c) || c == ',') {
                    ++cursor;
                    continue;
                }
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
                    if (!path || std::string_view{"MLHVCSQTAZz"}.find(c) == std::string_view::npos || ++commands > kMaximumPathCommands)
                        return false;
                    ++cursor;
                    continue;
                }
                double number{};
                if (!Number(value, cursor, number) || std::abs(number) > 1024.0 || ++numbers > kMaximumNumbers)
                    return false;
            }
            return true;
        }

        /** @brief Applies attribute-specific scalar and viewport bounds after finite geometry admission. */
        [[nodiscard]] bool ScalarAttribute(const std::string_view name, const std::string_view value) {
            if (name == "viewBox") {
                std::array<double, 4> bounds{};
                std::size_t cursor = 0;
                for (double &bound : bounds) {
                    while (cursor < value.size() && (Space(value[cursor]) || value[cursor] == ','))
                        ++cursor;
                    if (!Number(value, cursor, bound))
                        return false;
                }
                while (cursor < value.size() && Space(value[cursor]))
                    ++cursor;
                return cursor == value.size() && bounds[2] >= 1.0 && bounds[3] >= 1.0;
            }
            if (name == "stroke-width") {
                double width{};
                std::size_t cursor = 0;
                return Number(value, cursor, width) && cursor == value.size() && width >= 0.0 && width <= 64.0;
            }
            if (name != "d" && name != "points" && name != "viewBox") {
                double scalar{};
                std::size_t cursor = 0;
                if (!Number(value, cursor, scalar) || cursor != value.size())
                    return false;
                if ((name == "opacity" || name == "fill-opacity" || name == "stroke-opacity" || name == "stop-opacity" ||
                     name == "offset") &&
                    (scalar < 0.0 || scalar > 1.0))
                    return false;
            }
            return true;
        }

        /** @brief Checks an explicitly supported inert SVG attribute; unknown semantics fail closed. */
        [[nodiscard]] bool Attribute(const std::string_view name, const std::string_view value, std::size_t &numbers,
                                     std::size_t &commands) {
            constexpr std::array textNames{"id",         "fill",  "stroke",      "fill-rule",           "stroke-linecap", "stroke-linejoin",
                                           "stop-color", "xmlns", "xmlns:xlink", "preserveAspectRatio", "gradientUnits",  "spreadMethod"};
            constexpr std::array geometryNames{"d",
                                               "points",
                                               "x",
                                               "y",
                                               "x1",
                                               "y1",
                                               "x2",
                                               "y2",
                                               "cx",
                                               "cy",
                                               "r",
                                               "rx",
                                               "ry",
                                               "width",
                                               "height",
                                               "viewBox",
                                               "stroke-width",
                                               "opacity",
                                               "fill-opacity",
                                               "stroke-opacity",
                                               "stop-opacity",
                                               "offset"};
            if (value.find('&') != std::string_view::npos || value.find('\\') != std::string_view::npos)
                return false;
            if (std::ranges::find(textNames, name) != textNames.end()) {
                if (name == "xmlns")
                    return value == "http://www.w3.org/2000/svg";
                if (name == "xmlns:xlink")
                    return value == "http://www.w3.org/1999/xlink";
                if (value.find("url") != std::string_view::npos)
                    return value.starts_with("url(#") && value.ends_with(')') && value.find(')', 5) == value.size() - 1;
                return value.find(':') == std::string_view::npos && value.find('<') == std::string_view::npos;
            }
            if (std::ranges::find(geometryNames, name) == geometryNames.end() || !Geometry(value, numbers, commands, name == "d"))
                return false;
            return ScalarAttribute(name, value);
        }

        /** @brief Copies no XML data; admits at most 24 unique supported attributes on the current tag. */
        [[nodiscard]] bool Attributes(const std::string_view svg, std::size_t &cursor, std::size_t &numbers, std::size_t &commands) {
            std::size_t attributes = 0;
            std::array<std::string_view, 24> names{};
            while (cursor < svg.size() && svg[cursor] != '>' && svg[cursor] != '/') {
                while (cursor < svg.size() && Space(svg[cursor]))
                    ++cursor;
                if (cursor == svg.size())
                    return false;
                if (svg[cursor] == '>' || svg[cursor] == '/')
                    break;
                const std::size_t nameStart = cursor;
                while (cursor < svg.size() && !Space(svg[cursor]) && svg[cursor] != '=')
                    ++cursor;
                const auto name = svg.substr(nameStart, cursor - nameStart);
                while (cursor < svg.size() && Space(svg[cursor]))
                    ++cursor;
                if (attributes == names.size() ||
                    std::ranges::find(names.begin(), names.begin() + attributes, name) != names.begin() + attributes ||
                    cursor == svg.size() || svg[cursor++] != '=')
                    return false;
                names[attributes++] = name;
                while (cursor < svg.size() && Space(svg[cursor]))
                    ++cursor;
                if (cursor == svg.size() || (svg[cursor] != '\'' && svg[cursor] != '"'))
                    return false;
                const char quote = svg[cursor++];
                const std::size_t valueStart = cursor;
                const auto end = svg.find(quote, cursor);
                if (end == std::string_view::npos || !Attribute(name, svg.substr(valueStart, end - valueStart), numbers, commands))
                    return false;
                cursor = end + 1;
            }
            return true;
        }

        enum class XmlMisc {
            None,
            Consumed,
            Invalid
        };

        /** @brief Skips only bounded comments and the initial XML declaration, rejecting malformed terminators. */
        [[nodiscard]] XmlMisc Miscellaneous(const std::string_view svg, std::size_t &cursor, const bool root) {
            if (svg.substr(cursor).starts_with("!--")) {
                const auto end = svg.find("-->", cursor + 3);
                if (end == std::string_view::npos)
                    return XmlMisc::Invalid;
                cursor = end + 3;
                return XmlMisc::Consumed;
            }
            if (svg[cursor] == '?' && !root) {
                const auto end = svg.find("?>", cursor);
                if (end == std::string_view::npos || !svg.substr(cursor, end - cursor).starts_with("?xml "))
                    return XmlMisc::Invalid;
                cursor = end + 2;
                return XmlMisc::Consumed;
            }
            return XmlMisc::None;
        }

        /** @brief Closes exactly the current XML element and marks completion only when the root stack becomes empty. */
        [[nodiscard]] bool CloseTag(const std::string_view svg, std::size_t &cursor, const std::string_view tag,
                                    std::vector<std::string_view> &stack, bool &rootClosed) {
            while (cursor < svg.size() && Space(svg[cursor]))
                ++cursor;
            if (stack.empty() || stack.back() != tag || cursor == svg.size() || svg[cursor++] != '>')
                return false;
            stack.pop_back();
            if (stack.empty())
                rootClosed = true;
            return true;
        }

        /** @brief Admits and closes an opening element within the fixed XML stack capacity. */
        [[nodiscard]] bool OpenTag(const std::string_view svg, std::size_t &cursor, const std::string_view tag,
                                   std::vector<std::string_view> &stack, bool &rootClosed, std::size_t &numbers, std::size_t &commands) {
            if (!Attributes(svg, cursor, numbers, commands))
                return false;
            if (cursor == svg.size())
                return false;
            const bool empty = svg[cursor] == '/';
            if (empty)
                ++cursor;
            if (cursor == svg.size() || svg[cursor++] != '>')
                return false;
            if (!empty) {
                if (stack.size() == kMaximumDepth)
                    return false;
                stack.push_back(tag);
            } else if (stack.empty())
                rootClosed = true;
            return true;
        }

        /** @brief Validates XML structure and the finite static icon profile before invoking LunaSVG. */
        [[nodiscard]] bool ValidateSvg(const std::string_view svg) {
            if (svg.empty() || svg.size() > kMaximumBytes || svg.find('\0') != std::string_view::npos)
                return false;
            constexpr std::array tags{"svg",  "g",        "defs",    "path",           "rect",           "circle", "ellipse",
                                      "line", "polyline", "polygon", "linearGradient", "radialGradient", "stop"};
            std::vector<std::string_view> stack;
            stack.reserve(kMaximumDepth);
            std::size_t cursor = 0, elements = 0, numbers = 0, commands = 0;
            bool root = false;
            bool rootClosed = false;
            while (cursor < svg.size()) {
                while (cursor < svg.size() && Space(svg[cursor]))
                    ++cursor;
                if (cursor == svg.size())
                    break;
                if (svg[cursor++] != '<' || cursor == svg.size())
                    return false;
                const auto misc = Miscellaneous(svg, cursor, root);
                if (misc == XmlMisc::Invalid)
                    return false;
                if (misc == XmlMisc::Consumed)
                    continue;
                const bool close = svg[cursor] == '/';
                if (close)
                    ++cursor;
                const std::size_t start = cursor;
                while (cursor < svg.size() && ((svg[cursor] >= 'a' && svg[cursor] <= 'z') || (svg[cursor] >= 'A' && svg[cursor] <= 'Z')))
                    ++cursor;
                const auto tag = svg.substr(start, cursor - start);
                if (tag.empty() || std::ranges::find(tags, tag) == tags.end())
                    return false;
                if (close) {
                    if (!CloseTag(svg, cursor, tag, stack, rootClosed))
                        return false;
                    continue;
                }
                if (++elements > kMaximumElements || rootClosed || (!root && tag != "svg") || (root && tag == "svg"))
                    return false;
                root = true;
                if (!OpenTag(svg, cursor, tag, stack, rootClosed, numbers, commands))
                    return false;
            }
            return root && rootClosed && stack.empty();
        }
    }  // namespace

    /** @copydoc RasterizeEditorSvgIcon */
    Result<EditorSvgIcon> RasterizeEditorSvgIcon(const std::string_view svg) {
        const auto fail = [] {
            return Result<EditorSvgIcon>::Failure(
                MakeError(ExtensionErrors::EditorSurfaceDescriptorInvalid, "SVG icon violates the bounded static-vector profile."));
        };
        if (!ValidateSvg(svg))
            return fail();
        auto document = lunasvg::Document::loadFromData(svg.data(), svg.size());
        if (!document || !std::isfinite(document->width()) || !std::isfinite(document->height()) || document->width() < 1.0F ||
            document->height() < 1.0F || document->width() > 1024.0F || document->height() > 1024.0F)
            return fail();
        auto bitmap = document->renderToBitmap(kIconSize, kIconSize);
        if (bitmap.isNull() || bitmap.width() != kIconSize || bitmap.height() != kIconSize || bitmap.stride() != kIconSize * 4)
            return fail();
        bitmap.convertToRGBA();
        EditorSvgIcon icon{.width = kIconSize, .height = kIconSize, .pixels = std::vector<std::uint8_t>(kIconSize * kIconSize * 4U)};
        const std::span<const std::uint8_t> source{bitmap.data(), icon.pixels.size()};
        std::ranges::copy(source, icon.pixels.begin());
        return Result<EditorSvgIcon>::Success(std::move(icon));
    }
}  // namespace Horo::Extensions
