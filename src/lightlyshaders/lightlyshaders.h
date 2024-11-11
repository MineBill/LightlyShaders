/*
 *   Copyright © 2015 Robert Metsäranta <therealestrob@gmail.com>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *   General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; see the file COPYING.  if not, write to
 *   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *   Boston, MA 02110-1301, USA.
 */

#ifndef LIGHTLYSHADERS_H
#define LIGHTLYSHADERS_H

#include <effect/effecthandler.h>
#include <effect/offscreeneffect.h>

#include "lshelper.h"

namespace Lightly {
    class GLTexture;

    class Q_DECL_EXPORT LightlyShadersEffect : public KWin::OffscreenEffect {
        Q_OBJECT

    public:
        LightlyShadersEffect();

        ~LightlyShadersEffect() override;

        static bool supported();

        static bool enabledByDefault();

        void setRoundness(int const r, KWin::Output* s);
        void reconfigure(ReconfigureFlags flags) override;
        void paintScreen(KWin::RenderTarget const& renderTarget, KWin::RenderViewport const& viewport, int mask, QRegion const& region, KWin::Output* s) override;
        void prePaintWindow(KWin::EffectWindow* w, KWin::WindowPrePaintData& data, std::chrono::milliseconds time) override;
        void drawWindow(KWin::RenderTarget const& renderTarget, KWin::RenderViewport const& viewport, KWin::EffectWindow* w, int mask, QRegion const& region, KWin::WindowPaintData& data) override;

        virtual int requestedEffectChainPosition() const override { return 99; }

    protected Q_SLOTS:
        void windowAdded(KWin::EffectWindow* window);
        void windowDeleted(KWin::EffectWindow* window);
        void windowMaximizedStateChanged(KWin::EffectWindow* window, bool horizontal, bool vertical);
        void windowFullScreenChanged(KWin::EffectWindow* window);

    private:
        enum {
            Top = 0,
            Bottom,
            NShad
        };

        struct LSWindowStruct {
            bool skipEffect;
            bool isManaged;
        };

        struct LSScreenStruct {
            bool configured = false;
            qreal scale = 1.0;
            float sizeScaled;
        };

        bool isValidWindow(KWin::EffectWindow* w);

        LSHelper* m_helper {};

        int m_size {};
        int m_innerOutlineWidth {};
        int m_outerOutlineWidth {};
        int m_roundness {};
        int m_shadowOffset {};
        int m_squircleRatio {};
        int m_cornersType {};
        bool m_innerOutline {}, m_outerOutline {}, m_darkTheme {}, m_disabledForMaximized {};
        QColor m_innerOutlineColor {}, m_outerOutlineColor {};
        std::unique_ptr<KWin::GLShader> m_shader {};
        QSize m_corner {};

        std::unordered_map<KWin::Output*, LSScreenStruct> m_screens {};
        QMap<KWin::EffectWindow*, LSWindowStruct> m_windows {};
    };
} // namespace KWin

#endif // LIGHTLYSHADERS_H
