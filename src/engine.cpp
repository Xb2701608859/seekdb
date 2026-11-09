// MiniVectorDB — STUB engine.
//
// ============================ READ THIS FIRST ============================
// This is NOT the database.  It is a throwaway in-memory stand-in whose only
// job is to let the protocol layer, the local case runner and the Docker build
// be validated end to end today.  It keeps everything in RAM, persists nothing,
// has no transactions, no WAL, no recovery and no vector index.
//
// Everything below this banner is expected to be deleted and replaced by the
// real storage/execution layer.  src/engine.h is the interface that must
// survive; nothing in the protocol layer depends on this file's internals.
// =========================================================================
#include "engine.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mvd {
namespace {

// ------------------------------------------------------------------ errors

Result Err(const char* code, const std::string& msg) { return Result::Error(code, msg); }

// --------------------------------------------------------------- tokenizer

struct Token {
  enum class T { IDENT, NUMBER, STRING, VECTOR, PUNCT, END } t = T::END;
  std::string text;   // identifier (lowercased for keywords), punctuation
  std::string raw;    // original text for strings/vectors
  double num = 0;
  bool quoted_ident = false;
};

bool IsIdentStart(char c) {
  return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '@';
}
bool IsIdentChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' || c == '@';
}

class Lexer {
 public:
  explicit Lexer(const std::string& s) : s_(s) {}

  bool Tokenize(std::vector<Token>* out, std::string* err) {
    size_t i = 0;
    const size_t n = s_.size();
    while (i < n) {
      char c = s_[i];
      if (std::isspace(static_cast<unsigned char>(c))) {
        ++i;
        continue;
      }
      if (c == '-' && i + 1 < n && s_[i + 1] == '-') {
        while (i < n && s_[i] != '\n') ++i;
        continue;
      }
      if (c == '#') {
        while (i < n && s_[i] != '\n') ++i;
        continue;
      }
      if (c == '/' && i + 1 < n && s_[i + 1] == '*') {
        i += 2;
        while (i + 1 < n && !(s_[i] == '*' && s_[i + 1] == '/')) ++i;
        if (i + 1 >= n) {
          *err = "unterminated block comment";
          return false;
        }
        i += 2;
        continue;
      }
      if (IsIdentStart(c)) {
        size_t b = i;
        while (i < n && IsIdentChar(s_[i])) ++i;
        Token t;
        t.t = Token::T::IDENT;
        t.text = s_.substr(b, i - b);
        std::string lower = t.text;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        t.raw = lower;
        out->push_back(t);
        continue;
      }
      if (c == '`') {
        size_t b = ++i;
        while (i < n && s_[i] != '`') ++i;
        if (i >= n) {
          *err = "unterminated quoted identifier";
          return false;
        }
        Token t;
        t.t = Token::T::IDENT;
        t.text = s_.substr(b, i - b);
        t.raw = t.text;
        t.quoted_ident = true;
        ++i;
        out->push_back(t);
        continue;
      }
      if (c == '\'') {
        std::string val;
        ++i;
        bool closed = false;
        while (i < n) {
          char d = s_[i];
          if (d == '\\' && i + 1 < n) {
            char e = s_[i + 1];
            switch (e) {
              case 'n': val.push_back('\n'); break;
              case 't': val.push_back('\t'); break;
              case 'r': val.push_back('\r'); break;
              case '0': val.push_back('\0'); break;
              case '\\': val.push_back('\\'); break;
              case '\'': val.push_back('\''); break;
              default: val.push_back(e); break;
            }
            i += 2;
            continue;
          }
          if (d == '\'') {
            if (i + 1 < n && s_[i + 1] == '\'') {  // '' escape
              val.push_back('\'');
              i += 2;
              continue;
            }
            ++i;
            closed = true;
            break;
          }
          val.push_back(d);
          ++i;
        }
        if (!closed) {
          *err = "unterminated string literal";
          return false;
        }
        Token t;
        t.t = Token::T::STRING;
        t.text = val;
        t.raw = val;
        out->push_back(t);
        continue;
      }
      if (c == '\'') {
        // handled above
      }
      if (c == '[') {
        size_t b = i;
        while (i < n && s_[i] != ']') ++i;
        if (i >= n) {
          *err = "unterminated vector literal";
          return false;
        }
        ++i;
        Token t;
        t.t = Token::T::VECTOR;
        t.text = s_.substr(b, i - b);
        t.raw = t.text;
        out->push_back(t);
        continue;
      }
      if (std::isdigit(static_cast<unsigned char>(c)) ||
          (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(s_[i + 1]))) ||
          ((c == '-' || c == '+') && i + 1 < n &&
           (std::isdigit(static_cast<unsigned char>(s_[i + 1])) || s_[i + 1] == '.'))) {
        size_t b = i;
        if (c == '-' || c == '+') ++i;
        while (i < n && (std::isdigit(static_cast<unsigned char>(s_[i])) || s_[i] == '.' ||
                         s_[i] == 'e' || s_[i] == 'E' ||
                         ((s_[i] == '-' || s_[i] == '+') && i > b &&
                          (s_[i - 1] == 'e' || s_[i - 1] == 'E')))) {
          ++i;
        }
        std::string text = s_.substr(b, i - b);
        char* endp = nullptr;
        double v = std::strtod(text.c_str(), &endp);
        if (endp == nullptr || *endp != '\0') {
          *err = "malformed number '" + text + "'";
          return false;
        }
        Token t;
        t.t = Token::T::NUMBER;
        t.text = text;
        t.num = v;
        out->push_back(t);
        continue;
      }
      // punctuation / operators
      Token t;
      t.t = Token::T::PUNCT;
      if (i + 1 < n) {
        std::string two = s_.substr(i, 2);
        if (two == "<=" || two == ">=" || two == "<>" || two == "!=") {
          t.text = two;
          out->push_back(t);
          i += 2;
          continue;
        }
      }
      t.text = std::string(1, c);
      out->push_back(t);
      ++i;
    }
    Token end;
    end.t = Token::T::END;
    out->push_back(end);
    return true;
  }

 private:
  const std::string& s_;
};

// ------------------------------------------------------------ value helpers

int Utf8CodePoints(const std::string& s) {
  int n = 0;
  for (unsigned char c : s) {
    if ((c & 0xC0) != 0x80) ++n;
  }
  return n;
}

// Parses a decimal integer exactly, rejecting values that do not fit in int64.
// Doing this on the token text (rather than on the tokenizer's double) matters:
// 9223372036854775807 is not representable as a double, so a double round trip
// would either reject a legal value or silently wrap an illegal one.
bool ParseInt64(const std::string& text, int64_t* out) {
  size_t i = 0;
  bool neg = false;
  if (i < text.size() && (text[i] == '-' || text[i] == '+')) {
    neg = text[i] == '-';
    ++i;
  }
  if (i == text.size()) return false;
  // 2^63 = 9223372036854775808, the magnitude limit for negative values.
  const uint64_t limit = neg ? 9223372036854775808ULL : 9223372036854775807ULL;
  uint64_t acc = 0;
  for (; i < text.size(); ++i) {
    char c = text[i];
    if (c < '0' || c > '9') return false;
    uint64_t digit = static_cast<uint64_t>(c - '0');
    if (acc > (limit - digit) / 10) return false;  // would overflow
    acc = acc * 10 + digit;
  }
  if (neg) {
    // -(2^63) is exactly representable; the cast below is well defined for it.
    *out = (acc == 9223372036854775808ULL) ? INT64_MIN : -static_cast<int64_t>(acc);
  } else {
    *out = static_cast<int64_t>(acc);
  }
  return true;
}

// UTF-8 byte order equals Unicode code point order, so a plain std::string
// comparison implements "compare by code point" without decoding.
int CompareValues(const Value& a, const Value& b) {
  if (a.t == Value::T::INT && b.t == Value::T::INT) {
    return a.i < b.i ? -1 : (a.i > b.i ? 1 : 0);
  }
  if (a.t == Value::T::DOUBLE && b.t == Value::T::DOUBLE) {
    return a.d < b.d ? -1 : (a.d > b.d ? 1 : 0);
  }
  if (a.t == Value::T::STR && b.t == Value::T::STR) {
    return a.s < b.s ? -1 : (a.s > b.s ? 1 : 0);
  }
  if (a.t == Value::T::INT && b.t == Value::T::DOUBLE) {
    return static_cast<double>(a.i) < b.d ? -1 : (static_cast<double>(a.i) > b.d ? 1 : 0);
  }
  if (a.t == Value::T::DOUBLE && b.t == Value::T::INT) {
    return a.d < static_cast<double>(b.i) ? -1 : (a.d > static_cast<double>(b.i) ? 1 : 0);
  }
  return 0;
}

// --------------------------------------------------------------- the "db"

struct Table {
  std::string name;
  std::vector<Column> cols;
  size_t pk = 0;
  std::vector<std::vector<Value>> rows;
  bool alive = true;
  int64_t next_rowid = 0;
};

struct Cond {
  size_t col = 0;
  std::string op;
  Value rhs;
};

struct OrderKey {
  bool is_alias = false;
  std::string name;
  bool desc = false;
};

class StubEngine {
 public:
  explicit StubEngine(std::string data_dir) : data_dir_(std::move(data_dir)) {}

  Result Execute(const std::string& sql) {
    std::vector<Token> toks;
    std::string lerr;
    Lexer lx(sql);
    if (!lx.Tokenize(&toks, &lerr)) return Err("ERR_SYNTAX", lerr);
    if (toks.size() == 1) return Result::Ok();  // empty statement
    pos_ = 0;
    toks_ = &toks;
    std::string kw = toks[0].raw;
    if (toks[0].t != Token::T::IDENT) {
      return Err("ERR_SYNTAX", "statement must start with a keyword");
    }
    if (kw == "create") return ParseCreate();
    if (kw == "insert") return ParseInsert();
    if (kw == "select") return ParseSelect();
    if (kw == "update") return ParseUpdate();
    if (kw == "delete") return ParseDelete();
    if (kw == "drop") return ParseDrop();
    if (kw == "begin" || kw == "start" || kw == "commit" || kw == "rollback" ||
        kw == "set" || kw == "end") {
      // Transaction / session statements are accepted and ignored by the stub.
      return Result::Ok();
    }
    // Report the lowercased spelling so the message is stable regardless of how
    // the caller capitalised the keyword.
    return Err("ERR_UNSUPPORTED", "unsupported statement '" + toks[0].raw + "'");
  }

 private:
  // ---------------------------------------------------------- token helpers
  const Token& Peek(size_t k = 0) const {
    size_t i = pos_ + k;
    if (i >= toks_->size()) return (*toks_)[toks_->size() - 1];
    return (*toks_)[i];
  }
  bool AtEnd() const { return Peek().t == Token::T::END; }
  void Advance() {
    if (pos_ + 1 < toks_->size()) ++pos_;
  }
  bool IsKw(const char* kw, size_t k = 0) const {
    const Token& t = Peek(k);
    return t.t == Token::T::IDENT && t.raw == kw;
  }
  bool AcceptKw(const char* kw) {
    if (!IsKw(kw)) return false;
    Advance();
    return true;
  }
  bool IsPunct(const char* p, size_t k = 0) const {
    const Token& t = Peek(k);
    return t.t == Token::T::PUNCT && t.text == p;
  }
  bool AcceptPunct(const char* p) {
    if (!IsPunct(p)) return false;
    Advance();
    return true;
  }
  std::string ExpectIdent(const char* what) {
    if (Peek().t != Token::T::IDENT) {
      err_ = std::string("expected ") + what;
      return std::string();
    }
    std::string s = Peek().text;
    Advance();
    return s;
  }
  bool ExpectPunct(const char* p) {
    if (!AcceptPunct(p)) {
      err_ = std::string("expected '") + p + "'";
      return false;
    }
    return true;
  }
  Table* FindTable(const std::string& name) {
    auto it = tables_.find(name);
    if (it == tables_.end() || !it->second.alive) return nullptr;
    return &it->second;
  }

  bool ParseVectorLiteral(const Token& t, int dim, std::vector<float>* out) {
    // Vectors arrive either bare ('[0,1]') or quoted ('[0, 1]'), and the public
    // cases use the quoted form, so accept both.
    if (t.t != Token::T::VECTOR && t.t != Token::T::STRING) {
      err_ = "expected a vector literal like '[0,1]'";
      return false;
    }
    std::string body = t.text;
    if (body.size() < 2 || body.front() != '[' || body.back() != ']') {
      err_ = "malformed vector literal";
      return false;
    }
    body = body.substr(1, body.size() - 2);
    out->clear();
    size_t i = 0;
    while (i < body.size()) {
      while (i < body.size() && (std::isspace(static_cast<unsigned char>(body[i])) || body[i] == ','))
        ++i;
      if (i >= body.size()) break;
      size_t b = i;
      while (i < body.size() && body[i] != ',') ++i;
      std::string item = body.substr(b, i - b);
      // trim
      size_t s = item.find_first_not_of(" \t\r\n");
      size_t e = item.find_last_not_of(" \t\r\n");
      if (s == std::string::npos) continue;
      item = item.substr(s, e - s + 1);
      char* endp = nullptr;
      double v = std::strtod(item.c_str(), &endp);
      if (endp == nullptr || *endp != '\0') {
        err_ = "vector component '" + item + "' is not a number";
        return false;
      }
      if (!std::isfinite(v)) {
        err_ = "vector component must be finite";
        return false;
      }
      float f = static_cast<float>(v);
      if (!std::isfinite(f)) {
        err_ = "vector component overflows float32";
        return false;
      }
      out->push_back(f);
    }
    if (dim > 0 && static_cast<int>(out->size()) != dim) {
      err_ = "vector dimension mismatch: expected " + std::to_string(dim) + ", got " +
             std::to_string(out->size());
      return false;
    }
    return true;
  }

  // Parses a literal/typed value for column `c`.
  bool ParseValueFor(const Column& c, Value* out) {
    const Token& t = Peek();
    switch (c.kind) {
      case ColKind::BIGINT: {
        if (t.t == Token::T::NUMBER) {
          int64_t v = 0;
          if (!ParseInt64(t.text, &v)) {
            err_ = "value is not a valid BIGINT";
            return false;
          }
          *out = Value::Int(v);
          Advance();
          return true;
        }
        err_ = "expected an integer for BIGINT column '" + c.name + "'";
        return false;
      }
      case ColKind::VARCHAR: {
        if (t.t != Token::T::STRING) {
          err_ = "expected a quoted string for VARCHAR column '" + c.name + "'";
          return false;
        }
        if (Utf8CodePoints(t.text) > c.len) {
          err_ = "string too long for VARCHAR(" + std::to_string(c.len) + ") column '" + c.name +
                 "'";
          return false;
        }
        *out = Value::Str(t.text);
        Advance();
        return true;
      }
      case ColKind::VECTOR: {
        std::vector<float> v;
        if (!ParseVectorLiteral(t, c.len, &v)) return false;
        *out = Value::Vec(std::move(v));
        Advance();
        return true;
      }
    }
    err_ = "unsupported column type";
    return false;
  }

  // ------------------------------------------------------------- statements
  Result ParseCreate() {
    Advance();  // create
    if (AcceptKw("vector")) {
      if (!AcceptKw("index")) return Err("ERR_SYNTAX", "expected INDEX after CREATE VECTOR");
      std::string idx = ExpectIdent("index name");
      if (idx.empty()) return Err("ERR_SYNTAX", err_);
      if (!AcceptKw("on")) return Err("ERR_SYNTAX", "expected ON");
      std::string tbl = ExpectIdent("table name");
      if (!FindTable(tbl)) return Err("ERR_NO_TABLE", "table '" + tbl + "' does not exist");
      if (!ExpectPunct("(")) return Err("ERR_SYNTAX", err_);
      std::string col = ExpectIdent("column name");
      if (!ExpectPunct(")")) return Err("ERR_SYNTAX", err_);
      Table* t = FindTable(tbl);
      bool found = false;
      for (const auto& c : t->cols) {
        if (c.name == col && c.kind == ColKind::VECTOR) found = true;
      }
      if (!found) return Err("ERR_NO_COLUMN", "no VECTOR column '" + col + "' in '" + tbl + "'");
      // The stub has no index: the statement is accepted so that protocol
      // handling of DDL can be exercised, and searches stay exact.
      return Result::Ok();
    }
    if (!AcceptKw("table")) return Err("ERR_SYNTAX", "expected TABLE");
    if (IsKw("if")) return Err("ERR_UNSUPPORTED", "CREATE TABLE IF NOT EXISTS is not supported");
    std::string name = ExpectIdent("table name");
    if (name.empty()) return Err("ERR_SYNTAX", err_);
    if (FindTable(name)) return Err("ERR_TABLE_EXISTS", "table '" + name + "' already exists");
    if (!ExpectPunct("(")) return Err("ERR_SYNTAX", err_);

    Table t;
    t.name = name;
    std::string pk_col;
    bool pk_inline = false;
    while (true) {
      if (IsKw("primary")) {
        Advance();
        if (!AcceptKw("key")) return Err("ERR_SYNTAX", "expected KEY");
        if (!ExpectPunct("(")) return Err("ERR_SYNTAX", err_);
        pk_col = ExpectIdent("primary key column");
        if (!ExpectPunct(")")) return Err("ERR_SYNTAX", err_);
        if (!AcceptPunct(",")) break;
        continue;
      }
      Column c;
      c.name = ExpectIdent("column name");
      if (c.name.empty()) return Err("ERR_SYNTAX", err_);
      std::string ty = ExpectIdent("column type");
      if (ty.empty()) return Err("ERR_SYNTAX", err_);
      std::string lower = ty;
      std::transform(lower.begin(), lower.end(), lower.begin(),
                     [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
      if (lower == "bigint" || lower == "int" || lower == "integer" || lower == "bigint(20)") {
        c.kind = ColKind::BIGINT;
      } else if (lower == "varchar" || lower == "char" || lower == "text") {
        c.kind = ColKind::VARCHAR;
        c.len = 65535;
        if (AcceptPunct("(")) {
          if (Peek().t != Token::T::NUMBER) return Err("ERR_SYNTAX", "expected length");
          c.len = static_cast<int>(Peek().num);
          Advance();
          if (!ExpectPunct(")")) return Err("ERR_SYNTAX", err_);
        }
      } else if (lower == "vector") {
        c.kind = ColKind::VECTOR;
        if (!ExpectPunct("(")) return Err("ERR_SYNTAX", "VECTOR requires a dimension");
        if (Peek().t != Token::T::NUMBER) return Err("ERR_SYNTAX", "expected dimension");
        c.len = static_cast<int>(Peek().num);
        Advance();
        if (!ExpectPunct(")")) return Err("ERR_SYNTAX", err_);
        if (c.len <= 0) return Err("ERR_TYPE", "VECTOR dimension must be positive");
      } else {
        return Err("ERR_UNSUPPORTED", "unsupported column type '" + ty + "'");
      }
      if (AcceptKw("primary")) {
        if (!AcceptKw("key")) return Err("ERR_SYNTAX", "expected KEY");
        pk_col = c.name;
        pk_inline = true;
      }
      if (IsKw("not") || IsKw("null") || IsKw("unique") || IsKw("default")) {
        return Err("ERR_UNSUPPORTED", "column constraint is not supported");
      }
      t.cols.push_back(c);
      if (!AcceptPunct(",")) break;
    }
    if (!ExpectPunct(")")) return Err("ERR_SYNTAX", err_);
    if (!AtEnd()) return Err("ERR_SYNTAX", "unexpected text after CREATE TABLE");
    (void)pk_inline;

    int vector_cols = 0;
    for (size_t i = 0; i < t.cols.size(); ++i) {
      if (t.cols[i].kind == ColKind::VECTOR) ++vector_cols;
      if (!pk_col.empty() && t.cols[i].name == pk_col) t.pk = i;
    }
    if (vector_cols > 1) return Err("ERR_TYPE", "at most one VECTOR column per table");
    if (pk_col.empty()) return Err("ERR_SYNTAX", "a BIGINT PRIMARY KEY is required");
    if (t.cols[t.pk].kind != ColKind::BIGINT) {
      return Err("ERR_TYPE", "the primary key must be BIGINT");
    }
    tables_[name] = std::move(t);
    return Result::Ok();
  }

  Result ParseDrop() {
    Advance();
    if (!AcceptKw("table")) return Err("ERR_UNSUPPORTED", "only DROP TABLE is supported");
    std::string name = ExpectIdent("table name");
    Table* t = FindTable(name);
    if (!t) return Err("ERR_NO_TABLE", "table '" + name + "' does not exist");
    t->alive = false;
    return Result::Ok();
  }

  Result ParseInsert() {
    Advance();
    if (!AcceptKw("into")) return Err("ERR_SYNTAX", "expected INTO");
    std::string name = ExpectIdent("table name");
    Table* t = FindTable(name);
    if (!t) return Err("ERR_NO_TABLE", "table '" + name + "' does not exist");

    std::vector<size_t> target;
    if (AcceptPunct("(")) {
      while (true) {
        std::string c = ExpectIdent("column name");
        bool found = false;
        for (size_t i = 0; i < t->cols.size(); ++i) {
          if (t->cols[i].name == c) {
            target.push_back(i);
            found = true;
            break;
          }
        }
        if (!found) return Err("ERR_NO_COLUMN", "no column '" + c + "' in '" + name + "'");
        if (!AcceptPunct(",")) break;
      }
      if (!ExpectPunct(")")) return Err("ERR_SYNTAX", err_);
    } else {
      for (size_t i = 0; i < t->cols.size(); ++i) target.push_back(i);
    }

    if (!AcceptKw("values") && !AcceptKw("value")) return Err("ERR_SYNTAX", "expected VALUES");
    int64_t affected = 0;
    std::vector<std::vector<Value>> staged;
    while (true) {
      if (!ExpectPunct("(")) return Err("ERR_SYNTAX", err_);
      std::vector<Value> row(t->cols.size());
      std::vector<bool> given(t->cols.size(), false);
      size_t k = 0;
      while (true) {
        if (k >= target.size()) return Err("ERR_SYNTAX", "too many values in VALUES row");
        const Column& c = t->cols[target[k]];
        Value v;
        if (!ParseValueFor(c, &v)) return Err("ERR_TYPE", err_);
        row[target[k]] = std::move(v);
        given[target[k]] = true;
        ++k;
        if (!AcceptPunct(",")) break;
      }
      if (!ExpectPunct(")")) return Err("ERR_SYNTAX", err_);
      if (k != target.size()) return Err("ERR_SYNTAX", "not enough values in VALUES row");
      for (size_t i = 0; i < t->cols.size(); ++i) {
        if (!given[i]) {
          return Err("ERR_UNSUPPORTED", "column '" + t->cols[i].name + "' requires a value");
        }
      }
      // primary key uniqueness (staged rows included)
      const Value& pkv = row[t->pk];
      for (const auto& r : t->rows) {
        if (CompareValues(r[t->pk], pkv) == 0) {
          return Err("ERR_DUP_KEY", "duplicate primary key " + std::to_string(pkv.i));
        }
      }
      for (const auto& r : staged) {
        if (CompareValues(r[t->pk], pkv) == 0) {
          return Err("ERR_DUP_KEY", "duplicate primary key " + std::to_string(pkv.i));
        }
      }
      staged.push_back(std::move(row));
      ++affected;
      if (!AcceptPunct(",")) break;
    }
    for (auto& r : staged) t->rows.push_back(std::move(r));
    return Result::Affected(affected);
  }

  bool ParseConditions(Table* t, std::vector<Cond>* conds) {
    if (!AcceptKw("where")) return true;
    while (true) {
      std::string col = ExpectIdent("column name");
      if (col.empty()) return false;
      size_t idx = 0;
      bool found = false;
      for (size_t i = 0; i < t->cols.size(); ++i) {
        if (t->cols[i].name == col) {
          idx = i;
          found = true;
          break;
        }
      }
      if (!found) {
        err_ = "no column '" + col + "'";
        return false;
      }
      Cond c;
      c.col = idx;
      const Token& op = Peek();
      if (op.t != Token::T::PUNCT) {
        err_ = "expected a comparison operator after '" + col + "'";
        return false;
      }
      c.op = op.text;
      if (c.op != "=" && c.op != "<" && c.op != "<=" && c.op != ">" && c.op != ">=" &&
          c.op != "<>" && c.op != "!=") {
        err_ = "unsupported operator '" + c.op + "'";
        return false;
      }
      Advance();
      if (!ParseValueFor(t->cols[idx], &c.rhs)) return false;
      conds->push_back(std::move(c));
      if (AcceptKw("and")) continue;
      if (IsKw("or")) {
        err_ = "OR is not supported";
        return false;
      }
      break;
    }
    return true;
  }

  bool RowMatches(const std::vector<Value>& row, const std::vector<Cond>& conds) {
    for (const auto& c : conds) {
      int cmp = CompareValues(row[c.col], c.rhs);
      bool ok = false;
      if (c.op == "=") ok = cmp == 0;
      else if (c.op == "<") ok = cmp < 0;
      else if (c.op == "<=") ok = cmp <= 0;
      else if (c.op == ">") ok = cmp > 0;
      else if (c.op == ">=") ok = cmp >= 0;
      else if (c.op == "<>" || c.op == "!=") ok = cmp != 0;
      if (!ok) return false;
    }
    return true;
  }

  Result ParseDelete() {
    Advance();
    if (!AcceptKw("from")) return Err("ERR_SYNTAX", "expected FROM");
    std::string name = ExpectIdent("table name");
    Table* t = FindTable(name);
    if (!t) return Err("ERR_NO_TABLE", "table '" + name + "' does not exist");
    std::vector<Cond> conds;
    if (!ParseConditions(t, &conds)) return Err("ERR_SYNTAX", err_);
    if (!AtEnd()) return Err("ERR_SYNTAX", "unexpected text after DELETE");
    int64_t affected = 0;
    std::vector<std::vector<Value>> keep;
    for (auto& r : t->rows) {
      if (RowMatches(r, conds)) {
        ++affected;
      } else {
        keep.push_back(std::move(r));
      }
    }
    t->rows = std::move(keep);
    return Result::Affected(affected);
  }

  Result ParseUpdate() {
    Advance();
    std::string name = ExpectIdent("table name");
    Table* t = FindTable(name);
    if (!t) return Err("ERR_NO_TABLE", "table '" + name + "' does not exist");
    if (!AcceptKw("set")) return Err("ERR_SYNTAX", "expected SET");
    struct Assign {
      size_t col;
      Value v;
    };
    std::vector<Assign> assigns;
    while (true) {
      std::string col = ExpectIdent("column name");
      size_t idx = 0;
      bool found = false;
      for (size_t i = 0; i < t->cols.size(); ++i) {
        if (t->cols[i].name == col) {
          idx = i;
          found = true;
          break;
        }
      }
      if (!found) return Err("ERR_NO_COLUMN", "no column '" + col + "'");
      if (!ExpectPunct("=")) return Err("ERR_SYNTAX", err_);
      Value v;
      if (!ParseValueFor(t->cols[idx], &v)) return Err("ERR_TYPE", err_);
      if (idx == t->pk) return Err("ERR_UNSUPPORTED", "the primary key cannot be updated");
      assigns.push_back({idx, std::move(v)});
      if (!AcceptPunct(",")) break;
    }
    std::vector<Cond> conds;
    if (!ParseConditions(t, &conds)) return Err("ERR_SYNTAX", err_);
    if (!AtEnd()) return Err("ERR_SYNTAX", "unexpected text after UPDATE");
    int64_t affected = 0;
    for (auto& r : t->rows) {
      if (!RowMatches(r, conds)) continue;
      for (const auto& a : assigns) r[a.col] = a.v;
      ++affected;
    }
    return Result::Affected(affected);
  }

  struct SelectItem {
    enum class K { COL, DIST } k = K::COL;
    size_t col = 0;
    std::string src_col;  // column name as written in the query
    std::string alias;    // output column name
    std::vector<float> query;
  };

  Result ParseSelect() {
    Advance();
    if (IsKw("distinct")) return Err("ERR_UNSUPPORTED", "DISTINCT is not supported");
    std::string name;
    std::vector<SelectItem> items;
    bool star = false;
    if (AcceptPunct("*")) {
      star = true;
    } else {
      while (true) {
        SelectItem it;
        if (IsKw("l2_distance")) {
          Advance();
          if (!ExpectPunct("(")) return Err("ERR_SYNTAX", err_);
          std::string col = ExpectIdent("vector column");
          it.k = SelectItem::K::DIST;
          // resolved against the table below, once FROM has been parsed
          it.src_col = col;
          it.alias = col;
          if (!ExpectPunct(",")) return Err("ERR_SYNTAX", err_);
          std::vector<float> q;
          if (!ParseVectorLiteral(Peek(), 0, &q)) return Err("ERR_TYPE", err_);
          Advance();
          if (!ExpectPunct(")")) return Err("ERR_SYNTAX", err_);
          it.query = std::move(q);
          it.col = 0;
          items.push_back(std::move(it));
          if (AcceptKw("as")) {
            std::string a = ExpectIdent("alias");
            if (a.empty()) return Err("ERR_SYNTAX", err_);
            items.back().alias = a;
          }
          if (!AcceptPunct(",")) break;
          continue;
        }
        std::string col = ExpectIdent("column name");
        if (col.empty()) return Err("ERR_SYNTAX", err_);
        it.k = SelectItem::K::COL;
        it.src_col = col;
        it.alias = col;
        items.push_back(std::move(it));
        if (AcceptKw("as")) {
          std::string a = ExpectIdent("alias");
          if (a.empty()) return Err("ERR_SYNTAX", err_);
          items.back().alias = a;
        }
        if (!AcceptPunct(",")) break;
      }
    }
    if (!AcceptKw("from")) return Err("ERR_SYNTAX", "expected FROM");
    name = ExpectIdent("table name");
    Table* t = FindTable(name);
    if (!t) return Err("ERR_NO_TABLE", "table '" + name + "' does not exist");
    if (IsKw("as")) return Err("ERR_UNSUPPORTED", "table aliases are not supported");

    // Resolve item columns.
    std::vector<size_t> out_cols;
    if (star) {
      for (size_t i = 0; i < t->cols.size(); ++i) out_cols.push_back(i);
    } else {
      for (auto& it : items) {
        if (it.k == SelectItem::K::COL) {
          bool found = false;
          for (size_t i = 0; i < t->cols.size(); ++i) {
            if (t->cols[i].name == it.src_col) {
              it.col = i;
              found = true;
              break;
            }
          }
          if (!found) return Err("ERR_NO_COLUMN", "no column '" + it.src_col + "'");
        } else {
          bool found = false;
          for (size_t i = 0; i < t->cols.size(); ++i) {
            if (t->cols[i].name == it.src_col && t->cols[i].kind == ColKind::VECTOR) {
              it.col = i;
              found = true;
              break;
            }
          }
          if (!found) return Err("ERR_NO_COLUMN", "no VECTOR column '" + it.src_col + "'");
          if (static_cast<int>(it.query.size()) != t->cols[it.col].len) {
            return Err("ERR_TYPE", "query vector dimension mismatch");
          }
        }
      }
    }

    std::vector<Cond> conds;
    if (!ParseConditions(t, &conds)) return Err("ERR_SYNTAX", err_);

    std::vector<OrderKey> order;
    if (AcceptKw("order")) {
      if (!AcceptKw("by")) return Err("ERR_SYNTAX", "expected BY");
      while (true) {
        OrderKey k;
        k.name = ExpectIdent("order column");
        if (k.name.empty()) return Err("ERR_SYNTAX", err_);
        if (AcceptKw("desc")) k.desc = true;
        else AcceptKw("asc");
        order.push_back(k);
        if (!AcceptPunct(",")) break;
      }
    }

    int64_t limit = -1;
    if (AcceptKw("limit")) {
      if (Peek().t != Token::T::NUMBER) return Err("ERR_SYNTAX", "expected LIMIT count");
      limit = static_cast<int64_t>(Peek().num);
      Advance();
      if (limit < 0) return Err("ERR_SYNTAX", "LIMIT must be non-negative");
      if (IsPunct(",")) return Err("ERR_UNSUPPORTED", "LIMIT offset,count is not supported");
    }
    if (IsKw("offset")) return Err("ERR_UNSUPPORTED", "OFFSET is not supported");
    if (!AtEnd()) return Err("ERR_SYNTAX", "unexpected text after SELECT");

    // Resolve ORDER BY keys: alias first, then a plain column name.
    struct Resolved {
      bool is_dist = false;
      size_t item = 0;  // index into items (projection) or column index
      size_t col = 0;
      bool desc = false;
    };
    std::vector<Resolved> rorder;
    for (const auto& k : order) {
      Resolved r;
      r.desc = k.desc;
      bool done = false;
      for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].alias == k.name) {
          r.item = i;
          r.is_dist = items[i].k == SelectItem::K::DIST;
          r.col = items[i].col;
          done = true;
          break;
        }
      }
      if (!done) {
        for (size_t i = 0; i < t->cols.size(); ++i) {
          if (t->cols[i].name == k.name) {
            r.col = i;
            r.is_dist = false;
            done = true;
            break;
          }
        }
      }
      if (!done) return Err("ERR_NO_COLUMN", "no column or alias '" + k.name + "'");
      rorder.push_back(r);
    }

    // Materialize matching rows together with their projected values and the
    // sort keys.
    struct OutRow {
      std::vector<Value> vals;
      std::vector<Value> keys;
      int64_t pk = 0;
    };
    std::vector<OutRow> out;
    for (const auto& row : t->rows) {
      if (!RowMatches(row, conds)) continue;
      OutRow o;
      o.pk = row[t->pk].i;
      if (star) {
        for (size_t i = 0; i < t->cols.size(); ++i) o.vals.push_back(row[i]);
      } else {
        for (const auto& it : items) {
          if (it.k == SelectItem::K::COL) {
            o.vals.push_back(row[it.col]);
          } else {
            double sum = 0;
            const auto& v = row[it.col].v;
            for (size_t d = 0; d < v.size() && d < it.query.size(); ++d) {
              double diff = static_cast<double>(v[d]) - static_cast<double>(it.query[d]);
              sum += diff * diff;
            }
            o.vals.push_back(Value::Double(std::sqrt(sum)));
          }
        }
      }
      for (const auto& r : rorder) {
        if (r.is_dist) {
          double sum = 0;
          const auto& v = row[r.col].v;
          for (size_t d = 0; d < v.size() && d < items[r.item].query.size(); ++d) {
            double diff = static_cast<double>(v[d]) - static_cast<double>(items[r.item].query[d]);
            sum += diff * diff;
          }
          o.keys.push_back(Value::Double(std::sqrt(sum)));
        } else {
          o.keys.push_back(row[r.col]);
        }
      }
      out.push_back(std::move(o));
    }

    if (!rorder.empty()) {
      std::stable_sort(out.begin(), out.end(), [&](const OutRow& a, const OutRow& b) {
        for (size_t i = 0; i < rorder.size(); ++i) {
          int c = CompareValues(a.keys[i], b.keys[i]);
          if (c != 0) return rorder[i].desc ? c > 0 : c < 0;
        }
        return a.pk < b.pk;  // ties break by primary key ascending
      });
    }

    if (limit >= 0 && static_cast<int64_t>(out.size()) > limit) {
      out.resize(static_cast<size_t>(limit));
    }

    std::vector<std::string> cols;
    if (star) {
      for (const auto& c : t->cols) cols.push_back(c.name);
    } else {
      for (const auto& it : items) cols.push_back(it.alias);
    }
    std::vector<std::vector<Value>> rows;
    rows.reserve(out.size());
    for (auto& o : out) rows.push_back(std::move(o.vals));
    return Result::Rows(std::move(cols), std::move(rows));
  }

  std::string data_dir_;
  std::map<std::string, Table> tables_;
  const std::vector<Token>* toks_ = nullptr;
  size_t pos_ = 0;
  std::string err_;
};

}  // namespace

// The concrete implementation behind the Engine facade.  Today it is the stub;
// when the real storage layer lands it will be swapped here and nothing in the
// protocol layer needs to change.
class Engine::Impl : public StubEngine {
 public:
  using StubEngine::StubEngine;
};

Engine::Engine(std::string data_dir) : data_dir_(std::move(data_dir)) {
  impl_ = std::make_unique<Impl>(data_dir_);
}

Engine::~Engine() = default;

Result Engine::Execute(const std::string& sql) { return impl_->Execute(sql); }

}  // namespace mvd
