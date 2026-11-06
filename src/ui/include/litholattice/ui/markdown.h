// Litho GUI - markdown subset parser (JD request, 2026-11-01).
//
// An in-house, deterministic parser for the chat-style markdown
// subset the GUI renders through Label rich spans. No imports, no
// IO, no clocks: the same input always produces the same spans.
//
// Supported:
//   inline: **bold**, *italic* / _italic_, ***bold italic***,
//           `code`, ~~strikethrough~~, [text](url) links,
//           backslash escapes for * _ ` ~ \ [
//           Links do not nest: when link constructs overlap, the
//           innermost ]( wins and the outer brackets render as
//           literal text.
//   block:  # .. ###### headings, - / * bullet items, > quotes,
//           blank line = paragraph break
// NOT supported (renders as literal text): tables, images, nested
// lists, HTML, setext headings, reference links, code fences.
//
// Span styles are FULLY resolved against the caller's base style:
// Label treats a span's style as a complete replacement, so the
// parser composes weight / slant / size / color / decorations onto
// a copy of `base` for every span it emits (plain runs included).
// Fixed style choices, made once here so every surface agrees:
//   code spans:  weight 500, trace-purple color (the theme's
//                composition accent), so code reads as its own
//                voice without a per-span background (spans have
//                no background channel)
//   links:       trace-blue color + the TextStyle underline flag
//   quotes:      muted color (an opaque base color is scaled to
//                60%; an inherited base color becomes the muted
//                gray used for secondary text)
// Block structure stays one flowing span list: block lines are
// joined by explicit "\n" separator spans (two for a paragraph
// break), which the span layout treats as forced line breaks.
#pragma once

#include "litholattice/ui/litho_gui.h"

#include <cstddef>
#include <string>
#include <vector>

namespace lithogui {

// One link in a parsed document: the span at `spanIndex` (an index
// into MarkdownDoc::spans) carries `url`. A multi-span link text
// produces one entry per span it produced.
struct MarkdownLink {
    std::size_t spanIndex = 0;
    std::string url;
};

struct MarkdownDoc {
    std::vector<TextSpan> spans;
    std::vector<MarkdownLink> links;
};

// Parses `text` against `base`. Unclosed or unmatched markers
// render literally; code spans suppress inner parsing; escapes
// produce the literal character unstyled. Empty input (or input
// with no content lines) produces an empty document.
MarkdownDoc parseMarkdown(const std::string& text, const TextStyle& base);

} // namespace lithogui
