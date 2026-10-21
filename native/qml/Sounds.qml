import QtQuick 2.4
import QtMultimedia 5.0

// Home+'s focus sounds (tools/make-sounds.py): a tick when the focus moves, at most one every 60 ms
// while an arrow is held, and a softer note for OK. Loaded on its own (ControlCenterView.qml), so a
// TV whose compositor can't play sound still gets the Control Center, silent.
Item {
    readonly property bool ready: tickSound.status === SoundEffect.Ready
    property double last: 0
    function tick() {
        var now = Date.now();
        if (now - last < 60) return;
        last = now;
        tickSound.play();
    }
    function select() { selectSound.play(); }
    SoundEffect { id: tickSound; source: "sounds/tick.wav" }
    SoundEffect { id: selectSound; source: "sounds/select.wav" }
}
