// Copyright 2009 the Sputnik authors.  All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
info: >
    String.prototype.split() returns an Array object with:
    i) length equaled to 1,
    ii) [[Get]](0) equaled to the result of converting this object to a string
es5id: 15.5.4.14_A3_T8
description: Instance is Array(1,2,3,4,5)
---*/

var __instance = new Array(1,2,3,4,5);

__instance.split = String.prototype.split;

var __split = __instance.split();

//////////////////////////////////////////////////////////////////////////////
//CHECK#1
if (__split.constructor !== Array) {
  $ERROR('#1: var __instance = new Array(1,2,3,4,5); __instance.split = String.prototype.split; __split = __instance.split(); __split.constructor === Array. Actual: '+__split.constructor );
}
//
//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////
//CHECK#2
if (__split.length !== 1) {
  $ERROR('#2: var __instance = new Array(1,2,3,4,5); __instance.split = String.prototype.split; __split = __instance.split(); __split.length === 1. Actual: '+__split.length );
}
//
//////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////
//CHECK#3
if (__split[0] !== "1,2,3,4,5") {
  $ERROR('#3: var __instance = new Array(1,2,3,4,5); __instance.split = String.prototype.split; __split = __instance.split(); __split[0] === "1,2,3,4,5". Actual: '+__split[0] );
}
//
//////////////////////////////////////////////////////////////////////////////

reportCompare(0, 0);
