//===--- RewriteSystem.h - Generics with term rewriting ---------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_REWRITESYSTEM_H
#define SWIFT_REWRITESYSTEM_H

#include "swift/AST/Decl.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/LayoutConstraint.h"
#include "swift/AST/ProtocolGraph.h"
#include "swift/AST/Types.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/TinyPtrVector.h"
#include <algorithm>

namespace llvm {
  class raw_ostream;
}

namespace swift {

namespace rewriting {

class Atom final {
  using Storage = llvm::PointerUnion<Identifier,
                                     GenericTypeParamType *,
                                     LayoutConstraint>;

  llvm::TinyPtrVector<const ProtocolDecl *> Protos;
  Storage Value;

  explicit Atom(llvm::TinyPtrVector<const ProtocolDecl *> protos,
                Storage value)
      : Protos(protos), Value(value) {
    // Triggers assertion if the atom is not valid.
    (void) getKind();
  }

public:
  static Atom forName(Identifier name) {
    return Atom({}, name);
  }

  static Atom forProtocol(const ProtocolDecl *proto) {
    return Atom({proto}, Storage());
  }

  static Atom forAssociatedType(const ProtocolDecl *proto,
                                Identifier name) {
    assert(proto != nullptr);
    return Atom({proto}, name);
  }

  static Atom forAssociatedType(
      llvm::TinyPtrVector<const ProtocolDecl *>protos,
      Identifier name) {
    assert(!protos.empty());
    return Atom(protos, name);
  }

  static Atom forGenericParam(GenericTypeParamType *param) {
    assert(param->isCanonical());
    return Atom({}, param);
  }

  static Atom forLayout(LayoutConstraint layout) {
    assert(layout->isKnownLayout());
    return Atom({}, layout);
  }

  enum class Kind : uint8_t {
    AssociatedType,
    GenericParam,
    Name,
    Protocol,
    Layout
  };

  Kind getKind() const {
    if (!Value) {
      assert(Protos.size() == 1);
      return Kind::Protocol;
    }

    if (Value.is<Identifier>()) {
      if (!Protos.empty())
        return Kind::AssociatedType;
      return Kind::Name;
    }

    if (Value.is<GenericTypeParamType *>()) {
      assert(Protos.empty());
      return Kind::GenericParam;
    }

    if (Value.is<LayoutConstraint>()) {
      assert(Protos.empty());
      return Kind::Layout;
    }

    llvm_unreachable("Bad term rewriting atom");
  }

  Identifier getName() const {
    assert(getKind() == Kind::Name ||
           getKind() == Kind::AssociatedType);
    return Value.get<Identifier>();
  }

  const ProtocolDecl *getProtocol() const {
    assert(getKind() == Kind::Protocol);
    assert(Protos.size() == 1);
    return Protos.front();
  }

  llvm::TinyPtrVector<const ProtocolDecl *> getProtocols() const {
    assert(getKind() == Kind::Protocol ||
           getKind() == Kind::AssociatedType);
    assert(!Protos.empty());
    return Protos;
  }

  GenericTypeParamType *getGenericParam() const {
    assert(getKind() == Kind::GenericParam);
    return Value.get<GenericTypeParamType *>();
  }

  LayoutConstraint getLayoutConstraint() const {
    assert(getKind() == Kind::Layout);
    return Value.get<LayoutConstraint>();
  }

  int compare(Atom other, const ProtocolGraph &protos) const;

  void dump(llvm::raw_ostream &out) const;

  friend bool operator==(Atom lhs, Atom rhs) {
    return (lhs.Protos.size() == rhs.Protos.size() &&
            std::equal(lhs.Protos.begin(), lhs.Protos.end(),
                       rhs.Protos.begin()) &&
            lhs.Value == rhs.Value);
  }

  friend bool operator!=(Atom lhs, Atom rhs) {
    return !(lhs == rhs);
  }
};

class Term final {
  llvm::SmallVector<Atom, 3> Atoms;

public:
  Term() {}

  explicit Term(llvm::SmallVector<Atom, 3> &&atoms)
    : Atoms(atoms) {}

  explicit Term(ArrayRef<Atom> atoms)
    : Atoms(atoms.begin(), atoms.end()) {}

  void add(Atom atom) {
    Atoms.push_back(atom);
  }

  int compare(const Term &other, const ProtocolGraph &protos) const;

  size_t size() const { return Atoms.size(); }

  decltype(Atoms)::const_iterator begin() const { return Atoms.begin(); }
  decltype(Atoms)::const_iterator end() const { return Atoms.end(); }

  decltype(Atoms)::iterator begin() { return Atoms.begin(); }
  decltype(Atoms)::iterator end() { return Atoms.end(); }

  const Atom &operator[](size_t index) const {
    return Atoms[index];
  }

  decltype(Atoms)::const_iterator findSubTerm(const Term &other) const;

  decltype(Atoms)::iterator findSubTerm(const Term &other);

  bool containsSubTerm(const Term &other) const {
    return findSubTerm(other) != end();
  }

  bool rewriteSubTerm(const Term &lhs, const Term &rhs);

  bool checkForOverlap(const Term &other, Term &result) const;

  void dump(llvm::raw_ostream &out) const;
};

class Rule final {
  Term LHS;
  Term RHS;
  bool deleted;

public:
  Rule(const Term &lhs, const Term &rhs)
      : LHS(lhs), RHS(rhs), deleted(false) {}

  bool apply(Term &term) const {
    assert(!deleted);
    return term.rewriteSubTerm(LHS, RHS);
  }

  bool checkForOverlap(const Rule &other, Term &result) const {
    return LHS.checkForOverlap(other.LHS, result);
  }

  bool canReduceLeftHandSide(const Rule &other) const {
    return LHS.containsSubTerm(other.LHS);
  }

  bool isDeleted() const {
    return deleted;
  }

  void markDeleted() {
    assert(!deleted);
    deleted = true;
  }

  unsigned getDepth() const {
    return LHS.size();
  }

  int compare(const Rule &other,
              const ProtocolGraph &protos) const {
    return LHS.compare(other.LHS, protos);
  }

  void dump(llvm::raw_ostream &out) const;
};

class RewriteSystem final {
  std::vector<Rule> Rules;
  ProtocolGraph Protos;
  std::deque<std::pair<unsigned, unsigned>> Worklist;

  unsigned DebugSimplify : 1;
  unsigned DebugAdd : 1;

public:
  explicit RewriteSystem() {
    DebugSimplify = false;
    DebugAdd = false;
  }

  RewriteSystem(const RewriteSystem &) = delete;
  RewriteSystem(RewriteSystem &&) = delete;
  RewriteSystem &operator=(const RewriteSystem &) = delete;
  RewriteSystem &operator=(RewriteSystem &&) = delete;

  const ProtocolGraph &getProtocols() const { return Protos; }

  void initialize(std::vector<std::pair<Term, Term>> &&rules,
                  ProtocolGraph &&protos);

  bool addRule(Term lhs, Term rhs);

  bool simplify(Term &term) const;

  enum class CompletionResult {
    Success,
    MaxIterations,
    MaxDepth
  };

  CompletionResult computeConfluentCompletion(
      unsigned maxIterations,
      unsigned maxDepth);

  void dump(llvm::raw_ostream &out) const;
};

} // end namespace rewriting

} // end namespace swift

#endif
