/* -*- Mode: Java; c-basic-offset: 4; tab-width: 20; indent-tabs-mode: nil; -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.geckoview;

import org.mozilla.gecko.GeckoThread;
import org.mozilla.gecko.annotation.WrapForJNI;
import org.mozilla.gecko.EventDispatcher;
import org.mozilla.gecko.GeckoAppShell;
import org.mozilla.gecko.PrefsHelper;
import org.mozilla.gecko.util.GeckoBundle;
import org.mozilla.gecko.util.ThreadUtils;
import org.mozilla.gecko.mozglue.JNIObject;

import android.content.Context;
import android.graphics.Matrix;
import android.graphics.Rect;
import android.os.Build;
import android.os.Bundle;
import android.text.InputType;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewParent;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityManager;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeInfo.RangeInfo;
import android.view.accessibility.AccessibilityNodeInfo.CollectionItemInfo;
import android.view.accessibility.AccessibilityNodeInfo.CollectionInfo;
import android.view.accessibility.AccessibilityNodeProvider;

public class SessionAccessibility {
    private static final String LOGTAG = "GeckoAccessibility";

    // This is the number BrailleBack uses to start indexing routing keys.
    private static final int BRAILLE_CLICK_BASE_INDEX = -275000000;
    private static final String ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE =
            "ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE";

    @WrapForJNI static final int FLAG_ACCESSIBILITY_FOCUSED = 0;
    @WrapForJNI static final int FLAG_CHECKABLE = 1 << 1;
    @WrapForJNI static final int FLAG_CHECKED = 1 << 2;
    @WrapForJNI static final int FLAG_CLICKABLE = 1 << 3;
    @WrapForJNI static final int FLAG_CONTENT_INVALID = 1 << 4;
    @WrapForJNI static final int FLAG_CONTEXT_CLICKABLE = 1 << 5;
    @WrapForJNI static final int FLAG_EDITABLE = 1 << 6;
    @WrapForJNI static final int FLAG_ENABLED = 1 << 7;
    @WrapForJNI static final int FLAG_FOCUSABLE = 1 << 8;
    @WrapForJNI static final int FLAG_FOCUSED = 1 << 9;
    @WrapForJNI static final int FLAG_LONG_CLICKABLE = 1 << 10;
    @WrapForJNI static final int FLAG_MULTI_LINE = 1 << 11;
    @WrapForJNI static final int FLAG_PASSWORD = 1 << 12;
    @WrapForJNI static final int FLAG_SCROLLABLE = 1 << 13;
    @WrapForJNI static final int FLAG_SELECTED = 1 << 14;
    @WrapForJNI static final int FLAG_VISIBLE_TO_USER = 1 << 15;
    @WrapForJNI static final int FLAG_SELECTABLE = 1 << 16;


    /* package */ final class NodeProvider extends AccessibilityNodeProvider {
        @Override
        public AccessibilityNodeInfo createAccessibilityNodeInfo(int virtualDescendantId) {
            AccessibilityNodeInfo node = AccessibilityNodeInfo.obtain(mView, virtualDescendantId);
            if (mAttached) {
                populateNodeFromBundle(node, nativeProvider.getNodeInfo(virtualDescendantId));
            } else {
                if (Build.VERSION.SDK_INT < 17 || mView.getDisplay() != null) {
                    // When running junit tests we don't have a display
                    mView.onInitializeAccessibilityNodeInfo(node);
                }
                node.setClassName("android.webkit.WebView");
            }

            node.setAccessibilityFocused(mAccessibilityFocusedNode == virtualDescendantId);
            return node;
        }

        @Override
        public boolean performAction(final int virtualViewId, int action, Bundle arguments) {
            final GeckoBundle data;

            switch (action) {
            case AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS:
                    if (virtualViewId == View.NO_ID) {
                        sendEvent(AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUSED, View.NO_ID, null, null);
                    } else {
                        final GeckoBundle nodeInfo = nativeProvider.getNodeInfo(virtualViewId);
                        final int flags = nodeInfo != null ? nodeInfo.getInt("flags") : 0;
                        if ((flags & FLAG_FOCUSED) != 0) {
                            mSession.getEventDispatcher().dispatch("GeckoView:AccessibilityCursorToFocused", null);
                        } else {
                            sendEvent(AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUSED, virtualViewId, null, nodeInfo);
                        }
                    }
                return true;
            case AccessibilityNodeInfo.ACTION_CLICK:
                mSession.getEventDispatcher().dispatch("GeckoView:AccessibilityActivate", null);
                GeckoBundle nodeInfo = nativeProvider.getNodeInfo(virtualViewId);
                final int flags = nodeInfo != null ? nodeInfo.getInt("flags") : 0;
                if ((flags & (FLAG_SELECTABLE | FLAG_CHECKABLE)) == 0) {
                    sendEvent(AccessibilityEvent.TYPE_VIEW_CLICKED, virtualViewId, null, nodeInfo);
                }
                return true;
            case AccessibilityNodeInfo.ACTION_LONG_CLICK:
                mSession.getEventDispatcher().dispatch("GeckoView:AccessibilityLongPress", null);
                return true;
            case AccessibilityNodeInfo.ACTION_SCROLL_FORWARD:
                mSession.getEventDispatcher().dispatch("GeckoView:AccessibilityScrollForward", null);
                return true;
            case AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD:
                mSession.getEventDispatcher().dispatch("GeckoView:AccessibilityScrollBackward", null);
                return true;
            case AccessibilityNodeInfo.ACTION_SELECT:
                mSession.getEventDispatcher().dispatch("GeckoView:AccessibilitySelect", null);
                return true;
            case AccessibilityNodeInfo.ACTION_NEXT_HTML_ELEMENT:
            case AccessibilityNodeInfo.ACTION_PREVIOUS_HTML_ELEMENT:
                if (arguments != null) {
                    data = new GeckoBundle(1);
                    data.putString("rule", arguments.getString(AccessibilityNodeInfo.ACTION_ARGUMENT_HTML_ELEMENT_STRING));
                } else {
                    data = null;
                }
                mSession.getEventDispatcher().dispatch(action == AccessibilityNodeInfo.ACTION_NEXT_HTML_ELEMENT ?
                                                       "GeckoView:AccessibilityNext" : "GeckoView:AccessibilityPrevious", data);
                return true;
            case AccessibilityNodeInfo.ACTION_NEXT_AT_MOVEMENT_GRANULARITY:
            case AccessibilityNodeInfo.ACTION_PREVIOUS_AT_MOVEMENT_GRANULARITY:
                // XXX: Self brailling gives this action with a bogus argument instead of an actual click action;
                // the argument value is the BRAILLE_CLICK_BASE_INDEX - the index of the routing key that was hit.
                // Other negative values are used by ChromeVox, but we don't support them.
                // FAKE_GRANULARITY_READ_CURRENT = -1
                // FAKE_GRANULARITY_READ_TITLE = -2
                // FAKE_GRANULARITY_STOP_SPEECH = -3
                // FAKE_GRANULARITY_CHANGE_SHIFTER = -4
                int granularity = arguments.getInt(AccessibilityNodeInfo.ACTION_ARGUMENT_MOVEMENT_GRANULARITY_INT);
                if (granularity <= BRAILLE_CLICK_BASE_INDEX) {
                    int keyIndex = BRAILLE_CLICK_BASE_INDEX - granularity;
                    data = new GeckoBundle(1);
                    data.putInt("keyIndex", keyIndex);
                    mSession.getEventDispatcher().dispatch("GeckoView:AccessibilityActivate", data);
                } else if (granularity > 0) {
                    boolean extendSelection = arguments.getBoolean(AccessibilityNodeInfo.ACTION_ARGUMENT_EXTEND_SELECTION_BOOLEAN);
                    data = new GeckoBundle(3);
                    data.putString("direction", action == AccessibilityNodeInfo.ACTION_NEXT_AT_MOVEMENT_GRANULARITY ? "Next" : "Previous");
                    data.putInt("granularity", granularity);
                    data.putBoolean("select", extendSelection);
                    mSession.getEventDispatcher().dispatch("GeckoView:AccessibilityByGranularity", data);
                }
                return true;
            case AccessibilityNodeInfo.ACTION_SET_SELECTION:
                if (arguments == null) {
                    return false;
                }
                int selectionStart = arguments.getInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_START_INT);
                int selectionEnd = arguments.getInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_END_INT);
                data = new GeckoBundle(2);
                data.putInt("start", selectionStart);
                data.putInt("end", selectionEnd);
                mSession.getEventDispatcher().dispatch("GeckoView:AccessibilitySetSelection", data);
                return true;
            case AccessibilityNodeInfo.ACTION_CUT:
            case AccessibilityNodeInfo.ACTION_COPY:
            case AccessibilityNodeInfo.ACTION_PASTE:
                data = new GeckoBundle(1);
                data.putInt("action", action);
                mSession.getEventDispatcher().dispatch("GeckoView:AccessibilityClipboard", data);
                return true;
            case AccessibilityNodeInfo.ACTION_SET_TEXT:
                final String value = arguments.getString(Build.VERSION.SDK_INT >= 21
                        ? AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE
                        : ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE);
                if (mAttached) {
                    nativeProvider.setText(virtualViewId, value);
                }
                return true;
            }

            return mView.performAccessibilityAction(action, arguments);
        }

        @Override
        public AccessibilityNodeInfo findFocus(int focus) {
          if (focus == AccessibilityNodeInfo.FOCUS_ACCESSIBILITY &&
              mAccessibilityFocusedNode != 0) {
            return createAccessibilityNodeInfo(mAccessibilityFocusedNode);
          }

          return super.findFocus(focus);
        }

        private void populateNodeFromBundle(final AccessibilityNodeInfo node, final GeckoBundle nodeInfo) {
            if (mView == null || nodeInfo == null) {
                return;
            }

            boolean isRoot = nodeInfo.getInt("id") == View.NO_ID;
            if (isRoot) {
                if (Build.VERSION.SDK_INT < 17 || mView.getDisplay() != null) {
                    // When running junit tests we don't have a display
                    mView.onInitializeAccessibilityNodeInfo(node);
                }
                node.addAction(AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD);
                node.addAction(AccessibilityNodeInfo.ACTION_SCROLL_FORWARD);
            } else {
                node.setParent(mView, nodeInfo.getInt("parentId", View.NO_ID));
            }

            final int flags = nodeInfo.getInt("flags");

            // The basics
            node.setPackageName(GeckoAppShell.getApplicationContext().getPackageName());
            node.setClassName(nodeInfo.getString("className", "android.view.View"));
            node.setText(nodeInfo.getString("text", ""));

            // Add actions
            node.addAction(AccessibilityNodeInfo.ACTION_NEXT_HTML_ELEMENT);
            node.addAction(AccessibilityNodeInfo.ACTION_PREVIOUS_HTML_ELEMENT);
            node.addAction(AccessibilityNodeInfo.ACTION_CLEAR_ACCESSIBILITY_FOCUS);
            node.addAction(AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS);
            node.addAction(AccessibilityNodeInfo.ACTION_PREVIOUS_AT_MOVEMENT_GRANULARITY);
            node.addAction(AccessibilityNodeInfo.ACTION_NEXT_AT_MOVEMENT_GRANULARITY);
            node.setMovementGranularities(AccessibilityNodeInfo.MOVEMENT_GRANULARITY_CHARACTER |
                    AccessibilityNodeInfo.MOVEMENT_GRANULARITY_WORD |
                    AccessibilityNodeInfo.MOVEMENT_GRANULARITY_LINE |
                    AccessibilityNodeInfo.MOVEMENT_GRANULARITY_PARAGRAPH);
            if ((flags & FLAG_CLICKABLE) != 0) {
                node.addAction(AccessibilityNodeInfo.ACTION_CLICK);
            }


            // Set boolean properties
            node.setAccessibilityFocused((flags & FLAG_ACCESSIBILITY_FOCUSED) != 0);
            node.setCheckable((flags & FLAG_CHECKABLE) != 0);
            node.setChecked((flags & FLAG_CHECKED) != 0);
            node.setClickable((flags & FLAG_CLICKABLE) != 0);
            node.setEnabled((flags & FLAG_ENABLED) != 0);
            node.setFocusable((flags & FLAG_FOCUSABLE) != 0);
            node.setFocused((flags & FLAG_FOCUSED) != 0);
            node.setLongClickable((flags & FLAG_LONG_CLICKABLE) != 0);
            node.setPassword((flags & FLAG_PASSWORD) != 0);
            node.setScrollable((flags & FLAG_SCROLLABLE) != 0);
            node.setSelected((flags & FLAG_SELECTED) != 0);
            node.setVisibleToUser((flags & FLAG_VISIBLE_TO_USER) != 0);
            // Other boolean properties to consider later:
            // setHeading, setImportantForAccessibility, setScreenReaderFocusable, setShowingHintText, setDismissable

            // Bounds
            int[] b = nodeInfo.getIntArray("bounds");
            if (b != null) {
                final Rect screenBounds = new Rect(b[0], b[1], b[2], b[3]);
                node.setBoundsInScreen(screenBounds);

                final Matrix matrix = new Matrix();
                mSession.getClientToScreenMatrix(matrix);
                final float[] origin = new float[2];
                matrix.mapPoints(origin);
                final Rect parentBounds = new Rect(b[0] - (int)origin[0], b[1] - (int)origin[1], b[2], b[3]);
                node.setBoundsInParent(parentBounds);
            }

            // Children
            int[] children = nodeInfo.getIntArray("children");
            if (children != null) {
                for (int childId : children) {
                    node.addChild(mView, childId);
                }
            }

            // SDK 18 and above
            if (Build.VERSION.SDK_INT >= 18) {
                if ((flags & FLAG_EDITABLE) != 0) {
                    node.addAction(AccessibilityNodeInfo.ACTION_SET_SELECTION);
                    node.addAction(AccessibilityNodeInfo.ACTION_CUT);
                    node.addAction(AccessibilityNodeInfo.ACTION_COPY);
                    node.addAction(AccessibilityNodeInfo.ACTION_PASTE);
                    node.setEditable(true);
                }
            }

            // SDK 19 and above
            if (Build.VERSION.SDK_INT >= 19) {
                node.setMultiLine((flags & FLAG_MULTI_LINE) != 0);
                node.setContentInvalid((flags & FLAG_CONTENT_INVALID) != 0);

                // Set bundle keys like role and hint
                Bundle bundle = node.getExtras();
                if (nodeInfo.containsKey("hint")) {
                    final String hint =  nodeInfo.getString("hint");
                    bundle.putCharSequence("AccessibilityNodeInfo.hint", hint);
                    if (Build.VERSION.SDK_INT >= 26) {
                        node.setHintText(hint);
                    }
                }
                if (nodeInfo.containsKey("geckoRole")) {
                    bundle.putCharSequence("AccessibilityNodeInfo.geckoRole", nodeInfo.getString("geckoRole"));
                }
                if (nodeInfo.containsKey("roleDescription")) {
                    bundle.putCharSequence("AccessibilityNodeInfo.roleDescription", nodeInfo.getString("roleDescription"));
                }
                if (isRoot) {
                    // Argument values for ACTION_NEXT_HTML_ELEMENT/ACTION_PREVIOUS_HTML_ELEMENT.
                    // This is mostly here to let TalkBack know we are a legit "WebView".
                    bundle.putCharSequence(
                            "ACTION_ARGUMENT_HTML_ELEMENT_STRING_VALUES",
                            "ARTICLE,BUTTON,CHECKBOX,COMBOBOX,CONTROL," +
                                    "FOCUSABLE,FRAME,GRAPHIC,H1,H2,H3,H4,H5,H6," +
                                    "HEADING,LANDMARK,LINK,LIST,LIST_ITEM,MAIN," +
                                    "MEDIA,RADIO,SECTION,TABLE,TEXT_FIELD," +
                                    "UNVISITED_LINK,VISITED_LINK");
                }


                // Set RangeInfo
                GeckoBundle rangeBundle = nodeInfo.getBundle("rangeInfo");
                if (rangeBundle != null) {
                    final RangeInfo rangeInfo = RangeInfo.obtain(
                            rangeBundle.getInt("type"),
                            (float)rangeBundle.getDouble("min", Float.NEGATIVE_INFINITY),
                            (float)rangeBundle.getDouble("max", Float.POSITIVE_INFINITY),
                            (float)rangeBundle.getDouble("current", 0));
                    node.setRangeInfo(rangeInfo);
                }

                // Set CollectionItemInfo
                GeckoBundle collectionItemBundle = nodeInfo.getBundle("collectionItemInfo");
                if (collectionItemBundle != null) {
                    final CollectionItemInfo collectionItemInfo = CollectionItemInfo.obtain(
                            collectionItemBundle.getInt("rowIndex"),
                            collectionItemBundle.getInt("rowSpan"),
                            collectionItemBundle.getInt("columnIndex"),
                            collectionItemBundle.getInt("columnSpan"), false);
                    node.setCollectionItemInfo(collectionItemInfo);
                }

                // Set CollectionInfo
                GeckoBundle collectionBundle = nodeInfo.getBundle("collectionInfo");
                if (collectionBundle != null) {
                    final CollectionInfo collectionInfo = CollectionInfo.obtain(
                            collectionBundle.getInt("rowCount"),
                            collectionBundle.getInt("columnCount"),
                            collectionBundle.getBoolean("isHierarchical", false),
                            collectionBundle.getInt("selectionMode", 0));
                    node.setCollectionInfo(collectionInfo);
                }

                // Set inputType
                switch (nodeInfo.getString("inputType", "").toLowerCase()) {
                    case "email":
                        node.setInputType(InputType.TYPE_CLASS_TEXT |
                                InputType.TYPE_TEXT_VARIATION_WEB_EMAIL_ADDRESS);
                        break;
                    case "number":
                        node.setInputType(InputType.TYPE_CLASS_NUMBER);
                        break;
                    case "password":
                        node.setInputType(InputType.TYPE_CLASS_TEXT |
                                InputType.TYPE_TEXT_VARIATION_WEB_PASSWORD);
                        break;
                    case "tel":
                        node.setInputType(InputType.TYPE_CLASS_PHONE);
                        break;
                    case "text":
                        node.setInputType(InputType.TYPE_CLASS_TEXT |
                                InputType.TYPE_TEXT_VARIATION_WEB_EDIT_TEXT);
                        break;
                    case "url":
                        node.setInputType(InputType.TYPE_CLASS_TEXT |
                                InputType.TYPE_TEXT_VARIATION_URI);
                        break;
                    default:
                        break;
                }
            }

            // SDK 23 and above
            if (Build.VERSION.SDK_INT >= 23) {
                node.setContextClickable((flags & FLAG_CONTEXT_CLICKABLE) != 0);
            }
        }
    }

    // Gecko session we are proxying
    /* package */  final GeckoSession mSession;
    // This is the view that delegates accessibility to us. We also sends event through it.
    private View mView;
    // The native portion of the node provider.
    /* package */ final NativeProvider nativeProvider = new NativeProvider();
    private boolean mAttached = false;
    // The current node with accessibility focus
    private int mAccessibilityFocusedNode = 0;

    /* package */ SessionAccessibility(final GeckoSession session) {
        mSession = session;
        Settings.updateAccessibilitySettings();
    }

    /**
      * Get the View instance that delegates accessibility to this session.
      *
      * @return View instance.
      */
    public View getView() {
        return mView;
    }

    /**
      * Set the View instance that should delegate accessibility to this session.
      *
      * @param view View instance.
      */
    public void setView(final View view) {
        if (mView != null) {
            mView.setAccessibilityDelegate(null);
        }

        mView = view;

        if (mView == null) {
            return;
        }

        mView.setAccessibilityDelegate(new View.AccessibilityDelegate() {
            private NodeProvider mProvider;

            @Override
            public AccessibilityNodeProvider getAccessibilityNodeProvider(final View hostView) {
                if (hostView != mView) {
                    return null;
                }
                if (mProvider == null) {
                    mProvider = new NodeProvider();
                }
                return mProvider;
            }
        });
    }

    private static class Settings {
        private static final String FORCE_ACCESSIBILITY_PREF = "accessibility.force_disabled";

        private static volatile boolean sEnabled;
        private static volatile boolean sTouchExplorationEnabled;
        /* package */ static volatile boolean sForceEnabled;

        static {
            final Context context = GeckoAppShell.getApplicationContext();
            AccessibilityManager accessibilityManager =
                (AccessibilityManager) context.getSystemService(Context.ACCESSIBILITY_SERVICE);

            accessibilityManager.addAccessibilityStateChangeListener(
            new AccessibilityManager.AccessibilityStateChangeListener() {
                @Override
                public void onAccessibilityStateChanged(boolean enabled) {
                    updateAccessibilitySettings();
                }
            }
            );

            if (Build.VERSION.SDK_INT >= 19) {
                accessibilityManager.addTouchExplorationStateChangeListener(
                new AccessibilityManager.TouchExplorationStateChangeListener() {
                    @Override
                    public void onTouchExplorationStateChanged(boolean enabled) {
                        updateAccessibilitySettings();
                    }
                }
                );
            }

            PrefsHelper.PrefHandler prefHandler = new PrefsHelper.PrefHandlerBase() {
                @Override
                public void prefValue(String pref, int value) {
                    if (pref.equals(FORCE_ACCESSIBILITY_PREF)) {
                        sForceEnabled = value < 0;
                        dispatch();
                    }
                }
            };
            PrefsHelper.addObserver(new String[]{ FORCE_ACCESSIBILITY_PREF }, prefHandler);
        }

        public static boolean isPlatformEnabled() { return sEnabled; }

        public static boolean isEnabled() {
            return sEnabled || sForceEnabled;
        }

        public static boolean isTouchExplorationEnabled() {
            return sTouchExplorationEnabled || sForceEnabled;
        }

        public static void updateAccessibilitySettings() {
            final AccessibilityManager accessibilityManager = (AccessibilityManager)
                    GeckoAppShell.getApplicationContext().getSystemService(Context.ACCESSIBILITY_SERVICE);
            sEnabled = accessibilityManager.isEnabled();
            sTouchExplorationEnabled = sEnabled && accessibilityManager.isTouchExplorationEnabled();
            dispatch();
        }

        /* package */ static void dispatch() {
            final GeckoBundle ret = new GeckoBundle(2);
            ret.putBoolean("touchEnabled", isTouchExplorationEnabled());
            ret.putBoolean("enabled", isEnabled());
            // "GeckoView:AccessibilitySettings" is dispatched to the Gecko thread.
            EventDispatcher.getInstance().dispatch("GeckoView:AccessibilitySettings", ret);
            // "GeckoView:AccessibilityEnabled" is dispatched to the UI thread.
            EventDispatcher.getInstance().dispatch("GeckoView:AccessibilityEnabled", ret);

            if (GeckoThread.isStateAtLeast(GeckoThread.State.PROFILE_READY)) {
                toggleNativeAccessibility(isEnabled());
            } else {
                GeckoThread.queueNativeCallUntil(
                        GeckoThread.State.PROFILE_READY,
                        Settings.class, "toggleNativeAccessibility", isEnabled());
            }
        }

        @WrapForJNI(dispatchTo = "gecko")
        private static native void toggleNativeAccessibility(boolean enable);
    }

    public boolean onMotionEvent(final MotionEvent event) {
        if (!Settings.isTouchExplorationEnabled()) {
            return false;
        }

        if (event.getSource() != InputDevice.SOURCE_TOUCHSCREEN) {
            return false;
        }

        final int action = event.getActionMasked();
        if ((action != MotionEvent.ACTION_HOVER_MOVE) &&
                (action != MotionEvent.ACTION_HOVER_ENTER) &&
                (action != MotionEvent.ACTION_HOVER_EXIT)) {
            return false;
        }

        final GeckoBundle data = new GeckoBundle(2);
        data.putDoubleArray("coordinates", new double[] {event.getRawX(), event.getRawY()});
        mSession.getEventDispatcher().dispatch("GeckoView:AccessibilityExploreByTouch", data);
        return true;
    }

    /* package */ void sendEvent(final int eventType, final int sourceId, final GeckoBundle eventData, final GeckoBundle sourceInfo) {
        ThreadUtils.assertOnUiThread();
        if (mView == null) {
            return;
        }

        if (!Settings.isPlatformEnabled() && (Build.VERSION.SDK_INT < 17 || mView.getDisplay() != null)) {
            // Accessibility could be activated in Gecko via xpcom, for example when using a11y
            // devtools. Here we assure that either Android a11y is *really* enabled, or no
            // display is attached and we must be in a junit test.
            return;
        }
        if (eventType == AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUSED) {
            mAccessibilityFocusedNode = sourceId;
        }

        final AccessibilityEvent event = AccessibilityEvent.obtain(eventType);
        event.setPackageName(GeckoAppShell.getApplicationContext().getPackageName());
        event.setSource(mView, sourceId);
        event.setEnabled(true);

        if (sourceInfo != null) {
            final int flags = sourceInfo.getInt("flags");
            event.setClassName(sourceInfo.getString("className", "android.view.View"));
            event.setChecked((flags & FLAG_CHECKED) != 0);
            event.getText().add(sourceInfo.getString("text", ""));
        }

        if (eventData != null) {
            if (eventData.containsKey("text")) {
                event.getText().add(eventData.getString("text"));
            }
            event.setContentDescription(eventData.getString("description", ""));
            event.setAddedCount(eventData.getInt("addedCount", -1));
            event.setRemovedCount(eventData.getInt("removedCount", -1));
            event.setFromIndex(eventData.getInt("fromIndex", -1));
            event.setItemCount(eventData.getInt("itemCount", -1));
            event.setCurrentItemIndex(eventData.getInt("currentItemIndex", -1));
            event.setBeforeText(eventData.getString("beforeText", ""));
            event.setToIndex(eventData.getInt("toIndex", -1));
            event.setScrollX(eventData.getInt("scrollX", -1));
            event.setScrollY(eventData.getInt("scrollY", -1));
            event.setMaxScrollX(eventData.getInt("maxScrollX", -1));
            event.setMaxScrollY(eventData.getInt("maxScrollY", -1));
        }

        ((ViewParent) mView).requestSendAccessibilityEvent(mView, event);
    }

    /* package */ final class NativeProvider extends JNIObject {
        @WrapForJNI(calledFrom = "ui")
        private void setAttached(final boolean attached) {
            mAttached = attached;
        }

        @Override // JNIObject
        protected void disposeNative() {
            // Disposal happens in native code.
            throw new UnsupportedOperationException();
        }

        @WrapForJNI(dispatchTo = "current")
        public native GeckoBundle getNodeInfo(int id);

        @WrapForJNI(dispatchTo = "gecko")
        public native void setText(int id, String text);

        @WrapForJNI(calledFrom = "gecko", stubName = "SendEvent")
        private void sendEventNative(final int eventType, final int sourceId, final GeckoBundle eventData, final GeckoBundle sourceInfo) {
            ThreadUtils.postToUiThread(new Runnable() {
                @Override
                public void run() {
                    sendEvent(eventType, sourceId, eventData, sourceInfo);
                }
            });
        }
    }
}
