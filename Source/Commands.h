#pragma once

#include <JuceHeader.h>

namespace fc
{
enum Cmd : juce::CommandID
{
    cmdNew = 0x3000, cmdOpen, cmdSave, cmdSaveAs, cmdImport, cmdExport, cmdAudioSettings,
    cmdUndo, cmdRedo, cmdSelectAll, cmdDelete, cmdDuplicate, cmdSplit,
    cmdAddTrack, cmdRemoveTrack, cmdRenameTrack,
    cmdPlay, cmdHome, cmdEnd, cmdLoop, cmdLoopFromSel, cmdNudgeL, cmdNudgeR, cmdNudgeLBig, cmdNudgeRBig,
    cmdSnap, cmdFollow, cmdZoomIn, cmdZoomOut, cmdZoomFit, cmdMixer, cmdTaller, cmdShorter,
    cmdRecord, cmdArm, cmdMonitor, cmdImportVideo, cmdRemoveVideo, cmdExportVideo, cmdShowVideo,
    cmdDetachVideo, cmdFitTracks, cmdVideoOffsetMinus, cmdVideoOffsetPlus,
    cmdToolSelect, cmdToolRazor, cmdToolSlip, cmdToolHand, cmdToolZoom,
    cmdRippleDelete, cmdTrimStart, cmdTrimEnd, cmdTrackUp, cmdTrackDown, cmdDuplicateTrack,
    cmdAddPlugin, cmdAddMasterPlugin, cmdPdc,
    cmdAddMarker, cmdPrevMarker, cmdNextMarker, cmdExportMarkers, cmdToolAuto, cmdClearAutomation, cmdShortcuts, cmdCollectMedia, cmdClearRecent, cmdSessionSettings, cmdRulerTc,
    cmdAdrWindow, cmdAdrImport, cmdAdrAddCue, cmdAdrRecord, cmdAdrNextCue, cmdAdrPrevCue, cmdAdrExportCsv, cmdAdrExportPT, cmdAdrExportBwf, cmdTimeStretch, cmdToolStretch, cmdReturnToStart, cmdQuit, cmdAdrCueSheet, cmdAlignToGuide, cmdAutoDuck, cmdDetectCuts, cmdScriptReader, cmdCueMix, cmdRelink, cmdSaveVersion, cmdVersions, cmdCheckSession, cmdWorkflow, cmdReConform, cmdMtc, cmdKeyEditor, cmdAdrImportTakes, cmdJoin,
    cmdToolRange, cmdToolMute, cmdToolErase, cmdToolComp, cmdRangeSplit, cmdRangeDelete, cmdRangeRemove, cmdRangeTrim, cmdRangeLoop, cmdEditMode,
    cmdStripSilence, cmdProcInvert, cmdProcReverse, cmdProcDc, cmdProcLufs, cmdInfoLine, cmdSnapZero, cmdAutoFade, cmdCrossfade, cmdBatchExport, cmdExportPackage, cmdExportTrackArchive, cmdImportTrackArchive, cmdNotes, cmdCycleMarker
};

/** Strumenti della timeline. */
enum class Tool { Select, Razor, Slip, Hand, Zoom, Automation, Stretch, Range, Mute, Erase, Comp };

} // namespace fc
