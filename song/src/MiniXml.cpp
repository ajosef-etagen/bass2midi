#include "MiniXml.h"

#include <cctype>
#include <cstring>

namespace bass2midi::song::detail
{
    namespace
    {
        std::string trim (const std::string& s)
        {
            std::size_t a = 0, b = s.size();
            while (a < b && std::isspace (static_cast<unsigned char> (s[a])))
                ++a;
            while (b > a && std::isspace (static_cast<unsigned char> (s[b - 1])))
                --b;
            return s.substr (a, b - a);
        }

        std::string decodeEntities (const std::string& s)
        {
            std::string out;
            out.reserve (s.size());
            for (std::size_t i = 0; i < s.size(); ++i)
            {
                if (s[i] != '&')
                {
                    out += s[i];
                    continue;
                }
                const auto end = s.find (';', i);
                if (end == std::string::npos)
                {
                    out += s[i];
                    continue;
                }
                const auto entity = s.substr (i + 1, end - i - 1);
                if (entity == "amp") out += '&';
                else if (entity == "lt") out += '<';
                else if (entity == "gt") out += '>';
                else if (entity == "quot") out += '"';
                else if (entity == "apos") out += '\'';
                else if (! entity.empty() && entity[0] == '#')
                {
                    const long code = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X')
                                          ? std::strtol (entity.c_str() + 2, nullptr, 16)
                                          : std::strtol (entity.c_str() + 1, nullptr, 10);
                    if (code > 0 && code < 128)
                        out += static_cast<char> (code); // non-ASCII is irrelevant for the score data used here
                }
                else
                {
                    out += s.substr (i, end - i + 1);
                }
                i = end;
            }
            return out;
        }

        struct Parser
        {
            const std::string& s;
            std::size_t p = 0;
            std::string error;
            int depth = 0;
            static constexpr int maxDepth = 256; // GPIF nests about 10 levels; bounds the recursion

            bool startsWith (const char* token) const { return s.compare (p, std::strlen (token), token) == 0; }

            bool skipUntil (const char* token)
            {
                const auto found = s.find (token, p);
                if (found == std::string::npos)
                {
                    error = std::string ("unterminated construct, expected ") + token;
                    return false;
                }
                p = found + std::strlen (token);
                return true;
            }

            std::string readName()
            {
                const auto start = p;
                while (p < s.size() && ! std::isspace (static_cast<unsigned char> (s[p])) && s[p] != '>' && s[p] != '/' && s[p] != '=')
                    ++p;
                return s.substr (start, p - start);
            }

            void skipSpace()
            {
                while (p < s.size() && std::isspace (static_cast<unsigned char> (s[p])))
                    ++p;
            }

            // Parses an element starting at '<' (not a closing tag).
            std::unique_ptr<XmlNode> element()
            {
                if (depth >= maxDepth)
                {
                    error = "XML nested too deeply";
                    return nullptr;
                }
                ++depth;
                auto node = element (depth);
                --depth;
                return node;
            }

            std::unique_ptr<XmlNode> element (int)
            {
                auto node = std::make_unique<XmlNode>();
                ++p; // '<'
                node->name = readName();

                while (true)
                {
                    skipSpace();
                    if (p >= s.size())
                    {
                        error = "unexpected end inside <" + node->name + ">";
                        return nullptr;
                    }
                    if (startsWith ("/>"))
                    {
                        p += 2;
                        return node;
                    }
                    if (s[p] == '>')
                    {
                        ++p;
                        break;
                    }
                    const auto attributeName = readName();
                    skipSpace();
                    if (p >= s.size() || s[p] != '=')
                    {
                        error = "malformed attribute in <" + node->name + ">";
                        return nullptr;
                    }
                    ++p;
                    skipSpace();
                    if (p >= s.size() || (s[p] != '"' && s[p] != '\''))
                    {
                        error = "unquoted attribute in <" + node->name + ">";
                        return nullptr;
                    }
                    const char quote = s[p++];
                    const auto end = s.find (quote, p);
                    if (end == std::string::npos)
                    {
                        error = "unterminated attribute value";
                        return nullptr;
                    }
                    node->attributes[attributeName] = decodeEntities (s.substr (p, end - p));
                    p = end + 1;
                }

                // Content until the matching closing tag.
                while (p < s.size())
                {
                    if (startsWith ("</"))
                    {
                        p += 2;
                        const auto closing = readName();
                        if (closing != node->name)
                        {
                            error = "mismatched </" + closing + "> for <" + node->name + ">";
                            return nullptr;
                        }
                        if (! skipUntil (">"))
                            return nullptr;
                        return node;
                    }
                    if (startsWith ("<![CDATA["))
                    {
                        p += 9;
                        const auto end = s.find ("]]>", p);
                        if (end == std::string::npos)
                        {
                            error = "unterminated CDATA";
                            return nullptr;
                        }
                        node->text += s.substr (p, end - p);
                        p = end + 3;
                        continue;
                    }
                    if (startsWith ("<!--"))
                    {
                        if (! skipUntil ("-->"))
                            return nullptr;
                        continue;
                    }
                    if (startsWith ("<?"))
                    {
                        if (! skipUntil ("?>"))
                            return nullptr;
                        continue;
                    }
                    if (s[p] == '<')
                    {
                        auto child = element();
                        if (child == nullptr)
                            return nullptr;
                        node->children.push_back (std::move (child));
                        continue;
                    }
                    const auto next = s.find ('<', p);
                    const auto end = next == std::string::npos ? s.size() : next;
                    node->text += decodeEntities (s.substr (p, end - p));
                    p = end;
                }

                error = "unexpected end of document inside <" + node->name + ">";
                return nullptr;
            }
        };
    }

    const XmlNode* XmlNode::child (const std::string& childName) const
    {
        for (const auto& c : children)
            if (c->name == childName)
                return c.get();
        return nullptr;
    }

    std::vector<const XmlNode*> XmlNode::childrenNamed (const std::string& childName) const
    {
        std::vector<const XmlNode*> out;
        for (const auto& c : children)
            if (c->name == childName)
                out.push_back (c.get());
        return out;
    }

    std::string XmlNode::childText (const std::string& childName) const
    {
        const auto* c = child (childName);
        return c != nullptr ? c->trimmedText() : std::string();
    }

    std::string XmlNode::trimmedText() const { return trim (text); }

    std::string XmlNode::attribute (const std::string& attributeName) const
    {
        const auto it = attributes.find (attributeName);
        return it != attributes.end() ? it->second : std::string();
    }

    std::unique_ptr<XmlNode> parseXml (const std::string& document, std::string& error)
    {
        Parser parser { document, 0, {}, 0 };
        while (parser.p < document.size())
        {
            parser.skipSpace();
            if (parser.startsWith ("<?"))
            {
                if (! parser.skipUntil ("?>"))
                    break;
            }
            else if (parser.startsWith ("<!--"))
            {
                if (! parser.skipUntil ("-->"))
                    break;
            }
            else if (parser.startsWith ("<!"))
            {
                if (! parser.skipUntil (">"))
                    break;
            }
            else if (parser.p < document.size() && document[parser.p] == '<')
            {
                auto root = parser.element();
                if (root == nullptr)
                    break;
                return root;
            }
            else
            {
                parser.error = "no root element";
                break;
            }
        }
        error = parser.error.empty() ? "empty XML document" : parser.error;
        return nullptr;
    }
}
