/* -*- Mode: IDL; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * The origin of this IDL file is
 * http://www.whatwg.org/specs/web-apps/current-work/multipage/workers.html
 *
 * © Copyright 2004-2011 Apple Computer, Inc., Mozilla Foundation, and Opera
 * Software ASA.
 * You are granted a license to use, reproduce and create derivative works of
 * this document.
 */

[Constructor(USVString scriptURL, optional WorkerOptions options),
 Func="mozilla::dom::workers::WorkerPrivate::WorkerAvailable",
 Exposed=(Window,DedicatedWorker,SharedWorker,System)]
interface Worker : EventTarget {
  void terminate();

  [Throws]
  void postMessage(any message, optional sequence<object> transfer = []);

  attribute EventHandler onmessage;
};

Worker implements AbstractWorker;

dictionary WorkerOptions {
  // WorkerType type = "classic"; TODO: Bug 1247687
  // RequestCredentials credentials = "omit"; // credentials is only used if type is "module" TODO: Bug 1247687
  DOMString name = "";
};

[Constructor(USVString scriptURL),
 Func="mozilla::dom::workers::ChromeWorkerPrivate::WorkerAvailable",
 Exposed=(Window,DedicatedWorker,SharedWorker,System)]
interface ChromeWorker : Worker {
};
