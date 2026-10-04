import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    color: "#181818"
    border.color: "#333333"
    border.width: 1

    property int selectedClipIndex: -1
    property var selClip: selectedClipIndex >= 0
                          && selectedClipIndex < timelineEngine.timelineClips.length
                          ? timelineEngine.timelineClips[selectedClipIndex] : null

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 10

        Label {
            text: qsTr("Inspector")
            color: "#cccccc"
            font.pointSize: 10
        }

        ScrollView {
            id: inspectorScroll
            Layout.fillWidth: true
            Layout.fillHeight: true

            ColumnLayout {
                // Qt6 ScrollView viewport width (parent.width is the
                // contentItem and can be 0 during creation).
                width: inspectorScroll.availableWidth
                spacing: 15

                GroupBox {
                    title: qsTr("Transform")
                    Layout.fillWidth: true

                    ColumnLayout {
                        anchors.fill: parent
                        RowLayout {
                            Label { id: scaleXLabel; text: qsTr("Scale X"); color: "#cccccc"; font.pointSize: 9; Layout.preferredWidth: 70 }
                            SpinBox {
                                from: 10; to: 500
                                // Guard echo: engine round-trip must not
                                // jitter the value while spinning.
                                value: activeFocus ? value : Math.round(timelineEngine.clipScaleX * 100)
                                Accessible.name: qsTr("Scale X percent")
                                Accessible.description: qsTr("Clip horizontal scale, 10 to 500 percent")
                                onValueModified: function() { if (activeFocus) { timelineEngine.clipScaleX = value / 100 } }
                            }
                        }
                        RowLayout {
                            Label { text: qsTr("Scale Y"); color: "#cccccc"; font.pointSize: 9; Layout.preferredWidth: 70 }
                            SpinBox {
                                from: 10; to: 500
                                value: activeFocus ? value : Math.round(timelineEngine.clipScaleY * 100)
                                Accessible.name: qsTr("Scale Y percent")
                                Accessible.description: qsTr("Clip vertical scale, 10 to 500 percent")
                                onValueModified: function() { if (activeFocus) { timelineEngine.clipScaleY = value / 100 } }
                            }
                        }
                        RowLayout {
                            Label { text: qsTr("Rotation"); color: "#cccccc"; font.pointSize: 9; Layout.preferredWidth: 70 }
                            SpinBox {
                                value: activeFocus ? value : Math.round(timelineEngine.clipRotation)
                                to: 360; from: -360
                                Accessible.name: qsTr("Rotation degrees")
                                Accessible.description: qsTr("Clip rotation, minus 360 to 360 degrees")
                                onValueModified: function() { if (activeFocus) { timelineEngine.clipRotation = value } }
                            }
                        }
                    }
                }

                GroupBox {
                    title: qsTr("Audio")
                    Layout.fillWidth: true

                    ColumnLayout {
                        anchors.fill: parent
                        RowLayout {
                            Label { text: qsTr("Volume"); color: "#cccccc"; font.pointSize: 9; Layout.preferredWidth: 70 }
                            Slider {
                                id: volumeSlider
                                from: 0; to: 2.0; stepSize: 0.1
                                // Don't fight the user while dragging (same
                                // pressed guard as the monitor scrub sliders).
                                value: volumeSlider.pressed ? value : timelineEngine.volume
                                Accessible.name: qsTr("Volume")
                                Accessible.description: qsTr("Playback volume, 0 to 200 percent")
                                onMoved: timelineEngine.volume = value
                            }
                        }
                        Label {
                            text: selClip ? (selClip.name || qsTr("Clip")) : qsTr("No clip selected")
                            color: "#999999"
                            font.pointSize: 8
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                        RowLayout {
                            enabled: selClip !== null
                            Label { text: qsTr("Clip gain"); color: "#cccccc"; font.pointSize: 9; Layout.preferredWidth: 70 }
                            Slider {
                                id: gainSlider
                                from: 0; to: 2.0; stepSize: 0.1
                                value: gainSlider.pressed ? value : (selClip ? (selClip.gain === undefined ? 1.0 : selClip.gain) : 1.0)
                                Accessible.name: qsTr("Clip gain")
                                Accessible.description: qsTr("Per-clip gain, 0 to 200 percent")
                                onMoved: if (selClip) { timelineEngine.setClipGain(selectedClipIndex, value) }
                            }
                        }
                        RowLayout {
                            enabled: selClip !== null
                            Label { text: qsTr("Mute clip"); color: "#cccccc"; font.pointSize: 9; Layout.preferredWidth: 70 }
                            CheckBox {
                                checked: selClip ? !!selClip.muted : false
                                Accessible.name: qsTr("Mute clip")
                                onToggled: if (selClip) { timelineEngine.setClipMuted(selectedClipIndex, checked) }
                            }
                        }
                        RowLayout {
                            enabled: selClip !== null
                            Label { text: qsTr("Fade in"); color: "#cccccc"; font.pointSize: 9; Layout.preferredWidth: 70 }
                            SpinBox {
                                from: 0; to: 300
                                value: activeFocus ? value : Math.round((selClip ? (selClip.fadeIn || 0) : 0) * 10)
                                Accessible.name: qsTr("Fade in tenths")
                                Accessible.description: qsTr("Fade in, 0 to 30 seconds")
                                onValueModified: function() { if (activeFocus && selClip) { timelineEngine.setClipFadeIn(selectedClipIndex, value / 10) } }
                            }
                        }
                        RowLayout {
                            enabled: selClip !== null
                            Label { text: qsTr("Fade out"); color: "#cccccc"; font.pointSize: 9; Layout.preferredWidth: 70 }
                            SpinBox {
                                from: 0; to: 300
                                value: activeFocus ? value : Math.round((selClip ? (selClip.fadeOut || 0) : 0) * 10)
                                Accessible.name: qsTr("Fade out tenths")
                                Accessible.description: qsTr("Fade out, 0 to 30 seconds")
                                onValueModified: function() { if (activeFocus && selClip) { timelineEngine.setClipFadeOut(selectedClipIndex, value / 10) } }
                            }
                        }
                        RowLayout {
                            enabled: selClip !== null
                            Button {
                                text: qsTr("Detach audio")
                                Accessible.name: qsTr("Detach audio")
                                Accessible.description: qsTr("Copy clip audio to the A1 bed and mute the video clip")
                                onClicked: if (selClip) { timelineEngine.detachAudio(selectedClipIndex) }
                            }
                        }
                    }
                }
            }
        }
    }
}
