#include "server/config.h"

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <mutex>

#include "common/glob.h"
#include "common/string_util.h"

namespace tideskv {
namespace {

struct Option {
  std::string_view name;
  bool mutable_at_runtime;
  Status (*set)(ConfigValues&, std::string_view);
  std::string (*get)(const ConfigValues&);
};

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

template <typename T>
Status SetInt(std::string_view v, int64_t lo, int64_t hi, T* out) {
  int64_t n = 0;
  Status st = Bounded(v, lo, hi, &n);
  if (st.ok()) *out = static_cast<T>(n);
  return st;
}

const std::vector<Option>& Options() {
  static const std::vector<Option> kOptions = {
      {"bind", false, [](ConfigValues& c, std::string_view v) { return NonEmpty(v, &c.bind); },
       [](const ConfigValues& c) { return c.bind; }},
      {"block-cache-size", false,
       [](ConfigValues& c, std::string_view v) { return Memory(v, 0, &c.block_cache_size); },
       [](const ConfigValues& c) { return std::to_string(c.block_cache_size); }},
      {"client-query-buffer-limit", true,
       [](ConfigValues& c, std::string_view v) {
         return Memory(v, 1ULL << 20, &c.client_query_buffer_limit);
       },
       [](const ConfigValues& c) { return std::to_string(c.client_query_buffer_limit); }},
      {"dir", false, [](ConfigValues& c, std::string_view v) { return NonEmpty(v, &c.dir); },
       [](const ConfigValues& c) { return c.dir; }},
      {"logfile", false,
       [](ConfigValues& c, std::string_view v) {
         c.logfile = std::string(v);
         return Status::Ok();
       },
       [](const ConfigValues& c) { return c.logfile; }},
      {"loglevel", false,
       [](ConfigValues& c, std::string_view v) {
         return Choice(v, {"debug", "verbose", "notice", "warning"}, &c.loglevel);
       },
       [](const ConfigValues& c) { return c.loglevel; }},
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

// "name value" → {name, value}; a value wrapped in double quotes is unquoted.
bool SplitLine(std::string_view line, std::string* name, std::string* value) {
  const size_t sp = line.find_first_of(" \t");
  if (sp == std::string_view::npos) return false;
  *name = std::string(line.substr(0, sp));
  std::string_view v = Trim(line.substr(sp));
  if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
  *value = std::string(v);
  return true;
}

std::string QuoteIfNeeded(const std::string& v) {
  return (v.empty() || v.find_first_of(" \t") != std::string::npos) ? "\"" + v + "\"" : v;
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
    if (args[i].rfind("--", 0) != 0)
      return Status::InvalidArgument("expected --option, got '" + args[i] + "'");
    if (i + 1 >= args.size()) return Status::InvalidArgument("missing value for " + args[i]);
    pairs.emplace_back(args[i].substr(2), args[i + 1]);
  }
  return SetMany(pairs, false, nullptr);
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

Status Config::Rewrite() const {
  if (file_path_.empty()) return Status::InvalidArgument("The server is running without a config file");
  const std::string tmp = file_path_ + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) return Status::IOError("cannot write " + tmp);
    out << "# Generated by CONFIG REWRITE\n";
    for (const auto& [name, value] : Get("*")) out << name << ' ' << QuoteIfNeeded(value) << '\n';
    if (!out.flush()) return Status::IOError("cannot write " + tmp);
  }
  std::error_code ec;
  std::filesystem::rename(tmp, file_path_, ec);
  return ec ? Status::IOError("cannot replace " + file_path_ + ": " + ec.message()) : Status::Ok();
}

ConfigValues Config::Snapshot() const {
  std::shared_lock lock(mu_);
  return values_;
}

}  // namespace tideskv
