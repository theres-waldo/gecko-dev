/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const Services = require("Services");
const protocol = require("devtools/shared/protocol");
const {LongStringActor} = require("devtools/server/actors/string");
const {DebuggerServer} = require("devtools/server/main");
const {getSystemInfo} = require("devtools/shared/system");
const {deviceSpec} = require("devtools/shared/specs/device");

exports.DeviceActor = protocol.ActorClassWithSpec(deviceSpec, {
  _desc: null,

  getDescription: function() {
    return getSystemInfo();
  },

  screenshotToDataURL: function() {
    const window = Services.wm.getMostRecentWindow(DebuggerServer.chromeWindowType);
    const { devicePixelRatio } = window;
    const canvas = window.document.createElementNS("http://www.w3.org/1999/xhtml", "canvas");
    const width = window.innerWidth;
    const height = window.innerHeight;
    canvas.setAttribute("width", Math.round(width * devicePixelRatio));
    canvas.setAttribute("height", Math.round(height * devicePixelRatio));
    const context = canvas.getContext("2d");
    const flags =
          context.DRAWWINDOW_DRAW_CARET |
          context.DRAWWINDOW_DRAW_VIEW |
          context.DRAWWINDOW_USE_WIDGET_LAYERS;
    context.scale(devicePixelRatio, devicePixelRatio);
    context.drawWindow(window, 0, 0, width, height, "rgb(255,255,255)", flags);
    const dataURL = canvas.toDataURL("image/png");
    return new LongStringActor(this.conn, dataURL);
  }
});
