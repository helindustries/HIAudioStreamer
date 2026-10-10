cmake_minimum_required(VERSION 3.21)
project(AudioBridge VERSION 0.1.0 LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

set(SOURCES
    Source/AudioBackend.h
    Source/Autostart.cpp
    Source/Autostart.h
    Source/Bridge.cpp
    Source/Bridge.h
    Source/Config.cpp
    Source/Config.h
    Source/Crypto.cpp
    Source/Crypto.h
    Source/Main.cpp
    Source/Network.cpp
    Source/Network.h
    Source/Playout.cpp
    Source/Playout.h
    Source/Protocol.h
    Source/RingBuffer.h
    Source/Tray.cpp
    Source/Tray.h
    Source/ThirdParty/Monocypher/monocypher.c
    Source/ThirdParty/Monocypher/monocypher.h
)
if(WIN32)
    list(APPEND SOURCES Source/WindowsAudio.cpp)
elseif(APPLE)
    list(APPEND SOURCES Source/MacAudio.mm)
endif()

qt_add_executable(AudioBridge WIN32 MACOSX_BUNDLE ${SOURCES})
target_include_directories(AudioBridge PRIVATE Source)
target_link_libraries(AudioBridge PRIVATE Qt6::Widgets Qt6::Network)

if(WIN32)
    target_compile_definitions(AudioBridge PRIVATE NOMINMAX UNICODE _UNICODE)
    target_link_libraries(AudioBridge PRIVATE ole32 avrt mmdevapi)
elseif(APPLE)
    set_source_files_properties(Source/MacAudio.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set_target_properties(AudioBridge PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Source/Info.plist.in
        MACOSX_BUNDLE_GUI_IDENTIFIER com.helindustries.audiobridge
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
    target_link_libraries(AudioBridge PRIVATE "-framework CoreAudio" "-framework Foundation")
    # macOS ties audio capture permission to the code signature; an ad-hoc one at least keeps
    # the grant stable between launches of the same build.
    add_custom_command(TARGET AudioBridge POST_BUILD
        COMMAND codesign --force --sign - $<TARGET_BUNDLE_DIR:AudioBridge>
        VERBATIM)
endif()
