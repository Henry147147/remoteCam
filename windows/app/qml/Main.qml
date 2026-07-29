import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    width: 620
    height: 470
    minimumWidth: 520
    minimumHeight: 420
    visible: true
    title: "RemoteCam"
    color: "#0b1220"

    function statusColor() {
        switch (frameProducer.connectionState) {
        case 0: return "#f6c453"
        case 1: return "#35d07f"
        case 2: return "#f6a953"
        default: return "#ef6b73"
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 28
        spacing: 20

        ColumnLayout {
            spacing: 4
            Label {
                text: "RemoteCam"
                color: "white"
                font.pixelSize: 30
                font.bold: true
            }
            Label {
                text: "Windows virtual-camera producer"
                color: "#91a0b8"
                font.pixelSize: 14
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: statusContent.implicitHeight + 34
            radius: 14
            color: "#121d2f"
            border.color: "#24334c"

            RowLayout {
                id: statusContent
                anchors.fill: parent
                anchors.margins: 17
                spacing: 14

                Rectangle {
                    Layout.alignment: Qt.AlignTop
                    Layout.topMargin: 5
                    width: 12
                    height: 12
                    radius: 6
                    color: root.statusColor()
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 5
                    Label {
                        text: frameProducer.connectionLabel
                        color: "white"
                        font.pixelSize: 18
                        font.bold: true
                    }
                    Label {
                        Layout.fillWidth: true
                        text: frameProducer.connectionDetail
                        color: "#b5c0d1"
                        font.pixelSize: 13
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }

        GroupBox {
            id: outputGroup
            title: "Virtual camera output"
            Layout.fillWidth: true

            background: Rectangle {
                color: "#121d2f"
                radius: 14
                border.color: "#24334c"
            }

            label: Label {
                x: 14
                text: outputGroup.title
                color: "#d9e2ef"
                font.bold: true
            }

            GridLayout {
                anchors.fill: parent
                anchors.margins: 12
                columns: 2
                columnSpacing: 16
                rowSpacing: 10

                Label { text: "Format"; color: "#91a0b8" }
                ComboBox {
                    Layout.fillWidth: true
                    model: [frameProducer.outputFormat]
                    enabled: false
                }

                Label { text: "Resolution"; color: "#91a0b8" }
                ComboBox {
                    Layout.fillWidth: true
                    model: [frameProducer.outputResolution]
                    enabled: false
                }

                Label { text: "Frame rate"; color: "#91a0b8" }
                ComboBox {
                    Layout.fillWidth: true
                    model: [frameProducer.outputFrameRate]
                    enabled: false
                }
            }
        }

        Label {
            Layout.fillWidth: true
            text: "M1 output is fixed to NV12 1920 x 1080 at 30 fps. Other formats remain disabled until consumer-to-producer geometry negotiation is implemented."
            color: "#7888a2"
            font.pixelSize: 12
            wrapMode: Text.WordWrap
        }

        Item { Layout.fillHeight: true }

        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: frameProducer.publishing
                  ? "Frames are being published to the active camera consumer."
                  : frameProducer.connectionState === 0
                    ? "Waiting is normal when no application is using the virtual camera."
                    : frameProducer.connectionState === 2
                      ? "Close this window and return to the already-running RemoteCam instance."
                      : "See %ProgramData%\\RemoteCam\\logs\\rc-app.log for diagnostics."
            color: "#71819a"
            font.pixelSize: 12
            wrapMode: Text.WordWrap
        }
    }
}
