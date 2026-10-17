cmake_minimum_required(VERSION 3.22)

project(ArotronStudio VERSION 2.22.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# macOS: build universale (Apple Silicon + Intel), da macOS 11. Si puo' ridurre con -DCMAKE_OSX_ARCHITECTURES=arm64
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")

# Riusa i sorgenti JUCE gia' scaricati per Voice Cleaner, se ci sono.
set(_vc_juce "/Volumes/NVME_500/Progetti/VoiceCleaner/build/_deps/juce-src")
if(EXISTS "${_vc_juce}/CMakeLists.txt" AND NOT DEFINED FETCHCONTENT_SOURCE_DIR_JUCE)
    set(FETCHCONTENT_SOURCE_DIR_JUCE "${_vc_juce}" CACHE PATH "" FORCE)
endif()

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.9
    GIT_SHALLOW    ON)
FetchContent_MakeAvailable(JUCE)

juce_add_gui_app(ArotronStudio
    PRODUCT_NAME "ADR Dub Studio"
    VERSION 2.22.0
    BUNDLE_VERSION 2.22.0
    COMPANY_NAME "Arotron"
    BUNDLE_ID "com.arotron.studio"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon.png"
    DOCUMENT_EXTENSIONS adb arostudio
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "ADR Dub Studio usa il microfono per registrare l'audio sulle tracce."
    CAMERA_PERMISSION_ENABLED FALSE)

juce_generate_juce_header(ArotronStudio)

# cataloghi di traduzione dell'interfaccia (testo italiano = chiave)
juce_add_binary_data(ArotronLang NAMESPACE LangData HEADER_NAME LangData.h SOURCES Resources/lang/en.txt)
target_link_libraries(ArotronStudio PRIVATE ArotronLang)

# manuale utente (HTML con immagini, in italiano e inglese): si estrae e si apre dal menu Aiuto
juce_add_binary_data(ArotronManual NAMESPACE ManualData HEADER_NAME ManualData.h SOURCES Resources/manual/manual_it.zip Resources/manual/manual_en.zip)
target_link_libraries(ArotronStudio PRIVATE ArotronManual)

target_sources(ArotronStudio PRIVATE
    Source/Main.cpp
    Source/Model.cpp
    Source/AudioEngine.cpp
    Source/VideoViewer.cpp
    Source/PluginHost.cpp
    Source/BuiltinFx.cpp
    Source/PluginUi.cpp
    Source/ReaperTheme.cpp
    Source/ThemeManager.cpp
    Source/DubWorkflow.cpp
    Source/CueSheet.cpp
    Source/AudioAlign.cpp
    Source/AutoDuck.cpp
    Source/BoothTools.cpp
    Source/Relink.cpp
    Source/Versions.cpp
    Source/SessionCheck.cpp
    Source/WorkflowGuide.cpp
    Source/Conform.cpp
    Source/ExternalSync.cpp
    Source/Lang.cpp
    Source/EditBar.cpp
    Source/ExportEngine.cpp
    Source/ExportDialog.cpp
    Source/SessionSettings.cpp
    Source/AdrImport.cpp
    Source/TakeImport.cpp
    Source/AudioTools.cpp
    Source/InfoLine.cpp
    Source/AdrPanel.cpp
    Source/TimelineView.cpp
    Source/MixerView.cpp
    Source/SpectrumView.cpp
    Source/SampleEditor.cpp
    Source/Pool.cpp
    Source/Manual.cpp
    Source/LogicalEditor.cpp
    Source/EqEditor.cpp
    Source/MainComponent.cpp)

# Video: su macOS AVFoundation; altrove (Windows) ffmpeg/ffprobe accanto all'app o nel PATH.
# Su macOS si puo' provare il backend ffmpeg con -DADR_FFMPEG_VIDEO=ON.
if(APPLE)
    option(ADR_FFMPEG_VIDEO "Video via ffmpeg invece di AVFoundation" OFF)
else()
    set(ADR_FFMPEG_VIDEO ON)
endif()
if(ADR_FFMPEG_VIDEO)
    target_sources(ArotronStudio PRIVATE Source/VideoSupportFfmpeg.cpp Source/FfmpegFramePlayer.cpp)
else()
    target_sources(ArotronStudio PRIVATE Source/VideoSupport.mm)
endif()

target_compile_definitions(ArotronStudio PRIVATE
    JUCE_USE_CAMERA=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=0
    JUCE_MODAL_LOOPS_PERMITTED=0
    JUCE_APPLICATION_NAME_STRING="ADR Dub Studio"
    JUCE_APPLICATION_VERSION_STRING="2.22.0")

if(ADR_FFMPEG_VIDEO)
    target_compile_definitions(ArotronStudio PRIVATE ADR_FFMPEG_VIDEO=1)
else()
    target_compile_definitions(ArotronStudio PRIVATE ADR_FFMPEG_VIDEO=0)
endif()

if(APPLE AND NOT ADR_FFMPEG_VIDEO)
    target_link_libraries(ArotronStudio PRIVATE "-framework AVFoundation" "-framework CoreMedia")
endif()
if(NOT ADR_FFMPEG_VIDEO)
    target_link_libraries(ArotronStudio PRIVATE juce::juce_video)
endif()

target_link_libraries(ArotronStudio
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
