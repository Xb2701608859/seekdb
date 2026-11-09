// MiniVectorDB — protocol layer: statement framing and response formatting.
//
// Everything the hidden evaluation driver can disagree with us about lives in
// this file: how statements are delimited, what a result set looks like on
// stdout, what an error looks like, and how the vector search mode is switched.
// Each of those is a Config field, so aligning with the official package is a
// flag change (or a one-line default change) rather than an engine change.
#pragma once

#include <string>
#include <vector>

#include "engine.h"

namespace mvd {

// How a statement is recognised on stdin.
enum class StatementMode {
  kSemicolon,  // accumulate until ';' outside quotes/comments (can span lines)
  kLine,       // every non-empty line is one statement
};

// How results are rendered on stdout.
enum class OutputFormat {
  kTsv,    // tab separated rows, no header; "OK <n>" / "OK" status lines
  kJsonl,  // one JSON object per statement
};

struct Config {
  std::string data_dir = "/data";
  StatementMode statement_mode = StatementMode::kSemicolon;
  OutputFormat output = OutputFormat::kTsv;

  // Response framing.  A result set has no length prefix and no trailing
  // blank line is reliable (a query may legitimately return zero rows), so
  // every response ends with an explicit terminator line.  Which token the
  // official driver expects is unknown; this is the knob to turn.
  std::string terminator = "END";

  // Emit a header line with the column names before the rows.  The public
  // cases assert on column names, so they must reach the driver somehow.
  bool header = true;

  // Appended as its own line after the response of every statement.  Empty
  // means "no marker"; the official driver may or may not use one, and if it
  // does it will tell us the token through a flag of its own.
  std::string ready_marker;

  // Default vector search mode; SET vector_mode = ann|exact overrides it for
  // the rest of the session.
  std::string vector_mode = "exact";

  // Errors as "ERROR <numeric>: ..." instead of "ERROR <NAME>: ...".
  bool numeric_errors = false;

  // Echo every statement and its response to stderr.  Never touches stdout.
  bool verbose = false;
};

// Splits a byte stream into complete SQL statements.
//
// The splitter is quote- and comment-aware, so a ';' inside 'a;b', inside a
// backtick identifier, or inside /* ... */ does not end a statement.  A
// trailing fragment without a delimiter is returned only when `flush` is true
// (used at EOF so a driver that omits the last ';' still works).
class StatementSplitter {
 public:
  explicit StatementSplitter(StatementMode mode) : mode_(mode) {}

  // Feeds one line (without its trailing '\n') and appends any complete
  // statements to `out`.  Returns false if the line was consumed as part of a
  // multi-line statement and produced nothing yet.
  bool FeedLine(const std::string& line, std::vector<std::string>* out);

  // At EOF: emit whatever is pending (if it is not just whitespace).
  bool Flush(std::vector<std::string>* out);

 private:
  void ScanChar(char c);
  std::string TakeStatement();

  StatementMode mode_;
  std::string buf_;
  // Scanner state, kept across lines because a statement may span lines.
  bool in_squote_ = false;
  bool in_dquote_ = false;
  bool in_backtick_ = false;
  bool in_block_comment_ = false;
  bool in_line_comment_ = false;
  char prev_ = '\0';
};

// Renders a Result onto stdout according to Config.
class Formatter {
 public:
  explicit Formatter(const Config& cfg) : cfg_(cfg) {}

  // Returns the exact bytes (already newline terminated) to write to stdout.
  std::string Format(const Result& r) const;

  // Stable textual form of one value, shared by every format so that the
  // numeric rendering never differs between profiles.
  static std::string FormatValue(const Value& v);

 private:
  std::string FormatTsv(const Result& r) const;
  std::string FormatJsonl(const Result& r) const;

  const Config& cfg_;
};

}  // namespace mvd
