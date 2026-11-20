find_package(Qt6 REQUIRED COMPONENTS Test)

add_executable(test_foundation
    test_foundation.cpp
)

target_link_libraries(test_foundation PRIVATE
    Qt6::Core
    Qt6::Test
)

add_test(NAME test_foundation COMMAND test_foundation)

add_executable(test_network_offline
    test_network_offline.cpp
)

target_link_libraries(test_network_offline PRIVATE
    Qt6::Core
    Qt6::Test
)

add_test(NAME test_network_offline COMMAND test_network_offline)

add_executable(test_bit_perfect
    test_bit_perfect.cpp
)

target_link_libraries(test_bit_perfect PRIVATE
    Qt6::Core
    Qt6::Test
)

add_test(NAME test_bit_perfect COMMAND test_bit_perfect)

add_executable(test_alsa_capabilities
    test_alsa_capabilities.cpp
)

target_link_libraries(test_alsa_capabilities PRIVATE
    Qt6::Core
    Qt6::Test
    ALSA::ALSA
)

add_test(NAME test_alsa_capabilities COMMAND test_alsa_capabilities)

add_executable(test_audio_backends
    test_audio_backends.cpp
)

target_link_libraries(test_audio_backends PRIVATE
    Qt6::Core
    Qt6::Test
    ${PIPEWIRE_LIBRARIES}
    ${JACK_LIBRARIES}
    ALSA::ALSA
)

target_include_directories(test_audio_backends SYSTEM PRIVATE
    ${PIPEWIRE_INCLUDE_DIRS}
    ${JACK_INCLUDE_DIRS}
)

add_test(NAME test_audio_backends COMMAND test_audio_backends)

# Step 2: DSP Measurements, Format Conversion, and Memory Reclamation
add_executable(test_dsp_measurement
    test_dsp_measurement.cpp
)

target_link_libraries(test_dsp_measurement PRIVATE
    Qt6::Core
    Qt6::Test
)

target_include_directories(test_dsp_measurement PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

add_test(NAME test_dsp_measurement COMMAND test_dsp_measurement)

# Step 2: Kill Gate K3 float64 processing load benchmark
add_executable(benchmark_k3_float64
    benchmark_k3_float64.cpp
)

target_link_libraries(benchmark_k3_float64 PRIVATE
    Qt6::Core
    Qt6::Test
)

target_include_directories(benchmark_k3_float64 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

add_test(NAME benchmark_k3_float64 COMMAND benchmark_k3_float64)

# Step 3: Different Sample Rate Switching & Kill Gate K2 PoC
add_executable(test_sample_rate_switching
    test_sample_rate_switching.cpp
    ../src/engine/metadata/TrackMetadata.cpp
    ../src/engine/decoder/FFmpegAudioDecoder.cpp
    ../src/engine/decoder/DecoderWorker.cpp
    ../src/engine/session/FolderSession.cpp
    ../src/engine/deck/ReferenceDeck.cpp
    ../src/engine/deck/DeckManager.cpp
)

target_link_libraries(test_sample_rate_switching PRIVATE
    Qt6::Core
    Qt6::Test
    ${SOXR_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${TAGLIB_LIBRARIES}
)

target_include_directories(test_sample_rate_switching PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${SOXR_INCLUDE_DIRS}
)

target_include_directories(test_sample_rate_switching SYSTEM PRIVATE
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${TAGLIB_INCLUDE_DIRS}
)
target_compile_options(test_sample_rate_switching PRIVATE ${TAGLIB_CFLAGS_OTHER})

add_test(NAME test_sample_rate_switching COMMAND test_sample_rate_switching)

# Step 3: External Insert Bridge PoC & Kill Gate K1 Evaluation
add_executable(test_external_insert_bridge
    test_external_insert_bridge.cpp
)

target_link_libraries(test_external_insert_bridge PRIVATE
    Qt6::Core
    Qt6::Test
)

target_include_directories(test_external_insert_bridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

add_test(NAME test_external_insert_bridge COMMAND test_external_insert_bridge)

# Step 4: Deck, Folder Session, and Operation Log PoC
add_executable(test_step4_deck_session_log
    test_step4_deck_session_log.cpp
    ../src/engine/metadata/TrackMetadata.cpp
    ../src/engine/decoder/FFmpegAudioDecoder.cpp
    ../src/engine/decoder/DecoderWorker.cpp
    ../src/engine/session/FolderSession.cpp
    ../src/engine/deck/ReferenceDeck.cpp
    ../src/engine/deck/DeckManager.cpp
)

target_link_libraries(test_step4_deck_session_log PRIVATE
    Qt6::Core
    Qt6::Test
    Qt6::Sql
    ${SOXR_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${TAGLIB_LIBRARIES}
)

target_include_directories(test_step4_deck_session_log PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${SOXR_INCLUDE_DIRS}
)

target_include_directories(test_step4_deck_session_log SYSTEM PRIVATE
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${TAGLIB_INCLUDE_DIRS}
)
target_compile_options(test_step4_deck_session_log PRIVATE ${TAGLIB_CFLAGS_OTHER})

add_test(NAME test_step4_deck_session_log COMMAND test_step4_deck_session_log)

# Phase 1.1: Audio Pipeline & Core Engine Integration
add_executable(test_audio_pipeline
    test_audio_pipeline.cpp
)

target_link_libraries(test_audio_pipeline PRIVATE
    Qt6::Core
    Qt6::Test
    ${SOXR_LIBRARIES}
)

target_include_directories(test_audio_pipeline PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${SOXR_INCLUDE_DIRS}
)

add_test(NAME test_audio_pipeline COMMAND test_audio_pipeline)
set_tests_properties(test_audio_pipeline PROPERTIES TIMEOUT 300)

# Phase 1.2: Native PipeWire AudioOutput Implementation
add_executable(test_pipewire_output
    test_pipewire_output.cpp
    ../src/engine/backend/PipeWireAudioOutput.cpp
    ../src/engine/backend/PipeWireControlAdapter.cpp
)

target_link_libraries(test_pipewire_output PRIVATE
    Qt6::Core
    Qt6::Test
    ${PIPEWIRE_LIBRARIES}
    ${SOXR_LIBRARIES}
)

target_include_directories(test_pipewire_output PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_include_directories(test_pipewire_output SYSTEM PRIVATE
    ${PIPEWIRE_INCLUDE_DIRS}
    ${SOXR_INCLUDE_DIRS}
)

add_test(NAME test_pipewire_output COMMAND test_pipewire_output)

# Phase 1.3: FFmpeg Audio Decoder Implementation
add_executable(test_ffmpeg_decoder
    test_ffmpeg_decoder.cpp
    ../src/engine/metadata/TrackMetadata.cpp
    ../src/engine/decoder/FFmpegAudioDecoder.cpp
)

target_link_libraries(test_ffmpeg_decoder PRIVATE
    Qt6::Core
    Qt6::Test
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${TAGLIB_LIBRARIES}
)

target_include_directories(test_ffmpeg_decoder PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_include_directories(test_ffmpeg_decoder SYSTEM PRIVATE
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${TAGLIB_INCLUDE_DIRS}
)
target_compile_options(test_ffmpeg_decoder PRIVATE ${TAGLIB_CFLAGS_OTHER})

add_test(NAME test_ffmpeg_decoder COMMAND test_ffmpeg_decoder)

# Phase 1.4: Reference Deck System & Folder Session Implementation
add_executable(test_reference_decks
    test_reference_decks.cpp
    ../src/engine/metadata/TrackMetadata.cpp
    ../src/engine/decoder/FFmpegAudioDecoder.cpp
    ../src/engine/decoder/DecoderWorker.cpp
    ../src/engine/session/FolderSession.cpp
    ../src/engine/deck/ReferenceDeck.cpp
    ../src/engine/deck/DeckManager.cpp
)

target_link_libraries(test_reference_decks PRIVATE
    Qt6::Core
    Qt6::Test
    ${SOXR_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${TAGLIB_LIBRARIES}
)

target_include_directories(test_reference_decks PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${SOXR_INCLUDE_DIRS}
)

target_include_directories(test_reference_decks SYSTEM PRIVATE
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${TAGLIB_INCLUDE_DIRS}
)
target_compile_options(test_reference_decks PRIVATE ${TAGLIB_CFLAGS_OTHER})

add_test(NAME test_reference_decks COMMAND test_reference_decks)

# Phase 1.5: Log Tab Infrastructure Implementation
add_executable(test_log_service
    test_log_service.cpp
    ../src/core/log/LogService.cpp
)

target_link_libraries(test_log_service PRIVATE
    Qt6::Core
    Qt6::Test
    Qt6::Sql
)

target_include_directories(test_log_service PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

add_test(NAME test_log_service COMMAND test_log_service)

# Phase 2: Acoustic Correction & Built-in DSP Implementation
add_executable(test_phase2_dsp
    test_phase2_dsp.cpp
    ../src/core/EngineController.cpp
    ../src/core/log/LogService.cpp
    ../src/engine/preset/PresetManager.cpp
    ../src/engine/metadata/TrackMetadata.cpp
    ../src/engine/workspace/WorkspaceManager.cpp
    ../src/engine/decoder/DecoderWorker.cpp
    ../src/engine/decoder/FFmpegAudioDecoder.cpp
    ../src/engine/backend/PipeWireAudioOutput.cpp
    ../src/engine/backend/PipeWireControlAdapter.cpp
    ../src/engine/insert/PipeWireInsertTransport.cpp
    ../src/engine/render/RenderEngine.cpp
    ../src/engine/render/RenderSequenceManager.cpp
    ../src/engine/session/FolderSession.cpp
    ../src/engine/deck/ReferenceDeck.cpp
    ../src/engine/deck/DeckManager.cpp
)

target_link_libraries(test_phase2_dsp PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
    Qt6::Sql
    Qt6::Qml
    ${PIPEWIRE_LIBRARIES}
    ${SOXR_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${TAGLIB_LIBRARIES}
    ALSA::ALSA
)

target_include_directories(test_phase2_dsp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${SOXR_INCLUDE_DIRS}
)

target_include_directories(test_phase2_dsp SYSTEM PRIVATE
    ${PIPEWIRE_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${TAGLIB_INCLUDE_DIRS}
)
target_compile_options(test_phase2_dsp PRIVATE ${TAGLIB_CFLAGS_OTHER})

add_dependencies(test_phase2_dsp desk32_git_version)
add_test(NAME test_phase2_dsp COMMAND test_phase2_dsp)

# Phase 3: External Plugin Host Integration (External Insert & Preset Management)
add_executable(test_phase3_external_insert
    test_phase3_external_insert.cpp
    ../src/core/EngineController.cpp
    ../src/core/log/LogService.cpp
    ../src/engine/preset/PresetManager.cpp
    ../src/engine/metadata/TrackMetadata.cpp
    ../src/engine/workspace/WorkspaceManager.cpp
    ../src/engine/decoder/DecoderWorker.cpp
    ../src/engine/decoder/FFmpegAudioDecoder.cpp
    ../src/engine/backend/PipeWireAudioOutput.cpp
    ../src/engine/backend/PipeWireControlAdapter.cpp
    ../src/engine/insert/PipeWireInsertTransport.cpp
    ../src/engine/render/RenderEngine.cpp
    ../src/engine/render/RenderSequenceManager.cpp
    ../src/engine/session/FolderSession.cpp
    ../src/engine/deck/ReferenceDeck.cpp
    ../src/engine/deck/DeckManager.cpp
)

target_link_libraries(test_phase3_external_insert PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
    Qt6::Sql
    Qt6::Qml
    ${PIPEWIRE_LIBRARIES}
    ${SOXR_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${TAGLIB_LIBRARIES}
    ALSA::ALSA
)

target_include_directories(test_phase3_external_insert PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${SOXR_INCLUDE_DIRS}
)

target_include_directories(test_phase3_external_insert SYSTEM PRIVATE
    ${PIPEWIRE_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${TAGLIB_INCLUDE_DIRS}
)
target_compile_options(test_phase3_external_insert PRIVATE ${TAGLIB_CFLAGS_OTHER})

add_dependencies(test_phase3_external_insert desk32_git_version)
add_test(NAME test_phase3_external_insert COMMAND test_phase3_external_insert)

# Phase 4: Export and Render Engine (Offline Processing, Bijective Base-26 Serial, Staging & Rollback)
add_executable(test_phase4_export_render
    test_phase4_export_render.cpp
    ../src/core/EngineController.cpp
    ../src/core/log/LogService.cpp
    ../src/engine/preset/PresetManager.cpp
    ../src/engine/metadata/TrackMetadata.cpp
    ../src/engine/workspace/WorkspaceManager.cpp
    ../src/engine/decoder/DecoderWorker.cpp
    ../src/engine/decoder/FFmpegAudioDecoder.cpp
    ../src/engine/backend/PipeWireAudioOutput.cpp
    ../src/engine/backend/PipeWireControlAdapter.cpp
    ../src/engine/insert/PipeWireInsertTransport.cpp
    ../src/engine/render/RenderEngine.cpp
    ../src/engine/render/RenderSequenceManager.cpp
    ../src/engine/session/FolderSession.cpp
    ../src/engine/deck/ReferenceDeck.cpp
    ../src/engine/deck/DeckManager.cpp
)

target_link_libraries(test_phase4_export_render PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
    Qt6::Sql
    Qt6::Qml
    ${PIPEWIRE_LIBRARIES}
    ${SOXR_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${TAGLIB_LIBRARIES}
    ALSA::ALSA
)

target_include_directories(test_phase4_export_render PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${SOXR_INCLUDE_DIRS}
)

target_include_directories(test_phase4_export_render SYSTEM PRIVATE
    ${PIPEWIRE_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${TAGLIB_INCLUDE_DIRS}
)
target_compile_options(test_phase4_export_render PRIVATE ${TAGLIB_CFLAGS_OTHER})

add_dependencies(test_phase4_export_render desk32_git_version)
add_test(NAME test_phase4_export_render COMMAND test_phase4_export_render)
set_tests_properties(test_phase4_export_render PROPERTIES TIMEOUT 300)

# Decoder Ring-Buffer Underrun (Error Code 1020) Investigation & Reproducibility Tests
add_executable(test_underrun_investigation
    test_underrun_investigation.cpp
    ../src/engine/metadata/TrackMetadata.cpp
    ../src/engine/decoder/FFmpegAudioDecoder.cpp
    ../src/engine/decoder/DecoderWorker.cpp
    ../src/engine/session/FolderSession.cpp
    ../src/engine/deck/ReferenceDeck.cpp
    ../src/engine/deck/DeckManager.cpp
)

target_compile_definitions(test_underrun_investigation PRIVATE
    DESK32_ENABLE_TEST_HOOKS
)

target_link_libraries(test_underrun_investigation PRIVATE
    Qt6::Core
    Qt6::Test
    ${SOXR_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${TAGLIB_LIBRARIES}
)

target_include_directories(test_underrun_investigation PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${SOXR_INCLUDE_DIRS}
)

target_include_directories(test_underrun_investigation SYSTEM PRIVATE
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${TAGLIB_INCLUDE_DIRS}
)
target_compile_options(test_underrun_investigation PRIVATE ${TAGLIB_CFLAGS_OTHER})

add_test(NAME test_underrun_investigation COMMAND test_underrun_investigation)
set_tests_properties(test_underrun_investigation PROPERTIES TIMEOUT 300)

# v3.8.1 Dedicated Workspace, TagLib Metadata, Direct Mode, Tap Isolation, 16-Band PEQ, Filter Q & Presets
add_executable(test_workspace_v381
    test_workspace_v381.cpp
    ../src/core/EngineController.cpp
    ../src/core/log/LogService.cpp
    ../src/engine/preset/PresetManager.cpp
    ../src/engine/metadata/TrackMetadata.cpp
    ../src/engine/workspace/WorkspaceManager.cpp
    ../src/engine/decoder/DecoderWorker.cpp
    ../src/engine/decoder/FFmpegAudioDecoder.cpp
    ../src/engine/backend/PipeWireAudioOutput.cpp
    ../src/engine/backend/PipeWireControlAdapter.cpp
    ../src/engine/insert/PipeWireInsertTransport.cpp
    ../src/engine/render/RenderEngine.cpp
    ../src/engine/render/RenderSequenceManager.cpp
    ../src/engine/session/FolderSession.cpp
    ../src/engine/deck/ReferenceDeck.cpp
    ../src/engine/deck/DeckManager.cpp
)

target_link_libraries(test_workspace_v381 PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
    Qt6::Sql
    Qt6::Qml
    ${PIPEWIRE_LIBRARIES}
    ${SOXR_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${TAGLIB_LIBRARIES}
    ALSA::ALSA
)

target_include_directories(test_workspace_v381 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${SOXR_INCLUDE_DIRS}
)

target_include_directories(test_workspace_v381 SYSTEM PRIVATE
    ${PIPEWIRE_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${TAGLIB_INCLUDE_DIRS}
)
target_compile_options(test_workspace_v381 PRIVATE ${TAGLIB_CFLAGS_OTHER})

add_dependencies(test_workspace_v381 desk32_git_version)
add_test(NAME test_workspace_v381 COMMAND test_workspace_v381)
set_tests_properties(test_workspace_v381 PROPERTIES TIMEOUT 300)









