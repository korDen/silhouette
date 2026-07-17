// uic emit pass — the FIRST vertical slice of panel codegen: a module
// whose widgets use the ABSOLUTE subset compiles to a self-contained
// header:  template <class Sink> void <module>(const NS::UiSnapshot&,
// float screenW, float screenH, Sink&).
//
// Subset (grows toward the full dialect; anything outside it is a
// loud diagnostic, never a silent skip):
//   - widgets: panel (quad when colored, container otherwise), image,
//     frame (nine-slice, borderthickness vs PARENT — the dialect law);
//   - dims: plain px, %, @ (parent other axis), h, w (screen), leading
//     +/- = parent + value (sizes only);
//   - align/valign + x/y offsets; size:; visible: 0/1 literals fold;
//   - texture: path literal, asset const, $white/$invis; utile+uscale
//     (plain-float inversion law: span = 1/uscale); hflip;
//   - color: #hex[аа], "r g b [a]" floats, a few names;
//   - bind texture: (TextureId expression over the snapshot),
//     bind visible: (bool expression),
//     bind width/height: INTERIM LAW — the expression is a 0..1
//     fraction of the parent (typed dim binds land with the
//     instantiate pass); fracf(a, b) is the one builtin.
//
// Ids are the spec's content law (FNV-1a-32 over lowercased,
// '\\'->'/' text; 0 remaps to 1), precomputed here into constexpr
// literals; the emitted kTextures manifest carries {id, path} pairs
// for host registration. With EmitOptions.assetRoot set, every path
// (frame families expanded) must exist on disk.
#include "uic/uic.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
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
  // "%g" of a whole value prints no decimal point; "2f" is not a C++
  // literal — make it "2.0f"
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
  // the standard web colour keywords (rounded), plus 'invisible'
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
  // space-separated floats "r g b [a]"
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

struct Emit {
  const Module &m;
  const EmitOptions &opt;
  std::vector<Diag> &diags;
  std::ostringstream body;
  std::map<std::string, uint32_t> texIds; // path -> id (manifest order kept)
  std::vector<std::string> texOrder;
  std::map<std::string, std::string> assetConsts; // const name -> path
  int nextVar = 0;
  int indent = 1;

  void err(int line, std::string msg) {
    diags.push_back({m.name, line, 0, std::move(msg),
                     Diag::Severity::kError});
  }

  // graceful degradation: the generated tree must LOAD AND RENDER from
  // the first commit (the functional bar) — an unsupported
  // construct is a LOUD warning and a skip comment, never a hard stop
  void skip(int atLine, const std::string &what) {
    diags.push_back({m.name, atLine, 0, "skipped (unsupported yet): " + what,
                     Diag::Severity::kWarning});
    line("// SKIP(unsupported): " + what + " :" + std::to_string(atLine));
  }

  void line(const std::string &s) {
    body << std::string((size_t)indent * 2, ' ') << s << '\n';
  }

  uint32_t texId(const std::string &path, int atLine, bool family) {
    if (!opt.assetRoot.empty() && path[0] != '$') {
      namespace fs = std::filesystem;
      auto exists = [&](std::string p) {
        fs::path full = fs::path(opt.assetRoot) /
                        (p[0] == '/' ? p.substr(1) : p);
        std::error_code ec;
        if (fs::exists(full, ec)) {
          return true;
        }
        if (full.extension() == ".tga") {
          full.replace_extension(".dds");
          return fs::exists(full, ec);
        }
        return false;
      };
      if (family) {
        static const char *const kPieces[9] = {"_tl", "_t", "_tr",
                                               "_l",  "_c", "_r",
                                               "_bl", "_b", "_br"};
        const size_t dot = path.rfind('.');
        for (const char *suffix : kPieces) {
          const std::string piece =
              path.substr(0, dot) + suffix + path.substr(dot);
          if (!exists(piece)) {
            err(atLine, "missing asset: " + piece);
          }
        }
      } else if (!exists(path)) {
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

  // ---- expressions -> C++ ------------------------------------------------

  std::string expr(const Expr &e) {
    switch (e.kind) {
    case Expr::kIdent:
      if (e.text == "snapshot") {
        return "s";
      }
      err(e.line, "unknown identifier '" + e.text +
                      "' in bind (subset: snapshot fields, fracf)");
      return "0";
    case Expr::kNumber:
      return e.text;
    case Expr::kBool:
      return e.text;
    case Expr::kString:
      err(e.line, "string in bind expression (snapshots carry no strings)");
      return "0";
    case Expr::kPath: {
      char buf[16];
      std::snprintf(buf, sizeof buf, "0x%08Xu",
                    texId(e.text, e.line, /*family=*/false));
      return std::string(buf) + " /* " + e.text + " */";
    }
    case Expr::kDim:
    case Expr::kColor:
      err(e.line, "dim/color literals in binds land with the typed-dim "
                  "pass");
      return "0";
    case Expr::kField:
      return expr(*e.args[0]) + "." + e.text;
    case Expr::kIndex:
      return expr(*e.args[0]) + "[" + expr(*e.args[1]) + "]";
    case Expr::kCall: {
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
    }
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

  // dim -> C++ expression; pw/ph are the parent's size vars
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
      raw = "0";
      break;
    }
    if (isSize && d.delta) {
      // parent + value (the value already carries its sign)
      return (horizontal ? pw : ph) + " + (" + raw + ")";
    }
    return raw;
  }

  // ---- widgets -----------------------------------------------------------

  const std::string *attr(const Node &n, std::string_view key) {
    for (const Attr &a : n.attrs) {
      if (a.name == key) {
        return &a.value;
      }
    }
    return nullptr;
  }
  const Bind *bind(const Node &n, std::string_view key) {
    for (const Bind &b : n.binds) {
      if (b.target == key) {
        return &b;
      }
    }
    return nullptr;
  }

  void emitNode(const Node &n, const std::string &px, const std::string &py,
                const std::string &pw, const std::string &ph) {
    if (n.kind != Node::kWidget) {
      skip(n.line, n.kind == Node::kIf ? "if arms" : "widgetstate");
      return;
    }
    if (n.tag != "panel" && n.tag != "image" && n.tag != "frame") {
      skip(n.line, "widget '" + n.tag + "'");
      return;
    }
    if (const std::string *v = attr(n, "visible")) {
      if (*v == "0") {
        line("// " + n.tag + " :" + std::to_string(n.line) +
             " visible: 0 -- folded away");
        return;
      }
    }

    const int id = nextVar++;
    const std::string sfx = std::to_string(id);
    const std::string cw = "w" + sfx, chh = "h" + sfx, cx = "x" + sfx,
                      cy = "y" + sfx;

    line("{ // " + n.tag + " :" + std::to_string(n.line));
    ++indent;

    // size first (alignment needs it)
    std::string wExpr = pw, hExpr = ph; // default 100% of parent
    if (const std::string *sz = attr(n, "size")) {
      const Dim d = parseDim(*sz);
      wExpr = "R(" + dimExpr(d, true, pw, ph, true) + ")";
      hExpr = "R(" + dimExpr(d, false, pw, ph, true) + ")";
    }
    if (const std::string *w = attr(n, "width")) {
      wExpr = "R(" + dimExpr(parseDim(*w), true, pw, ph, true) + ")";
    }
    if (const std::string *h = attr(n, "height")) {
      hExpr = "R(" + dimExpr(parseDim(*h), false, pw, ph, true) + ")";
    }
    if (const Bind *b = bind(n, "width")) {
      // INTERIM LAW: bound width = fraction of the parent
      wExpr = "R(" + pw + " * " + expr(*b->expr) + ")";
    }
    if (const Bind *b = bind(n, "height")) {
      hExpr = "R(" + ph + " * " + expr(*b->expr) + ")";
    }
    line("const float " + cw + " = " + wExpr + ";");
    line("const float " + chh + " = " + hExpr + ";");

    // position: align/valign + offsets
    std::string xOff = "0", yOff = "0";
    if (const std::string *x = attr(n, "x")) {
      xOff = dimExpr(parseDim(*x), true, pw, ph, false);
    }
    if (const std::string *y = attr(n, "y")) {
      yOff = dimExpr(parseDim(*y), false, pw, ph, false);
    }
    const std::string *al = attr(n, "align");
    const std::string *val = attr(n, "valign");
    std::string xBase = px, yBase = py;
    if (al != nullptr && *al == "center") {
      xBase = px + " + R((" + pw + " - " + cw + ") / 2)";
    } else if (al != nullptr && *al == "right") {
      xBase = px + " + " + pw + " - " + cw;
    }
    if (val != nullptr && *val == "center") {
      yBase = py + " + R((" + ph + " - " + chh + ") / 2)";
    } else if (val != nullptr && *val == "bottom") {
      yBase = py + " + " + ph + " - " + chh;
    }
    line("const float " + cx + " = " + xBase + " + R(" + xOff + ");");
    line("const float " + cy + " = " + yBase + " + R(" + yOff + ");");

    const Bind *bv = bind(n, "visible");
    if (bv != nullptr) {
      line("if (" + expr(*bv->expr) + ") {");
      ++indent;
    }

    emitDraw(n, cx, cy, cw, chh, pw, ph);

    for (const NodePtr &c : n.children) {
      emitNode(*c, cx, cy, cw, chh);
    }

    if (bv != nullptr) {
      --indent;
      line("}");
    }
    --indent;
    line("}");
  }

  std::string colorLiteral(const Node &n, float def) {
    float c[4] = {def, def, def, 1};
    if (const std::string *col = attr(n, "color")) {
      if (!parseColor(*col, c)) {
        err(n.line, "unparsable color '" + *col + "'");
      }
    }
    return "ui::Color{" + ftos(c[0]) + ", " + ftos(c[1]) + ", " + ftos(c[2]) +
           ", " + ftos(c[3]) + "}";
  }

  void emitDraw(const Node &n, const std::string &cx, const std::string &cy,
                const std::string &cw, const std::string &chh,
                const std::string &pw, const std::string &ph) {
    const std::string dst =
        "{" + cx + ", " + cy + ", " + cw + ", " + chh + "}";

    if (n.tag == "frame") {
      const std::string *tex = attr(n, "texture");
      if (tex == nullptr) {
        skip(n.line, "textureless frame (the white-pieces law lands with "
                     "the full frame pass)");
        return;
      }
      const std::string *bt = attr(n, "borderthickness");
      // the dialect law: borderthickness resolves against the PARENT
      const std::string btE =
          "R(" +
          dimExpr(bt != nullptr ? parseDim(*bt) : parseDim("0"), false, pw,
                  ph, false) +
          ")";
      const std::string col = colorLiteral(n, 1);
      const size_t dot = tex->rfind('.');
      static const struct {
        const char *suffix;
        int gx, gy; // grid cell
      } kPieces[9] = {{"_tl", 0, 0}, {"_t", 1, 0},  {"_tr", 2, 0},
                      {"_l", 0, 1},  {"_c", 1, 1},  {"_r", 2, 1},
                      {"_bl", 0, 2}, {"_b", 1, 2},  {"_br", 2, 2}};
      // the base name is a FAMILY name, not a file — only the nine
      // pieces exist, validate + manifest below, one by one
      line("const float bt = std::min(" + btE + ", std::min(" + cw +
           ", " + chh + ") / 2);");
      line("const float cwm = " + cw + " - 2 * bt, chm = " + chh +
           " - 2 * bt;");
      for (const auto &p : kPieces) {
        const std::string piece =
            tex->substr(0, dot) + p.suffix + tex->substr(dot);
        char idbuf[16];
        std::snprintf(idbuf, sizeof idbuf, "0x%08Xu",
                      texId(piece, n.line, /*family=*/false));
        const std::string x =
            p.gx == 0 ? cx : (p.gx == 1 ? cx + " + bt" : cx + " + bt + cwm");
        const std::string y =
            p.gy == 0 ? cy : (p.gy == 1 ? cy + " + bt" : cy + " + bt + chm");
        const std::string w = p.gx == 1 ? "cwm" : "bt";
        const std::string h = p.gy == 1 ? "chm" : "bt";
        line("sink.image({" + x + ", " + y + ", " + w + ", " + h + "}, " +
             idbuf + " /* " + piece + " */, {0, 0, 1, 1}, " + col +
             ", 0, 0, ui::kNoClip);");
      }
      return;
    }

    // image / textured panel / colored panel
    const std::string *tex = attr(n, "texture");
    const Bind *texBind = bind(n, "texture");
    if (n.tag == "image" && tex == nullptr && texBind == nullptr) {
      // texture was deferred (a TODO'd interpolation) — degrade
      skip(n.line, "image without a resolvable texture");
      return;
    }
    if (tex != nullptr && *tex == "$invis") {
      return; // the dialect law: $invis draws nothing
    }
    if (texBind != nullptr || (tex != nullptr && *tex != "$white")) {
      std::string idExpr;
      if (texBind != nullptr) {
        idExpr = expr(*texBind->expr);
      } else {
        std::string path = *tex;
        auto it = assetConsts.find(path);
        if (it != assetConsts.end()) {
          path = it->second;
        }
        char idbuf[16];
        std::snprintf(idbuf, sizeof idbuf, "0x%08Xu",
                      texId(path, n.line, /*family=*/false));
        idExpr = std::string(idbuf) + " /* " + path + " */";
      }
      // uv: hflip and the uscale inversion law (span = 1 / uscale)
      std::string uv = "{0, 0, 1, 1}";
      std::string flags = "0";
      if (const std::string *us = attr(n, "uscale")) {
        const float span = 1.f / std::strtof(us->c_str(), nullptr);
        uv = "{0, 0, " + ftos(span) + ", 1}";
      }
      if (const std::string *hf = attr(n, "hflip"); hf != nullptr &&
                                                    *hf == "1") {
        uv = "{1, 0, -1, 1}";
      }
      if (const std::string *ut = attr(n, "utile"); ut != nullptr &&
                                                    *ut == "1") {
        flags = "ui::kTileU";
      }
      line("sink.image(" + dst + ", " + idExpr + ", " + uv + ", " +
           colorLiteral(n, 1) + ", " + flags + ", 0, ui::kNoClip);");
      return;
    }
    if (attr(n, "color") != nullptr || (tex != nullptr && *tex == "$white")) {
      line("sink.quad(" + dst + ", " + colorLiteral(n, 1) +
           ", 0, ui::kNoClip);");
    }
    // colorless textureless panel: container only (NO_DRAW)
  }
};

} // namespace

std::string emitPanelHeader(const Module &m, const EmitOptions &opt,
                            std::vector<Diag> &diags) {
  Emit e{m, opt, diags, {}, {}, {}, {}, 0, 1};
  for (const ConstDecl &c : m.consts) {
    if (c.type == "asset" || (!c.rawValue.empty() && c.rawValue[0] == '/')) {
      e.assetConsts[c.name] = c.rawValue;
    }
  }
  for (const NodePtr &n : m.roots) {
    e.emitNode(*n, "0.0f", "0.0f", "W", "H");
  }

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
     << "// Absolute-subset panel codegen (see src/uic/emit.cpp for the\n"
     << "// subset's laws). Depends on silhouette + the schema header "
        "only.\n"
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
     << "} // namespace gen_detail\n\n"
     << "// host registration manifest: load each path, register under "
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
     << "(const UiSnapshot &s, float screenW, float screenH, Sink &sink) "
        "{\n"
     << "  using gen_detail::R;\n"
     << "  (void)s;\n"
     << "  const float W = screenW, H = screenH;\n"
     << "  (void)W;\n"
     << e.body.str() << "}\n\n"
     << "} // namespace " << opt.ns << "\n";
  return os.str();
}

} // namespace uic
