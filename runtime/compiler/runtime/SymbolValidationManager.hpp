/*******************************************************************************
 * Copyright (c) 2000, 2019 IBM Corp. and others
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/

#ifndef SYMBOL_VALIDATION_MANAGER_INCL
#define SYMBOL_VALIDATION_MANAGER_INCL

#include <algorithm>                       // for std::max, etc
#include <map>
#include <set>
#include <vector>
#include <stddef.h>                        // for NULL
#include <stdint.h>                        // for int32_t, uint8_t, etc
#include "env/jittypes.h"                  // for uintptrj_t
#include "j9.h"
#include "j9nonbuilder.h"
#include "infra/TRlist.hpp"
#include "env/TRMemory.hpp"
#include "env/VMJ9.h"
#include "exceptions/AOTFailure.hpp"
#include "runtime/J9Runtime.hpp"

#define SVM_ASSERT_LOCATION_INNER(line) __FILE__ ":" #line
#define SVM_ASSERT_LOCATION(line) SVM_ASSERT_LOCATION_INNER(line)

#define SVM_ASSERT_IMPL(assertName, nonfatal, condition, condStr, format, ...)                  \
   do                                                                                           \
      {                                                                                         \
      if (!(condition))                                                                         \
         {                                                                                      \
         if (!(nonfatal) && ::TR::SymbolValidationManager::assertionsAreFatal())                \
            ::TR::fatal_assertion(__FILE__, __LINE__, condStr, "" format "", ##__VA_ARGS__);    \
         else                                                                                   \
            traceMsg(::TR::comp(), "" format "\n", ##__VA_ARGS__);                              \
                                                                                                \
         ::TR::comp()->failCompilation< ::J9::AOTSymbolValidationManagerFailure>(               \
            SVM_ASSERT_LOCATION(__LINE__) ": " assertName " failed: " condStr);                 \
         }                                                                                      \
      }                                                                                         \
   while (false)

// For logic errors. This is a fatal assertion in debug mode or when
// TR_svmAssertionsAreFatal is set in the environment. Otherwise it fails safe
// by bailing out of the current compilation or AOT load.
#define SVM_ASSERT(condition, format, ...) \
   SVM_ASSERT_IMPL("SVM_ASSERT", false, condition, #condition, format, ##__VA_ARGS__)

// For unhandled situations that are not necessarily a logic error, e.g.
// exceeding limits. This is never fatal; it always bails out of the current
// compilation or AOT load. Failure should be possible but very rare.
#define SVM_ASSERT_NONFATAL(condition, format, ...) \
   SVM_ASSERT_IMPL("SVM_ASSERT_NONFATAL", true, condition, #condition, format, ##__VA_ARGS__)

#define SVM_ASSERT_ALREADY_VALIDATED(svm, symbol)        \
   do                                                    \
      {                                                  \
      void *_0symbol = (symbol);                         \
      SVM_ASSERT_IMPL(                                   \
         "SVM_ASSERT_ALREADY_VALIDATED",                 \
         false,                                          \
         (svm)->isAlreadyValidated(_0symbol),            \
         "isAlreadyValidated(" #symbol ")",              \
         "%s %p should have already been validated",     \
         #symbol,                                        \
         _0symbol);                                      \
      }                                                  \
   while (false)

namespace TR {

struct SymbolValidationRecord
   {
   SymbolValidationRecord(TR_ExternalRelocationTargetKind kind)
      : _kind(kind)
      {}

   bool isEqual(SymbolValidationRecord *other)
      {
      return !isLessThan(other) && !other->isLessThan(this);
      }

   bool isLessThan(SymbolValidationRecord *other)
      {
      if (_kind < other->_kind)
         return true;
      else if (_kind > other->_kind)
         return false;
      else
         return isLessThanWithinKind(other);
      }

   virtual void printFields() = 0;

   virtual bool isClassValidationRecord() { return false; }

   TR_ExternalRelocationTargetKind _kind;

protected:
   virtual bool isLessThanWithinKind(SymbolValidationRecord *other) = 0;

   template <typename T>
   static T *downcast(T *that, SymbolValidationRecord *record)
      {
      TR_ASSERT(record->_kind == that->_kind, "unexpected SVM record comparison");
      return static_cast<T*>(record);
      }
   };

// Comparison for STL
struct LessSymbolValidationRecord
   {
   bool operator()(SymbolValidationRecord *a, SymbolValidationRecord *b) const
      {
      return a->isLessThan(b);
      }
   };

struct ClassValidationRecord : public SymbolValidationRecord
   {
   ClassValidationRecord(TR_ExternalRelocationTargetKind kind)
      : SymbolValidationRecord(kind)
      {}

   virtual bool isClassValidationRecord() { return true; }
   };

struct ClassByNameRecord : public ClassValidationRecord
   {
   ClassByNameRecord(TR_OpaqueClassBlock *clazz,
                     TR_OpaqueClassBlock *beholder)
      : ClassValidationRecord(TR_ValidateClassByName),
        _class(clazz),
        _beholder(beholder)
      {}

   virtual bool isLessThanWithinKind(SymbolValidationRecord *other);
   virtual void printFields();

   TR_OpaqueClassBlock * _class;
   TR_OpaqueClassBlock * _beholder;
   };

struct ProfiledClassRecord : public ClassValidationRecord
   {
   ProfiledClassRecord(TR_OpaqueClassBlock *clazz, void *classChain)
      : ClassValidationRecord(TR_ValidateProfiledClass),
        _class(clazz), _classChain(classChain)
      {}

   virtual bool isLessThanWithinKind(SymbolValidationRecord *other);
   virtual void printFields();

   TR_OpaqueClassBlock * _class;
   void * _classChain;
   };

struct ClassFromCPRecord : public ClassValidationRecord
   {
   ClassFromCPRecord(TR_OpaqueClassBlock *clazz, TR_OpaqueClassBlock *beholder, uint32_t cpIndex)
      : ClassValidationRecord(TR_ValidateClassFromCP),
        _class(clazz),
        _beholder(beholder),
        _cpIndex(cpIndex)
      {}

   virtual bool isLessThanWithinKind(SymbolValidationRecord *other);
   virtual void printFields();

   TR_OpaqueClassBlock * _class;
   TR_OpaqueClassBlock * _beholder;
   uint32_t  _cpIndex;
   };

struct DefiningClassFromCPRecord : public ClassValidationRecord
   {
   DefiningClassFromCPRecord(TR_OpaqueClassBlock *clazz, TR_OpaqueClassBlock *beholder, uint32_t cpIndex, bool isStatic)
      : ClassValidationRecord(TR_ValidateDefiningClassFromCP),
        _class(clazz),
        _beholder(beholder),
        _cpIndex(cpIndex),
        _isStatic(isStatic)
      {}

   virtual bool isLessThanWithinKind(SymbolValidationRecord *other);
   virtual void printFields();

   TR_OpaqueClassBlock * _class;
   TR_OpaqueClassBlock * _beholder;
   uint32_t  _cpIndex;
   bool      _isStatic;
   };

struct StaticClassFromCPRecord : public ClassValidationRecord
   {
   StaticClassFromCPRecord(TR_OpaqueClassBlock *clazz, TR_OpaqueClassBlock *beholder, uint32_t cpIndex)
      : ClassValidationRecord(TR_ValidateStaticClassFromCP),
        _class(clazz),
        _beholder(beholder),
        _cpIndex(cpIndex)
      {}

   virtual bool isLessThanWithinKind(SymbolValidationRecord *other);
   virtual void printFields();

   TR_OpaqueClassBlock * _class;
   TR_OpaqueClassBlock * _beholder;
   uint32_t  _cpIndex;
   };

struct ClassFromMethodRecord : public ClassValidationRecord
   {
   ClassFromMethodRecord(TR_OpaqueClassBlock *clazz, TR_OpaqueMethodBlock *method)
      : ClassValidationRecord(TR_ValidateClassFromMethod),
        _class(clazz),
        _method(method)
      {}

   virtual bool isLessThanWithinKind(SymbolValidationRecord *other);
   virtual void printFields();

   TR_OpaqueClassBlock * _class;
   TR_OpaqueMethodBlock *_method;
   };

struct ComponentClassFromArrayClassRecord : public ClassValidationRecord
   {
   ComponentClassFromArrayClassRecord(TR_OpaqueClassBlock *componentClass, TR_OpaqueClassBlock *arrayClass)
      : ClassValidationRecord(TR_ValidateComponentClassFromArrayClass),
        _componentClass(componentClass),
        _arrayClass(arrayClass)
      {}

   virtual bool isLessThanWithinKind(SymbolValidationRecord *other);
   virtual void printFields();

   TR_OpaqueClassBlock * _componentClass;
   TR_OpaqueClassBlock * _arrayClass;
   };

struct ArrayClassFromComponentClassRecord : public ClassValidationRecord
   {
   ArrayClassFromComponentClassRecord(TR_OpaqueClassBlock *arrayClass, TR_OpaqueClassBlock *componentClass)
      : ClassValidationRecord(TR_ValidateArrayClassFromComponentClass),
        _arrayClass(arrayClass),
        _componentClass(componentClass)
      {}

   virtual bool isLessThanWithinKind(SymbolValidationRecord *other);
   virtual void printFields();

   TR_OpaqueClassBlock * _arrayClass;
   TR_OpaqueClassBlock * _componentClass;
   };

struct SuperClassFromClassRecord : public ClassValidationRecord
   {
   SuperClassFromClassRecord(TR_OpaqueClassBlock *superClass, TR_OpaqueClassBlock *childClass)
      : ClassValidationRecord(TR_ValidateSuperClassFromClass),
        _superClass(superClass),
        _childClass(childClass)
      {}

   virtual bool isLessThanWithinKind(SymbolValidationRecord *other);
   virtual void printFields();

   TR_OpaqueClassBlock *_superClass;
   TR_OpaqueClassBlock *_childClass;
   };

struct ClassInstanceOfClassRecord : public SymbolValidationRecord
   {
   ClassInstanceOfClassRecord(TR_OpaqueClassBlock *classOne, TR_OpaqueClassBlock *classTwo, bool objectTypeIsFixed, bool castTypeIsFixed, bool isInstanceOf)
      : SymbolValidationRecord(TR_ValidateClassInstanceOfClass),
        _classOne(classOne),
        _classTwo(classTwo),
        _objectTypeIsFixed(objectTypeIsFixed),
        _castTypeIsFixed(castTypeIsFixed),
        _isInstanceOf(isInstanceOf)
      {}

   virtual bool isLessThanWithinKind(SymbolValidationRecord *other);
   virtual void printFields();

   TR_OpaqueClassBlock *_classOne;
   TR_OpaqueClassBlock *_classTwo;
   bool _objectTypeIsFixed;
   bool _castTypeIsFixed;
   bool _isInstanceOf;
   };

struct SystemClassByNameRecord : public ClassValidationRecord
   {
   SystemClassByNameRecord(TR_OpaqueClassBlock *systemClass)
      : ClassValidationRecord(TR_ValidateSystemClassByName),
        _systemClass(systemClass)
      {}

   virtual bool isLessThanWithinKind(SymbolValidationRecord *other);
   virtual void printFields();

   TR_OpaqueClassBlock *_systemClass;
   };

struct ClassFromITableIndexCPRecord : public ClassValidationRecord
   {
   ClassFromITableIndexCPRecord(TR_OpaqueClassBlock *clazz, TR_OpaqueClassBlock *beholder, uint32_t cpIndex)
      : ClassValidationRecord(TR_ValidateClassFromITableIndexCP),
        _class(clazz),
        _beholder(beholder),
        _cpIndex(cpIndex)
      {}

   virtual bool isLessThanWithinKind(SymbolValidationRecord *other);
   virtual void printFields();

   TR_OpaqueClassBlock * _class;
   TR_OpaqueClassBlock * _beholder;
   int32_t _cpIndex;

   };

struct DeclaringClassFromFieldOrStaticRecord : public ClassValidationRecord
   {
   DeclaringClassFromFieldOrStaticRecord(TR_OpaqueClassBlock *clazz, TR_OpaqueClassBlock *beholder, uint32_t cpIndex)
      : ClassValidationRecord(TR_ValidateDeclaringClassFromFieldOrStatic),
        _class(clazz),
        _beholder(beholder),
        _cpIndex(cpIndex)
      {}

   virtual bool isLessThanWithinKind(SymbolValidationRecord *other);
   virtual void printFields();

   TR_OpaqueClassBlock * _class;
   TR_OpaqueClassBlock * _beholder;
   uint32_t  _cpIndex;
   };

struct ClassClassRecord : public ClassValidationRecord
   {
   ClassClassRecord(TR_OpaqueClassBlock *classClass, TR_OpaqueClassBlock *objectClass)
      : ClassValidationRecord(TR_ValidateClassClass),
        _classClass(classClass),
        _objectClass(objectClass)
      {}

   virtual bool isLessThanWithinKind(SymbolValidationRecord *other);
   virtual void printFields();

   TR_OpaqueClassBlock *_classClass;
   TR_OpaqueClassBlock *_objectClass;
   };

struct ConcreteSubClassFromClassRecord : public ClassValidationRecord
   {
   ConcreteSubClassFromClassRecord(TR_OpaqueClassBlock *childClass, TR_OpaqueClassBlock *superClass)
      : ClassValidationRecord(TR_ValidateConcreteSubClassFromClass),
        _childClass(childClass),
        _superClass(superClass)
      {}

   virtual bool isLessThanWithinKind(SymbolValidationRecord *other);
   virtual void printFields();

   TR_OpaqueClassBlock *_childClass;
   TR_OpaqueClassBlock *_superClass;
   };

struct ClassChainRecord : public SymbolValidationRecord
   {
   ClassChainRecord(TR_OpaqueClassBlock *clazz, void *classChain)
      : SymbolValidationRecord(TR_ValidateClassChain),
        _class(clazz),
        _classChain(classChain)
      {}

   virtual bool isLessThanWithinKind(SymbolValidationRecord *other);
   virtual void printFields();

   TR_OpaqueClassBlock *_class;
   void *_classChain;
   };

struct MethodFromClassRecord : public SymbolValidationRecord
   {
   MethodFromClassRecord(TR_OpaqueMethodBlock *method, TR_OpaqueClassBlock *beholder, uint32_t index)
      : SymbolValidationRecord(TR_ValidateMethodFromClass),
        _method(method),
        _beholder(beholder),
        _index(index)
      {}

   virtual bool isLessThanWithinKind(SymbolValidationRecord *other);
   virtual void printFields();

   TR_OpaqueMethodBlock *_method;
   TR_OpaqueClassBlock *_beholder;
   uint32_t _index;
   };

struct StaticMethodFromCPRecord : public SymbolValidationRecord
   {
   StaticMethodFromCPRecord(TR_OpaqueMethodBlock *method,
                               TR_OpaqueClassBlock *beholder,
                               int32_t cpIndex)
      : SymbolValidationRecord(TR_ValidateStaticMethodFromCP),
        _method(method),
        _beholder(beholder),
        _cpIndex(cpIndex)
      {}

   virtual bool isLessThanWithinKind(SymbolValidationRecord *other);
   virtual void printFields();

   TR_OpaqueMethodBlock *_method;
   TR_OpaqueClassBlock *_beholder;
   int32_t _cpIndex;
   };

struct SpecialMethodFromCPRecord : public SymbolValidationRecord
   {
   SpecialMethodFromCPRecord(TR_OpaqueMethodBlock *method,
                             TR_OpaqueClassBlock *beholder,
                             int32_t cpIndex)
      : SymbolValidationRecord(TR_ValidateSpecialMethodFromCP),
        _method(method),
        _beholder(beholder),
        _cpIndex(cpIndex)
      {}

   virtual bool isLessThanWithinKind(SymbolValidationRecord *other);
   virtual void printFields();

   TR_OpaqueMethodBlock *_method;
   TR_OpaqueClassBlock *_beholder;
   int32_t _cpIndex;
   };

struct VirtualMethodFromCPRecord : public SymbolValidationRecord
   {
   VirtualMethodFromCPRecord(TR_OpaqueMethodBlock *method,
                             TR_OpaqueClassBlock *beholder,
                             int32_t cpIndex)
      : SymbolValidationRecord(TR_ValidateVirtualMethodFromCP),
        _method(method),
        _beholder(beholder),
        _cpIndex(cpIndex)
      {}

   virtual bool isLessThanWithinKind(SymbolValidationRecord *other);
   virtual void printFields();

   TR_OpaqueMethodBlock *_method;
   TR_OpaqueClassBlock *_beholder;
   int32_t _cpIndex;
   };

struct VirtualMethodFromOffsetRecord : public SymbolValidationRecord
   {
   VirtualMethodFromOffsetRecord(TR_OpaqueMethodBlock *method,
                                 TR_OpaqueClassBlock *beholder,
                                 int32_t virtualCallOffset,
                                 bool ignoreRtResolve)
      : SymbolValidationRecord(TR_ValidateVirtualMethodFromOffset),
        _method(method),
        _beholder(beholder),
        _virtualCallOffset(virtualCallOffset),
        _ignoreRtResolve(ignoreRtResolve)
      {}

   virtual bool isLessThanWithinKind(SymbolValidationRecord *other);
   virtual void printFields();

   TR_OpaqueMethodBlock *_method;
   TR_OpaqueClassBlock *_beholder;
   int32_t _virtualCallOffset;
   bool _ignoreRtResolve;
   };

struct InterfaceMethodFromCPRecord : public SymbolValidationRecord
   {
   InterfaceMethodFromCPRecord(TR_OpaqueMethodBlock *method,
                               TR_OpaqueClassBlock *beholder,
                               TR_OpaqueClassBlock *lookup,
                               int32_t cpIndex)
      : SymbolValidationRecord(TR_ValidateInterfaceMethodFromCP),
        _method(method),
        _beholder(beholder),
        _lookup(lookup),
        _cpIndex(cpIndex)
      {}

   virtual bool isLessThanWithinKind(SymbolValidationRecord *other);
   virtual void printFields();

   TR_OpaqueMethodBlock *_method;
   TR_OpaqueClassBlock *_beholder;
   TR_OpaqueClassBlock *_lookup;
   int32_t _cpIndex;
   };

struct MethodFromClassAndSigRecord : public SymbolValidationRecord
   {
   MethodFromClassAndSigRecord(TR_OpaqueMethodBlock *method,
                               TR_OpaqueClassBlock *methodClass,
                               TR_OpaqueClassBlock *beholder)
      : SymbolValidationRecord(TR_ValidateMethodFromClassAndSig),
        _method(method),
        _methodClass(methodClass),
        _beholder(beholder)
      {}

   virtual bool isLessThanWithinKind(SymbolValidationRecord *other);
   virtual void printFields();

   TR_OpaqueMethodBlock *_method;
   TR_OpaqueClassBlock *_methodClass;
   TR_OpaqueClassBlock *_beholder;
   };

struct StackWalkerMaySkipFramesRecord : public SymbolValidationRecord
   {
   StackWalkerMaySkipFramesRecord(TR_OpaqueMethodBlock *method,
                                  TR_OpaqueClassBlock *methodClass,
                                  bool skipFrames)
      : SymbolValidationRecord(TR_ValidateStackWalkerMaySkipFramesRecord),
        _method(method),
        _methodClass(methodClass),
        _skipFrames(skipFrames)
      {}

   virtual bool isLessThanWithinKind(SymbolValidationRecord *other);
   virtual void printFields();

   TR_OpaqueMethodBlock *_method;
   TR_OpaqueClassBlock *_methodClass;
   bool _skipFrames;
   };

struct ClassInfoIsInitialized : public SymbolValidationRecord
   {
   ClassInfoIsInitialized(TR_OpaqueClassBlock *clazz, bool isInitialized)
      : SymbolValidationRecord(TR_ValidateClassInfoIsInitialized),
        _class(clazz),
        _isInitialized(isInitialized)
      {}

   virtual bool isLessThanWithinKind(SymbolValidationRecord *other);
   virtual void printFields();

   TR_OpaqueClassBlock *_class;
   bool _isInitialized;
   };

struct MethodFromSingleImplementer : public SymbolValidationRecord
   {
   MethodFromSingleImplementer(TR_OpaqueMethodBlock *method,
                               TR_OpaqueClassBlock *thisClass,
                               int32_t cpIndexOrVftSlot,
                               TR_OpaqueMethodBlock *callerMethod,
                               TR_YesNoMaybe useGetResolvedInterfaceMethod)
      : SymbolValidationRecord(TR_ValidateMethodFromSingleImplementer),
        _method(method),
        _thisClass(thisClass),
        _cpIndexOrVftSlot(cpIndexOrVftSlot),
        _callerMethod(callerMethod),
        _useGetResolvedInterfaceMethod(useGetResolvedInterfaceMethod)
      {}

   virtual bool isLessThanWithinKind(SymbolValidationRecord *other);
   virtual void printFields();

   TR_OpaqueMethodBlock *_method;
   TR_OpaqueClassBlock *_thisClass;
   int32_t _cpIndexOrVftSlot;
   TR_OpaqueMethodBlock *_callerMethod;
   TR_YesNoMaybe _useGetResolvedInterfaceMethod;
   };

struct MethodFromSingleInterfaceImplementer : public SymbolValidationRecord
   {
   MethodFromSingleInterfaceImplementer(TR_OpaqueMethodBlock *method,
                                        TR_OpaqueClassBlock *thisClass,
                                        int32_t cpIndex,
                                        TR_OpaqueMethodBlock *callerMethod)
      : SymbolValidationRecord(TR_ValidateMethodFromSingleInterfaceImplementer),
        _method(method),
        _thisClass(thisClass),
        _cpIndex(cpIndex),
        _callerMethod(callerMethod)
      {}

   virtual bool isLessThanWithinKind(SymbolValidationRecord *other);
   virtual void printFields();

   TR_OpaqueMethodBlock *_method;
   TR_OpaqueClassBlock *_thisClass;
   int32_t _cpIndex;
   TR_OpaqueMethodBlock *_callerMethod;
   };

struct MethodFromSingleAbstractImplementer : public SymbolValidationRecord
   {
   MethodFromSingleAbstractImplementer(TR_OpaqueMethodBlock *method,
                                       TR_OpaqueClassBlock *thisClass,
                                       int32_t vftSlot,
                                       TR_OpaqueMethodBlock *callerMethod)
      : SymbolValidationRecord(TR_ValidateMethodFromSingleAbstractImplementer),
        _method(method),
        _thisClass(thisClass),
        _vftSlot(vftSlot),
        _callerMethod(callerMethod)
      {}

   virtual bool isLessThanWithinKind(SymbolValidationRecord *other);
   virtual void printFields();

   TR_OpaqueMethodBlock *_method;
   TR_OpaqueClassBlock *_thisClass;
   int32_t _vftSlot;
   TR_OpaqueMethodBlock *_callerMethod;
   };

struct ImproperInterfaceMethodFromCPRecord : public SymbolValidationRecord
   {
   ImproperInterfaceMethodFromCPRecord(TR_OpaqueMethodBlock *method,
                               TR_OpaqueClassBlock *beholder,
                               int32_t cpIndex)
      : SymbolValidationRecord(TR_ValidateImproperInterfaceMethodFromCP),
        _method(method),
        _beholder(beholder),
        _cpIndex(cpIndex)
      {}

   virtual bool isLessThanWithinKind(SymbolValidationRecord *other);
   virtual void printFields();

   TR_OpaqueMethodBlock *_method;
   TR_OpaqueClassBlock *_beholder;
   int32_t _cpIndex;
   };

class SymbolValidationManager
   {
public:
   TR_ALLOC(TR_MemoryBase::SymbolValidationManager);

   SymbolValidationManager(TR::Region &region, TR_ResolvedMethod *compilee);

   enum Presence
      {
      SymRequired,
      SymOptional
      };

   void* getSymbolFromID(uint16_t id, TR::SymbolType type, Presence presence = SymRequired);
   TR_OpaqueClassBlock *getClassFromID(uint16_t id, Presence presence = SymRequired);
   J9Class *getJ9ClassFromID(uint16_t id, Presence presence = SymRequired);
   TR_OpaqueMethodBlock *getMethodFromID(uint16_t id, Presence presence = SymRequired);
   J9Method *getJ9MethodFromID(uint16_t id, Presence presence = SymRequired);

   uint16_t tryGetIDFromSymbol(void *symbol);
   uint16_t getIDFromSymbol(void *symbol);

   bool isAlreadyValidated(void *symbol)
      {
      return inHeuristicRegion() || tryGetIDFromSymbol(symbol) != NO_ID;
      }

   bool addClassByNameRecord(TR_OpaqueClassBlock *clazz, TR_OpaqueClassBlock *beholder);
   bool addProfiledClassRecord(TR_OpaqueClassBlock *clazz);
   bool addClassFromCPRecord(TR_OpaqueClassBlock *clazz, J9ConstantPool *constantPoolOfBeholder, uint32_t cpIndex);
   bool addDefiningClassFromCPRecord(TR_OpaqueClassBlock *clazz, J9ConstantPool *constantPoolOfBeholder, uint32_t cpIndex, bool isStatic = false);
   bool addStaticClassFromCPRecord(TR_OpaqueClassBlock *clazz, J9ConstantPool *constantPoolOfBeholder, uint32_t cpIndex);
   bool addClassFromMethodRecord(TR_OpaqueClassBlock *clazz, TR_OpaqueMethodBlock *method);
   bool addComponentClassFromArrayClassRecord(TR_OpaqueClassBlock *componentClass, TR_OpaqueClassBlock *arrayClass);
   bool addArrayClassFromComponentClassRecord(TR_OpaqueClassBlock *arrayClass, TR_OpaqueClassBlock *componentClass);
   bool addSuperClassFromClassRecord(TR_OpaqueClassBlock *superClass, TR_OpaqueClassBlock *childClass);
   bool addClassInstanceOfClassRecord(TR_OpaqueClassBlock *classOne, TR_OpaqueClassBlock *classTwo, bool objectTypeIsFixed, bool castTypeIsFixed, bool isInstanceOf);
   bool addSystemClassByNameRecord(TR_OpaqueClassBlock *systemClass);
   bool addClassFromITableIndexCPRecord(TR_OpaqueClassBlock *clazz, J9ConstantPool *constantPoolOfBeholder, int32_t cpIndex);
   bool addDeclaringClassFromFieldOrStaticRecord(TR_OpaqueClassBlock *clazz, J9ConstantPool *constantPoolOfBeholder, int32_t cpIndex);
   bool addClassClassRecord(TR_OpaqueClassBlock *classClass, TR_OpaqueClassBlock *objectClass);
   bool addConcreteSubClassFromClassRecord(TR_OpaqueClassBlock *childClass, TR_OpaqueClassBlock *superClass);

   bool addMethodFromClassRecord(TR_OpaqueMethodBlock *method, TR_OpaqueClassBlock *beholder, uint32_t index);
   bool addStaticMethodFromCPRecord(TR_OpaqueMethodBlock *method, J9ConstantPool *cp, int32_t cpIndex);
   bool addSpecialMethodFromCPRecord(TR_OpaqueMethodBlock *method, J9ConstantPool *cp, int32_t cpIndex);
   bool addVirtualMethodFromCPRecord(TR_OpaqueMethodBlock *method, J9ConstantPool *cp, int32_t cpIndex);
   bool addVirtualMethodFromOffsetRecord(TR_OpaqueMethodBlock *method, TR_OpaqueClassBlock *beholder, int32_t virtualCallOffset, bool ignoreRtResolve);
   bool addInterfaceMethodFromCPRecord(TR_OpaqueMethodBlock *method, TR_OpaqueClassBlock *beholder, TR_OpaqueClassBlock *lookup, int32_t cpIndex);
   bool addImproperInterfaceMethodFromCPRecord(TR_OpaqueMethodBlock *method, J9ConstantPool *cp, int32_t cpIndex);
   bool addMethodFromClassAndSignatureRecord(TR_OpaqueMethodBlock *method, TR_OpaqueClassBlock *methodClass, TR_OpaqueClassBlock *beholder);
   bool addMethodFromSingleImplementerRecord(TR_OpaqueMethodBlock *method,
                                             TR_OpaqueClassBlock *thisClass,
                                             int32_t cpIndexOrVftSlot,
                                             TR_OpaqueMethodBlock *callerMethod,
                                             TR_YesNoMaybe useGetResolvedInterfaceMethod);
   bool addMethodFromSingleInterfaceImplementerRecord(TR_OpaqueMethodBlock *method,
                                           TR_OpaqueClassBlock *thisClass,
                                           int32_t cpIndex,
                                           TR_OpaqueMethodBlock *callerMethod);
   bool addMethodFromSingleAbstractImplementerRecord(TR_OpaqueMethodBlock *method,
                                          TR_OpaqueClassBlock *thisClass,
                                          int32_t vftSlot,
                                          TR_OpaqueMethodBlock *callerMethod);

   bool addStackWalkerMaySkipFramesRecord(TR_OpaqueMethodBlock *method, TR_OpaqueClassBlock *methodClass, bool skipFrames);
   bool addClassInfoIsInitializedRecord(TR_OpaqueClassBlock *clazz, bool isInitialized);



   bool validateClassByNameRecord(uint16_t classID, uint16_t beholderID, J9ROMClass *romClass);
   bool validateProfiledClassRecord(uint16_t classID, void *classChainIdentifyingLoader, void *classChainForClassBeingValidated);
   bool validateClassFromCPRecord(uint16_t classID, uint16_t beholderID, uint32_t cpIndex);
   bool validateDefiningClassFromCPRecord(uint16_t classID, uint16_t beholderID, uint32_t cpIndex, bool isStatic);
   bool validateStaticClassFromCPRecord(uint16_t classID, uint16_t beholderID, uint32_t cpIndex);
   bool validateClassFromMethodRecord(uint16_t classID, uint16_t methodID);
   bool validateComponentClassFromArrayClassRecord(uint16_t componentClassID, uint16_t arrayClassID);
   bool validateArrayClassFromComponentClassRecord(uint16_t arrayClassID, uint16_t componentClassID);
   bool validateSuperClassFromClassRecord(uint16_t superClassID, uint16_t childClassID);
   bool validateClassInstanceOfClassRecord(uint16_t classOneID, uint16_t classTwoID, bool objectTypeIsFixed, bool castTypeIsFixed, bool wasInstanceOf);
   bool validateSystemClassByNameRecord(uint16_t systemClassID, J9ROMClass *romClass);
   bool validateClassFromITableIndexCPRecord(uint16_t classID, uint16_t beholderID, uint32_t cpIndex);
   bool validateDeclaringClassFromFieldOrStaticRecord(uint16_t definingClassID, uint16_t beholderID, int32_t cpIndex);
   bool validateClassClassRecord(uint16_t classClassID, uint16_t objectClassID);
   bool validateConcreteSubClassFromClassRecord(uint16_t childClassID, uint16_t superClassID);

   bool validateClassChainRecord(uint16_t classID, void *classChain);

   bool validateMethodFromClassRecord(uint16_t methodID, uint16_t beholderID, uint32_t index);
   bool validateStaticMethodFromCPRecord(uint16_t methodID, uint16_t beholderID, int32_t cpIndex);
   bool validateSpecialMethodFromCPRecord(uint16_t methodID, uint16_t beholderID, int32_t cpIndex);
   bool validateVirtualMethodFromCPRecord(uint16_t methodID, uint16_t beholderID, int32_t cpIndex);
   bool validateVirtualMethodFromOffsetRecord(uint16_t methodID, uint16_t beholderID, int32_t virtualCallOffset, bool ignoreRtResolve);
   bool validateInterfaceMethodFromCPRecord(uint16_t methodID, uint16_t beholderID, uint16_t lookupID, int32_t cpIndex);
   bool validateImproperInterfaceMethodFromCPRecord(uint16_t methodID, uint16_t beholderID, int32_t cpIndex);
   bool validateMethodFromClassAndSignatureRecord(uint16_t methodID, uint16_t methodClassID, uint16_t beholderID, J9ROMMethod *romMethod);
   bool validateMethodFromSingleImplementerRecord(uint16_t methodID,
                                                  uint16_t thisClassID,
                                                  int32_t cpIndexOrVftSlot,
                                                  uint16_t callerMethodID,
                                                  TR_YesNoMaybe useGetResolvedInterfaceMethod);
   bool validateMethodFromSingleInterfaceImplementerRecord(uint16_t methodID,
                                                uint16_t thisClassID,
                                                int32_t cpIndex,
                                                uint16_t callerMethodID);
   bool validateMethodFromSingleAbstractImplementerRecord(uint16_t methodID,
                                               uint16_t thisClassID,
                                               int32_t vftSlot,
                                               uint16_t callerMethodID);

   bool validateStackWalkerMaySkipFramesRecord(uint16_t methodID, uint16_t methodClassID, bool couldSkipFrames);
   bool validateClassInfoIsInitializedRecord(uint16_t classID, bool wasInitialized);


   TR_OpaqueClassBlock *getBaseComponentClass(TR_OpaqueClassBlock *clazz, int32_t & numDims);

   typedef TR::list<SymbolValidationRecord *, TR::Region&> SymbolValidationRecordList;

   SymbolValidationRecordList& getValidationRecordList() { return _symbolValidationRecords; }

   void enterHeuristicRegion() { _heuristicRegion++; }
   void exitHeuristicRegion() { _heuristicRegion--; }
   bool inHeuristicRegion() { return (_heuristicRegion > 0); }

   static bool assertionsAreFatal();

private:

   static const uint16_t NO_ID = 0;
   static const uint16_t FIRST_ID = 1;

   uint16_t getNewSymbolID();

   bool shouldNotDefineSymbol(void *symbol) { return symbol == NULL || inHeuristicRegion(); }
   bool abandonRecord(TR::SymbolValidationRecord *record);

   bool recordExists(TR::SymbolValidationRecord *record);
   void appendNewRecord(void *symbol, TR::SymbolValidationRecord *record);
   void appendRecordIfNew(void *symbol, TR::SymbolValidationRecord *record);

   bool addVanillaRecord(void *symbol, TR::SymbolValidationRecord *record);
   bool addClassRecord(TR_OpaqueClassBlock *clazz, TR::ClassValidationRecord *record);
   bool addClassRecordWithRomClass(TR_OpaqueClassBlock *clazz, TR::ClassValidationRecord *record, int arrayDims);
   void addMultipleArrayRecords(TR_OpaqueClassBlock *clazz, int arrayDims);
   bool addMethodRecord(TR_OpaqueMethodBlock *method, TR::SymbolValidationRecord *record);

   bool validateSymbol(uint16_t idToBeValidated, void *validSymbol, TR::SymbolType type);
   bool validateSymbol(uint16_t idToBeValidated, TR_OpaqueClassBlock *clazz);
   bool validateSymbol(uint16_t idToBeValidated, J9Class *clazz);
   bool validateSymbol(uint16_t idToBeValidated, TR_OpaqueMethodBlock *method);
   bool validateSymbol(uint16_t idToBeValidated, J9Method *method);

   void setSymbolOfID(uint16_t id, void *symbol, TR::SymbolType type);
   void defineGuaranteedID(void *symbol, TR::SymbolType type);

   /* Monotonically increasing IDs */
   uint16_t _symbolID;

   uint32_t _heuristicRegion;

   TR::Region &_region;

   TR::Compilation * const _comp;
   J9VMThread * const _vmThread;
   TR_J9VM * const _fej9; // DEFAULT_VM
   TR_Memory * const _trMemory;
   TR_PersistentCHTable * const _chTable;

   /* List of validation records to be written to the AOT buffer */
   SymbolValidationRecordList _symbolValidationRecords;

   typedef TR::typed_allocator<SymbolValidationRecord*, TR::Region&> RecordPtrAlloc;
   typedef std::set<SymbolValidationRecord*, LessSymbolValidationRecord, RecordPtrAlloc> RecordSet;
   RecordSet _alreadyGeneratedRecords;

   typedef TR::typed_allocator<std::pair<void* const, uint16_t>, TR::Region&> SymbolToIdAllocator;
   typedef std::less<void*> SymbolToIdComparator;
   typedef std::map<void*, uint16_t, SymbolToIdComparator, SymbolToIdAllocator> SymbolToIdMap;

   struct TypedSymbol
      {
      void *_symbol;
      TR::SymbolType _type;
      bool _hasValue;
      };

   typedef TR::typed_allocator<TypedSymbol, TR::Region&> IdToSymbolAllocator;
   typedef std::vector<TypedSymbol, IdToSymbolAllocator> IdToSymbolTable;

   typedef TR::typed_allocator<void*, TR::Region&> SeenSymbolsAlloc;
   typedef std::less<void*> SeenSymbolsComparator;
   typedef std::set<void*, SeenSymbolsComparator, SeenSymbolsAlloc> SeenSymbolsSet;

   /* Used for AOT Compile */
   SymbolToIdMap _symbolToIdMap;

   /* Used for AOT Load */
   IdToSymbolTable _idToSymbolTable;

   SeenSymbolsSet _seenSymbolsSet;
   };

}

#endif //SYMBOL_VALIDATION_MANAGER_INCL
