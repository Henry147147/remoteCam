import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    objectName: "remoteCam.mainWindow"
    Accessible.name: appE2EMode ? "RemoteCam E2E main window" : "RemoteCam main window"
    width: 620
    height: 840
    minimumWidth: 520
    minimumHeight: 420
    visible: true
    title: appE2EMode ? "RemoteCam E2E" : "RemoteCam"
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
        anchors.bottomMargin: 44
        spacing: 20

        ColumnLayout {
            spacing: 4
            Label {
                id: productTitle
                objectName: "remoteCam.productTitle"
                Accessible.name: text
                text: appE2EMode ? "RemoteCam E2E" : "RemoteCam"
                color: "white"
                font.pixelSize: 30
                font.bold: true
            }
            Label {
                objectName: "remoteCam.productSubtitle"
                text: "Windows virtual-camera producer"
                color: "#91a0b8"
                font.pixelSize: 14
            }
        }

        Rectangle {
            objectName: "remoteCam.outputStatusCard"
            Accessible.name: "Virtual camera producer status"
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
                        objectName: "remoteCam.outputStatusLabel"
                        Accessible.name: text
                        text: frameProducer.connectionLabel
                        color: "white"
                        font.pixelSize: 18
                        font.bold: true
                    }
                    Label {
                        objectName: "remoteCam.outputStatusDetail"
                        Accessible.name: text
                        Layout.fillWidth: true
                        text: frameProducer.connectionDetail
                        color: "#b5c0d1"
                        font.pixelSize: 13
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }

        Rectangle {
            objectName: "remoteCam.phoneStatusCard"
            Accessible.name: "Phone receiver status"
            Layout.fillWidth: true
            implicitHeight: phoneStatusContent.implicitHeight + 34
            radius: 14
            color: "#121d2f"
            border.color: sessionStatus.streaming ? "#287a50" : "#24334c"

            RowLayout {
                id: phoneStatusContent
                anchors.fill: parent
                anchors.margins: 17
                spacing: 14

                Rectangle {
                    width: 12
                    height: 12
                    radius: 6
                    color: sessionStatus.streaming ? "#35d07f" : "#f6c453"
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 5
                    Label {
                        objectName: "remoteCam.phoneStatusLabel"
                        Accessible.name: text
                        text: sessionStatus.stateLabel
                        color: "white"
                        font.pixelSize: 18
                        font.bold: true
                    }
                    Label {
                        objectName: "remoteCam.phoneStatusDetail"
                        Accessible.name: text
                        Layout.fillWidth: true
                        text: sessionStatus.detail
                        color: "#b5c0d1"
                        font.pixelSize: 13
                        wrapMode: Text.WordWrap
                    }
                    Label {
                        objectName: "remoteCam.e2eWarning"
                        Accessible.name: text
                        visible: appE2EMode
                        text: "TEST HOST — authenticated pairing is bypassed in this executable only"
                        color: "#f6a953"
                        font.pixelSize: 11
                        font.bold: true
                    }
                }
            }
        }

        GroupBox {
            id: discoveryGroup
            objectName: "remoteCam.discoveryGroup"
            Accessible.name: title
            title: "Local network discovery"
            Layout.fillWidth: true
            implicitHeight: discoveryContent.implicitHeight + 46

            background: Rectangle {
                color: "#121d2f"
                radius: 14
                border.color: "#24334c"
            }

            label: Label {
                x: 14
                text: discoveryGroup.title
                color: "#d9e2ef"
                font.bold: true
            }

            RowLayout {
                id: discoveryContent
                anchors.fill: parent
                anchors.margins: 12
                spacing: 12

                Rectangle {
                    Layout.alignment: Qt.AlignTop
                    Layout.topMargin: 5
                    width: 10
                    height: 10
                    radius: 5
                    color: lanDiscovery.state === 1 || lanDiscovery.state === 4 ? "#35d07f"
                                                   : lanDiscovery.state === 2 ? "#ef6b73" : "#f6c453"
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4
                    Label {
                        objectName: "remoteCam.discoveryStatusLabel"
                        Accessible.name: text
                        text: lanDiscovery.statusLabel
                        color: "white"
                        font.pixelSize: 16
                        font.bold: true
                    }
                    Label {
                        objectName: "remoteCam.discoveryStatusDetail"
                        Accessible.name: text
                        Layout.fillWidth: true
                        text: lanDiscovery.statusDetail
                        color: "#b5c0d1"
                        font.pixelSize: 13
                        wrapMode: Text.WordWrap
                    }
                    Label {
                        objectName: "remoteCam.manualAddress"
                        Accessible.name: text
                        text: lanDiscovery.state === 4
                              ? "Test address: 127.0.0.1:" + lanDiscovery.port
                              : lanDiscovery.state === 1
                                ? "Manual address: " + lanDiscovery.computerName + ":" + lanDiscovery.port
                              : "Planned receiver address: " + lanDiscovery.computerName + ":" + lanDiscovery.port
                        color: "#91a0b8"
                        font.pixelSize: 12
                    }
                }
            }
        }

        GroupBox {
            id: outputGroup
            objectName: "remoteCam.outputGroup"
            Accessible.name: title
            title: "Virtual camera output"
            Layout.fillWidth: true
            implicitHeight: outputGrid.implicitHeight + 46

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
                id: outputGrid
                anchors.fill: parent
                anchors.margins: 12
                columns: 2
                columnSpacing: 16
                rowSpacing: 10

                Label { text: "Format"; color: "#91a0b8" }
                ComboBox {
                    objectName: "remoteCam.outputFormat"
                    Accessible.name: "Output format"
                    Layout.fillWidth: true
                    model: [frameProducer.outputFormat]
                    enabled: false
                }

                Label { text: "Resolution"; color: "#91a0b8" }
                ComboBox {
                    objectName: "remoteCam.outputResolution"
                    Accessible.name: "Output resolution"
                    Layout.fillWidth: true
                    model: [frameProducer.outputResolution]
                    enabled: false
                }

                Label { text: "Frame rate"; color: "#91a0b8" }
                ComboBox {
                    objectName: "remoteCam.outputFrameRate"
                    Accessible.name: "Output frame rate"
                    Layout.fillWidth: true
                    model: [frameProducer.outputFrameRate]
                    enabled: false
                }
            }
        }

        Label {
            objectName: "remoteCam.securityBoundaryNote"
            Accessible.name: text
            Layout.fillWidth: true
            text: appE2EMode
                  ? "This host verifies the phone wire stream and backend state. A desktop live-preview surface is not implemented yet."
                  : "The virtual camera requests its active NV12 geometry from the producer. Live phone video remains disabled until secure pairing is specified."
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
