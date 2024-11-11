#pragma once

#include "lightlyshaders_config.h"
#include "ui_lightlyshaders_config.h"
#include <kcmodule.h>

namespace KWin {
    class LightlyShadersKCM : public KCModule {
        Q_OBJECT

    public:
        explicit LightlyShadersKCM(QObject* parent, KPluginMetaData const& data);

    public Q_SLOTS:
        void save() override;
        void load() override;
        void defaults() override;

        void updateChanged();

    private:
        void setChanged(bool value);

        Ui::LightlyShadersKCM ui;
    };
} // namespace KWin
