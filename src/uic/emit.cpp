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
  const char *p = s.c_str();
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
  if (got < 3) {
    return false;
  }
  out[0] = v[0];
  out[1] = v[1];
  out[2] = v[2];
  out[3] = v[3];
  return true;
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
  std::map<std::string, std::string> assetConsts;
  std::map<std::string, const StyleDecl *> styles;
  std::map<std::string, const TemplateDecl *> templates;
  std::set<std::string> enumNames; // schema enums -> scope-resolved refs
  std::deque<SolvedW> arena;
  std::vector<std::map<std::string, std::string>> envStack;
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

  uint32_t texId(const std::string &path, int atLine) {
    if (!opt.assetRoot.empty() && !path.empty() && path[0] != '$') {
      namespace fs = std::filesystem;
      fs::path full = fs::path(opt.assetRoot) /
                      (path[0] == '/' ? path.substr(1) : path);
      std::error_code ec;
      bool ok = fs::exists(full, ec);
      if (!ok && full.extension() == ".tga") {
        full.replace_extension(".dds");
        ok = fs::exists(full, ec);
      }
      if (!ok) {
        err(atLine, "missing asset: " + path);
      }
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

  // ---- expressions (binds) ----------------------------------------------

  std::string expr(const Expr &e) {
    switch (e.kind) {
    case Expr::kIdent: {
      if (e.text == "snapshot") {
        return "s";
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
    case Expr::kString:
    case Expr::kDim:
    case Expr::kColor:
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
      err(e.line, "call in bind (subset builtin: fracf)");
      return "0";
    case Expr::kUnary:
      return "(" + e.text + expr(*e.args[0]) + ")";
    case Expr::kBinary:
      return "(" + expr(*e.args[0]) + " " + e.text + " " +
             expr(*e.args[1]) + ")";
    case Expr::kTernary:
      return "(" + expr(*e.args[0]) + " ? " + expr(*e.args[1]) + " : " +
             expr(*e.args[2]) + ")";
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

  std::map<std::string, std::string> bagOf(const Node &n) {
    std::map<std::string, std::string> bag;
    for (const Attr &a : n.attrs) {
      bag.emplace(a.name, a.value);
    }
    auto styleIt = bag.find("style");
    if (styleIt != bag.end()) {
      // left-to-right, widget wins (first insertion wins the bag)
      const std::string s = styleIt->second;
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
        auto it = styles.find(name);
        if (it == styles.end()) {
          if (!name.empty()) {
            diags.push_back({m.name, n.line, 0,
                             "unknown style '" + name + "'",
                             Diag::Severity::kWarning});
          }
        } else {
          for (const Attr &a : it->second->attrs) {
            bag.emplace(a.name, a.value);
          }
        }
        if (comma == s.size()) {
          break;
        }
        i = comma + 1;
      }
    }
    if (!envStack.empty()) {
      for (auto &kv : bag) {
        kv.second = substitute(kv.second);
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
    return v == nullptr ? def : (*v != "0");
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
                 const std::string &cursor, const char mainAxis) {
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
          solveInto(out, *b, pw, ph, cursor, mainAxis);
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
    if (n.kind != Node::kWidget) {
      skip(n.line, "widgetstate");
      return;
    }
    if (n.tag != "panel" && n.tag != "image" && n.tag != "frame") {
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
      for (const Attr &a : n.attrs) {
        bool known = false;
        for (const InParam &p : t.ins) {
          if (p.name == a.name) {
            known = true;
            break;
          }
        }
        if (!known) {
          diags.push_back({m.name, a.line, 0,
                           "'" + t.name + "' has no param '" + a.name +
                               "' (arg ignored)",
                           Diag::Severity::kWarning});
          continue;
        }
        // arg values may reference the OUTER instantiation's params
        env[a.name] = substitute(a.value);
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
      ++instanceDepth;
      for (const NodePtr &b : t.body) {
        solveInto(out, *b, pw, ph, cursor, mainAxis);
      }
      --instanceDepth;
      envStack.pop_back();
      solveLine("// <<< " + t.name);
      return;
    }

    SolvedW &sw = arena.emplace_back();
    sw.node = &n;
    sw.attrs = bagOf(n);
    sw.idx = nextVar++;
    sw.reverse = flag(sw.attrs, "reverse", false);
    // transpile binds NOW — the instantiation's param environment is
    // gone by draw time
    for (const Bind &b : n.binds) {
      sw.binds[b.target] = expr(*b.expr);
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

    // ---- size (annex: explicit floors the union on grow widgets;
    // missing size = 100% of parent, or the union alone when growing)
    auto sizeExpr = [&](const char *key, const char *sizeKey,
                        bool horizontal) -> std::string {
      const std::string *v = get(sw.attrs, key);
      if (v == nullptr) {
        v = get(sw.attrs, sizeKey);
      }
      if (v != nullptr) {
        const Dim d = parseDim(*v);
        if (d.kind == Dim::kBad) {
          diags.push_back({m.name, n.line, 0,
                           std::string("unsupported dim '") + *v +
                               "' for " + key,
                           Diag::Severity::kWarning});
        }
        return "R(" + dimExpr(d, horizontal, pw, ph, true) + ")";
      }
      if (grows) {
        return "0.0f"; // union alone (the floor law)
      }
      return horizontal ? pw : ph; // default 100% of the parent
    };
    std::string wE = sizeExpr("width", "size", true);
    std::string hE = sizeExpr("height", "size", false);
    if (auto b = sw.binds.find("width"); b != sw.binds.end()) {
      wE = "R(" + pw + " * " + b->second + ")"; // interim fraction law
    }
    if (auto b = sw.binds.find("height"); b != sw.binds.end()) {
      hE = "R(" + ph + " * " + b->second + ")";
    }
    solveLine(vw + " = " + wE + ";");
    solveLine(vh + " = " + hE + ";");

    // ---- children (before position: a grown size feeds the align)
    solveChildren(sw, vw, vh, grows);

    // ---- position
    std::string xOff = "0.0f", yOff = "0.0f";
    if (const std::string *x = get(sw.attrs, "x")) {
      xOff = dimExpr(parseDim(*x), true, pw, ph, false);
    }
    if (const std::string *y = get(sw.attrs, "y")) {
      yOff = dimExpr(parseDim(*y), false, pw, ph, false);
    }
    const std::string *al = get(sw.attrs, "align");
    const std::string *val = get(sw.attrs, "valign");
    auto alignBase = [&](const std::string *a, const std::string &pdim,
                         const std::string &cdim) -> std::string {
      if (a == nullptr || *a == "left" || *a == "top") {
        return "0.0f";
      }
      if (*a == "center") {
        return "R((" + pdim + " - " + cdim + ") / 2)";
      }
      return pdim + " - " + cdim; // right/bottom (unrounded base)
    };
    if (mainAxis == 'x') {
      solveLine(vx + " = " + cursor + " + R(" + xOff + ");");
      solveLine(vy + " = " + alignBase(val, ph, vh) + " + R(" + yOff +
                ");");
    } else if (mainAxis == 'y') {
      solveLine(vy + " = " + cursor + " + R(" + yOff + ");");
      solveLine(vx + " = " + alignBase(al, pw, vw) + " + R(" + xOff +
                ");");
    } else {
      solveLine(vx + " = " + alignBase(al, pw, vw) + " + R(" + xOff +
                ");");
      solveLine(vy + " = " + alignBase(val, ph, vh) + " + R(" + yOff +
                ");");
    }
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
      solveLine("for (int pass" + sfx + " = 0; pass" + sfx + " < 2; ++pass" +
                sfx + ") { // grow: pass 0 unions, pass 1 = the growPass");
      ++solveIndent;
    }
    std::string cursor;
    if (mainAxis != 0) {
      cursor = "cur" + sfx;
      decl << "  float " << cursor << " = 0;\n";
      solveLine(cursor + " = 0;");
    }
    for (const NodePtr &c : n.children) {
      const size_t before = sw.kids.size();
      solveInto(sw.kids, *c, vw, vh, cursor, mainAxis);
      for (size_t k = before; k < sw.kids.size(); ++k) {
        const SolvedW &cw = *sw.kids[k];
        const std::string cs = std::to_string(cw.idx);
        const std::string *cvis = get(cw.attrs, "visible");
        const bool hidden = cvis != nullptr && *cvis == "0";
        // a BOUND visible gates occupancy at RUNTIME under the same
        // flags — the one-shot equivalent of a visibility toggle +
        // re-solve (this IS the emergent reflow)
        const auto bv = cw.binds.find("visible");
        const std::string dynVis =
            bv != cw.binds.end() ? bv->second : std::string();
        const bool counts = !hidden || growinvis;
        const bool advances = !hidden || flag(cw.attrs, "stickytoinvis",
                                              true);
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
          solveLine("if (" + cond + ") { " + vw + " = std::max(" + vw +
                    ", x" + cs + " + w" + cs + "); " + vh +
                    " = std::max(" + vh + ", y" + cs + " + h" + cs +
                    "); }");
        }
        if (mainAxis != 0 && advances) {
          const std::string cend = mainAxis == 'x'
                                       ? "x" + cs + " + w" + cs
                                       : "y" + cs + " + h" + cs;
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
            solveLine("if (" + gate + ") { " + cursor + " = " + cend +
                      " + " + spacing + "; }");
          } else {
            solveLine(cursor + " = " + cend + " + " + spacing + ";");
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
    const bool hidden = vis != nullptr && *vis == "0";
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

  std::string colorLiteral(const std::map<std::string, std::string> &bag,
                           int line, float def) {
    float c[4] = {def, def, def, 1};
    if (const std::string *col = get(bag, "color")) {
      if (!parseColor(*col, c)) {
        diags.push_back({m.name, line, 0,
                         "unparsable color '" + *col + "'",
                         Diag::Severity::kWarning});
      }
    }
    return "ui::Color{" + ftos(c[0]) + ", " + ftos(c[1]) + ", " +
           ftos(c[2]) + ", " + ftos(c[3]) + "}";
  }

  void emitDraw(const SolvedW &sw, const std::string &ax,
                const std::string &ay, const std::string &w,
                const std::string &h) {
    const Node &n = *sw.node;
    const std::map<std::string, std::string> &bag = sw.attrs;
    const std::string dst =
        "{" + ax + ", " + ay + ", " + w + ", " + h + "}";
    if (n.tag == "frame") {
      const std::string *tex = get(bag, "texture");
      if (tex == nullptr) {
        skip(n.line, "textureless frame");
        return;
      }
      const std::string *bt = get(bag, "borderthickness");
      const std::string btE =
          "R(" +
          dimExpr(bt != nullptr ? parseDim(*bt) : parseDim("0"), false,
                  w, h, false) +
          ")";
      const std::string col = colorLiteral(bag, n.line, 1);
      const size_t dot = tex->rfind('.');
      static const struct {
        const char *suffix;
        int gx, gy;
      } kPieces[9] = {{"_tl", 0, 0}, {"_t", 1, 0},  {"_tr", 2, 0},
                      {"_l", 0, 1},  {"_c", 1, 1},  {"_r", 2, 1},
                      {"_bl", 0, 2}, {"_b", 1, 2},  {"_br", 2, 2}};
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
                 ", 0, 0, ui::kNoClip);");
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
    if (boundTex || (tex != nullptr && *tex != "$white")) {
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
      std::string flags = "0";
      if (const std::string *us = get(bag, "uscale")) {
        const float span = 1.f / std::strtof(us->c_str(), nullptr);
        uv = "{0, 0, " + ftos(span) + ", 1}";
      }
      if (const std::string *hf = get(bag, "hflip");
          hf != nullptr && *hf == "1") {
        uv = "{1, 0, -1, 1}";
      }
      if (const std::string *ut = get(bag, "utile");
          ut != nullptr && *ut == "1") {
        flags = "ui::kTileU";
      }
      drawLine("sink.image(" + dst + ", " + idExpr + ", " + uv + ", " +
               colorLiteral(bag, n.line, 1) + ", " + flags +
               ", 0, ui::kNoClip);");
      return;
    }
    if (get(bag, "color") != nullptr ||
        (tex != nullptr && *tex == "$white")) {
      drawLine("sink.quad(" + dst + ", " + colorLiteral(bag, n.line, 1) +
               ", 0, ui::kNoClip);");
    }
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
      solveInto(roots, *n, "W", "H", "", 0);
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
     << "#include <cmath>\n"
     << "#include <cstdint>\n"
     << "#include <string_view>\n\n"
     << "namespace " << opt.ns << " {\n"
     << "namespace gen_detail {\n"
     << "// Round: floor(f + 0.5), applied per widget\n"
     << "inline float R(float f) { return std::floor(f + 0.5f); }\n"
     << "inline float fracf(float a, float b) { return b > 0 ? a / b : 0; "
        "}\n"
     << "} // namespace gen_detail\n\n";
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
