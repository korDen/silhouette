#pragma once
// uic lexer — two reading modes, one position:
//
//   - TOKEN mode (`next`): identifiers, numbers, dimension literals
//     (number glued to a `% @ h w a s p i` suffix), color literals
//     (#hex), resource paths (/-rooted), strings (the only quoted
//     thing — text), operators/punctuation. Used for expressions and
//     all structural syntax.
//   - RAW mode (`rawValue`): everything up to the terminating `;`,
//     verbatim (trimmed). Used for plain attribute values — ported
//     trees stay byte-diffable against their source material.
//
// `/` is a path literal in operand position and division in operator
// position (the lexer tracks whether the previous significant token
// can end an operand — the classic regex-literal rule).
#include <cctype>
#include <string_view>

namespace uic {

enum class Tok {
  kEnd,
  kIdent,
  kNumber,
  kDim,
  kColor,
  kPath,
  kString, // text includes the quotes' content only (escapes resolved)
  kLBrace,
  kRBrace,
  kLParen,
  kRParen,
  kLBracket,
  kRBracket,
  kColon,
  kSemi,
  kComma,
  kDot,
  kQuestion,
  kBang,
  kPlus,
  kMinus,
  kStar,
  kSlash,
  kLt,
  kGt,
  kLe,
  kGe,
  kEqEq,
  kNe,
  kAndAnd,
  kOrOr,
  kEq,
  kArrow,
  kError,
};

struct Token {
  Tok kind = Tok::kEnd;
  std::string_view text;
  int line = 1;
  int col = 1;
};

class Lexer {
public:
  explicit Lexer(std::string_view src) : src_(src) {}

  Token next() {
    skipTrivia();
    Token t;
    t.line = line_;
    t.col = col_;
    if (pos_ >= src_.size()) {
      t.kind = Tok::kEnd;
      return t;
    }
    const size_t start = pos_;
    const char c = src_[pos_];

    if (std::isalpha((unsigned char)c) != 0 || c == '_') {
      while (pos_ < src_.size() && (std::isalnum((unsigned char)src_[pos_]) != 0 ||
                                    src_[pos_] == '_')) {
        advance();
      }
      t.kind = Tok::kIdent;
      t.text = src_.substr(start, pos_ - start);
      prevOperand_ = true;
      return t;
    }
    if (std::isdigit((unsigned char)c) != 0 ||
        (c == '.' && pos_ + 1 < src_.size() &&
         std::isdigit((unsigned char)src_[pos_ + 1]) != 0)) {
      lexNumberOrDim(t, start);
      return t;
    }
    if (c == '#') {
      advance();
      while (pos_ < src_.size() &&
             std::isxdigit((unsigned char)src_[pos_]) != 0) {
        advance();
      }
      t.kind = Tok::kColor;
      t.text = src_.substr(start, pos_ - start);
      prevOperand_ = true;
      return t;
    }
    if (c == '"') {
      lexString(t, start);
      return t;
    }
    if (c == '/' && !prevOperand_ && pos_ + 1 < src_.size() &&
        (std::isalpha((unsigned char)src_[pos_ + 1]) != 0 ||
         src_[pos_ + 1] == '_')) {
      while (pos_ < src_.size() &&
             (std::isalnum((unsigned char)src_[pos_]) != 0 ||
              src_[pos_] == '_' || src_[pos_] == '/' || src_[pos_] == '.' ||
              src_[pos_] == '-')) {
        advance();
      }
      t.kind = Tok::kPath;
      t.text = src_.substr(start, pos_ - start);
      prevOperand_ = true;
      return t;
    }

    advance();
    auto two = [&](char n, Tok yes, Tok no) {
      if (pos_ < src_.size() && src_[pos_] == n) {
        advance();
        t.kind = yes;
      } else {
        t.kind = no;
      }
    };
    prevOperand_ = false;
    switch (c) {
    case '{':
      t.kind = Tok::kLBrace;
      break;
    case '}':
      t.kind = Tok::kRBrace;
      prevOperand_ = true; // an initializer/body can end an operand
      break;
    case '(':
      t.kind = Tok::kLParen;
      break;
    case ')':
      t.kind = Tok::kRParen;
      prevOperand_ = true;
      break;
    case '[':
      t.kind = Tok::kLBracket;
      break;
    case ']':
      t.kind = Tok::kRBracket;
      prevOperand_ = true;
      break;
    case ':':
      t.kind = Tok::kColon;
      break;
    case ';':
      t.kind = Tok::kSemi;
      break;
    case ',':
      t.kind = Tok::kComma;
      break;
    case '.':
      t.kind = Tok::kDot;
      break;
    case '?':
      t.kind = Tok::kQuestion;
      break;
    case '+':
      t.kind = Tok::kPlus;
      break;
    case '*':
      t.kind = Tok::kStar;
      break;
    case '/':
      t.kind = Tok::kSlash;
      break;
    case '!':
      two('=', Tok::kNe, Tok::kBang);
      break;
    case '<':
      two('=', Tok::kLe, Tok::kLt);
      break;
    case '>':
      two('=', Tok::kGe, Tok::kGt);
      break;
    case '=':
      two('=', Tok::kEqEq, Tok::kEq);
      break;
    case '-':
      two('>', Tok::kArrow, Tok::kMinus);
      break;
    case '&':
      two('&', Tok::kAndAnd, Tok::kError);
      break;
    case '|':
      two('|', Tok::kOrOr, Tok::kError);
      break;
    default:
      t.kind = Tok::kError;
      break;
    }
    t.text = src_.substr(start, pos_ - start);
    return t;
  }

  // RAW mode: everything up to the terminating ';' (consumed), trimmed.
  // Quote-aware: a ';' inside a quoted text segment does not terminate
  // (text is the one quoted thing, and text may contain anything). A
  // value cannot span lines; rawTerminated() reports whether the ';'
  // was actually found (the parser turns a miss into a diagnostic).
  std::string_view rawValue() {
    skipTrivia();
    const size_t start = pos_;
    size_t end = start;
    bool quoted = false;
    while (pos_ < src_.size() && src_[pos_] != '\n' &&
           (quoted || src_[pos_] != ';')) {
      if (src_[pos_] == '"') {
        quoted = !quoted;
      } else if (quoted && src_[pos_] == '\\' && pos_ + 1 < src_.size() &&
                 src_[pos_ + 1] != '\n') {
        advance();
      }
      advance();
      end = pos_;
    }
    rawTerminated_ = pos_ < src_.size() && src_[pos_] == ';';
    if (rawTerminated_) {
      advance();
    }
    while (end > start &&
           std::isspace((unsigned char)src_[end - 1]) != 0) {
      --end;
    }
    prevOperand_ = false;
    return src_.substr(start, end - start);
  }
  bool rawTerminated() const { return rawTerminated_; }

  // native fn body: skip to '{{{', capture verbatim until '}}}'.
  bool nativeBody(std::string_view *body) {
    skipTrivia();
    if (src_.compare(pos_, 3, "{{{") != 0) {
      return false;
    }
    advance();
    advance();
    advance();
    const size_t start = pos_;
    while (pos_ + 2 < src_.size() && src_.compare(pos_, 3, "}}}") != 0) {
      advance();
    }
    if (src_.compare(pos_, 3, "}}}") != 0) {
      return false;
    }
    *body = src_.substr(start, pos_ - start);
    advance();
    advance();
    advance();
    prevOperand_ = false;
    return true;
  }

  int line() const { return line_; }

private:
  void advance() {
    if (src_[pos_] == '\n') {
      ++line_;
      col_ = 1;
    } else {
      ++col_;
    }
    ++pos_;
  }

  void skipTrivia() {
    for (;;) {
      while (pos_ < src_.size() &&
             std::isspace((unsigned char)src_[pos_]) != 0) {
        advance();
      }
      if (pos_ + 1 < src_.size() && src_[pos_] == '/' &&
          src_[pos_ + 1] == '/') {
        while (pos_ < src_.size() && src_[pos_] != '\n') {
          advance();
        }
        continue;
      }
      return;
    }
  }

  void lexNumberOrDim(Token &t, size_t start) {
    bool dot = false;
    while (pos_ < src_.size()) {
      const char c = src_[pos_];
      if (std::isdigit((unsigned char)c) != 0) {
        advance();
      } else if (c == '.' && !dot) {
        dot = true;
        advance();
      } else {
        break;
      }
    }
    t.kind = Tok::kNumber;
    if (pos_ < src_.size()) {
      const char u = src_[pos_];
      const bool suffix = u == '%' || u == '@' || u == 'h' || u == 'w' ||
                          u == 'a' || u == 's' || u == 'p' || u == 'i';
      const bool glued =
          pos_ + 1 >= src_.size() ||
          (std::isalnum((unsigned char)src_[pos_ + 1]) == 0 &&
           src_[pos_ + 1] != '_');
      if (suffix && glued) {
        advance();
        t.kind = Tok::kDim;
      }
    }
    t.text = src_.substr(start, pos_ - start);
    prevOperand_ = true;
  }

  void lexString(Token &t, size_t start) {
    advance(); // opening quote
    while (pos_ < src_.size() && src_[pos_] != '"') {
      if (src_[pos_] == '\\' && pos_ + 1 < src_.size()) {
        advance();
      }
      advance();
    }
    if (pos_ >= src_.size()) {
      t.kind = Tok::kError;
      t.text = src_.substr(start, pos_ - start);
      return;
    }
    advance(); // closing quote
    t.kind = Tok::kString;
    t.text = src_.substr(start + 1, pos_ - start - 2); // sans quotes
    prevOperand_ = true;
  }

  std::string_view src_;
  size_t pos_ = 0;
  int line_ = 1, col_ = 1;
  bool prevOperand_ = false;
  bool rawTerminated_ = false;
};

} // namespace uic
