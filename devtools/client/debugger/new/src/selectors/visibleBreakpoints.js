"use strict";

Object.defineProperty(exports, "__esModule", {
  value: true
});
exports.getVisibleBreakpoints = undefined;

var _breakpoints = require("../reducers/breakpoints");

var _sources = require("../reducers/sources");

var _devtoolsSourceMap = require("devtools/client/shared/source-map/index.js");

var _reselect = require("devtools/client/debugger/new/dist/vendors").vendored["reselect"];

var _memoize = require("../utils/memoize");

var _memoize2 = _interopRequireDefault(_memoize);

function _interopRequireDefault(obj) { return obj && obj.__esModule ? obj : { default: obj }; }

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at <http://mozilla.org/MPL/2.0/>. */
function getLocation(breakpoint, isGeneratedSource) {
  return isGeneratedSource ? breakpoint.generatedLocation || breakpoint.location : breakpoint.location;
}

const formatBreakpoint = (0, _memoize2.default)(function (breakpoint, selectedSource) {
  const {
    condition,
    loading,
    disabled,
    hidden
  } = breakpoint;
  const sourceId = selectedSource.id;
  const isGeneratedSource = (0, _devtoolsSourceMap.isGeneratedId)(sourceId);
  return {
    location: getLocation(breakpoint, isGeneratedSource),
    condition,
    loading,
    disabled,
    hidden
  };
});

function isVisible(breakpoint, selectedSource) {
  const sourceId = selectedSource.id;
  const isGeneratedSource = (0, _devtoolsSourceMap.isGeneratedId)(sourceId);
  const location = getLocation(breakpoint, isGeneratedSource);
  return location.sourceId === sourceId;
}
/*
 * Finds the breakpoints, which appear in the selected source.
  */


const getVisibleBreakpoints = exports.getVisibleBreakpoints = (0, _reselect.createSelector)(_sources.getSelectedSource, _breakpoints.getBreakpoints, (selectedSource, breakpoints) => {
  if (!selectedSource) {
    return null;
  }

  return breakpoints.filter(bp => isVisible(bp, selectedSource)).map(bp => formatBreakpoint(bp, selectedSource));
});