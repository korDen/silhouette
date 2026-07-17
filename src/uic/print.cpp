// print.cpp — the printer: AST -> canonical .ui text. The ONE writer of
// .ui files: hosts convert foreign markup into the AST, transform it,
// and print here; it is also the syntax-migration tool (parse old,
// print new). parse(print(m)) must reproduce m exactly (the round-trip
// test pins it) — comments are not part of the AST and do not survive.
#include "uic/uic.hpp"

#include <sstream>
#include <string>

namespace uic {

namespace {

// expression precedence, mirroring the parser's ladder: ternary(0) <
// || < && < ==/!= < relational < additive < multiplicative < unary <
// postfix
int precOf(const Expr &e) {
  switch (e.kind) {
  case Expr::kTernary:
    return 0;
  case Expr::kBinary:
    if (e.text == "||") {
      return 1;
    }
    if (e.text == "&&") {
      return 2;
    }
    if (e.text == "==" || e.text == "!=") {
      return 3;
    }
    if (e.text == "<" || e.text == ">" || e.text == "<=" || e.text == ">=") {
      return 4;
    }
    if (e.text == "+" || e.text == "-") {
      return 5;
    }
    return 6; // * /
  case Expr::kUnary:
    return 7;
  default:
    return 8; // postfix + primaries
  }
}

void printExpr(std::ostream &os, const Expr &e, int parentPrec) {
  const int prec = precOf(e);
  const bool parens = prec < parentPrec;
  if (parens) {
    os << '(';
  }
  switch (e.kind) {
  case Expr::kIdent:
  case Expr::kNumber:
  case Expr::kDim:
  case Expr::kColor:
  case Expr::kPath:
  case Expr::kBool:
    os << e.text;
    break;
  case Expr::kString:
    os << '"' << e.text << '"';
    break;
  case Expr::kField:
    printExpr(os, *e.args[0], prec);
    os << '.' << e.text;
    break;
  case Expr::kIndex:
    printExpr(os, *e.args[0], prec);
    os << '[';
    printExpr(os, *e.args[1], 0);
    os << ']';
    break;
  case Expr::kCall:
    printExpr(os, *e.args[0], prec);
    os << '(';
    for (size_t i = 1; i < e.args.size(); ++i) {
      if (i > 1) {
        os << ", ";
      }
      printExpr(os, *e.args[i], 0);
    }
    os << ')';
    break;
  case Expr::kUnary:
    os << e.text;
    printExpr(os, *e.args[0], prec);
    break;
  case Expr::kBinary:
    printExpr(os, *e.args[0], prec);
    os << ' ' << e.text << ' ';
    // right operand needs strictly-higher precedence to reassociate
    printExpr(os, *e.args[1], prec + 1);
    break;
  case Expr::kTernary:
    printExpr(os, *e.args[0], 1);
    os << " ? ";
    printExpr(os, *e.args[1], 0);
    os << " : ";
    printExpr(os, *e.args[2], 0);
    break;
  }
  if (parens) {
    os << ')';
  }
}

std::string exprText(const Expr &e) {
  std::ostringstream os;
  printExpr(os, e, 0);
  return os.str();
}

class Printer {
 public:
  std::string run(const Module &m) {
    for (const Import &imp : m.imports) {
      line(importText(imp));
    }
    gap();
    for (const ConstDecl &c : m.consts) {
      line(constText(c));
    }
    gap();
    for (const StructDecl &s : m.structs) {
      printStruct(s);
      gap();
    }
    for (const EnumDecl &e : m.enums) {
      printEnum(e);
      gap();
    }
    for (const StyleDecl &s : m.styles) {
      printStyle(s);
      gap();
    }
    for (const FnDecl &f : m.fns) {
      printFn(f);
      gap();
    }
    for (const TemplateDecl &t : m.templates) {
      printTemplate(t);
      gap();
    }
    for (const NodePtr &n : m.roots) {
      printNode(*n);
      gap();
    }
    return std::move(out_).str();
  }

 private:
  void line(const std::string &s) {
    out_ << std::string(static_cast<size_t>(indent_) * 4, ' ') << s << '\n';
    blank_ = false;
  }
  // one blank line between top-level declarations (never two)
  void gap() {
    if (!blank_ && out_.tellp() != std::streampos(0)) {
      out_ << '\n';
      blank_ = true;
    }
  }

  static std::string importText(const Import &imp) {
    std::string s = "import { ";
    for (size_t i = 0; i < imp.names.size(); ++i) {
      if (i > 0) {
        s += ", ";
      }
      s += imp.names[i];
    }
    return s + " } from \"" + imp.from + "\";";
  }

  std::string constText(const ConstDecl &c) {
    std::string s = "const " + c.name;
    if (!c.type.empty()) {
      s += ": " + c.type;
    }
    if (!c.initType.empty()) {
      s += " = " + c.initType + " {";
      for (const Attr &f : c.initFields) {
        s += " " + f.name + ": " + f.value + ";";
      }
      return s + " }";
    }
    return s + " = " + c.rawValue + ";";
  }

  void printStruct(const StructDecl &s) {
    line("struct " + s.name + " {");
    ++indent_;
    for (const StructField &f : s.fields) {
      std::string d = f.name + ": " + f.type;
      if (f.arrayLen != 0) {
        d += "[" + std::to_string(f.arrayLen) + "]";
      }
      if (f.hasDefault) {
        d += " = " + f.defaultValue;
      }
      line(d + ";");
    }
    --indent_;
    line("}");
  }

  void printEnum(const EnumDecl &e) {
    line("enum " + e.name + " {");
    ++indent_;
    for (const EnumEntry &entry : e.entries) {
      line(entry.name + (entry.hasValue ? " = " + entry.value : "") + ",");
    }
    --indent_;
    line("}");
  }

  void printStyle(const StyleDecl &s) {
    line("style " + s.name + " {");
    ++indent_;
    printAttrs(s.attrs);
    --indent_;
    line("}");
  }

  void printFn(const FnDecl &f) {
    std::string sig = (f.isNative ? "native fn " : "fn ") + f.name + "(";
    for (size_t i = 0; i < f.params.size(); ++i) {
      if (i > 0) {
        sig += ", ";
      }
      sig += f.params[i].name + ": " + f.params[i].type;
    }
    sig += ") -> " + f.returnType;
    if (f.isNative) {
      line(sig + " {{{" + f.native + "}}}");
    } else {
      line(sig + " = " + exprText(*f.body) + ";");
    }
  }

  void printTemplate(const TemplateDecl &t) {
    line("template " + t.name + " {");
    ++indent_;
    for (const InParam &p : t.ins) {
      std::string d = "in " + p.name + ": " + p.type;
      if (p.hasDefault) {
        d += " = " + p.defaultValue;
      }
      line(d + ";");
    }
    for (const NodePtr &n : t.body) {
      printNode(*n);
    }
    --indent_;
    line("}");
  }

  // attrs pack onto lines while they fit (the committed trees stay
  // readable); binds/actions/children always break
  void printAttrs(const std::vector<Attr> &attrs) {
    std::string cur;
    for (const Attr &a : attrs) {
      const std::string one = a.name + ": " + a.value + ";";
      if (cur.empty()) {
        cur = one;
      } else if (cur.size() + 1 + one.size() +
                     static_cast<size_t>(indent_) * 4 <=
                 72) {
        cur += " " + one;
      } else {
        line(cur);
        cur = one;
      }
    }
    if (!cur.empty()) {
      line(cur);
    }
  }

  void printNode(const Node &n) {
    if (n.kind == Node::kIf) {
      for (size_t i = 0; i < n.arms.size(); ++i) {
        const IfArm &arm = n.arms[i];
        std::string head =
            i == 0 ? "if" : (arm.cond != nullptr ? "} else if" : "} else");
        if (arm.cond != nullptr) {
          head += " (" + exprText(*arm.cond) + ")";
        }
        line(head + " {");
        ++indent_;
        for (const NodePtr &b : arm.body) {
          printNode(*b);
        }
        --indent_;
      }
      line("}");
      return;
    }
    const std::string open =
        n.kind == Node::kWidgetState ? "widgetstate " + n.tag : n.tag;
    // compact form for leaf nodes with few attrs (instances read as
    // one line, like the source XML)
    if (n.children.empty() && n.binds.empty() && n.actions.empty() &&
        n.arms.empty() && n.attrs.size() <= 3) {
      std::string one = open + " {";
      for (const Attr &a : n.attrs) {
        one += " " + a.name + ": " + a.value + ";";
      }
      one += " }";
      if (one.size() + static_cast<size_t>(indent_) * 4 <= 76) {
        line(one);
        return;
      }
    }
    line(open + " {");
    ++indent_;
    printAttrs(n.attrs);
    for (const Bind &b : n.binds) {
      line("bind " + b.target + ": " + exprText(*b.expr) + ";");
    }
    for (const Action &a : n.actions) {
      line("action " + a.event + ": " + exprText(*a.expr) + ";");
    }
    for (const NodePtr &c : n.children) {
      printNode(*c);
    }
    --indent_;
    line("}");
  }

  std::ostringstream out_;
  int indent_ = 0;
  bool blank_ = false;
};

} // namespace

std::string printModule(const Module &m) { return Printer().run(m); }

} // namespace uic
