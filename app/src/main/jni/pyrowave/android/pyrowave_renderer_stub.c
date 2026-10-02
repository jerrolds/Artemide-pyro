// PyroWave is only built for 64-bit ABIs (Vulkan 1.3 devices are 64-bit).
// Elsewhere the renderer reports itself unavailable, so it is never selected.

#include "pyrowave_renderer.h"

#include <stdio.h>
#include <string.h>

bool PwIsAvailable(char* reason, size_t reasonSize)
{
    if (reason != NULL && reasonSize != 0) {
        snprintf(reason, reasonSize, "PyroWave is not built for this ABI");
    }
    return false;
}

int PwRendererSetup(int videoFormat, int width, int height, int frameRate, bool fullRange)
{
    (void)videoFormat;
    (void)width;
    (void)height;
    (void)frameRate;
    (void)fullRange;
    return -1;
}

void PwRendererSetWindow(struct ANativeWindow* window)
{
    (void)window;
}

void PwRendererSetKeepWarm(bool enabled, int refreshHz)
{
    (void)enabled;
    (void)refreshHz;
}

void PwRendererSetPacing(bool justInTime)
{
    (void)justInTime;
}

void PwRendererSetPreParse(bool enabled)
{
    (void)enabled;
}

void PwRendererStart(void) {}
void PwRendererStop(void) {}
void PwRendererCleanup(void) {}

int PwRendererSubmitDecodeUnit(PDECODE_UNIT decodeUnit)
{
    (void)decodeUnit;
    return DR_OK;
}

void PwRendererGetStats(PW_RENDERER_STATS* stats)
{
    memset(stats, 0, sizeof(*stats));
}
