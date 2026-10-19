// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The Shotcut-style timeline: a ruler, stacked tracks holding one clip per kind,
// and a playhead that is the same clock the player is on.
//
// The clips are a VIEW of the file that is open -- its video track and its audio
// track -- not an editable project: there is no mixer behind them yet.
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

    // Keep the playhead inside the viewport. A playhead that scrolls out of
    // sight reads as a timeline that stopped moving -- which is what a 30 s
    // file looked like when the panel believed it was 18 s long: past the end
    // of the (placeholder) project nothing advanced and nothing said why.
    Connections {
        target: timeline
        function onPlayheadChanged() {
            const x = timeline.playhead * root.pps;
            const left = flick.contentX;
            const right = left + flick.width;
            if (x < left || x > right - 12) {
                flick.contentX = Math.max(0, Math.min(
                    flick.contentWidth - flick.width, x - flick.width * 0.5));
            }
        }
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

                // Which file the lanes describe. The panel has no project of
                // its own -- it shows the open file's tracks -- so the only
                // honest label is that file's name and length.
                Label {
                    text: player.source.length === 0 ? "no media"
                          : player.source.split("/").pop()
                            + (player.duration > 0
                               ? "  \u00b7  " + player.formatTime(player.duration)
                               : "  \u00b7  live")
                    color: "#6d7681"
                    font.pixelSize: 11
                    font.family: "Menlo"
                    elide: Text.ElideMiddle
                    Layout.maximumWidth: 320
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
                            // Repeater over a QAbstractListModel exposes the
                            // roles directly (name, track, start, clipLength...).
                            y: root.rulerHeight + model.track * root.trackHeight + 4
                            height: root.trackHeight - 8
                            x: model.start * root.pps
                            width: Math.max(6, model.clipLength * root.pps)
                            radius: 4
                            color: model.color

                            // Deliberately no MouseArea: there is no mixer
                            // behind the panel, so a clip that could be dragged
                            // or deleted would be a control wired to nothing.
                            // Clicks fall through to the lane, which seeks --
                            // the one edit that is actually implemented.
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
