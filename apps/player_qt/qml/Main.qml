// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The app shell: video surface, control bar overlay, timeline, and the two
// things every player UI needs and most samples skip -- drag-and-drop to open,
// and a bar that gets out of the way.
//
// The surface and the bar deliberately overlap: the bar floats over the bottom
// of the picture rather than sitting below it, which is Haruna's layout and the
// reason a video can use the window's full height.

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import AvbasePlayer 1.0

ApplicationWindow {
    id: window
    width: 1280
    height: 820
    minimumWidth: 720
    minimumHeight: 520
    visible: true
    color: "#0d0f12"
    title: player.source.length > 0
           ? player.source.split("/").pop() + " \u2014 avbase player"
           : "avbase player \u2014 drop a file"

    // ---- auto-hide ---------------------------------------------------------
    // The bar fades out two and a half seconds after the pointer stops moving
    // over the picture, and comes back on any movement. Driven by a timer
    // rather than by hover alone so a pointer parked over a button does not
    // keep the bar alive forever.
    property bool chromeVisible: true
    property bool timelineVisible: true

    Timer {
        id: hideTimer
        interval: 2500
        running: player.playing && window.chromeVisible
        onTriggered: window.chromeVisible = false
    }

    MouseArea {
        id: activity
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.NoButton
        // Below the chrome in z-order: this only observes, it never eats a
        // click meant for a control.
        z: -1
        onPositionChanged: {
            window.chromeVisible = true;
            hideTimer.restart();
        }
        onEntered: window.chromeVisible = true
    }

    // ---- video surface -----------------------------------------------------
    VideoItem {
        id: video
        // main.cc looks this object up by name to hand it the FrameMailbox.
        objectName: "videoSurface"
        anchors.fill: parent
    }

    // ---- empty state -------------------------------------------------------
    // Shown when there is nothing to show, NOT merely when no frame has
    // arrived: a source that is still opening is a black picture for a moment,
    // and putting "Drop a video here" on top of it reads as "your file was
    // rejected".
    ColumnLayout {
        anchors.centerIn: parent
        spacing: 10
        visible: player.source.length === 0 || player.errorString.length > 0
        opacity: 0.9

        Label {
            Layout.alignment: Qt.AlignHCenter
            text: player.errorString.length > 0 ? "Playback error"
                                                : "Drop a video here"
            color: player.errorString.length > 0 ? "#e8837a" : "#5f6772"
            font.pixelSize: 20
            font.bold: true
        }
        Label {
            Layout.alignment: Qt.AlignHCenter
            Layout.maximumWidth: 560
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
            text: player.errorString.length > 0
                  ? player.errorString
                  : "mp4 / mkv / mov / ts / flv, or a HLS/HTTP URL.\n"
                    + "The timeline below is the editor skeleton."
            color: "#5f6772"
            font.pixelSize: 12
        }
        Button {
            Layout.alignment: Qt.AlignHCenter
            text: "Open\u2026"
            onClicked: fileDialog.open()
        }
    }

    // ---- control bar -------------------------------------------------------
    ControlBar {
        id: bar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: timelineVisible
                        ? timelineBox.top : parent.bottom
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        anchors.bottomMargin: 10
        opacity: window.chromeVisible ? 1.0 : 0.0
        visible: opacity > 0.0
        Behavior on opacity { NumberAnimation { duration: 180 } }

        onSeekPreview: (ratio) => player.seekPreview(ratio * player.duration)
        onSeekCommit: (ratio) => player.seekCommit(ratio * player.duration)
    }

    // ---- timeline ----------------------------------------------------------
    Rectangle {
        id: timelineBox
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        // 34 toolbar + 26 ruler + 4 tracks x 44 = 236, plus a few pixels of
        // margin so the bottom track's border is not clipped. Sizing the panel
        // to the track count rather than to a round number is what stops the
        // last track from being cut in half.
        height: window.timelineVisible ? 242 : 0
        visible: height > 0
        color: "#14171b"
        border.color: "#232830"
        border.width: 1
        clip: true

        Behavior on height { NumberAnimation { duration: 160 } }

        TimelinePanel {
            anchors.fill: parent
            anchors.margins: 1
        }
    }

    // ---- top strip ---------------------------------------------------------
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 44
        color: "#b00d0f12"
        opacity: window.chromeVisible ? 1.0 : 0.0
        visible: opacity > 0.0
        Behavior on opacity { NumberAnimation { duration: 180 } }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            spacing: 10

            Label {
                text: player.source.length > 0
                      ? player.source.split("/").pop() : "no media"
                color: "#c9d0d9"
                font.pixelSize: 12
                elide: Text.ElideMiddle
                Layout.maximumWidth: 420
            }
            Label {
                text: player.stateName
                color: "#6d7681"
                font.pixelSize: 11
                font.family: "Menlo"
            }

            Item { Layout.fillWidth: true }

            Button {
                text: "Open\u2026"
                font.pixelSize: 11
                onClicked: fileDialog.open()
            }
            Button {
                text: window.timelineVisible ? "Hide timeline" : "Show timeline"
                font.pixelSize: 11
                onClicked: window.timelineVisible = !window.timelineVisible
            }
            Button {
                text: "Stats"
                font.pixelSize: 11
                onClicked: statsPopup.open()
            }
            Button {
                text: window.visibility === Window.FullScreen
                      ? "Exit full screen" : "Full screen"
                font.pixelSize: 11
                onClicked: window.visibility =
                    window.visibility === Window.FullScreen
                        ? Window.Windowed : Window.FullScreen
            }
        }
    }

    // ---- dialogs -----------------------------------------------------------
    FileDialog {
        id: fileDialog
        title: "Open media"
        nameFilters: ["Media files (*.mp4 *.mkv *.mov *.ts *.flv *.avi *.webm)",
                      "All files (*)"]
        onAccepted: player.open(selectedFile)
    }

    Popup {
        id: statsPopup
        anchors.centerIn: Overlay.overlay
        width: Math.min(560, window.width - 80)
        height: Math.min(480, window.height - 120)
        modal: true
        focus: true
        padding: 12

        background: Rectangle {
            color: "#15181c"
            radius: 8
            border.color: "#2a3038"
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 8

            Label {
                text: "Playback statistics"
                color: "#e6eaf0"
                font.pixelSize: 13
                font.bold: true
            }
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                TextArea {
                    readOnly: true
                    text: player.stats.length > 0 ? player.stats : "{}"
                    color: "#9fb0c0"
                    font.family: "Menlo"
                    font.pixelSize: 10
                    selectByMouse: true
                }
            }
            RowLayout {
                Item { Layout.fillWidth: true }
                Button { text: "Close"; onClicked: statsPopup.close() }
            }
        }
    }

    // ---- drag and drop -----------------------------------------------------
    DropArea {
        anchors.fill: parent
        onDropped: (drop) => {
            if (drop.hasUrls && drop.urls.length > 0) {
                player.open(drop.urls[0]);
            }
        }
    }

    Shortcut {
        sequences: ["Space"]
        onActivated: player.playPause()
    }
    Shortcut {
        sequences: ["Left"]
        onActivated: player.seekCommit(Math.max(0, player.position - 5))
    }
    Shortcut {
        sequences: ["Right"]
        onActivated: player.seekCommit(Math.min(player.duration,
                                                player.position + 5))
    }
    Shortcut {
        sequences: ["F"]
        onActivated: window.visibility =
            window.visibility === Window.FullScreen
                ? Window.Windowed : Window.FullScreen
    }
    Shortcut {
        sequences: ["Escape"]
        onActivated: {
            if (window.visibility === Window.FullScreen) {
                window.visibility = Window.Windowed;
            }
        }
    }
}
