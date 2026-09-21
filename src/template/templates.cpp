// Custom-element expansion and template rendering.
//
// Two layers: a small HTML scanner that finds registered custom-element tags
// and hands their attributes and children to the registered renderer, and inja
// on top of it for {{ }} substitution. inja lives here and nowhere else, which
// is what keeps it out of the public headers.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <inja/inja.hpp>

#include <imhtml/context.hpp>
#include <imhtml/core.hpp>

#include "internal/core_internal.hpp"

namespace ImHTML {

namespace {

bool IsAttributeSpace(char value) { return std::isspace(static_cast<unsigned char>(value)) != 0; }

void ReplaceAll(std::string& value, std::string_view from, std::string_view to) {
  std::size_t offset = 0;
  while ((offset = value.find(from, offset)) != std::string::npos) {
    value.replace(offset, from.size(), to);
    offset += to.size();
  }
}

std::string DecodeAttributeEntities(std::string value) {
  ReplaceAll(value, "&amp;", "&");
  ReplaceAll(value, "&quot;", "\"");
  ReplaceAll(value, "&apos;", "'");
  ReplaceAll(value, "&lt;", "<");
  ReplaceAll(value, "&gt;", ">");
  return value;
}

std::map<std::string, std::string> ParseCustomAttributes(std::string_view source) {
  std::map<std::string, std::string> attributes;
  std::size_t offset = 0;
  while (offset < source.size()) {
    while (offset < source.size() && (IsAttributeSpace(source[offset]) || source[offset] == '/')) ++offset;
    if (offset >= source.size()) break;

    const std::size_t name_start = offset;
    while (offset < source.size() && !IsAttributeSpace(source[offset]) && source[offset] != '=' && source[offset] != '/') {
      ++offset;
    }
    const std::string name(source.substr(name_start, offset - name_start));
    while (offset < source.size() && IsAttributeSpace(source[offset])) ++offset;

    std::string value;
    bool has_value = false;
    if (offset < source.size() && source[offset] == '=') {
      has_value = true;
      ++offset;
      while (offset < source.size() && IsAttributeSpace(source[offset])) ++offset;
      if (offset < source.size() && (source[offset] == '\"' || source[offset] == '\'')) {
        const char quote = source[offset++];
        const std::size_t value_start = offset;
        while (offset < source.size() && source[offset] != quote) ++offset;
        value = std::string(source.substr(value_start, offset - value_start));
        if (offset < source.size()) ++offset;
      } else {
        const std::size_t value_start = offset;
        while (offset < source.size() && !IsAttributeSpace(source[offset])) ++offset;
        value = std::string(source.substr(value_start, offset - value_start));
      }
    }
    if (!has_value) value = "true";
    if (!name.empty()) attributes.emplace(name, DecodeAttributeEntities(std::move(value)));
  }
  return attributes;
}

std::size_t FindTagEnd(std::string_view source, std::size_t offset) {
  char quote = 0;
  for (; offset < source.size(); ++offset) {
    const char value = source[offset];
    if (quote != 0) {
      if (value == quote) quote = 0;
    } else if (value == '\"' || value == '\'') {
      quote = value;
    } else if (value == '>') {
      return offset;
    }
  }
  return std::string::npos;
}

struct ParsedTag {
  std::string name;
  std::map<std::string, std::string> attributes;
  std::size_t end = std::string::npos;
  bool closing = false;
  bool self_closing = false;
};

bool IsVoidTag(std::string_view tag) {
  for (const std::string_view name : {"area", "base", "br", "col", "embed", "hr", "img", "input", "link",
                                      "meta", "param", "source", "track", "wbr"}) {
    if (tag == name) return true;
  }
  return false;
}

bool ParseTag(std::string_view source, std::size_t start, ParsedTag& result) {
  if (start >= source.size() || source[start] != '<') return false;
  const std::size_t end = FindTagEnd(source, start + 1);
  if (end == std::string::npos) return false;

  std::size_t offset = start + 1;
  if (offset < end && source[offset] == '/') {
    result.closing = true;
    ++offset;
  }
  while (offset < end && IsAttributeSpace(source[offset])) ++offset;
  if (offset == end || source[offset] == '!' || source[offset] == '?') return false;

  const std::size_t name_start = offset;
  while (offset < end && !IsAttributeSpace(source[offset]) && source[offset] != '/' && source[offset] != '>') ++offset;
  if (offset == name_start) return false;
  result.name = source.substr(name_start, offset - name_start);
  result.end = end;
  if (result.closing) return true;

  std::size_t attributes_end = end;
  while (attributes_end > offset && IsAttributeSpace(source[attributes_end - 1])) --attributes_end;
  result.self_closing = attributes_end > offset && source[attributes_end - 1] == '/';
  if (result.self_closing) {
    --attributes_end;
    while (attributes_end > offset && IsAttributeSpace(source[attributes_end - 1])) --attributes_end;
  }
  result.attributes = ParseCustomAttributes(std::string_view(source).substr(offset, attributes_end - offset));
  return true;
}

std::size_t FindMatchingTag(std::string_view source, const ParsedTag& opening) {
  if (opening.self_closing || IsVoidTag(opening.name)) return std::string::npos;

  int depth = 1;
  std::size_t search = opening.end + 1;
  while ((search = source.find('<', search)) != std::string::npos) {
    ParsedTag candidate;
    if (!ParseTag(source, search, candidate)) {
      ++search;
      continue;
    }
    if (candidate.name == opening.name) {
      if (candidate.closing) {
        if (--depth == 0) return search;
      } else if (!candidate.self_closing && !IsVoidTag(candidate.name)) {
        ++depth;
      }
    }
    search = candidate.end + 1;
  }
  return std::string::npos;
}

std::string ExpandCustomElementRange(std::string_view source, std::string_view parent_tag,
                                     const std::map<std::string, std::string>* parent_attributes) {
  std::string result;
  std::size_t cursor = 0;
  while (cursor < source.size()) {
    const std::size_t open = source.find('<', cursor);
    if (open == std::string_view::npos) {
      result.append(source.substr(cursor));
      break;
    }
    result.append(source.substr(cursor, open - cursor));

    ParsedTag tag;
    const std::size_t source_open = open;
    if (!ParseTag(source, source_open, tag)) {
      result.push_back(source[open]);
      cursor = open + 1;
      continue;
    }
    const std::size_t tag_size = tag.end - source_open + 1;
    const CustomElementHtmlFunction* custom = CurrentContext().find_custom_element_html(tag.name);
    if (tag.closing || custom == nullptr) {
      if (tag.closing || tag.self_closing || IsVoidTag(tag.name)) {
        result.append(source.substr(open, tag_size));
        cursor = open + tag_size;
        continue;
      }

      const std::size_t close = FindMatchingTag(source, tag);
      if (close == std::string::npos) {
        result.append(source.substr(open));
        break;
      }
      ParsedTag closing;
      if (!ParseTag(source, close, closing)) {
        result.append(source.substr(open));
        break;
      }
      result.append(source.substr(open, tag_size));
      result += ExpandCustomElementRange(source.substr(open + tag_size, close - open - tag_size), tag.name,
                                         &tag.attributes);
      const std::size_t close_size = closing.end - close + 1;
      result.append(source.substr(close, close_size));
      cursor = close + close_size;
      continue;
    }

    std::string_view children;
    std::size_t replacement_end = open + tag_size;
    if (!tag.self_closing && !IsVoidTag(tag.name)) {
      const std::size_t close = FindMatchingTag(source, tag);
      if (close == std::string::npos) {
        result.append(source.substr(open));
        break;
      }
      ParsedTag closing;
      if (!ParseTag(source, close, closing)) {
        result.append(source.substr(open));
        break;
      }
      children = source.substr(open + tag_size, close - open - tag_size);
      replacement_end = closing.end + 1;
    }

    const std::string expanded_children =
        ExpandCustomElementRange(children, tag.name, &tag.attributes);
    const HtmlElementContext context{parent_tag, parent_attributes};
    const std::string replacement = (*custom)(tag.attributes, expanded_children, context);
    result += replacement;
    cursor = replacement_end;
  }
  return result;
}
}  // namespace

std::string ExpandCustomElements(const std::string& html) {
  std::string expanded = html;
  for (int pass = 0; pass < 8; ++pass) {
    const std::string before = expanded;
    expanded = ExpandCustomElementRange(expanded, {}, nullptr);
    const bool changed = expanded != before;
    if (!changed) break;
  }
  return expanded;
}

std::string ExpandHtmlTemplate(std::string_view html_template,
                               const std::map<std::string, std::string>& attributes) {
  return ExpandHtmlTemplate(html_template, attributes, {});
}

namespace {
struct InjaTemplateCache {
  inja::Environment environment;
  std::unordered_map<std::string, inja::Template> templates;

  InjaTemplateCache() { environment.set_html_autoescape(false); }
};

InjaTemplateCache& GetInjaTemplateCache() {
  static InjaTemplateCache cache;
  return cache;
}

const inja::Template& GetInjaTemplate(std::string_view source) {
  InjaTemplateCache& cache = GetInjaTemplateCache();
  const std::string key(source);
  const auto found = cache.templates.find(key);
  if (found != cache.templates.end()) return found->second;
  return cache.templates.emplace(key, cache.environment.parse(source)).first->second;
}
}  // namespace

void PrepareHtmlTemplate(std::string_view html_template) { (void)GetInjaTemplate(html_template); }

std::string ExpandHtmlTemplate(std::string_view html_template, const inja::json& data) {
  return GetInjaTemplateCache().environment.render(GetInjaTemplate(html_template), data);
}

std::string ExpandHtmlTemplate(std::string_view html_template,
                               const std::map<std::string, std::string>& attributes,
                               std::string_view children) {
  inja::json data = inja::json::object();
  for (const auto& [name, value] : attributes) data[name] = value;
  data["children"] = std::string(children);
  return ExpandHtmlTemplate(html_template, data);
}

}  // namespace ImHTML
