//=== unittests/Sema/CodeCompleteTest.cpp - Code Complete tests ==============//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Parse/ParseAST.h"
#include "clang/Sema/Sema.h"
#include "clang/Sema/SemaDiagnostic.h"
#include "clang/Tooling/Tooling.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <cstddef>
#include <string>

namespace {

using namespace clang;
using namespace clang::tooling;
using ::testing::Each;
using ::testing::UnorderedElementsAre;

const char TestCCName[] = "test.cc";

struct CompletionContext {
  std::vector<std::string> VisitedNamespaces;
  std::string PreferredType;
};

class VisitedContextFinder : public CodeCompleteConsumer {
public:
  VisitedContextFinder(CompletionContext &ResultCtx)
      : CodeCompleteConsumer(/*CodeCompleteOpts=*/{},
                             /*CodeCompleteConsumer*/ false),
        ResultCtx(ResultCtx),
        CCTUInfo(std::make_shared<GlobalCodeCompletionAllocator>()) {}

  void ProcessCodeCompleteResults(Sema &S, CodeCompletionContext Context,
                                  CodeCompletionResult *Results,
                                  unsigned NumResults) override {
    ResultCtx.VisitedNamespaces =
        getVisitedNamespace(Context.getVisitedContexts());
    ResultCtx.PreferredType = Context.getPreferredType().getAsString();
  }

  CodeCompletionAllocator &getAllocator() override {
    return CCTUInfo.getAllocator();
  }

  CodeCompletionTUInfo &getCodeCompletionTUInfo() override { return CCTUInfo; }

private:
  std::vector<std::string> getVisitedNamespace(
      CodeCompletionContext::VisitedContextSet VisitedContexts) const {
    std::vector<std::string> NSNames;
    for (const auto *Context : VisitedContexts)
      if (const auto *NS = llvm::dyn_cast<NamespaceDecl>(Context))
        NSNames.push_back(NS->getQualifiedNameAsString());
    return NSNames;
  }

  CompletionContext &ResultCtx;
  CodeCompletionTUInfo CCTUInfo;
};

class CodeCompleteAction : public SyntaxOnlyAction {
public:
  CodeCompleteAction(ParsedSourceLocation P, CompletionContext &ResultCtx)
      : CompletePosition(std::move(P)), ResultCtx(ResultCtx) {}

  bool BeginInvocation(CompilerInstance &CI) override {
    CI.getFrontendOpts().CodeCompletionAt = CompletePosition;
    CI.setCodeCompletionConsumer(new VisitedContextFinder(ResultCtx));
    return true;
  }

private:
  // 1-based code complete position <Line, Col>;
  ParsedSourceLocation CompletePosition;
  CompletionContext &ResultCtx;
};

ParsedSourceLocation offsetToPosition(llvm::StringRef Code, size_t Offset) {
  Offset = std::min(Code.size(), Offset);
  StringRef Before = Code.substr(0, Offset);
  int Lines = Before.count('\n');
  size_t PrevNL = Before.rfind('\n');
  size_t StartOfLine = (PrevNL == StringRef::npos) ? 0 : (PrevNL + 1);
  return {TestCCName, static_cast<unsigned>(Lines + 1),
          static_cast<unsigned>(Offset - StartOfLine + 1)};
}

CompletionContext runCompletion(StringRef Code, size_t Offset) {
  CompletionContext ResultCtx;
  auto Action = llvm::make_unique<CodeCompleteAction>(
      offsetToPosition(Code, Offset), ResultCtx);
  clang::tooling::runToolOnCodeWithArgs(Action.release(), Code, {"-std=c++11"},
                                        TestCCName);
  return ResultCtx;
}

struct ParsedAnnotations {
  std::vector<size_t> Points;
  std::string Code;
};

ParsedAnnotations parseAnnotations(StringRef AnnotatedCode) {
  ParsedAnnotations R;
  while (!AnnotatedCode.empty()) {
    size_t NextPoint = AnnotatedCode.find('^');
    if (NextPoint == StringRef::npos) {
      R.Code += AnnotatedCode;
      AnnotatedCode = "";
      break;
    }
    R.Code += AnnotatedCode.substr(0, NextPoint);
    R.Points.push_back(R.Code.size());

    AnnotatedCode = AnnotatedCode.substr(NextPoint + 1);
  }
  return R;
}

CompletionContext runCodeCompleteOnCode(StringRef AnnotatedCode) {
  ParsedAnnotations P = parseAnnotations(AnnotatedCode);
  assert(P.Points.size() == 1 && "expected exactly one annotation point");
  return runCompletion(P.Code, P.Points.front());
}

std::vector<std::string> collectPreferredTypes(StringRef AnnotatedCode) {
  ParsedAnnotations P = parseAnnotations(AnnotatedCode);
  std::vector<std::string> Types;
  for (size_t Point : P.Points)
    Types.push_back(runCompletion(P.Code, Point).PreferredType);
  return Types;
}

TEST(SemaCodeCompleteTest, VisitedNSForValidQualifiedId) {
  auto VisitedNS = runCodeCompleteOnCode(R"cpp(
     namespace ns1 {}
     namespace ns2 {}
     namespace ns3 {}
     namespace ns3 { namespace nns3 {} }

     namespace foo {
     using namespace ns1;
     namespace ns4 {} // not visited
     namespace { using namespace ns2; }
     inline namespace bar { using namespace ns3::nns3; }
     } // foo
     namespace ns { foo::^ }
  )cpp")
                       .VisitedNamespaces;
  EXPECT_THAT(VisitedNS, UnorderedElementsAre("foo", "ns1", "ns2", "ns3::nns3",
                                              "foo::(anonymous)"));
}

TEST(SemaCodeCompleteTest, VisitedNSForInvalideQualifiedId) {
  auto VisitedNS = runCodeCompleteOnCode(R"cpp(
     namespace ns { foo::^ }
  )cpp")
                       .VisitedNamespaces;
  EXPECT_TRUE(VisitedNS.empty());
}

TEST(SemaCodeCompleteTest, VisitedNSWithoutQualifier) {
  auto VisitedNS = runCodeCompleteOnCode(R"cpp(
    namespace n1 {
    namespace n2 {
      void f(^) {}
    }
    }
  )cpp")
                       .VisitedNamespaces;
  EXPECT_THAT(VisitedNS, UnorderedElementsAre("n1", "n1::n2"));
}

TEST(PreferredTypeTest, BinaryExpr) {
  // Check various operations for arithmetic types.
  StringRef code1 = R"cpp(
    void test(int x) {
      x = ^10;
      x += ^10; x -= ^10; x *= ^10; x /= ^10; x %= ^10;
      x + ^10; x - ^10; x * ^10; x / ^10; x % ^10;
    })cpp";
  EXPECT_THAT(collectPreferredTypes(code1), Each("int"));
  StringRef code2 = R"cpp(
    void test(float x) {
      x = ^10;
      x += ^10; x -= ^10; x *= ^10; x /= ^10; x %= ^10;
      x + ^10; x - ^10; x * ^10; x / ^10; x % ^10;
    })cpp";
  EXPECT_THAT(collectPreferredTypes(code2), Each("float"));

  // Pointer types.
  StringRef code3 = R"cpp(
    void test(int *ptr) {
      ptr - ^ptr;
      ptr = ^ptr;
    })cpp";
  EXPECT_THAT(collectPreferredTypes(code3), Each("int *"));

  StringRef code4 = R"cpp(
    void test(int *ptr) {
      ptr + ^10;
      ptr += ^10;
      ptr -= ^10;
    })cpp";
  EXPECT_THAT(collectPreferredTypes(code4), Each("long")); // long is normalized 'ptrdiff_t'.

  // Comparison operators.
  StringRef code5 = R"cpp(
    void test(int i) {
      i <= ^1; i < ^1; i >= ^1; i > ^1; i == ^1; i != ^1;
    }
  )cpp";
  EXPECT_THAT(collectPreferredTypes(code5), Each("int"));

  StringRef code6 = R"cpp(
    void test(int *ptr) {
      ptr <= ^ptr; ptr < ^ptr; ptr >= ^ptr; ptr > ^ptr;
      ptr == ^ptr; ptr != ^ptr;
    }
  )cpp";
  EXPECT_THAT(collectPreferredTypes(code6), Each("int *"));

  // Relational operations.
  StringRef code7 = R"cpp(
    void test(int i, int *ptr) {
      i && ^1; i || ^1;
      ptr && ^1; ptr || ^1;
    }
  )cpp";
  EXPECT_THAT(collectPreferredTypes(code7), Each("_Bool"));

  // Bitwise operations.
  StringRef code8 = R"cpp(
    void test(long long ll) {
      ll | ^1; ll & ^1;
    }
  )cpp";
  EXPECT_THAT(collectPreferredTypes(code8), Each("long long"));

  StringRef code9 = R"cpp(
    enum A {};
    void test(A a) {
      a | ^1; a & ^1;
    }
  )cpp";
  EXPECT_THAT(collectPreferredTypes(code9), Each("enum A"));

  StringRef code10 = R"cpp(
    enum class A {};
    void test(A a) {
      // This is technically illegal with the 'enum class' without overloaded
      // operators, but we pretend it's fine.
      a | ^a; a & ^a;
    }
  )cpp";
  EXPECT_THAT(collectPreferredTypes(code10), Each("enum A"));

  // Binary shifts.
  StringRef code11 = R"cpp(
    void test(int i, long long ll) {
      i << ^1; ll << ^1;
      i <<= ^1; i <<= ^1;
      i >> ^1; ll >> ^1;
      i >>= ^1; i >>= ^1;
    }
  )cpp";
  EXPECT_THAT(collectPreferredTypes(code11), Each("int"));

  // Comma does not provide any useful information.
  StringRef code12 = R"cpp(
    class Cls {};
    void test(int i, int* ptr, Cls x) {
      (i, ^i);
      (ptr, ^ptr);
      (x, ^x);
    }
  )cpp";
  EXPECT_THAT(collectPreferredTypes(code12), Each("NULL TYPE"));

  // User-defined types do not take operator overloading into account.
  // However, they provide heuristics for some common cases.
  StringRef code13 = R"cpp(
    class Cls {};
    void test(Cls c) {
      // we assume arithmetic and comparions ops take the same type.
      c + ^c; c - ^c; c * ^c; c / ^c; c % ^c;
      c == ^c; c != ^c; c < ^c; c <= ^c; c > ^c; c >= ^c;
      // same for the assignments.
      c = ^c; c += ^c; c -= ^c; c *= ^c; c /= ^c; c %= ^c;
    }
  )cpp";
  EXPECT_THAT(collectPreferredTypes(code13), Each("class Cls"));

  StringRef code14 = R"cpp(
    class Cls {};
    void test(Cls c) {
      // we assume relational ops operate on bools.
      c && ^c; c || ^c;
    }
  )cpp";
  EXPECT_THAT(collectPreferredTypes(code14), Each("_Bool"));

  StringRef code15 = R"cpp(
    class Cls {};
    void test(Cls c) {
      // we make no assumptions about the following operators, since they are
      // often overloaded with a non-standard meaning.
      c << ^c; c >> ^c; c | ^c; c & ^c;
      c <<= ^c; c >>= ^c; c |= ^c; c &= ^c;
    }
  )cpp";
  EXPECT_THAT(collectPreferredTypes(code15), Each("NULL TYPE"));
}

} // namespace
