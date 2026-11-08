// Crate Decks design tokens for QML (stage 2, design section 14.4).
// Template for tools/crate-design/generate.py: every token in
// res/crate-design/tokens.json has a property here. Lengths are in pixels,
// tracking in em, durations in milliseconds and sweeps in degrees.
pragma Singleton
import QtQuick 2.12

QtObject {
    // color
    readonly property color bgApp: "#0a0a0f"
    readonly property color bgPanel: "#0d0e14"
    readonly property color bgRaised: "#0f1016"
    readonly property color bgPopover: "#111218"
    readonly property color bgSecondary: "#15161d"
    readonly property color border: "#1af1f5f9"
    readonly property color borderSubtle: "#0ff1f5f9"
    readonly property color borderHover: "#33f1f5f9"
    readonly property color hoverBg: "#0df1f5f9"
    readonly property color hoverBgStrong: "#1af1f5f9"
    readonly property color scrollbarThumb: "#1af1f5f9"
    readonly property color scrollbarHover: "#2ef1f5f9"
    readonly property color textPrimary: "#f1f5f9"
    readonly property color textSecondary: "#c7f1f5f9"
    readonly property color textMuted: "#999ca0"
    readonly property color textSubtle: "#7e8084"
    readonly property color textFaint: "#4a4c52"
    readonly property color navIdle: "#6bffffff"
    readonly property color accent: "#06b6d4"
    readonly property color onAccent: "#0a0a0f"
    readonly property color accentHi: "#67e8f9"
    readonly property color accentBg: "#1a06b6d4"
    readonly property color accentBorder: "#4c06b6d4"
    readonly property color accentBorderFocus: "#6606b6d4"
    readonly property color rowHover: "#0a06b6d4"
    readonly property color rowSelected: "#1406b6d4"
    readonly property color focusRing: "#06b6d4"
    readonly property color warning: "#fbbf24"
    readonly property color warningText: "#f9c94a"
    readonly property color danger: "#ef4444"
    readonly property color dangerText: "#ef6465"
    readonly property color logoStart: "#67e8f9"
    readonly property color logoEnd: "#0e7490"
    readonly property color logoShade: "#0e4f5c"

    // iconState
    readonly property color iconIdle: "#999ca0"
    readonly property color iconHover: "#67e8f9"
    readonly property color iconActive: "#67e8f9"
    readonly property color iconDisabled: "#4a4c52"
    readonly property color iconOnAccent: "#0a0a0f"

    // deck
    readonly property color deckA: "#67e8f9"
    readonly property color deckB: "#f1f5f9"
    readonly property color deckC: "#67e8f9"
    readonly property color deckD: "#f1f5f9"

    // waveform
    readonly property color waveBg: "#0a0a0f"
    readonly property color waveLowAccent: "#0e7490"
    readonly property color waveMidAccent: "#06b6d4"
    readonly property color waveHighAccent: "#67e8f9"
    readonly property color waveLowNeutral: "#4a4c52"
    readonly property color waveMidNeutral: "#999ca0"
    readonly property color waveHighNeutral: "#f1f5f9"
    readonly property color wavePlayhead: "#f1f5f9"
    readonly property color waveBeat: "#33f1f5f9"
    readonly property color waveDownbeat: "#66f1f5f9"
    readonly property color waveOverviewBg: "#0d0e14"
    readonly property color waveOverviewPlayed: "#8c0a0a0f"

    // meter
    readonly property color vuLevel: "#67e8f9"
    readonly property color vuWarning: "#fbbf24"
    readonly property color vuClip: "#ef4444"
    readonly property color vuTrack: "#0ff1f5f9"
    readonly property real vuWarningDb: -6
    readonly property real vuBarWidth: 4

    // font
    readonly property string fontBody: "Roboto"
    readonly property string fontDisplay: "Bricolage Grotesque"
    readonly property string fontMono: "JetBrains Mono"
    readonly property real fontWeightBody: 400
    readonly property real fontWeightTitle: 700
    readonly property real fontWeightDisplay: 800
    readonly property real fontWeightMono: 400

    // fontSize
    readonly property real fontSizeCounter: 8
    readonly property real fontSizeBadge: 10
    readonly property real fontSizeMeta: 10.5
    readonly property real fontSizeLabel: 11
    readonly property real fontSizeEyebrow: 11.5
    readonly property real fontSizeInfoBadge: 12
    readonly property real fontSizeCaption: 13
    readonly property real fontSizeBody: 15
    readonly property real fontSizeTitle: 30
    readonly property real fontSizeDisplay: 40
    readonly property real fontSizeLegend: 9
    readonly property real fontSizeTable: 11
    readonly property real fontSizeDeckNumber: 34

    // tracking
    readonly property real trackingCaps: 0.14
    readonly property real trackingKicker: 0.16
    readonly property real trackingEyebrow: 0.18
    readonly property real trackingTitle: -0.045
    readonly property real trackingNumeral: -0.05

    // height
    readonly property real heightControl: 40
    readonly property real heightControlSm: 32
    readonly property real heightControlXs: 24
    readonly property real heightControlLg: 44
    readonly property real heightTableCell: 44
    readonly property real heightTrackRow: 56
    readonly property real heightTrackRowCompact: 52
    readonly property real heightTableHeader: 32
    readonly property real heightLibraryRow: 30
    readonly property real heightSearch: 40

    // layout
    readonly property real sidebarWidth: 240
    readonly property real sidebarWidthCollapsed: 144
    readonly property real topbarHeight: 64
    readonly property real activeBarWidth: 2
    readonly property real focusRingWidth: 2

    // lane
    readonly property real laneMeter: 1
    readonly property real laneSlider: 2
    readonly property real sliderDot: 8
    readonly property real scrollbarWidth: 8

    // radius
    readonly property real radiusSm: 2
    readonly property real radiusMd: 4
    readonly property real radiusLg: 6
    readonly property real radiusXl: 8
    readonly property real radiusPanel: 14
    readonly property real radiusPill: 999

    // motion
    readonly property real motionFast: 150
    readonly property real motionNormal: 250
    readonly property string motionEaseSnap: "cubic-bezier(0.22, 1, 0.36, 1)"

    // iconSize
    readonly property real iconNano: 10
    readonly property real iconMicro: 12
    readonly property real iconXs: 14
    readonly property real iconSm: 16
    readonly property real iconMd: 18
    readonly property real iconLg: 20
    readonly property real iconXl: 24

    // geometry
    readonly property real strokeHairline: 1
    readonly property real strokeNode: 1.25
    readonly property real strokeActive: 1.5
    readonly property real strokeNodeActive: 1.75
    readonly property real strokeSelected: 2.25
    readonly property real strokeConnection: 1.4
    readonly property real nodeLeaf: 3.5
    readonly property real nodeSibling: 4.5
    readonly property real nodeRoot: 5.5
    readonly property real nodeSelected: 8
    readonly property real tickCount: 48
    readonly property real tickEvery: 4
    readonly property real tickLong: 7
    readonly property real tickShort: 3
    readonly property var dashData: [9, 4]
    readonly property var dashQueue: [3, 4]
    readonly property var dashTelemetry: [1, 5]
    readonly property var dashAxis: [1, 7]
    readonly property real opacityContext: 0.25
    readonly property real opacityRest: 0.34
    readonly property real opacityMuted: 0.12
    readonly property real opacityGuide: 0.45
    readonly property real opacityLevel: 0.55
    readonly property real glowPulseRadius: 3
    readonly property real glowPulseAlpha: 0.7
    readonly property real levelRingRadius: 7

    // knob
    readonly property real knobSize: 40
    readonly property real knobSweep: 270
    readonly property real knobTickCount: 24
    readonly property real knobTickEvery: 2
    readonly property real knobArcWidth: 1.5
    readonly property real knobDot: 3

    // fader
    readonly property real faderRail: 1
    readonly property real faderTickStep: 10
    readonly property real faderHandleWidth: 18
    readonly property real faderHandleHeight: 10

    // deckDisplay
    readonly property string deckNumberFont: "Bricolage Grotesque"
    readonly property real deckNumberWeight: 700
}
