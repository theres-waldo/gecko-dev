// Copyright (C) 2018 the V8 project authors. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.
/*---
esid: sec-function.prototype.tostring
description: >
  toString of Proxy for function target does not throw (Async Generator Function Expression)
info: |
  ...
  If Type(func) is Object and IsCallable(func) is true, then return an
  implementation-dependent String source code representation of func.
  The representation must have the syntax of a NativeFunction.
  ...

  NativeFunction:
    function IdentifierName_opt ( FormalParameters ) { [ native code ] }

features: [async-functions, generators, Proxy]
includes: [nativeFunctionMatcher.js]
---*/

assertNativeFunction(new Proxy(async function * () {}, {}));
assertNativeFunction(new Proxy(async function * () {}, { apply() {} }).apply);

reportCompare(0, 0);
