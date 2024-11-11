/*
    SPDX-FileCopyrightText: 2010 Fredrik Höglund <fredrik@kde.org>
    SPDX-FileCopyrightText: 2011 Philipp Knechtges <philipp-dev@knechtges.com>
    SPDX-FileCopyrightText: 2018 Alex Nemeth <alex.nemeth329@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "blur.h"
// KConfigSkeleton
#include "blurconfig.h"

#include "core/pixelgrid.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effecthandler.h"
#include "opengl/glplatform.h"
#include "scene/decorationitem.h"
// #include "scene/surfaceitem.h"
#include "scene/windowitem.h"
#include "wayland/blur.h"
// #include "wayland/display.h"
#include "wayland/surface.h"

#if KWIN_BUILD_X11
#    include "utils/xcbutils.h"
#endif

#include <QGuiApplication>
#include <QMatrix4x4>
#include <QScreen>
#include <QTime>
#include <QTimer>
#include <QWindow>
#include <cmath> // for ceil()
#include <cstdlib>

#include <KConfigGroup>
#include <KSharedConfig>

#include <KDecoration2/Decoration>

Q_LOGGING_CATEGORY(KWIN_BLUR, "kwin_effect_blur", QtWarningMsg)

static void ensureResources()
{
    // Must initialize resources manually because the effect is a static lib.
    Q_INIT_RESOURCE(blur);
}

namespace Lightly {

    static QByteArray const s_blurAtomName = QByteArrayLiteral("_KDE_NET_WM_BLUR_BEHIND_REGION");

    KWin::BlurManagerInterface* BlurEffect::s_blurManager = nullptr;
    QTimer* BlurEffect::s_blurManagerRemoveTimer = nullptr;

    BlurEffect::BlurEffect()
    {
        KWin::BlurConfig::instance(KWin::effects->config());
        ensureResources();

        m_helper = new LSHelper();

        m_downsamplePass.shader = KWin::ShaderManager::instance()->generateShaderFromFile(KWin::ShaderTrait::MapTexture, QStringLiteral(":/KWin::effects/blur/shaders/vertex.vert"), QStringLiteral(":/KWin::effects/blur/shaders/downsample.frag"));
        if (!m_downsamplePass.shader) {
            qCWarning(KWIN_BLUR) << "Failed to load downsampling pass shader";
            return;
        }
        m_downsamplePass.mvpMatrixLocation = m_downsamplePass.shader->uniformLocation("modelViewProjectionMatrix");
        m_downsamplePass.offsetLocation = m_downsamplePass.shader->uniformLocation("offset");
        m_downsamplePass.halfpixelLocation = m_downsamplePass.shader->uniformLocation("halfpixel");

        m_upsamplePass.shader = KWin::ShaderManager::instance()->generateShaderFromFile(KWin::ShaderTrait::MapTexture, QStringLiteral(":/KWin::effects/blur/shaders/vertex.vert"), QStringLiteral(":/KWin::effects/blur/shaders/upsample.frag"));
        if (!m_upsamplePass.shader) {
            qCWarning(KWIN_BLUR) << "Failed to load upsampling pass shader";
            return;
        }
        m_upsamplePass.mvpMatrixLocation = m_upsamplePass.shader->uniformLocation("modelViewProjectionMatrix");
        m_upsamplePass.offsetLocation = m_upsamplePass.shader->uniformLocation("offset");
        m_upsamplePass.halfpixelLocation = m_upsamplePass.shader->uniformLocation("halfpixel");

        m_noisePass.shader = KWin::ShaderManager::instance()->generateShaderFromFile(KWin::ShaderTrait::MapTexture, QStringLiteral(":/KWin::effects/blur/shaders/vertex.vert"), QStringLiteral(":/KWin::effects/blur/shaders/noise.frag"));
        if (!m_noisePass.shader) {
            qCWarning(KWIN_BLUR) << "Failed to load noise pass shader";
            return;
        }
        m_noisePass.mvpMatrixLocation = m_noisePass.shader->uniformLocation("modelViewProjectionMatrix");
        m_noisePass.noiseTextureSizeLocation = m_noisePass.shader->uniformLocation("noiseTextureSize");
        m_noisePass.texStartPosLocation = m_noisePass.shader->uniformLocation("texStartPos");

        initBlurStrengthValues();
        BlurEffect::reconfigure(ReconfigureAll);

#if KWIN_BUILD_X11
        if (KWin::effects->xcbConnection()) {
            net_wm_blur_region = KWin::effects->announceSupportProperty(s_blurAtomName, this);
        }
#endif

        if (KWin::effects->waylandDisplay()) {
            if (!s_blurManagerRemoveTimer) {
                s_blurManagerRemoveTimer = new QTimer(QCoreApplication::instance());
                s_blurManagerRemoveTimer->setSingleShot(true);
                s_blurManagerRemoveTimer->callOnTimeout([]() {
                    s_blurManager->remove();
                    s_blurManager = nullptr;
                });
            }
            s_blurManagerRemoveTimer->stop();
            if (!s_blurManager) {
                s_blurManager = new KWin::BlurManagerInterface(KWin::effects->waylandDisplay(), s_blurManagerRemoveTimer);
            }
        }

        connect(KWin::effects, &KWin::EffectsHandler::windowAdded, this, &BlurEffect::slotWindowAdded);
        connect(KWin::effects, &KWin::EffectsHandler::windowDeleted, this, &BlurEffect::slotWindowDeleted);
        connect(KWin::effects, &KWin::EffectsHandler::screenRemoved, this, &BlurEffect::slotScreenRemoved);
#if KWIN_BUILD_X11
        connect(KWin::effects, &KWin::EffectsHandler::propertyNotify, this, &BlurEffect::slotPropertyNotify);
        connect(KWin::effects, &KWin::EffectsHandler::xcbConnectionChanged, this, [this]() {
            net_wm_blur_region = KWin::effects->announceSupportProperty(s_blurAtomName, this);
        });
#endif

        // Fetch the blur regions for all windows
        auto const stackingOrder = KWin::effects->stackingOrder();
        for (KWin::EffectWindow* window : stackingOrder) {
            slotWindowAdded(window);
        }

        m_valid = true;
    }

    BlurEffect::~BlurEffect()
    {
        // When compositing is restarted, avoid removing the manager immediately.
        if (s_blurManager) {
            s_blurManagerRemoveTimer->start(1000);
        }
    }

    void BlurEffect::initBlurStrengthValues()
    {
        // This function creates an array of blur strength values that are evenly distributed

        // The range of the slider on the blur settings UI
        int numOfBlurSteps = 15;
        int remainingSteps = numOfBlurSteps;

        /*
         * Explanation for these numbers:
         *
         * The texture blur amount depends on the downsampling iterations and the offset value.
         * By changing the offset we can alter the blur amount without relying on further downsampling.
         * But there is a minimum and maximum value of offset per downsample iteration before we
         * get artifacts.
         *
         * The minOffset variable is the minimum offset value for an iteration before we
         * get blocky artifacts because of the downsampling.
         *
         * The maxOffset value is the maximum offset value for an iteration before we
         * get diagonal line artifacts because of the nature of the dual kawase blur algorithm.
         *
         * The expandSize value is the minimum value for an iteration before we reach the end
         * of a texture in the shader and sample outside of the area that was copied into the
         * texture from the screen.
         */

        // {minOffset, maxOffset, expandSize}
        blurOffsets.append({ 1.0, 2.0, 10 });  // Down sample size / 2
        blurOffsets.append({ 2.0, 3.0, 20 });  // Down sample size / 4
        blurOffsets.append({ 2.0, 5.0, 50 });  // Down sample size / 8
        blurOffsets.append({ 3.0, 8.0, 150 }); // Down sample size / 16
        // blurOffsets.append({5.0, 10.0, 400}); // Down sample size / 32
        // blurOffsets.append({7.0, ?.0});       // Down sample size / 64

        float offsetSum = 0;

        for (int i = 0; i < blurOffsets.size(); i++) {
            offsetSum += blurOffsets[i].maxOffset - blurOffsets[i].minOffset;
        }

        for (int i = 0; i < blurOffsets.size(); i++) {
            int iterationNumber = std::ceil((blurOffsets[i].maxOffset - blurOffsets[i].minOffset) / offsetSum * numOfBlurSteps);
            remainingSteps -= iterationNumber;

            if (remainingSteps < 0) {
                iterationNumber += remainingSteps;
            }

            float offsetDifference = blurOffsets[i].maxOffset - blurOffsets[i].minOffset;

            for (int j = 1; j <= iterationNumber; j++) {
                // {iteration, offset}
                blurStrengthValues.append({ i + 1, blurOffsets[i].minOffset + (offsetDifference / iterationNumber) * j });
            }
        }
    }

    void BlurEffect::reconfigure(ReconfigureFlags flags)
    {
        Q_UNUSED(flags)
        KWin::BlurConfig::self()->read();

        int blurStrength = KWin::BlurConfig::blurStrength() - 1;
        m_iterationCount = blurStrengthValues[blurStrength].iteration;
        m_offset = blurStrengthValues[blurStrength].offset;
        m_expandSize = blurOffsets[m_iterationCount - 1].expandSize;
        m_noiseStrength = KWin::BlurConfig::noiseStrength();

        // Update all windows for the blur to take effect
        KWin::effects->addRepaintFull();

        m_helper->reconfigure();
    }

    void BlurEffect::updateBlurRegion(KWin::EffectWindow* window)
    {
        std::optional<QRegion> content;
        std::optional<QRegion> frame;

#if KWIN_BUILD_X11
        if (net_wm_blur_region != XCB_ATOM_NONE) {
            QByteArray const value = window->readProperty(net_wm_blur_region, XCB_ATOM_CARDINAL, 32);
            QRegion region;
            if (value.size() > 0 && !(value.size() % (4 * sizeof(uint32_t)))) {
                uint32_t const* cardinals = reinterpret_cast<uint32_t const*>(value.constData());
                for (unsigned int i = 0; i < value.size() / sizeof(uint32_t);) {
                    int x = cardinals[i++];
                    int y = cardinals[i++];
                    int w = cardinals[i++];
                    int h = cardinals[i++];
                    region += KWin::Xcb::fromXNative(QRect(x, y, w, h)).toRect();
                }
            }
            if (!value.isNull()) {
                content = region;
            }
        }
#endif

        auto surface = window->surface();

        if (surface && surface->blur()) {
            content = surface->blur()->region();
        }

        if (auto internal = window->internalWindow()) {
            auto const property = internal->property("kwin_blur");
            if (property.isValid()) {
                content = property.value<QRegion>();
            }
        }

        if (window->decorationHasAlpha() && decorationSupportsBlurBehind(window)) {
            frame = decorationBlurRegion(window);
        }

        if (content.has_value() || frame.has_value()) {
            BlurEffectData& data = m_windows[window];
            data.content = content;
            data.frame = frame;
            data.windowEffect = KWin::ItemEffect(window->windowItem());
        } else {
            if (auto it = m_windows.find(window); it != m_windows.end()) {
                KWin::effects->makeOpenGLContextCurrent();
                m_windows.erase(it);
            }
        }
    }

    void BlurEffect::slotWindowAdded(KWin::EffectWindow* w)
    {
        if (auto surface = w->surface()) {
            windowBlurChangedConnections[w] = connect(surface, &KWin::SurfaceInterface::blurChanged, this, [this, w]() {
                if (w) {
                    updateBlurRegion(w);
                }
            });
        }
        if (auto internal = w->internalWindow()) {
            internal->installEventFilter(this);
        }

        connect(w, &KWin::EffectWindow::windowDecorationChanged, this, &BlurEffect::setupDecorationConnections);
        setupDecorationConnections(w);

        updateBlurRegion(w);

        // Check if window needs rounding corners
        m_helper->blurWindowAdded(w);
    }

    void BlurEffect::slotWindowDeleted(KWin::EffectWindow* w)
    {
        if (auto it = m_windows.find(w); it != m_windows.end()) {
            KWin::effects->makeOpenGLContextCurrent();
            m_windows.erase(it);
        }
        if (auto it = windowBlurChangedConnections.find(w); it != windowBlurChangedConnections.end()) {
            disconnect(*it);
            windowBlurChangedConnections.erase(it);
        }

        // remove window
        m_helper->blurWindowDeleted(w);
    }

    void BlurEffect::slotScreenRemoved(KWin::Output* screen)
    {
        for (auto& [window, data] : m_windows) {
            if (auto it = data.render.find(screen); it != data.render.end()) {
                KWin::effects->makeOpenGLContextCurrent();
                data.render.erase(it);
            }
        }
    }

#if KWIN_BUILD_X11
    void BlurEffect::slotPropertyNotify(KWin::EffectWindow* w, long atom)
    {
        if (w && atom == net_wm_blur_region && net_wm_blur_region != XCB_ATOM_NONE) {
            updateBlurRegion(w);
        }
    }
#endif

    void BlurEffect::setupDecorationConnections(KWin::EffectWindow* w)
    {
        if (!w->decoration()) {
            return;
        }

        connect(w->decoration(), &KDecoration2::Decoration::blurRegionChanged, this, [this, w]() {
            updateBlurRegion(w);
        });
    }

    bool BlurEffect::eventFilter(QObject* watched, QEvent* event)
    {
        auto internal = qobject_cast<QWindow*>(watched);
        if (internal && event->type() == QEvent::DynamicPropertyChange) {
            QDynamicPropertyChangeEvent* pe = static_cast<QDynamicPropertyChangeEvent*>(event);
            if (pe->propertyName() == "kwin_blur") {
                if (auto w = KWin::effects->findWindow(internal)) {
                    updateBlurRegion(w);
                }
            }
        }
        return false;
    }

    bool BlurEffect::enabledByDefault()
    {
        return false;
    }

    bool BlurEffect::supported()
    {
        return KWin::effects->openglContext() && (KWin::effects->openglContext()->supportsBlits() || KWin::effects->waylandDisplay());
    }

    bool BlurEffect::decorationSupportsBlurBehind(KWin::EffectWindow const* window) const
    {
        return window->decoration() && !window->decoration()->blurRegion().isNull();
    }

    QRegion BlurEffect::decorationBlurRegion(KWin::EffectWindow const* window) const
    {
        if (!decorationSupportsBlurBehind(window)) {
            return QRegion();
        }

        QRegion decorationRegion = QRegion(window->decoration()->rect()) - window->contentsRect().toRect();
        //! we return only blurred regions that belong to decoration region
        return decorationRegion.intersected(window->decoration()->blurRegion());
    }

    QRegion BlurEffect::blurRegion(KWin::EffectWindow* window) const
    {
        QRegion region;

        if (auto it = m_windows.find(window); it != m_windows.end()) {
            std::optional<QRegion> const& content = it->second.content;
            std::optional<QRegion> const& frame = it->second.frame;
            if (content.has_value()) {
                if (content->isEmpty()) {
                    // An empty region means that the blur effect should be enabled
                    // for the whole window.
                    region = window->contentsRect().toRect();
                } else {
                    region = content->translated(window->contentsRect().topLeft().toPoint()) & window->contentsRect().toRect();
                }
                if (frame.has_value()) {
                    region += frame.value();
                }
            } else if (frame.has_value()) {
                region = frame.value();
            }

            // Apply LighlyShaders to blur region
            m_helper->roundBlurRegion(window, &region);
        }

        return region;
    }

    void BlurEffect::prePaintScreen(KWin::ScreenPrePaintData& data, std::chrono::milliseconds presentTime)
    {
        m_paintedArea = QRegion();
        m_currentBlur = QRegion();
        m_currentScreen = KWin::effects->waylandDisplay() ? data.screen : nullptr;

        KWin::effects->prePaintScreen(data, presentTime);
    }

    void BlurEffect::prePaintWindow(KWin::EffectWindow* w, KWin::WindowPrePaintData& data, std::chrono::milliseconds presentTime)
    {
        // this effect relies on prePaintWindow being called in the bottom to top order

        KWin::effects->prePaintWindow(w, data, presentTime);

        QRegion const oldOpaque = data.opaque;
        if (data.opaque.intersects(m_currentBlur)) {
            // to blur an area partially we have to shrink the opaque area of a window
            QRegion newOpaque;
            for (QRect const& rect : data.opaque) {
                newOpaque += rect.adjusted(m_expandSize, m_expandSize, -m_expandSize, -m_expandSize);
            }
            data.opaque = newOpaque;

            // we don't have to blur a region we don't see
            m_currentBlur -= newOpaque;
        }

        // if we have to paint a non-opaque part of this window that intersects with the
        // currently blurred region we have to redraw the whole region
        if ((data.paint - oldOpaque).intersects(m_currentBlur)) {
            data.paint += m_currentBlur;
        }

        // in case this window has regions to be blurred
        QRegion const blurArea = blurRegion(w).boundingRect().translated(w->pos().toPoint());

        // if this window or a window underneath the blurred area is painted again we have to
        // blur everything
        if (m_paintedArea.intersects(blurArea) || data.paint.intersects(blurArea)) {
            data.paint += blurArea;
            // we have to check again whether we do not damage a blurred area
            // of a window
            if (blurArea.intersects(m_currentBlur)) {
                data.paint += m_currentBlur;
            }
        }

        m_currentBlur += blurArea;

        m_paintedArea -= data.opaque;
        m_paintedArea += data.paint;
    }

    bool BlurEffect::shouldBlur(KWin::EffectWindow const* window, int mask, KWin::WindowPaintData const& data) const
    {
        if (KWin::effects->activeFullScreenEffect() && !window->data(KWin::WindowForceBlurRole).toBool()) {
            return false;
        }

        if (window->isDesktop()) {
            return false;
        }

        bool scaled = !qFuzzyCompare(data.xScale(), 1.0) && !qFuzzyCompare(data.yScale(), 1.0);
        bool translated = data.xTranslation() || data.yTranslation();

        if ((scaled || (translated || (mask & PAINT_WINDOW_TRANSFORMED))) && !window->data(KWin::WindowForceBlurRole).toBool()) {
            return false;
        }

        return true;
    }

    void BlurEffect::drawWindow(KWin::RenderTarget const& renderTarget, KWin::RenderViewport const& viewport, KWin::EffectWindow* w, int mask, QRegion const& region, KWin::WindowPaintData& data)
    {
        blur(renderTarget, viewport, w, mask, region, data);

        // Draw the window over the blurred area
        KWin::effects->drawWindow(renderTarget, viewport, w, mask, region, data);
    }

    KWin::GLTexture* BlurEffect::ensureNoiseTexture()
    {
        if (m_noiseStrength == 0) {
            return nullptr;
        }

        qreal const scale = std::max(1.0, QGuiApplication::primaryScreen()->logicalDotsPerInch() / 96.0);
        if (!m_noisePass.noiseTexture || m_noisePass.noiseTextureScale != scale || m_noisePass.noiseTextureStength != m_noiseStrength) {
            // Init randomness based on time
            std::srand(static_cast<uint>(QTime::currentTime().msec()));

            QImage noiseImage(QSize(256, 256), QImage::Format_Grayscale8);

            for (int y = 0; y < noiseImage.height(); y++) {
                uint8_t* noiseImageLine = (uint8_t*)noiseImage.scanLine(y);

                for (int x = 0; x < noiseImage.width(); x++) {
                    noiseImageLine[x] = std::rand() % m_noiseStrength;
                }
            }

            noiseImage = noiseImage.scaled(noiseImage.size() * scale);

            m_noisePass.noiseTexture = KWin::GLTexture::upload(noiseImage);
            if (!m_noisePass.noiseTexture) {
                return nullptr;
            }
            m_noisePass.noiseTexture->setFilter(GL_NEAREST);
            m_noisePass.noiseTexture->setWrapMode(GL_REPEAT);
            m_noisePass.noiseTextureScale = scale;
            m_noisePass.noiseTextureStength = m_noiseStrength;
        }

        return m_noisePass.noiseTexture.get();
    }

    void BlurEffect::blur(KWin::RenderTarget const& renderTarget, KWin::RenderViewport const& viewport, KWin::EffectWindow* w, int mask, QRegion const& region, KWin::WindowPaintData& data)
    {
        auto it = m_windows.find(w);
        if (it == m_windows.end()) {
            return;
        }

        BlurEffectData& blurInfo = it->second;
        BlurRenderData& renderInfo = blurInfo.render[m_currentScreen];
        if (!shouldBlur(w, mask, data)) {
            return;
        }

        // Compute the effective blur shape. Note that if the window is transformed, so will be the blur shape.
        QRegion blurShape = blurRegion(w).translated(w->pos().toPoint());
        if (data.xScale() != 1 || data.yScale() != 1) {
            QPoint pt = blurShape.boundingRect().topLeft();
            QRegion scaledShape;
            for (QRect const& r : blurShape) {
                QPointF const topLeft(pt.x() + (r.x() - pt.x()) * data.xScale() + data.xTranslation(), pt.y() + (r.y() - pt.y()) * data.yScale() + data.yTranslation());
                QPoint const bottomRight(std::floor(topLeft.x() + r.width() * data.xScale()) - 1, std::floor(topLeft.y() + r.height() * data.yScale()) - 1);
                scaledShape += QRect(QPoint(std::floor(topLeft.x()), std::floor(topLeft.y())), bottomRight);
            }
            blurShape = scaledShape;
        } else if (data.xTranslation() || data.yTranslation()) {
            blurShape.translate(std::round(data.xTranslation()), std::round(data.yTranslation()));
        }

        QRect const backgroundRect = blurShape.boundingRect();
        QRect const deviceBackgroundRect = KWin::snapToPixelGrid(KWin::scaledRect(backgroundRect, viewport.scale()));
        auto const opacity = w->opacity() * data.opacity();

        // Get the effective shape that will be actually blurred. It's possible that all of it will be clipped.
        QList<QRectF> effectiveShape;
        effectiveShape.reserve(blurShape.rectCount());
        if (region != KWin::infiniteRegion()) {
            for (QRect const& clipRect : region) {
                QRectF const deviceClipRect = KWin::snapToPixelGridF(KWin::scaledRect(clipRect, viewport.scale()))
                                                  .translated(-deviceBackgroundRect.topLeft());
                for (QRect const& shapeRect : blurShape) {
                    QRectF const deviceShapeRect = KWin::snapToPixelGridF(KWin::scaledRect(shapeRect.translated(-backgroundRect.topLeft()), viewport.scale()));
                    if (QRectF const intersected = deviceClipRect.intersected(deviceShapeRect); !intersected.isEmpty()) {
                        effectiveShape.append(intersected);
                    }
                }
            }
        } else {
            for (QRect const& rect : blurShape) {
                effectiveShape.append(KWin::snapToPixelGridF(KWin::scaledRect(rect.translated(-backgroundRect.topLeft()), viewport.scale())));
            }
        }
        if (effectiveShape.isEmpty()) {
            return;
        }

        // Maybe reallocate offscreen render targets. Keep in mind that the first one contains
        // original background behind the window, it's not blurred.
        GLenum textureFormat = GL_RGBA8;
        if (renderTarget.texture()) {
            textureFormat = renderTarget.texture()->internalFormat();
        }

        if (renderInfo.framebuffers.size() != (m_iterationCount + 1) || renderInfo.textures[0]->size() != backgroundRect.size() || renderInfo.textures[0]->internalFormat() != textureFormat) {
            renderInfo.framebuffers.clear();
            renderInfo.textures.clear();

            for (size_t i = 0; i <= m_iterationCount; ++i) {
                auto texture = KWin::GLTexture::allocate(textureFormat, backgroundRect.size() / (1 << i));
                if (!texture) {
                    qCWarning(KWIN_BLUR) << "Failed to allocate an offscreen texture";
                    return;
                }
                texture->setFilter(GL_LINEAR);
                texture->setWrapMode(GL_CLAMP_TO_EDGE);

                auto framebuffer = std::make_unique<KWin::GLFramebuffer>(texture.get());
                if (!framebuffer->valid()) {
                    qCWarning(KWIN_BLUR) << "Failed to create an offscreen framebuffer";
                    return;
                }
                renderInfo.textures.push_back(std::move(texture));
                renderInfo.framebuffers.push_back(std::move(framebuffer));
            }
        }

        // Fetch the pixels behind the shape that is going to be blurred.
        QRegion const dirtyRegion = region & backgroundRect;
        for (QRect const& dirtyRect : dirtyRegion) {
            renderInfo.framebuffers[0]->blitFromRenderTarget(renderTarget, viewport, dirtyRect, dirtyRect.translated(-backgroundRect.topLeft()));
        }

        // Upload the geometry: the first 6 vertices are used when down sampling and sampling offscreen,
        // the remaining vertices are used when rendering on the screen.
        KWin::GLVertexBuffer* vbo = KWin::GLVertexBuffer::streamingBuffer();
        vbo->reset();
        vbo->setAttribLayout(std::span(KWin::GLVertexBuffer::GLVertex2DLayout), sizeof(KWin::GLVertex2D));

        int const vertexCount = effectiveShape.size() * 6;
        if (auto result = vbo->map<KWin::GLVertex2D>(6 + vertexCount)) {
            auto map = *result;

            size_t vboIndex = 0;

            // The geometry that will be blurred offscreen, in logical pixels.
            {
                QRectF const localRect = QRectF(0, 0, backgroundRect.width(), backgroundRect.height());

                float const x0 = localRect.left();
                float const y0 = localRect.top();
                float const x1 = localRect.right();
                float const y1 = localRect.bottom();

                float const u0 = x0 / backgroundRect.width();
                float const v0 = 1.0f - y0 / backgroundRect.height();
                float const u1 = x1 / backgroundRect.width();
                float const v1 = 1.0f - y1 / backgroundRect.height();

                // first triangle
                map[vboIndex++] = KWin::GLVertex2D {
                    .position = QVector2D(x0, y0),
                    .texcoord = QVector2D(u0, v0),
                };
                map[vboIndex++] = KWin::GLVertex2D {
                    .position = QVector2D(x1, y1),
                    .texcoord = QVector2D(u1, v1),
                };
                map[vboIndex++] = KWin::GLVertex2D {
                    .position = QVector2D(x0, y1),
                    .texcoord = QVector2D(u0, v1),
                };

                // second triangle
                map[vboIndex++] = KWin::GLVertex2D {
                    .position = QVector2D(x0, y0),
                    .texcoord = QVector2D(u0, v0),
                };
                map[vboIndex++] = KWin::GLVertex2D {
                    .position = QVector2D(x1, y0),
                    .texcoord = QVector2D(u1, v0),
                };
                map[vboIndex++] = KWin::GLVertex2D {
                    .position = QVector2D(x1, y1),
                    .texcoord = QVector2D(u1, v1),
                };
            }

            // The geometry that will be painted on screen, in device pixels.
            for (QRectF const& rect : effectiveShape) {
                float const x0 = rect.left();
                float const y0 = rect.top();
                float const x1 = rect.right();
                float const y1 = rect.bottom();

                float const u0 = x0 / deviceBackgroundRect.width();
                float const v0 = 1.0f - y0 / deviceBackgroundRect.height();
                float const u1 = x1 / deviceBackgroundRect.width();
                float const v1 = 1.0f - y1 / deviceBackgroundRect.height();

                // first triangle
                map[vboIndex++] = KWin::GLVertex2D {
                    .position = QVector2D(x0, y0),
                    .texcoord = QVector2D(u0, v0),
                };
                map[vboIndex++] = KWin::GLVertex2D {
                    .position = QVector2D(x1, y1),
                    .texcoord = QVector2D(u1, v1),
                };
                map[vboIndex++] = KWin::GLVertex2D {
                    .position = QVector2D(x0, y1),
                    .texcoord = QVector2D(u0, v1),
                };

                // second triangle
                map[vboIndex++] = KWin::GLVertex2D {
                    .position = QVector2D(x0, y0),
                    .texcoord = QVector2D(u0, v0),
                };
                map[vboIndex++] = KWin::GLVertex2D {
                    .position = QVector2D(x1, y0),
                    .texcoord = QVector2D(u1, v0),
                };
                map[vboIndex++] = KWin::GLVertex2D {
                    .position = QVector2D(x1, y1),
                    .texcoord = QVector2D(u1, v1),
                };
            }

            vbo->unmap();
        } else {
            qCWarning(KWIN_BLUR) << "Failed to map vertex buffer";
            return;
        }

        vbo->bindArrays();

        // The downsample pass of the dual Kawase algorithm: the background will be scaled down 50% every iteration.
        {
            KWin::ShaderManager::instance()->pushShader(m_downsamplePass.shader.get());

            QMatrix4x4 projectionMatrix;
            projectionMatrix.ortho(QRectF(0.0, 0.0, backgroundRect.width(), backgroundRect.height()));

            m_downsamplePass.shader->setUniform(m_downsamplePass.mvpMatrixLocation, projectionMatrix);
            m_downsamplePass.shader->setUniform(m_downsamplePass.offsetLocation, float(m_offset));

            for (size_t i = 1; i < renderInfo.framebuffers.size(); ++i) {
                auto const& read = renderInfo.framebuffers[i - 1];
                auto const& draw = renderInfo.framebuffers[i];

                QVector2D const halfpixel(0.5 / read->colorAttachment()->width(), 0.5 / read->colorAttachment()->height());
                m_downsamplePass.shader->setUniform(m_downsamplePass.halfpixelLocation, halfpixel);

                read->colorAttachment()->bind();

                KWin::GLFramebuffer::pushFramebuffer(draw.get());
                vbo->draw(GL_TRIANGLES, 0, 6);
            }

            KWin::ShaderManager::instance()->popShader();
        }

        // The upsample pass of the dual Kawase algorithm: the background will be scaled up 200% every iteration.
        {
            KWin::ShaderManager::instance()->pushShader(m_upsamplePass.shader.get());

            QMatrix4x4 projectionMatrix;
            projectionMatrix.ortho(QRectF(0.0, 0.0, backgroundRect.width(), backgroundRect.height()));

            m_upsamplePass.shader->setUniform(m_upsamplePass.mvpMatrixLocation, projectionMatrix);
            m_upsamplePass.shader->setUniform(m_upsamplePass.offsetLocation, float(m_offset));

            for (size_t i = renderInfo.framebuffers.size() - 1; i > 1; --i) {
                KWin::GLFramebuffer::popFramebuffer();
                auto const& read = renderInfo.framebuffers[i];

                QVector2D const halfpixel(0.5 / read->colorAttachment()->width(), 0.5 / read->colorAttachment()->height());
                m_upsamplePass.shader->setUniform(m_upsamplePass.halfpixelLocation, halfpixel);

                read->colorAttachment()->bind();

                vbo->draw(GL_TRIANGLES, 0, 6);
            }

            // The last upsampling pass is rendered on the screen, not in framebuffers[0].
            KWin::GLFramebuffer::popFramebuffer();
            auto const& read = renderInfo.framebuffers[1];

            projectionMatrix = viewport.projectionMatrix();
            projectionMatrix.translate(deviceBackgroundRect.x(), deviceBackgroundRect.y());
            m_upsamplePass.shader->setUniform(m_upsamplePass.mvpMatrixLocation, projectionMatrix);

            QVector2D const halfpixel(0.5 / read->colorAttachment()->width(), 0.5 / read->colorAttachment()->height());
            m_upsamplePass.shader->setUniform(m_upsamplePass.halfpixelLocation, halfpixel);

            read->colorAttachment()->bind();

            // Modulate the blurred texture with the window opacity if the window isn't opaque
            if (opacity < 1.0) {
                glEnable(GL_BLEND);
                float o = 1.0f - (opacity);
                o = 1.0f - o * o;
                glBlendColor(0, 0, 0, o);
                glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
            }

            vbo->draw(GL_TRIANGLES, 6, vertexCount);

            if (opacity < 1.0) {
                glDisable(GL_BLEND);
            }

            KWin::ShaderManager::instance()->popShader();
        }

        if (m_noiseStrength > 0) {
            // Apply an additive noise onto the blurred image. The noise is useful to mask banding
            // artifacts, which often happens due to the smooth color transitions in the blurred image.

            glEnable(GL_BLEND);
            if (opacity < 1.0) {
                glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE);
            } else {
                glBlendFunc(GL_ONE, GL_ONE);
            }

            if (KWin::GLTexture* noiseTexture = ensureNoiseTexture()) {
                KWin::ShaderManager::instance()->pushShader(m_noisePass.shader.get());

                QMatrix4x4 projectionMatrix = viewport.projectionMatrix();
                projectionMatrix.translate(deviceBackgroundRect.x(), deviceBackgroundRect.y());

                m_noisePass.shader->setUniform(m_noisePass.mvpMatrixLocation, projectionMatrix);
                m_noisePass.shader->setUniform(m_noisePass.noiseTextureSizeLocation, QVector2D(noiseTexture->width(), noiseTexture->height()));
                m_noisePass.shader->setUniform(m_noisePass.texStartPosLocation, QVector2D(deviceBackgroundRect.topLeft()));

                noiseTexture->bind();

                vbo->draw(GL_TRIANGLES, 6, vertexCount);

                KWin::ShaderManager::instance()->popShader();
            }

            glDisable(GL_BLEND);
        }

        vbo->unbindArrays();
    }

    bool BlurEffect::isActive() const
    {
        return m_valid && !KWin::effects->isScreenLocked();
    }

    bool BlurEffect::blocksDirectScanout() const
    {
        return false;
    }

} // namespace KWin

#include "moc_blur.cpp"
