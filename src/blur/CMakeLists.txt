add_subdirectory(kcm)

set(lightlyshaders_blur_SOURCES
    blur.cpp
    blur.qrc
    main.cpp
)

kconfig_add_kcfg_files(lightlyshaders_blur_SOURCES
    blurconfig.kcfgc
)

add_library(lightlyshaders_blur MODULE ${lightlyshaders_blur_SOURCES})
target_link_libraries(lightlyshaders_blur PRIVATE
    KWin::kwin
    KF6::ConfigGui
    KDecoration2::KDecoration
    xcb
    lshelper
)

install(TARGETS lightlyshaders_blur DESTINATION ${KDE_INSTALL_PLUGINDIR}/kwin/effects/plugins)