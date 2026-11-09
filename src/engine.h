// MiniVectorDB — engine interface.
//
// This header is the contract between the protocol layer (stdin/stdout) and the
// storage/execution layer.  The current implementation behind it
// (src/engine.cpp) is a deliberately small in-memory STUB: it exists so the
// protocol layer, the local driver and the Docker build can be validated end to
// end before the real engine is written.  It does not persist anything.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mvd {

enum class ColKind { BIGINT, VARCHAR, VECTOR };

struct Column {
  std::string name;
  ColKind kind = ColKind::BIGINT;
  int len = 0;  // VARCHAR(n) code points / VECTOR(n) dimensions
};

struct Value {
  enum class T { INT, STR, VEC, DOUBLE } t = T::INT;
  int64_t i = 0;
  double d = 0;
  std::string s;
  std::vector<float> v;

  static Value Int(int64_t x) {
    Value r;
    r.t = T::INT;
    r.i = x;
    return r;
  }
  static Value Str(std::string x) {
    Value r;
    r.t = T::STR;
    r.s = std::move(x);
    return r;
  }
  static Value Vec(std::vector<float> x) {
    Value r;
    r.t = T::VEC;
    r.v = std::move(x);
    return r;
  }
  static Value Double(double x) {
    Value r;
    r.t = T::DOUBLE;
    r.d = x;
    return r;
  }
};

struct Result {
  enum class Kind { ROWS, AFFECTED, OK, ERROR } kind = Kind::OK;
  std::vector<std::string> columns;
  std::vector<std::vector<Value>> rows;
  int64_t affected = 0;
  std::string code;     // stable error code, e.g. ERR_SYNTAX
  std::string message;  // human readable, single line

  static Result Ok() { return Result(); }
  static Result Affected(int64_t n) {
    Result r;
    r.kind = Kind::AFFECTED;
    r.affected = n;
    return r;
  }
  static Result Error(std::string c, std::string m) {
    Result r;
    r.kind = Kind::ERROR;
    r.code = std::move(c);
    r.message = std::move(m);
    return r;
  }
  static Result Rows(std::vector<std::string> cols,
                     std::vector<std::vector<Value>> rs) {
    Result r;
    r.kind = Kind::ROWS;
    r.columns = std::move(cols);
    r.rows = std::move(rs);
    return r;
  }
};

// Executes one complete SQL statement (no trailing semicolon required).
class Engine {
 public:
  explicit Engine(std::string data_dir);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  Result Execute(const std::string& sql);

  // Set by the protocol layer before Execute() when the driver asked for a
  // different vector search mode.  The stub ignores it beyond echoing it back.
  void set_vector_mode(const std::string& mode) { vector_mode_ = mode; }
  const std::string& vector_mode() const { return vector_mode_; }

 private:
  class Impl;
  std::string data_dir_;
  std::string vector_mode_ = "exact";
  std::unique_ptr<Impl> impl_;
};

}  // namespace mvd
