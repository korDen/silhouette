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
