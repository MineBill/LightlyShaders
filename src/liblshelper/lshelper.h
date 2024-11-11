#include "liblshelper_export.h"

#include <QImage>
#include <QPainterPath>
#include <QRegion>
#include <effect/effecthandler.h>

template<typename T>
int signum(T val)
{
    return (T(0) < val) - (val < T(0));
}

namespace Lightly {
    class LIBLSHELPER_EXPORT LSHelper : public QObject {
        Q_OBJECT

    public:
        LSHelper();

        ~LSHelper() override;

        void reconfigure();

        QPainterPath superellipse(float size, int n, int translate);
        QImage genMaskImg(int size, bool mask, bool outer_rect);

        void roundBlurRegion(KWin::EffectWindow* w, QRegion* region);
        bool isManagedWindow(KWin::EffectWindow const* w);
        void blurWindowAdded(KWin::EffectWindow* w);
        void blurWindowDeleted(KWin::EffectWindow* w);

        int roundness() const;

        enum {
            RoundedCorners = 0,
            SquircledCorners
        };

        enum {
            TopLeft = 0,
            TopRight,
            BottomRight,
            BottomLeft,
            NTex
        };

        QRegion* maskedRegions[NTex];

    private:
        bool hasShadow(KWin::EffectWindow const* w);

        void setMaskRegions();

        QRegion* createMaskRegion(QImage img, int size, int corner);

        int m_size {}, m_cornersType {}, m_squircleRatio {}, m_shadowOffset {};
        bool m_disabledForMaximized {};
        QList<KWin::EffectWindow*> m_managed {};
    };
} // namespace
