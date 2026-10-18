// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The Shotcut-style timeline: a ruler, stacked tracks with draggable clips, and
// a playhead that is the same clock the player is on.
//
// WHAT IS BORROWED FROM SHOTCUT (github.com/mltframework/shotcut, GPL-3.0 --
// LAYOUT AND INTERACTION IDEAS ONLY, no code): tracks are horizontal lanes with
// a fixed-height header column on the left; video tracks stack above audio;
// the ruler is the scroll surface's own coordinate space, so scrolling and
// zooming do not move the playhead; clicking empty track area moves the
// playhead, clicking a clip selects it.
//
// The model (clips, arithmetic) lives in timeline_model.cc. This file only
// maps seconds to pixels -- which is the split that lets the same model drive a
// QWidget or web timeline later without moving any logic.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    readonly property real trackHeight: 44
    readonly property real headerWidth: 92
    readonly property real rulerHeight: 26
    readonly property real pps: timeline.pixelsPerSecond
    readonly property int trackCount: timeline.trackCount

    // The content width: never narrower than the viewport, so a short project
    // still fills the panel instead of leaving a dead zone that looks broken.
    readonly property real contentWidth: Math.max(timeline.duration * pps + 60,
                                                  flick.width)

    function secondsAt(x) {
        return Math.max(0, Math.min(timeline.duration, x / pps));
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- toolbar -------------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            color: "#1b1e22"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 8

                Label {
                    text: "TIMELINE"
                    color: "#8a929c"
                    font.pixelSize: 11
                    font.bold: true
                    font.letterSpacing: 1.2
                }

                ToolButton {
                    text: "+ Clip"
                    font.pixelSize: 11
                    onClicked: timeline.addClip("clip.mp4", 0,
                                                timeline.playhead, 4.0, "video")
                    ToolTip.visible: hovered
                    ToolTip.text: "Append a clip at the playhead (model demo)"
                }
                ToolButton {
                    text: "\u2212 Clip"
                    font.pixelSize: 11
                    enabled: timeline.selectedRow >= 0
                    onClicked: timeline.removeClip(timeline.selectedRow)
                    ToolTip.visible: hovered
                    ToolTip.text: "Remove the selected clip"
                }

                Item { Layout.fillWidth: true }

                Label {
                    text: Math.round(root.pps) + " px/s"
                    color: "#6d7681"
                    font.pixelSize: 11
                    font.family: "Menlo"
                }
                Slider {
                    Layout.preferredWidth: 120
                    from: 10
                    to: 300
                    value: root.pps
                    onMoved: timeline.pixelsPerSecond = value
                }
            }
        }

        // ---- ruler + tracks ------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // Track headers. Fixed, outside the scroll surface, so they stay
            // put while the timeline scrolls -- the whole point of a header.
            Column {
                Layout.preferredWidth: root.headerWidth
                Layout.fillHeight: true
                spacing: 0

                Item { width: parent.width; height: root.rulerHeight }

                Repeater {
                    model: root.trackCount
                    Rectangle {
                        width: root.headerWidth
                        height: root.trackHeight
                        color: index < 2 ? "#22262b" : "#1e2226"
                        border.color: "#2b3037"
                        border.width: 1

                        Label {
                            anchors.left: parent.left
                            anchors.leftMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            text: index < 2 ? ("V" + (2 - index))
                                            : ("A" + (index - 1))
                            color: index < 2 ? "#7fa8dd" : "#6fbf9a"
                            font.pixelSize: 12
                            font.bold: true
                        }
                    }
                }
            }

            // The scroll surface. One Flickable containing ruler + lanes +
            // playhead, so a horizontal scroll moves all three together.
            Flickable {
                id: flick
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: root.contentWidth
                contentHeight: root.rulerHeight + root.trackCount * root.trackHeight
                boundsBehavior: Flickable.StopAtBounds

                Item {
                    width: flick.contentWidth
                    height: flick.contentHeight

                    // ---- ruler ----
                    Item {
                        id: ruler
                        width: parent.width
                        height: root.rulerHeight

                        Rectangle {
                            anchors.fill: parent
                            color: "#191c20"
                        }

                        // Tick spacing adapts to zoom, so labels never collide
                        // and never thin out into nothing.
                        readonly property int step:
                            root.pps >= 120 ? 1
                            : root.pps >= 60 ? 2
                            : root.pps >= 24 ? 5 : 10

                        Repeater {
                            model: Math.ceil(root.contentWidth / (ruler.step * root.pps)) + 1
                            Item {
                                x: index * ruler.step * root.pps
                                width: 1
                                height: ruler.height

                                Rectangle {
                                    width: 1
                                    height: index % 5 === 0 ? 10 : 5
                                    anchors.bottom: parent.bottom
                                    color: "#3c434c"
                                }
                                Label {
                                    visible: index % 5 === 0 || ruler.step >= 5
                                    text: player.formatTime(index * ruler.step)
                                    color: "#6d7681"
                                    font.pixelSize: 9
                                    font.family: "Menlo"
                                    anchors.left: parent.left
                                    anchors.leftMargin: 3
                                    anchors.top: parent.top
                                    anchors.topMargin: 3
                                }
                            }
                        }
                    }

                    // ---- lanes ----
                    Repeater {
                        model: root.trackCount
                        Rectangle {
                            y: root.rulerHeight + index * root.trackHeight
                            width: parent.width
                            height: root.trackHeight
                            color: index % 2 === 0 ? "#1c2025" : "#1a1e23"
                            border.color: "#262b31"
                            border.width: 1

                            // Clicking empty lane area moves the playhead there
                            // AND seeks -- the two are the same clock, so a
                            // timeline that did not seek would be a drawing.
                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.LeftButton
                                onClicked: (mouse) => {
                                    timeline.selectedRow = -1;
                                    timeline.playhead = root.secondsAt(mouse.x);
                                    player.seekCommit(timeline.playhead);
                                }
                            }
                        }
                    }

                    // ---- clips ----
                    Repeater {
                        model: timeline

                        Rectangle {
                            id: clipRect
                            // Repeater over a QAbstractListModel exposes the
                            // roles directly (name, track, start, clipLength...).
                            y: root.rulerHeight + model.track * root.trackHeight + 4
                            height: root.trackHeight - 8
                            x: model.start * root.pps
                            width: Math.max(6, model.clipLength * root.pps)
                            radius: 4
                            color: model.color
                            border.width: timeline.selectedRow === index ? 2 : 0
                            border.color: "#ffffff"
                            opacity: dragArea.drag.active ? 0.85 : 1.0

                            Label {
                                anchors.fill: parent
                                anchors.leftMargin: 7
                                anchors.rightMargin: 4
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideRight
                                text: model.name
                                color: "#f2f5f8"
                                font.pixelSize: 11
                            }

                            MouseArea {
                                id: dragArea
                                anchors.fill: parent
                                drag.target: clipRect
                                drag.axis: Drag.XAxis
                                // Clamped so a clip cannot be dragged before the
                                // project start; the model clamps again, which
                                // is the authoritative bound.
                                drag.minimumX: 0
                                drag.maximumX: root.contentWidth - clipRect.width

                                onClicked: timeline.selectedRow = index

                                onReleased: {
                                    if (drag.active) {
                                        timeline.moveClip(index,
                                            clipRect.x / root.pps);
                                    }
                                }
                            }
                        }
                    }

                    // ---- playhead ----
                    Rectangle {
                        x: timeline.playhead * root.pps - 1
                        y: 0
                        width: 2
                        height: parent.height
                        color: "#e8574a"
                        visible: timeline.playhead >= 0

                        Rectangle {
                            // The head: the grab target a user aims at, and
                            // the only part that is comfortably clickable.
                            width: 11
                            height: 9
                            radius: 2
                            x: -4.5
                            color: "#e8574a"
                        }
                    }
                }
            }
        }
    }
}
