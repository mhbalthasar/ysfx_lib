// Manual smoke test for the self-contained @gfx window.
//
// This needs a real desktop session.  It is built only when
// YSFX_GFX_WINDOW_SMOKE=ON and is intentionally not registered with CTest:
// headless CI runners cannot create a native window.

#include "ysfx.h"

#include <stdio.h>
#include <string.h>

static int fail(const char *what)
{
    fprintf(stderr, "FAIL: %s\n", what);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: ysfx_gfx_window_smoke <window.jsfx>\n");
        return 2;
    }

    ysfx_config_t *config = ysfx_config_new();
    ysfx_t *fx = ysfx_new(config);

    if (!fx || !ysfx_load_file(fx, argv[1], 0) || !ysfx_compile(fx, 0)) {
        fprintf(stderr, "FAIL: could not load/compile %s\n", argv[1]);
        return 1;
    }

    ysfx_set_block_size(fx, 64);
    ysfx_set_sample_rate(fx, 48000.0);
    ysfx_init(fx);

    ysfx_gfx_window_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.title = "ysfx_lib @gfx window smoke";
    desc.width = 320;
    desc.height = 200;
    desc.scale_factor = 1.0;
    desc.flags = ysfx_gfx_window_flag_resizable;

    ysfx_gfx_window_t *window = ysfx_gfx_window_create(fx, &desc);
    if (!window)
        return fail("ysfx_gfx_window_create");

    for (int i = 0; i < 60 && ysfx_gfx_window_is_open(window); ++i)
        ysfx_gfx_window_pump(window, 10);

    if (ysfx_read_var(fx, "gfx_frames") <= 0.0)
        return fail("@gfx did not run inside the window");

    ysfx_gfx_window_request_close(window);

    for (int i = 0; i < 100 && ysfx_gfx_window_is_open(window); ++i)
        ysfx_gfx_window_pump(window, 10);

    if (ysfx_gfx_window_is_open(window))
        return fail("window did not close");

    ysfx_gfx_window_destroy(window);
    ysfx_free(fx);
    ysfx_config_free(config);

    printf("PASS: @gfx window create/pump/close\n");
    return 0;
}
