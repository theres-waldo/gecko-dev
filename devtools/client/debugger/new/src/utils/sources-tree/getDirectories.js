"use strict";

Object.defineProperty(exports, "__esModule", {
  value: true
});
exports.getDirectories = getDirectories;

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at <http://mozilla.org/MPL/2.0/>. */
// import { createParentMap } from "./utils";
function _traverse(subtree, source) {
  if (subtree.type === "source") {
    if (subtree.contents.id === source.id) {
      return subtree;
    }

    return null;
  }

  const matches = subtree.contents.map(child => _traverse(child, source));
  return matches && matches.filter(Boolean)[0];
}

function findSourceItem(sourceTree, source) {
  return _traverse(sourceTree, source);
}

function getAncestors(sourceTree, parentMap, item) {
  if (!item) {
    return null;
  }

  const directories = [];
  directories.push(item);

  while (true) {
    item = parentMap.get(item);

    if (!item) {
      return directories;
    }

    directories.push(item);
  }
}

function getDirectories(source, parentMap, sourceTree) {
  const item = findSourceItem(sourceTree, source);
  const ancestors = getAncestors(sourceTree, parentMap, item);
  return ancestors || [sourceTree];
}