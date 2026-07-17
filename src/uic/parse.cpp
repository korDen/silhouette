// uic parser — recursive descent over uic::Lexer (one-token lookahead,
// lexer-state snapshots where the grammar needs a second probe), error
// recovery at statement boundaries, plus the deterministic AST dump the
// tests compare against. Grammar: docs/ui-language.md.
#include "uic/lex.hpp"
#include "uic/uic.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace uic {

namespace {

constexpr int kMaxDiags = 100;

class Parser {
public:
  Parser(std::string_view src, std::string_view file,
         std::vector<Diag> &diags)
      : lex_(src), file_(file), diags_(diags) {}

  Module run() {
    Module m;
    m.name = std::string(file_);
    const size_t slash = m.name.find_last_of("/\\");
    if (slash != std::string::npos) {
      m.name.erase(0, slash + 1);
    }
    const size_t dot = m.name.rfind('.');
    if (dot != std::string::npos) {
      m.name.erase(dot);
    }
    while (peek().kind != Tok::kEnd) {
      drainComments(pendingLead_, nullptr, 0);
      if (!parseItem(m)) {
        sync();
      }
    }
    drainComments(m.tail, nullptr, 0);
    return m;
  }

private:
  // ---- token plumbing ----------------------------------------------------

  struct State {
    Lexer lex;
    Token la;
    bool hasLa;
  };
  State save() { return {lex_, la_, hasLa_}; }
  void restore(const State &s) {
    lex_ = s.lex;
    la_ = s.la;
    hasLa_ = s.hasLa;
  }

  const Token &peek() {
    if (!hasLa_) {
      la_ = lex_.next();
      hasLa_ = true;
    }
    return la_;
  }
  Token take() {
    peek();
    hasLa_ = false;
    return la_;
  }
  bool at(Tok k) { return peek().kind == k; }
  bool atIdent(std::string_view word) {
    return peek().kind == Tok::kIdent && peek().text == word;
  }
  bool eat(Tok k) {
    if (at(k)) {
      take();
      return true;
    }
    return false;
  }

  // Is the token AFTER the current lookahead a ':'? (lexer-state probe;
  // the keyword/attribute disambiguation needs exactly this one peek.)
  bool secondIsColon() {
    peek();
    Lexer probe = lex_;
    return probe.next().kind == Tok::kColon;
  }

  void error(const Token &t, std::string msg) {
    if ((int)diags_.size() >= kMaxDiags) {
      return;
    }
    diags_.push_back({std::string(file_), t.line, t.col, std::move(msg)});
  }

  bool expect(Tok k, const char *what) {
    if (eat(k)) {
      return true;
    }
    error(peek(), std::string("expected ") + what);
    return false;
  }

  Token expectIdent(const char *what) {
    if (at(Tok::kIdent)) {
      return take();
    }
    error(peek(), std::string("expected ") + what);
    return Token{};
  }

  // statement-boundary recovery: consume through the next ';', or stop
  // before '}' / end so the enclosing body can close
  void sync() {
    for (;;) {
      const Token &t = peek();
      if (t.kind == Tok::kEnd || t.kind == Tok::kRBrace) {
        return;
      }
      if (take().kind == Tok::kSemi) {
        return;
      }
    }
  }

  // raw value: lookahead must be empty (the caller consumed ':' / '=')
  std::string raw() {
    // un-lex is impossible; the grammar guarantees no pending token here
    std::string v(lex_.rawValue());
    if (!lex_.rawTerminated()) {
      Token here;
      here.line = lex_.line();
      error(here, "missing ';' after value");
    }
    return v;
  }

  // ---- comment trivia -------------------------------------------------------

  void pumpComments() {
    for (LexComment &c : lex_.takeComments()) {
      commentQ_.push_back(std::move(c));
    }
  }

  // move captured comments into `lead`; the end-of-line comment of the
  // enclosing construct's open line (if any) becomes its trail
  void drainComments(std::vector<std::string> &lead, std::string *trail,
                     int trailLine) {
    pumpComments();
    for (LexComment &c : commentQ_) {
      if (!c.ownLine && trail != nullptr && trail->empty() &&
          c.line == trailLine) {
        *trail = std::move(c.text);
      } else {
        lead.push_back(std::move(c.text));
      }
    }
    commentQ_.clear();
  }

  // the end-of-line comment sitting on `line` (a construct's CLOSING
  // line — `chip { ... } // :293`), pulled selectively; own-line
  // comments stay queued for the next drain
  std::string takeTrail(int line) {
    peek(); // force the next token so any same-line comment is captured
    pumpComments();
    for (auto it = commentQ_.begin(); it != commentQ_.end(); ++it) {
      if (!it->ownLine && it->line == line) {
        std::string s = std::move(it->text);
        commentQ_.erase(it);
        return s;
      }
    }
    return std::string();
  }

  // ---- module items --------------------------------------------------------

  bool parseItem(Module &m) {
    const Token &t = peek();
    if (t.kind != Tok::kIdent) {
      error(t, "expected a module item");
      take(); // consume the stray token — progress is the loop's invariant
      return false;
    }
    if (t.text == "import") {
      return parseImport(m);
    }
    if (t.text == "const") {
      return parseConst(m);
    }
    if (t.text == "struct") {
      return parseStruct(m);
    }
    if (t.text == "enum") {
      return parseEnum(m);
    }
    if (t.text == "style") {
      return parseStyle(m);
    }
    if (t.text == "template") {
      return parseTemplate(m);
    }
    if (t.text == "fn") {
      return parseFn(m, /*isNative=*/false);
    }
    if (t.text == "native") {
      take();
      if (!atIdent("fn")) {
        error(peek(), "expected 'fn' after 'native'");
        return false;
      }
      return parseFn(m, /*isNative=*/true);
    }
    // a top-level widget tree
    std::vector<std::string> lead = std::move(pendingLead_);
    pendingLead_.clear();
    NodePtr n = parseNode();
    if (n == nullptr) {
      return false;
    }
    n->lead = std::move(lead);
    const int closeLine = lex_.line();
    if (n->kind != Node::kIf && n->trail.empty()) {
      n->trail = takeTrail(closeLine);
    }
    m.roots.push_back(std::move(n));
    return true;
  }

  bool parseImport(Module &m) {
    Import imp;
    pendingLead_.clear(); // imports/schema decls carry no trivia (yet)
    imp.line = take().line; // 'import'
    if (!expect(Tok::kLBrace, "'{'")) {
      return false;
    }
    for (;;) {
      const Token n = expectIdent("imported name");
      if (n.kind != Tok::kIdent) {
        return false;
      }
      imp.names.emplace_back(n.text);
      if (eat(Tok::kComma)) {
        continue;
      }
      break;
    }
    if (!expect(Tok::kRBrace, "'}'")) {
      return false;
    }
    if (!atIdent("from")) {
      error(peek(), "expected 'from'");
      return false;
    }
    take();
    if (!at(Tok::kString)) {
      error(peek(), "expected module path string");
      return false;
    }
    imp.from = std::string(take().text);
    expect(Tok::kSemi, "';'");
    m.imports.push_back(std::move(imp));
    return true;
  }

  bool parseConst(Module &m) {
    ConstDecl c;
    c.lead = std::move(pendingLead_);
    pendingLead_.clear();
    c.line = take().line; // 'const'
    const Token n = expectIdent("const name");
    if (n.kind != Tok::kIdent) {
      return false;
    }
    c.name = std::string(n.text);
    if (eat(Tok::kColon)) {
      const Token ty = expectIdent("type");
      if (ty.kind != Tok::kIdent) {
        return false;
      }
      c.type = std::string(ty.text);
    }
    if (!expect(Tok::kEq, "'='")) {
      return false;
    }
    // `Type { field: value; ... }` initializer, or a raw literal. The
    // probe may lex ahead (peek fills the lookahead even on a miss);
    // EVERY non-initializer outcome must restore before raw() reads
    // from the lexer position, or the probed token leaks.
    const State s = save();
    if (at(Tok::kIdent)) {
      const Token ty = take();
      if (at(Tok::kLBrace)) {
        take();
        c.initType = std::string(ty.text);
        while (!at(Tok::kRBrace) && !at(Tok::kEnd)) {
          Attr a;
          const Token f = expectIdent("field name");
          if (f.kind != Tok::kIdent || !expect(Tok::kColon, "':'")) {
            sync();
            continue;
          }
          a.name = std::string(f.text);
          a.line = f.line;
          a.value = raw();
          c.initFields.push_back(std::move(a));
        }
        expect(Tok::kRBrace, "'}'");
        m.consts.push_back(std::move(c));
        return true;
      }
    }
    restore(s); // undo the probe on EVERY non-initializer outcome
    c.rawValue = raw();
    m.consts.push_back(std::move(c));
    return true;
  }

  bool parseStruct(Module &m) {
    pendingLead_.clear();
    StructDecl s;
    s.line = take().line; // 'struct'
    const Token n = expectIdent("struct name");
    if (n.kind != Tok::kIdent || !expect(Tok::kLBrace, "'{'")) {
      return false;
    }
    s.name = std::string(n.text);
    while (!at(Tok::kRBrace) && !at(Tok::kEnd)) {
      StructField f;
      const Token fn = expectIdent("field name");
      if (fn.kind != Tok::kIdent || !expect(Tok::kColon, "':'")) {
        sync();
        continue;
      }
      f.name = std::string(fn.text);
      f.line = fn.line;
      const Token ty = expectIdent("field type");
      if (ty.kind != Tok::kIdent) {
        sync();
        continue;
      }
      f.type = std::string(ty.text);
      if (eat(Tok::kLBracket)) { // `type[N]` fixed array
        if (!at(Tok::kNumber)) {
          error(peek(), "expected array length");
          sync();
          continue;
        }
        f.arrayLen = std::atoi(std::string(take().text).c_str());
        if (!expect(Tok::kRBracket, "']'")) {
          sync();
          continue;
        }
      }
      if (eat(Tok::kEq)) {
        f.defaultValue = raw();
        f.hasDefault = true;
      } else {
        expect(Tok::kSemi, "';'");
      }
      s.fields.push_back(std::move(f));
    }
    expect(Tok::kRBrace, "'}'");
    m.structs.push_back(std::move(s));
    return true;
  }

  bool parseEnum(Module &m) {
    pendingLead_.clear();
    EnumDecl e;
    e.line = take().line; // 'enum'
    const Token n = expectIdent("enum name");
    if (n.kind != Tok::kIdent || !expect(Tok::kLBrace, "'{'")) {
      return false;
    }
    e.name = std::string(n.text);
    while (!at(Tok::kRBrace) && !at(Tok::kEnd)) {
      EnumEntry entry;
      const Token en = expectIdent("enum entry");
      if (en.kind != Tok::kIdent) {
        sync();
        continue;
      }
      entry.name = std::string(en.text);
      if (eat(Tok::kEq)) {
        if (!at(Tok::kNumber)) {
          error(peek(), "expected enum value");
          sync();
          continue;
        }
        entry.value = std::string(take().text);
        entry.hasValue = true;
      }
      e.entries.push_back(std::move(entry));
      if (!eat(Tok::kComma)) {
        break; // trailing comma optional
      }
    }
    expect(Tok::kRBrace, "'}'");
    m.enums.push_back(std::move(e));
    return true;
  }

  bool parseStyle(Module &m) {
    StyleDecl s;
    s.lead = std::move(pendingLead_);
    pendingLead_.clear();
    s.line = take().line; // 'style'
    const Token n = expectIdent("style name");
    if (n.kind != Tok::kIdent) {
      return false;
    }
    const int openLine = peek().line;
    if (!expect(Tok::kLBrace, "'{'")) {
      return false;
    }
    s.name = std::string(n.text);
    while (!at(Tok::kRBrace) && !at(Tok::kEnd)) {
      Attr a;
      drainComments(a.lead, &s.trail, openLine);
      const Token f = expectIdent("attribute name");
      if (f.kind != Tok::kIdent || !expect(Tok::kColon, "':'")) {
        sync();
        continue;
      }
      a.name = std::string(f.text);
      a.line = f.line;
      a.value = raw();
      s.attrs.push_back(std::move(a));
    }
    expect(Tok::kRBrace, "'}'");
    drainComments(s.bodyTail, &s.trail, openLine);
    m.styles.push_back(std::move(s));
    return true;
  }

  bool parseFn(Module &m, bool isNative) {
    pendingLead_.clear();
    FnDecl f;
    f.isNative = isNative;
    f.line = take().line; // 'fn'
    const Token n = expectIdent("fn name");
    if (n.kind != Tok::kIdent || !expect(Tok::kLParen, "'('")) {
      return false;
    }
    f.name = std::string(n.text);
    if (!at(Tok::kRParen)) {
      for (;;) {
        FnParam p;
        const Token pn = expectIdent("parameter name");
        if (pn.kind != Tok::kIdent || !expect(Tok::kColon, "':'")) {
          return false;
        }
        p.name = std::string(pn.text);
        const Token ty = expectIdent("parameter type");
        if (ty.kind != Tok::kIdent) {
          return false;
        }
        p.type = std::string(ty.text);
        f.params.push_back(std::move(p));
        if (eat(Tok::kComma)) {
          continue;
        }
        break;
      }
    }
    if (!expect(Tok::kRParen, "')'") || !expect(Tok::kArrow, "'->'")) {
      return false;
    }
    const Token ty = expectIdent("return type");
    if (ty.kind != Tok::kIdent) {
      return false;
    }
    f.returnType = std::string(ty.text);
    if (isNative) {
      // lookahead must be empty before the raw '{{{' scan
      if (hasLa_) {
        error(peek(), "expected '{{{' after native fn signature");
        return false;
      }
      std::string_view body;
      if (!lex_.nativeBody(&body)) {
        Token here;
        here.line = lex_.line();
        error(here, "expected '{{{ ... }}}' native body");
        return false;
      }
      f.native = std::string(body);
    } else {
      if (!expect(Tok::kEq, "'='")) {
        return false;
      }
      f.body = parseExpr();
      if (f.body == nullptr) {
        return false;
      }
      expect(Tok::kSemi, "';'");
    }
    m.fns.push_back(std::move(f));
    return true;
  }

  bool parseTemplate(Module &m) {
    TemplateDecl t;
    t.lead = std::move(pendingLead_);
    pendingLead_.clear();
    t.line = take().line; // 'template'
    const Token n = expectIdent("template name");
    if (n.kind != Tok::kIdent) {
      return false;
    }
    const int openLine = peek().line;
    if (!expect(Tok::kLBrace, "'{'")) {
      return false;
    }
    t.name = std::string(n.text);
    while (!at(Tok::kRBrace) && !at(Tok::kEnd)) {
      std::vector<std::string> lead;
      drainComments(lead, &t.trail, openLine);
      if (atIdent("in")) {
        InParam p;
        p.lead = std::move(lead);
        p.line = take().line;
        const Token pn = expectIdent("param name");
        if (pn.kind != Tok::kIdent || !expect(Tok::kColon, "':'")) {
          sync();
          continue;
        }
        p.name = std::string(pn.text);
        const Token ty = expectIdent("param type");
        if (ty.kind != Tok::kIdent) {
          sync();
          continue;
        }
        // `in x: a | b | c` — an inline enum (a closed value set); the
        // first ident is the first value, and `|` chains the rest
        if (at(Tok::kPipe)) {
          p.enumValues.push_back(std::string(ty.text));
          while (eat(Tok::kPipe)) {
            const Token v = expectIdent("enum value");
            if (v.kind != Tok::kIdent) {
              sync();
              break;
            }
            p.enumValues.push_back(std::string(v.text));
          }
        } else {
          p.type = std::string(ty.text);
        }
        if (eat(Tok::kEq)) {
          p.defaultValue = raw();
          p.hasDefault = true;
        } else {
          expect(Tok::kSemi, "';'");
        }
        t.ins.push_back(std::move(p));
        continue;
      }
      NodePtr node = parseNode();
      if (node == nullptr) {
        sync();
        continue;
      }
      node->lead = std::move(lead);
      const int closeLine = lex_.line();
      if (node->kind != Node::kIf && node->trail.empty()) {
        node->trail = takeTrail(closeLine);
      }
      t.body.push_back(std::move(node));
    }
    expect(Tok::kRBrace, "'}'");
    drainComments(t.bodyTail, &t.trail, openLine);
    m.templates.push_back(std::move(t));
    return true;
  }

  // ---- widgets --------------------------------------------------------------

  NodePtr parseNode() {
    if (atIdent("if")) {
      return parseIf();
    }
    if (atIdent("widgetstate")) {
      auto n = std::make_unique<Node>();
      n->kind = Node::kWidgetState;
      n->line = take().line;
      const Token sn = expectIdent("state name");
      if (sn.kind != Tok::kIdent) {
        return nullptr;
      }
      n->tag = std::string(sn.text);
      if (!parseBody(*n)) {
        return nullptr;
      }
      return n;
    }
    if (!at(Tok::kIdent)) {
      error(peek(), "expected a widget");
      return nullptr;
    }
    auto n = std::make_unique<Node>();
    n->kind = Node::kWidget;
    const Token tag = take();
    n->tag = std::string(tag.text);
    n->line = tag.line;
    if (!parseBody(*n)) {
      return nullptr;
    }
    return n;
  }

  bool parseBody(Node &n) {
    const int openLine = peek().line;
    if (!expect(Tok::kLBrace, "'{'")) {
      return false;
    }
    while (!at(Tok::kRBrace) && !at(Tok::kEnd)) {
      std::vector<std::string> lead;
      drainComments(lead, &n.trail, openLine);
      // `bind`/`action` are keywords only when a target ident follows;
      // `bind: value;` is a plain attribute NAMED bind (ported trees
      // carry it — the source material's key-binding attribute)
      if (atIdent("bind") && !secondIsColon()) {
        Bind b;
        b.lead = std::move(lead);
        b.line = take().line;
        const Token bn = expectIdent("bind target");
        if (bn.kind != Tok::kIdent || !expect(Tok::kColon, "':'")) {
          sync();
          continue;
        }
        b.target = std::string(bn.text);
        b.expr = parseExpr();
        if (b.expr == nullptr) {
          sync();
          continue;
        }
        expect(Tok::kSemi, "';'");
        n.binds.push_back(std::move(b));
        continue;
      }
      if (atIdent("action") && !secondIsColon()) {
        Action a;
        a.lead = std::move(lead);
        a.line = take().line;
        const Token an = expectIdent("action event");
        if (an.kind != Tok::kIdent || !expect(Tok::kColon, "':'")) {
          sync();
          continue;
        }
        a.event = std::string(an.text);
        a.expr = parseExpr();
        if (a.expr == nullptr) {
          sync();
          continue;
        }
        expect(Tok::kSemi, "';'");
        n.actions.push_back(std::move(a));
        continue;
      }
      if (atIdent("if") || atIdent("widgetstate")) {
        NodePtr child = parseNode();
        if (child == nullptr) {
          sync();
          continue;
        }
        child->lead = std::move(lead);
        const int closeLine = lex_.line();
        if (child->kind != Node::kIf && child->trail.empty()) {
          child->trail = takeTrail(closeLine);
        }
        n.children.push_back(std::move(child));
        continue;
      }
      if (!at(Tok::kIdent)) {
        error(peek(), "expected an attribute, bind, action, or widget");
        sync();
        continue;
      }
      // `ident {` = child widget; `ident :` = plain attribute
      const Token head = take();
      if (at(Tok::kLBrace)) {
        auto child = std::make_unique<Node>();
        child->kind = Node::kWidget;
        child->tag = std::string(head.text);
        child->line = head.line;
        child->lead = std::move(lead);
        if (!parseBody(*child)) {
          sync();
          continue;
        }
        const int closeLine = lex_.line();
        if (child->trail.empty()) {
          child->trail = takeTrail(closeLine);
        }
        n.children.push_back(std::move(child));
        continue;
      }
      if (!eat(Tok::kColon)) {
        error(peek(), "expected ':' or '{' after '" +
                          std::string(head.text) + "'");
        sync();
        continue;
      }
      Attr a;
      a.name = std::string(head.text);
      a.line = head.line;
      a.lead = std::move(lead);
      // `attr: match scrutinee { value: result; ... }` — the value is
      // chosen by an enum param instead of being a single literal.
      // Probing for `match` fills the lookahead, which raw() requires
      // empty, so undo the probe on the plain-value path.
      const State beforeValue = save();
      if (atIdent("match")) {
        take();
        const Token sc = expectIdent("match scrutinee");
        if (sc.kind != Tok::kIdent || !expect(Tok::kLBrace, "'{'")) {
          sync();
          continue;
        }
        a.matchOn = std::string(sc.text);
        while (!at(Tok::kRBrace) && !at(Tok::kEnd)) {
          const Token v = expectIdent("match value");
          if (v.kind != Tok::kIdent || !expect(Tok::kColon, "':'")) {
            sync();
            continue;
          }
          MatchArm arm;
          arm.value = std::string(v.text);
          arm.result = raw();
          a.arms.push_back(std::move(arm));
        }
        expect(Tok::kRBrace, "'}'");
      } else {
        restore(beforeValue); // the probe read a token; give it back
        a.value = raw();
      }
      n.attrs.push_back(std::move(a));
    }
    const bool ok = expect(Tok::kRBrace, "'}'");
    drainComments(n.bodyTail, &n.trail, openLine);
    return ok;
  }

  NodePtr parseIf() {
    auto n = std::make_unique<Node>();
    n->kind = Node::kIf;
    n->line = peek().line;
    for (;;) {
      IfArm arm;
      arm.line = take().line; // 'if' (or the 'else' handled below)
      if (!expect(Tok::kLParen, "'('")) {
        return nullptr;
      }
      arm.cond = parseExpr();
      if (arm.cond == nullptr || !expect(Tok::kRParen, "')'")) {
        return nullptr;
      }
      if (!parseArmBody(arm)) {
        return nullptr;
      }
      n->arms.push_back(std::move(arm));
      if (!atIdent("else")) {
        return n;
      }
      take(); // 'else'
      if (atIdent("if")) {
        continue; // else-if: loop consumes 'if'
      }
      IfArm tail;
      tail.line = peek().line;
      if (!parseArmBody(tail)) {
        return nullptr;
      }
      n->arms.push_back(std::move(tail));
      return n;
    }
  }

  bool parseArmBody(IfArm &arm) {
    if (!expect(Tok::kLBrace, "'{'")) {
      return false;
    }
    while (!at(Tok::kRBrace) && !at(Tok::kEnd)) {
      std::vector<std::string> lead;
      drainComments(lead, nullptr, 0);
      NodePtr child = parseNode();
      if (child == nullptr) {
        sync();
        continue;
      }
      child->lead = std::move(lead);
      const int closeLine = lex_.line();
      if (child->kind != Node::kIf && child->trail.empty()) {
        child->trail = takeTrail(closeLine);
      }
      arm.body.push_back(std::move(child));
    }
    return expect(Tok::kRBrace, "'}'");
  }

  // ---- expressions ------------------------------------------------------------

  ExprPtr parseExpr() { return parseTernary(); }

  ExprPtr parseTernary() {
    ExprPtr c = parseOr();
    if (c == nullptr || !at(Tok::kQuestion)) {
      return c;
    }
    take();
    auto e = std::make_unique<Expr>();
    e->kind = Expr::kTernary;
    e->line = c->line;
    ExprPtr t = parseExpr();
    if (t == nullptr || !expect(Tok::kColon, "':'")) {
      return nullptr;
    }
    ExprPtr f = parseTernary();
    if (f == nullptr) {
      return nullptr;
    }
    e->args.push_back(std::move(c));
    e->args.push_back(std::move(t));
    e->args.push_back(std::move(f));
    return e;
  }

  ExprPtr parseBinaryChain(ExprPtr (Parser::*sub)(),
                           std::initializer_list<Tok> ops) {
    ExprPtr lhs = (this->*sub)();
    if (lhs == nullptr) {
      return nullptr;
    }
    for (;;) {
      const Tok k = peek().kind;
      if (std::find(ops.begin(), ops.end(), k) == ops.end()) {
        return lhs;
      }
      const Token op = take();
      ExprPtr rhs = (this->*sub)();
      if (rhs == nullptr) {
        return nullptr;
      }
      auto e = std::make_unique<Expr>();
      e->kind = Expr::kBinary;
      e->text = std::string(op.text);
      e->line = op.line;
      e->args.push_back(std::move(lhs));
      e->args.push_back(std::move(rhs));
      lhs = std::move(e);
    }
  }

  ExprPtr parseOr() {
    return parseBinaryChain(&Parser::parseAnd, {Tok::kOrOr});
  }
  ExprPtr parseAnd() {
    return parseBinaryChain(&Parser::parseEquality, {Tok::kAndAnd});
  }
  ExprPtr parseEquality() {
    return parseBinaryChain(&Parser::parseRelational,
                            {Tok::kEqEq, Tok::kNe});
  }
  ExprPtr parseRelational() {
    return parseBinaryChain(&Parser::parseAdditive,
                            {Tok::kLt, Tok::kGt, Tok::kLe, Tok::kGe});
  }
  ExprPtr parseAdditive() {
    return parseBinaryChain(&Parser::parseMultiplicative,
                            {Tok::kPlus, Tok::kMinus});
  }
  ExprPtr parseMultiplicative() {
    return parseBinaryChain(&Parser::parseUnary,
                            {Tok::kStar, Tok::kSlash});
  }

  ExprPtr parseUnary() {
    if (at(Tok::kBang) || at(Tok::kMinus)) {
      const Token op = take();
      ExprPtr sub = parseUnary();
      if (sub == nullptr) {
        return nullptr;
      }
      auto e = std::make_unique<Expr>();
      e->kind = Expr::kUnary;
      e->text = std::string(op.text);
      e->line = op.line;
      e->args.push_back(std::move(sub));
      return e;
    }
    return parsePostfix();
  }

  ExprPtr parsePostfix() {
    ExprPtr e = parsePrimary();
    if (e == nullptr) {
      return nullptr;
    }
    for (;;) {
      if (eat(Tok::kDot)) {
        const Token f = expectIdent("field name");
        if (f.kind != Tok::kIdent) {
          return nullptr;
        }
        auto fe = std::make_unique<Expr>();
        fe->kind = Expr::kField;
        fe->text = std::string(f.text);
        fe->line = f.line;
        fe->args.push_back(std::move(e));
        e = std::move(fe);
        continue;
      }
      if (at(Tok::kLBracket)) {
        const Token open = take();
        ExprPtr ix = parseExpr();
        if (ix == nullptr || !expect(Tok::kRBracket, "']'")) {
          return nullptr;
        }
        auto ie = std::make_unique<Expr>();
        ie->kind = Expr::kIndex;
        ie->line = open.line;
        ie->args.push_back(std::move(e));
        ie->args.push_back(std::move(ix));
        e = std::move(ie);
        continue;
      }
      if (at(Tok::kLParen)) {
        const Token open = take();
        auto ce = std::make_unique<Expr>();
        ce->kind = Expr::kCall;
        ce->line = open.line;
        ce->args.push_back(std::move(e));
        if (!at(Tok::kRParen)) {
          for (;;) {
            ExprPtr arg = parseExpr();
            if (arg == nullptr) {
              return nullptr;
            }
            ce->args.push_back(std::move(arg));
            if (eat(Tok::kComma)) {
              continue;
            }
            break;
          }
        }
        if (!expect(Tok::kRParen, "')'")) {
          return nullptr;
        }
        e = std::move(ce);
        continue;
      }
      return e;
    }
  }

  ExprPtr parsePrimary() {
    const Token &t = peek();
    auto lit = [&](Expr::Kind k) {
      const Token v = take();
      auto e = std::make_unique<Expr>();
      e->kind = k;
      e->text = std::string(v.text);
      e->line = v.line;
      return e;
    };
    switch (t.kind) {
    case Tok::kNumber:
      return lit(Expr::kNumber);
    case Tok::kDim:
      return lit(Expr::kDim);
    case Tok::kColor:
      return lit(Expr::kColor);
    case Tok::kPath:
      return lit(Expr::kPath);
    case Tok::kString:
      return lit(Expr::kString);
    case Tok::kIdent:
      if (t.text == "true" || t.text == "false") {
        return lit(Expr::kBool);
      }
      return lit(Expr::kIdent);
    case Tok::kLParen: {
      take();
      ExprPtr e = parseExpr();
      if (e == nullptr || !expect(Tok::kRParen, "')'")) {
        return nullptr;
      }
      return e;
    }
    default:
      error(t, "expected an expression");
      return nullptr;
    }
  }

  Lexer lex_;
  Token la_;
  bool hasLa_ = false;
  std::string_view file_;
  std::vector<Diag> &diags_;
  std::vector<std::string> pendingLead_; // module-level, drained by run()
  std::vector<LexComment> commentQ_;     // pumped from the lexer
};

// ---- dump ---------------------------------------------------------------

void dumpExpr(const Expr &e, std::ostream &os) {
  switch (e.kind) {
  case Expr::kIdent:
  case Expr::kNumber:
  case Expr::kDim:
  case Expr::kColor:
  case Expr::kPath:
  case Expr::kBool:
    os << e.text;
    return;
  case Expr::kString:
    os << '"' << e.text << '"';
    return;
  case Expr::kField:
    os << "(. ";
    dumpExpr(*e.args[0], os);
    os << ' ' << e.text << ')';
    return;
  case Expr::kIndex:
    os << "([] ";
    dumpExpr(*e.args[0], os);
    os << ' ';
    dumpExpr(*e.args[1], os);
    os << ')';
    return;
  case Expr::kCall:
    os << "(call";
    for (const ExprPtr &a : e.args) {
      os << ' ';
      dumpExpr(*a, os);
    }
    os << ')';
    return;
  case Expr::kUnary:
    os << '(' << e.text << ' ';
    dumpExpr(*e.args[0], os);
    os << ')';
    return;
  case Expr::kBinary:
    os << '(' << e.text << ' ';
    dumpExpr(*e.args[0], os);
    os << ' ';
    dumpExpr(*e.args[1], os);
    os << ')';
    return;
  case Expr::kTernary:
    os << "(?: ";
    dumpExpr(*e.args[0], os);
    os << ' ';
    dumpExpr(*e.args[1], os);
    os << ' ';
    dumpExpr(*e.args[2], os);
    os << ')';
    return;
  }
}

void dumpNode(const Node &n, int depth, std::ostream &os) {
  const std::string pad((size_t)depth * 2, ' ');
  if (n.kind == Node::kIf) {
    bool first = true;
    for (const IfArm &arm : n.arms) {
      os << pad << (first ? "if" : (arm.cond != nullptr ? "elif" : "else"));
      if (arm.cond != nullptr) {
        os << ' ';
        dumpExpr(*arm.cond, os);
      }
      os << '\n';
      for (const NodePtr &c : arm.body) {
        dumpNode(*c, depth + 1, os);
      }
      first = false;
    }
    return;
  }
  os << pad << (n.kind == Node::kWidgetState ? "widgetstate " : "widget ")
     << n.tag << '\n';
  for (const Attr &a : n.attrs) {
    if (a.isMatch()) {
      os << pad << "  attr " << a.name << " match " << a.matchOn << " {";
      for (const MatchArm &arm : a.arms) {
        os << ' ' << arm.value << "=`" << arm.result << "`;";
      }
      os << " }\n";
    } else {
      os << pad << "  attr " << a.name << "=`" << a.value << "`\n";
    }
  }
  for (const Bind &b : n.binds) {
    os << pad << "  bind " << b.target << "= ";
    dumpExpr(*b.expr, os);
    os << '\n';
  }
  for (const Action &a : n.actions) {
    os << pad << "  action " << a.event << "= ";
    dumpExpr(*a.expr, os);
    os << '\n';
  }
  for (const NodePtr &c : n.children) {
    dumpNode(*c, depth + 1, os);
  }
}

} // namespace

Module parseModule(std::string_view source, std::string_view fileName,
                   std::vector<Diag> &diags) {
  return Parser(source, fileName, diags).run();
}

std::string dumpModule(const Module &m) {
  std::ostringstream os;
  os << "module " << m.name << '\n';
  for (const Import &i : m.imports) {
    os << "import {";
    for (size_t k = 0; k < i.names.size(); ++k) {
      os << (k != 0 ? ", " : "") << i.names[k];
    }
    os << "} from \"" << i.from << "\"\n";
  }
  for (const ConstDecl &c : m.consts) {
    os << "const " << c.name;
    if (!c.type.empty()) {
      os << ": " << c.type;
    }
    if (!c.initType.empty()) {
      os << " = " << c.initType << "{";
      for (const Attr &f : c.initFields) {
        os << ' ' << f.name << "=`" << f.value << "`;";
      }
      os << " }\n";
    } else {
      os << " = `" << c.rawValue << "`\n";
    }
  }
  for (const StructDecl &s : m.structs) {
    os << "struct " << s.name << " {";
    for (const StructField &f : s.fields) {
      os << ' ' << f.name << ": " << f.type;
      if (f.arrayLen > 0) {
        os << '[' << f.arrayLen << ']';
      }
      if (f.hasDefault) {
        os << " = `" << f.defaultValue << "`";
      }
      os << ';';
    }
    os << " }\n";
  }
  for (const EnumDecl &e : m.enums) {
    os << "enum " << e.name << " {";
    for (size_t k = 0; k < e.entries.size(); ++k) {
      os << (k != 0 ? ", " : " ") << e.entries[k].name;
      if (e.entries[k].hasValue) {
        os << '=' << e.entries[k].value;
      }
    }
    os << " }\n";
  }
  for (const StyleDecl &s : m.styles) {
    os << "style " << s.name << " {";
    for (const Attr &a : s.attrs) {
      os << ' ' << a.name << "=`" << a.value << "`;";
    }
    os << " }\n";
  }
  for (const FnDecl &f : m.fns) {
    os << (f.isNative ? "nativefn " : "fn ") << f.name << '(';
    for (size_t k = 0; k < f.params.size(); ++k) {
      os << (k != 0 ? ", " : "") << f.params[k].name << ": "
         << f.params[k].type;
    }
    os << ") -> " << f.returnType;
    if (f.isNative) {
      os << " {{{" << f.native.size() << " bytes}}}\n";
    } else {
      os << " = ";
      dumpExpr(*f.body, os);
      os << '\n';
    }
  }
  for (const TemplateDecl &t : m.templates) {
    os << "template " << t.name << '\n';
    for (const InParam &p : t.ins) {
      os << "  in " << p.name << ": ";
      if (p.isEnum()) {
        for (size_t i = 0; i < p.enumValues.size(); ++i) {
          os << (i ? " | " : "") << p.enumValues[i];
        }
      } else {
        os << p.type;
      }
      if (p.hasDefault) {
        os << " = `" << p.defaultValue << "`";
      }
      os << '\n';
    }
    for (const NodePtr &n : t.body) {
      dumpNode(*n, 1, os);
    }
  }
  for (const NodePtr &n : m.roots) {
    dumpNode(*n, 0, os);
  }
  return os.str();
}

} // namespace uic
