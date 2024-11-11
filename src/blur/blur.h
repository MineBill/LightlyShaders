/*
    SPDX-FileCopyrightText: 2010 Fredrik Höglund <fredrik@kde.org>
    SPDX-FileCopyrightText: 2018 Alex Nemeth <alex.nemeth329@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "effect/effect.h"
#include "opengl/glutils.h"
#include "scene/item.h"

#include <QList>

#include <unordered_map>

#include "lshelper.h"

namespace KWin {
    class BlurManagerInterface;
}

namespace Lightly {

    struct BlurRenderData {
        /// Temporary render targets needed for the Dual Kawase algorithm, the first texture
        /// contains not blurred background behind the window, it's cached.
        std::vector<std::unique_ptr<KWin::GLTexture>> textures;
        std::vector<std::unique_ptr<KWin::GLFramebuffer>> framebuffers;
    };

    struct BlurEffectData {
        /// The region that should be blurred behind the window
        std::optional<QRegion> content;

        /// The region that should be blurred behind the frame
        std::optional<QRegion> frame;

        /// The render data per screen. Screens can have different color spaces.
        std::unordered_map<KWin::Output*, BlurRenderData> render;

        KWin::ItemEffect windowEffect;
    };

    class BlurEffect : public KWin::Effect {
        Q_OBJECT

    public:
        BlurEffect();
        ~BlurEffect() override;

        static bool supported();
        static bool enabledByDefault();

        void reconfigure(ReconfigureFlags flags) override;
        void prePaintScreen(KWin::ScreenPrePaintData& data, std::chrono::milliseconds presentTime) override;
        void prePaintWindow(KWin::EffectWindow* w, KWin::WindowPrePaintData& data, std::chrono::milliseconds presentTime) override;
        void drawWindow(KWin::RenderTarget const& renderTarget, KWin::RenderViewport const& viewport, KWin::EffectWindow* w, int mask, QRegion const& region, KWin::WindowPaintData& data) override;

        bool provides(Feature feature) override;
        bool isActive() const override;

        int requestedEffectChainPosition() const override
        {
            return 20;
        }

        bool eventFilter(QObject* watched, QEvent* event) override;

        bool blocksDirectScanout() const override;

    public Q_SLOTS:
        void slotWindowAdded(KWin::EffectWindow* w);
        void slotWindowDeleted(KWin::EffectWindow* w);
        void slotScreenRemoved(KWin::Output* screen);
#if KWIN_BUILD_X11
        void slotPropertyNotify(KWin::EffectWindow* w, long atom);
#endif
        void setupDecorationConnections(KWin::EffectWindow* w);

    private:
        void initBlurStrengthValues();
        QRegion blurRegion(KWin::EffectWindow* window) const;
        QRegion decorationBlurRegion(KWin::EffectWindow const* window) const;
        bool decorationSupportsBlurBehind(KWin::EffectWindow const* window) const;
        bool shouldBlur(KWin::EffectWindow const* window, int mask, KWin::WindowPaintData const& data) const;
        void updateBlurRegion(KWin::EffectWindow* window);
        void blur(KWin::RenderTarget const& renderTarget, KWin::RenderViewport const& viewport, KWin::EffectWindow* w, int mask, QRegion const& region, KWin::WindowPaintData& data);
        KWin::GLTexture* ensureNoiseTexture();

    private:
        LSHelper* m_helper;

        struct
        {
            std::unique_ptr<KWin::GLShader> shader;
            int mvpMatrixLocation;
            int offsetLocation;
            int halfpixelLocation;
        } m_downsamplePass;

        struct
        {
            std::unique_ptr<KWin::GLShader> shader;
            int mvpMatrixLocation;
            int offsetLocation;
            int halfpixelLocation;
        } m_upsamplePass;

        struct
        {
            std::unique_ptr<KWin::GLShader> shader;
            int mvpMatrixLocation;
            int noiseTextureSizeLocation;
            int texStartPosLocation;

            std::unique_ptr<KWin::GLTexture> noiseTexture;
            qreal noiseTextureScale = 1.0;
            int noiseTextureStength = 0;
        } m_noisePass;

        bool m_valid = false;
#if KWIN_BUILD_X11
        long net_wm_blur_region = 0;
#endif
        QRegion m_paintedArea; // keeps track of all painted areas (from bottom to top)
        QRegion m_currentBlur; // keeps track of the currently blured area of the windows(from bottom to top)
        KWin::Output* m_currentScreen = nullptr;

        size_t m_iterationCount; // number of times the texture will be downsized to half size
        int m_offset;
        int m_expandSize;
        int m_noiseStrength;

        struct OffsetStruct {
            float minOffset;
            float maxOffset;
            int expandSize;
        };

        QList<OffsetStruct> blurOffsets;

        struct BlurValuesStruct {
            int iteration;
            float offset;
        };

        QList<BlurValuesStruct> blurStrengthValues;

        QMap<KWin::EffectWindow*, QMetaObject::Connection> windowBlurChangedConnections;
        std::unordered_map<KWin::EffectWindow*, BlurEffectData> m_windows;

        static KWin::BlurManagerInterface* s_blurManager;
        static QTimer* s_blurManagerRemoveTimer;
    };

    inline bool BlurEffect::provides(Feature feature)
    {
        if (feature == Blur) {
            return true;
        }
        return Effect::provides(feature);
    }

} // namespace Lightly
