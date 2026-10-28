# Audio: synth, SFX and the command queue (depends on core only).
add_library(litholattice_audio STATIC voice.cpp synth.cpp audio_engine.cpp voice_allocator.cpp occlusion_thread.cpp spatial_audio.cpp effects.cpp streaming.cpp codecs.cpp audio_selftest.cpp)
target_include_directories(litholattice_audio PUBLIC include)
target_link_libraries(litholattice_audio PUBLIC litholattice_core)
litholattice_module_options(litholattice_audio)

# Platform audio backend: miniaudio (the only user of miniaudio.h). Implements openAudioOutput().
# MA_NO_WEBAUDIO (2026-10-28, purge stage 1): the vendored header's WebAudio
# backend is written against the banned script-snippet macro, and its device
# bodies call page-layer helpers the thin runtime never provided — web audio
# output was broken by construction (finding B1) and only dormant because
# builds boot muted. Web audio returns later via our own path; the vendored
# header itself is NOT patched.
find_package(Threads REQUIRED)
add_library(litholattice_miniaudio_impl STATIC miniaudio/miniaudio_impl.c)
target_include_directories(litholattice_miniaudio_impl PUBLIC ${LITHOLATTICE_MINIAUDIO_DIR})
target_compile_definitions(litholattice_miniaudio_impl PUBLIC MA_NO_DECODING MA_NO_ENCODING MA_NO_GENERATION
                           MA_NO_RESOURCE_MANAGER MA_NO_NODE_GRAPH MA_NO_ENGINE MA_NO_WEBAUDIO)
target_link_libraries(litholattice_miniaudio_impl PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(UNIX AND NOT APPLE)
  target_link_libraries(litholattice_miniaudio_impl PUBLIC m)
endif()

add_library(litholattice_audio_device STATIC miniaudio/miniaudio_output.cpp)
target_link_libraries(litholattice_audio_device PUBLIC litholattice_audio PRIVATE litholattice_miniaudio_impl)
litholattice_module_options(litholattice_audio_device)
