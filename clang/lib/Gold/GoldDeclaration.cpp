//===- GoldDeclaration.cpp ------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// Copyright (c) Lock3 Software 2019, all rights reserved.
//
//===----------------------------------------------------------------------===//
//
//  Implementation for the gold declaration.
//
//===----------------------------------------------------------------------===//

#include "clang/Gold/GoldDeclaration.h"
#include "clang/Gold/GoldScope.h"
#include "clang/Gold/GoldSyntax.h"

#include "llvm/Support/raw_ostream.h"
namespace gold {

Declaration::~Declaration() {
  delete SavedScope;
}

clang::SourceLocation Declaration::getEndOfDecl() const {
  const Declarator *D = Decl;
  if (!D)
    return clang::SourceLocation();

  if (Init)
    return Init->getLoc();

  while(D->Next) {
    D = D->Next;
  }
  return D->getLoc();
}

// A declarator declares a variable, if it does not declare a function.
bool Declaration::declaresVariable() const {
  return !declaresFunction();
}

bool Declaration::templateHasDefaultParameters() const {
  // TODO: This is necessary for figuring out if a template parameter has
  // delayed evaluation or not.
  llvm_unreachable("This isn't implemented yet, but it may need to be in the "
      "near future.");
}

bool Declaration::declaresInitializedVariable() const {
  return declaresVariable() && Init;
}

bool Declaration::declaresType() const {
  const Declarator* D = Decl;
  if (TypeDcl)
    if (const auto *Atom = dyn_cast<AtomSyntax>(D->getAsType()->getTyExpr()))
      return Atom->getSpelling() == "type";
  return false;
}

bool Declaration::declaresForwardRecordDecl() const {
  if (declaresInitializedVariable()) {
    if (const AtomSyntax *RHS = dyn_cast<AtomSyntax>(Init)) {
      return RHS->hasToken(tok::ClassKeyword)
             || RHS->hasToken(tok::UnionKeyword)
             || RHS->hasToken(tok::EnumKeyword);
    } else if (const CallSyntax *Call = dyn_cast<CallSyntax>(Init)) {
      if (const AtomSyntax *Nm = dyn_cast<AtomSyntax>(Call->getCallee())) {
        return Nm->hasToken(tok::EnumKeyword);
      }
    }
  }
  return false;
}

bool Declaration::declaresTag() const {
  if (Cxx)
    return isa<clang::CXXRecordDecl>(Cxx);
  if (Init)
    if (const MacroSyntax *Macro = dyn_cast<MacroSyntax>(Init)) {
      if (const AtomSyntax *Atom = dyn_cast<AtomSyntax>(Macro->getCall()))
        return Atom->hasToken(tok::ClassKeyword)
               || Atom->hasToken(tok::UnionKeyword)
               || Atom->hasToken(tok::EnumKeyword);
      if (const CallSyntax *ClsWithBases = dyn_cast<CallSyntax>(Macro->getCall()))
        if (const AtomSyntax *Callee
                  = dyn_cast<AtomSyntax>(ClsWithBases->getCallee()))
          return Callee->hasToken(tok::ClassKeyword)
                  || Callee->hasToken(tok::UnionKeyword)
                  || Callee->hasToken(tok::EnumKeyword);
    }
  return false;
}

bool Declaration::getTagName(const AtomSyntax *&NameNode) const {
  if (Init)
    if (const MacroSyntax *Macro = dyn_cast<MacroSyntax>(Init)) {
      if (const AtomSyntax *Atom = dyn_cast<AtomSyntax>(Macro->getCall()))
        if (Atom->hasToken(tok::ClassKeyword)
            || Atom->hasToken(tok::UnionKeyword)
            || Atom->hasToken(tok::EnumKeyword)) {
          NameNode = Atom;
          return true;
        }
      if (const CallSyntax *ClsWithBases = dyn_cast<CallSyntax>(Macro->getCall()))
        if (const AtomSyntax *Callee
                  = dyn_cast<AtomSyntax>(ClsWithBases->getCallee()))
          if (Callee->hasToken(tok::ClassKeyword)
              || Callee->hasToken(tok::UnionKeyword)
              || Callee->hasToken(tok::EnumKeyword)) {
            NameNode = Callee;
            return true;
          }
    }
  return false;
}

bool Declaration::declaresNamespace() const {
  if (Cxx)
    return isa<clang::NamespaceDecl>(Cxx);
  if (const MacroSyntax *Macro = dyn_cast_or_null<MacroSyntax>(Init))
    return cast<AtomSyntax>(Macro->getCall())->hasToken(tok::NamespaceKeyword);
  return false;
}

bool Declaration::declaresTemplateType() const {
  return TemplateParameters && !FunctionDcl;
}

// A declarator declares a function if it's first non-id declarator is
// declares parameters.
bool Declaration::declaresFunction() const {
  return FunctionDcl;
}

bool Declaration::declaresFunctionWithImplicitReturn() const {
  if (declaresFunction() || declaresFunctionTemplate()) {
    if (!Op)
      // Something is very wrong here?!
      return false;
    if (const CallSyntax *Call = dyn_cast<CallSyntax>(Op)){
      if (const AtomSyntax *Name = dyn_cast<AtomSyntax>(Call->getCallee())) {
        if (Name->getSpelling() == "operator'='") {
          return true;
        }
      }
    }
  }
  return false;
}

bool Declaration::declaresPossiblePureVirtualFunction() const {
  if (declaresFunction() || declaresFunctionTemplate()) {
    if (!Op)
      return false;
    if (const CallSyntax *Call = dyn_cast<CallSyntax>(Op))
      if (const AtomSyntax *Name = dyn_cast<AtomSyntax>(Call->getCallee()))
        if (Name->getSpelling() == "operator'='")
          if (const LiteralSyntax *Lit
                                = dyn_cast<LiteralSyntax>(Call->getArgument(1)))
            if (Lit->getToken().getKind() == tok::DecimalInteger)
              if (Lit->getSpelling() == "0")
                return true;
  }
  return false;
}

static bool isSpecialExpectedAssignedFuncValue(const Syntax *Op, TokenKind TK) {
  if (const CallSyntax *Call = dyn_cast<CallSyntax>(Op))
    if (const AtomSyntax *Name = dyn_cast<AtomSyntax>(Call->getCallee()))
      if (Name->getSpelling() == "operator'='")
        if (const AtomSyntax *Atom = dyn_cast<AtomSyntax>(Call->getArgument(1)))
          if (Atom->getToken().getKind() == TK)
            return true;
  return false;
}

bool Declaration::declaresDefaultedFunction() const {
 if (declaresFunction() || declaresFunctionTemplate()) {
    if (!Op)
      return false;
    return isSpecialExpectedAssignedFuncValue(Op, tok::DefaultKeyword);
  }
  return false;
}


bool Declaration::declaresDeletedFunction() const {
 if (declaresFunction() || declaresFunctionTemplate()) {
    if (!Op)
      return false;
    return isSpecialExpectedAssignedFuncValue(Op, tok::DeleteKeyword);
  }
  return false;
}

bool Declaration::declaresMemberVariable() const {
  return declaresVariable() && Cxx && clang::isa<clang::FieldDecl>(Cxx);
}

bool Declaration::declaresMemberFunction() const {
  return declaresFunction() && Cxx && clang::isa<clang::CXXMethodDecl>(Cxx);
}

bool Declaration::declaresConstructor() const {
  return declaresFunction() && Cxx
    && clang::isa<clang::CXXConstructorDecl>(Cxx);
}

bool Declaration::declaresDestructor() const {
  return declaresFunction() && Cxx && clang::isa<clang::CXXConstructorDecl>(Cxx);
}

// A declarator declares a template if it's first non-id declarator is
// declares template parameters.
bool Declaration::declaresFunctionTemplate() const {
  // TODO: In the future we would need to extend this definition to make sure
  // that everything works as expected whe we do have an identifier that
  // is infact also a template name.
  return FunctionDcl && TemplateParameters;
}


bool Declaration::declaresOperatorOverload() const {
  if (!OpInfo)
    return false;
  return declaresFunction();
}

bool Declaration::declaresTypeAlias() const {
  return Cxx && isa<clang::TypeAliasDecl>(Cxx);
}

bool Declaration::declIsStatic() const {
  if (!IdDcl->UnprocessedAttributes)
    return false;

  auto Iter = std::find_if(IdDcl->UnprocessedAttributes->begin(),
      IdDcl->UnprocessedAttributes->end(), [](const Syntax *S) -> bool{
        if (const AtomSyntax *Atom = dyn_cast<AtomSyntax>(S)) {
          if (Atom->getSpelling() == "static") {
            return true;
          }
        }
        return false;
      });
  return Iter != IdDcl->UnprocessedAttributes->end();
}

bool Declaration::declaresFunctionDecl() const {
  return declaresFunction() && !Init;
}

bool Declaration::declaresFunctionDef() const {
  return declaresFunction() && Init;
}

bool Declaration::declaresInlineInitializedStaticVarDecl() const {
  if (!Cxx)
    return false;
  clang::VarDecl *VD = dyn_cast<clang::VarDecl>(Cxx);
  if (!VD)
    return false;
  return VD->isInline() && VD->getStorageClass() == clang::SC_Static;
}

const Syntax *Declaration::getTemplateParams() const {
  if (!TemplateParameters) {
    return nullptr;
  }
  TemplateParamsDeclarator *TPD
                           = cast<TemplateParamsDeclarator>(TemplateParameters);
  return TPD->getParams();
}

const Declarator *Declaration::getFirstTemplateDeclarator() const {
  const Declarator *D = Decl;
  while (D && D->getKind() != DK_TemplateParams) {
    D = D->Next;
  }
  return D;
}

Declarator *Declaration::getFirstTemplateDeclarator() {
  Declarator *D = Decl;
  while (D && D->getKind() != DK_TemplateParams) {
    D = D->Next;
  }
  return D;
}

const Declarator *Declaration::getIdDeclarator() const {
  return IdDcl;
}

Declarator *Declaration::getIdDeclarator() {
  return IdDcl;
}

const Declarator *Declaration::getFirstDeclarator(DeclaratorKind DK) const {
  const Declarator *D = Decl;
  while (D && D->getKind() != DK) {
    D = D->Next;
  }
  return D;
}

Declarator *Declaration::getFirstDeclarator(DeclaratorKind DK) {
  Declarator *D = Decl;
  while (D && D->getKind() != DK) {
    D = D->Next;
  }
  return D;
}

clang::DeclContext *Declaration::getCxxContext() const {
  return clang::Decl::castToDeclContext(Cxx);
}

void Declaration::setPreviousDecl(Declaration *Prev) {
  Prev->Next = this;
  First = Prev->First;
  Next = First;
}

bool Declaration::isDeclaredWithinClass() const {
  const Scope *Cur = ScopeForDecl;
  while(Cur) {
    if (Cur->getKind() == SK_Class)
      return true;
    Cur = Cur->getParent();
  }
  return false;
}
}
