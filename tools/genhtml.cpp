// genhtml.cpp - converts docs/tutorial/tutorial.md to a single self-contained
// build/tutorial.html, with every image base64-embedded as a data: URI.  No
// external dependencies, no link-time deps beyond the standard library, no
// pandoc.
//
// Build & run from the repo root:
//   c++ -O2 -std=c++17 -o build/genhtml tools/genhtml.cpp
//   ./build/genhtml          # writes build/tutorial.html
//
// Unlike the other offline tools in this directory, genhtml's output is NOT
// committed - tutorial.html is a build artefact, picked up from build/ by
// downstream packaging (CPack, web/build.sh, etc.).  The tool is still
// offline (not wired into CMake); run it by hand whenever tutorial.md or the
// PNGs under docs/tutorial/img/ change.
//
// The markdown subset is intentionally narrow (everything tutorial.md uses,
// nothing it doesn't):
//
//   - ATX headings #, ##, ###
//   - paragraphs separated by blank lines
//   - **bold**, _italic_, `code`
//   - [text](url), ![alt](path) - the latter base64-embeds the file
//   - unordered list (- item) and ordered list (N. item)
//   - GFM pipe tables with a |---| separator row
//   - horizontal rule (--- alone on a line)
//
// Each delimiter has exactly one meaning - * only appears inside **bold**,
// _ only inside _italic_ - so the inline tokenizer is a simple one-pass
// scanner with no precedence or backtracking.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// --- File I/O --------------------------------------------------------------
std::string readText(const std::string& path) {
  std::ifstream f(path);
  if (!f) { std::fprintf(stderr, "cannot read %s\n", path.c_str()); std::exit(1); }
  std::stringstream ss; ss << f.rdbuf();
  return ss.str();
}

std::vector<uint8_t> readBytes(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { std::fprintf(stderr, "cannot read %s\n", path.c_str()); std::exit(1); }
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                              std::istreambuf_iterator<char>());
}

void writeText(const std::string& path, const std::string& content) {
  std::ofstream f(path);
  if (!f) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); std::exit(1); }
  f << content;
}

// --- Base64 ----------------------------------------------------------------
std::string base64(const std::vector<uint8_t>& bytes) {
  static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                          "0123456789+/";
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  for (size_t i = 0; i < bytes.size(); i += 3) {
    int n = int(std::min<size_t>(3, bytes.size() - i));
    uint32_t v = 0;
    for (int j = 0; j < n; ++j) v |= uint32_t(bytes[i + j]) << (16 - 8 * j);
    out += A[(v >> 18) & 0x3f];
    out += A[(v >> 12) & 0x3f];
    out += (n >= 2) ? A[(v >> 6) & 0x3f] : '=';
    out += (n >= 3) ? A[v & 0x3f]        : '=';
  }
  return out;
}

// --- HTML escape -----------------------------------------------------------
std::string esc(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '&':  out += "&amp;";  break;
      case '"':  out += "&quot;"; break;
      default:   out += c;
    }
  }
  return out;
}

// Append one literal text character to `out`, HTML-escaping the four
// metacharacters.
void appendChar(std::string& out, char c) {
  switch (c) {
    case '<':  out += "&lt;";  break;
    case '>':  out += "&gt;";  break;
    case '&':  out += "&amp;"; break;
    default:   out += c;
  }
}

// --- Inline parser ---------------------------------------------------------
std::string renderInline(const std::string& text, const std::string& imgDir);

// Collapse runs of whitespace to a single space.  Used to clean up multi-line
// image alt-text before it becomes an attribute value.
std::string flattenWs(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  bool inWs = false;
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      if (!inWs && !out.empty()) { out += ' '; inWs = true; }
    } else {
      out += c; inWs = false;
    }
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

std::string renderInline(const std::string& text, const std::string& imgDir) {
  std::string out;
  out.reserve(text.size() + 64);
  size_t i = 0;
  while (i < text.size()) {
    char c = text[i];

    // Backslash escape: \X emits X literally (HTML-escaping it if needed).
    if (c == '\\' && i + 1 < text.size()) {
      appendChar(out, text[i + 1]);
      i += 2;
      continue;
    }

    // Bold: **...**.  Inner span is parsed recursively so [code](nested)
    // and friends still work, but we never have nested bold in practice.
    if (c == '*' && i + 1 < text.size() && text[i + 1] == '*') {
      size_t close = text.find("**", i + 2);
      if (close != std::string::npos) {
        out += "<strong>";
        out += renderInline(text.substr(i + 2, close - (i + 2)), imgDir);
        out += "</strong>";
        i = close + 2;
        continue;
      }
    }

    // Italic: _..._.  Recursive on the inner for the same reason as bold.
    if (c == '_') {
      size_t close = text.find('_', i + 1);
      if (close != std::string::npos) {
        out += "<em>";
        out += renderInline(text.substr(i + 1, close - (i + 1)), imgDir);
        out += "</em>";
        i = close + 1;
        continue;
      }
    }

    // Code: `...`.  Content is *not* recursively parsed - it's a verbatim
    // span, only HTML-escaped.
    if (c == '`') {
      size_t close = text.find('`', i + 1);
      if (close != std::string::npos) {
        out += "<code>";
        out += esc(text.substr(i + 1, close - (i + 1)));
        out += "</code>";
        i = close + 1;
        continue;
      }
    }

    // Image: ![alt](path).
    if (c == '!' && i + 1 < text.size() && text[i + 1] == '[') {
      size_t rb = text.find(']', i + 2);
      if (rb != std::string::npos && rb + 1 < text.size() && text[rb + 1] == '(') {
        size_t rp = text.find(')', rb + 2);
        if (rp != std::string::npos) {
          std::string alt  = text.substr(i + 2, rb - (i + 2));
          std::string path = text.substr(rb + 2, rp - (rb + 2));
          std::vector<uint8_t> bytes = readBytes(imgDir + "/" + path);
          out += "<img src=\"data:image/png;base64,";
          out += base64(bytes);
          out += "\" alt=\"";
          out += esc(flattenWs(alt));
          out += "\">";
          i = rp + 1;
          continue;
        }
      }
    }

    // Link: [text](url).  Link text is recursively parsed.
    if (c == '[') {
      size_t rb = text.find(']', i + 1);
      if (rb != std::string::npos && rb + 1 < text.size() && text[rb + 1] == '(') {
        size_t rp = text.find(')', rb + 2);
        if (rp != std::string::npos) {
          std::string lt  = text.substr(i + 1, rb - (i + 1));
          std::string url = text.substr(rb + 2, rp - (rb + 2));
          out += "<a href=\"";
          out += esc(url);
          out += "\">";
          out += renderInline(lt, imgDir);
          out += "</a>";
          i = rp + 1;
          continue;
        }
      }
    }

    // Plain text.
    appendChar(out, c);
    ++i;
  }
  return out;
}

// --- Block parser ----------------------------------------------------------
//
// Walks the markdown line by line and produces a flat list of typed blocks.
// Consecutive list items and table rows stay flat; the emitter groups them.
struct Block {
  enum Kind { HEADING, PARA, UL_ITEM, OL_ITEM, TABLE_ROW, TABLE_SEP, HR };
  Kind kind = PARA;
  int  level = 0;          // heading level (1..3)
  std::string text;        // raw markdown text of the block
};

bool isBlank(const std::string& line) {
  return line.find_first_not_of(" \t") == std::string::npos;
}

// `---` on its own line.
bool isHr(const std::string& line) {
  if (isBlank(line)) return false;
  int dashes = 0;
  for (char c : line) {
    if (c == '-') ++dashes;
    else if (c != ' ' && c != '\t') return false;
  }
  return dashes >= 3;
}

// A table separator row: `|---|---|...|` (no setext underlines because we
// don't accept setext).
bool isTableSep(const std::string& line) {
  if (line.find('|') == std::string::npos) return false;
  for (char c : line) {
    if (c != '|' && c != '-' && c != ':' && c != ' ' && c != '\t') return false;
  }
  return true;
}

// Match `N. ` at the start of a line.  Returns the length of the marker
// (digits + dot + space) or 0 if not an ordered-list line.
size_t olMarkerLen(const std::string& line) {
  size_t k = 0;
  while (k < line.size() && std::isdigit((unsigned char)line[k])) ++k;
  if (k > 0 && k + 1 < line.size() && line[k] == '.' && line[k + 1] == ' ')
    return k + 2;
  return 0;
}

// True if `line` begins a new block (so the current list item or paragraph
// should close before processing it).
bool startsNewBlock(const std::string& line) {
  if (isBlank(line)) return true;
  if (line[0] == '#') return true;
  if (line[0] == '|') return true;
  if (isHr(line))     return true;
  if (line.size() >= 2 && line[0] == '-' && line[1] == ' ') return true;
  if (olMarkerLen(line) > 0) return true;
  return false;
}

// Split a string by '\n' into lines (excluding the newlines).
std::vector<std::string> splitLines(const std::string& s) {
  std::vector<std::string> out;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == '\n') {
      std::string line = s.substr(start, i - start);
      // Strip trailing CR for CRLF input.
      if (!line.empty() && line.back() == '\r') line.pop_back();
      out.push_back(line);
      start = i + 1;
    }
  }
  return out;
}

std::vector<Block> parseBlocks(const std::string& md) {
  std::vector<Block> blocks;
  std::vector<std::string> lines = splitLines(md);

  size_t i = 0;
  while (i < lines.size()) {
    const std::string& line = lines[i];

    if (isBlank(line)) { ++i; continue; }

    // ATX heading.
    if (line[0] == '#') {
      int level = 0;
      while (level < (int)line.size() && line[level] == '#') ++level;
      if (level <= 3 && level < (int)line.size() && line[level] == ' ') {
        Block b; b.kind = Block::HEADING; b.level = level;
        b.text = line.substr(level + 1);
        blocks.push_back(b);
        ++i;
        continue;
      }
    }

    // Horizontal rule (must come before table-row check because `---` alone
    // could be confused with anything, but we know it has no `|`).
    if (isHr(line)) {
      Block b; b.kind = Block::HR;
      blocks.push_back(b);
      ++i;
      continue;
    }

    // Table row.
    if (line[0] == '|') {
      Block b;
      b.kind = isTableSep(line) ? Block::TABLE_SEP : Block::TABLE_ROW;
      b.text = line;
      blocks.push_back(b);
      ++i;
      continue;
    }

    // Unordered list item.
    if (line.size() >= 2 && line[0] == '-' && line[1] == ' ') {
      Block b; b.kind = Block::UL_ITEM; b.text = line.substr(2);
      ++i;
      // Consume continuation lines (non-blank, not the start of any new block).
      while (i < lines.size() && !startsNewBlock(lines[i])) {
        size_t lws = lines[i].find_first_not_of(" \t");
        b.text += ' ';
        if (lws != std::string::npos) b.text += lines[i].substr(lws);
        ++i;
      }
      blocks.push_back(b);
      continue;
    }

    // Ordered list item.
    if (size_t mlen = olMarkerLen(line)) {
      Block b; b.kind = Block::OL_ITEM; b.text = line.substr(mlen);
      ++i;
      while (i < lines.size() && !startsNewBlock(lines[i])) {
        size_t lws = lines[i].find_first_not_of(" \t");
        b.text += ' ';
        if (lws != std::string::npos) b.text += lines[i].substr(lws);
        ++i;
      }
      blocks.push_back(b);
      continue;
    }

    // Paragraph: consume non-blank lines until a new block starts.
    Block b; b.kind = Block::PARA; b.text = line;
    ++i;
    while (i < lines.size() && !startsNewBlock(lines[i])) {
      b.text += ' ';
      b.text += lines[i];
      ++i;
    }
    blocks.push_back(b);
  }
  return blocks;
}

// --- Table cell splitter ---------------------------------------------------
// Splits a pipe-table row "| a | b | c |" into ["a", "b", "c"].
std::vector<std::string> splitCells(const std::string& row) {
  std::vector<std::string> cells;
  std::string cur;
  bool sawBar = false;
  for (size_t i = 0; i < row.size(); ++i) {
    char c = row[i];
    if (c == '\\' && i + 1 < row.size() && row[i + 1] == '|') {
      cur += '|'; ++i; continue;
    }
    if (c == '|') {
      if (sawBar) {
        size_t s = cur.find_first_not_of(" \t");
        size_t e = cur.find_last_not_of(" \t");
        cells.push_back(s == std::string::npos
                          ? std::string()
                          : cur.substr(s, e - s + 1));
      }
      cur.clear();
      sawBar = true;
    } else {
      cur += c;
    }
  }
  return cells;
}

// --- HTML emitter ----------------------------------------------------------
std::string emitBody(const std::vector<Block>& blocks,
                     const std::string& imgDir,
                     std::string& titleOut) {
  std::string out;

  enum { NONE, IN_UL, IN_OL, IN_TBL_HEAD, IN_TBL_BODY } state = NONE;
  auto close = [&]() {
    switch (state) {
      case IN_UL:       out += "</ul>\n"; break;
      case IN_OL:       out += "</ol>\n"; break;
      case IN_TBL_HEAD: out += "</thead></table>\n"; break;     // (degenerate)
      case IN_TBL_BODY: out += "</tbody></table>\n"; break;
      case NONE: break;
    }
    state = NONE;
  };

  for (const Block& b : blocks) {
    // State transitions.
    if (b.kind == Block::UL_ITEM) {
      if (state != IN_UL) { close(); out += "<ul>\n"; state = IN_UL; }
    } else if (b.kind == Block::OL_ITEM) {
      if (state != IN_OL) { close(); out += "<ol>\n"; state = IN_OL; }
    } else if (b.kind == Block::TABLE_ROW) {
      if (state == NONE) {
        out += "<table>\n<thead>\n";
        state = IN_TBL_HEAD;
      }
      // (If we're already in head, the next row would also be head; the SEP
      // line is what flips us to body.)
    } else if (b.kind == Block::TABLE_SEP) {
      if (state == IN_TBL_HEAD) {
        out += "</thead>\n<tbody>\n";
        state = IN_TBL_BODY;
      }
      continue;
    } else {
      close();
    }

    switch (b.kind) {
      case Block::HEADING: {
        if (b.level == 1 && titleOut.empty()) titleOut = b.text;
        std::string lvl = std::to_string(b.level);
        out += "<h" + lvl + ">";
        out += renderInline(b.text, imgDir);
        out += "</h" + lvl + ">\n";
        break;
      }
      case Block::PARA:
        out += "<p>";
        out += renderInline(b.text, imgDir);
        out += "</p>\n";
        break;
      case Block::UL_ITEM:
      case Block::OL_ITEM:
        out += "<li>";
        out += renderInline(b.text, imgDir);
        out += "</li>\n";
        break;
      case Block::TABLE_ROW: {
        auto cells = splitCells(b.text);
        const char* tag = (state == IN_TBL_HEAD) ? "th" : "td";
        out += "<tr>";
        for (auto& c : cells) {
          out += "<"; out += tag; out += ">";
          out += renderInline(c, imgDir);
          out += "</"; out += tag; out += ">";
        }
        out += "</tr>\n";
        break;
      }
      case Block::HR:        out += "<hr>\n"; break;
      case Block::TABLE_SEP: break;
    }
  }
  close();
  return out;
}

// --- Embedded stylesheet ---------------------------------------------------
// Designed for comfortable on-screen reading: narrow column, generous
// line-height, wood-toned highlights that pick up the in-game palette.  Made
// to look right both standalone and at narrow widths.
const char* CSS = R"CSS(
  html { -webkit-text-size-adjust: 100%; }
  body {
    font-family: -apple-system, "Helvetica Neue", "Segoe UI", system-ui,
                 sans-serif;
    color: #2a2620;
    background: #faf8f3;
    line-height: 1.6;
    margin: 0;
    padding: 0;
  }
  main {
    max-width: 40em;
    margin: 2.5em auto;
    padding: 0 1.4em;
  }
  h1, h2, h3 {
    color: #1a1611;
    line-height: 1.25;
    margin: 2em 0 0.5em;
  }
  h1 {
    font-size: 2em;
    margin-top: 0;
    border-bottom: 2px solid #6e4f1f;
    padding-bottom: 0.3em;
  }
  h2 {
    font-size: 1.4em;
    border-bottom: 1px solid #ddd4c2;
    padding-bottom: 0.2em;
  }
  h3 { font-size: 1.12em; color: #45382a; }
  p  { margin: 0.85em 0; }
  img {
    max-width: 100%;
    height: auto;
    display: block;
    margin: 1.2em auto;
  }
  table {
    border-collapse: collapse;
    margin: 1.2em auto;
    font-size: 0.95em;
  }
  th, td {
    border: 1px solid #c9bfa6;
    padding: 0.45em 0.85em;
    text-align: left;
    vertical-align: middle;
  }
  th { background: #ede4cb; font-weight: 600; }
  td img { margin: 0 auto; max-height: 3em; }
  code {
    background: #ece5d0;
    padding: 0.08em 0.32em;
    border-radius: 3px;
    font-family: "SF Mono", Menlo, Consolas, monospace;
    font-size: 0.92em;
  }
  hr { border: none; border-top: 1px solid #c9bfa6; margin: 2.5em 0; }
  a { color: #6e4f1f; text-decoration: none; }
  a:hover { text-decoration: underline; }
  em { font-style: italic; }
  strong { font-weight: 600; }
  ul, ol { padding-left: 1.5em; }
  li { margin: 0.35em 0; }
)CSS";

}  // namespace

int main() {
  std::printf("genhtml: docs/tutorial/tutorial.md -> build/tutorial.html\n");

  std::string md = readText("docs/tutorial/tutorial.md");
  auto blocks = parseBlocks(md);

  std::string title;
  std::string body = emitBody(blocks, "docs/tutorial", title);
  if (title.empty()) title = "Shogi tutorial";

  std::string html;
  html += "<!doctype html>\n";
  html += "<html lang=\"en\">\n<head>\n";
  html += "<meta charset=\"utf-8\">\n";
  html += "<meta name=\"viewport\" "
          "content=\"width=device-width, initial-scale=1\">\n";
  html += "<title>" + esc(title) + "</title>\n";
  html += "<style>";
  html += CSS;
  html += "</style>\n";
  html += "</head>\n<body>\n<main>\n";
  html += body;
  html += "</main>\n</body>\n</html>\n";

  writeText("build/tutorial.html", html);
  std::printf("wrote build/tutorial.html (%zu bytes)\n", html.size());
  return 0;
}
