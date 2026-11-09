#!/usr/bin/env python3
"""Add Apple clang's XNU kalloc_type builtins to an llvm-project 21.x clang tree.

    clang-xnu-builtins.py LLVM_PROJECT_DIR

Xcode's clang has three builtins and an attribute that XNU's typed allocator
(osfmk/kern/kalloc.h) needs; upstream and swiftlang clang do not:

  __builtin_xnu_type_signature(T)   "122.." one digit per 8-byte granule of T:
                                    OR of 0 padding, 1 pointer, 2 data, 8 PAC
  __builtin_xnu_type_summary(T)     OR of (1 << digit) over T's granules
  __builtin_xnu_types_compatible(A, B)  same type, or same signature
  __attribute__((xnu_usage_semantics("pointer"|"data")))  on a typedef or field:
                                    the encoding of every scalar in it
                                    (__kernel_ptr_semantics, e.g. ppnum_t)

Also: C++ may dereference void * in unevaluated operands (__typeof__(*p)).

Sema replaces each use with a string / integer literal, so nothing else in clang
has to know about them. Checked against a Mac (Xcode 27.2) build of the same
kernel: tools/wsl-xnu/kalloc-type-sigs.py compares every kalloc_type_view.
Idempotent: a tree that already has the edits is left alone.
"""
import pathlib, sys

root = pathlib.Path(sys.argv[1]) / 'clang'
MARK = 'XNU kalloc_type builtins'


def edit(rel, old, new, count=1):
    p = root / rel
    s = p.read_text()
    if new in s:
        return
    if s.count(old) != count:
        sys.exit(f'{rel}: expected {count}x {old[:60]!r}, found {s.count(old)}')
    p.write_text(s.replace(old, new))


edit('include/clang/Basic/TokenKinds.def',
     'UNARY_EXPR_OR_TYPE_TRAIT(__builtin_ptrauth_type_discriminator, PtrAuthTypeDiscriminator, KEYALL)\n',
     'UNARY_EXPR_OR_TYPE_TRAIT(__builtin_ptrauth_type_discriminator, PtrAuthTypeDiscriminator, KEYALL)\n'
     f'// {MARK} (Apple clang)\n'
     'UNARY_EXPR_OR_TYPE_TRAIT(__builtin_xnu_type_signature, XNUTypeSignature, KEYALL)\n'
     'UNARY_EXPR_OR_TYPE_TRAIT(__builtin_xnu_type_summary, XNUTypeSummary, KEYALL)\n'
     'TYPE_TRAIT_2(__builtin_xnu_types_compatible, XNUTypesCompatible, KEYALL)\n')

# Parse: same shape as __builtin_ptrauth_type_discriminator(type-name).
edit('lib/Parse/ParseExpr.cpp',
     '  case tok::kw___builtin_ptrauth_type_discriminator:\n    return ParseBuiltinPtrauthTypeDiscriminator();\n',
     f'  // {MARK}\n'
     '  case tok::kw___builtin_xnu_type_signature:\n'
     '  case tok::kw___builtin_xnu_type_summary:\n'
     '  case tok::kw___builtin_ptrauth_type_discriminator:\n    return ParseBuiltinPtrauthTypeDiscriminator();\n')
edit('lib/Parse/ParseExpr.cpp',
     'ExprResult Parser::ParseBuiltinPtrauthTypeDiscriminator() {\n  SourceLocation Loc = ConsumeToken();\n',
     'ExprResult Parser::ParseBuiltinPtrauthTypeDiscriminator() {\n'
     f'  // Also parses the {MARK}.\n'
     '  UnaryExprOrTypeTrait Kind =\n'
     '      Tok.is(tok::kw___builtin_xnu_type_signature) ? UETT_XNUTypeSignature\n'
     '      : Tok.is(tok::kw___builtin_xnu_type_summary) ? UETT_XNUTypeSummary\n'
     '                                                    : UETT_PtrAuthTypeDiscriminator;\n'
     '  SourceLocation Loc = ConsumeToken();\n')
edit('lib/Parse/ParseExpr.cpp',
     '  return Actions.ActOnUnaryExprOrTypeTraitExpr(\n      Loc, UETT_PtrAuthTypeDiscriminator,\n',
     '  return Actions.ActOnUnaryExprOrTypeTraitExpr(\n      Loc, Kind,\n')

# Sema: the signature walk, and literals in place of the UETT expressions.
edit('lib/Sema/SemaExpr.cpp',
     'static bool checkPtrAuthTypeDiscriminatorOperandType(Sema &S, QualType T,\n',
     f'''// {MARK}: XNU type signatures (osfmk/kern/kalloc.h, kt_granule_t).
#include "clang/AST/RecordLayout.h"
namespace {{
enum : uint8_t {{ XNUGranPointer = 1, XNUGranData = 2, XNUGranPAC = 8 }};

void xnuMark(SmallVectorImpl<uint8_t> &G, uint64_t Off, uint64_t Size,
             uint8_t Enc) {{
  if (!Size)
    return;
  for (uint64_t I = Off / 8, E = (Off + Size - 1) / 8; I <= E && I < G.size(); ++I)
    G[I] |= Enc;
}}

// __attribute__((xnu_usage_semantics("pointer"|"data"...))) on a typedef or
// field (XNU's __kernel_{{ptr,data,dual}}_semantics): overrides the encoding of
// every scalar inside. Outermost wins (field over its type). Only the outermost
// typedef counts, as in Apple clang: vm_size_t (typedef uintptr_t vm_size_t, no
// attribute) is data though uintptr_t has __kernel_ptr_semantics. Field
// attributes do not count inside array elements: bsd's
// lck_mtx_t {{ uintptr_t opaque[2] __kernel_data_semantics; }} is data in a struct
// but pointers (uintptr_t) in struct ch_selinfo na_si[NR_ALL].
uint8_t xnuDeclSem(const Decl *D) {{
  const auto *A = D ? D->getAttr<XNUUsageSemanticsAttr>() : nullptr;
  if (!A)
    return 0;
  uint8_t E = 0;
  for (StringRef S : A->semantics())
    E |= S == "pointer" ? XNUGranPointer : S == "data" ? XNUGranData : 0;
  return E;
}}

uint8_t xnuTypeSem(QualType T) {{
  const auto *TT = T->getAs<TypedefType>();
  return TT ? xnuDeclSem(TT->getDecl()) : 0;
}}

void xnuWalk(const ASTContext &Ctx, QualType T, uint64_t Off,
             SmallVectorImpl<uint8_t> &G, uint8_t Ov, bool InArray = false) {{
  if (!Ov)
    Ov = xnuTypeSem(T);
  if (const auto *AT = T->getAs<AtomicType>())
    T = AT->getValueType();
  if (T->isDependentType() || T->isIncompleteType() || T->isSizelessType())
    return;
  uint64_t Size = Ctx.getTypeSizeInChars(T).getQuantity();

  if (const auto *CAT = Ctx.getAsConstantArrayType(T)) {{
    QualType ET = CAT->getElementType();
    if (ET->isIncompleteType())
      return;
    uint64_t ES = Ctx.getTypeSizeInChars(ET).getQuantity();
    uint64_t N = CAT->getZExtSize();
    for (uint64_t I = 0; I < N && ES; ++I)
      xnuWalk(Ctx, ET, Off + I * ES, G, Ov, true);
    return;
  }}
  if (const RecordDecl *RD = T->getAsRecordDecl()) {{
    RD = RD->getDefinition();
    if (!RD || RD->isInvalidDecl())
      return;
    const ASTRecordLayout &L = Ctx.getASTRecordLayout(RD);
    if (const auto *CRD = dyn_cast<CXXRecordDecl>(RD)) {{
      if (L.hasOwnVFPtr())
        xnuMark(G, Off, Ctx.getTypeSizeInChars(Ctx.VoidPtrTy).getQuantity(),
                Ov ? Ov : XNUGranPointer);
      for (const CXXBaseSpecifier &B : CRD->bases()) {{
        const CXXRecordDecl *BD = B.getType()->getAsCXXRecordDecl();
        if (!BD || B.isVirtual())
          continue;
        xnuWalk(Ctx, B.getType(), Off + L.getBaseClassOffset(BD).getQuantity(), G, Ov,
                InArray);
      }}
      for (const CXXBaseSpecifier &B : CRD->vbases())
        if (const CXXRecordDecl *BD = B.getType()->getAsCXXRecordDecl())
          xnuWalk(Ctx, B.getType(), Off + L.getVBaseClassOffset(BD).getQuantity(), G, Ov,
                  InArray);
    }}
    unsigned Idx = 0;
    for (const FieldDecl *FD : RD->fields()) {{
      uint64_t Bit = L.getFieldOffset(Idx++);
      uint8_t FOv = Ov || InArray ? Ov : xnuDeclSem(FD);
      if (FD->isBitField()) {{
        unsigned W = FD->getBitWidthValue();
        if (!FOv)
          FOv = xnuTypeSem(FD->getType());
        if (W)
          xnuMark(G, Off + Bit / 8, (Bit % 8 + W + 7) / 8, FOv ? FOv : XNUGranData);
        continue;
      }}
      xnuWalk(Ctx, FD->getType(), Off + Bit / 8, G, FOv, InArray);
    }}
    return;
  }}
  if (T->isPointerType() || T->isBlockPointerType() || T->isReferenceType() ||
      T->isObjCObjectPointerType() || T->isNullPtrType()) {{
    xnuMark(G, Off, Size, Ov ? Ov
                          : T.getQualifiers().getPointerAuth() ? XNUGranPAC
                                                               : XNUGranPointer);
    return;
  }}
  if (const auto *MPT = T->getAs<MemberPointerType>()) {{
    if (Ov) {{
      xnuMark(G, Off, Size, Ov);
    }} else if (MPT->isMemberFunctionPointer()) {{
      xnuMark(G, Off, 8, XNUGranPointer);
      xnuMark(G, Off + 8, Size - 8, XNUGranData);
    }} else {{
      xnuMark(G, Off, Size, XNUGranData);
    }}
    return;
  }}
  xnuMark(G, Off, Size, Ov ? Ov : XNUGranData);
}}
}} // namespace

// Global (not clang::) so SemaTypeTraits.cpp can declare it at block scope.
std::string getXNUTypeSignature(const ASTContext &Ctx, QualType T) {{
  if (T->isVoidType() || T->isDependentType() || T->isIncompleteType() ||
      T->isFunctionType() || T->isSizelessType())
    return std::string();
  uint64_t Size = Ctx.getTypeSizeInChars(T).getQuantity();
  SmallVector<uint8_t, 64> G((Size + 7) / 8, 0);
  xnuWalk(Ctx, T, 0, G, 0);
  std::string S;
  for (uint8_t E : G)
    S.push_back(E < 10 ? '0' + E : 'a' + E - 10);
  return S;
}}

static bool checkPtrAuthTypeDiscriminatorOperandType(Sema &S, QualType T,
''')
edit('lib/Sema/SemaExpr.cpp',
     '''ExprResult Sema::CreateUnaryExprOrTypeTraitExpr(TypeSourceInfo *TInfo,
                                                SourceLocation OpLoc,
                                                UnaryExprOrTypeTrait ExprKind,
                                                SourceRange R) {
''',
     '''ExprResult Sema::CreateUnaryExprOrTypeTraitExpr(TypeSourceInfo *TInfo,
                                                SourceLocation OpLoc,
                                                UnaryExprOrTypeTrait ExprKind,
                                                SourceRange R) {
  // %s: fold to literals once the type is known.
  if (ExprKind == UETT_XNUTypeSignature || ExprKind == UETT_XNUTypeSummary) {
    QualType T = TInfo->getType();
    if (T->isDependentType())
      return new (Context) UnaryExprOrTypeTraitExpr(
          ExprKind, TInfo,
          ExprKind == UETT_XNUTypeSummary
              ? Context.UnsignedIntTy
              : Context.getPointerType(Context.CharTy.withConst()),
          OpLoc, R.getEnd());
    if (!T->isVoidType() &&
        RequireCompleteType(OpLoc, T, diag::err_incomplete_type))
      return ExprError();
    std::string Sig = getXNUTypeSignature(Context, T);
    if (ExprKind == UETT_XNUTypeSummary) {
      uint32_t Sum = 0;
      for (char C : Sig)
        Sum |= 1u << (C <= '9' ? C - '0' : C - 'a' + 10);
      return IntegerLiteral::Create(
          Context, llvm::APInt(Context.getIntWidth(Context.UnsignedIntTy), Sum),
          Context.UnsignedIntTy, OpLoc);
    }
    QualType StrTy = Context.getStringLiteralArrayType(Context.CharTy, Sig.size());
    return StringLiteral::Create(Context, Sig, StringLiteralKind::Ordinary,
                                 /*Pascal=*/false, StrTy, OpLoc);
  }
''' % MARK)

edit('lib/Sema/SemaTypeTraits.cpp',
     '  case BTT_IsSame:\n    return Self.Context.hasSameType(LhsT, RhsT);\n',
     '  case BTT_IsSame:\n    return Self.Context.hasSameType(LhsT, RhsT);\n'
     f'  case BTT_XNUTypesCompatible: {{ // {MARK}\n'
     '    std::string getXNUTypeSignature(const ASTContext &, QualType);\n'
     '    if (Self.Context.hasSameUnqualifiedType(LhsT, RhsT))\n'
     '      return true;\n'
     '    if (LhsT->isVoidType() || RhsT->isVoidType() ||\n'
     '        LhsT->isIncompleteType() || RhsT->isIncompleteType())\n'
     '      return false;\n'
     '    return getXNUTypeSignature(Self.Context, LhsT) ==\n'
     '           getXNUTypeSignature(Self.Context, RhsT);\n'
     '  }\n')

# Dependent forms are never constant-evaluated or mangled; keep switches complete.
edit('lib/AST/ItaniumMangle.cpp',
     '    case UETT_PtrAuthTypeDiscriminator:\n    case UETT_DataSizeOf: {\n',
     '    case UETT_PtrAuthTypeDiscriminator:\n'
     f'    case UETT_XNUTypeSignature: // {MARK}\n'
     '    case UETT_XNUTypeSummary:\n'
     '    case UETT_DataSizeOf: {\n')
edit('lib/AST/ExprConstant.cpp',
     '  case UETT_PtrAuthTypeDiscriminator: {\n    if (E->getArgumentType()->isDependentType())\n      return false;\n',
     f'  case UETT_XNUTypeSignature: // {MARK}: folded by Sema\n'
     '  case UETT_XNUTypeSummary:\n'
     '    return false;\n'
     '  case UETT_PtrAuthTypeDiscriminator: {\n    if (E->getArgumentType()->isDependentType())\n      return false;\n')
# The attribute behind XNU's __kernel_{ptr,data,dual}_semantics (sys/cdefs.h).
edit('include/clang/Basic/Attr.td',
     'def NoSanitize : InheritableAttr {\n',
     f'// {MARK}: XNU signature semantics of a typedef or field.\n'
     'def XNUUsageSemantics : InheritableAttr {\n'
     '  let Spellings = [Clang<"xnu_usage_semantics">];\n'
     '  let Args = [VariadicStringArgument<"Semantics">];\n'
     '  let Subjects = SubjectList<[TypedefName, Field], ErrorDiag>;\n'
     '  let Documentation = [InternalOnly];\n'
     '}\n\n'
     'def NoSanitize : InheritableAttr {\n')
edit('lib/Sema/SemaDeclAttr.cpp',
     '  case ParsedAttr::AT_NoSanitize:\n    handleNoSanitizeAttr(S, D, AL);\n    break;\n',
     f'  case ParsedAttr::AT_XNUUsageSemantics: {{ // {MARK}\n'
     '    SmallVector<StringRef, 2> Sems;\n'
     '    for (unsigned I = 0, E = AL.getNumArgs(); I != E; ++I) {\n'
     '      StringRef Str;\n'
     '      if (!S.checkStringLiteralArgumentAttr(AL, I, Str))\n'
     '        return;\n'
     '      Sems.push_back(Str);\n'
     '    }\n'
     '    D->addAttr(::new (S.Context) XNUUsageSemanticsAttr(S.Context, AL, Sems.data(),\n'
     '                                                       Sems.size()));\n'
     '    break;\n'
     '  }\n'
     '  case ParsedAttr::AT_NoSanitize:\n    handleNoSanitizeAttr(S, D, AL);\n    break;\n')

# Apple clang lets C++ dereference void * in unevaluated operands, which XNU's
# KALLOC_TYPE_IS_COMPATIBLE_PTR does in __typeof__(*(ptr)) (it silences
# -Wvoid-ptr-dereference around it).
edit('lib/Sema/SemaExpr.cpp',
     '    if (LO.CPlusPlus)\n      S.Diag(OpLoc, diag::err_typecheck_indirection_through_void_pointer_cpp)\n',
     f'    if (LO.CPlusPlus && !S.isUnevaluatedContext()) // {MARK}: as Apple clang\n'
     '      S.Diag(OpLoc, diag::err_typecheck_indirection_through_void_pointer_cpp)\n')

print('clang tree patched')
