//===- GoldTokens.cpp - Gold Token Implementation -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// Copyright (c) Lock3 Software 2019, all rights reserved.
//
//===----------------------------------------------------------------------===//
//
//  This file defines the Token functions.
//
//===----------------------------------------------------------------------===//

#include "clang/Gold/GoldTokens.h"

#include <iostream>

namespace gold {

char const* getDisplayName(TokenKind K) {
  switch (K) {
#define def_token(TK) \
  case tok::TK: return # TK;
#define def_keyword(TK, S) \
  case tok::TK ## Keyword: return # TK;
#include "clang/Gold/GoldTokens.def"
  }
  assert(false);
}

bool hasUniqueSpelling(TokenKind K) {
  switch (K) {
  case tok::Newline: return true;
  case tok::Separator: return true;
  case tok::Indent: return true;
  case tok::Dedent: return true;
#define def_puncop(TK, S) \
  case tok::TK: return true;
#define def_keyword(TK, S) \
  case tok::TK ## Keyword: return true;
#include "clang/Gold/GoldTokens.def"
  default: return false;
  }
}

bool hasMultipleSpellings(TokenKind K) {
  return !hasUniqueSpelling(K);
}

const char* getSpelling(TokenKind K) {
  switch (K) {
  case tok::EndOfFile: return "end-of-file";
  case tok::Unknown: return "unknown";
  case tok::Indent: return "indent";
  case tok::Dedent: return "dedent";
  case tok::Separator: return "separator";
#define def_puncop(TK, S) \
  case tok::TK: return S;
#define def_keyword(TK, S) \
  case tok::TK ## Keyword: return S;
#include "clang/Gold/GoldTokens.def"
  case tok::Identifier: return "identifier";
  case tok::BinaryInteger: return "binary-integer";
  case tok::DecimalInteger: return "decimal-integer";
  case tok::HexadecimalInteger: return "hexadecimal-integer";
  case tok::DecimalFloat: return "decimal-float";
  case tok::HexadecimalFloat: return "hexadecimal-float";
  case tok::Character: return "character";
  case tok::String: return "string";
  case tok::Invalid: return "invalid";
  default: break;
  }
  assert(false);
}

template<std::size_t N>
static constexpr std::size_t getSize(char const (&A)[N]) {
  return N - 1;
}

std::size_t getTokenLength(TokenKind K) {
  switch (K) {
  default: return 0;
  case tok::Unknown: return 1;
#define def_puncop(TK, S) \
  case tok::TK: return getSize(S);
#define def_keyword(TK, S) \
  case tok::TK ## Keyword: return getSize(S);
#include "clang/Gold/GoldTokens.def"
  }
}

llvm::StringRef Token::getSpelling() const {
  // FIXME: Generate a spelling for fused tokens? This would probably
  // need to be a std::string instead of a character pointer.
  assert(!isFused());
  return Sym.data();
}

bool Token::hasSuffix() const {
  return isNumericConstant() && !Suffixes.empty();
}

llvm::SmallVector<llvm::StringRef, 4> Token::getSuffixes() const {
  assert(hasSuffix() && "Token does not have a suffix");
  return Suffixes;
}

void Token::setSuffixes(llvm::SmallVectorImpl<llvm::StringRef> &Sufs) {
  assert(isNumericConstant() && "Token cannot accept a suffix");
  for (auto &Suffix : Sufs)
    Suffixes.push_back(Suffix);
}

void Token::dump() const {
  dump(std::cerr);
}

void Token::dump(std::ostream& OS, bool Nl) const {
  OS << '<';
  OS << getDisplayName(getKind());
  if (*this && hasMultipleSpellings(getKind()))
    OS << ':' << getSpelling().data();
  OS << '>';
  if (Nl)
    OS << '\n';
}

} // namespace gold
