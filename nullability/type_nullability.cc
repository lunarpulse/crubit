// Part of the Crubit project, under the Apache License v2.0 with LLVM
// Exceptions. See /LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "nullability/type_nullability.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/base/nullability.h"
#include "absl/log/check.h"
#include "nullability/type_and_maybe_loc_visitor.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTFwd.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Expr.h"
#include "clang/AST/NestedNameSpecifier.h"
#include "clang/AST/TemplateBase.h"
#include "clang/AST/TemplateName.h"
#include "clang/AST/Type.h"
#include "clang/AST/TypeLoc.h"
#include "clang/AST/TypeVisitor.h"
#include "clang/Analysis/FlowSensitive/Arena.h"
#include "clang/Basic/LLVM.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/Specifiers.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/ScopedPrinter.h"

namespace clang::tidy::nullability {

static bool SmartPointersEnabled = false;

namespace test {

EnableSmartPointers::EnableSmartPointers() { enableSmartPointers(true); }

}  // namespace test

void enableSmartPointers(bool Enabled) { SmartPointersEnabled = Enabled; }

bool smartPointersEnabled() { return SmartPointersEnabled; }

bool isSupportedPointerType(QualType T) {
  return isSupportedRawPointerType(T) || isSupportedSmartPointerType(T);
}

bool isSupportedRawPointerType(QualType T) { return T->isPointerType(); }

bool isSupportedSmartPointerType(QualType T) {
  return !underlyingRawPointerType(T).isNull();
}

static bool isStandardSmartPointerDecl(const CXXRecordDecl *RD) {
  if (!RD->getDeclContext()->isStdNamespace()) return false;

  const IdentifierInfo *ID = RD->getIdentifier();
  if (ID == nullptr) return false;

  StringRef Name = ID->getName();
  return Name == "unique_ptr" || Name == "shared_ptr";
}

static bool hasAbslNullabilityCompatibleTag(const CXXRecordDecl *RD) {
  // If the specialization hasn't been instantiated -- for example because it
  // is only used as a function parameter type -- then the specialization won't
  // contain the `absl_nullability_compatible` tag. Therefore, we look at the
  // template rather than the specialization.
  if (const auto *CTSD = dyn_cast<ClassTemplateSpecializationDecl>(RD))
    RD = CTSD->getSpecializedTemplate()->getTemplatedDecl();
  const auto &Idents = RD->getASTContext().Idents;
  auto It = Idents.find("absl_nullability_compatible");
  if (It == Idents.end()) return false;
  return RD->lookup(It->getValue()).find_first<TypedefNameDecl>() != nullptr;
}

static absl::Nullable<const CXXRecordDecl *> getSmartPointerBaseClass(
    absl::Nullable<const CXXRecordDecl *> RD,
    llvm::SmallPtrSet<const CXXRecordDecl *, 2> &Seen) {
  if (RD == nullptr) return nullptr;

  if (isStandardSmartPointerDecl(RD) || hasAbslNullabilityCompatibleTag(RD))
    return RD;

  if (RD->hasDefinition())
    for (const CXXBaseSpecifier &Base : RD->bases())
      if (Base.getAccessSpecifier() == AS_public) {
        const CXXRecordDecl *BaseClass = Base.getType()->getAsCXXRecordDecl();

        // If we didn't get a `CXXRecordDecl` above, this could be something
        // like `unique_ptr<T>` (where `T` is a dependent type). In this case,
        // return the `CXXRecordDecl` of the underlying template -- it's the
        // best we can do.
        if (BaseClass == nullptr) {
          if (const auto *TST =
                  Base.getType()->getAs<TemplateSpecializationType>()) {
            BaseClass = dyn_cast<CXXRecordDecl>(
                TST->getTemplateName().getAsTemplateDecl()->getTemplatedDecl());

            // We need to be careful here: Once we start looking at underlying
            // templates, we may walk into cycles, as a template may derive from
            // itself (either directly or indirectly), though with different
            // template arguments.
            // To protect against infinite recursion, make sure we haven't seen
            // this particular base class before. (We only need to do this in
            // this case where we're looking at the template itself rather than
            // a specialization.)
            if (BaseClass != nullptr) {
              if (!Seen.insert(BaseClass).second) return nullptr;
            }
          }
        }

        if (const CXXRecordDecl *Result =
                getSmartPointerBaseClass(BaseClass, Seen))
          return Result;
      }

  return nullptr;
}

/// If `RD` or one of its public bases is a smart pointer class, returns that
/// smart pointer class; otherwise, returns null.
static absl::Nullable<const CXXRecordDecl *> getSmartPointerBaseClass(
    absl::Nullable<const CXXRecordDecl *> RD) {
  llvm::SmallPtrSet<const CXXRecordDecl *, 2> Seen;
  return getSmartPointerBaseClass(RD, Seen);
}

QualType underlyingRawPointerType(QualType T) {
  if (!SmartPointersEnabled) return QualType();

  const CXXRecordDecl *RD = T.getCanonicalType()->getAsCXXRecordDecl();
  if (RD == nullptr) return QualType();

  const ASTContext &ASTCtx = RD->getASTContext();

  // There's a special case we need to handle here:
  // If `RD` is a `ClassTemplateSpecializationDecl` for an uninstantiated
  // specialization of a smart pointer (or a class derived from it), it's just
  // an empty shell -- it doesn't contain any base specifiers or any of the type
  // aliases we need (`absl_nullability_compatible`, `pointer`, `element_type`).
  // We deal with this as follows:
  // *  We check the primary template for base classes and the
  //    `absl_nullability_compatible` type alias.
  // *  We extract the underlying pointer type from the template argument (as
  //    that's the best we can do).
  if (auto *CTSD = dyn_cast<ClassTemplateSpecializationDecl>(RD);
      CTSD && !CTSD->hasDefinition()) {
    if (getSmartPointerBaseClass(
            CTSD->getSpecializedTemplate()->getTemplatedDecl()) == nullptr)
      return QualType();

    if (CTSD->getTemplateArgs().size() == 0) return QualType();
    if (CTSD->getTemplateArgs()[0].getKind() != TemplateArgument::Type)
      return QualType();

    QualType TemplateArg = CTSD->getTemplateArgs()[0].getAsType();
    return ASTCtx.getPointerType(ASTCtx.getBaseElementType(TemplateArg));
  }

  const CXXRecordDecl *SmartPtrDecl = getSmartPointerBaseClass(RD);
  if (SmartPtrDecl == nullptr) return QualType();

  const auto &Idents = ASTCtx.Idents;
  if (auto PointerIt = Idents.find("pointer"); PointerIt != Idents.end()) {
    if (auto *TND = SmartPtrDecl->lookup(PointerIt->getValue())
                        .find_first<TypedefNameDecl>()) {
      // It's possible for a `unique_ptr` to have an underlying `pointer` type
      // that is not a raw pointer if there is a custom deleter that specifies
      // such a type. (The only requirement is the the underlying pointer type
      // is a NullablePointer.) This case is rare, so we simply ignore such
      // pointers.
      if (isSupportedRawPointerType(TND->getUnderlyingType()))
        return TND->getUnderlyingType();
      return QualType();
    }
  }
  if (auto PointerIt = Idents.find("element_type"); PointerIt != Idents.end()) {
    if (auto *TND = SmartPtrDecl->lookup(PointerIt->getValue())
                        .find_first<TypedefNameDecl>())
      return ASTCtx.getPointerType(TND->getUnderlyingType());
  }
  return QualType();
}

PointerTypeNullability PointerTypeNullability::createSymbolic(
    dataflow::Arena &A) {
  PointerTypeNullability Symbolic;
  Symbolic.Symbolic = true;
  Symbolic.Nonnull = A.makeAtom();
  Symbolic.Nullable = A.makeAtom();
  return Symbolic;
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                              const PointerTypeNullability &PN) {
  // TODO: should symbolic nullabilities have names?
  if (PN.isSymbolic())
    return OS << "Symbolic(Nonnull=" << PN.Nonnull << ", "
              << "Nullable=" << PN.Nullable << ")";
  return OS << PN.concrete();
}

std::string nullabilityToString(const TypeNullability &Nullability) {
  std::string Result = "[";
  llvm::interleave(
      Nullability,
      [&](const PointerTypeNullability &PN) { Result += llvm::to_string(PN); },
      [&] { Result += ", "; });
  Result += "]";
  return Result;
}

// Recognize aliases e.g. Nonnull<T> as equivalent to T _Nonnull, etc.
// These aliases should be annotated with [[clang::annotate("Nullable")]] etc.
//
// TODO: Ideally such aliases could apply the _Nonnull attribute themselves.
// This requires resolving compatibility issues with clang, such as use with
// user-defined pointer-like types.
std::optional<NullabilityKind> getAliasNullability(const TemplateName &TN) {
  if (const auto *TD = TN.getAsTemplateDecl()) {
    if (!TD->getTemplatedDecl()) return std::nullopt;  // BuiltinTemplateDecl
    if (const auto *A = TD->getTemplatedDecl()->getAttr<AnnotateAttr>()) {
      if (A->getAnnotation() == "Nullable") return NullabilityKind::Nullable;
      if (A->getAnnotation() == "Nonnull") return NullabilityKind::NonNull;
      if (A->getAnnotation() == "Nullability_Unspecified")
        return NullabilityKind::Unspecified;
    }
  }
  return std::nullopt;
}

namespace {
FileID getGoverningFile(absl::Nullable<const Decl *> D) {
  if (!D) return FileID();
  return D->getASTContext()
      .getSourceManager()
      .getDecomposedExpansionLoc(D->getLocation())
      .first;
}

// Traverses a Type to find the points where it might be nullable.
// This will visit the contained PointerType in the correct order to produce
// the TypeNullability vector.
//
// Subclasses must provide
//   `void report(const Type*, FileID, optional<NullabilityKind>,
//                std::optional<TypeLoc>)`
//  (the FileID is the one whose #pragma governs the type)
// They may override TypeAndMaybeLocVisitor visit*Type methods to customize the
// traversal.
//
// Canonically-equivalent Types produce equivalent sequences of report() calls:
//  - corresponding pointer Types are canonically-equivalent
//  - the NullabilityKind may be different, as it derives from type sugar
template <class Impl>
class NullabilityWalker : public TypeAndMaybeLocVisitor<Impl> {
  using Base = TypeAndMaybeLocVisitor<Impl>;
  Impl &derived() { return *static_cast<Impl *>(this); }

  // A nullability attribute we've seen, waiting to attach to a pointer type.
  // There may be sugar in between: Attributed -> Typedef -> Typedef -> Pointer.
  // All non-sugar types must consume nullability, most will ignore it.
  std::optional<NullabilityKind> PendingNullability;

  // The file whose #pragma governs the type currently being walked.
  FileID File;

  // The most complete and direct TypeLoc seen so far for the type currently
  // being visited.
  std::optional<TypeLoc> BestLocSoFar;

  // Update `BestLocSoFar` for a new TypeLoc seen.
  //
  // If not `OverrideElaborated`, an existing ElaboratedTypeLoc in
  // `BestLocSoFar` will be kept instead of replacing it with `Loc`. This
  // supports reporting of the most complete TypeLoc for a type, e.g.
  // `std::unique_ptr<int>` instead of just `unique_ptr<int>`.
  void recordLoc(TypeLoc Loc, bool OverrideElaborated = false) {
    if (BestLocSoFar && BestLocSoFar->getType().getCanonicalType() !=
                            Loc.getType().getCanonicalType()) {
      // We've moved on to visiting a new type, so clear the Loc.
      BestLocSoFar = std::nullopt;
    }
    // In most cases we want to keep the most Elaborated Loc for the type, but
    // template arguments supersede that preference.
    if (!BestLocSoFar || OverrideElaborated ||
        BestLocSoFar->getTypeLocClass() != TypeLoc::Elaborated) {
      BestLocSoFar = Loc;
    }
  }

  void sawNullability(NullabilityKind NK) {
    // If we see nullability applied twice, the outer one wins.
    if (!PendingNullability.has_value()) PendingNullability = NK;
  }

  void ignoreUnexpectedNullability() {
    // TODO: Can we upgrade this to an assert?
    // clang is pretty thorough about ensuring we can't put _Nullable on
    // non-pointers, even failing template instantiation on this basis.
    PendingNullability.reset();
  }

  // While walking types instantiated from templates, e.g.:
  //  - the underlying type of alias TemplateSpecializationTypes
  //  - type aliases inside class template instantiations
  // we see SubstTemplateTypeParmTypes where type parameters were referenced.
  // The directly-available underlying types lack sugar, but we can retrieve the
  // sugar from the arguments of the original e.g. TemplateSpecializationType.
  //
  // The "template context" associates template params with the
  // corresponding args, to allow this retrieval.
  // In general, not just the directly enclosing template params but also those
  // of outer classes are accessible.
  // So conceptually this maps (depth, index, pack_index) => TemplateArgument.
  // To avoid copying these maps, inner contexts *extend* from outer ones.
  //
  // When we start to walk a TemplateArgument (in place of a SubstTTPType), we
  // must do so in the template instantiation context where the argument was
  // written. Then when we're done, we must restore the old context.
  struct TemplateContext {
    // A decl that owns an arg list, per SubstTTPType::getAssociatedDecl.
    // For aliases: TypeAliasTemplateDecl.
    // For classes: ClassTemplateSpecializationDecl.
    const Decl *AssociatedDecl = nullptr;
    // The sugared template arguments to AssociatedDecl, as written in the code.
    // If absent, the arguments could not be reconstructed.
    std::optional<ArrayRef<TemplateArgument>> Args;
    // The file whose #pragma governs types written in Args.
    FileID ArgsFile;
    // In general, multiple template params are in scope (nested templates).
    // These are a linked list: *this describes one, *Extends describes the
    // next. In practice, this is the enclosing class template.
    const TemplateContext *Extends = nullptr;
    // The template context in which the args were written.
    // The args may reference params visible in this context.
    const TemplateContext *ArgContext = nullptr;
    // `Args` plus location information, if available.
    std::optional<std::vector<TemplateArgumentLoc>> ArgLocs;

    // Example showing a TemplateContext graph:
    //
    //   // (some sugar and nested templates for the example)
    //   using INT = int; using FLOAT = float;
    //   template <class T> struct Outer {
    //     template <class U> struct Inner {
    //       using Pair = std::pair<T, U>;
    //     }
    //   }
    //
    //   template <class X>
    //   struct S {
    //     using Type = typename Outer<INT>::Inner<X>::Pair;
    //   }
    //
    //   using Target = S<FLOAT>::Type;
    //
    // Per clang's AST, instantiated Type is std::pair<int, float> with only
    // SubstTemplateTypeParmTypes for sugar, we're trying to recover INT, FLOAT.
    //
    // When walking the ElaboratedType for the S<FLOAT>:: qualifier we set up:
    //
    // Current -> {Associated=S<float>, Args=<FLOAT>, Extends=null, ArgCtx=null}
    //
    // This means that when resolving ::Type:
    //   - we can resugar occurrences of X (float -> FLOAT)
    //   - ArgContext=null: the arg FLOAT may not refer to template params
    //                      (or at least we can't resugar them)
    //   - Extends=null: there are no other template params we can resugar
    //
    // Skipping up to ::Pair inside S<FLOAT>'s instantiation, we have the graph:
    //
    // Current -> {Associated=Outer<int>::Inner<float>, Args=<X>}
    //            | Extends                                  |
    // A{Associated=Outer<int>, Args=<INT>, Extends=null}    | ArgContext
    //                          | ArgContext                 |
    //       B{Associated=S<float>, Args=<FLOAT>, Extends=null, ArgContext=null}
    //
    // (Note that B here is the original TemplateContext we set up above).
    //
    // This means that when resolving ::Pair:
    //   - we can resugar instances of U (float -> X)
    //   - ArgContext=B: when resugaring U, we can resugar X (float -> FLOAT)
    //   - Extends=A: we can also resugar T (int -> INT)
    //   - A.ArgContext=B: when resugaring T, we can resugar X.
    //                     (we never do, because INT doesn't mention X)
    //   - A.Extends=null: there are no other template params te resugar
    //   - B.ArgContext=null: FLOAT may not refer to any template params
    //   - B.Extends=null: there are no other template params to resugar
    //                     (e.g. Type's definition cannot refer to T)
  };
  // The context that provides sugared args for the template params that are
  // accessible to the type we're currently walking.
  const TemplateContext *CurrentTemplateContext = nullptr;

  // Adjusts args list from those of primary template => template pattern.
  //
  // A template arg list corresponds 1:1 to primary template params.
  // In partial specializations, the correspondence may differ:
  //   template <int, class> struct S;
  //   template <class T> struct S<0, T> {
  //       using Alias = T;  // T refers to param #0
  //   };
  //   S<0, int*>::Alias X;  // T is bound to arg #1
  // or
  //   template <class> struct S;
  //   template <class T> struct S<T*> { using Alias = T; }
  //   S<int*>::Alias X;  // arg #0 is int*, param #0 is bound to int
  void translateTemplateArgsForSpecialization(TemplateContext &Ctx) {
    // Only relevant where partial specialization is used.
    // - Full specializations may not refer to template params at all.
    // - For primary templates, the input is already correct.
    const TemplateArgumentList *PartialArgs = nullptr;
    if (const ClassTemplateSpecializationDecl *CTSD =
            llvm::dyn_cast<ClassTemplateSpecializationDecl>(
                Ctx.AssociatedDecl)) {
      if (isa_and_nonnull<ClassTemplatePartialSpecializationDecl>(
              CTSD->getTemplateInstantiationPattern()))
        PartialArgs = &CTSD->getTemplateInstantiationArgs();
    } else if (const VarTemplateSpecializationDecl *VTSD =
                   llvm::dyn_cast<VarTemplateSpecializationDecl>(
                       Ctx.AssociatedDecl)) {
      if (isa_and_nonnull<VarTemplatePartialSpecializationDecl>(
              VTSD->getTemplateInstantiationPattern()))
        PartialArgs = &VTSD->getTemplateInstantiationArgs();
    }
    if (!PartialArgs) return;

    // To get from the template arg list to the partial-specialization arg list
    // means running much of the template argument deduction algorithm.
    // This is complex in general. [temp.deduct] For now, bail out.
    // In future, hopefully we can handle at least simple cases.
    Ctx.Args.reset();
    Ctx.ArgLocs.reset();
  }

  void report(const Type *T) {
    if (BestLocSoFar &&
        //  We only report unqualified types, but the best Loc for such a type
        //  is the qualified Loc (if present). So, when checking that
        //  `BestLocSoFar` is a valid TypeLoc for `T`, compare the canonical
        //  types of `BestLocSoFar`'s *unqualified* type and `T`.
        BestLocSoFar->getType()->getCanonicalTypeUnqualified() !=
            T->getCanonicalTypeInternal()) {
      BestLocSoFar = std::nullopt;
    }
    derived().report(T, File, PendingNullability, BestLocSoFar);
    PendingNullability.reset();
    BestLocSoFar = std::nullopt;
  }

 public:
  NullabilityWalker(FileID File) : File(File) {}

  void visit(TypeLoc Loc) { visit(Loc.getType(), Loc); }
  void visit(QualType T, std::optional<TypeLoc> L = std::nullopt) {
    visit(T.getTypePtr(), L);
  }
  void visit(const TemplateArgument &TA,
             std::optional<TemplateArgumentLoc> TAL) {
    switch (TA.getKind()) {
      case TemplateArgument::Type: {
        const auto *ArgTypeSourceInfo =
            TAL ? TAL->getTypeSourceInfo() : nullptr;
        auto ArgLoc =
            ArgTypeSourceInfo != nullptr
                ? std::optional<TypeLoc>(ArgTypeSourceInfo->getTypeLoc())
                : std::nullopt;
        // Always prefer a template argument Loc over a broader Loc for a type
        // defined as equal to a template argument, e.g. for the type
        // `std::vector<int *>::value_type`, prefer to report the Loc for the
        // `int *` template argument rather than the entire type, since the
        // value_type alias is equal to the template parameter.
        if (ArgLoc) recordLoc(*ArgLoc, /*OverrideElaborated=*/true);
        visit(TA.getAsType(), ArgLoc);
        break;
      }
      case TemplateArgument::Pack: {
        for (const auto &PackElt : TA.getPackAsArray())
          visit(PackElt, std::nullopt);
        break;
      }
      default:
        // Don't handle non-type template arguments.
        break;
    }
  }
  void visit(absl::Nonnull<const DeclContext *> DC) {
    // For now, only consider enclosing classes.
    // TODO: The nullability of template functions can affect local classes too,
    // this can be relevant e.g. when instantiating templates with such types.
    if (auto *CRD = dyn_cast<CXXRecordDecl>(DC))
      visit(DC->getParentASTContext().getRecordType(CRD), std::nullopt);
  }

  void visit(absl::Nonnull<const Type *> T, std::optional<TypeLoc> L) {
    if (L) recordLoc(*L);
    Base::visit(T, L);
  }

  void visitType(absl::Nonnull<const Type *> T, std::optional<TypeLoc> L) {
    // For sugar not explicitly handled below, desugar and continue.
    // (We need to walk the full structure of the canonical type.)
    if (auto *Desugar =
            T->getLocallyUnqualifiedSingleStepDesugaredType().getTypePtr();
        Desugar != T) {
      // We can't arbitrarily desugar TypeLocs the way we can for types, so we
      // don't collect more TypeLocs from this point in.
      return visit(Desugar, std::nullopt);
    }

    // We don't expect to see any nullable non-sugar types except PointerType
    // and `RecordType`s that correspond to smart pointers.
    ignoreUnexpectedNullability();
    Base::visitType(T, L);
  }

  void visitFunctionProtoType(absl::Nonnull<const FunctionProtoType *> FPT,
                              std::optional<FunctionProtoTypeLoc> L) {
    ignoreUnexpectedNullability();
    visit(FPT->getReturnType(),
          L ? std::optional<TypeLoc>(L->getReturnLoc()) : std::nullopt);
    if (L) {
      CHECK(FPT->getParamTypes().size() == L->getNumParams());
    }
    for (unsigned I = 0, N = FPT->getParamTypes().size(); I < N; ++I) {
      std::optional<TypeLoc> ParamLoc;
      if (L) {
        const auto *ParamDecl = L->getParam(I);
        CHECK(ParamDecl);
        if (auto *TSI = ParamDecl->getTypeSourceInfo()) {
          ParamLoc = TSI->getTypeLoc();
        }
      }
      visit(FPT->getParamType(I), ParamLoc);
    }
  }

  void visitTemplateSpecializationType(
      absl::Nonnull<const TemplateSpecializationType *> TST,
      std::optional<TemplateSpecializationTypeLoc> L) {
    if (TST->isTypeAlias()) {
      if (auto NK = getAliasNullability(TST->getTemplateName()))
        sawNullability(*NK);

      // Aliases are sugar, visit the underlying type.
      // Record template args so we can resugar substituted params.
      //
      // TODO(b/281474380): `TemplateSpecializationType::template_arguments()`
      // doesn't contain defaulted arguments. Can we fetch or compute these in
      // sugared form?
      TemplateContext Ctx{
          /*AssociatedDecl=*/TST->getTemplateName().getAsTemplateDecl(),
          /*Args=*/TST->template_arguments(),
          /*ArgsFile=*/File,
          /*Extends=*/CurrentTemplateContext,
          /*ArgContext=*/CurrentTemplateContext,
      };
      if (L) {
        Ctx.ArgLocs = std::vector<TemplateArgumentLoc>();
        Ctx.ArgLocs->reserve(L->getNumArgs());
        for (unsigned I = 0, N = L->getNumArgs(); I < N; ++I) {
          Ctx.ArgLocs->push_back(L->getArgLoc(I));
        }
      }
      llvm::SaveAndRestore<const TemplateContext *> UseAlias(
          CurrentTemplateContext, &Ctx);
      llvm::SaveAndRestore SwitchFile(
          File, getGoverningFile(TST->getTemplateName().getAsTemplateDecl()));
      visitType(TST, L);
      return;
    }

    auto *CRD = TST->getAsCXXRecordDecl();
    CHECK(CRD) << "Expected an alias or class specialization in concrete code";
    if (isSupportedSmartPointerType(QualType(TST, 0))) {
      report(TST);
    } else {
      ignoreUnexpectedNullability();
    }
    visit(CRD->getDeclContext());

    ArrayRef<TemplateArgument> TSTArgs = TST->template_arguments();
    CHECK(!L || TSTArgs.size() == L->getNumArgs());

    for (unsigned I = 0; I < TSTArgs.size(); ++I) {
      visit(TSTArgs[I], L ? std::optional<TemplateArgumentLoc>(L->getArgLoc(I))
                          : std::nullopt);
    }

    // `TSTArgs` doesn't contain any default arguments.
    // Retrieve these (though in unsugared form) from the
    // `ClassTemplateSpecializationDecl`.
    // TODO(b/281474380): Can we fetch or compute default arguments in sugared
    // form?
    if (auto *CTSD = dyn_cast<ClassTemplateSpecializationDecl>(CRD)) {
      for (unsigned I = TSTArgs.size(); I < CTSD->getTemplateArgs().size();
           ++I) {
        visit(CTSD->getTemplateArgs()[I], std::nullopt);
      }
    }
  }

  void visitSubstTemplateTypeParmType(
      absl::Nonnull<const SubstTemplateTypeParmType *> T,
      std::optional<SubstTemplateTypeParmTypeLoc> L) {
    // The underlying type of T in the AST has no sugar, as the template has
    // only one body instantiated per canonical args.
    // Instead, try to find the (sugared) template argument that T is bound to.
    for (const auto *Ctx = CurrentTemplateContext; Ctx; Ctx = Ctx->Extends) {
      if (T->getAssociatedDecl() != Ctx->AssociatedDecl) continue;
      // If args are not available, fall back to un-sugared arg.
      if (!Ctx->Args.has_value()) break;
      unsigned Index = T->getIndex();
      // Valid because pack must be the last param in non-function templates.
      // TODO: if we support function templates, we need to be smarter here.
      if (auto PackIndex = T->getPackIndex())
        Index = Ctx->Args->size() - 1 - *PackIndex;

      // TODO(b/281474380): `Args` may be too short if `Index` refers to an
      // arg that was defaulted.  We eventually want to populate
      // `CurrentAliasTemplate->Args` with the default arguments in this case,
      // but for now, we just walk the underlying type without sugar.
      if (Index < Ctx->Args->size()) {
        const TemplateArgument &Arg = (*Ctx->Args)[Index];
        std::optional<TemplateArgumentLoc> ArgLoc;
        if (Ctx->ArgLocs) {
          ArgLoc = (*Ctx->ArgLocs)[Index];
        }
        // When we start to walk a sugared TemplateArgument (in place of T),
        // we must do so in the template instantiation context where the
        // argument was written.
        llvm::SaveAndRestore OriginalContext(
            CurrentTemplateContext, CurrentTemplateContext->ArgContext);
        llvm::SaveAndRestore SwitchFile(File, Ctx->ArgsFile);

        return visit(Arg, ArgLoc);
      }
    }
    // Our top-level type references an unbound type param.
    // Our original input was the underlying type of an  instantiation, we
    // lack the context needed to resugar it.
    // TODO: maybe this could be an assert in some cases (alias params)?
    // We would need to trust all callers are obtaining types appropriately,
    // and that clang never partially-desugars in a problematic way.
    visitType(T, L);
  }

  // If we see foo<args>::ty then we may need sugar from args to resugar ty.
  void visitElaboratedType(absl::Nonnull<const ElaboratedType *> ET,
                           std::optional<ElaboratedTypeLoc> L) {
    std::vector<TemplateContext> BoundTemplateArgs;
    std::optional<NestedNameSpecifierLoc> NNSLoc;
    if (L) {
      NNSLoc = L->getQualifierLoc();
    }
    // Iterate over qualifiers right-to-left, looking for template args.
    for (auto *NNS = ET->getQualifier(); NNS;) {
      // TODO: there are other ways a NNS could bind template args:
      //   template <typename T> foo { struct bar { using baz = T; }; };
      //   using T = foo<int * _Nullable>::bar;
      //   using U = T::baz;
      // Here T:: is not a TemplateSpecializationType (directly or indirectly).
      // Nevertheless it provides sugar that is referenced from baz.
      // Probably we need another type visitor to collect bindings in general.
      if (const auto *TST =
              dyn_cast_or_null<TemplateSpecializationType>(NNS->getAsType())) {
        TemplateContext Ctx;
        Ctx.Args = TST->template_arguments();
        Ctx.ArgsFile = File;
        Ctx.ArgContext = CurrentTemplateContext;
        // `Extends` is initialized below: we chain BoundTemplateArgs together.
        Ctx.AssociatedDecl =
            TST->isTypeAlias() ? TST->getTemplateName().getAsTemplateDecl()
                               : static_cast<Decl *>(TST->getAsCXXRecordDecl());

        if (NNSLoc) {
          if (auto TSTLoc =
                  NNSLoc->getTypeLoc().getAs<TemplateSpecializationTypeLoc>()) {
            Ctx.ArgLocs = std::vector<TemplateArgumentLoc>();
            Ctx.ArgLocs->reserve(TSTLoc.getNumArgs());
            for (unsigned I = 0, N = TSTLoc.getNumArgs(); I < N; ++I) {
              Ctx.ArgLocs->push_back(TSTLoc.getArgLoc(I));
            }
          }
        }

        translateTemplateArgsForSpecialization(Ctx);
        BoundTemplateArgs.push_back(Ctx);
      }

      NNS = NNS->getPrefix();
      if (NNSLoc) {
        NNSLoc = NNSLoc->getPrefix();
      }
    }
    std::optional<llvm::SaveAndRestore<const TemplateContext *>> Restore;
    if (!BoundTemplateArgs.empty()) {
      // Wire up the inheritance chain so all the contexts are visible.
      BoundTemplateArgs.back().Extends = CurrentTemplateContext;
      for (int I = 0; I < BoundTemplateArgs.size() - 1; ++I)
        BoundTemplateArgs[I].Extends = &BoundTemplateArgs[I + 1];
      Restore.emplace(CurrentTemplateContext, &BoundTemplateArgs.front());
    }
    visit(ET->getNamedType(),
          L ? std::optional<TypeLoc>(L->getNamedTypeLoc()) : std::nullopt);
  }

  void visitTypedefType(const TypedefType *T, std::optional<TypedefTypeLoc> L) {
    llvm::SaveAndRestore SwitchFile(File, getGoverningFile(T->getDecl()));
    // Don't look for new Locs inside an alias.
    visitType(T, std::nullopt);
  }

  void visitRecordType(absl::Nonnull<const RecordType *> RT,
                       std::optional<RecordTypeLoc> L) {
    if (isSupportedSmartPointerType(QualType(RT, 0))) {
      report(RT);
    } else {
      ignoreUnexpectedNullability();
    }
    visit(RT->getDecl()->getDeclContext());

    // Visit template arguments of this record type.
    if (auto *CTSD = dyn_cast<ClassTemplateSpecializationDecl>(RT->getDecl())) {
      unsigned I = 0;

      // If we have a sugared template context, use the sugar.
      for (auto Ctx = CurrentTemplateContext; Ctx; Ctx = Ctx->Extends) {
        if (Ctx->AssociatedDecl != CTSD) continue;
        llvm::SaveAndRestore SwitchFile(File, Ctx->ArgsFile);
        llvm::SaveAndRestore OriginalContext(CurrentTemplateContext,
                                             Ctx->ArgContext);
        if (!Ctx->Args) break;
        for (unsigned N = Ctx->Args->size(); I < N; ++I) {
          std::optional<TemplateArgumentLoc> ArgLoc;
          if (Ctx->ArgLocs) {
            ArgLoc = (*Ctx->ArgLocs)[I];
          }
          auto Arg = (*Ctx->Args)[I];
          visit(Arg, ArgLoc);
        }
        break;
      }
      // If we didn't see all the declarations's arguments in the template
      // context, either there wasn't a matching context available or there are
      // defaulted arguments. Visit (remaining) arguments from the declaration,
      // without sugar or location.
      auto DeclArgs = CTSD->getTemplateArgs().asArray();
      for (unsigned N = DeclArgs.size(); I < N; ++I) {
        visit(DeclArgs[I], std::nullopt);
      }
    }
  }

  void visitAttributedType(absl::Nonnull<const AttributedType *> AT,
                           std::optional<AttributedTypeLoc> L) {
    if (auto NK = AT->getImmediateNullability()) sawNullability(*NK);
    visit(AT->getModifiedType(),
          L ? std::optional<TypeLoc>(L->getModifiedLoc()) : std::nullopt);
    CHECK(!PendingNullability.has_value())
        << "Should have been consumed by modified type! "
        << AT->getModifiedType().getAsString();
  }

  void visitPointerType(absl::Nonnull<const PointerType *> PT,
                        std::optional<PointerTypeLoc> L) {
    report(PT);
    visit(PT->getPointeeType(),
          L ? std::optional<TypeLoc>(L->getPointeeLoc()) : std::nullopt);
  }

  void visitReferenceType(absl::Nonnull<const ReferenceType *> RT,
                          std::optional<ReferenceTypeLoc> L) {
    ignoreUnexpectedNullability();
    visit(RT->getPointeeTypeAsWritten(),
          L ? std::optional<TypeLoc>(L->getPointeeLoc()) : std::nullopt);
  }

  void visitArrayType(absl::Nonnull<const ArrayType *> AT,
                      std::optional<ArrayTypeLoc> L) {
    ignoreUnexpectedNullability();
    visit(AT->getElementType(),
          L ? std::optional<TypeLoc>(L->getElementLoc()) : std::nullopt);
  }

  void visitParenType(absl::Nonnull<const ParenType *> PT,
                      std::optional<ParenTypeLoc> L) {
    visit(PT->getInnerType(),
          L ? std::optional<TypeLoc>(L->getInnerLoc()) : std::nullopt);
  }
};

struct CountWalker : public NullabilityWalker<CountWalker> {
  CountWalker() : NullabilityWalker(FileID()) {}
  unsigned Count = 0;
  void report(absl::Nonnull<const Type *>, FileID,
              std::optional<NullabilityKind>, std::optional<TypeLoc>) {
    ++Count;
  }
};
}  // namespace

unsigned countPointersInType(QualType T) {
  // Certain expressions like pseudo-destructors have no type, treat as void.
  // (exprType() cannot fold them to void, as it doesn't have the ASTContext).
  if (T.isNull()) return 0;
  CountWalker PointerCountWalker;
  PointerCountWalker.visit(T);
  return PointerCountWalker.Count;
}

unsigned countPointersInType(absl::Nonnull<const DeclContext *> DC) {
  CountWalker PointerCountWalker;
  PointerCountWalker.visit(DC);
  return PointerCountWalker.Count;
}

unsigned countPointersInType(const TemplateArgument &TA) {
  CountWalker PointerCountWalker;
  PointerCountWalker.visit(TA, std::nullopt);
  return PointerCountWalker.Count;
}

QualType exprType(absl::Nonnull<const Expr *> E) {
  if (E->hasPlaceholderType(BuiltinType::BoundMember))
    return Expr::findBoundMemberType(E);
  return E->getType();
}

unsigned countPointersInType(absl::Nonnull<const Expr *> E) {
  return countPointersInType(exprType(E));
}

NullabilityKind TypeNullabilityDefaults::get(FileID File) const {
  if (FileNullability && File.isValid())
    if (auto It = FileNullability->find(File); It != FileNullability->end())
      return It->second;
  return DefaultNullability;
}

TypeNullability getTypeNullability(
    QualType T, FileID File, const TypeNullabilityDefaults &Defaults,
    llvm::function_ref<GetTypeParamNullability> SubstituteTypeParam) {
  CHECK(!T->isDependentType()) << T.getAsString();

  struct Walker : NullabilityWalker<Walker> {
    std::vector<PointerTypeNullability> Annotations;
    llvm::function_ref<GetTypeParamNullability> SubstituteTypeParam;
    const TypeNullabilityDefaults &Defaults;

    Walker(FileID File, const TypeNullabilityDefaults &Defaults)
        : NullabilityWalker(File), Defaults(Defaults) {}

    void report(absl::Nonnull<const Type *>, FileID File,
                std::optional<NullabilityKind> NK, std::optional<TypeLoc>) {
      if (!NK) NK = Defaults.get(File);
      Annotations.push_back(*NK);
    }

    void visitSubstTemplateTypeParmType(
        absl::Nonnull<const SubstTemplateTypeParmType *> ST,
        std::optional<SubstTemplateTypeParmTypeLoc> L) {
      if (SubstituteTypeParam) {
        if (auto Subst = SubstituteTypeParam(ST)) {
          DCHECK_EQ(Subst->size(),
                    countPointersInType(ST->getCanonicalTypeInternal()))
              << "Substituted nullability has the wrong structure: "
              << QualType(ST, 0).getAsString();
          llvm::append_range(Annotations, *Subst);
          return;
        }
      }
      NullabilityWalker::visitSubstTemplateTypeParmType(ST, std::nullopt);
    }
  } AnnotationVisitor(File, Defaults);

  AnnotationVisitor.SubstituteTypeParam = SubstituteTypeParam;
  AnnotationVisitor.visit(T, std::nullopt);
  return std::move(AnnotationVisitor.Annotations);
}

TypeNullability getTypeNullability(
    TypeLoc TL, const TypeNullabilityDefaults &Defaults,
    llvm::function_ref<GetTypeParamNullability> SubstituteTypeParam) {
  return getTypeNullability(TL.getType(),
                            Defaults.Ctx
                                ? Defaults.Ctx->getSourceManager().getFileID(
                                      TL.getLocalSourceRange().getBegin())
                                : FileID(),
                            Defaults, SubstituteTypeParam);
}

TypeNullability getTypeNullability(
    const ValueDecl &D, const TypeNullabilityDefaults &Defaults,
    llvm::function_ref<GetTypeParamNullability> SubstituteTypeParam) {
  return getTypeNullability(D.getType(), getGoverningFile(&D), Defaults,
                            SubstituteTypeParam);
}

TypeNullability getTypeNullability(
    const TypeDecl &D, const TypeNullabilityDefaults &Defaults,
    llvm::function_ref<GetTypeParamNullability> SubstituteTypeParam) {
  return getTypeNullability(D.getASTContext().getTypeDeclType(&D),
                            getGoverningFile(&D), Defaults,
                            SubstituteTypeParam);
}

TypeNullability unspecifiedNullability(absl::Nonnull<const Expr *> E) {
  return TypeNullability(countPointersInType(E), NullabilityKind::Unspecified);
}

namespace {

// Visitor to rebuild a QualType with explicit nullability.
// Extra AttributedType nodes are added wrapping interior PointerTypes, and
// other sugar is added as needed to allow this (e.g. TypeSpecializationType).
//
// We only have to handle types that have nontrivial nullability vectors, i.e.
// those handled by NullabilityWalker.
// Additionally, we only operate on canonical types (otherwise the sugar we're
// adding could conflict with existing sugar).
//
// This needs to stay in sync with the other algorithms that manipulate
// nullability data structures for particular types: the non-flow-sensitive
// transfer and NullabilityWalker.
struct Rebuilder : public TypeVisitor<Rebuilder, QualType> {
  Rebuilder(const TypeNullability &Nullability, ASTContext &Ctx)
      : Nullability(Nullability), Ctx(Ctx) {}

  bool done() const { return Nullability.empty(); }

  using Base = TypeVisitor<Rebuilder, QualType>;
  using Base::Visit;
  QualType Visit(QualType T) {
    if (T.isNull()) return T;
    return Ctx.getQualifiedType(Visit(T.getTypePtr()), T.getLocalQualifiers());
  }
  TemplateArgument Visit(TemplateArgument TA) {
    if (TA.getKind() == TemplateArgument::Type)
      return TemplateArgument(Visit(TA.getAsType()));
    return TA;
  }

  // Default behavior for unhandled types: do not transform.
  QualType VisitType(absl::Nonnull<const Type *> T) { return QualType(T, 0); }

  QualType VisitPointerType(absl::Nonnull<const PointerType *> PT) {
    CHECK(!Nullability.empty())
        << "Nullability vector too short at " << QualType(PT, 0).getAsString();
    NullabilityKind NK = Nullability.front().concrete();
    Nullability = Nullability.drop_front();

    QualType Rebuilt = Ctx.getPointerType(Visit(PT->getPointeeType()));
    if (NK == NullabilityKind::Unspecified) return Rebuilt;
    return Ctx.getAttributedType(AttributedType::getNullabilityAttrKind(NK),
                                 Rebuilt, Rebuilt);
  }

  QualType VisitRecordType(absl::Nonnull<const RecordType *> RT) {
    if (const auto *CTSD =
            dyn_cast<ClassTemplateSpecializationDecl>(RT->getDecl())) {
      std::vector<TemplateArgument> TransformedArgs;
      for (const auto &Arg : CTSD->getTemplateArgs().asArray())
        TransformedArgs.push_back(Visit(Arg));
      return Ctx.getTemplateSpecializationType(
          TemplateName(CTSD->getSpecializedTemplate()), TransformedArgs,
          QualType(RT, 0));
    }
    return QualType(RT, 0);
  }

  QualType VisitFunctionProtoType(absl::Nonnull<const FunctionProtoType *> T) {
    QualType Ret = Visit(T->getReturnType());
    std::vector<QualType> Params;
    for (const auto &Param : T->getParamTypes()) Params.push_back(Visit(Param));
    return Ctx.getFunctionType(Ret, Params, T->getExtProtoInfo());
  }

  QualType VisitLValueReferenceType(
      absl::Nonnull<const LValueReferenceType *> T) {
    return Ctx.getLValueReferenceType(Visit(T->getPointeeType()));
  }
  QualType VisitRValueReferenceType(
      absl::Nonnull<const RValueReferenceType *> T) {
    return Ctx.getRValueReferenceType(Visit(T->getPointeeType()));
  }

  QualType VisitConstantArrayType(absl::Nonnull<const ConstantArrayType *> AT) {
    return Ctx.getConstantArrayType(Visit(AT->getElementType()), AT->getSize(),
                                    AT->getSizeExpr(), AT->getSizeModifier(),
                                    AT->getIndexTypeCVRQualifiers());
  }
  QualType VisitIncompleteArrayType(
      absl::Nonnull<const IncompleteArrayType *> AT) {
    return Ctx.getIncompleteArrayType(Visit(AT->getElementType()),
                                      AT->getSizeModifier(),
                                      AT->getIndexTypeCVRQualifiers());
  }
  QualType VisitVariableArrayType(absl::Nonnull<const VariableArrayType *> AT) {
    return Ctx.getVariableArrayType(
        Visit(AT->getElementType()), AT->getSizeExpr(), AT->getSizeModifier(),
        AT->getIndexTypeCVRQualifiers(), AT->getBracketsRange());
  }

 private:
  ArrayRef<PointerTypeNullability> Nullability;
  ASTContext &Ctx;
};

}  // namespace

QualType rebuildWithNullability(QualType T, const TypeNullability &Nullability,
                                ASTContext &Ctx) {
  Rebuilder V(Nullability, Ctx);
  QualType Result = V.Visit(T.getCanonicalType());
  CHECK(V.done()) << "Nullability vector[" << Nullability.size()
                  << "] too long for " << T.getAsString();
  return Result;
}

std::string printWithNullability(QualType T, const TypeNullability &Nullability,
                                 ASTContext &Ctx) {
  return rebuildWithNullability(T, Nullability, Ctx)
      .getAsString(Ctx.getPrintingPolicy());
}

std::vector<TypeNullabilityLoc> getTypeNullabilityLocs(TypeLoc Loc) {
  struct Walker : NullabilityWalker<Walker> {
    std::vector<TypeNullabilityLoc> TypeNullabilityLocs;
    unsigned Slot = 0;

    // Ignores any default nullability pragmas.
    Walker() : NullabilityWalker(FileID()) {}

    void report(absl::Nonnull<const Type *> T, FileID,
                std::optional<NullabilityKind> NK, std::optional<TypeLoc> Loc) {
      TypeNullabilityLocs.push_back({Slot, T, Loc, NK});
      ++Slot;
    }
  } LocsWalker;

  LocsWalker.visit(Loc);
  return std::move(LocsWalker.TypeNullabilityLocs);
}

TypeLoc returnTypeLoc(const FunctionDecl &D) {
  if (auto FTL = D.getFunctionTypeLoc()) return FTL.getReturnLoc();
  SourceLocation Loc = D.isImplicit() ? SourceLocation() : D.getLocation();

  return D.getASTContext()
      .getTrivialTypeSourceInfo(D.getReturnType(), Loc)
      ->getTypeLoc();
}

}  // namespace clang::tidy::nullability
