import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import CrossSerial

ApplicationWindow {
    id: root
    width: 640
    height: 580
    visible: true
    title: qsTr("CrossSerial terminal")

    property string rxText: ""     // accumulated received bytes
    property string statusText: ""

    SerialController {
        id: serial

        onDataReceived: function(text) { root.rxText += text }
        onErrorOccurred: function(message) { root.statusText = qsTr("Error: ") + message }
        onConnectedChanged: {
            root.statusText = serial.connected
                    ? qsTr("Connected to ") + serial.portName + qsTr("  @") + baudCombo.currentValue
                    : qsTr("Disconnected")
        }
    }

    function doSend() {
        if (!serial.connected || inputField.length === 0)
            return
        // The transport itself is binary-clean; the EOL choice is purely an
        // application-level convention about how lines are terminated.
        var suffix = appendEolCheckBox.checked ? eolCombo.currentValue : ""
        serial.send(inputField.text + suffix)
        inputField.clear()
    }

    function connectDisconnect() {
        if (serial.connected) {
            serial.disconnectPort()
            return
        }
        var port = portCombo.currentValue
        if (!port) {
            root.statusText = qsTr("Select a port first")
            return
        }
        serial.connectPort(port, baudCombo.currentValue)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        // --- Port selection + refresh -------------------------------------
        RowLayout {
            spacing: 8
            ComboBox {
                id: portCombo
                model: PortListModel { }
                valueRole: "portName"
                textRole: "portName"
                enabled: !serial.connected
                Layout.fillWidth: true
                delegate: ItemDelegate {
                    width: portCombo.width
                    highlighted: portCombo.highlightedIndex === index
                    contentItem: ColumnLayout {
                        spacing: 0
                        Text {
                            text: model.portName
                            font.bold: true
                            color: model.vid > 0 ? "#1a73e8" : palette.text
                        }
                        Text {
                            text: (model.description ? model.description : "")
                                  + (model.manufacturer ? "   [" + model.manufacturer + "]" : "")
                            color: "gray"
                            font.pixelSize: 11
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                    }
                }
            }
            Button {
                text: qsTr("Refresh")
                enabled: !serial.connected
                onClicked: {
                    portCombo.model.refresh()
                    portCombo.currentIndex = portCombo.count > 0 ? 0 : -1
                }
            }
        }

        // --- Baud + connect/disconnect + clear ----------------------------
        RowLayout {
            spacing: 8
            Text { text: qsTr("Baud:"); anchors.verticalCenter: baudCombo.verticalCenter }
            ComboBox {
                id: baudCombo
                model: [1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600]
                currentIndex: 7 // 115200
                enabled: !serial.connected
                implicitWidth: 120
            }
            Button {
                text: serial.connected ? qsTr("Disconnect") : qsTr("Connect")
                enabled: serial.connected || (portCombo.currentValue !== undefined
                                              && portCombo.currentValue !== "")
                highlighted: serial.connected
                onClicked: connectDisconnect()
            }
            Button {
                text: qsTr("Clear")
                onClicked: {
                    rxText = ""
                    root.statusText = ""
                }
            }
            Item { Layout.fillWidth: true }
        }

        // --- Incoming data (scroll view, accumulates) ---------------------
        ScrollView {
            id: scrollView
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            TextArea {
                id: rxArea
                readOnly: true
                wrapMode: TextArea.WrapAnywhere
                text: root.rxText
                font.family: "Consolas,Menlo,monospace"
                placeholderText: qsTr("Received data appears here…")
                onTextChanged: cursorPosition = text.length // follow the tail
            }
        }

        // --- Send: input field above the Send button ----------------------
        RowLayout {
            spacing: 8
            TextField {
                id: inputField
                Layout.fillWidth: true
                placeholderText: qsTr("Text to send (UTF-8)…")
                enabled: serial.connected
                onAccepted: doSend()
            }
            Button {
                text: qsTr("Send")
                enabled: serial.connected && inputField.length > 0
                onClicked: doSend()
            }
            CheckBox {
                id: appendEolCheckBox
                checked: true
                text: qsTr("End with")
            }
            ComboBox {
                id: eolCombo
                model: [
                    { label: "\\r\\n", value: "\r\n" },
                    { label: "\\n", value: "\n" },
                    { label: "\\r", value: "\r" },
                    { label: "(none)", value: "" }
                ]
                textRole: "label"
                valueRole: "value"
                enabled: appendEolCheckBox.checked
                currentIndex: 0 // \r\n
            }
        }

        // --- Status line --------------------------------------------------
        Text {
            text: root.statusText
            color: "gray"
            Layout.fillWidth: true
            elide: Text.ElideRight
        }
    }
}
