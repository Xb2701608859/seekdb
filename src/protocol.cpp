#include "protocol.h"

#include <cstdio>
#include <cstring>

namespace mvd {
namespace {

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string Trim(const std::string& s) {
  size_t b = 0;
  size_t e = s.size();
  while (b < e && IsSpace(s[b])) ++b;
  while (e > b && IsSpace(s[e - 1])) --e;
  return s.substr(b, e - b);
}

// Shortest decimal that round-trips a float; keeps "5" and "0.5" tidy while
// never losing precision (the driver compares floats with a tolerance, but
// integers and column names must match exactly).
std::string ShortestFloat(float f) {
  char buf[64];
  for (int prec = 6; prec <= 9; ++prec) {
    std::snprintf(buf, sizeof(buf), "%.*g", prec, static_cast<double>(f));
    if (std::strtof(buf, nullptr) == f) return std::string(buf);
  }
  std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(f));
  return std::string(buf);
}

void JsonEscape(std::string* out, const std::string& s) {
  out->push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"': *out += "\\\""; break;
      case '\\': *out += "\\\\"; break;
      case '\n': *out += "\\n"; break;
      case '\r': *out += "\\r"; break;
      case '\t': *out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          *out += buf;
        } else {
          out->push_back(static_cast<char>(c));
        }
    }
  }
  out->push_back('"');
}

}  // namespace

// ---------------------------------------------------------------- splitter

void StatementSplitter::ScanChar(char c) {
  if (in_line_comment_) {
    if (c == '\n') in_line_comment_ = false;
    buf_.push_back(c);
    return;
  }
  if (in_block_comment_) {
    if (prev_ == '*' && c == '/') in_block_comment_ = false;
    buf_.push_back(c);
    return;
  }
  if (in_squote_) {
    if (c == '\'' && prev_ != '\\') in_squote_ = false;
    buf_.push_back(c);
    return;
  }
  if (in_dquote_) {
    if (c == '"' && prev_ != '\\') in_dquote_ = false;
    buf_.push_back(c);
    return;
  }
  if (in_backtick_) {
    if (c == '`') in_backtick_ = false;
    buf_.push_back(c);
    return;
  }
  // Outside of any quoting/comment context.
  if (prev_ == '-' && c == '-') {
    // "--" starts a comment only at the start of a token, but treating it
    // everywhere is harmless for the SQL subset we accept and keeps the
    // splitter simple.
    in_line_comment_ = true;
    buf_.push_back(c);
    return;
  }
  if (c == '#') {
    in_line_comment_ = true;
    buf_.push_back(c);
    return;
  }
  if (prev_ == '/' && c == '*') {
    in_block_comment_ = true;
    buf_.push_back(c);
    return;
  }
  switch (c) {
    case '\'': in_squote_ = true; break;
    case '"': in_dquote_ = true; break;
    case '`': in_backtick_ = true; break;
    default: break;
  }
  buf_.push_back(c);
}

std::string StatementSplitter::TakeStatement() {
  std::string s = Trim(buf_);
  buf_.clear();
  // Reset scanner state: a statement never starts inside a quote.
  in_squote_ = in_dquote_ = in_backtick_ = false;
  in_block_comment_ = in_line_comment_ = false;
  prev_ = '\0';
  return s;
}

bool StatementSplitter::FeedLine(const std::string& line, std::vector<std::string>* out) {
  if (mode_ == StatementMode::kLine) {
    std::string s = Trim(line);
    if (s.empty()) return false;
    out->push_back(s);
    return true;
  }

  size_t produced = out->size();
  for (char c : line) {
    prev_ = buf_.empty() ? '\0' : buf_.back();
    if (!in_squote_ && !in_dquote_ && !in_backtick_ && !in_block_comment_ &&
        !in_line_comment_ && c == ';') {
      std::string stmt = TakeStatement();
      if (!stmt.empty()) out->push_back(stmt);
      prev_ = '\0';
      continue;
    }
    ScanChar(c);
    prev_ = c;
  }
  // A newline always terminates a line comment.
  in_line_comment_ = false;
  buf_.push_back('\n');
  return out->size() > produced;
}

bool StatementSplitter::Flush(std::vector<std::string>* out) {
  if (mode_ == StatementMode::kLine) return false;
  std::string stmt = TakeStatement();
  if (stmt.empty()) return false;
  out->push_back(stmt);
  return true;
}

// --------------------------------------------------------------- formatter

std::string Formatter::FormatValue(const Value& v) {
  switch (v.t) {
    case Value::T::INT:
      return std::to_string(v.i);
    case Value::T::DOUBLE: {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.6f", v.d);
      return std::string(buf);
    }
    case Value::T::STR:
      return v.s;
    case Value::T::VEC: {
      std::string out = "[";
      for (size_t i = 0; i < v.v.size(); ++i) {
        if (i) out.push_back(',');
        out += ShortestFloat(v.v[i]);
      }
      out.push_back(']');
      return out;
    }
  }
  return std::string();
}

std::string Formatter::FormatTsv(const Result& r) const {
  std::string out;
  auto terminate = [&]() {
    if (!cfg_.terminator.empty()) {
      out += cfg_.terminator;
      out.push_back('\n');
    }
  };
  switch (r.kind) {
    case Result::Kind::ERROR: {
      out += "ERROR ";
      out += cfg_.numeric_errors ? "1" : r.code.c_str();
      out += ": ";
      out += r.message;
      out.push_back('\n');
      terminate();
      return out;
    }
    case Result::Kind::OK:
      out += "OK\n";
      terminate();
      return out;
    case Result::Kind::AFFECTED:
      out += "OK " + std::to_string(r.affected) + "\n";
      terminate();
      return out;
    case Result::Kind::ROWS:
      if (cfg_.header) {
        for (size_t i = 0; i < r.columns.size(); ++i) {
          if (i) out.push_back('\t');
          out += r.columns[i];
        }
        out.push_back('\n');
      }
      for (const auto& row : r.rows) {
        for (size_t i = 0; i < row.size(); ++i) {
          if (i) out.push_back('\t');
          out += FormatValue(row[i]);
        }
        out.push_back('\n');
      }
      terminate();
      return out;
  }
  return out;
}

std::string Formatter::FormatJsonl(const Result& r) const {
  std::string out = "{";
  switch (r.kind) {
    case Result::Kind::ERROR:
      out += "\"error\":{\"code\":";
      JsonEscape(&out, r.code);
      out += ",\"message\":";
      JsonEscape(&out, r.message);
      out += "}}";
      out.push_back('\n');
      return out;
    case Result::Kind::OK:
      out += "}";
      out.push_back('\n');
      return out;
    case Result::Kind::AFFECTED:
      out += "\"affected_rows\":" + std::to_string(r.affected) + "}";
      out.push_back('\n');
      return out;
    case Result::Kind::ROWS: {
      out += "\"columns\":[";
      for (size_t i = 0; i < r.columns.size(); ++i) {
        if (i) out.push_back(',');
        JsonEscape(&out, r.columns[i]);
      }
      out += "],\"rows\":[";
      for (size_t i = 0; i < r.rows.size(); ++i) {
        if (i) out.push_back(',');
        out.push_back('[');
        for (size_t j = 0; j < r.rows[i].size(); ++j) {
          if (j) out.push_back(',');
          const Value& v = r.rows[i][j];
          switch (v.t) {
            case Value::T::INT: out += std::to_string(v.i); break;
            case Value::T::DOUBLE: {
              char buf[64];
              std::snprintf(buf, sizeof(buf), "%.17g", v.d);
              out += buf;
              break;
            }
            case Value::T::STR: JsonEscape(&out, v.s); break;
            case Value::T::VEC: JsonEscape(&out, FormatValue(v)); break;
          }
        }
        out.push_back(']');
      }
      out += "]}";
      out.push_back('\n');
      return out;
    }
  }
  return out;
}

std::string Formatter::Format(const Result& r) const {
  std::string out = cfg_.output == OutputFormat::kJsonl ? FormatJsonl(r) : FormatTsv(r);
  if (!cfg_.ready_marker.empty()) {
    out += cfg_.ready_marker;
    out.push_back('\n');
  }
  return out;
}

}  // namespace mvd
