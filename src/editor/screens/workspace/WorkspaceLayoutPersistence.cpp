#include "Horo/Editor/WorkspaceLayoutPersistence.h"

#include <charconv>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace Horo::Editor {
    namespace {
        constexpr std::string_view LegacyContentBrowserPanelId = "horo.content_browser";
        constexpr std::string_view GlobalDockPanelId = "horo.global_dock";

        /**
         * @brief Migrates the former Content Browser host identity at the persistence boundary.
         * @param value Persisted panel identity or node identity to normalize.
         */
        void NormalizeLegacyGlobalDockIdentity(std::string &value) {
            if (value == LegacyContentBrowserPanelId) {
                value = GlobalDockPanelId;
                return;
            }

            std::size_t offset = 0;
            while ((offset = value.find(LegacyContentBrowserPanelId, offset)) != std::string::npos) {
                value.replace(offset, LegacyContentBrowserPanelId.size(), GlobalDockPanelId);
                offset += GlobalDockPanelId.size();
            }
        }

        class Parser {
        public:
            explicit Parser(const std::string_view text) : m_text(text) {}

            std::optional<WorkspaceLayout> Parse(std::string &error) {
                WorkspaceLayout layout;
                Skip();
                if (!ObjectStart() || !Key("schemaVersion") || !UInt(layout.schemaVersion) || !Comma() || !Key("root")) {
                    error = "invalid workspace document header";
                    return std::nullopt;
                }
                auto root = Node(error);
                if (!root)
                    return std::nullopt;
                layout.root = std::move(*root);
                if (Take(',')) {
                    if (!Key("documents") || !Take('[')) {
                        error = "workspace document tabs invalid";
                        return std::nullopt;
                    }
                    auto documents = ParseDocumentList(error);
                    if (!documents)
                        return std::nullopt;
                    layout.openDocuments = std::move(*documents);
                }
                if (!ObjectEnd())
                    return std::nullopt;
                if (layout.schemaVersion != WorkspaceLayoutPersistence::CurrentSchemaVersion) {
                    error = "unsupported workspace schema version";
                    return std::nullopt;
                }
                if (!layout.Validate().empty())
                    return std::nullopt;
                return layout;
            }

        private:
            std::string_view m_text;
            std::size_t m_pos = 0;

            void Skip() {
                while (m_pos < m_text.size() &&
                       (m_text[m_pos] == ' ' || m_text[m_pos] == '\n' || m_text[m_pos] == '\r' || m_text[m_pos] == '\t'))
                    ++m_pos;
            }

            bool Take(const char c) {
                Skip();
                if (m_pos >= m_text.size() || m_text[m_pos] != c)
                    return false;
                ++m_pos;
                return true;
            }

            bool ObjectStart() {
                return Take('{');
            }

            bool ObjectEnd() {
                return Take('}');
            }

            bool Comma() {
                return Take(',');
            }

            bool Key(const char *key) {
                std::string value;
                const bool isExpectedKey = String(value) && value == key;
                return isExpectedKey && Take(':');
            }

            bool String(std::string &out) {
                Skip();
                if (m_pos >= m_text.size() || m_text[m_pos] != '"')
                    return false;
                ++m_pos;
                out.clear();

                while (m_pos < m_text.size()) {
                    const char c = m_text[m_pos++];
                    if (c == '"')
                        return true;
                    if (c == '\\' && m_pos < m_text.size()) {
                        const char escaped = m_text[m_pos++];
                        out += escaped == 'n' ? '\n' : escaped;
                    } else
                        out += c;
                }
                return false;
            }

            bool UInt(std::uint32_t &out) {
                Skip();
                const char *begin = m_text.data() + m_pos;
                const char *end = m_text.data() + m_text.size();
                auto [ptr, ec] = std::from_chars(begin, end, out);
                if (ec != std::errc{})
                    return false;
                m_pos = static_cast<std::size_t>(ptr - m_text.data());
                return true;
            }

            bool Float(float &out) {
                Skip();
                const std::size_t start = m_pos;
                while (m_pos < m_text.size() &&
                       (m_text[m_pos] == '-' || m_text[m_pos] == '.' || (m_text[m_pos] >= '0' && m_text[m_pos] <= '9')))
                    ++m_pos;
                try {
                    out = std::stof(std::string(m_text.substr(start, m_pos - start)));
                    return true;
                } catch (const std::invalid_argument &) {
                    return false;
                } catch (const std::out_of_range &) {
                    return false;
                }
            }

            // Parses `"key":"<string>"` into out after the node type dispatch.
            bool KeyedString(const char *key, std::string &out) {
                return Key(key) && String(out);
            }

            /**
             * @brief Parses a `"panel"` node body after its id has been consumed.
             */
            std::optional<LayoutNode> ParsePanelNode(std::string id, std::string &error) {
                std::string panel;
                if (!KeyedString("panel", panel) || !ObjectEnd()) {
                    error = "panel node invalid";
                    return std::nullopt;
                }
                NormalizeLegacyGlobalDockIdentity(panel);
                return LayoutNode(PanelNode{std::move(id), std::move(panel)});
            }

            /**
             * @brief Parses the remaining `"tabs"` array elements into stack, starting after the opening bracket.
             */
            std::optional<TabStackNode> ParseTabList(TabStackNode stack, std::string &error) {
                Skip();
                if (Take(']'))
                    return stack;
                while (true) {
                    std::string tab;
                    if (!String(tab)) {
                        error = "tab invalid";
                        return std::nullopt;
                    }
                    NormalizeLegacyGlobalDockIdentity(tab);
                    stack.tabs.push_back(std::move(tab));
                    if (Take(']'))
                        return stack;
                    if (!Comma()) {
                        error = "tab separator missing";
                        return std::nullopt;
                    }
                }
            }

            /**
             * @brief Parses persisted document open keys without restoring session-local instances.
             */
            std::optional<std::vector<SerializedDocumentOpenKey>> ParseDocumentList(std::string &error) {
                std::vector<SerializedDocumentOpenKey> documents;
                Skip();
                if (Take(']'))
                    return documents;
                while (true) {
                    if (!ObjectStart()) {
                        error = "workspace document tab must be an object";
                        return std::nullopt;
                    }
                    SerializedDocumentOpenKey document;
                    if (!Key("kind") || !String(document.kind) || !Comma() || !Key("source") || !String(document.source) || !ObjectEnd()) {
                        error = "workspace document tab invalid";
                        return std::nullopt;
                    }
                    documents.push_back(std::move(document));
                    if (Take(']'))
                        return documents;
                    if (!Comma()) {
                        error = "workspace document tab separator missing";
                        return std::nullopt;
                    }
                }
            }

            /**
             * @brief Parses a `"stack"` node body after its id has been consumed.
             */
            std::optional<LayoutNode> ParseStackNode(std::string id, std::string &error) {
                if (!Key("tabs")) {
                    error = "stack tabs missing";
                    return std::nullopt;
                }
                if (!Take('[')) {
                    error = "stack tabs invalid";
                    return std::nullopt;
                }
                TabStackNode stack{std::move(id)};
                auto tabs = ParseTabList(std::move(stack), error);
                if (!tabs)
                    return std::nullopt;
                auto &resolvedTabs = *tabs;
                if (!Comma() || !Key("active") || !String(resolvedTabs.activeTab.emplace()) || !ObjectEnd()) {
                    error = "stack active tab invalid";
                    return std::nullopt;
                }
                NormalizeLegacyGlobalDockIdentity(*resolvedTabs.activeTab);
                return LayoutNode(std::move(resolvedTabs));
            }

            // Parses the axis value onward after Node() consumed the "axis" key.
            std::optional<LayoutNode> ParseSplitAxisValue(std::string id, std::string &error) {
                std::string axis;
                float ratio = 0.5F;
                if (!String(axis) || !Comma() || !Key("ratio") || !Float(ratio) || !Comma() || !Key("first")) {
                    error = "split properties invalid";
                    return std::nullopt;
                }
                auto first = Node(error);
                if (!first || !Comma() || !Key("second"))
                    return std::nullopt;
                auto second = Node(error);
                if (!second || !ObjectEnd())
                    return std::nullopt;
                return LayoutNode(SplitNode{std::move(id),
                                            axis == "vertical" ? WorkspaceSplitAxis::Vertical : WorkspaceSplitAxis::Horizontal, ratio,
                                            160.0F, 160.0F, std::make_unique<LayoutNode>(std::move(*first)),
                                            std::make_unique<LayoutNode>(std::move(*second))});
            }

            std::optional<LayoutNode> Node(std::string &error) {
                if (!ObjectStart()) {
                    error = "node must be an object";
                    return std::nullopt;
                }
                if (!Key("type")) {
                    error = "node type missing";
                    return std::nullopt;
                }
                std::string type;
                if (!String(type) || !Comma() || !Key("id")) {
                    error = "node identity missing";
                    return std::nullopt;
                }
                std::string id;
                if (!String(id) || !Comma()) {
                    error = "node id invalid";
                    return std::nullopt;
                }
                NormalizeLegacyGlobalDockIdentity(id);
                if (type == "panel")
                    return ParsePanelNode(std::move(id), error);
                if (type == "stack")
                    return ParseStackNode(std::move(id), error);
                if (type != "split") {
                    error = "unknown node type";
                    return std::nullopt;
                }
                if (!Key("axis")) {
                    error = "split properties invalid";
                    return std::nullopt;
                }
                return ParseSplitAxisValue(std::move(id), error);
            }
        };

        std::string Escape(const std::string_view value) {
            std::string out;
            for (const char c : value) {
                if (c == '"' || c == '\\')
                    out += '\\';
                out += c;
            }
            return out;
        }

        void WriteNode(std::ostringstream &out, const LayoutNode &node);

        /**
         * @brief Serializes a panel node as JSON into out.
         */
        void WritePanelNode(std::ostringstream &out, const PanelNode &value) {
            out << R"({"type":"panel","id":")" << Escape(value.id) << R"(","panel":")" << Escape(value.panel) << R"("})";
        }

        /**
         * @brief Serializes a tab stack node as JSON into out.
         */
        void WriteTabStackNode(std::ostringstream &out, const TabStackNode &value) {
            out << R"({"type":"stack","id":")" << Escape(value.id) << R"(","tabs":[)";
            for (std::size_t i = 0; i < value.tabs.size(); ++i) {
                if (i)
                    out << ',';
                out << '"' << Escape(value.tabs[i]) << '"';
            }
            out << R"(],"active":")" << Escape(value.activeTab.value_or("")) << R"("})";
        }

        void WriteSplitNode(std::ostringstream &out, const SplitNode &value) {
            out << R"({"type":"split","id":")" << Escape(value.id) << R"(","axis":")"
                << (value.axis == WorkspaceSplitAxis::Vertical ? "vertical" : "horizontal") << R"(","ratio":)" << value.ratio
                << R"(,"first":)";
            WriteNode(out, *value.first);
            out << R"(,"second":)";
            WriteNode(out, *value.second);
            out << '}';
        }

        void WriteNode(std::ostringstream &out, const LayoutNode &node) {
            std::visit([&out]<typename T>(const T &value) {
                if constexpr (std::is_same_v<T, PanelNode>)
                    WritePanelNode(out, value);
                else if constexpr (std::is_same_v<T, TabStackNode>)
                    WriteTabStackNode(out, value);
                else
                    WriteSplitNode(out, value);
            }, node.value);
        }

    }  // namespace

    std::string WorkspaceLayoutPersistence::Serialize(const WorkspaceLayout &layout) {
        std::ostringstream out;
        out << "{\"schemaVersion\":" << CurrentSchemaVersion << ",\"root\":";
        WriteNode(out, layout.root);
        out << R"(,"documents":[)";
        for (std::size_t index = 0; index < layout.openDocuments.size(); ++index) {
            if (index)
                out << ',';
            const SerializedDocumentOpenKey &document = layout.openDocuments[index];
            out << R"({"kind":")" << Escape(document.kind) << R"(","source":")" << Escape(document.source) << R"("})";
        }
        out << ']';
        out << '}';
        return out.str();
    }

    std::optional<WorkspaceLayout> WorkspaceLayoutPersistence::Deserialize(const std::string_view json, std::string *error) {
        std::string local;
        Parser parser(json);
        auto result = parser.Parse(local);
        if (!result && error)
            *error = local;
        return result;
    }

    bool WorkspaceLayoutPersistence::Save(const std::filesystem::path &path, const WorkspaceLayout &layout, std::string *error) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            if (error)
                *error = ec.message();
            return false;
        }
        const std::filesystem::path temp = path.string() + ".tmp";
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << Serialize(layout);
        out.close();
        if (!out) {
            if (error)
                *error = "workspace write failed";
            return false;
        }
        std::filesystem::rename(temp, path, ec);
        if (ec) {
            std::filesystem::remove(path, ec);
            std::filesystem::rename(temp, path, ec);
        }
        if (ec && error)
            *error = ec.message();
        return !ec;
    }

    std::optional<WorkspaceLayout> WorkspaceLayoutPersistence::Load(const std::filesystem::path &path, std::string *error) {
        const std::ifstream in(path, std::ios::binary);
        if (!in) {
            if (error)
                *error = "workspace file not found";
            return std::nullopt;
        }
        std::stringstream buffer;
        buffer << in.rdbuf();
        return Deserialize(buffer.str(), error);
    }
}  // namespace Horo::Editor
