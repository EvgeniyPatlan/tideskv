#include "script/shebang.h"

#include <vector>

#include "types/errors.h"

namespace tideskv {
namespace {

// Splits on spaces, tabs and CR. Redis uses sdssplitargs, which also honours quotes; no real
// script quotes a shebang flag, so plain whitespace splitting is enough here.
std::vector<std::string_view> SplitWhitespace(std::string_view s) {
  std::vector<std::string_view> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) ++i;
    const size_t start = i;
    while (i < s.size() && s[i] != ' ' && s[i] != '\t' && s[i] != '\r') ++i;
    if (i > start) out.push_back(s.substr(start, i - start));
  }
  return out;
}

Status ApplyFlags(std::string_view list, ScriptFlags* flags) {
  if (list.empty()) return Status::Ok();  // "flags=" declares no flags (sds.c:855)
  size_t start = 0;
  while (true) {
    const size_t comma = list.find(',', start);
    const std::string_view f =
        list.substr(start, comma == std::string_view::npos ? list.npos : comma - start);
    if (f == "no-writes") {
      flags->no_writes = true;
    } else if (f == "allow-oom") {
      flags->allow_oom = true;
    } else if (f == "allow-stale") {
      flags->allow_stale = true;
    } else if (f == "no-cluster") {
      flags->no_cluster = true;
    } else if (f == "allow-cross-slot-keys") {
      flags->allow_cross_slot = true;
    } else {
      return Status::UserError(std::string(kErrUnexpectedShebangFlag).append(f));
    }
    if (comma == std::string_view::npos) return Status::Ok();
    start = comma + 1;
  }
}

}  // namespace

Result<ParsedScript> ParseShebang(std::string_view body) {
  ParsedScript out;
  if (!body.starts_with("#!")) {
    out.code = std::string(body);
    return out;
  }
  const size_t nl = body.find('\n');
  if (nl == std::string_view::npos) return Status::UserError(std::string(kErrInvalidShebang));
  const std::vector<std::string_view> parts = SplitWhitespace(body.substr(0, nl));
  if (parts.empty()) return Status::UserError(std::string(kErrInvalidShebangEngine));
  if (parts[0] != "#!lua")
    return Status::UserError(std::string(kErrUnexpectedShebangEngine).append(parts[0]));
  out.flags.shebang = true;
  for (size_t i = 1; i < parts.size(); ++i) {
    if (!parts[i].starts_with("flags=")) {
      return Status::UserError(std::string(kErrUnknownShebangOption).append(parts[i]));
    }
    if (Status st = ApplyFlags(parts[i].substr(6), &out.flags); !st.ok()) return st;
  }
  out.code = std::string(body.substr(nl));
  return out;
}

}  // namespace tideskv
