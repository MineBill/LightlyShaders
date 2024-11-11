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

#include "lightlyshaders.h"
#include "lightlyshaders_config.h"
#include <KWindowEffects>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QTextStream>
#include <core/renderviewport.h>
#include <effect/effect.h>
#include <opengl/glutils.h>
#include <opengl/openglcontext.h>

Q_LOGGING_CATEGORY(LIGHTLYSHADERS, "kwin_effect_lightlyshaders", QtWarningMsg)

static void ensureResources()
{
    // Must initialize resources manually because the effect is a static lib.
    Q_INIT_RESOURCE(lightlyshaders);
}

QRectF scale(QRectF const& rect, qreal scaleFactor)
{
    return QRectF(
        rect.x() * scaleFactor,
        rect.y() * scaleFactor,
        rect.width() * scaleFactor,
        rect.height() * scaleFactor
    );
}

namespace Lightly {
    KWIN_EFFECT_FACTORY_SUPPORTED_ENABLED(LightlyShadersEffect, "lightlyshaders.json", return LightlyShadersEffect::supported();, return LightlyShadersEffect::enabledByDefault();)

    LightlyShadersEffect::LightlyShadersEffect()
        : OffscreenEffect()
    {
        ensureResources();

        m_helper = new LSHelper();
        LightlyShadersEffect::reconfigure(ReconfigureAll);

        m_shader = KWin::ShaderManager::instance()->generateShaderFromFile(
            KWin::ShaderTrait::MapTexture,
            QStringLiteral(""),
            QStringLiteral(":/effects/lightlyshaders/shaders/lightlyshaders.frag")
        );

        if (!m_shader) {
            qCWarning(LIGHTLYSHADERS) << "Failed to load shader";
            return;
        }

        if (m_shader->isValid()) {
            auto const stackingOrder = KWin::effects->stackingOrder();
            for (KWin::EffectWindow* window : stackingOrder) {
                windowAdded(window);
            }

            connect(KWin::effects, &KWin::EffectsHandler::windowAdded, this, &LightlyShadersEffect::windowAdded);
            connect(KWin::effects, &KWin::EffectsHandler::windowDeleted, this, &LightlyShadersEffect::windowDeleted);

            qCWarning(LIGHTLYSHADERS) << "LightlyShaders loaded.";
        } else
            qCWarning(LIGHTLYSHADERS) << "LightlyShaders: no valid shaders found! LightlyShaders will not work.";
    }

    LightlyShadersEffect::~LightlyShadersEffect()
    {
        m_windows.clear();
    }

    void LightlyShadersEffect::windowDeleted(KWin::EffectWindow* window)
    {
        m_windows.remove(window);
    }

    void LightlyShadersEffect::windowAdded(KWin::EffectWindow* window)
    {
        m_windows[window].isManaged = false;

        if (!m_helper->isManagedWindow(window))
            return;

        m_windows[window].isManaged = true;
        m_windows[window].skipEffect = false;

        connect(window, &KWin::EffectWindow::windowMaximizedStateChanged, this, &LightlyShadersEffect::windowMaximizedStateChanged);
        connect(window, &KWin::EffectWindow::windowFullScreenChanged, this, &LightlyShadersEffect::windowFullScreenChanged);

        QRectF maximized_area = KWin::effects->clientArea(KWin::MaximizeArea, window);
        if (maximized_area == window->frameGeometry() && m_disabledForMaximized)
            m_windows[window].skipEffect = true;

        redirect(window);
        setShader(window, m_shader.get());
    }

    void LightlyShadersEffect::windowFullScreenChanged(KWin::EffectWindow* window)
    {
        if (window->isFullScreen()) {
            m_windows[window].isManaged = false;
        } else {
            m_windows[window].isManaged = true;
        }
    }

    void LightlyShadersEffect::windowMaximizedStateChanged(KWin::EffectWindow* window, bool horizontal, bool vertical)
    {
        if (!m_disabledForMaximized)
            return;

        if ((horizontal == true) && (vertical == true)) {
            m_windows[window].skipEffect = true;
        } else {
            m_windows[window].skipEffect = false;
        }
    }

    void LightlyShadersEffect::setRoundness(int const r, KWin::Output* s)
    {
        m_size = r;
        m_screens[s].sizeScaled = static_cast<float>(r) * m_screens[s].scale;
        m_corner = QSize(m_size + (m_shadowOffset - 1), m_size + (m_shadowOffset - 1));
    }

    void LightlyShadersEffect::reconfigure(ReconfigureFlags flags)
    {
        Q_UNUSED(flags)

        LightlyShadersConfig::self()->load();

        m_innerOutlineWidth = LightlyShadersConfig::innerOutlineWidth();
        m_outerOutlineWidth = LightlyShadersConfig::outerOutlineWidth();
        m_innerOutline = LightlyShadersConfig::innerOutline();
        m_outerOutline = LightlyShadersConfig::outerOutline();
        m_innerOutlineColor = LightlyShadersConfig::innerOutlineColor();
        m_outerOutlineColor = LightlyShadersConfig::outerOutlineColor();
        m_disabledForMaximized = LightlyShadersConfig::disabledForMaximized();
        m_shadowOffset = LightlyShadersConfig::shadowOffset();
        m_squircleRatio = LightlyShadersConfig::squircleRatio();
        m_cornersType = LightlyShadersConfig::cornersType();

        m_helper->reconfigure();
        m_roundness = m_helper->roundness();

        if (m_shadowOffset >= m_roundness) {
            m_shadowOffset = m_roundness - 1;
        }

        if (!m_innerOutline) {
            m_innerOutlineWidth = 0.0;
        }
        if (!m_outerOutline) {
            m_outerOutlineWidth = 0.0;
        }

        auto const screens = KWin::effects->screens();
        for (KWin::Output* s : screens) {
            if (KWin::effects->waylandDisplay() == nullptr) {
                s = nullptr;
            }
            setRoundness(m_roundness, s);

            if (KWin::effects->waylandDisplay() == nullptr) {
                break;
            }
        }

        KWin::effects->addRepaintFull();
    }

    void LightlyShadersEffect::paintScreen(KWin::RenderTarget const& renderTarget, KWin::RenderViewport const& viewport, int mask, QRegion const& region, KWin::Output* s)
    {
        bool set_roundness = false;

        if (!m_screens[s].configured) {
            m_screens[s].configured = true;
            set_roundness = true;
        }

        qreal scale = viewport.scale();

        if (scale != m_screens[s].scale) {
            m_screens[s].scale = scale;
            set_roundness = true;
        }

        if (set_roundness) {
            setRoundness(m_roundness, s);
            m_helper->reconfigure();
        }

        KWin::effects->paintScreen(renderTarget, viewport, mask, region, s);
    }

    void LightlyShadersEffect::prePaintWindow(KWin::EffectWindow* w, KWin::WindowPrePaintData& data, std::chrono::milliseconds time)
    {
        if (!isValidWindow(w)) {
            KWin::effects->prePaintWindow(w, data, time);
            return;
        }

        KWin::Output* s = w->screen();
        if (KWin::effects->waylandDisplay() == nullptr) {
            s = nullptr;
        }

        QRectF const geo(w->frameGeometry());
        for (int corner = 0; corner < LSHelper::NTex; ++corner) {
            QRegion reg = QRegion(scale(m_helper->maskedRegions[corner]->boundingRect(), m_screens[s].scale).toRect());
            switch (corner) {
            case LSHelper::TopLeft:
                reg.translate(geo.x() - m_shadowOffset, geo.y() - m_shadowOffset);
                break;
            case LSHelper::TopRight:
                reg.translate(geo.x() + geo.width() - m_size, geo.y() - m_shadowOffset);
                break;
            case LSHelper::BottomRight:
                reg.translate(geo.x() + geo.width() - m_size - 1, geo.y() + geo.height() - m_size - 1);
                break;
            case LSHelper::BottomLeft:
                reg.translate(geo.x() - m_shadowOffset + 1, geo.y() + geo.height() - m_size - 1);
                break;
            default:
                break;
            }

            data.opaque -= reg;
        }

        KWin::effects->prePaintWindow(w, data, time);
    }

    bool LightlyShadersEffect::isValidWindow(KWin::EffectWindow* w)
    {
        if (!m_shader->isValid()
            || !m_windows[w].isManaged
            || m_windows[w].skipEffect) {
            return false;
        }
        return true;
    }

    void LightlyShadersEffect::drawWindow(KWin::RenderTarget const& renderTarget, KWin::RenderViewport const& viewport, KWin::EffectWindow* w, int mask, QRegion const& region, KWin::WindowPaintData& data)
    {
        QRectF const screen = viewport.renderRect().toRect();
        QRectF const logicalGeometry = w->expandedGeometry();
        bool hasInvalidSize = logicalGeometry.width() == 0 || logicalGeometry.height() == 0;

        if (hasInvalidSize || !isValidWindow(w) || (!screen.intersects(w->frameGeometry()) && !(mask & PAINT_WINDOW_TRANSFORMED))) {
            KWin::effects->drawWindow(renderTarget, viewport, w, mask, region, data);
            return;
        }

        KWin::Output* s = w->screen();
        if (KWin::effects->waylandDisplay() == nullptr) {
            s = nullptr;
        }

        QRectF const geo(w->frameGeometry());
        QRectF const exp_geo(w->expandedGeometry());

        QRectF const geo_scaled = scale(geo, m_screens[s].scale);
        QRectF const exp_geo_scaled = scale(exp_geo, m_screens[s].scale);

        // Draw rounded corners with shadows
        int const frameSizeLocation = m_shader->uniformLocation("frame_size");
        int const expandedSizeLocation = m_shader->uniformLocation("expanded_size");
        int const shadowSizeLocation = m_shader->uniformLocation("shadow_size");
        int const radiusLocation = m_shader->uniformLocation("radius");
        int const shadowOffsetLocation = m_shader->uniformLocation("shadow_sample_offset");
        int const innerOutlineColorLocation = m_shader->uniformLocation("inner_outline_color");
        int const outerOutlineColorLocation = m_shader->uniformLocation("outer_outline_color");
        int const innerOutlineWidthLocation = m_shader->uniformLocation("inner_outline_width");
        int const outerOutlineWidthLocation = m_shader->uniformLocation("outer_outline_width");
        int const drawInnerOutlineLocation = m_shader->uniformLocation("draw_inner_outline");
        int const drawOuterOutlineLocation = m_shader->uniformLocation("draw_outer_outline");
        int const squircleRatioLocation = m_shader->uniformLocation("squircle_ratio");
        int const isSquircleLocation = m_shader->uniformLocation("is_squircle");
        KWin::ShaderManager* sm = KWin::ShaderManager::instance();
        sm->pushShader(m_shader.get());

        m_shader->setUniform(frameSizeLocation, QVector2D(geo_scaled.width(), geo_scaled.height()));
        m_shader->setUniform(expandedSizeLocation, QVector2D(exp_geo_scaled.width(), exp_geo_scaled.height()));
        m_shader->setUniform(shadowSizeLocation, QVector3D(geo_scaled.x() - exp_geo_scaled.x(), geo_scaled.y() - exp_geo_scaled.y(), exp_geo_scaled.height() - geo_scaled.height() - geo_scaled.y() + exp_geo_scaled.y()));
        m_shader->setUniform(radiusLocation, m_screens[s].sizeScaled);
        m_shader->setUniform(shadowOffsetLocation, static_cast<float>(m_shadowOffset * m_screens[s].scale));
        m_shader->setUniform(innerOutlineColorLocation, QVector4D(m_innerOutlineColor.red() / 255.0, m_innerOutlineColor.green() / 255.0, m_innerOutlineColor.blue() / 255.0, m_innerOutlineColor.alpha() / 255.0));
        m_shader->setUniform(outerOutlineColorLocation, QVector4D(m_outerOutlineColor.red() / 255.0, m_outerOutlineColor.green() / 255.0, m_outerOutlineColor.blue() / 255.0, m_outerOutlineColor.alpha() / 255.0));
        m_shader->setUniform(innerOutlineWidthLocation, static_cast<float>(m_innerOutlineWidth * m_screens[s].scale));
        m_shader->setUniform(outerOutlineWidthLocation, static_cast<float>(m_outerOutlineWidth * m_screens[s].scale));
        m_shader->setUniform(drawInnerOutlineLocation, m_innerOutline);
        m_shader->setUniform(drawOuterOutlineLocation, m_outerOutline);
        m_shader->setUniform(squircleRatioLocation, m_squircleRatio);
        m_shader->setUniform(isSquircleLocation, (m_cornersType == LSHelper::SquircledCorners));

        glActiveTexture(GL_TEXTURE0);

        OffscreenEffect::drawWindow(renderTarget, viewport, w, mask, region, data);

        sm->popShader();
    }

    bool LightlyShadersEffect::enabledByDefault()
    {
        return supported();
    }

    bool LightlyShadersEffect::supported()
    {
        return KWin::effects->openglContext() && KWin::effects->openglContext()->checkSupported() && KWin::effects->openglContext()->supportsBlits();
    }
} // namespace KWin

#include "lightlyshaders.moc"
