#include "smoothlamp_algo_abi.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <string>

namespace {

constexpr double kPi = 3.14159265358979323846;

inline double clamp01(double v)
{
    return std::clamp(v, 0.0, 1.0);
}

inline double lerp(double a, double b, double t)
{
    return a + (b - a) * t;
}

inline double glslSmoothStep(double edge0, double edge1, double x)
{
    const double span = edge1 - edge0;
    if (span <= 1e-6) {
        return x >= edge1 ? 1.0 : 0.0;
    }
    const double t = clamp01((x - edge0) / span);
    return t * t * (3.0 - 2.0 * t);
}

std::string trim(const std::string &s)
{
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) {
        ++a;
    }
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) {
        --b;
    }
    return s.substr(a, b - a);
}

bool parseDouble(const std::string &s, double &out)
{
    double v = 0.0;
    const char *begin = s.data();
    const char *end = begin + s.size();
    const auto [ptr, ec] = std::from_chars(begin, end, v);
    if (ec == std::errc() && ptr == end && std::isfinite(v)) {
        out = v;
        return true;
    }
    return false;
}

bool parseInt(const std::string &s, int &out)
{
    int v = 0;
    const char *begin = s.data();
    const char *end = begin + s.size();
    const auto [ptr, ec] = std::from_chars(begin, end, v);
    if (ec == std::errc() && ptr == end) {
        out = v;
        return true;
    }
    return false;
}

struct AlgoContext {
    SmoothLampHostParams host{55.0, 0, 5.0, 80, 128, 24, 32};

    double refWinW = 860.0;
    double refWinH = 590.0;
    double refDurationMs = 380.0;
    double unminimizeSpeedScale = 1.08;
    double sizeScaleExponent = 0.24;
    double minDurationScale = 0.82;
    double maxDurationScale = 1.25;
    bool pourMode = true;
    double pourMix = 0.62; // 1.0=纯倾泻, 0.0=纯压扁, 0.62=抗锯齿甜点混合

    double normSquashEnd = 0.34;
    double normNearReachEnd = 0.28;
    double normSuckStart = 0.16;
    double normSuckPower = 1.15;
    double normTipHalfW = 0.012;
    double minNeckHalfPx = 2.0;
    double tuckStart = 0.84;
    double iconAnchor = 0.62;
    double tailFadeSpan = 0.14;
};

struct FrameGeom {
    bool vertical = true;
    bool canPour = false;
    double c = 0.0;
    double opacity = 1.0;
    double clipVMax = 1e9;
    double winFar = 0.0;
    double iconTarget = 0.0;
    double winU0 = 0.0;
    double winU1 = 0.0;
    double iconCenterU = 0.0;
    double effectiveNeckHalf = 2.0;
    double funnelStrength = 0.0;
    double curFar = 0.0;
    double curNearOrVirtual = 0.0;
    double span = 0.0;
    bool validSpan = false;
};

inline double evalProgressCurve(double rawT, int minimizing)
{
    const double t = clamp01(rawT);
    return minimizing ? t : (t * t);
}

inline double invertProgressCurve(double visualT, int targetMinimizing)
{
    const double t = clamp01(visualT);
    return targetMinimizing ? t : std::sqrt(t);
}

FrameGeom computeFrameGeom(const AlgoContext &cfg, const SmoothLampFrameInfo &info)
{
    FrameGeom g;
    g.c = evalProgressCurve(info.t, info.minimizing);
    g.opacity = (cfg.tailFadeSpan > 1e-5)
        ? glslSmoothStep(0.0, cfg.tailFadeSpan, 1.0 - g.c)
        : 1.0;

    g.vertical = (info.edge == 0 || info.edge == 1);
    const double winCross = g.vertical ? info.frame_w : info.frame_h;

    double winNear = 0.0, iconU0 = 0.0, iconU1 = 0.0;
    switch (info.edge) {
    case 0: // Bottom
        g.winFar = info.frame_y; winNear = info.frame_y + info.frame_h;
        g.iconTarget = lerp(info.icon_y, info.icon_y + info.icon_h, cfg.iconAnchor);
        g.winU0 = info.frame_x; g.winU1 = info.frame_x + info.frame_w;
        iconU0 = info.icon_x; iconU1 = info.icon_x + info.icon_w;
        break;
    case 1: // Top
        g.winFar = info.frame_y + info.frame_h; winNear = info.frame_y;
        g.iconTarget = lerp(info.icon_y + info.icon_h, info.icon_y, cfg.iconAnchor);
        g.winU0 = info.frame_x; g.winU1 = info.frame_x + info.frame_w;
        iconU0 = info.icon_x; iconU1 = info.icon_x + info.icon_w;
        break;
    case 3: // Right
        g.winFar = info.frame_x; winNear = info.frame_x + info.frame_w;
        g.iconTarget = lerp(info.icon_x, info.icon_x + info.icon_w, cfg.iconAnchor);
        g.winU0 = info.frame_y; g.winU1 = info.frame_y + info.frame_h;
        iconU0 = info.icon_y; iconU1 = info.icon_y + info.icon_h;
        break;
    default: // Left (2)
        g.winFar = info.frame_x + info.frame_w; winNear = info.frame_x;
        g.iconTarget = lerp(info.icon_x + info.icon_w, info.icon_x, cfg.iconAnchor);
        g.winU0 = info.frame_y; g.winU1 = info.frame_y + info.frame_h;
        iconU0 = info.icon_y; iconU1 = info.icon_y + info.icon_h;
        break;
    }

    g.iconCenterU = 0.5 * (iconU0 + iconU1);
    const double iconCross = std::max(4.0, std::abs(iconU1 - iconU0));

    const double maxNeckHalf = std::max(cfg.minNeckHalfPx, std::min(14.0, iconCross * 0.30));
    const double baseNeckHalf = std::clamp(winCross * cfg.normTipHalfW, cfg.minNeckHalfPx, maxNeckHalf);
    const double tuck = glslSmoothStep(cfg.tuckStart, 1.0, g.c);
    g.effectiveNeckHalf = lerp(baseNeckHalf, std::max(1.8, cfg.minNeckHalfPx * 0.85), tuck);

    g.funnelStrength = glslSmoothStep(0.0, cfg.normSquashEnd, g.c);
    const double pNear = glslSmoothStep(0.0, cfg.normNearReachEnd, g.c);
    const double slideRaw = glslSmoothStep(cfg.normSuckStart, 1.0, g.c);
    const double pSuck = std::pow(slideRaw, cfg.normSuckPower);

    g.curFar = lerp(g.winFar, g.iconTarget, pSuck);
    g.span = g.iconTarget - g.winFar;
    g.validSpan = std::abs(g.span) > 1.0;

    const double winMainSpan = winNear - g.winFar;
    const double curNearSquash = lerp(winNear, g.iconTarget, pNear);
    const double drop = g.curFar - g.winFar;
    const double effectiveMix = cfg.pourMode ? cfg.pourMix : 0.0;
    const double virtualNear = curNearSquash + drop * effectiveMix;
    const double virtualSpan = virtualNear - g.curFar;

    g.canPour = cfg.pourMode
        && (effectiveMix > 0.01)
        && (winMainSpan * g.span > 0.0)
        && (std::abs(g.span) > 4.0)
        && (std::abs(virtualSpan) > 4.0);

    if (g.canPour) {
        g.curNearOrVirtual = virtualNear;
        g.clipVMax = (g.iconTarget - g.curFar) / virtualSpan;
    } else {
        g.curNearOrVirtual = curNearSquash;
        g.clipVMax = 1e9;
    }
    return g;
}

void loadIni(AlgoContext &ctx, const char *path)
{
    if (!path || !*path) return;
    std::ifstream in(path);
    if (!in.is_open()) return;

    std::string line, section;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));

        double d = 0.0;
        int n = 0;
        if (section == "Basic") {
            if (key == "refDurationMs" && parseDouble(val, d)) ctx.refDurationMs = std::clamp(d, 50.0, 3000.0);
            else if (key == "unminimizeSpeedScale" && parseDouble(val, d)) ctx.unminimizeSpeedScale = std::clamp(d, 0.2, 3.0);
            else if (key == "turnaroundMs" && parseDouble(val, d)) ctx.host.turnaround_ms = d;
            else if (key == "ignoreGlobalSpeed") ctx.host.ignore_global_speed = (val == "true" || val == "1") ? 1 : 0;
            else if (key == "sizeScaleExponent" && parseDouble(val, d)) ctx.sizeScaleExponent = std::clamp(d, 0.0, 1.0);
            else if (key == "minDurationScale" && parseDouble(val, d)) ctx.minDurationScale = std::clamp(d, 0.4, 1.0);
            else if (key == "maxDurationScale" && parseDouble(val, d)) ctx.maxDurationScale = std::clamp(d, 1.0, 2.5);
            else if (key == "pourMode") ctx.pourMode = (val == "true" || val == "1");
            else if (key == "pourMix" && parseDouble(val, d)) ctx.pourMix = std::clamp(d, 0.0, 1.0);
        } else if (section == "QML_Standard") {
            if (key == "refWinW" && parseDouble(val, d)) ctx.refWinW = std::clamp(d, 200.0, 3840.0);
            else if (key == "refWinH" && parseDouble(val, d)) ctx.refWinH = std::clamp(d, 200.0, 2160.0);
            else if (key == "normSquashEnd" && parseDouble(val, d)) ctx.normSquashEnd = std::clamp(d, 0.10, 0.80);
            else if (key == "normNearReachEnd" && parseDouble(val, d)) ctx.normNearReachEnd = std::clamp(d, 0.10, 0.80);
            else if (key == "normSuckStart" && parseDouble(val, d)) ctx.normSuckStart = std::clamp(d, 0.05, 0.60);
            else if (key == "normSuckPower" && parseDouble(val, d)) ctx.normSuckPower = std::clamp(d, 0.8, 2.5);
            else if (key == "normTipHalfW" && parseDouble(val, d)) ctx.normTipHalfW = std::clamp(d, 0.002, 0.06);
            else if (key == "minNeckHalfPx" && parseDouble(val, d)) ctx.minNeckHalfPx = std::clamp(d, 0.5, 14.0);
            else if (key == "tuckStart" && parseDouble(val, d)) ctx.tuckStart = std::clamp(d, 0.30, 0.98);
            else if (key == "iconAnchor" && parseDouble(val, d)) ctx.iconAnchor = std::clamp(d, 0.10, 1.10);
            else if (key == "tailFadeSpan" && parseDouble(val, d)) ctx.tailFadeSpan = std::clamp(d, 0.0, 0.35);
        } else if (section == "Mesh") {
            if (key == "gridStepPx" && parseDouble(val, d)) ctx.host.grid_step_px = d;
            else if (key == "minGridSteps" && parseInt(val, n)) ctx.host.min_grid_steps = n;
            else if (key == "maxGridSteps" && parseInt(val, n)) ctx.host.max_grid_steps = n;
            else if (key == "crossStepsMin" && parseInt(val, n)) ctx.host.cross_steps_min = n;
            else if (key == "crossStepsMax" && parseInt(val, n)) ctx.host.cross_steps_max = n;
        }
    }
}

} // namespace

extern "C" {

int smoothlamp_algo_abi_version(void)
{
    return SMOOTHLAMP_ALGO_ABI_VERSION;
}

void *smoothlamp_algo_create(const char *config_path)
{
    auto *ctx = new AlgoContext();
    loadIni(*ctx, config_path);
    return ctx;
}

void smoothlamp_algo_destroy(void *raw_ctx)
{
    delete static_cast<AlgoContext *>(raw_ctx);
}

void smoothlamp_algo_get_params(void *raw_ctx, SmoothLampHostParams *out_params)
{
    if (!raw_ctx || !out_params) return;
    *out_params = static_cast<const AlgoContext *>(raw_ctx)->host;
}

double smoothlamp_algo_calc_duration(void *raw_ctx, const SmoothLampFrameInfo *info)
{
    if (!raw_ctx || !info) return 380.0;
    const AlgoContext &cfg = *static_cast<const AlgoContext *>(raw_ctx);

    const double winW = std::max(1.0, info->frame_w);
    const double winH = std::max(1.0, info->frame_h);
    const double refDiag = std::hypot(cfg.refWinW, cfg.refWinH);
    const double curDiag = std::hypot(winW, winH);
    const double sigma = std::max(0.2, curDiag / std::max(1.0, refDiag));

    const double scale = std::clamp(
        std::pow(sigma, cfg.sizeScaleExponent),
        cfg.minDurationScale,
        cfg.maxDurationScale);

    double ms = cfg.refDurationMs * scale;
    if (!info->minimizing) {
        ms *= cfg.unminimizeSpeedScale;
    }
    return std::max(30.0, ms);
}

double smoothlamp_algo_remap_progress(void *raw_ctx, double t, int from_minimizing, int to_minimizing)
{
    (void)raw_ctx;
    const double visualT = evalProgressCurve(t, from_minimizing);
    return invertProgressCurve(visualT, to_minimizing);
}

void smoothlamp_algo_prepare_frame(void *raw_ctx,
                                   const SmoothLampFrameInfo *info,
                                   SmoothLampFrameResult *out_result)
{
    if (!raw_ctx || !info || !out_result) return;
    const AlgoContext &cfg = *static_cast<const AlgoContext *>(raw_ctx);
    const FrameGeom g = computeFrameGeom(cfg, *info);
    out_result->opacity = g.opacity;
    out_result->clip_v_max = g.clipVMax;
}

void smoothlamp_algo_deform(void *raw_ctx,
                            const SmoothLampFrameInfo *info,
                            SmoothLampVertex *verts,
                            int count,
                            SmoothLampFrameResult *out_result)
{
    if (!raw_ctx || !info || !verts || count <= 0) return;

    const AlgoContext &cfg = *static_cast<const AlgoContext *>(raw_ctx);
    const FrameGeom g = computeFrameGeom(cfg, *info);

    if (out_result) {
        out_result->opacity = g.opacity;
        out_result->clip_v_max = g.clipVMax;
    }

    double lastV = -99999.0;
    double cachedY = 0.0;
    double cachedXL = 0.0;
    double cachedSpanX = 0.0;
    double cachedShadowTuck = 0.0;

    for (int i = 0; i < count; ++i) {
        const double u = verts[i].u;
        const double v = verts[i].v;

        if (std::abs(v - lastV) > 1e-5) {
            lastV = v;
            cachedY = lerp(g.curFar, g.curNearOrVirtual, v);
            if (g.canPour) {
                if (g.span > 0.0) {
                    cachedY = std::min(cachedY, g.iconTarget);
                } else {
                    cachedY = std::max(cachedY, g.iconTarget);
                }
            }

            const double h = g.validSpan ? clamp01((cachedY - g.winFar) / g.span) : clamp01(v);
            const double sCurve = 0.5 - 0.5 * std::cos(kPi * h);
            const double pinchAtY = g.funnelStrength * sCurve;

            cachedXL = lerp(g.winU0, g.iconCenterU - g.effectiveNeckHalf, pinchAtY);
            const double curXR = lerp(g.winU1, g.iconCenterU + g.effectiveNeckHalf, pinchAtY);
            cachedSpanX = curXR - cachedXL;
            cachedShadowTuck = glslSmoothStep(0.12, 0.50, pinchAtY);
        }

        const double safeU = (u >= 0.0 && u <= 1.0)
            ? u
            : lerp(u, clamp01(u), cachedShadowTuck);
        const double x = cachedXL + safeU * cachedSpanX;

        verts[i].x = g.vertical ? x : cachedY;
        verts[i].y = g.vertical ? cachedY : x;
    }
}

} // extern "C"