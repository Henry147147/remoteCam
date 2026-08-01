// Context properties are injected by both the production and E2E hosts.
// qmllint disable unqualified
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    objectName: "remoteCam.mainWindow"
    Accessible.name: appE2EMode ? "RemoteCam E2E main window" : "RemoteCam main window"
    width: 620
    height: 1080
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

    ScrollView {
        id: pageScroll
        anchors.fill: parent
        clip: true
        topPadding: 28
        bottomPadding: 44
        leftPadding: 28
        rightPadding: 28
        contentWidth: availableWidth
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ColumnLayout {
            width: pageScroll.availableWidth
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
                    Layout.preferredWidth: 12
                    Layout.preferredHeight: 12
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
            objectName: "remoteCam.livePreview"
            Accessible.name: "Live processed camera preview"
            Layout.fillWidth: true
            Layout.preferredHeight: 260
            radius: 14
            color: "#050912"
            border.color: "#24334c"
            clip: true
            visible: !appE2EMode

            Image {
                anchors.fill: parent
                anchors.margins: 1
                source: frameProducer.previewSource
                cache: false
                fillMode: Image.PreserveAspectFit
                asynchronous: true
            }

            Label {
                anchors.centerIn: parent
                visible: !frameProducer.publishing
                text: "Processed preview appears after a paired iPhone starts streaming"
                color: "#7888a2"
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                width: Math.min(parent.width - 40, 420)
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
                    Layout.preferredWidth: 12
                    Layout.preferredHeight: 12
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
                    Layout.preferredWidth: 10
                    Layout.preferredHeight: 10
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

                Button {
                    objectName: "remoteCam.retryDiscovery"
                    Accessible.name: "Re-advertise on the local network"
                    Layout.alignment: Qt.AlignTop
                    // Registration happens once at launch, so joining Wi-Fi afterwards
                    // leaves the PC advertised on a link no phone is watching.
                    visible: lanDiscovery.state !== 4
                    text: "Retry"
                    onClicked: lanDiscovery.restart()
                }
            }
        }

        GroupBox {
            id: phoneControlsGroup
            objectName: "remoteCam.phoneControlsGroup"
            Accessible.name: title
            title: "iPhone camera"
            Layout.fillWidth: true
            enabled: phoneController.controlsEnabled
            implicitHeight: phoneControlsGrid.implicitHeight + 46

            background: Rectangle {
                color: "#121d2f"
                radius: 14
                border.color: "#24334c"
            }

            label: Label {
                x: 14
                text: phoneControlsGroup.title
                color: "#d9e2ef"
                font.bold: true
            }

            GridLayout {
                id: phoneControlsGrid
                anchors.fill: parent
                anchors.margins: 12
                columns: 3
                columnSpacing: 14
                rowSpacing: 8

                Label { text: "Camera"; color: "#91a0b8" }
                ComboBox {
                    objectName: "remoteCam.phoneCamera"
                    Accessible.name: "iPhone camera"
                    Layout.fillWidth: true
                    Layout.columnSpan: 2
                    model: phoneController.cameraNames
                    currentIndex: phoneController.cameraIndex
                    onActivated: phoneController.cameraIndex = currentIndex
                }

                Label { text: "Stream"; color: "#91a0b8" }
                RowLayout {
                    Layout.fillWidth: true
                    Layout.columnSpan: 2
                    ComboBox {
                        objectName: "remoteCam.codec"
                        Accessible.name: "Video codec"
                        model: ["h264", "hevc"]
                        currentIndex: phoneController.codec === "hevc" ? 1 : 0
                        onActivated: phoneController.codec = currentText
                    }
                    ComboBox {
                        objectName: "remoteCam.streamResolution"
                        Accessible.name: "Capture resolution"
                        model: ["640x480", "960x540", "1280x720", "1920x1080",
                                "2560x1440", "3840x2160"]
                        currentIndex: Math.max(0, model.indexOf(phoneController.resolution))
                        onActivated: phoneController.resolution = currentText
                    }
                    ComboBox {
                        objectName: "remoteCam.streamFrameRate"
                        Accessible.name: "Capture frame rate"
                        model: [30, 60]
                        currentIndex: phoneController.frameRate === 60 ? 1 : 0
                        onActivated: phoneController.frameRate = currentValue
                    }
                    Button {
                        objectName: "remoteCam.applyFormat"
                        Accessible.name: "Apply capture format"
                        text: "Apply"
                        onClicked: phoneController.applyFormat()
                    }
                }

                Label { text: "Zoom"; color: "#91a0b8" }
                Slider {
                    objectName: "remoteCam.phoneZoom"
                    Accessible.name: "iPhone camera zoom"
                    Layout.fillWidth: true
                    from: 1
                    to: 10
                    value: phoneController.phoneZoom
                    onMoved: phoneController.phoneZoom = value
                }
                Label { text: Number(phoneController.phoneZoom).toFixed(1) + "x"; color: "white" }

                Label { text: "Focus"; color: "#91a0b8" }
                Slider {
                    objectName: "remoteCam.focus"
                    Accessible.name: "Manual focus"
                    Layout.fillWidth: true
                    from: 0
                    to: 1
                    value: phoneController.focus
                    onMoved: phoneController.focus = value
                }
                Label { text: Number(phoneController.focus).toFixed(2); color: "white" }

                Label { text: "Exposure"; color: "#91a0b8" }
                Slider {
                    objectName: "remoteCam.exposureBias"
                    Accessible.name: "Exposure bias"
                    Layout.fillWidth: true
                    from: -4
                    to: 4
                    stepSize: 0.1
                    value: phoneController.exposureBias
                    onMoved: phoneController.exposureBias = value
                }
                Label { text: Number(phoneController.exposureBias).toFixed(1) + " EV"; color: "white" }

                Label { text: "White balance"; color: "#91a0b8" }
                Slider {
                    objectName: "remoteCam.whiteBalance"
                    Accessible.name: "White balance temperature"
                    Layout.fillWidth: true
                    from: 2500
                    to: 9000
                    stepSize: 50
                    value: phoneController.whiteBalance
                    onMoved: phoneController.whiteBalance = value
                }
                Label { text: Math.round(phoneController.whiteBalance) + " K"; color: "white" }

                Label { text: "Options"; color: "#91a0b8" }
                RowLayout {
                    Layout.fillWidth: true
                    Layout.columnSpan: 2
                    CheckBox {
                        objectName: "remoteCam.torch"
                        Accessible.name: "Phone torch"
                        text: "Torch"
                        checked: phoneController.torch
                        onToggled: phoneController.torch = checked
                    }
                    CheckBox {
                        objectName: "remoteCam.stabilization"
                        Accessible.name: "Phone stabilization"
                        text: "Stabilization"
                        checked: phoneController.stabilization
                        onToggled: phoneController.stabilization = checked
                    }
                    CheckBox {
                        objectName: "remoteCam.phonePreview"
                        Accessible.name: "Keep phone preview on"
                        text: "Phone preview"
                        checked: phoneController.previewEnabled
                        onToggled: phoneController.previewEnabled = checked
                    }
                }

                Label { text: "Status"; color: "#91a0b8" }
                Label {
                    Layout.fillWidth: true
                    Layout.columnSpan: 2
                    text: phoneController.commandStatus
                    color: "#b5c0d1"
                    wrapMode: Text.WordWrap
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

                Label { text: "Screenshot"; color: "#91a0b8"; visible: !appE2EMode }
                RowLayout {
                    visible: !appE2EMode
                    Layout.fillWidth: true
                    Button {
                        objectName: "remoteCam.takeScreenshot"
                        Accessible.name: "Save screenshot"
                        text: "Save PNG"
                        onClicked: frameProducer.takeScreenshot()
                    }
                    Label {
                        Layout.fillWidth: true
                        text: frameProducer.screenshotStatus
                        color: "#b5c0d1"
                        elide: Text.ElideMiddle
                    }
                }

                Label { text: "Recording"; color: "#91a0b8"; visible: !appE2EMode }
                RowLayout {
                    visible: !appE2EMode
                    Layout.fillWidth: true
                    Button {
                        objectName: "remoteCam.recording"
                        Accessible.name: frameProducer.recording ? "Stop and save recording"
                                                                  : "Start MP4 recording"
                        Accessible.description: frameProducer.recordingStatus
                        text: frameProducer.recording
                              ? "Stop & Save"
                              : (frameProducer.recordingCanToggle ? "Record MP4" : "Finalizing...")
                        enabled: frameProducer.recordingCanToggle
                        highlighted: frameProducer.recording
                        onClicked: frameProducer.toggleRecording()
                    }
                    Label {
                        objectName: "remoteCam.recordingStatus"
                        Accessible.name: text
                        Layout.fillWidth: true
                        text: frameProducer.recordingStatus
                              + (frameProducer.recording ? " - " + frameProducer.recordingDuration : "")
                              + (frameProducer.recordingDroppedFrames > 0
                                 ? " - " + frameProducer.recordingDroppedFrames + " dropped" : "")
                        color: frameProducer.recording ? "#7de2b8" : "#b5c0d1"
                        elide: Text.ElideRight
                    }
                }

                Label {
                    text: "Saved file"
                    color: "#91a0b8"
                    visible: !appE2EMode && frameProducer.recordingPath.length > 0
                }
                Label {
                    objectName: "remoteCam.recordingPath"
                    Accessible.name: text.length > 0 ? "Recording file " + text : ""
                    Layout.fillWidth: true
                    visible: !appE2EMode && frameProducer.recordingPath.length > 0
                    text: frameProducer.recordingPath
                    color: "#b5c0d1"
                    elide: Text.ElideMiddle
                }
            }
        }

        GroupBox {
            id: transformGroup
            objectName: "remoteCam.transformGroup"
            Accessible.name: title
            title: "Transform"
            visible: !appE2EMode
            Layout.fillWidth: true
            implicitHeight: transformGrid.implicitHeight + 46

            background: Rectangle {
                color: "#121d2f"
                radius: 14
                border.color: "#24334c"
            }

            label: Label {
                x: 14
                text: transformGroup.title
                color: "#d9e2ef"
                font.bold: true
            }

            GridLayout {
                id: transformGrid
                anchors.fill: parent
                anchors.margins: 12
                columns: 3
                columnSpacing: 14
                rowSpacing: 8

                Label { text: "Rotation"; color: "#91a0b8" }
                Slider {
                    objectName: "remoteCam.rotation"
                    Accessible.name: "Clockwise rotation"
                    Layout.fillWidth: true
                    from: -180
                    to: 180
                    stepSize: 1
                    value: frameProducer.rotationDeg
                    onMoved: frameProducer.rotationDeg = value
                }
                Label { text: Math.round(frameProducer.rotationDeg) + "°"; color: "white" }

                Label { text: "Framing"; color: "#91a0b8" }
                ComboBox {
                    objectName: "remoteCam.fitMode"
                    Accessible.name: "Framing mode"
                    Layout.fillWidth: true
                    model: ["Fit", "Fill", "Stretch"]
                    currentIndex: frameProducer.fitMode
                    onActivated: frameProducer.fitMode = currentIndex
                }
                Button {
                    objectName: "remoteCam.resetTransform"
                    Accessible.name: "Reset transform"
                    text: "Reset"
                    onClicked: frameProducer.resetTransform()
                }

                Label { text: "Zoom"; color: "#91a0b8" }
                Slider {
                    objectName: "remoteCam.zoom"
                    Accessible.name: "Transform zoom"
                    Layout.fillWidth: true
                    from: 1
                    to: 4
                    stepSize: 0.01
                    value: frameProducer.zoom
                    onMoved: frameProducer.zoom = value
                }
                Label { text: Number(frameProducer.zoom).toFixed(2) + "×"; color: "white" }

                Label { text: "Mirror"; color: "#91a0b8" }
                RowLayout {
                    CheckBox {
                        objectName: "remoteCam.flipHorizontal"
                        Accessible.name: "Flip horizontally"
                        text: "Horizontal"
                        checked: frameProducer.flipH
                        onToggled: frameProducer.flipH = checked
                    }
                    CheckBox {
                        objectName: "remoteCam.flipVertical"
                        Accessible.name: "Flip vertically"
                        text: "Vertical"
                        checked: frameProducer.flipV
                        onToggled: frameProducer.flipV = checked
                    }
                }
                Item { Layout.fillWidth: true }
            }
        }

        CheckBox {
            objectName: "remoteCam.allowUnauthenticated"
            Accessible.name: "Allow iPhones to connect without pairing"
            visible: !appE2EMode && securityPolicy !== null
            text: "Allow iPhones to connect without pairing"
            checked: securityPolicy !== null && securityPolicy.allowUnauthenticated
            onToggled: securityPolicy.allowUnauthenticated = checked
        }

        Label {
            objectName: "remoteCam.allowUnauthenticatedNote"
            Accessible.name: text
            Layout.fillWidth: true
            Layout.leftMargin: 26
            visible: !appE2EMode && securityPolicy !== null
            text: securityPolicy !== null && securityPolicy.allowUnauthenticated
                  ? "Any iPhone on this network that also has this option enabled can connect and stream. Control and video are unencrypted."
                  : "Only paired iPhones may stream. Pairing is not implemented yet, so no phone can connect while this is unticked."
            color: securityPolicy !== null && securityPolicy.allowUnauthenticated ? "#f6c453" : "#7888a2"
            font.pixelSize: 12
            wrapMode: Text.WordWrap
        }

        CheckBox {
            objectName: "remoteCam.minimizeToTray"
            Accessible.name: "Keep RemoteCam running in the system tray when closed"
            visible: !appE2EMode && shellController.trayAvailable
            text: "Keep running in the system tray when this window is closed"
            checked: shellController.minimizeToTray
            onToggled: shellController.minimizeToTray = checked
        }

        Label {
            objectName: "remoteCam.securityBoundaryNote"
            Accessible.name: text
            Layout.fillWidth: true
            text: appE2EMode
                  ? "This host verifies the phone wire stream and backend state. A desktop live-preview surface is not implemented yet."
                  : sessionStatus.unauthenticated
                    ? "This session skipped pairing. Preview and every output use the same processed frame."
                    : "Production accepts video only after pairing, or after both ends enable the option above. Preview and every output use the same processed frame."
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
}
