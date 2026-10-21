import QtQuick 2.4
import SystemUIComponent 1.0

// Home+'s Control Center over other apps. LG's compositor runs this file as a system UI, an
// overlay like LG's own Quick Settings, when the remote's Settings key is pressed and Home+ isn't
// in front: the service opens it (systemUIManager/launch) with the controls' state in its params,
// and the video underneath keeps playing. A second launch (the Settings key again) closes it.
// The panel itself is ControlCenterView.qml, loaded here so that a mistake in it can't take
// anything else down: it's reported, and the service opens LG's Quick Settings instead.
StarfishSystemUIWindow {
    id: root

    property Item view: null

    // Home+'s service, over its local API (POST /api/<what>, with its token): the compositor may only
    // call LG's services over luna, not ours. `control` runs a control and answers with the new state;
    // `overlay` does what this can't (open an app, say that it closed or failed).
    property int port: 47823
    property string token: ""
    function call(what, body, done) {
        var x = new XMLHttpRequest();
        x.onreadystatechange = function () {
            if (x.readyState !== XMLHttpRequest.DONE || !done) return;
            var r = null;
            try { r = JSON.parse(x.responseText); } catch (e) { r = null; }
            if (x.status !== 200 || !r) r = { returnValue: false, errorText: (r && r.errorText) || "Home+ didn't answer" };
            done(r);
        };
        x.open("POST", "http://127.0.0.1:" + port + "/api/" + what);
        x.setRequestHeader("Content-Type", "application/json");
        x.setRequestHeader("X-Homeplus-Token", token);
        x.send(JSON.stringify(body || {}));
    }

    function open(params) {
        if (params.preload) { // compiled now, so the first time it opens is as quick as the rest
            if (view) return; // (it's open: leave it be)
            var p = Qt.createComponent("ControlCenterView.qml");
            if (p.status !== Component.Ready) console.warn("[Home+] Control Center: " + p.errorString());
            root.exit();
            return;
        }
        if (view) { // the Settings key again
            view.close();
            return;
        }
        port = params.port || port;
        token = params.token || "";
        webOSShell.setWindowProperty("_WEBOS_ACCESS_POLICY_KEYS_BACK", "true"); // Back is ours: out of a sheet, editing, then closed
        webOSShell.setWindowProperty("delayCloseWindowTimeout", 1000);          // time to animate out when LG closes it
        var c = Qt.createComponent("ControlCenterView.qml");
        if (c.status !== Component.Ready) {
            failed(c.errorString());
            return;
        }
        var state = params.state || {};
        state.frame = params.frame || ""; // where the service puts frames of the video, for the glass
        view = c.createObject(root, { "cc": state, "host": root });
        if (!view) {
            failed("couldn't create the panel");
            return;
        }
        view.closed.connect(root.quit);
        root.show();
        view.forceActiveFocus();
        shown.start();
    }
    Timer { id: shown; interval: 16; onTriggered: root.call("overlay", { "event": "opened" }) } // after its first frame

    // gone: the service stops taking frames of the video
    function quit() {
        if (view) call("overlay", { "event": "closed" });
        root.exit();
    }

    function failed(why) {
        console.warn("[Home+] Control Center: " + why);
        call("overlay", { "event": "failed", "error": String(why) });
        root.exit();
    }

    Connections {
        target: root.lifeCycleStatus
        onLaunch: root.open(payload.params || payload || {})
        onClose: { // LG closes it (another app, the TV going to sleep): animated, unless it's going to sleep
            if (view && payload.reason !== "powerSuspend") view.close();
            else root.quit();
        }
    }
}
