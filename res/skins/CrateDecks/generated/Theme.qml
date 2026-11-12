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
    readonly property color stemDrums: "#67e8f9"
    readonly property color stemBass: "#0e7490"
    readonly property color stemOther: "#999ca0"
    readonly property color stemVocals: "#fbbf24"
    readonly property color dangerBg: "#1aef4444"
    readonly property color dangerBorder: "#40ef4444"
    readonly property color warningBg: "#1afbbf24"
    readonly property color warningBorder: "#40fbbf24"
    readonly property color badgeOffText: "#4ff1f5f9"
    readonly property color badgeOffBorder: "#0af1f5f9"
    readonly property color scrim: "#b80a0a0f"
    readonly property color accentHiBorder: "#6667e8f9"
    readonly property color textPrimaryBorder: "#66f1f5f9"

    // iconState
    readonly property color iconIdle: "#999ca0"
    readonly property color iconHover: "#67e8f9"
    readonly property color iconActive: "#67e8f9"
    readonly property color iconDisabled: "#4a4c52"
    readonly property color iconOnAccent: "#0a0a0f"
    readonly property color iconPrimary: "#f1f5f9"
    readonly property color iconAccent: "#06b6d4"
    readonly property color iconDanger: "#ef6465"
    readonly property color iconWarning: "#f9c94a"

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
    // "Accent" (logo bands) or "Neutral" (the reserve: greys on every deck)
    readonly property string wavePaletteDeckAC: "Accent"
    readonly property color waveLowDeckAC: "#0e7490"
    readonly property color waveMidDeckAC: "#06b6d4"
    readonly property color waveHighDeckAC: "#67e8f9"
    readonly property color waveLowDeckBD: "#4a4c52"
    readonly property color waveMidDeckBD: "#999ca0"
    readonly property color waveHighDeckBD: "#f1f5f9"

    // meter
    readonly property color vuLevel: "#67e8f9"
    readonly property color vuWarning: "#fbbf24"
    readonly property color vuClip: "#ef4444"
    readonly property color vuTrack: "#0ff1f5f9"
    readonly property color vuClipTrack: "#2eef4444"
    readonly property real vuWarningDb: -6
    readonly property real vuBarWidth: 4
    readonly property real vuClipHeight: 4
    readonly property real vuClipGap: 2
    readonly property real vuPeakHold: 1
    readonly property real vuPeakHoldTime: 500
    readonly property real vuPeakFallTime: 1000
    readonly property real vuGap: 6

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
    readonly property real fontSizeMonoLabel: 10.3125
    readonly property real fontSizeDeckNumber: 34
    readonly property real fontSizeDeckNumberCompact: 26
    readonly property real fontSizeBrand: 14
    readonly property real fontSizeDeckNumberNarrow: 28
    readonly property real fontSizeDeckNumberCompactShort: 22
    readonly property real fontSizeDeckTitle: 19
    readonly property real fontSizeDeckTitleCompact: 16
    readonly property real fontSizeDeckTitleCompactShort: 14
    readonly property real fontSizeDeckArtistCompactShort: 12
    readonly property real fontSizeLabelSmall: 9.5
    readonly property real fontSizeDropTitle: 14
    readonly property real fontWeightDeckTitle: 500

    // tracking
    readonly property real trackingCaps: 0.14
    readonly property real trackingKicker: 0.16
    readonly property real trackingEyebrow: 0.18
    readonly property real trackingTitle: -0.045
    readonly property real trackingNumeral: -0.05
    readonly property real trackingDeckTitle: -0.01
    readonly property real lineHeightDisplay: 0.95
    readonly property real lineHeightBody: 1.5
    readonly property real lineHeightLabel: 1.4

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
    readonly property real heightSearchTall: 44

    // layout
    readonly property real sidebarWidth: 240
    readonly property real sidebarWidthCollapsed: 144
    readonly property real topbarHeight: 64
    readonly property real activeBarWidth: 2
    readonly property real focusRingWidth: 2
    readonly property real gutter: 20
    readonly property real gutterNarrow: 14
    readonly property real layoutNarrowBelow: 1500
    readonly property real layoutShortBelow: 800
    readonly property real toolbarGap: 22
    readonly property real toolbarGapNarrow: 12
    readonly property real toolbarGroupGap: 4
    readonly property real toolbarSeparatorHeight: 24
    readonly property real toolbarButtonPadding: 10
    readonly property real toolbarButtonPaddingNarrow: 7
    readonly property real iconTextGap: 8
    readonly property real brandGap: 10
    readonly property real brandLogoWidth: 28
    readonly property real brandLogoHeight: 30
    readonly property real readingMinWidth: 64
    readonly property real readingGap: 5
    readonly property real statusDot: 6
    readonly property real monoSeparatorPadding: 4

    // deckLayout
    readonly property real waveformRowHeight: 92
    readonly property real waveformRowHeightShort: 64
    readonly property real waveformRowHeightFour: 52
    readonly property real waveformRowHeightFourShort: 36
    readonly property real waveformRowHeightLibrary: 44
    readonly property real waveformGutterWidth: 34
    readonly property real waveformGutterPadding: 7
    readonly property real deckPaddingTop: 14
    readonly property real deckPaddingBottom: 12
    readonly property real deckPaddingShort: 10
    readonly property real deckPaddingFourShort: 7
    readonly property real deckColumnGap: 14
    readonly property real deckRowGap: 10
    readonly property real deckRowGapShort: 8
    readonly property real deckRowGapFourShort: 6
    readonly property real deckHeadGap: 10
    readonly property real deckLetterSize: 20
    readonly property real deckTrackGap: 18
    readonly property real deckTrackGapNarrow: 12
    readonly property real coverSize: 52
    readonly property real coverSizeCompact: 40
    readonly property real deckTitleGap: 3
    readonly property real deckNumberGap: 6
    readonly property real overviewMinHeight: 40
    readonly property real overviewMaxHeight: 88
    readonly property real overviewMaxHeightShort: 44
    readonly property real phraseHeight: 18
    readonly property real phraseGap: 12
    readonly property real nextHeight: 22
    readonly property real transportGap: 8
    readonly property real transportButtonHeight: 36
    readonly property real transportButtonHeightFour: 30
    readonly property real transportButtonHeightFourShort: 26
    readonly property real transportButtonMinWidth: 52
    readonly property real transportButtonPadding: 12
    readonly property real smallButtonHeight: 28
    readonly property real smallButtonPadding: 8
    readonly property real loopGap: 4
    readonly property real loopReadoutWidth: 26
    readonly property real hotcueSize: 28
    readonly property real hotcueSizeFourShort: 24
    readonly property real hotcueGap: 2
    readonly property real hotcueInset: 6
    readonly property real stemsGap: 14
    readonly property real stemsPaddingTop: 8
    readonly property real stemGap: 8
    readonly property real stemsRowGap: 6
    readonly property real stemNameMinWidth: 42
    readonly property real stemRailHeight: 18
    readonly property real stemHandleWidth: 5
    readonly property real stemHandleHeight: 9
    readonly property real stemLevelHeight: 2
    readonly property real stemLevelOpacity: 0.85
    readonly property real stemTickShort: 2
    readonly property real stemTickLong: 3
    readonly property real stemTickOffset: 4
    readonly property real stemRailInset: 3
    readonly property real muteWidth: 24
    readonly property real muteHeight: 22
    readonly property real stemChipHeight: 22
    readonly property real stemChipGap: 7
    readonly property real stemChipPaddingLeft: 8
    readonly property real stemChipPaddingRight: 9
    readonly property real stemChipLevelWidth: 3
    readonly property real stemChipLevelHeight: 12
    readonly property real stemChipsGap: 6
    readonly property real pitchColumnWidth: 54
    readonly property real pitchFaderWidth: 34
    readonly property real pitchGap: 8
    readonly property real pitchFaderMinHeight: 120
    readonly property real pitchFaderMinHeightFour: 80
    readonly property real pitchFaderMinHeightFourShort: 60
    readonly property real dropZoneGap: 14
    readonly property real dropZoneTextGap: 4
    readonly property real badgeHeight: 20
    readonly property real badgePadding: 7
    readonly property real menuButtonSize: 24
    readonly property real dropIconSize: 28

    // library
    readonly property real libraryCellPadding: 10
    readonly property real libraryDeckMarkWidth: 34
    readonly property real libraryColumnBpm: 76
    readonly property real libraryColumnKey: 54
    readonly property real libraryColumnTime: 60
    readonly property real libraryColumnEnergy: 46
    readonly property real libraryColumnProfile: 108
    readonly property real libraryColumnCache: 58
    readonly property real libraryColumnArtist: 0.16
    readonly property real libraryColumnGenre: 0.13
    readonly property real libraryStateGap: 6
    readonly property real searchGap: 14
    readonly property real searchIconGap: 10
    readonly property real sidebarPaddingTop: 6
    readonly property real sidebarPaddingBottom: 12
    readonly property real sidebarSectionPaddingTop: 14
    readonly property real sidebarSectionPaddingX: 16
    readonly property real sidebarSectionPaddingBottom: 6
    readonly property real sidebarItemInset: 8
    readonly property real sidebarItemPadding: 10
    readonly property real sidebarIndent: 20
    readonly property real sidebarBarInset: 6

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
    readonly property real wheelCanvas: 760
    readonly property real camelotRingA: 196
    readonly property real camelotRingB: 292
    readonly property real tickPhrase: 10
    readonly property real hotcueRadius: 10
    readonly property real hotcueRadiusFourShort: 9
    readonly property real levelRingRadius: 7
    readonly property real levelRingMinimum: 0.04

    // knob
    readonly property real knobSize: 36
    readonly property real knobSizeCompact: 30
    readonly property real knobTickLong: 4.5
    readonly property real knobTickShort: 2
    readonly property real knobSweep: 270
    readonly property real knobTickCount: 24
    readonly property real knobTickEvery: 2
    readonly property real knobArcWidth: 1.5
    readonly property real knobDot: 3
    readonly property real knobCanvas: 40
    readonly property real knobTickGap: 1.5
    readonly property real knobDotInset: 5
    readonly property real knobSizeFourShort: 26
    readonly property real knobLabelGap: 2
    readonly property real knobGap: 3
    readonly property real knobGapShort: 1
    readonly property real knobGapFour: 0

    // fader
    readonly property real faderRail: 1
    readonly property real faderTickStep: 10
    readonly property real faderHandleWidth: 18
    readonly property real faderHandleHeight: 10
    readonly property real faderHandleLine: 8
    readonly property real faderTickGap: 2
    readonly property real faderWidth: 30

    // mixerLayout
    readonly property real mixerPaddingX: 18
    readonly property real mixerPaddingTop: 14
    readonly property real mixerPaddingBottom: 12
    readonly property real mixerPaddingTopShort: 10
    readonly property real mixerPaddingBottomShort: 8
    readonly property real mixerRowGap: 10
    readonly property real stripGap: 18
    readonly property real stripColumnGap: 10
    readonly property real stripHeadGap: 4
    readonly property real stripLetterSize: 18
    readonly property real fontSizeStripLetter: 10
    readonly property real stripFaderGap: 8
    readonly property real stripFaderMarginFour: 10
    readonly property real masterPaddingX: 4
    readonly property real masterGap: 8
    readonly property real masterVuHeight: 130
    readonly property real masterVuHeightShort: 96
    readonly property real masterVuGap: 4
    readonly property real xfaderGap: 10

    // deckDisplay
    readonly property string deckNumberFont: "Bricolage Grotesque"
    readonly property real deckNumberWeight: 700

    // colorPicker
    readonly property color pickerRed: "#ff0000"
    readonly property color pickerYellow: "#ffff00"
    readonly property color pickerGreen: "#00ff00"
    readonly property color pickerCyan: "#00ffff"
    readonly property color pickerBlue: "#0000ff"
    readonly property color pickerMagenta: "#ff00ff"

    // dialog
    readonly property real dialogTitleHeight: 44
    readonly property real dialogPaddingX: 16
    readonly property real dialogFootPaddingY: 12
    readonly property real dialogTitleGap: 12
    readonly property real dialogButtonGap: 8
    readonly property real dialogLogoWidth: 16
    readonly property real dialogLogoHeight: 17
    readonly property real iconButtonSize: 32
    readonly property real prefsWidth: 1180
    readonly property real prefsHeight: 940
    readonly property real prefsNavWidth: 232
    readonly property real prefsNavPaddingY: 10
    readonly property real prefsNavRowHeight: 32
    readonly property real prefsNavLogoWidth: 15
    readonly property real prefsNavLogoHeight: 16
    readonly property real prefsPagePaddingY: 22
    readonly property real prefsPagePaddingX: 36
    readonly property real cacheViewPaddingX: 20
    readonly property real fontWeightPrefsTitle: 600
    readonly property real trackingPrefsTitle: -0.04
    readonly property real prefsTitleGap: 6
    readonly property real prefsIntroGap: 22
    readonly property real prefsIntroMaxWidth: 680
    readonly property real prefsSectionPaddingTop: 18
    readonly property real prefsSectionPaddingBottom: 20
    readonly property real prefsSectionLabelGap: 12
    readonly property real prefsRowLabelWidth: 220
    readonly property real prefsRowLabelWidthColumn: 96
    readonly property real prefsRowGap: 18
    readonly property real prefsColumnGap: 40
    readonly property real prefsHintGap: 2
    readonly property real serverGap: 14
    readonly property real serverPaddingY: 12
    readonly property real serverNameGap: 3
    readonly property real serverMetaGap: 10
    readonly property real featureBadgeHeight: 22
    readonly property real featureBadgePaddingX: 8
    readonly property real featureGap: 6
    readonly property real featuresTop: 8
    readonly property real statusBadgePaddingX: 9
    readonly property real helperGap: 10
    readonly property real addServerTop: 14
    readonly property real switchWidth: 32
    readonly property real switchHeight: 18
    readonly property real switchKnob: 14
    readonly property real cacheGaugeSize: 64
    readonly property real cacheGaugeRadius: 24
    readonly property real cacheGaugeTickGap: 3
    readonly property real cacheGaugeTickLong: 5
    readonly property real cacheGaugeTickShort: 2
    readonly property real cacheLimitWidth: 52
    readonly property real cacheLimitMaximum: 100
    readonly property real fontSizeUsage: 26
    readonly property real usageGap: 16
    readonly property real usageBottom: 12
    readonly property real usageNumberTop: 4
    readonly property real chipPaddingX: 12
    readonly property real chipPaddingY: 4
    readonly property real chipGap: 8
    readonly property real chipHeight: 32
    readonly property real trackingChip: 0.04
    readonly property real segmentLabelTop: 4
    readonly property real segmentLabelBottom: 10
    readonly property real segmentBottom: 12
    readonly property real firstRunWidth: 560
    readonly property real firstRunPaddingTop: 32
    readonly property real firstRunPaddingX: 36
    readonly property real firstRunPaddingBottom: 28
    readonly property real firstRunGap: 18
    readonly property real firstRunHeadGap: 14
    readonly property real firstRunLogoWidth: 40
    readonly property real firstRunLogoHeight: 43
    readonly property real lineHeightFirstRunTitle: 1.05
    readonly property real lineHeightPrefsTitle: 1.1
    readonly property real fontSizeFirstRunTitle: 34
    readonly property real firstRunActionsTop: 4
    readonly property real messageIconSize: 32
    readonly property real stepsGap: 10
    readonly property real stepGap: 8
    readonly property real fieldGap: 8
    readonly property real fieldPaddingX: 12
    readonly property real fieldIconGap: 8
    readonly property real buttonPaddingX: 16
    readonly property real buttonPaddingXSm: 12
    readonly property real buttonGapSm: 6
    readonly property real fontSizeButton: 14
    readonly property real fontWeightButton: 500
    readonly property real fontSizeHelper: 11.5
    readonly property real menuPadding: 4
    readonly property real menuItemPaddingX: 8
    readonly property real menuSeparatorMargin: 4
    readonly property real tabPaddingX: 14
    readonly property real splashLaneWidth: 160
    readonly property real splashGap: 18
    readonly property real aboutWidth: 560
    readonly property real aboutMetaGap: 6
    readonly property real aboutTextHeight: 160
}
