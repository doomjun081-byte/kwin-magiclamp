#include "smoothlamp.h"

KWIN_EFFECT_FACTORY_SUPPORTED(SmoothLampEffect,
                              "metadata.json",
                              return SmoothLampEffect::supported();)

#include "main.moc"