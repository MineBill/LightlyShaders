set(kwin_lightlyshaders_blur_config_SRCS blur_config.cpp)
ki18n_wrap_ui(kwin_lightlyshaders_blur_config_SRCS blur_config.ui)
kconfig_add_kcfg_files(kwin_lightlyshaders_blur_config_SRCS ../blurconfig.kcfgc)

qt_add_dbus_interface(kwin_lightlyshaders_blur_config_SRCS ${KWIN_EFFECTS_INTERFACE} kwineffects_interface)

#kwin_add_effect_config(kwin_lightlyshaders_blur_config ${kwin_lightlyshaders_blur_config_SRCS})
add_library(kwin_lightlyshaders_blur_config MODULE ${kwin_lightlyshaders_blur_config_SRCS})
target_link_libraries(kwin_lightlyshaders_blur_config
    KF6::KCMUtils
    KF6::CoreAddons
    KF6::I18n
    Qt6::DBus
)

install(TARGETS kwin_lightlyshaders_blur_config DESTINATION ${KDE_INSTALL_PLUGINDIR}/kwin/effects/configs)
