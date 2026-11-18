#pragma once

#define SMOOTHLAMP_ALGO_ABI_VERSION 4

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double t;        // 宿主线性进度 [0.0, 1.0] (0=完整窗口, 1=进入图标)
    int minimizing;  // 1=最小化收起, 0=还原展开
    int edge;        // 0=Bottom, 1=Top, 2=Left, 3=Right
    double frame_x, frame_y, frame_w, frame_h;
    double icon_x, icon_y, icon_w, icon_h;
    double screen_x, screen_y, screen_w, screen_h;
} SmoothLampFrameInfo;

typedef struct {
    double x; // 入参: 原屏幕坐标 X; 出参: 变形后屏幕坐标 X
    double y; // 入参: 原屏幕坐标 Y; 出参: 变形后屏幕坐标 Y
    double u; // 横向归一化 [0, 1]（阴影区域可略超出）
    double v; // 纵向归一化（0=远端, 1=近图标端）
} SmoothLampVertex;

typedef struct {
    double opacity;    // 本帧透明度倍率 [0.0, 1.0]
    double clip_v_max; // 最大保留的纵向归一化坐标 v（超出裁切；>= 10.0 表示不裁切）
} SmoothLampFrameResult;

typedef struct {
    double turnaround_ms;
    int ignore_global_speed;
    double grid_step_px;
    int min_grid_steps;
    int max_grid_steps;
    int cross_steps_min;
    int cross_steps_max;
} SmoothLampHostParams;

typedef int (*SmoothLampAbiVersionFn)(void);
typedef void *(*SmoothLampCreateFn)(const char *config_path);
typedef void (*SmoothLampDestroyFn)(void *ctx);
typedef void (*SmoothLampGetParamsFn)(void *ctx, SmoothLampHostParams *out_params);
typedef double (*SmoothLampCalcDurationFn)(void *ctx, const SmoothLampFrameInfo *info);
typedef double (*SmoothLampRemapProgressFn)(void *ctx, double t, int from_minimizing, int to_minimizing);
typedef void (*SmoothLampPrepareFrameFn)(void *ctx,
                                         const SmoothLampFrameInfo *info,
                                         SmoothLampFrameResult *out_result);
typedef void (*SmoothLampDeformFn)(void *ctx,
                                   const SmoothLampFrameInfo *info,
                                   SmoothLampVertex *verts,
                                   int count,
                                   SmoothLampFrameResult *out_result);

#ifdef __cplusplus
}
#endif
