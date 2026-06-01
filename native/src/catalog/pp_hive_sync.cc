// primeparts/catalog/pp_hive_sync.cc — see header.
//
// Forks scripts/hive_register.sh and captures its combined output. We exec the
// script directly (it has a #!/usr/bin/env bash shebang and is chmod +x) rather
// than going through `sh -c`, so arguments pass through without shell quoting.

#include "primeparts/catalog/pp_hive_sync.h"

#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>

namespace primeparts::catalog {

namespace fs = std::filesystem;

namespace {

// Resolve the path to hive_register.sh: explicit override, else relative to the
// running executable (../scripts/ from native/build), else a repo-relative
// guess. Returns empty if none exists.
std::string ResolveScript(const std::string& override_path) {
  if (!override_path.empty()) return override_path;

  std::error_code ec;
  // /proc/self/exe -> .../native/build/pp-catalog ; scripts/ is ../../scripts
  fs::path exe = fs::read_symlink("/proc/self/exe", ec);
  if (!ec) {
    fs::path cand = exe.parent_path() / ".." / ".." / "scripts" / "hive_register.sh";
    if (fs::exists(cand)) return fs::weakly_canonical(cand, ec).string();
  }
  // Fallback: CWD-relative (when run from the repo root).
  if (fs::exists("scripts/hive_register.sh")) return "scripts/hive_register.sh";
  return {};
}

}  // namespace

int RunHiveRegister(const HiveSyncOptions& opts,
                    const std::vector<std::string>& args, std::string* output) {
  output->clear();
  std::string script = ResolveScript(opts.script_path);
  if (script.empty()) {
    *output = "hive_register.sh not found (set HiveSyncOptions::script_path)";
    return -1;
  }

  int pipefd[2];
  if (pipe(pipefd) != 0) {
    *output = std::string("pipe: ") + std::strerror(errno);
    return -1;
  }

  pid_t pid = fork();
  if (pid < 0) {
    *output = std::string("fork: ") + std::strerror(errno);
    close(pipefd[0]);
    close(pipefd[1]);
    return -1;
  }

  if (pid == 0) {
    // Child: redirect stdout+stderr to the pipe, exec the script.
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[0]);
    close(pipefd[1]);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(script.c_str()));
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    execv(script.c_str(), argv.data());
    // execv only returns on failure.
    std::string msg = std::string("execv ") + script + ": " + std::strerror(errno) + "\n";
    ssize_t wrote = write(STDERR_FILENO, msg.data(), msg.size());
    (void)wrote;
    _exit(127);
  }

  // Parent: read all output, then reap.
  close(pipefd[1]);
  std::array<char, 4096> buf;
  ssize_t n;
  while ((n = read(pipefd[0], buf.data(), buf.size())) > 0) {
    output->append(buf.data(), static_cast<size_t>(n));
  }
  close(pipefd[0]);

  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    output->append("waitpid failed\n");
    return -1;
  }
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return -1;
}

bool HiveSync(const HiveSyncOptions& opts, const std::string& db_table,
              const std::string& metadata_uri, std::string* output) {
  return RunHiveRegister(opts, {"sync", db_table, metadata_uri}, output) == 0;
}

bool HiveExec(const HiveSyncOptions& opts, const std::string& sql,
              std::string* output) {
  return RunHiveRegister(opts, {"exec", sql}, output) == 0;
}

}  // namespace primeparts::catalog
