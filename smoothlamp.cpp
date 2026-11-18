#include "smoothlamp.h"

#include "effect/effecthandler.h"
#include "effect/effectwindow.h"

#include <dlfcn.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLoggingCategory>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

Q_LOGGING_CATEGORY(KWIN_SMOOTHLAMP, "kwin_effect_smoothlamp", QtWarningMsg)

namespace {

inline double clamp01(double v)
{
    return std::clamp(v, 0.0, 1.0);
}

inline double lerp(double a, double b, double t)
{
    return a + (b - a) * t;
}

} // namespace

SmoothLampEffect::LoadedAlgo::~LoadedAlgo()
{
    if (ctx && destroyFn) {
        destroyFn(ctx);
        ctx = nullptr;
    }
    if (dlHandle) {
        dlclose(dlHandle);
        dlHandle = nullptr;
    }
    if (!tempSoPath.isEmpty()) {
        QFile::remove(tempSoPath);
    }
}

SmoothLampEffect::SmoothLampEffect()
{
    setVertexSnappingMode(KWin::RenderGeometry::VertexSnappingMode::None);

    reconfigure(ReconfigureAll);

    connect(KWin::effects, &KWin::EffectsHandler::windowAdded,
            this, &SmoothLampEffect::slotWindowAdded);
    connect(KWin::effects, &KWin::EffectsHandler::windowDeleted,
            this, &SmoothLampEffect::slotWindowDeleted);

    const auto existingWindows = KWin::effects->stackingOrder();
    for (KWin::EffectWindow *w : existingWindows) {
        slotWindowAdded(w);
    }
}

SmoothLampEffect::~SmoothLampEffect()
{
    const auto activeWindows = m_animations.keys();
    for (KWin::EffectWindow *w : activeWindows) {
        unredirect(w);
    }
    m_animations.clear();
    m_algo.reset();
}

bool SmoothLampEffect::supported()
{
    return KWin::OffscreenEffect::supported();
}

std::shared_ptr<SmoothLampEffect::LoadedAlgo> SmoothLampEffect::loadAlgorithmLibrary(const QString &configPath)
{
    QStringList candidates = {
        QDir::homePath() + QStringLiteral("/.local/lib/smoothlamp_algo.so"),
#ifdef SMOOTHLAMP_SYS_SO_PATH
        QStringLiteral(SMOOTHLAMP_SYS_SO_PATH),
#endif
        QStringLiteral("/usr/lib/x86_64-linux-gnu/smoothlamp_algo.so"),
        QStringLiteral("/usr/lib/smoothlamp_algo.so"),
        QStringLiteral("/usr/local/lib/smoothlamp_algo.so"),
    };

    QString srcSo;
    for (const QString &path : candidates) {
        if (QFile::exists(path)) {
            srcSo = path;
            break;
        }
    }
    if (srcSo.isEmpty()) {
        qCWarning(KWIN_SMOOTHLAMP) << "Could not find smoothlamp_algo.so in:" << candidates;
        return nullptr;
    }

    QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtimeDir.isEmpty()) {
        runtimeDir = QDir::tempPath();
    }
    QDir().mkpath(runtimeDir);

    const QString tempPath = QStringLiteral("%1/smoothlamp_algo_%2_%3.so")
                                 .arg(runtimeDir)
                                 .arg(QCoreApplication::applicationPid())
                                 .arg(++m_loadSeq);

    QFile::remove(tempPath);
    if (!QFile::copy(srcSo, tempPath)) {
        qCWarning(KWIN_SMOOTHLAMP) << "Failed to copy algorithm library from" << srcSo << "to" << tempPath;
        return nullptr;
    }

    void *handle = dlopen(QFile::encodeName(tempPath).constData(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        qCWarning(KWIN_SMOOTHLAMP) << "dlopen failed:" << dlerror();
        QFile::remove(tempPath);
        return nullptr;
    }

    auto abiFn = reinterpret_cast<SmoothLampAbiVersionFn>(dlsym(handle, "smoothlamp_algo_abi_version"));
    auto createFn = reinterpret_cast<SmoothLampCreateFn>(dlsym(handle, "smoothlamp_algo_create"));
    auto destroyFn = reinterpret_cast<SmoothLampDestroyFn>(dlsym(handle, "smoothlamp_algo_destroy"));
    auto getParamsFn = reinterpret_cast<SmoothLampGetParamsFn>(dlsym(handle, "smoothlamp_algo_get_params"));
    auto calcDurationFn = reinterpret_cast<SmoothLampCalcDurationFn>(dlsym(handle, "smoothlamp_algo_calc_duration"));
    auto remapProgressFn = reinterpret_cast<SmoothLampRemapProgressFn>(dlsym(handle, "smoothlamp_algo_remap_progress"));
    auto prepareFrameFn = reinterpret_cast<SmoothLampPrepareFrameFn>(dlsym(handle, "smoothlamp_algo_prepare_frame"));
    auto deformFn = reinterpret_cast<SmoothLampDeformFn>(dlsym(handle, "smoothlamp_algo_deform"));

    const int abi = abiFn ? abiFn() : -1;
    if (abi != SMOOTHLAMP_ALGO_ABI_VERSION
        || !createFn || !destroyFn || !getParamsFn
        || !calcDurationFn || !remapProgressFn || !prepareFrameFn || !deformFn) {
        qCWarning(KWIN_SMOOTHLAMP) << "Invalid ABI or missing symbols in" << srcSo
                                   << "got ABI" << abi << "expected" << SMOOTHLAMP_ALGO_ABI_VERSION;
        dlclose(handle);
        QFile::remove(tempPath);
        return nullptr;
    }

    auto algo = std::make_shared<LoadedAlgo>();
    algo->dlHandle = handle;
    algo->destroyFn = destroyFn;
    algo->calcDurationFn = calcDurationFn;
    algo->remapProgressFn = remapProgressFn;
    algo->prepareFrameFn = prepareFrameFn;
    algo->deformFn = deformFn;
    algo->tempSoPath = tempPath;
    algo->ctx = createFn(QFile::encodeName(configPath).constData());

    algo->params = SmoothLampHostParams{55.0, 0, 5.0, 80, 128, 24, 32};
    getParamsFn(algo->ctx, &algo->params);

    algo->params.turnaround_ms = std::clamp(algo->params.turnaround_ms, 1.0, 300.0);
    algo->params.grid_step_px = std::clamp(algo->params.grid_step_px, 2.0, 32.0);
    algo->params.min_grid_steps = std::clamp(algo->params.min_grid_steps, 16, 240);
    algo->params.max_grid_steps = std::clamp(algo->params.max_grid_steps, algo->params.min_grid_steps, 320);
    algo->params.cross_steps_min = std::clamp(algo->params.cross_steps_min, 8, 96);
    algo->params.cross_steps_max = std::clamp(algo->params.cross_steps_max, algo->params.cross_steps_min, 128);

    return algo;
}

void SmoothLampEffect::reconfigure(ReconfigureFlags)
{
    const QString configPath =
        QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + QStringLiteral("/smoothlamprc");

    if (auto fresh = loadAlgorithmLibrary(configPath)) {
        m_algo = fresh;
    }
}

void SmoothLampEffect::slotWindowAdded(KWin::EffectWindow *w)
{
    if (!w) {
        return;
    }
    connect(w, &KWin::EffectWindow::minimizedChanged,
            this, &SmoothLampEffect::slotWindowMinimizedChanged,
            Qt::UniqueConnection);
}

void SmoothLampEffect::slotWindowMinimizedChanged(KWin::EffectWindow *w)
{
    if (!w) {
        return;
    }
    if (w->isMinimized()) {
        handleWindowMinimized(w);
    } else {
        handleWindowUnminimized(w);
    }
}

SmoothLampEffect::PanelEdge SmoothLampEffect::detectPanelEdge(KWin::EffectWindow *w, const QRectF &iconGeo) const
{
    const QPoint iconCenter = iconGeo.center().toPoint();
    const KWin::LogicalOutput *iconScreen = KWin::effects->screenAt(iconCenter);
    const auto desktops = w->desktops();
    const KWin::VirtualDesktop *desktop = desktops.isEmpty() ? nullptr : desktops.constFirst();

    const QRectF screen = iconScreen
        ? KWin::effects->clientArea(KWin::ScreenArea, iconScreen, desktop)
        : KWin::effects->clientArea(KWin::ScreenArea, w);

    const QPointF ic = iconGeo.center();
    const double dBottom = std::abs(screen.bottom() - ic.y());
    const double dTop = std::abs(ic.y() - screen.top());
    const double dLeft = std::abs(ic.x() - screen.left());
    const double dRight = std::abs(screen.right() - ic.x());

    const double minD = std::min({dBottom, dTop, dLeft, dRight});
    if (minD == dTop) {
        return PanelEdge::Top;
    }
    if (minD == dLeft) {
        return PanelEdge::Left;
    }
    if (minD == dRight) {
        return PanelEdge::Right;
    }
    return PanelEdge::Bottom;
}

double SmoothLampEffect::windowDurationMs(const std::shared_ptr<LoadedAlgo> &algo,
                                          KWin::EffectWindow *w,
                                          const QRectF &iconGeo,
                                          PanelEdge edge,
                                          bool minimizing) const
{
    if (!algo || !w) {
        return 380.0;
    }

    const QRectF frameGeo = w->frameGeometry();
    const QRectF screenGeo = KWin::effects->clientArea(KWin::ScreenArea, w);
    const SmoothLampFrameInfo info{
        minimizing ? 0.0 : 1.0,
        minimizing ? 1 : 0,
        int(edge),
        frameGeo.x(), frameGeo.y(), frameGeo.width(), frameGeo.height(),
        iconGeo.x(), iconGeo.y(), iconGeo.width(), iconGeo.height(),
        screenGeo.x(), screenGeo.y(), screenGeo.width(), screenGeo.height()
    };

    const double rawMs = std::clamp(algo->calcDurationFn(algo->ctx, &info), 30.0, 5000.0);
    if (algo->params.ignore_global_speed) {
        return rawMs;
    }
    return std::max(1.0, animationTime(std::chrono::milliseconds(std::max(1, int(std::lround(rawMs))))));
}

void SmoothLampEffect::handleWindowMinimized(KWin::EffectWindow *w)
{
    if (!m_algo || KWin::effects->hasActiveFullScreenEffect()) {
        return;
    }

    const QRectF iconGeo = w->iconGeometry();
    if (!iconGeo.isValid() || iconGeo.width() <= 0.0 || iconGeo.height() <= 0.0) {
        return;
    }

    const PanelEdge edge = detectPanelEdge(w, iconGeo);

    auto it = m_animations.find(w);
    if (it != m_animations.end()) {
        if (!it->minimizing && it->algo) {
            it->progress = clamp01(it->algo->remapProgressFn(it->algo->ctx, it->progress, 0, 1));
        }
        it->algo = m_algo;
        it->minimizing = true;
        it->iconGeometry = iconGeo;
        it->edge = edge;
        it->durationMs = windowDurationMs(it->algo, w, iconGeo, edge, true);
    } else {
        AnimationState state;
        state.progress = 0.0;
        state.minimizing = true;
        state.iconGeometry = iconGeo;
        state.edge = edge;
        state.algo = m_algo;
        state.durationMs = windowDurationMs(state.algo, w, iconGeo, edge, true);
        state.velocity = 1.0 / state.durationMs;
        state.visibleRef = std::make_shared<KWin::EffectWindowVisibleRef>(
            w, KWin::EffectWindow::PAINT_DISABLED_BY_MINIMIZE);
        m_animations.insert(w, state);
        redirect(w);
    }

    KWin::effects->addRepaintFull();
}

void SmoothLampEffect::handleWindowUnminimized(KWin::EffectWindow *w)
{
    if (!m_algo || KWin::effects->hasActiveFullScreenEffect()) {
        return;
    }

    const QRectF iconGeo = w->iconGeometry();
    if (!iconGeo.isValid() || iconGeo.width() <= 0.0 || iconGeo.height() <= 0.0) {
        return;
    }

    const PanelEdge edge = detectPanelEdge(w, iconGeo);

    auto it = m_animations.find(w);
    if (it != m_animations.end()) {
        if (it->minimizing && it->algo) {
            it->progress = clamp01(it->algo->remapProgressFn(it->algo->ctx, it->progress, 1, 0));
        }
        it->algo = m_algo;
        it->minimizing = false;
        it->iconGeometry = iconGeo;
        it->edge = edge;
        it->durationMs = windowDurationMs(it->algo, w, iconGeo, edge, false);
    } else {
        AnimationState state;
        state.progress = 1.0;
        state.minimizing = false;
        state.iconGeometry = iconGeo;
        state.edge = edge;
        state.algo = m_algo;
        state.durationMs = windowDurationMs(state.algo, w, iconGeo, edge, false);
        state.velocity = -1.0 / state.durationMs;
        state.visibleRef = std::make_shared<KWin::EffectWindowVisibleRef>(
            w, KWin::EffectWindow::PAINT_DISABLED_BY_MINIMIZE);
        m_animations.insert(w, state);
        redirect(w);
    }

    KWin::effects->addRepaintFull();
}

void SmoothLampEffect::slotWindowDeleted(KWin::EffectWindow *w)
{
    if (m_animations.remove(w) > 0) {
        unredirect(w);
    }
}

void SmoothLampEffect::prePaintScreen(KWin::ScreenPrePaintData &data, std::chrono::milliseconds presentTime)
{
    double deltaMs = 0.0;
    if (m_lastPresentTime != std::chrono::milliseconds::zero()) {
        deltaMs = std::clamp(double((presentTime - m_lastPresentTime).count()), 0.0, 50.0);
    }
    if (presentTime > m_lastPresentTime) {
        m_lastPresentTime = presentTime;
    }

    for (auto it = m_animations.begin(); it != m_animations.end(); ++it) {
        const double turnaround = it->algo ? it->algo->params.turnaround_ms : 55.0;
        const double blend = clamp01(deltaMs / turnaround);
        const double targetVel = it->minimizing
            ? (1.0 / it->durationMs)
            : (-1.0 / it->durationMs);

        it->velocity = lerp(it->velocity, targetVel, blend);
        it->progress += it->velocity * deltaMs;

        if (it->progress >= 1.0) {
            it->progress = 1.0;
            if (it->velocity > 0.0) {
                it->velocity = 0.0;
            }
        } else if (it->progress <= 0.0) {
            it->progress = 0.0;
            if (it->velocity < 0.0) {
                it->velocity = 0.0;
            }
        }
    }

    if (!m_animations.isEmpty()) {
        data.mask |= PAINT_SCREEN_WITH_TRANSFORMED_WINDOWS;
    }

    KWin::effects->prePaintScreen(data, presentTime);
}

void SmoothLampEffect::prePaintWindow(KWin::RenderView *view,
                                      KWin::EffectWindow *w,
                                      KWin::WindowPrePaintData &data,
                                      std::chrono::milliseconds presentTime)
{
    if (m_animations.contains(w)) {
        data.setTransformed();
    }

    KWin::effects->prePaintWindow(view, w, data, presentTime);
}

void SmoothLampEffect::postPaintScreen()
{
    KWin::effects->postPaintScreen();

    if (m_animations.isEmpty()) {
        m_lastPresentTime = std::chrono::milliseconds::zero();
        return;
    }

    bool removedAny = false;
    for (auto it = m_animations.begin(); it != m_animations.end();) {
        const bool finished = (it->minimizing && it->progress >= 1.0)
                           || (!it->minimizing && it->progress <= 0.0);
        if (finished) {
            KWin::EffectWindow *w = it.key();
            it = m_animations.erase(it);
            unredirect(w);
            removedAny = true;
        } else {
            ++it;
        }
    }

    if (m_animations.isEmpty()) {
        m_lastPresentTime = std::chrono::milliseconds::zero();
    }

    if (!m_animations.isEmpty() || removedAny) {
        KWin::effects->addRepaintFull();
    }
}

bool SmoothLampEffect::isActive() const
{
    return !m_animations.isEmpty();
}

void SmoothLampEffect::apply(KWin::EffectWindow *window, int mask, KWin::WindowPaintData &data, KWin::WindowQuadList &quads)
{
    Q_UNUSED(mask)

    auto it = m_animations.constFind(window);
    if (it == m_animations.constEnd() || quads.isEmpty() || !it->algo) {
        return;
    }

    const AnimationState &state = it.value();
    if (state.progress <= 0.0) {
        return;
    }

    double qMinX = quads[0][0].x();
    double qMinY = quads[0][0].y();
    for (const KWin::WindowQuad &q : quads) {
        for (int i = 0; i < 4; ++i) {
            qMinX = std::min(qMinX, q[i].x());
            qMinY = std::min(qMinY, q[i].y());
        }
    }

    const QRectF expandedGeo = window->expandedGeometry();
    const QRectF frameGeo = window->frameGeometry();
    const QRectF iconGeo = state.iconGeometry;
    const PanelEdge edge = state.edge;

    if (frameGeo.width() <= 1.0 || frameGeo.height() <= 1.0) {
        return;
    }

    const SmoothLampHostParams &hp = state.algo->params;
    const bool vertical = (edge == PanelEdge::Bottom || edge == PanelEdge::Top);
    const double travelSpan = vertical
        ? std::max(frameGeo.height(), std::abs(iconGeo.center().y() - frameGeo.center().y()) + frameGeo.height() * 0.5)
        : std::max(frameGeo.width(), std::abs(iconGeo.center().x() - frameGeo.center().x()) + frameGeo.width() * 0.5);

    const int mainSteps = std::clamp(int(std::ceil(travelSpan / hp.grid_step_px)), hp.min_grid_steps, hp.max_grid_steps);
    const double crossDim = vertical ? frameGeo.width() : frameGeo.height();
    const int crossSteps = std::clamp(int(std::ceil(crossDim / 20.0)), hp.cross_steps_min, hp.cross_steps_max);

    quads = vertical
        ? quads.makeRegularGrid(crossSteps, mainSteps)
        : quads.makeRegularGrid(mainSteps, crossSteps);

    double winFar = 0.0, winNear = 0.0, winU0 = 0.0, winU1 = 0.0;
    switch (edge) {
    case PanelEdge::Bottom:
        winFar = frameGeo.top(); winNear = frameGeo.bottom();
        winU0 = frameGeo.left(); winU1 = frameGeo.right();
        break;
    case PanelEdge::Top:
        winFar = frameGeo.bottom(); winNear = frameGeo.top();
        winU0 = frameGeo.left(); winU1 = frameGeo.right();
        break;
    case PanelEdge::Right:
        winFar = frameGeo.left(); winNear = frameGeo.right();
        winU0 = frameGeo.top(); winU1 = frameGeo.bottom();
        break;
    case PanelEdge::Left:
        winFar = frameGeo.right(); winNear = frameGeo.left();
        winU0 = frameGeo.top(); winU1 = frameGeo.bottom();
        break;
    }

    const double winMainSpan = winNear - winFar;
    const double winCrossSpan = winU1 - winU0;
    const double quadOffsetX = qMinX - expandedGeo.left();
    const double quadOffsetY = qMinY - expandedGeo.top();

    const QRectF screenGeo = KWin::effects->clientArea(KWin::ScreenArea, window);
    const SmoothLampFrameInfo info{
        clamp01(state.progress),
        state.minimizing ? 1 : 0,
        int(edge),
        frameGeo.x(), frameGeo.y(), frameGeo.width(), frameGeo.height(),
        iconGeo.x(), iconGeo.y(), iconGeo.width(), iconGeo.height(),
        screenGeo.x(), screenGeo.y(), screenGeo.width(), screenGeo.height()
    };

    SmoothLampFrameResult result{1.0, 1e9};
    state.algo->prepareFrameFn(state.algo->ctx, &info, &result);

    if (std::isfinite(result.opacity)) {
        const double op = clamp01(result.opacity);
        if (op <= 0.001) {
            quads.clear();
            return;
        }
        data.setOpacity(data.opacity() * op);
    }

    if (std::isfinite(result.clip_v_max) && result.clip_v_max < 10.0) {
        const double quadOffsetMain = vertical ? quadOffsetY : quadOffsetX;
        const double cutQuadMain = winFar + result.clip_v_max * winMainSpan + quadOffsetMain;
        quads = vertical ? quads.splitAtY(cutQuadMain) : quads.splitAtX(cutQuadMain);

        KWin::WindowQuadList keptQuads;
        keptQuads.reserve(quads.size());
        for (const KWin::WindowQuad &q : quads) {
            double minM = vertical ? q[0].y() : q[0].x();
            double maxM = minM;
            for (int i = 1; i < 4; ++i) {
                const double m = vertical ? q[i].y() : q[i].x();
                minM = std::min(minM, m);
                maxM = std::max(maxM, m);
            }
            const double midScreenM = 0.5 * (minM + maxM) - quadOffsetMain;
            const double midV = (midScreenM - winFar) / winMainSpan;
            if (midV <= result.clip_v_max) {
                keptQuads.append(q);
            }
        }
        quads = keptQuads;
        if (quads.isEmpty()) {
            return;
        }
    }

    const int nQuads = quads.size();
    m_scratchVerts.resize(size_t(nQuads) * 4);

    for (int qi = 0; qi < nQuads; ++qi) {
        const KWin::WindowQuad &q = quads[qi];
        for (int i = 0; i < 4; ++i) {
            const double sx = q[i].x() - quadOffsetX;
            const double sy = q[i].y() - quadOffsetY;
            const double origMain = vertical ? sy : sx;
            const double origCross = vertical ? sx : sy;

            SmoothLampVertex &sv = m_scratchVerts[size_t(qi) * 4 + size_t(i)];
            sv.x = sx;
            sv.y = sy;
            sv.u = (origCross - winU0) / winCrossSpan;
            sv.v = (origMain - winFar) / winMainSpan;
        }
    }

    state.algo->deformFn(state.algo->ctx, &info, m_scratchVerts.data(), nQuads * 4, &result);

    for (int qi = 0; qi < nQuads; ++qi) {
        KWin::WindowQuad &q = quads[qi];
        for (int i = 0; i < 4; ++i) {
            const SmoothLampVertex &sv = m_scratchVerts[size_t(qi) * 4 + size_t(i)];
            if (std::isfinite(sv.x) && std::isfinite(sv.y)) {
                q[i].setX(sv.x + quadOffsetX);
                q[i].setY(sv.y + quadOffsetY);
            }
        }
    }
}