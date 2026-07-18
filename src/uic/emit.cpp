// uic emit pass — panel codegen with THE LAYOUT SCHEDULER (M-A): the
// solve laws live in the spec's layout annex (spec-first — this file
// implements the annex). A module compiles
// to a self-contained header:
//
//   template <class Sink>
//   void <module>(const NS::UiSnapshot&, float screenW, float screenH,
//                 Sink&, RectLog* = nullptr);
//
// Generated shape: one flat set of rect variables, a SOLVE block that
// replays the schedule (document order; float-chain cursors; grow
// unions run incrementally inside a two-pass loop — pass 0 unions,
// pass 1 is the growPass re-solve), then a DRAW block in render order
// (reverse honored) using absolute coordinates. Visibility gates draws
// only; occupancy follows growinvis/stickytoinvis.
//
// Degradation is loud, never fatal: unsupported constructs warn and
// leave SKIP comments; the module still renders (the functional bar).
// RectLog is the rect gate's hook: when non-null the solve block
// reports every widget's absolute rect with tag/name for diffing
// against a reference solver's dump.
#include "uic/uic.hpp"

#include <cctype>
#include <functional>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace uic {

namespace {

uint32_t contentHash(std::string_view s) {
  uint32_t h = 2166136261u;
  for (char raw : s) {
    char c = raw;
    if (c == '\\') {
      c = '/';
    } else if (c >= 'A' && c <= 'Z') {
      c = (char)(c + ('a' - 'A'));
    }
    h ^= (uint8_t)c;
    h *= 16777619u;
  }
  return h == 0 ? 1u : h;
}

// an inline-enum id: raw FNV-1a, no case-fold — it must equal sid("value")
// byte for byte, since the schema default, the host, and the bind harness
// all id a value that way. (contentHash lowercases for asset paths; ids
// must not.)
uint32_t enumSid(std::string_view s) {
  uint32_t h = 2166136261u;
  for (const unsigned char c : s) {
    h ^= c;
    h *= 16777619u;
  }
  return h;
}

struct Dim {
  enum Kind { kPx, kPercent, kOther, kScreenH, kScreenW, kBad };
  Kind kind = kPx;
  float value = 0;
  bool delta = false; // leading '+' or negative: parent + value (sizes)
};

Dim parseDim(const std::string &s) {
  Dim d;
  if (s.empty()) {
    d.kind = Dim::kBad;
    return d;
  }
  const char *p = s.c_str();
  char *end = nullptr;
  d.value = std::strtof(p, &end);
  if (end == p) {
    d.kind = Dim::kBad;
    return d;
  }
  d.delta = s[0] == '+' || d.value < 0;
  if (*end == '\0') {
    d.kind = Dim::kPx;
  } else if (std::string_view(end) == "%") {
    d.kind = Dim::kPercent;
  } else if (std::string_view(end) == "@") {
    d.kind = Dim::kOther;
  } else if (std::string_view(end) == "h") {
    d.kind = Dim::kScreenH;
  } else if (std::string_view(end) == "w") {
    d.kind = Dim::kScreenW;
  } else {
    d.kind = Dim::kBad;
  }
  return d;
}

// True when a size value is a lone dim in the classic grammar (optional
// sign, number, optional %/@/h/w unit) or an empty/degenerate substitution
// — anything that is NOT a multi-term arithmetic or reference expression.
// parseDim (with the delta law) handles the former; sizeCppExpr the latter.
bool isLoneDim(std::string_view v) {
  size_t i = 0;
  const size_t n = v.size();
  auto spaces = [&]() {
    while (i < n && v[i] == ' ') {
      ++i;
    }
  };
  spaces();
  if (i == n) {
    return true; // empty / all-whitespace: a degenerate dim, warned as before
  }
  // A leading reference (width/height/parent...) or a parenthesis is an
  // expression, never a lone dim.
  if (std::isalpha((unsigned char)v[i]) != 0 || v[i] == '(') {
    return false;
  }
  // Otherwise a number leads: optional sign, digits, optional %/@/h/w unit.
  if (v[i] == '+' || v[i] == '-') {
    ++i;
  }
  while (i < n && (std::isdigit((unsigned char)v[i]) != 0 || v[i] == '.')) {
    ++i;
  }
  while (i < n && (v[i] == '%' || v[i] == '@' || v[i] == 'h' || v[i] == 'w')) {
    ++i;
  }
  spaces();
  // A binary operator or a parenthesis AFTER the dim means an expression;
  // trailing junk (a bad unit like '-2p') stays a lone dim — parseDim warns
  // on it exactly as it always did, rather than failing the emit.
  if (i < n && (v[i] == '+' || v[i] == '-' || v[i] == '*' || v[i] == '/' ||
                v[i] == '(')) {
    return false;
  }
  return true;
}

std::string ftos(float v) {
  char buf[48];
  std::snprintf(buf, sizeof buf, "%.9g", (double)v);
  std::string s = buf;
  if (s.find('.') == std::string::npos &&
      s.find('e') == std::string::npos) {
    s += ".0";
  }
  return s + "f";
}

bool parseColor(const std::string &s, float out[4]) {
  out[0] = out[1] = out[2] = 1;
  out[3] = 1;
  if (s.empty()) {
    return false;
  }
  if (s[0] == '#') {
    if (s.size() != 7 && s.size() != 9) {
      return false;
    }
    auto hex = [&](size_t i) {
      return (float)std::strtol(s.substr(i, 2).c_str(), nullptr, 16) / 255.f;
    };
    out[0] = hex(1);
    out[1] = hex(3);
    out[2] = hex(5);
    out[3] = s.size() == 9 ? hex(7) : 1.f;
    return true;
  }
  // rgb(r, g, b) / rgba(r, g, b, a): the parenthesized literal. The channels
  // are parsed like the bare-float form (commas are just separators), so a
  // stray count (rgb with 4, rgba with 3) is rejected.
  if (s.compare(0, 4, "rgb(") == 0 || s.compare(0, 5, "rgba(") == 0) {
    const bool hasA = s[3] == 'a';
    const size_t open = s.find('(');
    const size_t close = s.rfind(')');
    if (close == std::string::npos || close <= open) {
      return false;
    }
    std::string inner = s.substr(open + 1, close - open - 1);
    for (char &c : inner) {
      if (c == ',') {
        c = ' ';
      }
    }
    const char *p = inner.c_str();
    char *end = nullptr;
    float v[4] = {0, 0, 0, 1};
    int got = 0;
    while (got < 4) {
      const float f = std::strtof(p, &end);
      if (end == p) {
        break;
      }
      v[got++] = f;
      p = end;
    }
    if (got != (hasA ? 4 : 3)) {
      return false;
    }
    out[0] = v[0];
    out[1] = v[1];
    out[2] = v[2];
    out[3] = hasA ? v[3] : 1.f;
    return true;
  }
  static const struct {
    const char *name;
    float r, g, b, a;
  } kNamed[] = {
      {"white", 1, 1, 1, 1},       {"black", 0, 0, 0, 1},
      {"gray", .5f, .5f, .5f, 1},  {"grey", .5f, .5f, .5f, 1},
      {"silver", .75f, .75f, .75f, 1},
      {"red", 1, 0, 0, 1},         {"green", 0, .5f, 0, 1},
      {"lime", 0, 1, 0, 1},        {"blue", 0, 0, 1, 1},
      {"navy", 0, 0, .5f, 1},      {"teal", 0, .5f, .5f, 1},
      {"aqua", 0, 1, 1, 1},        {"cyan", 0, 1, 1, 1},
      {"yellow", 1, 1, 0, 1},      {"orange", 1, .647f, 0, 1},
      {"brown", .647f, .165f, .165f, 1},
      {"olive", .5f, .5f, 0, 1},   {"purple", .5f, 0, .5f, 1},
      {"maroon", .5f, 0, 0, 1},    {"fuchsia", 1, 0, 1, 1},
      {"magenta", 1, 0, 1, 1},     {"invisible", 0, 0, 0, 0},
  };
  for (const auto &n : kNamed) {
    if (s == n.name) {
      out[0] = n.r;
      out[1] = n.g;
      out[2] = n.b;
      out[3] = n.a;
      return true;
    }
  }
  // no bare "r g b [a]" form: colours are rgb()/rgba(), #rrggbb[aa], or named.
  return false;
}

// One solved widget INSTANCE — template bodies are shared AST nodes,
// so identity lives here, not on Node*: each instantiation yields its
// own SolvedW with its own variable index and substituted bag.
struct SolvedW {
  int idx = -1;
  const Node *node = nullptr;
  std::map<std::string, std::string> attrs; // effective, substituted
  // bind target -> transpiled C++ (captured at SOLVE time, while the
  // instantiation's param environment is still on the stack)
  std::map<std::string, std::string> binds;
  // structural-if arm guard (transpiled C++): when false this widget
  // does not EXIST — no union growth, no cursor advance, no draws.
  // Absence, not invisibility: gates regardless of growinvis and
  // stickytoinvis (unlike a bound visible).
  std::string guard;
  std::vector<SolvedW *> kids;
  bool reverse = false;
  // a widgetstate layer: excluded from unions and float chains
  bool isState = false;
};

// the enclosing float chain, threaded to children: axis, the parent's
// variable suffix (target rect tx/ty/tw/th + hasT live on the parent),
// and the parent's padding expression
struct ChainCtx {
  char axis = 0;
  std::string sfx;
  std::string spacing;
};

struct Emit {
  Emit(const Module &mod, const EmitOptions &options,
       std::vector<Diag> &out)
      : m(mod), opt(options), diags(out) {}

  const Module &m;
  const EmitOptions &opt;
  std::vector<Diag> &diags;
  std::ostringstream decl, solve, draw;
  std::map<std::string, uint32_t> texIds;
  std::vector<std::string> texOrder;
  std::map<std::string, uint32_t> fontIds; // name -> id (content hash)
  std::vector<std::string> fontOrder;
  std::map<std::string, std::string> assetConsts;
  std::map<std::string, const StyleDecl *> styles;
  std::map<std::string, const TemplateDecl *> templates;
  std::set<std::string> enumNames; // schema enums -> scope-resolved refs
  // inline-enum id constants THIS file references (UPPER -> value). Emitted
  // per file as `inline constexpr uint32_t NAME = 0x..u;` — a param-only
  // enum has no schema home, and `inline` makes identical defs across
  // translation units agree instead of collide.
  std::map<std::string, std::string> enumIds;
  std::deque<SolvedW> arena;
  std::vector<std::map<std::string, std::string>> envStack;
  std::vector<const TemplateDecl *> declStack; // enclosing param types
  // the resolved width/height VARS of the current ancestor chain (a widget's
  // parent is the top), so a size can name an ancestor's dimension:
  // parent.width / parent.parent.height — the ancestor-dimension idiom
  std::vector<std::string> wStack, hStack;
  int instanceDepth = 0;
  int nextVar = 0;
  int solveIndent = 1, drawIndent = 1;

  void err(int line, std::string msg) {
    diags.push_back({m.name, line, 0, std::move(msg),
                     Diag::Severity::kError});
  }
  void skip(int atLine, const std::string &what) {
    diags.push_back({m.name, atLine, 0,
                     "skipped (unsupported yet): " + what,
                     Diag::Severity::kWarning});
    drawLine("// SKIP(unsupported): " + what + " :" +
             std::to_string(atLine));
  }
  void solveLine(const std::string &s) {
    solve << std::string((size_t)solveIndent * 2, ' ') << s << '\n';
  }
  void drawLine(const std::string &s) {
    draw << std::string((size_t)drawIndent * 2, ' ') << s << '\n';
  }

  // does the host's tree carry this art? (the archive law: a source
  // .tga may ship as .dds)
  bool assetExists(const std::string &path) const {
    if (opt.assetRoot.empty() || path.empty() || path[0] == '$') {
      return true; // nothing to check against
    }
    namespace fs = std::filesystem;
    fs::path full =
        fs::path(opt.assetRoot) / (path[0] == '/' ? path.substr(1) : path);
    std::error_code ec;
    if (fs::exists(full, ec)) {
      return true;
    }
    if (full.extension() == ".tga") {
      full.replace_extension(".dds");
      return fs::exists(full, ec);
    }
    return false;
  }

  uint32_t texId(const std::string &path, int atLine) {
    if (!assetExists(path)) {
      err(atLine, "missing asset: " + path);
    }
    auto it = texIds.find(path);
    if (it != texIds.end()) {
      return it->second;
    }
    const uint32_t id = contentHash(path);
    texIds.emplace(path, id);
    texOrder.push_back(path);
    return id;
  }
  std::string texIdExpr(const std::string &path, int atLine) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08Xu", texId(path, atLine));
    return std::string(buf) + " /* " + path + " */";
  }

  // fonts are host resources exactly like textures: the module names
  // them, the manifest hands the host {id, name}, and the host
  // registers whatever face/size that name means to it
  std::string fontIdExpr(const std::string &name) {
    auto it = fontIds.find(name);
    if (it == fontIds.end()) {
      it = fontIds.emplace(name, contentHash(name)).first;
      fontOrder.push_back(name);
    }
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08Xu", it->second);
    return std::string(buf) + " /* " + name + " */";
  }

  // ---- expressions (binds) ----------------------------------------------

  // the inline-enum value set of an atom that names an inline-enum param
  // of an enclosing instantiation (a schema field's set needs walking the
  // struct decls — not resolved here yet, so a field returns null)
  const std::vector<std::string> *inlineEnumSet(const Expr &e) const {
    if (e.kind != Expr::kIdent) {
      return nullptr;
    }
    for (auto it = declStack.rbegin(); it != declStack.rend(); ++it) {
      for (const InParam &p : (*it)->ins) {
        if (p.name == e.text && p.isEnum()) {
          return &p.enumValues;
        }
      }
    }
    return nullptr;
  }

  // the enclosing-instantiation param an atom names, if any (for its type)
  const InParam *lookupParam(const std::string &name) const {
    for (auto it = declStack.rbegin(); it != declStack.rend(); ++it) {
      for (const InParam &p : (*it)->ins) {
        if (p.name == name) {
          return &p;
        }
      }
    }
    return nullptr;
  }

  // a colour value (#rrggbb, "r g b[ a]", or a named colour) as a ui::Color
  // ctor — the form a bound colour needs (the static attr path uses the
  // same parseColor but keeps its per-callsite default)
  std::string colorFrom(const std::string &val, int line) {
    float c[4] = {1, 1, 1, 1};
    if (!parseColor(val, c)) {
      err(line, "unparsable color '" + val + "'");
    }
    return "ui::Color{" + ftos(c[0]) + ", " + ftos(c[1]) + ", " + ftos(c[2]) +
           ", " + ftos(c[3]) + "}";
  }

  static std::string upper(const std::string &s) {
    std::string u = s;
    for (char &c : u) {
      c = static_cast<char>(std::toupper((unsigned char)c));
    }
    return u;
  }

  // the named id constant for an inline-enum value: its UPPER, recorded so
  // the file emits `inline constexpr uint32_t NAME = 0x..u;` for it. A value
  // whose UPPER already stands for a DIFFERENT value collides (as it would
  // in the schema).
  std::string enumConst(const std::string &value) {
    std::string up = upper(value);
    const auto ins = enumIds.emplace(up, value);
    if (!ins.second && ins.first->second != value) {
      err(0, "enum values '" + ins.first->second + "' and '" + value +
                 "' share the constant '" + up + "'");
    }
    return up;
  }

  // one operand of an inline-enum comparison: a bare VALUE lowers to its
  // named id constant (its UPPER, matched case-insensitively so a consumer's
  // SOLID and hand .ui's solid both land there); the param and a
  // uint32_t field defer to expr() (the param folds to the constant there,
  // the field already IS a sid)
  std::string exprEnum(const Expr &e, const std::vector<std::string> *set) {
    if (e.kind == Expr::kIdent && e.text != "snapshot" &&
        inlineEnumSet(e) == nullptr && substitute(e.text) == e.text) {
      for (const std::string &v : *set) {
        if (upper(v) == upper(e.text)) {
          return enumConst(v);
        }
      }
      err(e.line, "'" + e.text + "' is not a value of this enum");
      return "0";
    }
    return expr(e);
  }

  std::string expr(const Expr &e) {
    switch (e.kind) {
    case Expr::kIdent: {
      if (e.text == "snapshot") {
        return "s";
      }
      // an inline-enum param folds to its value's named id constant
      if (inlineEnumSet(e) != nullptr) {
        return enumConst(substitute(e.text));
      }
      // a colour-typed param folds to its instance value as a ui::Color
      // (so a bound colour can read the widget's own `color` param)
      if (const InParam *p = lookupParam(e.text);
          p != nullptr && p->type == "color") {
        return colorFrom(substitute(e.text), e.line);
      }
      // a template parameter: the folded argument substitutes (typed
      // params — the value must itself be a valid expression)
      const std::string sub = substitute(e.text);
      if (sub != e.text) {
        std::string out = sub;
        const size_t at = out.find("snapshot");
        if (at != std::string::npos) {
          out.replace(at, 8, "s");
        }
        return "(" + out + ")";
      }
      err(e.line, "unknown identifier '" + e.text + "' in bind");
      return "0";
    }
    case Expr::kNumber:
    case Expr::kBool:
      return e.text;
    case Expr::kPath:
      return texIdExpr(e.text, e.line);
    case Expr::kColor:
      return colorFrom(e.text, e.line);
    case Expr::kString:
    case Expr::kDim:
      err(e.line, "literal kind not yet allowed in binds");
      return "0";
    case Expr::kField:
      // an enum reference (Variant.Triple) scope-resolves against the
      // schema module's declared enums
      if (e.args[0]->kind == Expr::kIdent &&
          enumNames.count(e.args[0]->text) != 0) {
        return e.args[0]->text + "::" + e.text;
      }
      return expr(*e.args[0]) + "." + e.text;
    case Expr::kIndex:
      return expr(*e.args[0]) + "[" + expr(*e.args[1]) + "]";
    case Expr::kCall:
      if (e.args[0]->kind == Expr::kIdent && e.args[0]->text == "fracf") {
        if (e.args.size() != 3) {
          err(e.line, "fracf takes (num, den)");
          return "0";
        }
        return "gen_detail::fracf((float)(" + expr(*e.args[1]) +
               "), (float)(" + expr(*e.args[2]) + "))";
      }
      // the typed conversions at a text sink (no interpolation in the
      // language — these are calls, and their result IS a run)
      if (e.args[0]->kind == Expr::kIdent && e.args[0]->text == "num") {
        if (e.args.size() != 2) {
          err(e.line, "num takes (value)");
          return "0";
        }
        return "gen_detail::num((long long)(" + expr(*e.args[1]) + "))";
      }
      // sid("value") — a string id, the schema's constexpr. Passed
      // through verbatim so the same call yields the same uint32_t in a
      // bind as it does populating the snapshot (a patch writes the
      // explicit form; hand .ui can also just write the bare enum value)
      if (e.args[0]->kind == Expr::kIdent && e.args[0]->text == "sid") {
        if (e.args.size() != 2 || e.args[1]->kind != Expr::kString) {
          err(e.line, "sid takes (\"value\")");
          return "0";
        }
        return "sid(" + quotedLit(e.args[1]->text) + ")";
      }
      // an enum's ordinal: the one bridge between a typed schema enum
      // and the plain numbers a folded template param carries
      if (e.args[0]->kind == Expr::kIdent && e.args[0]->text == "ord") {
        if (e.args.size() != 2) {
          err(e.line, "ord takes (value)");
          return "0";
        }
        return "(long long)(" + expr(*e.args[1]) + ")";
      }
      // rgb(r, g, b) / rgba(r, g, b, a): a colour built from expressions —
      // the one way to spell a colour inside a bind (a bare "r g b a" is not
      // one expression, and #rrggbb can't carry a computed channel). rgb is
      // the opaque short form (alpha 1).
      if (e.args[0]->kind == Expr::kIdent &&
          (e.args[0]->text == "rgb" || e.args[0]->text == "rgba")) {
        const bool hasA = e.args[0]->text == "rgba";
        if (e.args.size() != (hasA ? 5u : 4u)) {
          err(e.line, e.args[0]->text +
                          (hasA ? " takes (r, g, b, a)" : " takes (r, g, b)"));
          return "0";
        }
        const std::string a = hasA ? expr(*e.args[4]) : std::string("1");
        return "ui::Color{(float)(" + expr(*e.args[1]) + "), (float)(" +
               expr(*e.args[2]) + "), (float)(" + expr(*e.args[3]) +
               "), (float)(" + a + ")}";
      }
      if (e.args[0]->kind == Expr::kIdent && e.args[0]->text == "fixed") {
        if (e.args.size() != 3) {
          err(e.line, "fixed takes (value, decimals)");
          return "0";
        }
        return "gen_detail::fixed((double)(" + expr(*e.args[1]) + "), (int)(" +
               expr(*e.args[2]) + "))";
      }
      // fmt("{} / {}", a, b): a format literal, then one arg per {}
      if (e.args[0]->kind == Expr::kIdent && e.args[0]->text == "fmt") {
        if (e.args.size() < 2 || e.args[1]->kind != Expr::kString) {
          err(e.line, "fmt takes (\"literal with {}\", args...)");
          return "0";
        }
        std::string out = "gen_detail::fmt(" + quotedLit(e.args[1]->text);
        for (size_t i = 2; i < e.args.size(); ++i) {
          out += ", (" + expr(*e.args[i]) + ")";
        }
        return out + ")";
      }
      err(e.line,
          "call in bind (subset builtins: fracf, num, fixed, ord, fmt)");
      return "0";
    case Expr::kUnary:
      return "(" + e.text + expr(*e.args[0]) + ")";
    case Expr::kBinary:
      // an inline-enum comparison lowers both sides to sids; a bare value
      // is validated against the set, and comparing two DIFFERENT sets is
      // a type error even though both are uint32_t at runtime
      if (e.text == "==" || e.text == "!=") {
        const std::vector<std::string> *ls = inlineEnumSet(*e.args[0]);
        const std::vector<std::string> *rs = inlineEnumSet(*e.args[1]);
        if (ls != nullptr || rs != nullptr) {
          if (ls != nullptr && rs != nullptr && *ls != *rs) {
            err(e.line, "'" + e.text + "' between two different enum sets");
          }
          const std::vector<std::string> *set = ls != nullptr ? ls : rs;
          return "(" + exprEnum(*e.args[0], set) + " " + e.text + " " +
                 exprEnum(*e.args[1], set) + ")";
        }
      }
      return "(" + expr(*e.args[0]) + " " + e.text + " " +
             expr(*e.args[1]) + ")";
    case Expr::kTernary:
      return "(" + expr(*e.args[0]) + " ? " + expr(*e.args[1]) + " : " +
             expr(*e.args[2]) + ")";
    case Expr::kMatch: {
      // [conv](scrutinee == V0 ? r0 : ... : rLast) — last arm is the else.
      // An inline-enum scrutinee folds to its named id and its values are
      // members, validated; any other scrutinee (a number, a field) is a
      // plain expression and its values are literals compared directly.
      const std::vector<std::string> *set = inlineEnumSet(*e.args[0]);
      const size_t n = e.cases.size(); // arms; args = 1 (scrutinee) + n
      if (n < 2 || e.args.size() != n + 1) {
        err(e.line, "match needs at least two arms");
        return "0";
      }
      const std::string sc =
          set != nullptr ? exprEnum(*e.args[0], set) : expr(*e.args[0]);
      std::string out = expr(*e.args[n]); // last arm = else
      for (int i = static_cast<int>(n) - 2; i >= 0; --i) {
        std::string vv;
        if (set != nullptr) {
          Expr val;
          val.kind = Expr::kIdent;
          val.text = e.cases[i];
          val.line = e.line;
          vv = exprEnum(val, set); // validates + lowers the enum member
        } else {
          vv = e.cases[i]; // a literal (number), compared as written
        }
        out = "((" + sc + " == " + vv + ") ? " + expr(*e.args[i + 1]) +
              " : " + out + ")";
      }
      if (e.text.empty()) {
        return out; // no conversion: the arms are the value
      }
      if (e.text == "num") {
        return "gen_detail::num((long long)(" + out + "))";
      }
      if (e.text == "ord") {
        return "(long long)(" + out + ")";
      }
      err(e.line, "match conversion '" + e.text + "' (only num/ord)");
      return "0";
    }
    }
    return "0";
  }

  // dim -> C++ expression against the parent's CURRENT size variables
  std::string dimExpr(const Dim &d, bool horizontal, const std::string &pw,
                      const std::string &ph, bool isSize) {
    std::string raw;
    switch (d.kind) {
    case Dim::kPx:
      raw = ftos(d.value);
      break;
    case Dim::kPercent:
      raw = ftos(d.value / 100.f) + " * " + (horizontal ? pw : ph);
      break;
    case Dim::kOther:
      raw = ftos(d.value / 100.f) + " * " + (horizontal ? ph : pw);
      break;
    case Dim::kScreenH:
      raw = ftos(d.value / 100.f) + " * H";
      break;
    case Dim::kScreenW:
      raw = ftos(d.value / 100.f) + " * W";
      break;
    case Dim::kBad:
      raw = "0.0f";
      break;
    }
    if (isSize && d.delta) {
      return (horizontal ? pw : ph) + " + (" + raw + ")";
    }
    return raw;
  }

  // A size VALUE parsed as an ARITHMETIC EXPRESSION (+ - * /, parens) over
  // dims (7h, 66%, 42), the widget's OWN dimensions (width, height), and its
  // ANCESTORS' (parent.width, parent.parent.height, ...). Returns the
  // unrounded float C++ expression. Sets readsW/readsH when the expression
  // reads the widget's own width/height — that dimension must be solved first.
  // A reference the tree has no ancestor for, or an unknown token, is a
  // diagnostic (never silently zero).
  std::string sizeCppExpr(std::string_view s, bool horizontal,
                          const std::string &vw, const std::string &vh,
                          const std::string &pw, const std::string &ph,
                          int line, bool &readsW, bool &readsH) {
    size_t p = 0;
    auto skip = [&]() {
      while (p < s.size() && s[p] == ' ') {
        ++p;
      }
    };
    std::function<std::string()> expr, term, factor;
    factor = [&]() -> std::string {
      skip();
      if (p < s.size() && s[p] == '(') {
        ++p;
        std::string inner = expr();
        skip();
        if (p < s.size() && s[p] == ')') {
          ++p;
        } else {
          err(line, "size expression: unbalanced '('");
        }
        return "(" + inner + ")";
      }
      // a dimension reference: (parent.)* (width|height)
      if (p < s.size() && std::isalpha((unsigned char)s[p]) != 0) {
        const size_t start = p;
        while (p < s.size() &&
               (std::isalnum((unsigned char)s[p]) != 0 || s[p] == '.')) {
          ++p;
        }
        const std::string full(s.substr(start, p - start));
        std::string_view tok(full);
        int levels = 0;
        while (tok.substr(0, 7) == "parent.") {
          tok.remove_prefix(7);
          ++levels;
        }
        const bool isW = tok == "width";
        if (!isW && tok != "height") {
          err(line, "size expression: unknown reference '" + full + "'");
          return "0.0f";
        }
        if (levels == 0) {
          if (isW) {
            readsW = true;
          } else {
            readsH = true;
          }
          return "(float)" + (isW ? vw : vh);
        }
        if (levels == 1) {
          return "(float)" + (isW ? pw : ph);
        }
        const std::vector<std::string> &stk = isW ? wStack : hStack;
        if (stk.size() < (size_t)levels) {
          err(line, "size expression: '" + full + "' has no such ancestor");
          return "0.0f";
        }
        return "(float)" + stk[stk.size() - (size_t)levels];
      }
      // a dim literal: optional sign, number, optional unit
      const size_t start = p;
      if (p < s.size() && (s[p] == '+' || s[p] == '-')) {
        ++p;
      }
      while (p < s.size() &&
             (std::isdigit((unsigned char)s[p]) != 0 || s[p] == '.')) {
        ++p;
      }
      while (p < s.size() &&
             (std::isalpha((unsigned char)s[p]) != 0 || s[p] == '%')) {
        ++p;
      }
      const std::string lit(s.substr(start, p - start));
      const Dim d = parseDim(lit);
      if (d.kind == Dim::kBad) {
        err(line, "size expression: bad dim '" + lit + "'");
      }
      return dimExpr(d, horizontal, pw, ph, false);
    };
    term = [&]() -> std::string {
      std::string a = factor();
      skip();
      while (p < s.size() && (s[p] == '*' || s[p] == '/')) {
        const char op = s[p++];
        a = "(" + a + " " + op + " " + factor() + ")";
        skip();
      }
      return a;
    };
    expr = [&]() -> std::string {
      std::string a = term();
      skip();
      while (p < s.size() && (s[p] == '+' || s[p] == '-')) {
        const char op = s[p++];
        a = "(" + a + " " + op + " " + term() + ")";
        skip();
      }
      return a;
    };
    std::string result = expr();
    skip();
    if (p != s.size()) {
      err(line, "size expression: trailing '" + std::string(s.substr(p)) + "'");
    }
    return result;
  }

  // ---- the effective bag (style merge, widget wins; param subst) ---------

  // a value that exactly names a template parameter substitutes to the
  // instantiation's argument (typed params fold — no splicing)
  std::string substitute(const std::string &v) const {
    for (auto it = envStack.rbegin(); it != envStack.rend(); ++it) {
      auto hit = it->find(v);
      if (hit != it->end()) {
        return hit->second;
      }
    }
    return v;
  }

  // a comma-separated style list, each name trimmed
  static std::vector<std::string> styleNames(const std::string &s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= s.size()) {
      const size_t comma = std::min(s.find(',', i), s.size());
      std::string name = s.substr(i, comma - i);
      while (!name.empty() && name.front() == ' ') {
        name.erase(name.begin());
      }
      while (!name.empty() && name.back() == ' ') {
        name.pop_back();
      }
      if (!name.empty()) {
        out.push_back(name);
      }
      if (comma == s.size()) {
        break;
      }
      i = comma + 1;
    }
    return out;
  }

  // merge one style's attrs into `bag` (first-writer-wins), then resolve
  // its OWN `style:` base(s) AFTER — so a derived style overrides its base
  // (its own attrs win; the base's other attrs flow through). `seen` guards
  // cycles and diamonds.
  void mergeStyle(std::map<std::string, std::string> &bag,
                  const std::string &name, std::set<std::string> &seen,
                  int line) {
    if (!seen.insert(name).second) {
      return;
    }
    auto it = styles.find(name);
    if (it == styles.end()) {
      diags.push_back({m.name, line, 0, "unknown style '" + name + "'",
                       Diag::Severity::kWarning});
      return;
    }
    std::string base;
    for (const Attr &a : it->second->attrs) {
      if (a.name == "style") {
        base = a.value; // resolved after this style's own attrs
        continue;
      }
      bag.emplace(a.name, a.value);
    }
    for (const std::string &b : styleNames(base)) {
      mergeStyle(bag, b, seen, line);
    }
  }


  // Resolve `attr: match p { v: path; ... }` — the language's answer to
  // interpolated values. The scrutinee's folded value picks the one arm
  // this instantiation draws. When the attr names an asset, every arm's
  // path is statically validated (a missing file is a hard error);
  // a match on any other attr (e.g. `style`) resolves to
  // a name whose existence its own domain checks.
  std::string resolveMatch(const Attr &a) {
    if (a.name == "texture" || a.name == "alphamaskfile") {
      for (const MatchArm &arm : a.arms) {
        if (!assetExists(arm.result)) {
          err(a.line, "match '" + a.name + "' arm '" + arm.value +
                          "': missing asset " + arm.result);
        }
      }
    }
    const std::string picked = substitute(a.matchOn);
    if (picked == a.matchOn) {
      err(a.line, "match scrutinee '" + a.matchOn +
                      "' does not fold to a value here");
      return "";
    }
    for (const MatchArm &arm : a.arms) {
      if (arm.value == picked) {
        return arm.result;
      }
    }
    err(a.line, "match '" + a.name + "' has no arm for '" + picked + "'");
    return "";
  }

  std::map<std::string, std::string> bagOf(const Node &n) {
    std::map<std::string, std::string> bag;
    for (const Attr &a : n.attrs) {
      if (a.isMatch()) {
        bag.emplace(a.name, resolveMatch(a));
        continue;
      }
      bag.emplace(a.name, a.value);
    }
    auto styleIt = bag.find("style");
    if (styleIt != bag.end()) {
      // left-to-right, widget wins (first writer wins). The NAME may be a
      // template param (a caller choosing this instance's look), so it
      // folds before the lookup. Each style resolves its own `style:` base
      // chain (mergeStyle), so inherited attrs flow through.
      const std::string s = substitute(styleIt->second);
      std::set<std::string> seen;
      for (const std::string &name : styleNames(s)) {
        mergeStyle(bag, name, seen, n.line);
      }
    }
    if (!envStack.empty()) {
      for (auto &kv : bag) {
        kv.second = substitute(kv.second);
      }
    }
    // an interpolation hole must NEVER reach the compiler: a converter's
    // asset-match pass turns every {param} asset into a match over an
    // enum-typed param, statically validated. A surviving hole is a
    // hard error (the pass missed one).
    for (auto &kv : bag) {
      if ((kv.first == "texture" || kv.first == "alphamaskfile") &&
          kv.second.find('{') != std::string::npos) {
        err(n.line, "interpolation hole in asset '" + kv.second +
                        "' — must be a match over an enum param");
      }
    }
    return bag;
  }

  static const std::string *get(
      const std::map<std::string, std::string> &bag, std::string_view key) {
    auto it = bag.find(std::string(key));
    return it == bag.end() ? nullptr : &it->second;
  }
  static bool flag(const std::map<std::string, std::string> &bag,
                   std::string_view key, bool def) {
    const std::string *v = get(bag, key);
    // dialect truthiness ("0" is false) plus the typed spelling
    return v == nullptr ? def : (*v != "0" && *v != "false");
  }

  // ---- arg typing (the .ui is fully type-checked) -----------------------

  // the syntactic type of an instantiation arg: a literal's form, or
  // the declared type of the outer param it references
  std::string valueType(const std::string &v) const {
    if (v.empty()) {
      return "";
    }
    if (v == "true" || v == "false") {
      return "bool";
    }
    char *end = nullptr;
    std::strtod(v.c_str(), &end);
    if (end != v.c_str() && end != nullptr) {
      if (*end == '\0') {
        return "num";
      }
      // the unit grammar: a number wearing a single unit suffix is a dim
      const std::string_view sfx(end);
      if (sfx == "h" || sfx == "w" || sfx == "%" || sfx == "@" ||
          sfx == "s" || sfx == "i" || sfx == "a") {
        return "dim";
      }
    }
    if (v.front() == '"') {
      return "str";
    }
    if (v.front() == '/') {
      return "asset";
    }
    if (v.front() == '#') {
      return "color"; // #rrggbb[aa]
    }
    {
      // 3-4 space-separated floats = a color literal
      int tokens = 0;
      size_t i = 0;
      bool numeric = true;
      while (i < v.size() && numeric) {
        while (i < v.size() && v[i] == ' ') {
          ++i;
        }
        if (i >= v.size()) {
          break;
        }
        size_t j = v.find(' ', i);
        if (j == std::string::npos) {
          j = v.size();
        }
        const std::string tok = v.substr(i, j - i);
        char *tokEnd = nullptr;
        std::strtod(tok.c_str(), &tokEnd);
        numeric = tokEnd != tok.c_str() && *tokEnd == '\0';
        ++tokens;
        i = j;
      }
      if (numeric && tokens >= 3 && tokens <= 4) {
        return "color";
      }
    }
    for (auto it = declStack.rbegin(); it != declStack.rend(); ++it) {
      for (const InParam &p : (*it)->ins) {
        if (p.name == v) {
          return p.type;
        }
      }
    }
    return "word"; // a bare symbol
  }

  static bool typeCompatible(const std::string &declared,
                             const std::string &cls) {
    if (cls.empty()) {
      return true; // nothing to judge
    }
    if (declared == cls) {
      return true;
    }
    if (declared == "dim") {
      return cls == "num"; // a bare number is a px dim
    }
    if (declared == "ident") {
      return cls == "word"; // a bare symbol IS an identifier
    }
    if (declared == "color") {
      return cls == "word"; // named colors; parseColor judges the name
    }
    // only-text-is-quoted: word-like strings pass unquoted
    return declared == "str" && (cls == "word" || cls == "str");
  }

  const Bind *bind(const Node &n, std::string_view key) {
    for (const Bind &b : n.binds) {
      if (b.target == key) {
        return &b;
      }
    }
    return nullptr;
  }

  // ---- the solve schedule ---------------------------------------------------

  // Solve one AST node into zero or more widget INSTANCES appended to
  // `out`: a builtin widget yields one SolvedW; a template invocation
  // is transparent — its body's roots join the parent's child list
  // (each participating in the chain/union individually).
  void solveInto(std::vector<SolvedW *> &out, const Node &n,
                 const std::string &pw, const std::string &ph,
                 const ChainCtx &chain) {
    if (n.kind == Node::kIf) {
      // structural if — per-arm occupancy gating: arm N lowers to the
      // guard cond_N && !cond_0 && ... && !cond_(N-1) (the else arm =
      // all conditions negated), attached to every widget instance the
      // arm's body solves into. Conditions transpile NOW — the
      // instantiation's param environment is still on the stack.
      std::string notPrior;
      for (const IfArm &arm : n.arms) {
        const std::string cond =
            arm.cond != nullptr ? expr(*arm.cond) : std::string();
        std::string g;
        if (!cond.empty() && !notPrior.empty()) {
          g = "(" + cond + ") && " + notPrior;
        } else if (!cond.empty()) {
          g = cond;
        } else {
          g = notPrior; // the else arm
        }
        for (const NodePtr &b : arm.body) {
          const size_t before = out.size();
          solveInto(out, *b, pw, ph, chain);
          for (size_t k = before; k < out.size(); ++k) {
            SolvedW &cw = *out[k];
            cw.guard = cw.guard.empty()
                           ? g
                           : "(" + g + ") && (" + cw.guard + ")";
          }
        }
        if (!cond.empty()) {
          const std::string neg = "!(" + cond + ")";
          notPrior = notPrior.empty() ? neg : notPrior + " && " + neg;
        }
      }
      return;
    }
    // a widgetstate is a state LAYER: only the resting state ('up')
    // renders in the static build; states are excluded from unions and
    // float chains
    const bool isState = n.kind == Node::kWidgetState;
    if (isState && n.tag != "up") {
      return; // the other states await interaction (silent by design)
    }
    if (!isState &&
        n.tag != "panel" && n.tag != "image" && n.tag != "frame" &&
        n.tag != "button" && n.tag != "label") {
      auto tpl = templates.find(n.tag);
      if (tpl == templates.end()) {
        skip(n.line, "widget '" + n.tag + "'");
        return;
      }
      if (instanceDepth > 32) {
        err(n.line, "template instantiation deeper than 32 ('" + n.tag +
                        "') — cycle?");
        return;
      }
      // ---- instantiate: params fold at compile time -------------------
      const TemplateDecl &t = *tpl->second;
      std::map<std::string, std::string> env;
      for (const InParam &p : t.ins) {
        if (p.hasDefault) {
          env[p.name] = p.defaultValue;
        }
      }
      // a `style` arg is resolved before the params are filled, not passed
      // through: the named style's props merge into the args, the instance's
      // own args winning (the same style expansion widgets get). So a
      // color-carrying style supplies the template's color param.
      std::vector<Attr> args;
      std::string styleList;
      for (const Attr &a : n.attrs) {
        if (a.name == "style") {
          styleList = substitute(a.value);
        } else {
          args.push_back(a);
        }
      }
      if (!styleList.empty()) {
        std::map<std::string, std::string> styleBag;
        std::set<std::string> seen;
        for (const std::string &name : styleNames(styleList)) {
          mergeStyle(styleBag, name, seen, n.line);
        }
        for (const auto &kv : styleBag) {
          bool present = kv.first == "style";
          for (const Attr &a : args) {
            present = present || a.name == kv.first;
          }
          if (!present) {
            Attr sa;
            sa.name = kv.first;
            sa.value = kv.second;
            sa.line = n.line;
            args.push_back(std::move(sa));
          }
        }
      }
      for (const Attr &a : args) {
        const InParam *param = nullptr;
        for (const InParam &p : t.ins) {
          if (p.name == a.name) {
            param = &p;
            break;
          }
        }
        if (param == nullptr) {
          diags.push_back({m.name, a.line, 0,
                           "'" + t.name + "' has no param '" + a.name +
                               "' (arg ignored)",
                           Diag::Severity::kWarning});
          continue;
        }
        // arg values may reference the OUTER instantiation's params; a
        // match arg PROJECTS a fused enum back to a component — foo {
        // tone: match fused_state { ... } } — folding the
        // scrutinee here and passing the picked arm on
        const std::string folded =
            a.isMatch() ? resolveMatch(a) : substitute(a.value);
        if (param->isEnum()) {
          // an enum param: the arg must fold to one of its values (a
          // literal, a forward, or a match projection that resolves to one)
          bool ok = false;
          for (const std::string &v : param->enumValues) {
            ok = ok || folded == v;
          }
          if (!ok) {
            std::string vals;
            for (const std::string &v : param->enumValues) {
              vals += (vals.empty() ? "" : " | ") + v;
            }
            err(a.line, "arg '" + a.name + "' of '" + t.name + "' is '" +
                            folded + "', not one of: " + vals);
          }
        } else if (!a.isMatch()) {
          // the args are TYPED: the value's form (or the referenced
          // outer param's declared type) must match the declaration
          const std::string cls = valueType(a.value);
          if (!typeCompatible(param->type, cls)) {
            err(a.line, "arg '" + a.name + "' of '" + t.name + "' is " +
                            param->type + ", got " +
                            (cls.empty() ? "nothing" : cls) + " '" +
                            a.value + "'");
          }
        }
        env[a.name] = folded;
      }
      // A `bind <param>: <expr>` on the invocation threads a snapshot
      // EXPRESSION into the instance. The expression is resolved HERE, in the
      // CALLER's env (envStack still has the caller on top — the instance's
      // env is not pushed yet), so it reads the caller's own params; the
      // resolved form becomes the param's value, and the body's uses of that
      // param — `bind visible: <param>` — carry it through expr()'s param
      // substitution. This is how a template shared across call sites gets a
      // per-call-site decision without one site's fact leaking into another.
      for (const Bind &b : n.binds) {
        const InParam *param = nullptr;
        for (const InParam &p : t.ins) {
          if (p.name == b.target) {
            param = &p;
            break;
          }
        }
        if (param == nullptr) {
          diags.push_back({m.name, n.line, 0,
                           "'" + t.name + "' has no param '" + b.target +
                               "' to bind",
                           Diag::Severity::kWarning});
          continue;
        }
        env[b.target] = expr(*b.expr);
      }
      for (const InParam &p : t.ins) {
        if (env.find(p.name) == env.end()) {
          diags.push_back({m.name, n.line, 0,
                           "'" + t.name + "' param '" + p.name +
                               "' has no argument and no default",
                           Diag::Severity::kWarning});
          env[p.name] = "";
        }
      }
      solveLine("// >>> " + t.name + " :" + std::to_string(n.line));
      envStack.push_back(std::move(env));
      declStack.push_back(&t);
      ++instanceDepth;
      for (const NodePtr &b : t.body) {
        solveInto(out, *b, pw, ph, chain);
      }
      --instanceDepth;
      declStack.pop_back();
      envStack.pop_back();
      solveLine("// <<< " + t.name);
      return;
    }

    SolvedW &sw = arena.emplace_back();
    sw.node = &n;
    sw.attrs = bagOf(n);
    sw.idx = nextVar++;
    sw.isState = isState;
    sw.reverse = flag(sw.attrs, "reverse", false);
    // transpile binds NOW — the instantiation's param environment is
    // gone by draw time
    for (const Bind &b : n.binds) {
      sw.binds[b.target] =
          b.target == "content" ? textBind(*b.expr) : expr(*b.expr);
    }
    out.push_back(&sw);

    const std::string sfx = std::to_string(sw.idx);
    const std::string vx = "x" + sfx, vy = "y" + sfx, vw = "w" + sfx,
                      vh = "h" + sfx;
    decl << "  float " << vx << " = 0, " << vy << " = 0, " << vw
         << " = 0, " << vh << " = 0;\n";

    const bool grows = flag(sw.attrs, "grow", false);
    solveLine("// " + n.tag +
              (get(sw.attrs, "name") != nullptr
                   ? " '" + *get(sw.attrs, "name") + "'"
                   : std::string()) +
              " :" + std::to_string(n.line));

    // ---- size: an explicit width/height/size is an ARITHMETIC EXPRESSION
    // over dims and self/ancestor dimensions (sizeCppExpr). A missing size is
    // 100% of the parent, or the union alone when growing (the floor law).
    bool wReadsW = false, wReadsH = false, hReadsW = false, hReadsH = false;
    auto sizeExpr = [&](const char *key, const char *sizeKey, bool horizontal,
                        bool &readsW, bool &readsH) -> std::string {
      const std::string *v = get(sw.attrs, key);
      if (v == nullptr) {
        v = get(sw.attrs, sizeKey);
      }
      if (v != nullptr) {
        // A lone dim (or an empty/degenerate substitution) keeps the classic
        // grammar: parseDim + the delta law, WARNING (never failing) on a
        // value it can't parse. Only a genuine arithmetic/reference
        // expression — anything parseDim can't spell — routes to sizeCppExpr.
        if (isLoneDim(*v)) {
          const Dim d = parseDim(*v);
          if (d.kind == Dim::kBad) {
            diags.push_back({m.name, n.line, 0,
                             std::string("unsupported dim '") + *v + "' for " +
                                 key,
                             Diag::Severity::kWarning});
          }
          return "R(" + dimExpr(d, horizontal, pw, ph, true) + ")";
        }
        return "R(" + sizeCppExpr(*v, horizontal, vw, vh, pw, ph, n.line,
                                  readsW, readsH) +
               ")";
      }
      if (grows) {
        return "0.0f"; // union alone (the floor law)
      }
      return horizontal ? pw : ph; // default 100% of the parent
    };
    std::string wE = sizeExpr("width", "size", true, wReadsW, wReadsH);
    std::string hE = sizeExpr("height", "size", false, hReadsW, hReadsH);
    if (auto b = sw.binds.find("width"); b != sw.binds.end()) {
      wE = "R(" + pw + " * " + b->second + ")"; // interim fraction law
    }
    if (auto b = sw.binds.find("height"); b != sw.binds.end()) {
      hE = "R(" + ph + " * " + b->second + ")";
    }
    // an expression reading the widget's OWN other dimension forces an order:
    // solve that dimension first so the expression can read its variable.
    if (wReadsH && hReadsW) {
      err(n.line, "size: width and height reference each other");
    }
    if (wReadsH) {
      solveLine(vh + " = " + hE + ";");
      solveLine(vw + " = " + wE + ";");
    } else {
      solveLine(vw + " = " + wE + ";");
      solveLine(vh + " = " + hE + ";");
    }
    // the fitx solve law: a fitx label
    // sizes to clamp(textWidth, fitxmin, fitxmax) + fitxpadding, fity
    // to lineHeight + fitypadding — UNROUNDED, and re-run on every
    // pass (the reset-to-base above happens every pass too). The sink
    // is the font authority, so the SOLVE asks it: layout and ink can
    // never disagree.
    if (n.tag == "label") {
      emitLabelFit(sw, vw, vh, pw, ph);
    }

    // ---- children (before position: a grown size feeds the align).
    // This widget joins the ancestor chain while its subtree solves, so a
    // descendant's size can name parent.width / parent.parent.height.
    wStack.push_back(vw);
    hStack.push_back(vh);
    solveChildren(sw, vw, vh, grows);
    wStack.pop_back();
    hStack.pop_back();

    // ---- position (the spec's position laws)
    const std::string *xAttr = get(sw.attrs, "x");
    const std::string *yAttr = get(sw.attrs, "y");
    std::string xOff = "0.0f", yOff = "0.0f";
    if (xAttr != nullptr) {
      xOff = dimExpr(parseDim(*xAttr), true, pw, ph, false);
    }
    if (yAttr != nullptr) {
      yOff = dimExpr(parseDim(*yAttr), false, pw, ph, false);
    }
    const std::string *al = get(sw.attrs, "align");
    const std::string *val = get(sw.attrs, "valign");
    auto alignBase = [&](const std::string *a, const std::string &pdim,
                         const std::string &cdim) -> std::string {
      if (a != nullptr && *a == "center") {
        return "R((" + pdim + " - " + cdim + ") / 2)";
      }
      if (a != nullptr && (*a == "right" || *a == "bottom")) {
        return pdim + " - " + cdim; // unrounded base (right/bottom never rounds)
      }
      return "0.0f"; // left/top and everything else (empty folds here)
    };
    auto plainPosition = [&] {
      solveLine(vx + " = " + alignBase(al, pw, vw) + " + R(" + xOff +
                ");");
      solveLine(vy + " = " + alignBase(val, ph, vh) + " + R(" + yOff +
                ");");
    };
    // the chain law: a child with ANY explicit x or y opts OUT of the
    // float chain (baseX/baseY empty is the eligibility test); states
    // never chain. Chain children take the main axis from the previous
    // target's far edge and the CROSS axis from the target's rect.
    // a child chains iff its x AND y offsets are EMPTY. Emptiness is the
    // offset's parse being empty — an absent attr AND an empty-string one
    // both qualify; a set offset like "0h" does not. A template threading
    // `y: y` from an unset no-default param folds y to "", so testing the
    // attr's PRESENCE would wrongly opt it out; test the VALUE's emptiness.
    const bool chains = chain.axis != 0 &&
                        (xAttr == nullptr || xAttr->empty()) &&
                        (yAttr == nullptr || yAttr->empty()) && !isState;
    if (chains) {
      const std::string &ps = chain.sfx;
      const std::string tx = "tx" + ps, ty = "ty" + ps, tw = "tw" + ps,
                        th = "th" + ps;
      solveLine("if (hasT" + ps + " != 0) {");
      ++solveIndent;
      auto crossAdj = [&](const std::string *a, const std::string &tdim,
                          const std::string &cdim) -> std::string {
        if (a != nullptr && *a == "center") {
          return " + R((" + tdim + " - " + cdim + ") / 2)";
        }
        if (a != nullptr && (*a == "right" || *a == "bottom")) {
          return " + (" + tdim + " - " + cdim + ")"; // unrounded
        }
        return "";
      };
      if (chain.axis == 'x') {
        solveLine(vx + " = R(" + tx + " + " + tw + " + " + chain.spacing +
                  ");");
        solveLine(vy + " = std::floor(" + ty + ")" + crossAdj(val, th, vh) +
                  ";");
      } else {
        solveLine(vy + " = R(" + ty + " + " + th + " + " + chain.spacing +
                  ");");
        solveLine(vx + " = std::floor(" + tx + ")" + crossAdj(al, tw, vw) +
                  ";");
      }
      --solveIndent;
      solveLine("} else {");
      ++solveIndent;
      plainPosition(); // the chain's first child positions plainly
      --solveIndent;
      solveLine("}");
    } else {
      plainPosition();
    }
  }

  // ---- labels -------------------------------------------------------------

  // a label's text: the `content` attr, or a bound expression (a
  // snapshot value formatted by the host). Both are borrowed for the
  // duration of the sink call (the Sink's string-lifetime contract).
  // the run's SOURCE value (a bound snapshot field, a formatted
  // number, or a literal) — the caller keeps it in a local and takes
  // views of it, per the Sink's string-lifetime contract
  // a `content` value is a text run. When it is a ternary (a gate on a
  // param, say), the RESULT branches can be different text types — a snapshot
  // char[N] field and a str param share no common type — so each result is
  // coerced to a string_view and the branches then unify. sv() already takes
  // a char[N], a string_view, a literal, or a TextBuf, and is idempotent, so
  // textExpr's own outer sv() over the whole run stays correct. A non-ternary
  // content is left untouched (that outer sv wraps it).
  std::string textBind(const Expr &e) {
    if (e.kind != Expr::kTernary) {
      return expr(e);
    }
    auto result = [&](const Expr &r) {
      return r.kind == Expr::kTernary ? textBind(r)
                                      : "gen_detail::sv(" + expr(r) + ")";
    };
    return "(" + expr(*e.args[0]) + " ? " + result(*e.args[1]) + " : " +
           result(*e.args[2]) + ")";
  }

  std::string textSource(const SolvedW &sw) {
    if (auto b = sw.binds.find("content"); b != sw.binds.end()) {
      return b->second;
    }
    const std::string *c = get(sw.attrs, "content");
    return c == nullptr ? std::string("std::string_view()")
                        : "std::string_view(" + quotedLit(*c) + ")";
  }

  // the same, as ONE borrowed run (for the solve's fitx measure)
  std::string textExpr(const SolvedW &sw) {
    return "gen_detail::sv(" + textSource(sw) + ")";
  }

  // attr values arrive verbatim, so a TEXT value already wears its
  // quotes (the quoting law: only text is quoted) — and .ui escapes
  // \" and \\ are C++'s. Anything else (a folded param, a bare word)
  // becomes a literal here.
  static std::string quotedLit(const std::string &s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
      return s;
    }
    std::string out = "\"";
    for (const char c : s) {
      if (c == '"' || c == '\\') {
        out += '\\';
      }
      out += c;
    }
    return out + "\"";
  }

  std::string fontOf(const SolvedW &sw) {
    const std::string *f = get(sw.attrs, "font");
    // the default font resource for an unfonted label
    return fontIdExpr(f != nullptr ? *f : std::string("system_medium"));
  }

  void emitLabelFit(const SolvedW &sw, const std::string &vw,
                    const std::string &vh, const std::string &pw,
                    const std::string &ph) {
    const bool fitX = flag(sw.attrs, "fitx", false);
    const bool fitY = flag(sw.attrs, "fity", false);
    if (!fitX && !fitY) {
      return;
    }
    const std::string font = fontOf(sw);
    auto sizeAttr = [&](const char *key, bool horizontal) -> std::string {
      const std::string *v = get(sw.attrs, key);
      if (v == nullptr) {
        return "";
      }
      return dimExpr(parseDim(*v), horizontal, pw, ph, false);
    };
    if (fitX) {
      solveLine(vw + " = sink.measure(" + font + ", " + textExpr(sw) + ");");
      const std::string mn = sizeAttr("fitxmin", true);
      const std::string mx = sizeAttr("fitxmax", true);
      if (!mn.empty()) {
        solveLine(vw + " = std::max(" + vw + ", " + mn + ");");
      }
      if (!mx.empty()) {
        solveLine(vw + " = std::min(" + vw + ", " + mx + ");");
      }
      const std::string pad = sizeAttr("fitxpadding", true);
      if (!pad.empty()) {
        solveLine(vw + " += " + pad + ";");
      }
    }
    if (fitY) {
      solveLine(vh + " = sink.line_height(" + font + ");");
      const std::string mn = sizeAttr("fitymin", false);
      const std::string mx = sizeAttr("fitymax", false);
      if (!mn.empty()) {
        solveLine(vh + " = std::max(" + vh + ", " + mn + ");");
      }
      if (!mx.empty()) {
        solveLine(vh + " = std::min(" + vh + ", " + mx + ");");
      }
      const std::string pad = sizeAttr("fitypadding", false);
      if (!pad.empty()) {
        solveLine(vh + " += " + pad + ";");
      }
    }
  }

  // Label draw: align the run in the widget's box, put the pen on the
  // baseline, and draw the shadow/outline pass under it.
  void emitLabel(const SolvedW &sw, const std::string &ax,
                 const std::string &ay, const std::string &w,
                 const std::string &h) {
    const std::map<std::string, std::string> &bag = sw.attrs;
    const std::string font = fontOf(sw);
    const std::string sfx = std::to_string(sw.idx);
    // the run's SOURCE is a local: a formatted number lives in its
    // buffer, and every borrow below points into that live value
    drawLine("const auto src" + sfx + " = " + textSource(sw) + ";");
    const std::string text = "gen_detail::sv(src" + sfx + ")";
    const std::string tw = "tw" + sfx, lh = "lh" + sfx;
    drawLine("const float " + tw + " = sink.measure(" + font + ", " + text +
             ");");
    const std::string *lhAttr = get(bag, "lineheight");
    drawLine("const float " + lh + " = " +
             (lhAttr != nullptr
                  ? "R(" + dimExpr(parseDim(*lhAttr), false, w, h, false) + ")"
                  : "sink.line_height(" + font + ")") +
             ";");

    const std::string *ta = get(bag, "textalign");
    std::string tx = ax;
    if (ta != nullptr && *ta == "center") {
      tx = ax + " + std::ceil((" + w + " - " + tw + ") / 2)";
    } else if (ta != nullptr && *ta == "right") {
      tx = ax + " + std::ceil(" + w + " - " + tw + ")";
    }
    const std::string *tva = get(bag, "textvalign");
    std::string ty = ay;
    if (tva != nullptr && *tva == "center") {
      ty = ay + " + std::ceil((" + h + " - " + lh + ") / 2)";
    } else if (tva != nullptr && *tva == "bottom") {
      ty = ay + " + std::ceil(" + h + " - " + lh + ")";
    }
    drawLine("const ui::Vec2 pen" + sfx + "{" + tx + ", " + ty +
             " + sink.ascent(" + font + ")};");

    // an untextured label with no color is GRAY, not white
    const std::string col = colorLiteral(sw, 0.5f);
    const bool outline = flag(bag, "outline", false);
    const bool shadow = !outline && flag(bag, "shadow", false);
    if (outline || shadow) {
      const std::string *offAttr =
          get(bag, outline ? "outlineoffset" : "shadowoffset");
      std::string off = offAttr != nullptr ? *offAttr : "1";
      if (const std::string *ox = get(bag, "shadowoffsetx");
          ox != nullptr && *ox != "0") {
        off = *ox;
      }
      float c[4] = {0, 0, 0, 1};
      if (const std::string *sc =
              get(bag, outline ? "outlinecolor" : "shadowcolor");
          sc != nullptr) {
        parseColor(*sc, c);
      }
      const std::string scol = "ui::Color{" + ftos(c[0]) + ", " + ftos(c[1]) +
                               ", " + ftos(c[2]) + ", " + ftos(c[3]) + "}";
      if (outline) {
        // the stroked variants where the font declares a stroker, else
        // the offset copy
        drawLine("if (sink.outline_width(" + font + ") > 0) {");
        drawLine("  sink.text_stroked(pen" + sfx + ", " + text + ", " + font +
                 ", " + scol + ", ui::kNoClip);");
        drawLine("} else {");
        drawLine("  sink.text({pen" + sfx + ".x + " + ftos(std::strtof(off.c_str(), nullptr)) +
                 ", pen" + sfx + ".y + " + ftos(std::strtof(off.c_str(), nullptr)) +
                 "}, " + text + ", " + font + ", " + scol + ", ui::kNoClip);");
        drawLine("}");
      } else {
        drawLine("sink.text({pen" + sfx + ".x + " +
                 ftos(std::strtof(off.c_str(), nullptr)) + ", pen" + sfx +
                 ".y + " + ftos(std::strtof(off.c_str(), nullptr)) + "}, " +
                 text + ", " + font + ", " + scol + ", ui::kNoClip);");
      }
    }
    drawLine("sink.text(pen" + sfx + ", " + text + ", " + font + ", " + col +
             ", ui::kNoClip);");
  }

  void solveChildren(SolvedW &sw, const std::string &vw,
                     const std::string &vh, bool grows) {
    const Node &n = *sw.node;
    if (n.children.empty()) {
      return;
    }
    const std::string *fl = get(sw.attrs, "float");
    const char mainAxis =
        fl == nullptr ? 0 : (*fl == "right" ? 'x' : (*fl == "bottom" ? 'y' : 0));
    const std::string sfx = std::to_string(sw.idx);
    std::string spacing = "0.0f";
    if (const std::string *pad = get(sw.attrs, "padding")) {
      spacing = "R(" + dimExpr(parseDim(*pad), mainAxis == 'x', vw, vh,
                               false) +
                ")";
    }
    const bool growinvis = flag(sw.attrs, "growinvis", true);

    if (grows) {
      // the union is FOUR-EDGED: a negative-offset child pulls
      // the origin — lo marks track it, the size is hi - lo, and the
      // growPass repositions children against the grown rect
      decl << "  float lox" << sfx << " = 0, loy" << sfx << " = 0, hix"
           << sfx << " = 0, hiy" << sfx << " = 0;\n";
      solveLine("for (int pass" + sfx + " = 0; pass" + sfx + " < 2; ++pass" +
                sfx + ") { // grow: pass 0 unions, pass 1 = the growPass");
      ++solveIndent;
      solveLine("if (pass" + sfx + " == 0) { lox" + sfx + " = 0; loy" +
                sfx + " = 0; hix" + sfx + " = " + vw + "; hiy" + sfx +
                " = " + vh + "; }");
    }
    ChainCtx chain;
    if (mainAxis != 0) {
      chain.axis = mainAxis;
      chain.sfx = sfx;
      chain.spacing = spacing;
      // the float TARGET rect: each surviving child becomes the next
      // target — main axis advances from its
      // far edge, the cross axis reads its position and size
      decl << "  float tx" << sfx << " = 0, ty" << sfx << " = 0, tw"
           << sfx << " = 0, th" << sfx << " = 0; int hasT" << sfx
           << " = 0;\n";
      solveLine("tx" + sfx + " = 0; ty" + sfx + " = 0; tw" + sfx +
                " = 0; th" + sfx + " = 0; hasT" + sfx + " = 0;");
    }
    for (const NodePtr &c : n.children) {
      const size_t before = sw.kids.size();
      solveInto(sw.kids, *c, vw, vh, chain);
      for (size_t k = before; k < sw.kids.size(); ++k) {
        const SolvedW &cw = *sw.kids[k];
        const std::string cs = std::to_string(cw.idx);
        const std::string *cvis = get(cw.attrs, "visible");
        const bool hidden =
            cvis != nullptr && (*cvis == "0" || *cvis == "false");
        // a BOUND visible gates occupancy at RUNTIME under the same
        // flags — the one-shot equivalent of a visibility toggle +
        // re-solve (this IS the emergent reflow)
        const auto bv = cw.binds.find("visible");
        const std::string dynVis =
            bv != cw.binds.end() ? bv->second : std::string();
        const bool counts = (!hidden || growinvis) && !cw.isState;
        const bool advances =
            (!hidden || flag(cw.attrs, "stickytoinvis", true)) &&
            !cw.isState;
        if (grows && counts) {
          std::string cond = "pass" + sfx + " == 0";
          if (!dynVis.empty() && !growinvis) {
            cond += " && (" + dynVis + ")";
          }
          // a structural-if guard is ABSENCE: it gates the union
          // regardless of growinvis
          if (!cw.guard.empty()) {
            cond += " && (" + cw.guard + ")";
          }
          // the child solved against the CURRENT (shifted) origin, which
          // sits at lo in the pass-0 frame — offset its extent before
          // uniting, or later children re-count the shift (uniting in
          // the parent's parent space would get this for free)
          solveLine("if (" + cond + ") { const float nx" + cs + " = lox" +
                    sfx + " + x" + cs + ", ny" + cs + " = loy" + sfx +
                    " + y" + cs + "; hix" + sfx + " = std::max(hix" + sfx +
                    ", nx" + cs + " + w" + cs + "); hiy" + sfx +
                    " = std::max(hiy" + sfx + ", ny" + cs + " + h" + cs +
                    "); lox" + sfx + " = std::min(lox" + sfx + ", nx" +
                    cs + "); loy" + sfx + " = std::min(loy" + sfx +
                    ", ny" + cs + "); " + vw + " = hix" + sfx + " - lox" +
                    sfx + "; " + vh + " = hiy" + sfx + " - loy" + sfx +
                    "; }");
        }
        if (mainAxis != 0 && advances) {
          const std::string update = "tx" + sfx + " = x" + cs + "; ty" +
                                     sfx + " = y" + cs + "; tw" + sfx +
                                     " = w" + cs + "; th" + sfx + " = h" +
                                     cs + "; hasT" + sfx + " = 1;";
          std::string gate;
          if (!dynVis.empty() && !flag(cw.attrs, "stickytoinvis", true)) {
            gate = dynVis;
          }
          // absence gates the chain advance regardless of stickytoinvis
          if (!cw.guard.empty()) {
            gate = gate.empty() ? cw.guard
                                : "(" + cw.guard + ") && (" + gate + ")";
          }
          if (!gate.empty()) {
            solveLine("if (" + gate + ") { " + update + " }");
          } else {
            solveLine(update);
          }
        }
      }
    }
    if (grows) {
      --solveIndent;
      solveLine("}");
    }
  }

  // ---- the draw pass (render order; absolute coordinates) -----------------

  void drawNode(const SolvedW &sw, const std::string &pax,
                const std::string &pay) {
    const Node &n = *sw.node;
    const std::string sfx = std::to_string(sw.idx);
    const std::string ax = "ax" + sfx, ay = "ay" + sfx;
    drawLine("const float " + ax + " = " + pax + " + x" + sfx + ";");
    drawLine("const float " + ay + " = " + pay + " + y" + sfx + ";");
    if (opt.rectLog) {
      const std::string *nm = get(sw.attrs, "name");
      drawLine("if (log) log->add(\"" + n.tag + "\", \"" +
               (nm != nullptr ? *nm : std::string()) + "\", " + ax + ", " +
               ay + ", w" + sfx + ", h" + sfx + ");");
    }

    const std::string *vis = get(sw.attrs, "visible");
    const bool hidden =
        vis != nullptr && (*vis == "0" || *vis == "false");
    if (hidden) {
      // visible: 0 gates the SUBTREE's drawing (solved for occupancy)
      drawLine("// hidden subtree (visible: 0) :" +
               std::to_string(n.line));
      return;
    }
    const auto bv = sw.binds.find("visible");
    std::string gate =
        bv != sw.binds.end() ? bv->second : std::string();
    if (!sw.guard.empty()) { // structural-if arm: the subtree may not exist
      gate = gate.empty() ? sw.guard
                          : "(" + sw.guard + ") && (" + gate + ")";
    }
    const bool bound = !gate.empty();
    if (bound) {
      drawLine("if (" + gate + ") {");
      ++drawIndent;
    }
    emitDraw(sw, ax, ay, "w" + sfx, "h" + sfx);

    // render order: solved order, reversed when reverse: 1 (layout
    // already ran in document order — the solve pass)
    if (sw.reverse) {
      for (auto it = sw.kids.rbegin(); it != sw.kids.rend(); ++it) {
        drawNode(**it, ax, ay);
      }
    } else {
      for (const SolvedW *c : sw.kids) {
        drawNode(*c, ax, ay);
      }
    }
    if (bound) { // a bound visible gates the subtree too
      --drawIndent;
      drawLine("}");
    }
  }

  std::string colorLiteral(const SolvedW &sw, float def) {
    // a bound colour wins over the static attr: the transpiled expr already
    // evaluates to a ui::Color (a colour-typed param / color(...) / #hex)
    if (auto b = sw.binds.find("color"); b != sw.binds.end()) {
      return b->second;
    }
    float c[4] = {def, def, def, 1};
    if (const std::string *col = get(sw.attrs, "color")) {
      if (!parseColor(*col, c)) {
        diags.push_back({m.name, sw.node->line, 0,
                         "unparsable color '" + *col + "'",
                         Diag::Severity::kWarning});
      }
    }
    return "ui::Color{" + ftos(c[0]) + ", " + ftos(c[1]) + ", " +
           ftos(c[2]) + ", " + ftos(c[3]) + "}";
  }

  // the sink flags a widget's rendermode/tiling imply — the blend/grayscale
  // bits (rendermode="overlay"|"additive"|"grayscale") plus U/V tiling.
  // uscale/hflip stay UV-side, so they are not here.
  std::string renderFlags(const std::map<std::string, std::string> &bag) {
    std::vector<std::string> fs;
    if (const std::string *rm = get(bag, "rendermode")) {
      if (*rm == "additive") {
        fs.push_back("ui::kBlendAdditive");
      } else if (*rm == "overlay") {
        fs.push_back("ui::kBlendOverlay");
      } else if (*rm == "grayscale") {
        fs.push_back("ui::kGrayscale");
      }
    }
    if (const std::string *u = get(bag, "utile"); u != nullptr && *u == "1") {
      fs.push_back("ui::kTileU");
    }
    if (const std::string *v = get(bag, "vtile"); v != nullptr && *v == "1") {
      fs.push_back("ui::kTileV");
    }
    if (fs.empty()) {
      return "0";
    }
    std::string out = fs[0];
    for (size_t i = 1; i < fs.size(); ++i) {
      out += " | " + fs[i];
    }
    return out;
  }

  void emitDraw(const SolvedW &sw, const std::string &ax,
                const std::string &ay, const std::string &w,
                const std::string &h) {
    const Node &n = *sw.node;
    const std::map<std::string, std::string> &bag = sw.attrs;
    const std::string dst =
        "{" + ax + ", " + ay + ", " + w + ", " + h + "}";
    if (n.tag == "label") {
      emitLabel(sw, ax, ay, w, h);
      return;
    }
    if (n.tag == "frame") {
      const std::string *tex = get(bag, "texture");
      if (tex == nullptr) {
        // a textureless frame is a plain border: bordercolor drawn as
        // four edge quads of borderthickness
        if (get(bag, "bordercolor") == nullptr) {
          skip(n.line, "textureless frame with no border");
          return;
        }
        const std::string *bt = get(bag, "borderthickness");
        const std::string bthE =
            "R(" +
            dimExpr(bt != nullptr ? parseDim(*bt) : parseDim("1"), false, w,
                    h, false) +
            ")";
        float bc[4] = {0, 0, 0, 1};
        if (const std::string *v = get(bag, "bordercolor")) {
          parseColor(*v, bc);
        }
        const std::string bcol = "ui::Color{" + ftos(bc[0]) + ", " +
                                 ftos(bc[1]) + ", " + ftos(bc[2]) + ", " +
                                 ftos(bc[3]) + "}";
        const std::string bth = "bth" + std::to_string(sw.idx);
        drawLine("const float " + bth + " = " + bthE + ";");
        drawLine("sink.quad({" + ax + ", " + ay + ", " + w + ", " + bth +
                 "}, " + bcol + ", 0, ui::kNoClip);"); // top
        drawLine("sink.quad({" + ax + ", " + ay + " + " + h + " - " + bth +
                 ", " + w + ", " + bth + "}, " + bcol +
                 ", 0, ui::kNoClip);"); // bottom
        drawLine("sink.quad({" + ax + ", " + ay + ", " + bth + ", " + h +
                 "}, " + bcol + ", 0, ui::kNoClip);"); // left
        drawLine("sink.quad({" + ax + " + " + w + " - " + bth + ", " + ay +
                 ", " + bth + ", " + h + "}, " + bcol +
                 ", 0, ui::kNoClip);"); // right
        return;
      }
      const std::string *bt = get(bag, "borderthickness");
      const std::string btE =
          "R(" +
          dimExpr(bt != nullptr ? parseDim(*bt) : parseDim("0"), false,
                  w, h, false) +
          ")";
      const std::string col = colorLiteral(sw, 1);
      const size_t dot = tex->rfind('.');
      static const struct {
        const char *suffix;
        int gx, gy;
      } kPieces[9] = {{"_tl", 0, 0}, {"_t", 1, 0},  {"_tr", 2, 0},
                      {"_l", 0, 1},  {"_c", 1, 1},  {"_r", 2, 1},
                      {"_bl", 0, 2}, {"_b", 1, 2},  {"_br", 2, 2}};
      const std::string fflags = renderFlags(bag);
      drawLine("{");
      ++drawIndent;
      drawLine("const float bt = std::min(" + btE + ", std::min(" + w +
               ", " + h + ") / 2);");
      drawLine("const float cwm = " + w + " - 2 * bt, chm = " + h +
               " - 2 * bt;");
      for (const auto &p : kPieces) {
        const std::string piece =
            tex->substr(0, dot) + p.suffix + tex->substr(dot);
        const std::string x =
            p.gx == 0 ? ax : (p.gx == 1 ? ax + " + bt" : ax + " + bt + cwm");
        const std::string y =
            p.gy == 0 ? ay : (p.gy == 1 ? ay + " + bt" : ay + " + bt + chm");
        drawLine("sink.image({" + x + ", " + y + ", " +
                 (p.gx == 1 ? "cwm" : "bt") + ", " +
                 (p.gy == 1 ? "chm" : "bt") + "}, " +
                 texIdExpr(piece, n.line) + ", {0, 0, 1, 1}, " + col +
                 ", " + fflags + ", 0, ui::kNoClip);");
      }
      --drawIndent;
      drawLine("}");
      return;
    }

    const std::string *tex = get(bag, "texture");
    const auto texBind = sw.binds.find("texture");
    const bool boundTex = texBind != sw.binds.end();
    if (n.tag == "image" && tex == nullptr && !boundTex) {
      skip(n.line, "image without a resolvable texture");
      return;
    }
    if (tex != nullptr && *tex == "$invis") {
      return; // the dialect law: $invis draws nothing
    }
    // the dialect's solid texels: $white and $black are fills, not art
    // (a mask cuts them to shape — that IS the stencil pattern)
    const bool solid =
        tex != nullptr && (*tex == "$white" || *tex == "$black");
    if (boundTex || (tex != nullptr && !solid)) {
      std::string idExpr;
      if (boundTex) {
        idExpr = texBind->second;
      } else {
        std::string path = *tex;
        auto it = assetConsts.find(path);
        if (it != assetConsts.end()) {
          path = it->second;
        }
        idExpr = texIdExpr(path, n.line);
      }
      std::string uv = "{0, 0, 1, 1}";
      const std::string flags = renderFlags(bag);
      if (const std::string *us = get(bag, "uscale")) {
        const float span = 1.f / std::strtof(us->c_str(), nullptr);
        uv = "{0, 0, " + ftos(span) + ", 1}";
      }
      if (const std::string *hf = get(bag, "hflip");
          hf != nullptr && *hf == "1") {
        uv = "{1, 0, -1, 1}";
      }
      drawLine("sink.image(" + dst + ", " + idExpr + ", " + uv + ", " +
               colorLiteral(sw, 1) + ", " + flags + ", " +
               maskOf(bag, n.line) + ", ui::kNoClip);");
      return;
    }
    if (get(bag, "color") != nullptr || solid) {
      // an unpainted $black fills black, an unpainted $white white
      const float def = tex != nullptr && *tex == "$black" ? 0.0f : 1.0f;
      const std::string col = colorLiteral(sw, def);
      const std::string mask = maskOf(bag, n.line);
      const std::string flags = renderFlags(bag);
      if (mask != "0") {
        drawLine("sink.image(" + dst + ", ui::Texture::White, {0, 0, 1, 1}, " +
                 col + ", " + flags + ", " + mask + ", ui::kNoClip);");
        return;
      }
      drawLine("sink.quad(" + dst + ", " + col + ", " + flags +
               ", ui::kNoClip);");
    }
  }

  // the alpha mask a widget wears (usealphamask + alphamaskfile) — the
  // shape its ink is cut to, sampled across the destination rect
  std::string maskOf(const std::map<std::string, std::string> &bag,
                     int atLine) {
    if (!flag(bag, "usealphamask", false)) {
      return "0";
    }
    const std::string *f = get(bag, "alphamaskfile");
    if (f == nullptr) {
      diags.push_back({m.name, atLine, 0,
                       "usealphamask without an alphamaskfile",
                       Diag::Severity::kWarning});
      return "0";
    }
    std::string path = *f;
    auto it = assetConsts.find(path);
    if (it != assetConsts.end()) {
      path = it->second;
    }
    // A mask is a MODIFIER: art the host cannot load simply does not
    // cut the shape — the widget simply draws whole, uncut.
    // Converted trees carry dead mask paths their own source shipped, so
    // this degrades loudly instead of failing the build — unlike a
    // texture the widget IS.
    if (!assetExists(path)) {
      diags.push_back({m.name, atLine, 0,
                       "missing alpha mask (drawing unmasked): " + path,
                       Diag::Severity::kWarning});
      return "0";
    }
    return texIdExpr(path, atLine);
  }

  void registerModule(const Module &mod) {
    for (const StyleDecl &s : mod.styles) {
      styles.emplace(s.name, &s);
    }
    for (const EnumDecl &e : mod.enums) {
      enumNames.insert(e.name); // Variant.Triple -> Variant::Triple
    }
    for (const TemplateDecl &t : mod.templates) {
      templates.emplace(t.name, &t);
    }
    for (const ConstDecl &c : mod.consts) {
      if (c.type == "asset" ||
          (!c.rawValue.empty() && c.rawValue[0] == '/')) {
        assetConsts.emplace(c.name, c.rawValue);
      }
    }
  }

  void run() {
    registerModule(m);
    for (const Module *sm : opt.styleModules) {
      registerModule(*sm);
    }
    for (const Module *wm : opt.withModules) {
      registerModule(*wm);
    }
    std::vector<SolvedW *> roots;
    for (const NodePtr &n : m.roots) { // screen = the parent
      solveInto(roots, *n, "W", "H", ChainCtx{});
    }
    for (const SolvedW *r : roots) {
      drawNode(*r, "0.0f", "0.0f");
    }
  }
};

} // namespace

std::string emitPanelHeader(const Module &m, const EmitOptions &opt,
                            std::vector<Diag> &diags) {
  Emit e(m, opt, diags);
  e.run();

  std::ostringstream os;
  std::string fn = m.name;
  for (char &c : fn) {
    if (std::isalnum((unsigned char)c) == 0) {
      c = '_';
    }
  }
  os << "#pragma once\n"
     << "// GENERATED by uic --emit from " << m.name
     << ".ui -- DO NOT EDIT.\n"
     << "// Layout scheduler codegen (the layout annex in the language spec "
        "is normative).\n"
     << "// Depends on silhouette + the schema header only.\n"
     << "#include \"" << opt.schemaInclude << "\"\n"
     << "#include \"paint/sink.hpp\"\n\n"
     << "#include <algorithm>\n"
     << "#include <array>\n"
     << "#include <cmath>\n"
     << "#include <cstddef>\n"
     << "#include <cstdint>\n"
     << "#include <cstdio>\n"
     << "#include <string_view>\n"
     << "#include <type_traits>\n\n"
     << "namespace " << opt.ns << " {\n"
     << "namespace gen_detail {\n"
     << "// Round: floor(f + 0.5), applied per widget\n"
     << "inline float R(float f) { return std::floor(f + 0.5f); }\n"
     << "inline float fracf(float a, float b) { return b > 0 ? a / b : 0; "
        "}\n"
     << "// a text run's source: a schema char array is a run up to its\n"
     << "// first NUL; a string_view is itself. Borrowed for the call\n"
     << "// only — the Sink's string-lifetime contract.\n"
     << "template <size_t N>\n"
     << "inline std::string_view sv(const std::array<char, N> &a) {\n"
     << "  size_t n = 0;\n"
     << "  while (n < N && a[n] != '\\0') { ++n; }\n"
     << "  return std::string_view(a.data(), n);\n"
     << "}\n"
     << "inline std::string_view sv(std::string_view s) { return s; }\n"
     << "// A number rendered into a text run. The LANGUAGE has no string\n"
     << "// interpolation; these are the compiler's typed conversions at\n"
     << "// a text sink — num(v), fixed(v, decimals), and fmt(\"{} / {}\",\n"
     << "// ...) that fills each {} with the next arg. The buffer is a\n"
     << "// value with automatic storage: no allocation, and the run is\n"
     << "// borrowed from a live local exactly like every other run.\n"
     << "struct TextBuf {\n"
     << "  char b[64];\n"
     << "  int n = 0;\n"
     << "  void put(char c) { if (n < (int)sizeof b) b[n++] = c; }\n"
     << "  void put(std::string_view s) { for (char c : s) put(c); }\n"
     << "};\n"
     << "inline std::string_view sv(const TextBuf &t) {\n"
     << "  return std::string_view(t.b, (size_t)t.n);\n"
     << "}\n"
     << "inline TextBuf num(long long v) {\n"
     << "  TextBuf t;\n"
     << "  char tmp[24];\n"
     << "  t.put(std::string_view(tmp, (size_t)std::max(0,\n"
     << "        std::snprintf(tmp, sizeof tmp, \"%lld\", v))));\n"
     << "  return t;\n"
     << "}\n"
     << "inline TextBuf fixed(double v, int decimals) {\n"
     << "  TextBuf t;\n"
     << "  char tmp[32];\n"
     << "  t.put(std::string_view(tmp, (size_t)std::max(0,\n"
     << "        std::snprintf(tmp, sizeof tmp, \"%.*f\", decimals, v))));\n"
     << "  return t;\n"
     << "}\n"
     << "// one fmt argument -> the buffer, by type\n"
     << "inline void fmt_one(TextBuf &t, std::string_view s) { t.put(s); }\n"
     << "template <std::size_t N>\n"
     << "inline void fmt_one(TextBuf &t, const std::array<char, N> &a) {\n"
     << "  t.put(sv(a));\n"
     << "}\n"
     << "template <class T>\n"
     << "inline void fmt_one(TextBuf &t, T v) {\n"
     << "  char tmp[32];\n"
     << "  if constexpr (std::is_floating_point_v<T>) {\n"
     << "    t.put(std::string_view(tmp, (size_t)std::max(0,\n"
     << "          std::snprintf(tmp, sizeof tmp, \"%g\", (double)v))));\n"
     << "  } else {\n"
     << "    t.put(std::string_view(tmp, (size_t)std::max(0,\n"
     << "          std::snprintf(tmp, sizeof tmp, \"%lld\", (long long)v))));\n"
     << "  }\n"
     << "}\n"
     << "inline void fmt_into(TextBuf &t, std::string_view f) { t.put(f); }\n"
     << "template <class A, class... Rest>\n"
     << "inline void fmt_into(TextBuf &t, std::string_view f, const A &a,\n"
     << "                     const Rest &...rest) {\n"
     << "  const std::size_t h = f.find(\"{}\");\n"
     << "  if (h == std::string_view::npos) { t.put(f); return; }\n"
     << "  t.put(f.substr(0, h));\n"
     << "  fmt_one(t, a);\n"
     << "  fmt_into(t, f.substr(h + 2), rest...);\n"
     << "}\n"
     << "template <class... Args>\n"
     << "inline TextBuf fmt(std::string_view f, const Args &...args) {\n"
     << "  TextBuf t;\n"
     << "  fmt_into(t, f, args...);\n"
     << "  return t;\n"
     << "}\n"
     << "} // namespace gen_detail\n\n";
  // inline-enum id constants THIS file uses, folded to their sid at emit
  // time. `inline` so identical defs across translation units agree; a
  // param-only enum (one a codegen pass invents) has no schema home, so this
  // is where its ids live. No sid() call ships — the value is the literal.
  if (!e.enumIds.empty()) {
    for (const auto &nv : e.enumIds) {
      char idbuf[16];
      std::snprintf(idbuf, sizeof idbuf, "0x%08Xu", enumSid(nv.second));
      os << "inline constexpr uint32_t " << nv.first << " = " << idbuf
         << "; // " << nv.second << "\n";
    }
    os << "\n";
  }
  if (opt.rectLog) {
    os << "// the rect gate's hook: absolute rect per widget, document "
          "order\n"
       << "struct RectLog {\n"
       << "  virtual ~RectLog() = default;\n"
       << "  virtual void add(std::string_view tag, std::string_view "
          "name,\n"
       << "                   float x, float y, float w, float h) = 0;\n"
       << "};\n\n";
  }
  os << "// host registration manifest: load each path, register under "
        "its id\n"
     << "struct TexRef { ui::TextureId id; std::string_view path; };\n"
     << "inline constexpr TexRef k" << fn << "_textures[] = {\n";
  for (const std::string &path : e.texOrder) {
    char idbuf[16];
    std::snprintf(idbuf, sizeof idbuf, "0x%08Xu", e.texIds[path]);
    os << "  {" << idbuf << ", \"" << path << "\"},\n";
  }
  os << "};\n\n"
     << "// the same for fonts: the module NAMES them (the host decides\n"
     << "// what face/size each name is) and asks the sink to measure\n"
     << "struct FontRef { ui::FontId id; std::string_view name; };\n"
     << "inline constexpr FontRef k" << fn << "_fonts[] = {\n";
  for (const std::string &name : e.fontOrder) {
    char idbuf[16];
    std::snprintf(idbuf, sizeof idbuf, "0x%08Xu", e.fontIds[name]);
    os << "  {" << idbuf << ", \"" << name << "\"},\n";
  }
  os << "};\n\n"
     << "template <class Sink>\n"
     << "void " << fn
     << "(const UiSnapshot &s, float screenW, float screenH, Sink &sink"
     << (opt.rectLog ? ", RectLog *log = nullptr" : "") << ") {\n"
     << "  using gen_detail::R;\n"
     << "  (void)s;\n"
     << "  const float W = screenW, H = screenH;\n"
     << "  (void)W;\n"
     << "  // ---- rect variables (parent-relative) ----\n"
     << e.decl.str() << "  // ---- solve (the schedule) ----\n"
     << e.solve.str() << "  // ---- draw (render order, absolute) ----\n"
     << e.draw.str() << "}\n\n"
     << "} // namespace " << opt.ns << "\n";
  return os.str();
}

} // namespace uic
