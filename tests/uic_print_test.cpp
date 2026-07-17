// uic printer tests — parse∘print is the identity on the AST: the
// printer is the ONE writer of .ui files (hosts transform the AST and
// print), so print(m) must reparse to exactly m, and printing must be
// a fixpoint (print(parse(print(m))) == print(m)).
#include "uic/uic.hpp"

#include <gtest/gtest.h>

namespace {

// one module exercising every printable construct
constexpr char kAll[] = R"UI(
import { alpha, beta } from "common/things";

const kBack: asset = /art/back.img;
const kPair = Pairing { first: 1; second: 2; }

struct Slot {
    exists: bool;
    level: num[4];
    kind: str = "none";
}

enum Kind {
    Plain,
    Fancy = 7,
}

style dim_text {
    color: 0 0 0 .85; textalign: center;
}

fn half(v: num) -> num = v / 2;

template chip {
    in slot: num;
    in label: str = "hi";
    panel {
        width: 7h; height: 100%;
        bind visible: snapshot.slots[slot].exists && slot != 3;
        action click: game.Poke(slot);
        image { texture: /art/icon.img; }
        widgetstate hover {
            color: 1 1 1 1;
        }
    }
}

template row {
    in slot: num;
    panel {
        height: 100%; grow: 1; growinvis: 0;
        if (snapshot.slots[slot].kind == Kind.Fancy ||
                !snapshot.slots[slot].exists) {
            chip { slot: slot; }
        } else if (snapshot.slots[slot].kind == Kind.Plain) {
            chip { slot: slot; label: "plain"; }
        } else {
            panel { width: 5h; }
        }
    }
}

panel {
    width: 40h; height: 3h; align: center;
    row { slot: 0; }
}
)UI";

TEST(UicPrint, ParsePrintRoundTripsTheAst) {
  std::vector<uic::Diag> d1;
  const uic::Module m1 = uic::parseModule(kAll, "all.ui", d1);
  ASSERT_FALSE(uic::hasErrors(d1)) << d1[0].msg;

  const std::string printed = uic::printModule(m1);
  std::vector<uic::Diag> d2;
  const uic::Module m2 = uic::parseModule(printed, "all.ui", d2);
  ASSERT_FALSE(uic::hasErrors(d2))
      << d2[0].msg << "\n--- printed ---\n"
      << printed;

  EXPECT_EQ(uic::dumpModule(m1), uic::dumpModule(m2)) << printed;
}

TEST(UicPrint, PrintIsAFixpoint) {
  std::vector<uic::Diag> d1;
  const uic::Module m1 = uic::parseModule(kAll, "all.ui", d1);
  ASSERT_FALSE(uic::hasErrors(d1));
  const std::string once = uic::printModule(m1);

  std::vector<uic::Diag> d2;
  const uic::Module m2 = uic::parseModule(once, "all.ui", d2);
  ASSERT_FALSE(uic::hasErrors(d2));
  EXPECT_EQ(once, uic::printModule(m2));
}

// comments are TRIVIA on the AST and survive the printer: banners,
// open-line trails (provenance), attr leads (`// was:` records),
// between-children notes, body tails, and module tails
TEST(UicPrint, CommentsSurviveTheRoundTrip) {
  constexpr char kSrc[] =
      "// GENERATED banner line one\n"
      "// banner line two\n"
      "\n"
      "template chip { // :186\n"
      "    in slot: num;\n"
      "    panel { // :187\n"
      "        // was: onloadlua=\"Host:Foo(self)\"\n"
      "        width: 7h;\n"
      "        // between children\n"
      "        image { texture: /art/icon.img; } // :188\n"
      "        // trailing note\n"
      "    }\n"
      "}\n"
      "\n"
      "// a root note\n"
      "panel {\n"
      "    chip { slot: 0; } // :293\n"
      "}\n"
      "// module tail\n";
  std::vector<uic::Diag> d1;
  const uic::Module m1 = uic::parseModule(kSrc, "c.ui", d1);
  ASSERT_FALSE(uic::hasErrors(d1)) << d1[0].msg;

  ASSERT_EQ(m1.templates.size(), 1u);
  const uic::TemplateDecl &t = m1.templates[0];
  ASSERT_EQ(t.lead.size(), 2u);
  EXPECT_EQ(t.lead[0], "GENERATED banner line one");
  EXPECT_EQ(t.trail, ":186");
  ASSERT_EQ(t.body.size(), 1u);
  const uic::Node &panel = *t.body[0];
  EXPECT_EQ(panel.trail, ":187");
  ASSERT_EQ(panel.attrs.size(), 1u);
  ASSERT_EQ(panel.attrs[0].lead.size(), 1u);
  EXPECT_EQ(panel.attrs[0].lead[0], "was: onloadlua=\"Host:Foo(self)\"");
  ASSERT_EQ(panel.children.size(), 1u);
  ASSERT_EQ(panel.children[0]->lead.size(), 1u);
  EXPECT_EQ(panel.children[0]->lead[0], "between children");
  EXPECT_EQ(panel.children[0]->trail, ":188");
  ASSERT_EQ(panel.bodyTail.size(), 1u);
  EXPECT_EQ(panel.bodyTail[0], "trailing note");
  ASSERT_EQ(m1.roots.size(), 1u);
  ASSERT_EQ(m1.roots[0]->lead.size(), 1u);
  EXPECT_EQ(m1.roots[0]->lead[0], "a root note");
  ASSERT_EQ(m1.roots[0]->children.size(), 1u);
  EXPECT_EQ(m1.roots[0]->children[0]->trail, ":293");
  ASSERT_EQ(m1.tail.size(), 1u);
  EXPECT_EQ(m1.tail[0], "module tail");

  // print -> reparse: the trivia re-attaches identically
  const std::string printed = uic::printModule(m1);
  std::vector<uic::Diag> d2;
  const uic::Module m2 = uic::parseModule(printed, "c.ui", d2);
  ASSERT_FALSE(uic::hasErrors(d2)) << printed;
  EXPECT_EQ(uic::dumpModule(m1), uic::dumpModule(m2));
  ASSERT_EQ(m2.templates.size(), 1u);
  EXPECT_EQ(m2.templates[0].lead, t.lead);
  EXPECT_EQ(m2.templates[0].trail, t.trail);
  EXPECT_EQ(m2.templates[0].body[0]->attrs[0].lead, panel.attrs[0].lead);
  EXPECT_EQ(m2.templates[0].body[0]->bodyTail, panel.bodyTail);
  EXPECT_EQ(m2.roots[0]->children[0]->trail, ":293");
  EXPECT_EQ(m2.tail, m1.tail);
  EXPECT_EQ(printed, uic::printModule(m2)); // and printing is a fixpoint
}

// operator precedence survives: a parenthesized || under && must keep
// its parens, a natural chain must not grow any
TEST(UicPrint, ExpressionParensFollowPrecedence) {
  constexpr char kSrc[] =
      "panel { bind visible: (a || b) && !c; bind width: a || b && c; }\n";
  std::vector<uic::Diag> d1;
  const uic::Module m1 = uic::parseModule(kSrc, "e.ui", d1);
  ASSERT_FALSE(uic::hasErrors(d1));
  const std::string printed = uic::printModule(m1);
  EXPECT_NE(printed.find("bind visible: (a || b) && !c;"),
            std::string::npos)
      << printed;
  EXPECT_NE(printed.find("bind width: a || b && c;"), std::string::npos)
      << printed;
}

} // namespace
