#include "server/config.h"

#include <fcntl.h>
#include <unistd.h>

#include <sys/stat.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <mutex>

#include "common/glob.h"
#include "common/string_util.h"
#include "server/config_options.h"

namespace tideskv {
namespace {

Status Bounded(std::string_view v, int64_t lo, int64_t hi, int64_t* out) {
  if (!ParseInt64(v, out) || *out < lo || *out > hi) {
    return Status::InvalidArgument("argument must be between " + std::to_string(lo) + " and " +
                                   std::to_string(hi));
  }
  return Status::Ok();
}

Status Memory(std::string_view v, uint64_t min, uint64_t* out) {
  uint64_t parsed = 0;
  if (!ParseMemory(v, &parsed) || parsed < min) {
    return Status::InvalidArgument("argument must be a memory value of at least " + std::to_string(min) +
                                   " bytes");
  }
  *out = parsed;
  return Status::Ok();
}

Status Choice(std::string_view v, std::initializer_list<std::string_view> allowed, std::string* out) {
  const std::string lower = ToLower(v);
  std::string list;
  for (std::string_view a : allowed) {
    if (a == lower) {
      *out = lower;
      return Status::Ok();
    }
    list += (list.empty() ? "" : ", ") + std::string(a);
  }
  return Status::InvalidArgument("argument must be one of: " + list);
}

Status NonEmpty(std::string_view v, std::string* out) {
  if (v.empty()) return Status::InvalidArgument("argument must not be empty");
  *out = std::string(v);
  return Status::Ok();
}

// Reject control characters (< 0x20 or == 0x7f).
Status ValidateNoControlChars(std::string_view v) {
  for (size_t i = 0; i < v.size(); ++i) {
    uint8_t ch = static_cast<uint8_t>(v[i]);
    if (ch < 0x20 || ch == 0x7f) {
      return Status::InvalidArgument("argument must not contain control characters");
    }
  }
  return Status::Ok();
}

template <typename T>
Status SetInt(std::string_view v, int64_t lo, int64_t hi, T* out) {
  int64_t n = 0;
  Status st = Bounded(v, lo, hi, &n);
  if (st.ok()) *out = static_cast<T>(n);
  return st;
}

std::vector<Option> BaseOptions() {
  return {
      {"bind", false,
       [](ConfigValues& c, std::string_view v) {
         if (Status st = ValidateNoControlChars(v); !st.ok()) return st;
         return NonEmpty(v, &c.bind);
       },
       [](const ConfigValues& c) { return c.bind; }},
      {"block-cache-size", false,
       [](ConfigValues& c, std::string_view v) { return Memory(v, 0, &c.block_cache_size); },
       [](const ConfigValues& c) { return std::to_string(c.block_cache_size); }},
      {"client-query-buffer-limit", true,
       [](ConfigValues& c, std::string_view v) {
         return Memory(v, 1ULL << 20, &c.client_query_buffer_limit);
       },
       [](const ConfigValues& c) { return std::to_string(c.client_query_buffer_limit); }},
      {"compression", false,
       [](ConfigValues& c, std::string_view v) { return Choice(v, {"no", "lz4", "zstd"}, &c.compression); },
       [](const ConfigValues& c) { return c.compression; }},
      {"dir", false,
       [](ConfigValues& c, std::string_view v) {
         if (Status st = ValidateNoControlChars(v); !st.ok()) return st;
         return NonEmpty(v, &c.dir);
       },
       [](const ConfigValues& c) { return c.dir; }},
      {"logfile", false,
       [](ConfigValues& c, std::string_view v) {
         if (Status st = ValidateNoControlChars(v); !st.ok()) return st;
         c.logfile = std::string(v);
         return Status::Ok();
       },
       [](const ConfigValues& c) { return c.logfile; }},
      {"loglevel", false,
       [](ConfigValues& c, std::string_view v) {
         return Choice(v, {"debug", "verbose", "notice", "warning"}, &c.loglevel);
       },
       [](const ConfigValues& c) { return c.loglevel; }},
      {"compaction-collection-deletes", true,
       [](ConfigValues& c, std::string_view v) {
         return SetInt(v, 0, 1'000'000'000, &c.compaction_collection_deletes);
       },
       [](const ConfigValues& c) { return std::to_string(c.compaction_collection_deletes); }},
      {"busy-reply-threshold", true,
       [](ConfigValues& c, std::string_view v) {
         return SetInt(v, 0, std::numeric_limits<int64_t>::max(), &c.busy_reply_threshold_ms);
       },
       [](const ConfigValues& c) { return std::to_string(c.busy_reply_threshold_ms); }},
      {"lua-time-limit", true,
       [](ConfigValues& c, std::string_view v) {
         return SetInt(v, 0, std::numeric_limits<int64_t>::max(), &c.busy_reply_threshold_ms);
       },
       [](const ConfigValues& c) { return std::to_string(c.busy_reply_threshold_ms); }},
      {"maxclients", true,
       [](ConfigValues& c, std::string_view v) { return SetInt(v, 1, 1'000'000, &c.maxclients); },
       [](const ConfigValues& c) { return std::to_string(c.maxclients); }},
      {"memtable-size", false,
       [](ConfigValues& c, std::string_view v) { return Memory(v, 1ULL << 20, &c.memtable_size); },
       [](const ConfigValues& c) { return std::to_string(c.memtable_size); }},
      {"port", false, [](ConfigValues& c, std::string_view v) { return SetInt(v, 1, 65535, &c.port); },
       [](const ConfigValues& c) { return std::to_string(c.port); }},
      {"requirepass", true,
       [](ConfigValues& c, std::string_view v) {
         if (Status st = ValidateNoControlChars(v); !st.ok()) return st;
         c.requirepass = std::string(v);
         return Status::Ok();
       },
       [](const ConfigValues& c) { return c.requirepass; }},
      {"sync-interval-us", false,
       [](ConfigValues& c, std::string_view v) { return SetInt(v, 1, 60'000'000, &c.sync_interval_us); },
       [](const ConfigValues& c) { return std::to_string(c.sync_interval_us); }},
      {"sync-mode", false,
       [](ConfigValues& c, std::string_view v) {
         return Choice(v, {"none", "interval", "full"}, &c.sync_mode);
       },
       [](const ConfigValues& c) { return c.sync_mode; }},
      {"workers", false, [](ConfigValues& c, std::string_view v) { return SetInt(v, 0, 1024, &c.workers); },
       [](const ConfigValues& c) { return std::to_string(c.workers); }},
  };
}

// Core options followed by the replication table (config_repl_options.cc).
const std::vector<Option>& Options() {
  static const std::vector<Option> kOptions = [] {
    std::vector<Option> all = BaseOptions();
    const std::vector<Option>& repl = ReplOptions();
    all.insert(all.end(), repl.begin(), repl.end());
    return all;
  }();
  return kOptions;
}

const Option* FindOption(std::string_view name) {
  for (const Option& o : Options()) {
    if (EqualsIgnoreCase(o.name, name)) return &o;
  }
  return nullptr;
}

Status ApplyOne(ConfigValues& v, std::string_view name, std::string_view value, bool at_runtime) {
  const Option* opt = FindOption(name);
  if (opt == nullptr) return Status::InvalidArgument("unknown option '" + std::string(name) + "'");
  if (at_runtime && !opt->mutable_at_runtime) return Status::InvalidArgument("can't set immutable config");
  return opt->set(v, value);
}

std::string_view Trim(std::string_view s) {
  const size_t b = s.find_first_not_of(" \t\r");
  if (b == std::string_view::npos) return {};
  return s.substr(b, s.find_last_not_of(" \t\r") - b + 1);
}

std::string QuoteIfNeeded(const std::string& v) {
  const bool needs_quote = v.empty() || v.find_first_of(" \t\"\\") != std::string::npos;
  if (!needs_quote) return v;
  std::string quoted;
  quoted.push_back('"');
  for (char ch : v) {
    if (ch == '\\') {
      quoted += "\\\\";
    } else if (ch == '"') {
      quoted += "\\\"";
    } else {
      quoted.push_back(ch);
    }
  }
  quoted.push_back('"');
  return quoted;
}

// "name value" → {name, value}; a value wrapped in double quotes is unquoted.
bool SplitLine(std::string_view line, std::string* name, std::string* value) {
  const size_t sp = line.find_first_of(" \t");
  if (sp == std::string_view::npos) return false;
  *name = std::string(line.substr(0, sp));
  std::string_view v = Trim(line.substr(sp));
  if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
    // Unquote and unescape
    v = v.substr(1, v.size() - 2);
    std::string unquoted;
    for (size_t i = 0; i < v.size(); ++i) {
      if (v[i] == '\\' && i + 1 < v.size()) {
        if (v[i + 1] == '\\') {
          unquoted.push_back('\\');
          ++i;
        } else if (v[i + 1] == '"') {
          unquoted.push_back('"');
          ++i;
        } else {
          unquoted.push_back(v[i]);
        }
      } else {
        unquoted.push_back(v[i]);
      }
    }
    *value = unquoted;
  } else {
    *value = std::string(v);
  }
  return true;
}

}  // namespace

Status Config::LoadFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) return Status::IOError("cannot open config file " + path);
  std::unique_lock lock(mu_);
  ConfigValues next = values_;
  std::string raw;
  for (int line_no = 1; std::getline(in, raw); ++line_no) {
    const std::string_view line = Trim(raw);
    if (line.empty() || line.front() == '#') continue;
    std::string name, value;
    const std::string where = "config file line " + std::to_string(line_no) + ": ";
    if (!SplitLine(line, &name, &value))
      return Status::InvalidArgument(where + "missing value for '" + std::string(line) + "'");
    if (Status st = ApplyOne(next, name, value, false); !st.ok())
      return Status::InvalidArgument(where + st.message());
  }
  values_ = std::move(next);
  file_path_ = path;
  return Status::Ok();
}

Status Config::ApplyArgs(const std::vector<std::string>& args) {
  Pairs pairs;
  for (size_t i = 0; i < args.size(); i += 2) {
    if (args[i].rfind("--", 0) != 0)  // never echo the value: it may be a password
      return Status::InvalidArgument("expected --option at argument " + std::to_string(i + 1));
    if (i + 1 >= args.size()) return Status::InvalidArgument("missing value for " + args[i]);
    pairs.emplace_back(args[i].substr(2), args[i + 1]);
  }
  return SetMany(pairs, false, nullptr);
}

Status Config::ApplyEnv(const std::function<const char*(const char*)>& getenv_fn,
                        std::vector<std::string>* ignored) {
  static constexpr std::pair<const char*, std::string_view> kVars[] = {
      {"TIDESKV_REQUIREPASS", "requirepass"}};
  for (const auto& [var, option] : kVars) {
    const char* value = getenv_fn(var);
    if (value == nullptr) continue;
    if (*value == '\0') {  // treat as unset; disabling auth needs the file or --requirepass ""
      if (ignored != nullptr) ignored->emplace_back(var);
      continue;
    }
    if (Status st = Set(option, value, /*at_runtime=*/false); !st.ok()) {
      return Status::InvalidArgument(std::string(var) + ": " + st.message());
    }
  }
  return Status::Ok();
}

Status Config::Set(std::string_view name, std::string_view value, bool at_runtime) {
  return SetMany({{std::string(name), std::string(value)}}, at_runtime, nullptr);
}

Status Config::SetMany(const Pairs& pairs, bool at_runtime, std::string* failed_name) {
  std::unique_lock lock(mu_);
  ConfigValues next = values_;
  for (const auto& [name, value] : pairs) {
    if (Status st = ApplyOne(next, name, value, at_runtime); !st.ok()) {
      if (failed_name != nullptr) *failed_name = name;
      return st;
    }
  }
  values_ = std::move(next);
  return Status::Ok();
}

Config::Pairs Config::Get(std::string_view pattern) const {
  std::shared_lock lock(mu_);
  Pairs out;
  for (const Option& o : Options()) {  // Options() is sorted by name
    if (GlobMatch(pattern, o.name, /*nocase=*/true)) out.emplace_back(std::string(o.name), o.get(values_));
  }
  return out;
}

namespace {

// fsyncs the directory holding `path` so the rename is durable.
Status SyncParentDir(const std::string& path) {
  std::string dir_path = std::filesystem::path(path).parent_path().string();
  if (dir_path.empty()) dir_path = ".";
  const int dir_fd = ::open(dir_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir_fd < 0) return Status::IOError("cannot open directory " + dir_path);
  if (::fsync(dir_fd) != 0) {
    ::close(dir_fd);
    return Status::IOError("cannot fsync directory " + dir_path);
  }
  ::close(dir_fd);
  return Status::Ok();
}

// Writes the header and one "name value" line (values already quoted) per entry, then flushes, fsyncs and
// closes `f`. Always closes `f`; the caller removes the temp file on failure.
Status WriteConfigLines(FILE* f, const std::vector<std::pair<std::string, std::string>>& lines,
                        const std::string& tmp) {
  fprintf(f, "# Generated by CONFIG REWRITE\n");
  for (const auto& [name, value] : lines) {
    if (fprintf(f, "%s %s\n", name.c_str(), value.c_str()) < 0) {
      fclose(f);
      return Status::IOError("cannot write " + tmp);
    }
  }
  if (::fflush(f) != 0 || ::fsync(::fileno(f)) != 0) {
    fclose(f);
    return Status::IOError("cannot fsync " + tmp);
  }
  if (::fclose(f) != 0) return Status::IOError("cannot close " + tmp);
  return Status::Ok();
}

}  // namespace

Status Config::Rewrite() const {
  std::lock_guard<std::mutex> rewrite_lock(rewrite_mu_);
  std::string path;
  std::vector<std::pair<std::string, std::string>> lines;
  {
    std::shared_lock lock(mu_);
    if (file_path_.empty()) return Status::InvalidArgument("The server is running without a config file");
    path = file_path_;
    for (const Option& o : Options()) {
      if (o.alias) continue;
      const std::string value = o.get(values_);
      if (o.multi_arg && value.empty()) continue;  // e.g. a master has no replicaof line
      lines.emplace_back(std::string(o.name), o.multi_arg ? value : QuoteIfNeeded(value));
    }
  }
  const std::string tmp = path + ".tmp";
  ::unlink(tmp.c_str());  // remove a stale tmp file
  mode_t mode = 0600;     // keep the original permissions when the file exists
  struct stat st;
  if (::stat(path.c_str(), &st) == 0) mode = st.st_mode & 0777;
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_TRUNC | O_CLOEXEC, mode);
  if (fd < 0) return Status::IOError("cannot create " + tmp);
  FILE* f = ::fdopen(fd, "w");
  if (!f) {
    ::close(fd);
    ::unlink(tmp.c_str());
    return Status::IOError("cannot fdopen " + tmp);
  }
  if (Status s = WriteConfigLines(f, lines, tmp); !s.ok()) {
    ::unlink(tmp.c_str());
    return s;
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return Status::IOError("cannot rename " + tmp + " to " + path);
  }
  return SyncParentDir(path);
}

std::string Config::file_path() const {
  std::shared_lock lock(mu_);
  return file_path_;
}

ConfigValues Config::Snapshot() const {
  std::shared_lock lock(mu_);
  return values_;
}

}  // namespace tideskv
