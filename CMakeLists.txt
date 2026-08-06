cmake_minimum_required(VERSION 3.16)
project(rtsp-server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Dependencies ---
find_package(nlohmann_json REQUIRED)
find_package(spdlog REQUIRED)
find_package(CURL REQUIRED)

# --- ASR-LLM-TTS pipeline (optional, link if available) ---
set(PIPELINE_DIR "/eir/lixin/ASR-LLM-TTS" CACHE PATH "Path to ASR-LLM-TTS project")
set(PIPELINE_SRC_DIR "${PIPELINE_DIR}/src")

# --- Source files ---
set(SERVER_SOURCES
    src/main.cpp
    src/server/ws_server.cpp
    src/server/session_manager.cpp
    src/server/rtsp_manager.cpp
    src/pipeline/pipeline_bridge.cpp
    # Skills & memory
    src/brain/skill_manager.cpp
    src/brain/skills/skill_weather.cpp
    src/brain/skills/skill_time.cpp
    src/brain/skills/skill_calculator.cpp
    src/brain/skills/skill_entertainment.cpp
    src/brain/skills/skill_riddle.cpp
    src/brain/skills/skill_fortune.cpp
    src/brain/skills/skill_poetry.cpp
    src/brain/skills/skill_games.cpp
    src/brain/skills/skill_memory.cpp
    src/memory/chat_memory.cpp
    src/memory/user_memory.cpp
    src/memory/token_counter.cpp
    src/llm/function_caller.cpp
)

set(SERVER_HEADERS
    src/server/ws_server.h
    src/server/session_manager.h
    src/server/rtsp_manager.h
    src/pipeline/pipeline_bridge.h
    src/utils/base64.h
    src/utils/logger.h
    src/utils/http_client.h
    src/utils/feishu_notifier.h
    # Skills & memory headers
    src/brain/skill_base.h
    src/brain/skill_utils.h
    src/brain/skill_manager.h
    src/brain/skills/skill_weather.h
    src/brain/skills/skill_time.h
    src/brain/skills/skill_calculator.h
    src/brain/skills/skill_entertainment.h
    src/brain/skills/skill_riddle.h
    src/brain/skills/skill_fortune.h
    src/brain/skills/skill_poetry.h
    src/brain/skills/skill_games.h
    src/brain/skills/skill_memory.h
    src/memory/chat_memory.h
    src/memory/user_memory.h
    src/memory/vector_store.h
    src/memory/token_counter.h
    src/llm/function_caller.h
)

# --- Executable ---
add_executable(rtsp-mtx-server ${SERVER_SOURCES} ${SERVER_HEADERS})

target_include_directories(rtsp-mtx-server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/speech
    ${PIPELINE_SRC_DIR}
    ${PIPELINE_SRC_DIR}/third_party/sherpa-onnx/include
)

# sherpa-onnx C API (ASR)
set(SHERPA_ONNX_DIR "${PIPELINE_SRC_DIR}/third_party/sherpa-onnx")
if(EXISTS "${SHERPA_ONNX_DIR}/lib/libsherpa-onnx-c-api.so")
    target_compile_definitions(rtsp-mtx-server PRIVATE SHERPA_ONNX_AVAILABLE=1)
    target_link_directories(rtsp-mtx-server PRIVATE "${SHERPA_ONNX_DIR}/lib")
    message(STATUS "sherpa-onnx C API found — using direct library calls for ASR")
else()
    message(STATUS "sherpa-onnx C API not found — falling back to popen() for ASR")
endif()

# espeak-ng direct library (TTS)
# The dev package may not be installed, so try the versioned .so first
find_library(ESPEAK_NG_LIB espeak-ng PATHS /usr/lib/x86_64-linux-gnu /usr/lib NO_DEFAULT_PATH)
if(NOT ESPEAK_NG_LIB)
  find_library(ESPEAK_NG_LIB espeak-ng)
endif()
# Fallback: check for versioned .so explicitly
if(NOT ESPEAK_NG_LIB AND EXISTS "/usr/lib/x86_64-linux-gnu/libespeak-ng.so.1")
  set(ESPEAK_NG_LIB "/usr/lib/x86_64-linux-gnu/libespeak-ng.so.1")
endif()
if(ESPEAK_NG_LIB)
    target_compile_definitions(rtsp-mtx-server PRIVATE ESPEAK_NG_AVAILABLE=1)
    message(STATUS "espeak-ng found at ${ESPEAK_NG_LIB} — using direct library calls for TTS")
else()
    set(ESPEAK_NG_LIB "")
    message(STATUS "espeak-ng not found — falling back to system() for TTS")
endif()

# Conditionally link sherpa-onnx
if(EXISTS "${SHERPA_ONNX_DIR}/lib/libsherpa-onnx-c-api.so")
    set(SHERPA_ONNX_LIBS
        "${SHERPA_ONNX_DIR}/lib/libsherpa-onnx-c-api.so"
        "${SHERPA_ONNX_DIR}/lib/libonnxruntime.so"
    )
else()
    set(SHERPA_ONNX_LIBS "")
endif()

target_link_libraries(rtsp-mtx-server PRIVATE
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    CURL::libcurl
    pthread
    ${ESPEAK_NG_LIB}
    ${SHERPA_ONNX_LIBS}
    dl
)

# Optionally link against the ASR-LLM-TTS pipeline library if built
if(EXISTS "${PIPELINE_DIR}/build/libvoice_pipeline.a")
    target_link_libraries(rtsp-mtx-server PRIVATE
        "${PIPELINE_DIR}/build/libvoice_pipeline.a"
        "${PIPELINE_DIR}/build/libspeech.a"
        "${PIPELINE_DIR}/build/libbrain.a"
        "${PIPELINE_DIR}/build/libllm.a"
        "${PIPELINE_DIR}/build/libaudio.a"
        "${PIPELINE_DIR}/build/libmemory.a"
    )
    target_compile_definitions(rtsp-mtx-server PRIVATE HAS_VOICE_PIPELINE=1)
    message(STATUS "Linking against ASR-LLM-TTS pipeline libraries")
else()
    message(STATUS "ASR-LLM-TTS pipeline not built — using stub (text-only mode)")
endif()

# --- Tests ---
option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_executable(test_ws_protocol tests/test_ws_protocol.cpp src/server/ws_server.cpp)
    target_include_directories(test_ws_protocol PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_ws_protocol PRIVATE nlohmann_json::nlohmann_json spdlog::spdlog pthread)
    add_test(NAME ws_protocol COMMAND test_ws_protocol)
endif()

# --- Install ---
install(TARGETS rtsp-mtx-server DESTINATION bin)
install(FILES config.json DESTINATION etc/rtsp-server/)
