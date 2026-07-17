#pragma once
// uic AST — the parsed shape of one .ui module (docs/ui-language.md).
// Later passes (instantiate/fold, schema, layout plan, emit) transform
// this tree; the parser owns only its shape.
#include <memory>
#include <string>
#include <vector>

namespace uic {

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
  enum Kind {
    kIdent,   // text = name
    kNumber,  // text = lexeme
    kDim,     // text = lexeme incl. unit suffix ("13.8h")
    kColor,   // text = "#rrggbb[aa]"
    kPath,    // text = "/..." resource path
    kString,  // text = unquoted content
    kBool,    // text = "true"/"false"
    kField,   // args[0].text — text = field name
    kIndex,   // args[0] [ args[1] ]
    kCall,    // args[0] ( args[1..] )
    kUnary,   // text = "!" or "-", args[0]
    kBinary,  // text = operator, args[0] op args[1]
    kTernary, // args[0] ? args[1] : args[2]
  };
  Kind kind = kIdent;
  std::string text;
  std::vector<ExprPtr> args;
  int line = 0;
};

// A plain attribute: value verbatim to ';' (the porting surface).
struct Attr {
  std::string name;
  std::string value;
  int line = 0;
};

struct Bind {
  std::string target; // the attribute this expression drives
  ExprPtr expr;
  int line = 0;
};

struct Action {
  std::string event; // click, rightclick, ...
  ExprPtr expr;      // host call expression
  int line = 0;
};

struct Node;
using NodePtr = std::unique_ptr<Node>;

struct IfArm {
  ExprPtr cond; // null = else
  std::vector<NodePtr> body;
  int line = 0;
};

struct Node {
  enum Kind {
    kWidget,      // tag = widget class or template name
    kWidgetState, // tag = state name
    kIf,          // arms populated; everything else empty
  };
  Kind kind = kWidget;
  std::string tag;
  std::vector<Attr> attrs;
  std::vector<Bind> binds;
  std::vector<Action> actions;
  std::vector<NodePtr> children;
  std::vector<IfArm> arms;
  int line = 0;
};

struct InParam {
  std::string name;
  std::string type;
  std::string defaultValue; // verbatim; empty + !hasDefault = required
  bool hasDefault = false;
  int line = 0;
};

struct TemplateDecl {
  std::string name;
  std::vector<InParam> ins;
  std::vector<NodePtr> body;
  int line = 0;
};

struct StructField {
  std::string name;
  std::string type;
  std::string defaultValue;
  bool hasDefault = false;
  int line = 0;
};

struct StructDecl {
  std::string name;
  std::vector<StructField> fields;
  int line = 0;
};

struct StyleDecl {
  std::string name;
  std::vector<Attr> attrs;
  int line = 0;
};

struct ConstDecl {
  std::string name;
  std::string type;      // optional annotation ("" = inferred later)
  std::string rawValue;  // set when the value is a plain literal
  std::string initType;  // set when the value is `Type { fields }`
  std::vector<Attr> initFields;
  int line = 0;
};

struct FnParam {
  std::string name;
  std::string type;
};

struct FnDecl {
  std::string name;
  std::vector<FnParam> params;
  std::string returnType;
  ExprPtr body;       // expression-bodied fn
  std::string native; // verbatim C++ when this is a native fn
  bool isNative = false;
  int line = 0;
};

struct Import {
  std::vector<std::string> names;
  std::string from;
  int line = 0;
};

struct Module {
  std::string name; // from the file name
  std::vector<Import> imports;
  std::vector<ConstDecl> consts;
  std::vector<StructDecl> structs;
  std::vector<StyleDecl> styles;
  std::vector<FnDecl> fns;
  std::vector<TemplateDecl> templates;
  std::vector<NodePtr> roots; // top-level widget trees
};

} // namespace uic
