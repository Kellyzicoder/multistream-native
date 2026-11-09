cmake_minimum_required(VERSION 3.28...3.30)

# Multistream: send the one picture to many channels at once (YouTube, Facebook, Instagram, any RTMP)
find_package(Qt6 REQUIRED Widgets)

add_library(multistream-channels MODULE)
add_library(OBS::multistream-channels ALIAS multistream-channels)

target_sources(multistream-channels PRIVATE channels.cpp)

target_compile_options(
  multistream-channels
  PRIVATE $<$<PLATFORM_ID:Darwin>:-Wno-quoted-include-in-framework-header> $<$<PLATFORM_ID:Darwin>:-Wno-comma>
)

target_link_libraries(multistream-channels PRIVATE OBS::frontend-api OBS::libobs Qt::Widgets)

if(OS_WINDOWS)
  configure_file(cmake/windows/obs-module.rc.in multistream-channels.rc)
  target_sources(multistream-channels PRIVATE multistream-channels.rc)
endif()

set_target_properties_obs(
  multistream-channels
  PROPERTIES FOLDER plugins/multistream-channels
             PREFIX ""
             AUTOMOC ON
             AUTOUIC ON
             AUTORCC ON
)
