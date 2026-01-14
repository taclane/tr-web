# tr-web: Web Status Plugin for Trunk-Recorder
# A lightweight web dashboard with no external dependencies beyond trunk-recorder's existing requirements

# Generate embedded web assets header using a basic shell script
set(TR_WEB_ASSETS_HEADER "${CMAKE_CURRENT_BINARY_DIR}/web_assets.h")
set(TR_WEB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/web")
set(EMBED_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/embed_assets.sh")

add_custom_command(
    OUTPUT ${TR_WEB_ASSETS_HEADER}
    COMMAND ${EMBED_SCRIPT} ${TR_WEB_DIR} ${TR_WEB_ASSETS_HEADER}
    DEPENDS 
        ${EMBED_SCRIPT}
        ${TR_WEB_DIR}/index.html
        ${TR_WEB_DIR}/style.css
        ${TR_WEB_DIR}/app.js
    COMMENT "Generating embedded web assets for tr-web"
)

# Plugin library - web assets are embedded automatically
add_library(tr_web_plugin
MODULE
  tr_web_plugin.cc
  ${TR_WEB_ASSETS_HEADER}
)

include_directories(
  ${CMAKE_BINARY_DIR}/../
  ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(tr_web_plugin 
  trunk_recorder_library 
  pthread
  ssl
  crypto
  z
  ${Boost_LIBRARIES} 
  ${GNURADIO_PMT_LIBRARIES} 
  ${GNURADIO_RUNTIME_LIBRARIES} 
  ${GNURADIO_FILTER_LIBRARIES} 
  ${GNURADIO_DIGITAL_LIBRARIES} 
  ${GNURADIO_ANALOG_LIBRARIES} 
  ${GNURADIO_AUDIO_LIBRARIES} 
  ${GNURADIO_UHD_LIBRARIES} 
  ${UHD_LIBRARIES} 
  ${GNURADIO_BLOCKS_LIBRARIES} 
  ${GNURADIO_OSMOSDR_LIBRARIES}  
  ${LIBOP25_REPEATER_LIBRARIES} 
  gnuradio-op25_repeater
)

if(NOT Gnuradio_VERSION VERSION_LESS "3.8")
    target_link_libraries(tr_web_plugin
    gnuradio::gnuradio-analog
    gnuradio::gnuradio-blocks
    gnuradio::gnuradio-digital
    gnuradio::gnuradio-filter
    gnuradio::gnuradio-pmt
    ) 
endif()

install(TARGETS tr_web_plugin LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/trunk-recorder)
