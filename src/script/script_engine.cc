#include "script/script_engine.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>

#include "script/sha1hex.h"
#include "types/errors.h"

namespace tideskv {
namespace {

std::string Lower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

}  // namespace

// Forwards the sandbox's requests to the caller and owns the per-run kill/timeout policy.
class ScriptEngine::Host final : public SandboxHost {
 public:
  Host(ScriptEngine& engine, ScriptCaller& caller, std::string sha, int64_t threshold_ms)
      : engine_(engine),
        caller_(caller),
        sha_(std::move(sha)),
        threshold_ms_(threshold_ms),
        start_(std::chrono::steady_clock::now()) {}

  std::optional<std::string> Call(const std::vector<std::string>& argv, int resp,
                                  std::string* error) noexcept override {
    try {
      return caller_.Call(argv, resp, error);
    } catch (...) {
      *error = "-" + std::string(kErrInternal);
      return std::nullopt;
    }
  }

  bool CommandExists(std::string_view name) const noexcept override {
    try {
      return caller_.CommandExists(name);
    } catch (...) {
      return false;
    }
  }

  void Log(int level, std::string_view msg) noexcept override {
    static constexpr spdlog::level::level_enum kLevels[] = {spdlog::level::debug, spdlog::level::debug,
                                                            spdlog::level::info, spdlog::level::warn};
    try {
      spdlog::log(kLevels[std::clamp(level, 0, 3)], "{}", msg);
    } catch (...) {  // logging must not unwind through Lua
      return;
    }
  }

  bool ShouldKill() noexcept override {
    if (engine_.kill_.load() || engine_.shutdown_.load()) return true;
    if (!engine_.timed_out_.load() && ElapsedMs() >= threshold_ms_) {  // script.c:80-109
      engine_.timed_out_ = true;
      try {
        spdlog::warn(
            "Slow script detected: still in execution after {} milliseconds. You can try killing the "
            "script using the SCRIPT KILL command. Script name is: {}.",
            ElapsedMs(), sha_);
        if (engine_.on_timeout_) engine_.on_timeout_();
      } catch (...) {  // the warning is best-effort; the timeout flag is already set
        return false;
      }
    }
    return false;
  }

 private:
  int64_t ElapsedMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_)
        .count();
  }

  ScriptEngine& engine_;
  ScriptCaller& caller_;
  std::string sha_;
  int64_t threshold_ms_;
  std::chrono::steady_clock::time_point start_;
};

ScriptEngine::ScriptEngine() : sandbox_(LuaSandbox::Create()) { lua_memory_ = sandbox_->memory_bytes(); }

Result<std::string> ScriptEngine::LookupOrCompile(std::string_view script, bool by_sha) {
  if (by_sha) {
    std::string sha = Lower(script);
    if (scripts_.find(sha) == scripts_.end()) return Status::UserError(std::string(kErrNoScript));
    return sha;
  }
  std::string sha = Sha1Hex(script);
  if (scripts_.find(sha) != scripts_.end()) return sha;
  Result<ParsedScript> parsed = ParseShebang(script);
  if (!parsed.ok()) return parsed.status();
  Status st = sandbox_->Compile(sha, parsed->code);
  if (!st.ok()) return st;
  scripts_.emplace(sha, Script{parsed->flags});
  cached_ = scripts_.size();
  return sha;
}

ScriptEngine::EvalResult ScriptEngine::Eval(const EvalRequest& req, ScriptCaller& caller,
                                            resp::ReplyBuilder& out) {
  std::lock_guard<std::mutex> lock(mu_);
  Result<std::string> sha = LookupOrCompile(req.script, req.by_sha);
  if (!sha.ok()) {
    out.Error(sha.status().message());
    return {};
  }
  const ScriptFlags flags = scripts_.at(*sha).flags;
  if (req.read_only_command && flags.shebang && !flags.no_writes) {
    out.Error(kErrRoScriptWithWriteFlag);
    return {};
  }
  caller.SetReadOnly(req.read_only_command || flags.no_writes);
  running_ = true;
  timed_out_ = false;
  kill_ = false;
  Host host(*this, caller, *sha, req.busy_threshold_ms);
  const auto r =
      sandbox_->Run(*sha, *req.keys, *req.args, host, {req.caller_resp, req.busy_threshold_ms > 0}, out);
  running_ = false;
  timed_out_ = false;
  kill_ = false;
  lua_memory_ = sandbox_->memory_bytes();
  return {r.killed};
}

Result<std::string> ScriptEngine::Load(std::string_view body) {
  std::lock_guard<std::mutex> lock(mu_);
  return LookupOrCompile(body, false);
}

std::vector<bool> ScriptEngine::Exists(const std::vector<std::string>& shas) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<bool> out;
  out.reserve(shas.size());
  for (const std::string& s : shas) out.push_back(scripts_.find(Lower(s)) != scripts_.end());
  return out;
}

void ScriptEngine::Flush() {
  std::lock_guard<std::mutex> lock(mu_);
  scripts_.clear();
  sandbox_ = LuaSandbox::Create();  // the old state's destructor runs the full GC and close
  cached_ = 0;
  lua_memory_ = sandbox_->memory_bytes();
}

bool ScriptEngine::TimedOut() const noexcept { return timed_out_.load() || forced_timed_out_.load(); }

bool ScriptEngine::Kill() noexcept {
  if (!timed_out_.load()) return false;
  kill_ = true;
  return true;
}

void ScriptEngine::KillForShutdown() noexcept { shutdown_ = true; }

void ScriptEngine::SetOnTimeout(std::function<void()> fn) {
  std::lock_guard<std::mutex> lock(mu_);
  on_timeout_ = std::move(fn);
}

}  // namespace tideskv
