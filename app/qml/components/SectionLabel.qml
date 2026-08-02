import QtQuick
import MazeConnect.App

/// Small tracked heading above a group of pills.
Text {
    property int size: 10

    color: Theme.faint
    font.family: Theme.fontSans
    font.pixelSize: size
    font.weight: Font.Bold
    font.letterSpacing: Theme.trackingLabel * size
    font.capitalization: Font.AllUppercase
}
