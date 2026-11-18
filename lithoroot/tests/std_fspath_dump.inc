/* Emits one line per query into out (string). Needs P (path type), S(x) to string. */
for (const char* c : kPaths) {
  P p(c);
  out += S(p.filename()); out += '|'; out += S(p.stem()); out += '|'; out += S(p.extension()); out += '|';
  out += S(p.parent_path()); out += '|'; out += S(p.root_path()); out += '|'; out += S(p.relative_path()); out += '|';
  out += S(p.lexically_normal()); out += '|';
  out += p.has_filename() ? 'F' : 'f'; out += p.has_parent_path() ? 'P' : 'p'; out += p.has_extension() ? 'E' : 'e';
  out += p.has_stem() ? 'S' : 's'; out += p.is_absolute() ? 'A' : 'a'; out += '|';
  { P q = p; q.replace_extension(".bin"); out += S(q); out += '|'; }
  { P q = p; q.remove_filename(); out += S(q); out += '|'; }
  { P q = p / "n"; out += S(q); out += '|'; }
  for (auto it = p.begin(); it != p.end(); ++it) { out += '['; out += S(*it); out += ']'; }
  out += '\n';
  for (const char* b : kBases) { out += S(p.lexically_relative(P(b))); out += ';'; out += S(p.lexically_proximate(P(b))); out += ' '; }
  out += '\n';
  for (const char* d : kPaths) { int r = p.compare(P(d)); out += r < 0 ? '<' : r > 0 ? '>' : '='; }
  out += '\n';
}
