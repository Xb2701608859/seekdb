// MiniVectorDB — entry point.
//
// Reads SQL from stdin, writes results to stdout, logs to stderr only.  The
// process stays alive until EOF or an explicit quit; every response is flushed
// immediately so a driver waiting on a pipe never blocks.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "engine.h"
#include "protocol.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void OnSignal(int) { g_stop = 1; }

void Usage(const char* argv0) {
  std::fprintf(stderr,
               "Usage: %s [options]\n"
               "  --data-dir DIR         data directory (default /data)\n"
               "  --statement-mode M     semicolon | line (default semicolon)\n"
               "  --output-format F      tsv | jsonl (default tsv)\n"
               "  --ready-marker TOKEN   append TOKEN after each response\n"
               "  --vector-mode M        exact | ann (default exact)\n"
               "  --numeric-errors       render error codes as numbers\n"
               "  --verbose              echo statements to stderr\n"
               "  --help                 show this help\n",
               argv0);
}

bool ParseArgs(int argc, char** argv, mvd::Config* cfg) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](const char* what) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", what);
        return nullptr;
      }
      return argv[++i];
    };
    if (a == "--data-dir") {
      const char* v = next("--data-dir");
      if (!v) return false;
      cfg->data_dir = v;
    } else if (a == "--statement-mode") {
      const char* v = next("--statement-mode");
      if (!v) return false;
      if (!std::strcmp(v, "semicolon")) cfg->statement_mode = mvd::StatementMode::kSemicolon;
      else if (!std::strcmp(v, "line")) cfg->statement_mode = mvd::StatementMode::kLine;
      else {
        std::fprintf(stderr, "unknown statement mode '%s'\n", v);
        return false;
      }
    } else if (a == "--output-format") {
      const char* v = next("--output-format");
      if (!v) return false;
      if (!std::strcmp(v, "tsv")) cfg->output = mvd::OutputFormat::kTsv;
      else if (!std::strcmp(v, "jsonl")) cfg->output = mvd::OutputFormat::kJsonl;
      else {
        std::fprintf(stderr, "unknown output format '%s'\n", v);
        return false;
      }
    } else if (a == "--ready-marker") {
      const char* v = next("--ready-marker");
      if (!v) return false;
      cfg->ready_marker = v;
    } else if (a == "--vector-mode") {
      const char* v = next("--vector-mode");
      if (!v) return false;
      cfg->vector_mode = v;
    } else if (a == "--numeric-errors") {
      cfg->numeric_errors = true;
    } else if (a == "--verbose") {
      cfg->verbose = true;
    } else if (a == "--help" || a == "-h") {
      Usage(argv[0]);
      std::exit(0);
    } else {
      std::fprintf(stderr, "unknown option '%s'\n", a.c_str());
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  mvd::Config cfg;
  if (!ParseArgs(argc, argv, &cfg)) {
    Usage(argv[0]);
    return 2;
  }

  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);

  mvd::Engine engine(cfg.data_dir);
  engine.set_vector_mode(cfg.vector_mode);
  mvd::Formatter formatter(cfg);
  mvd::StatementSplitter splitter(cfg.statement_mode);

  auto emit = [&](const std::string& sql) {
    mvd::Result r = engine.Execute(sql);
    std::string out = formatter.Format(r);
    if (cfg.verbose) {
      std::fprintf(stderr, "[sql] %s\n[res] %s", sql.c_str(), out.c_str());
    }
    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fflush(stdout);
  };

  char* line = nullptr;
  size_t cap = 0;
  std::vector<std::string> pending;
  while (!g_stop) {
    ssize_t n = ::getline(&line, &cap, stdin);
    if (n < 0) break;
    std::string s(line, static_cast<size_t>(n));
    if (!s.empty() && s.back() == '\n') s.pop_back();
    pending.clear();
    splitter.FeedLine(s, &pending);
    for (const auto& stmt : pending) emit(stmt);
  }
  std::free(line);
  pending.clear();
  if (splitter.Flush(&pending)) {
    for (const auto& stmt : pending) emit(stmt);
  }
  std::fflush(stdout);
  return 0;
}
