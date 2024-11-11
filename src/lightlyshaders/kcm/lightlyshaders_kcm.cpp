#include "lightlyshaders_kcm.h"
#include "../lightlyshaders.h"
#include "kwineffects_interface.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>

#include <KPluginFactory>

namespace Lightly {
    K_PLUGIN_CLASS(LightlyShadersKCM)

    LightlyShadersKCM::LightlyShadersKCM(QObject* parent, KPluginMetaData const& data)
        : KCModule(parent, data)
    {
        ui.setupUi(KCModule::widget());
        addConfig(LightlyShadersConfig::self(), KCModule::widget());

        if (ui.kcfg_CornersType->currentIndex() == LSHelper::SquircledCorners) {
            ui.kcfg_SquircleRatio->setEnabled(true);
        } else {
            ui.kcfg_SquircleRatio->setEnabled(false);
        }

        connect(ui.kcfg_CornersType, SIGNAL(currentIndexChanged(int)), SLOT(updateChanged()));
    }

    void LightlyShadersKCM::load()
    {
        KCModule::load();
        LightlyShadersConfig::self()->load();
    }

    void LightlyShadersKCM::save()
    {
        LightlyShadersConfig::self()->save();
        KCModule::save();

        OrgKdeKwinEffectsInterface interface(QStringLiteral("org.kde.KWin"), QStringLiteral("/Effects"), QDBusConnection::sessionBus());
        interface.reconfigureEffect(QStringLiteral("kwin_effect_lightlyshaders"));
        interface.reconfigureEffect(QStringLiteral("lightlyshaders_blur"));
    }

    void LightlyShadersKCM::defaults()
    {
        KCModule::defaults();
        LightlyShadersConfig::self()->setDefaults();
    }

    void LightlyShadersKCM::updateChanged() const
    {
        if (ui.kcfg_CornersType->currentIndex() == LSHelper::SquircledCorners) {
            ui.kcfg_SquircleRatio->setEnabled(true);
        } else {
            ui.kcfg_SquircleRatio->setEnabled(false);
        }
    }
} // namespace KWin

#include "lightlyshaders_kcm.moc"
