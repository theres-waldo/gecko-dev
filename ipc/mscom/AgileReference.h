/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_mscom_AgileReference_h
#define mozilla_mscom_AgileReference_h

#include "mozilla/RefPtr.h"

#include <objidl.h>

namespace mozilla {
namespace mscom {

/**
 * This class encapsulates an "agile reference." These are references that
 * allow you to pass COM interfaces between apartments. When you have an
 * interface that you would like to pass between apartments, you wrap that
 * interface in an AgileReference and pass the agile reference instead. Then
 * you unwrap the interface by calling AgileReference::Resolve.
 *
 * Sample usage:
 *
 * // In the multithreaded apartment, foo is an IFoo*
 * auto myAgileRef = MakeUnique<AgileReference>(IID_IFoo, foo);
 *
 * // myAgileRef is passed to our main thread, which runs in a single-threaded
 * // apartment:
 *
 * RefPtr<IFoo> foo;
 * HRESULT hr = myAgileRef->Resolve(IID_IFoo, getter_AddRefs(foo));
 * // Now foo may be called from the main thread
 */
class AgileReference final
{
public:
  AgileReference();

  template <typename InterfaceT>
  explicit AgileReference(RefPtr<InterfaceT>& aObject)
    : AgileReference(__uuidof(InterfaceT), aObject)
  {
  }

  AgileReference(REFIID aIid, IUnknown* aObject);
  AgileReference(AgileReference&& aOther);

  ~AgileReference();

  explicit operator bool() const
  {
    return mAgileRef || mGitCookie;
  }

  template <typename T>
  void Assign(const RefPtr<T>& aOther)
  {
    Assign(__uuidof(T), aOther);
  }

  template <typename T>
  AgileReference& operator=(const RefPtr<T>& aOther)
  {
    Assign(aOther);
    return *this;
  }

  HRESULT Resolve(REFIID aIid, void** aOutInterface) const;

  AgileReference(const AgileReference& aOther) = delete;
  AgileReference& operator=(const AgileReference& aOther) = delete;

  AgileReference& operator=(AgileReference&& aOther);

  AgileReference& operator=(decltype(nullptr))
  {
    Clear();
    return *this;
  }

  void Clear();

private:
  void Assign(REFIID aIid, IUnknown* aObject);
  void AssignInternal(IUnknown* aObject);
  static IGlobalInterfaceTable* ObtainGit();

private:
  IID                     mIid;
  RefPtr<IAgileReference> mAgileRef;
  DWORD                   mGitCookie;
};

} // namespace mscom
} // namespace mozilla

template <typename T>
RefPtr<T>::RefPtr(const mozilla::mscom::AgileReference& aAgileRef)
  : mRawPtr(nullptr)
{
  (*this) = aAgileRef;
}

template <typename T>
RefPtr<T>&
RefPtr<T>::operator=(const mozilla::mscom::AgileReference& aAgileRef)
{
  void* newRawPtr;
  if (FAILED(aAgileRef.Resolve(__uuidof(T), &newRawPtr))) {
    newRawPtr = nullptr;
  }
  assign_assuming_AddRef(static_cast<T*>(newRawPtr));
  return *this;
}

#endif // mozilla_mscom_AgileReference_h
