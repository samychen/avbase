// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The Haruna-style control bar: a translucent panel floating over the video,
// with the scrubber occupying the full width above one row of controls.
//
// WHAT IS BORROWED FROM HARUNA (invent.kde.org/multimedia/haruna, GPL-3.0 --
// LAYOUT AND INTERACTION IDEAS ONLY, no code): the bar is an overlay rather
// than a strip below the picture, so the video keeps its full height; the
// scrubber spans the whole width and sits above the buttons; the time readout
// is a single "position / duration" pair on the left of the scrubber's row; and
// the whole bar fades instead of hiding, because a bar that disappears makes
// the mouse hunt for it.
//
// QtQuick.Controls is used for the buttons' look-and-feel only. There is no
// MediaPlayer/QtMultimedia anywhere: every control here calls player.* directly.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    // Seconds, from the bridge. Kept as separate properties so this component
    // can be dropped into any host that provides the same four numbers.
    property real position: player.position
    property real duration: player.duration
    property real buffered: player.buffered
    property bool playing: player.playing
    property bool live: player.live
    property string title: player.source.length > 0
                          ? player.source.split("/").pop() : "No media"

    signal seekPreview(real ratio)
    signal seekCommit(real ratio)

    implicitHeight: 84
    color: "#d016181c"
    radius: 14
    border.color: "#24ffffff"
    border.width: 1

    // A live stream has no duration to scrub: the bar still shows the position
    // but the scrubber becomes a non-interactive indicator.
    readonly property real progress: duration > 0
                                     ? Math.max(0, Math.min(1, position / duration))
                                     : 0
    readonly property real bufferedRatio: duration > 0
                                          ? Math.max(0, Math.min(1, buffered / duration))
                                          : 0

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        anchors.topMargin: 8
        spacing: 2

        // ---- scrubber ------------------------------------------------------
        Item {
            id: scrubArea
            Layout.fillWidth: true
            Layout.preferredHeight: 18

            Rectangle {
                id: track
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                height: 4
                radius: 2
                color: "#3a3f46"
            }
            Rectangle {
                id: bufferedFill
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width * root.bufferedRatio
                height: 4
                radius: 2
                color: "#5c636d"
            }
            Rectangle {
                id: playedFill
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width * root.progress
                height: 4
                radius: 2
                color: "#3f8ce8"
            }
            Rectangle {
                id: knob
                visible: root.duration > 0
                width: 12
                height: 12
                radius: 6
                color: "#ffffff"
                anchors.verticalCenter: parent.verticalCenter
                x: Math.max(0, Math.min(parent.width - width,
                                        parent.width * root.progress - width / 2))
            }

            MouseArea {
                id: scrubMouse
                anchors.fill: parent
                // The hit area is taller than the 4 px track: a 4 px target is
                // unusable with a mouse and impossible with a finger.
                anchors.topMargin: -8
                anchors.bottomMargin: -8
                hoverEnabled: true
                enabled: root.duration > 0 && !root.live
                cursorShape: Qt.PointingHandCursor

                function ratioAt(mx) {
                    return Math.max(0, Math.min(1, mx / width));
                }
                // Dragging previews on keyframes and only the release asks for
                // the exact frame -- the same split the bridge exposes, because
                // an accurate seek per mouse-move would queue dozens of decodes.
                onPressed: (mouse) => root.seekPreview(ratioAt(mouse.x))
                onPositionChanged: (mouse) => {
                    if (pressed) root.seekPreview(ratioAt(mouse.x));
                }
                onReleased: (mouse) => root.seekCommit(ratioAt(mouse.x))
            }
        }

        // ---- buttons -------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 6

            Label {
                text: player.formatTime(root.position) + "  /  "
                      + (root.live ? "LIVE" : player.formatTime(root.duration))
                color: "#d8dce3"
                font.pixelSize: 12
                font.family: "Menlo"
            }

            Item { Layout.fillWidth: true }

            ToolButton {
                text: "\u23EE"  // previous: jump to start
                font.pixelSize: 15
                onClicked: player.seekCommit(0)
                ToolTip.visible: hovered
                ToolTip.text: "Back to start"
            }
            ToolButton {
                text: "\u25C0\u2758"  // step back one frame
                font.pixelSize: 13
                onClicked: player.stepOnce()
                ToolTip.visible: hovered
                ToolTip.text: "Step one frame (paused only)"
            }

            // The play button is the one control that has to be unmistakable,
            // so it is a filled circle rather than another flat tool button.
            Rectangle {
                Layout.preferredWidth: 40
                Layout.preferredHeight: 40
                radius: 20
                color: playMouse.containsMouse ? "#4a97ef" : "#3f8ce8"

                Label {
                    anchors.centerIn: parent
                    text: root.playing ? "\u23F8" : "\u25B6"
                    color: "#ffffff"
                    font.pixelSize: 17
                }
                MouseArea {
                    id: playMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: player.playPause()
                }
            }

            ToolButton {
                text: "\u25A0"
                font.pixelSize: 14
                onClicked: player.stop()
                ToolTip.visible: hovered
                ToolTip.text: "Stop"
            }

            Item { Layout.fillWidth: true }

            // ---- volume ----------------------------------------------------
            ToolButton {
                text: player.muted || player.volume === 0 ? "\uD83D\uDD07"
                                                          : "\uD83D\uDD0A"
                font.pixelSize: 14
                onClicked: player.toggleMute()
                ToolTip.visible: hovered
                ToolTip.text: "Mute"
            }
            Slider {
                Layout.preferredWidth: 84
                from: 0
                to: 1
                value: player.muted ? 0 : player.volume
                onMoved: player.setVolume(value)
            }

            // ---- rate ------------------------------------------------------
            ComboBox {
                Layout.preferredWidth: 84
                model: ["0.5x", "0.75x", "1x", "1.25x", "1.5x", "2x"]
                currentIndex: 2
                onActivated: (index) => {
                    const rates = [0.5, 0.75, 1.0, 1.25, 1.5, 2.0];
                    player.setRate(rates[index]);
                }
                ToolTip.visible: hovered
                ToolTip.text: "Playback rate"
            }

            // ---- tracks ----------------------------------------------------
            ToolButton {
                text: "CC/TR"
                font.pixelSize: 11
                enabled: player.audioTracks.length + player.videoTracks.length
                         + player.textTracks.length > 0
                onClicked: trackMenu.popup()
                ToolTip.visible: hovered
                ToolTip.text: "Tracks"

                Menu {
                    id: trackMenu
                    Menu {
                        title: "Audio"
                        enabled: player.audioTracks.length > 0
                        Repeater {
                            model: player.audioTracks
                            MenuItem {
                                text: modelData.label + (modelData.index
                                      === player.audioTrack ? "  \u2713" : "")
                                onClicked: player.selectAudioTrack(modelData.index)
                            }
                        }
                    }
                    Menu {
                        title: "Video"
                        enabled: player.videoTracks.length > 0
                        Repeater {
                            model: player.videoTracks
                            MenuItem {
                                text: modelData.label + (modelData.index
                                      === player.videoTrack ? "  \u2713" : "")
                                onClicked: player.selectVideoTrack(modelData.index)
                            }
                        }
                    }
                    MenuSeparator {}
                    MenuItem {
                        text: "Snapshot to ~/avbase-snapshot.png"
                        onClicked: player.snapshot(
                            player.homePath + "/avbase-snapshot.png")
                    }
                }
            }
        }
    }
}
