#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace bass2midi::song::detail
{
    // Small XML DOM for the GPIF score: elements, attributes, text and CDATA (concatenated, entities
    // decoded), comments/processing instructions/doctype skipped. No namespaces, no validation.
    struct XmlNode
    {
        std::string name;
        std::map<std::string, std::string> attributes;
        std::string text;
        std::vector<std::unique_ptr<XmlNode>> children;

        const XmlNode* child (const std::string& childName) const;
        std::vector<const XmlNode*> childrenNamed (const std::string& childName) const;
        // Text of a direct child, trimmed; empty if absent.
        std::string childText (const std::string& childName) const;
        std::string trimmedText() const;
        std::string attribute (const std::string& attributeName) const;
    };

    std::unique_ptr<XmlNode> parseXml (const std::string& document, std::string& error);
}
