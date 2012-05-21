/* -*- Mode: Java; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * TEST DESCRIPTION:
 *
 * This test checks that in a basic history query all transition types visits
 * appear but TRANSITION_EMBED and TRANSITION_FRAMED_LINK ones.
 */

function add_visit(aURI, aType) {
  PlacesUtils.history.addVisit(uri(aURI), Date.now() * 1000, null, aType,
                            false, 0);
}

let transitions = [
  TRANSITION_LINK
, TRANSITION_TYPED
, TRANSITION_BOOKMARK
, TRANSITION_EMBED
, TRANSITION_FRAMED_LINK
, TRANSITION_REDIRECT_PERMANENT
, TRANSITION_REDIRECT_TEMPORARY
, TRANSITION_DOWNLOAD
];

function runQuery(aResultType) {
  let options = PlacesUtils.history.getNewQueryOptions();
  options.resultType = aResultType;
  let root = PlacesUtils.history.executeQuery(PlacesUtils.history.getNewQuery(),
                                              options).root;
  root.containerOpen = true;
  let cc = root.childCount;
  do_check_eq(cc, transitions.length - 2);

  for (let i = 0; i < cc; i++) {
    let node = root.getChild(i);
    // Check that all transition types but EMBED and FRAMED appear in results
    do_check_neq(node.uri.substr(6,1), TRANSITION_EMBED);
    do_check_neq(node.uri.substr(6,1), TRANSITION_FRAMED_LINK);
  }
  root.containerOpen = false;
}

function run_test() {
  // add visits, one for each transition type
  transitions.forEach(function(transition) {
    add_visit("http://" + transition +".mozilla.org/", transition);
  });

  runQuery(Ci.nsINavHistoryQueryOptions.RESULTS_AS_VISIT);
  runQuery(Ci.nsINavHistoryQueryOptions.RESULTS_AS_URI);
}
