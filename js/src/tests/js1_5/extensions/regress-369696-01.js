/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//-----------------------------------------------------------------------------
var BUGNUMBER = 369696;
var summary = 'Do not assert: map->depth > 0" in js_LeaveSharpObject';
var actual = '';
var expect = '';


//-----------------------------------------------------------------------------
test();
//-----------------------------------------------------------------------------

function test()
{
  enterFunc ('test');
  printBugNumber(BUGNUMBER);
  printStatus (summary);

  q = [];
  q.__defineGetter__("0", q.toString);
  q[2] = q;
  try
  {
    q.toSource();
    throw new Error("didn't throw");
  }
  catch (e)
  {
    assertEq(e instanceof InternalError, true, "bad error: " + e);
  }

  reportCompare(expect, actual, summary);

  exitFunc ('test');
}
