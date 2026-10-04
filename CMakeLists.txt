cmake_minimum_required(VERSION 3.16)
project(PrimoReels VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Quick Multimedia QuickDialogs2)
find_package(Threads REQUIRED)

# Find FFmpeg packages (imported targets for robust include/link flags)
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(SWSCALE REQUIRED IMPORTED_TARGET libswscale)
pkg_check_modules(SWRESAMPLE REQUIRED IMPORTED_TARGET libswresample)

# Create library to share code between executable and tests
add_library(PrimoReelsLib STATIC
    src/TimelineEngine.cpp
    src/ProjectFile.cpp
    src/ListModels.cpp
    src/DecoderThread.cpp
    src/PreloadThread.cpp
    src/ExportThread.cpp
    src/VideoEffects.cpp
    src/MediaDecoder.cpp
    src/FrameImageProvider.cpp
    src/AudioPlayer.cpp
    src/VideoFrameItem.cpp
)

target_compile_options(PrimoReelsLib PRIVATE -Wall -Wextra -Wpedantic)

target_link_libraries(PrimoReelsLib PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::Multimedia
    Qt6::QuickDialogs2
    Threads::Threads
    PkgConfig::AVCODEC
    PkgConfig::AVFORMAT
    PkgConfig::AVUTIL
    PkgConfig::SWSCALE
    PkgConfig::SWRESAMPLE
)

target_include_directories(PrimoReelsLib PUBLIC src)

# Executable
add_executable(PrimoReels src/main.cpp qml.qrc)
target_compile_options(PrimoReels PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(PrimoReels PRIVATE PrimoReelsLib)

# Install rules (reverse-DNS names per AppStream/Flathub convention:
# desktop id must match <id> in metainfo.xml).
include(GNUInstallDirs)
install(TARGETS PrimoReels RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/io.github.primoreels.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/io.github.primoreels.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES packaging/io.github.primoreels.metainfo.xml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)

# Registers one ctest per QtTest function (one process per test) when
# PRIMOREELS_TESTS_ISOLATE_FUNCTIONS=ON, otherwise a single ctest that runs
# the whole binary. Isolation matters under TSan: joining a finished QThread
# corrupts this system's TSan thread registry (CHECK failed:
# sanitizer_thread_registry.cpp), killing the process mid-suite and being
# masked by TSAN exitcode=0 — see the tsan job in .github/workflows.
function(primoreels_add_test NAME TARGET TIMEOUT LABELS)
    if(PRIMOREELS_TESTS_ISOLATE_FUNCTIONS)
        foreach(fn ${ARGN})
            add_test(NAME "${NAME}.${fn}" COMMAND ${TARGET} ${fn})
            set_tests_properties("${NAME}.${fn}" PROPERTIES TIMEOUT ${TIMEOUT}
                LABELS "${LABELS}" PASS_REGULAR_EXPRESSION "Totals:"
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_MEDIA_BACKEND=dummy")
        endforeach()
    else()
        add_test(NAME ${NAME} COMMAND ${TARGET})
        set_tests_properties(${NAME} PROPERTIES TIMEOUT ${TIMEOUT}
            LABELS "${LABELS}" PASS_REGULAR_EXPRESSION "Totals:"
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_MEDIA_BACKEND=dummy")
    endif()
endfunction()

# A TSan compile poisons the thread registry on joined QThreads and on
# radeonsi_drv_video threads (CHECK failed: sanitizer_thread_registry.cpp),
# killing the process mid-suite and being masked by TSAN exitcode=0 — see the
# tsan job in .github/workflows/build.yml.
if(CMAKE_CXX_FLAGS MATCHES "-fsanitize=thread")
    set(PRIMOREELS_TSAN 1)
    target_compile_definitions(PrimoReelsLib PRIVATE PRIMOREELS_TSAN=1)
endif()

# Tests
include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    option(PRIMOREELS_TESTS_ISOLATE_FUNCTIONS
        "Run each QtTest function in its own process" OFF)
    add_executable(PrimoReelsTests tests/TestTimelineEngine.cpp)
    if(PRIMOREELS_TSAN)
        target_compile_definitions(PrimoReelsTests PRIVATE PRIMOREELS_TSAN=1)
    endif()
    target_compile_options(PrimoReelsTests PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(PrimoReelsTests PRIVATE PrimoReelsLib Qt6::Test)
    primoreels_add_test(PrimoReelsTests PrimoReelsTests 120 "unit" invalidLoad_reportsFailure importMedia_leavesProgramUntouched importMedia_probesRealFileWithoutLoadingProgram seekBeforeLoad_isIgnored effectDefaults_areSane engineStartsWithoutAudio removeMedia_removesEntryAndEmits timelineAppend_unknownPathUsesPlaceholder timelineSequence_startTimesAccumulate timelineRemoveMoveClear timelineInsert_placesClipAndRecomputesStarts sequenceIndexOf_findsClips playSequenceFrom_emptyIsNoOp sequencePlayback_skipsFailedClipThenStops sequencePlayback_structuralEditCancels sequencePlayback_skipsOverlayClips playSequenceFrom_resolvesOverlayStart playSequenceFrom_allOverlayIsNoOp pause_preservesSequenceResume sequenceIndexOf_prefersV1Lane moveTimelineClip_keepsPlayingIndex projectSaveLoad_roundTrip importMedia_marksDirtyAndUndoable importMedia_staysDirtyAfterProbe importMedia_undoRedoReprobes projectSave_overwriteKeepsUsableFile projectLoad_invalidFileKeepsState undoRedo_timelineEdits undoRedo_removeAndClear newProject_resetsAndIsUndoable thumbnails_failedDecodeStillResolves trim_clampsAndPreservesPlayable split_dividesClipAndCarriesTrims split_absoluteTracksShiftRightStart setClipEffect_validatesAndIsUndoable setClipTransition_validatesClampsAndUndoes overlayClips_keepAbsolutePosition setClipTitle_truncatesUndoableAndPersists audioGainScalesAndClamps clipFadeGain_ramps mixAudio_sumsAndClamps audioBed_modelOps bedClipAt_resolvesTopmostAudible projectSaveLoad_preservesAudioFields previewChunkAppliesGainAndFade liveBedMix_containsBothTones liveBedOnly_withoutProgram bedFailure_warnsTransiently sourcePlayback_leavesProgramAudioUntouched audioPlayerBuffersWithoutBackend effectSetters_clampOutOfRange catalogs_matchLibrary cancelExport_withoutExportIsNoop exportEmpty_reportsFailure continuousScrubbing_rapidSeeksConverge listModels_stayInSyncWithLists listModels_incrementalSync listModels_directSyncOps droppedSeek_releasesSeekingGate seekPreemptedByAudioRestart_emitsSeekDropped preloadRetarget_samePathNewPositionRewarms audioSeekPastTruncatedTail_reportsFailure redoOfFailedImport_dropsGhostPlaceholder exportWarning_isAdvisoryNotFailure decoderStopAndWait_joinsWorker engineTeardown_withInflightOpen_isClean loadProject_rejectsOversizeFile loadProject_validatesMediaRows loadProject_rejectsForeignCurrentSource newProject_clearsWaveThumbCaches)
    add_executable(ExportTests tests/TestExport.cpp)
    if(PRIMOREELS_TSAN)
        target_compile_definitions(ExportTests PRIVATE PRIMOREELS_TSAN=1)
    endif()
    target_compile_options(ExportTests PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(ExportTests PRIVATE PrimoReelsLib Qt6::Test)
    primoreels_add_test(ExportTests ExportTests 300 "integration" exportEmptyTimeline_reportsFailure exportSequence_rendersMp4 exportSequence_honorsTrim exportSequence_rendersDissolve sequencePlayback_advancesThroughRealClips waveforms_audioClipCachesPeaks exportSequence_compositesOverlay exportSequence_mixesA1Bed exportSequence_appliesGainMuteAndVolume exportSequence_appliesFades exportSequence_audioOnlyRendersPlayableVideo thumbnails_realClipCachesImage playbackWithRealMedia_playPauseSeek sourceTransport_playPauseSeek previewTimelineClip_setsEffectWindow overlayStartTime_movesFreely removeMedia_cascadesToTimeline seekNonFinite_isIgnored newProject_clearsPendingLoad cancelExport_duringActiveExport exportDestination failedExport_preservesExistingDestination imageProviders_renderWithoutCrash clipUtils_normalizesFileUrls exportRejectsOverlongTimeline projectVersionMismatch_isRejected independentSourcePlayback_doesNotDisturbProgram)
    add_executable(EffectTests tests/TestEffects.cpp)
    target_compile_options(EffectTests PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(EffectTests PRIVATE PrimoReelsLib Qt6::Test)
    primoreels_add_test(EffectTests EffectTests 120 "unit" catalogMatchesLibrary passthroughCases eachEffectKeepsSizeAndChangesPixels tinyImagesDoNotCrash vignetteDarkensCorners)
    add_executable(QmlLoadTests tests/TestQmlLoad.cpp qml.qrc)
    target_compile_options(QmlLoadTests PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(QmlLoadTests PRIVATE PrimoReelsLib Qt6::Test)
    primoreels_add_test(QmlLoadTests QmlLoadTests 120 "unit" mainWindow_loads mainWindow_interaction dialogs_existAndConfigured layout_saveRestoreReset timeline_dndRulerPlayhead timeline_a1Interactions library_catalogsAndTabs recentsAndTitle)
endif()

set(CPACK_PACKAGE_NAME "primoreels")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "PrimoReels")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Modern desktop video editor")
set(CPACK_GENERATOR "TGZ;DEB")
# Resolve .deb dependencies from linked libraries at pack time instead of
# pinning FFmpeg SONAMEs (libavcodec60, ...), which break on FFmpeg 7 distros.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# Baseline Qt deps (shlibdeps appends FFmpeg + QuickDialogs2 SONAMEs at pack time).
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt6core6, libqt6gui6, libqt6quick6, libqt6multimedia6, libqt6quickdialogs2-6")
include(CPack)
