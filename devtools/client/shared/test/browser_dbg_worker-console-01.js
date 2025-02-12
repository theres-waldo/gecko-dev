/* -*- indent-tabs-mode: nil; js-indent-level: 2 -*- */
/* vim: set ft=javascript ts=2 et sw=2 tw=80: */
/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

// Check to make sure that a worker can be attached to a toolbox
// and that the console works.

// Import helpers for the workers
/* import-globals-from helper_workers.js */
Services.scriptloader.loadSubScript(
  "chrome://mochitests/content/browser/devtools/client/shared/test/helper_workers.js",
  this);

var TAB_URL = EXAMPLE_URL + "doc_WorkerTargetActor.attachThread-tab.html";
var WORKER_URL = "code_WorkerTargetActor.attachThread-worker.js";

add_task(async function testNormalExecution() {
  const {client, tab, workerClient, toolbox} =
    await initWorkerDebugger(TAB_URL, WORKER_URL);

  const jsterm = await getSplitConsole(toolbox);
  const executed = await jsterm.execute("this.location.toString()");
  ok(executed.textContent.includes(WORKER_URL),
      "Evaluating the global's location works");

  terminateWorkerInTab(tab, WORKER_URL);
  await waitForWorkerClose(workerClient);
  await gDevTools.closeToolbox(TargetFactory.forWorker(workerClient));
  await close(client);
  await removeTab(tab);
});
