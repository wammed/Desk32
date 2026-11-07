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
)

target_link_libraries(test_sample_rate_switching PRIVATE
    Qt6::Core
    Qt6::Test
    ${SOXR_LIBRARIES}
)

target_include_directories(test_sample_rate_switching PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${SOXR_INCLUDE_DIRS}
)

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
)

target_link_libraries(test_step4_deck_session_log PRIVATE
    Qt6::Core
    Qt6::Test
    Qt6::Sql
    ${SOXR_LIBRARIES}
)

target_include_directories(test_step4_deck_session_log PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${SOXR_INCLUDE_DIRS}
)

add_test(NAME test_step4_deck_session_log COMMAND test_step4_deck_session_log)



