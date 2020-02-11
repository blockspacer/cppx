//=== GoldElaborator.cpp - Elaboration for Gold Nodes ----------------------==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// Copyright (c) Lock3 Software 2019, all rights reserved.
//
//===----------------------------------------------------------------------===//
//
//  This file implements the Elaborator interface, which creates clang::Decl*
//  nodes out of Gold Syntax nodes.
//
//===----------------------------------------------------------------------===//

#include "clang/AST/DeclarationName.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/DiagnosticSema.h"
#include "clang/Sema/DeclSpec.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/Sema.h"

#include "clang/Gold/GoldSema.h"
#include "clang/Gold/GoldElaborator.h"
#include "clang/Gold/GoldExprElaborator.h"
#include "clang/Gold/GoldStmtElaborator.h"
#include "clang/Gold/GoldSyntaxContext.h"

namespace gold {

Elaborator::Elaborator(SyntaxContext &Context, Sema &SemaRef)
  : Context(Context), SemaRef(SemaRef) {}

clang::Decl *Elaborator::elaborateFile(const Syntax *S) {
  assert(isa<FileSyntax>(S) && "S is not a file");

  startFile(S);

  const FileSyntax *File = cast<FileSyntax>(S);

  // Pass 1. identify declarations in scope.
  for (const Syntax *SS : File->children())
    identifyDecl(SS);

  // Pass 2: elaborate the types.
  for (const Syntax *SS : File->children())
    elaborateDeclType(SS);

  // Pass 3: elaborate definitions.
  for (const Syntax *SS : File->children())
    elaborateDeclInit(SS);

  finishFile(S);

  return Context.CxxAST.getTranslationUnitDecl();
}

void Elaborator::startFile(const Syntax *S) {
  // Enter the global scope.
  SemaRef.enterScope(SK_Namespace, S);

  /// Build the declaration for the global namespace.
  Declaration *D = new Declaration(S);
  D->SavedScope = SemaRef.getCurrentScope();
  D->Cxx = Context.CxxAST.getTranslationUnitDecl();
  SemaRef.pushDecl(D);
}

void Elaborator::finishFile(const Syntax *S) {
  SemaRef.popDecl();
  SemaRef.leaveScope(S);

  // TODO: Any pending semantic analysis to do here?
}

clang::Decl *Elaborator::elaborateDeclType(const Syntax *S) {
  // TODO: Can we elaborate top-level statements? What would they do?
  // Would these equivalent to directives?
  //
  // TODO: Look for module-related declarations.
  //
  // TODO: What should we find for a list of declarators?
  Declaration *D = SemaRef.getCurrentScope()->findDecl(S);
  if (!D)
    return nullptr;

  return elaborateDecl(D);
}

clang::Decl *Elaborator::elaborateDecl(Declaration *D) {
  // FIXME: This almost certainly needs its own elaboration context
  // because we can end up with recursive elaborations of declarations,
  // possibly having cyclic dependencies.
  if (D->declaresFunction())
    return elaborateFunctionDecl(D);
  else
    return elaborateVariableDecl(D);

  // TODO: We should be able to elaborate definitions at this point too.
  // We've already loaded salient identifier tables, so it shouldn't any
  // forward references should be resolvable.
}

// The parameter scope of a function declaration is always found in the
// second declarator.
static Declarator *getFunctionDeclarator(Declarator *D) {
  assert(D->isIdentifier());
  assert(D->Next->isFunction());
  return D->Next;
}

// Returns the function declarator part of D.
static Declarator *getFunctionDeclarator(Declaration *D) {
  assert(D->Decl);
  return getFunctionDeclarator(D->Decl);
}

// Get the Clang parameter declarations for D
static void getFunctionParameters(Declaration *D,
                          llvm::SmallVectorImpl<clang::ParmVarDecl *> &Params) {
  Declarator *FnDecl = getFunctionDeclarator(D);
  const Syntax *ParamList = FnDecl->Data.ParamInfo.Params;
  Scope *ParamScope = FnDecl->Data.ParamInfo.ConstructedScope;
  for (const Syntax *P : ParamList->children()) {
    Declaration *PD = ParamScope->findDecl(P);
    assert(PD->Cxx && "No corresponding declaration");
    Params.push_back(cast<clang::ParmVarDecl>(PD->Cxx));
  }
}

clang::Decl *Elaborator::elaborateFunctionDecl(Declaration *D) {
  // Get the type of the entity.
  clang::DeclContext *Owner = SemaRef.getCurrentCxxDeclContext();

  ExprElaborator TypeElab(Context, SemaRef);
  ExprElaborator::Expression TypeExpr = TypeElab.elaborateTypeExpr(D->Decl);
  if (TypeExpr.isNull()) {
    SemaRef.Diags.Report(D->Op->getLoc(),
                         clang::diag::err_failed_to_translate_type);
    return nullptr;
  }
  clang::TypeSourceInfo *TInfo = TypeExpr.get<clang::TypeSourceInfo *>();

  clang::DeclarationName Name = D->getId();
  clang::SourceLocation Loc = D->Op->getLoc();

  // FIXME: Make sure we have the right storage class.
  clang::FunctionDecl *FD = clang::FunctionDecl::Create(Context.CxxAST, Owner,
                                                        Loc, Loc, Name,
                                                        TInfo->getType(),
                                                        TInfo, clang::SC_Extern);

  if (FD->isMain()) {
    clang::AttributeFactory Attrs;
    clang::DeclSpec DS(Attrs);
    SemaRef.getCxxSema().CheckMain(FD, DS);
  }

  // Update the function parameters.
  llvm::SmallVector<clang::ParmVarDecl *, 4> Params;
  getFunctionParameters(D, Params);
  FD->setParams(Params);

  // Add the declaration and update bindings.
  Owner->addDecl(FD);
  D->Cxx = FD;

  // FIXME: is this necessary for Gold? It enables some more semantic
  // checking, but not all of it is necessarily meaningful to us.
  SemaRef.getCxxSema().PushFunctionScope();
  return FD;
}

static clang::StorageClass getStorageClass(Elaborator &Elab) {
  // FIXME: What is the storage class for a variable? Computed from scope
  // and specifiers probably. We don't have specifiers yet.
  return Elab.SemaRef.getCurrentScope()->isBlockScope()
    ? clang::SC_Auto
    : clang::SC_Static;
}

clang::Decl *Elaborator::elaborateVariableDecl(Declaration *D) {
  if (SemaRef.getCurrentScope()->isParameterScope())
    return elaborateParameterDecl(D);

  // Get the type of the entity.
  clang::DeclContext *Owner = SemaRef.getCurrentCxxDeclContext();

  ExprElaborator TypeElab(Context, SemaRef);
  ExprElaborator::Expression TypeExpr = TypeElab.elaborateTypeExpr(D->Decl);

  if (TypeExpr.isNull()) {
    SemaRef.Diags.Report(D->Op->getLoc(),
                         clang::diag::err_failed_to_translate_type);
    return nullptr;
  }
  clang::TypeSourceInfo *TInfo = TypeExpr.get<clang::TypeSourceInfo *>();

  clang::IdentifierInfo *Id = D->getId();
  clang::SourceLocation Loc = D->Op->getLoc();
  clang::StorageClass SC = getStorageClass(*this);

  // Create the variable and add it to it's owning context.
  clang::VarDecl *VD = clang::VarDecl::Create(Context.CxxAST, Owner, Loc, Loc,
                                              Id, TInfo->getType(), TInfo, SC);
  Owner->addDecl(VD);
  D->Cxx = VD;
  return VD;
}

clang::Decl *Elaborator::elaborateParameterDecl(Declaration *D) {
  // Get type information.
  clang::DeclContext *Owner = SemaRef.getCurrentCxxDeclContext();

  ExprElaborator TypeElab(Context, SemaRef);
  ExprElaborator::Expression TypeExpr = TypeElab.elaborateTypeExpr(D->Decl);
  if (TypeExpr.isNull()) {
    SemaRef.Diags.Report(D->Op->getLoc(),
                         clang::diag::err_failed_to_translate_type);
    return nullptr;
  }
  clang::TypeSourceInfo *TInfo = TypeExpr.get<clang::TypeSourceInfo *>();

  clang::IdentifierInfo *Id = D->getId();
  clang::SourceLocation Loc = D->Op->getLoc();

  // Just return the parameter. We add it to it's function later.
  clang::ParmVarDecl *P = clang::ParmVarDecl::Create(Context.CxxAST, Owner, Loc,
                                                     Loc, Id, TInfo->getType(),
                                                     TInfo, clang::SC_None,
                                                     /*DefaultArg=*/nullptr);
  D->Cxx = P;
  return P;
}

clang::Decl *Elaborator::elaborateDeclSyntax(const Syntax *S) {
  // Identify this as a declaration first.
  identifyDecl(S);

  // Elaborate the declaration.
  Declaration *D = SemaRef.getCurrentScope()->findDecl(S);
  if (D) {
    elaborateDecl(D);
    elaborateDef(D);
    return D->Cxx;
  }

  return nullptr;
}

void Elaborator::elaborateDeclInit(const Syntax *S) {
  // TODO: See elaborateDeclType. We have the same kinds of concerns.
  Declaration *D = SemaRef.getCurrentScope()->findDecl(S);
  if (!D)
    return;
  elaborateDef(D);
}

void Elaborator::elaborateDef(Declaration *D) {
  if (D->declaresFunction())
    elaborateFunctionDef(D);
  else
    elaborateVariableInit(D);
}

void Elaborator::elaborateFunctionDef(Declaration *D) {
  clang::FunctionDecl *FD = cast<clang::FunctionDecl>(D->Cxx);

  if (!D->Init)
    return;

  if (SemaRef.checkForRedefinition<clang::FunctionDecl>(D))
    return;

  SemaRef.pushDecl(D);

  // We saved the parameter scope while elaborating this function's type,
  // so push it on before we enter the function scope.
  Declarator *FnDecl = getFunctionDeclarator(D);
  SemaRef.pushScope(FnDecl->Data.ParamInfo.ConstructedScope);
  SemaRef.enterScope(SK_Function, D->Init);

  // Elaborate the function body.
  StmtElaborator BodyElaborator(Context, SemaRef);
  clang::Stmt *Body = BodyElaborator.elaborateBlock(D->Init);
  FD->setBody(Body);

  // Leave the function scope.
  SemaRef.leaveScope(D->Init);
  // Leave the parameter scope.
  SemaRef.popScope();
  SemaRef.getCxxSema().PopFunctionScopeInfo();

  SemaRef.popDecl();
}

void Elaborator::elaborateVariableInit(Declaration *D) {
  if (!D->Cxx)
    return;
  clang::VarDecl *VD = cast<clang::VarDecl>(D->Cxx);

  if (!D->Init) {
    // FIXME: We probably want to synthesize some kind of initializer here.
    // Not quite sure how we want to do this.
    //
    // FIXME: What if D has type auto? Surely this is an error. For example:
    //
    //    x : auto
    //
    // declares an undeduced-type variable with no initializer. Presumably
    // this should be an error.
    return;
  }

  // FIXME: If we synthesize initializers, this might need to happen before that
  if (SemaRef.checkForRedefinition<clang::VarDecl>(D))
    return;

  // Elaborate the initializer.
  ExprElaborator ExprElab(Context, SemaRef);
  ExprElaborator::Expression Init = ExprElab.elaborateExpr(D->Init);

  // Make sure the initializer was not elaborated as a type.
  if (Init.is<clang::TypeSourceInfo *>()) {
    llvm::errs() << "Expected expression.\n";
    return;
  }

  clang::Expr *InitExpr = Init.get<clang::Expr *>();
  // Perform auto deduction.
  if (VD->getType()->isUndeducedType()) {
    clang::Sema &CxxSema = SemaRef.getCxxSema();
    clang::QualType Ty;
    auto Result = CxxSema.DeduceAutoType(VD->getTypeSourceInfo(), InitExpr, Ty);
    if (Result == clang::Sema::DAR_Failed) {
      // FIXME: Make this a real diagnostic.
      llvm::errs() << "Failed to deduce type of expression.\n";
      return;
    }
    VD->setType(Ty);
  }

  // Update the initializer.
  SemaRef.getCxxSema().AddInitializerToDecl(VD, InitExpr, /*DirectInit=*/true);
  // VD->setInit(InitExpr);
}

// Get the clang::QualType described by an operator':' call.
clang::QualType Elaborator::getOperatorColonType(const CallSyntax *S) const {
  // Get the argument list of an operator':' call. This should have
  // two arguments, the entity (argument 1) and its type (argument 2).

  // Right now this has to be an explicitly named type.
  if (const AtomSyntax *Typename = dyn_cast<AtomSyntax>(S->getArgument(1))) {
    auto BuiltinMapIter = BuiltinTypes.find(Typename->Tok.getSpelling());
    if (BuiltinMapIter == BuiltinTypes.end())
      assert(false && "Only builtin types are supported right now.");

    return BuiltinMapIter->second;
  }

  assert(false && "User defined types are not supported yet.");
}

static Declarator *buildIdDeclarator(const AtomSyntax *S, Declarator *Next) {
  Declarator *D = new Declarator(DK_Identifier, Next);
  D->Data.Id = S;
  return D;
}

static Declarator *buildTypeDeclarator(const Syntax *S, Declarator *Next) {
  assert((isa<AtomSyntax>(S) || isa<CallSyntax>(S)) &&
         "cannot build type-declarator out of given syntax");

  Declarator *D = new Declarator(DK_Type, Next);

  if (const CallSyntax *Call = dyn_cast<CallSyntax>(S)) {
    D->Call = Call;
    D->Data.Type = Next ? Next->getType() : Call->getArgument(1);
  } else if (isa<AtomSyntax>(S)) {
    D->Data.Type = S;
  }

  return D;
}

static Declarator *buildFunctionDeclarator(const CallSyntax *S, Declarator *Next) {
  // FIXME: Store the parameter list.
  Declarator *D = new Declarator(DK_Function, Next);
  D->Call = S;
  D->Data.ParamInfo.Params = S->getArguments();
  return D;
}

static Declarator *buildPointerDeclarator(const CallSyntax *S, 
                                          Declarator *Next) {
  Declarator *D = new Declarator(DK_Pointer, Next);
  D->Call = S;
  return D;
}

/// FIXME: Convert this back to an iterative function, if possible (see
///        disabled iterative version below).
///
/// Analyze and decompose the declarator.
///
/// This is a recursive walk through a series of call nodes. In each step,
/// we build a declarator fragment.
static Declarator *makeDeclarator(Sema &SemaRef, const Syntax *S,
                                  Declarator *Next) {
  // If we find an atom, then we're done.
  if (const AtomSyntax *Atom = dyn_cast<AtomSyntax>(S)) {
    // This might be a typename, in which case, build a type-declarator.
    clang::DeclarationNameInfo DNI(
      {&SemaRef.Context.CxxAST.Idents.get(Atom->getSpelling()), S->getLoc()});
    clang::LookupResult R(SemaRef.getCxxSema(), DNI, clang::Sema::LookupTagName);
    if (SemaRef.lookupUnqualifiedName(R, SemaRef.getCurrentScope()))
      return buildTypeDeclarator(Atom, Next);

    // Otherwise just build an identifier-declarator.
    return buildIdDeclarator(Atom, Next);
  }

  else if(const CallSyntax *Call = dyn_cast<CallSyntax>(S)) {
    const AtomSyntax *Callee = cast<AtomSyntax>(Call->getCallee());
    // Check for "builtin" operators in the declarator.
    if (Callee->getSpelling() == "operator':'") {

      // If the rhs is something complicated, we need to
      // elaborate it recursively.
      if (isa<CallSyntax>(Call->getArgument(1))) {
        // Elaborate rhs, and then elaborate lhs using the completed
        // type-declarator from rhs as the type.
        Next = makeDeclarator(SemaRef, Call->getArgument(1), Next);
        return makeDeclarator(SemaRef, Call->getArgument(0), Next);
      }

      // Otherwise, rhs is just a literal type.
      return makeDeclarator(SemaRef, Call->getArgument(0),
                            buildTypeDeclarator(Call, Next));
    } else if (Callee->getSpelling() == "operator'^'") {
      // We have a pointer operator, so first create a declarator
      // out of its inner type and use that as `Next`.
      Next = makeDeclarator(SemaRef, Call->getArgument(0), Next);

      // Now build a pointer-declarator that owns its inner type and
      // we're done.
      return buildPointerDeclarator(Call, Next);
    }

    // Otherwise, this appears to be a function declarator.
    return makeDeclarator(SemaRef, Callee,
                          buildFunctionDeclarator(Call, Next));
  }

  return nullptr;
}

static Declarator *makeDeclarator(Sema &SemaRef, const Syntax *S) {
  Declarator *D = nullptr;
  return makeDeclarator(SemaRef, S, D);
}

#if 0
/// Analyze and decompose the declarator.
///
/// This is a recursive walk through a series of call nodes. In each step,
/// we build a declarator fragment.
static Declarator *makeDeclarator(Sema &SemaRef, const Syntax *S) {
  Declarator* D = nullptr;

  while (true) {
    // If we find an atom, then we're done.
    if (const auto *Atom = dyn_cast<AtomSyntax>(S)) {
      D = buildIdDeclarator(Atom, D);
      break;
    }

    else if (const auto *Call = dyn_cast<CallSyntax>(S)) {
      const Syntax *Callee = Call->getCallee();
      if (const auto *Atom = dyn_cast<AtomSyntax>(Callee)) {
        // Check for "builtin" operators in the declarator.
        if (Atom->getSpelling() == "operator':'") {

          D = buildTypeDeclarator(Call, D);
          S = Call->getArgument(0);
          continue;
        }

        // Otherwise, this appears to be a function declarator.
        D = buildFunctionDeclarator(Call, D);
        S = Callee;
        continue;
      }
    }

    // FIXME: Is there anything else we can get here?
    return nullptr;
  }

  return D;
}
#endif

static clang::IdentifierInfo *getIdentifier(Elaborator &Elab,
                                            const Declarator *D) {
  if (const auto *Atom = dyn_cast_or_null<AtomSyntax>(D->getId()))
    return &Elab.Context.CxxAST.Idents.get(Atom->getSpelling());
  return nullptr;
}

void Elaborator::identifyDecl(const Syntax *S) {
  // Keep track of whether or not this is an operator'=' call.
  bool OperatorEquals = false;

  // Declarations only appear in calls.
  if (const auto *Call = dyn_cast<CallSyntax>(S)) {
    if (const auto *Callee = dyn_cast<AtomSyntax>(Call->getCallee())) {
      llvm::StringRef Op = Callee->getToken().getSpelling();

      // Unpack the declarator.
      const Syntax *Decl;
      const Syntax *Init;
      if (Op == "operator'='") {
        const auto *Args = cast<ListSyntax>(Call->getArguments());
        Decl = Args->getChild(0);
        Init = Args->getChild(1);
        OperatorEquals = true;
      } else if (Op == "operator'!'") {
        const auto *Args = cast<ListSyntax>(Call->getArguments());

        // Disallow definitions here.
        //
        // FIXME: This should be an error, not an assertion.
        if (SemaRef.getCurrentScope()->isParameterScope())
          assert(false && "Function definition");

        Decl = Args->getChild(0);
        Init = Args->getChild(1);
      } else if (Op == "operator':'") {
        Decl = S;
        Init = nullptr;
      } else {
        // Syntactically, this is not a declaration.
        return;
      }


      // FIXME: I think we can filter out some syntactic forms as
      // non-declarations. For example, the following look like definitions
      // but are actually assignments.
      //
      //    f(x) = 4
      //    a[3] = 5
      //
      // The array case might be tricky to disambiguate, and requires
      // a lookup. If it's the first initialization of the variable, then
      // it must be a declaration. See below.

      // Try to build a declarator for the declaration.
      Declarator *Dcl = makeDeclarator(SemaRef, Decl);
      if (!Dcl)
        return;

      // Parameters can only be declared as x, x:T, or :T. The full range
      // of declarator syntax is not supported.
      //
      // FIXME: Emit an error instead of a diagnostic.
      if (SemaRef.getCurrentScope()->isParameterScope() && !Dcl->isIdentifier())
        assert(false && "Invalid parameter declaration");

      clang::IdentifierInfo* Id = getIdentifier(*this, Dcl);

      Scope *CurScope = SemaRef.getCurrentScope();
      if (CurScope->isBlockScope()) {
        // If we're assigning to a name that already exist in the current block,
        // then we're not declaring anything. For example:
        // \code
        //    x = 3
        //    x = 4
        // \endcode
        // The first statement is a declaration. The second is an assignment.
        if (CurScope->findDecl(Id) && OperatorEquals)
          return;
      }

      // Create a declaration for this node.
      //
      // FIXME: Do a better job managing memory.
      Declaration *ParentDecl = SemaRef.getCurrentDecl();
      Declaration *TheDecl = new Declaration(ParentDecl, S, Dcl, Init);
      TheDecl->Id = Id;

      // If we're in namespace or parameter scope and this identifier already
      // exists, consider it a redeclaration.
      // TODO: distinguish between redefinition, redeclaration, and redeclaration
      // with different type.
      if (CurScope->isNamespaceScope() || CurScope->isParameterScope()) {
        if (Declaration *OldDecl = CurScope->findDecl(Id)) {
          TheDecl->setPreviousDecl(OldDecl);
          // SemaRef.Diags.Report(S->getLoc(), clang::diag::err_redefinition) <<
          //   clang::DeclarationName(Id);
          // return;
        }
      }

      SemaRef.getCurrentScope()->addDecl(TheDecl);
    }
  }

  // FIXME: What other kinds of things are declarations?
  //
  // TODO: If S is a list, then we might be looking at one of these
  //
  //    x, y : int
  //    x, y = foo()
  //
  // We need to elaborate each declarator in the list, and then propagate
  // type information backwards.

  return;
}

  FusedOpKind getFusedOpKind(Sema &SemaRef, llvm::StringRef Spelling) {
  const clang::IdentifierInfo *Tokenization =
    &SemaRef.Context.CxxAST.Idents.get(Spelling);

  if (Tokenization == SemaRef.OperatorColonII)
    return FOK_Colon;
  if (Tokenization == SemaRef.OperatorExclaimII)
    return FOK_Exclaim;
  if (Tokenization == SemaRef.OperatorEqualsII)
    return FOK_Equals;
  if (Tokenization == SemaRef.OperatorIfII)
    return FOK_If;
  if (Tokenization == SemaRef.OperatorElseII)
    return FOK_Else;
  if (Tokenization == SemaRef.OperatorReturnII)
    return FOK_Return;
  if (Tokenization == SemaRef.OperatorReturnsII)
    return FOK_Return;

  return FOK_Unknown;
}

} // namespace gold
