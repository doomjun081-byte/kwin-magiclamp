#pragma once

#include "smoothlamp_algo_abi.h"

#include "effect/offscreeneffect.h"
#include "effect/effectwindow.h"

#include <QHash>
#include <QRectF>
#include <QString>
#include <chrono>
#include <memory>
#include <vector>

class SmoothLampEffect : public KWin::OffscreenEffect
{
    Q_OBJECT

public:
    SmoothLampEffect();
    ~SmoothLampEffect() override;

    static bool supported();

    void reconfigure(ReconfigureFlags flags) override;
    void prePaintScreen(KWin::ScreenPrePaintData &data, std::chrono::milliseconds presentTime) override;
    void prePaintWindow(KWin::RenderView *view,
                        KWin::EffectWindow *w,
                        KWin::WindowPrePaintData &data,
                        std::chrono::milliseconds presentTime) override;
    void postPaintScreen() override;
    bool isActive() const override;
    int requestedEffectChainPosition() const override { return 50; }

protected:
    void apply(KWin::EffectWindow *window, int mask, KWin::WindowPaintData &data, KWin::WindowQuadList &quads) override;

private Q_SLOTS:
    void slotWindowAdded(KWin::EffectWindow *w);
    void slotWindowMinimizedChanged(KWin::EffectWindow *w);
    void slotWindowDeleted(KWin::EffectWindow *w);

private:
    enum class PanelEdge {
        Bottom = 0,
        Top = 1,
        Left = 2,
        Right = 3
    };

    struct LoadedAlgo {
        void *dlHandle = nullptr;
        void *ctx = nullptr;
        SmoothLampDestroyFn destroyFn = nullptr;
        SmoothLampCalcDurationFn calcDurationFn = nullptr;
        SmoothLampRemapProgressFn remapProgressFn = nullptr;
        SmoothLampPrepareFrameFn prepareFrameFn = nullptr;
        SmoothLampDeformFn deformFn = nullptr;
        QString tempSoPath;
        SmoothLampHostParams params{};

        ~LoadedAlgo();
    };

    struct AnimationState {
        double progress = 0.0;
        double velocity = 0.0;
        double durationMs = 380.0;
        bool minimizing = true;
        QRectF iconGeometry;
        PanelEdge edge = PanelEdge::Bottom;
        std::shared_ptr<KWin::EffectWindowVisibleRef> visibleRef;
        std::shared_ptr<LoadedAlgo> algo;
    };

    void handleWindowMinimized(KWin::EffectWindow *w);
    void handleWindowUnminimized(KWin::EffectWindow *w);

    std::shared_ptr<LoadedAlgo> loadAlgorithmLibrary(const QString &configPath);
    PanelEdge detectPanelEdge(KWin::EffectWindow *w, const QRectF &iconGeo) const;
    double windowDurationMs(const std::shared_ptr<LoadedAlgo> &algo,
                            KWin::EffectWindow *w,
                            const QRectF &iconGeo,
                            PanelEdge edge,
                            bool minimizing) const;

    QHash<KWin::EffectWindow *, AnimationState> m_animations;
    std::chrono::milliseconds m_lastPresentTime = std::chrono::milliseconds::zero();
    std::shared_ptr<LoadedAlgo> m_algo;
    std::vector<SmoothLampVertex> m_scratchVerts;
    quint64 m_loadSeq = 0;
};