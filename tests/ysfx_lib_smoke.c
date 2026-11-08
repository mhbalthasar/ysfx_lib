// C ABI smoke test for the shared ysfx library.
//
// This is deliberately a plain C program: the TuneLab bridge consumes the
// library through P/Invoke and must be able to load, compile, process,
// serialize and render a JSFX without any C++ host code.

#include "ysfx.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void log_cb(intptr_t userdata, ysfx_log_level level, const char *message)
{
    (void)userdata;
    fprintf(stderr, "ysfx[%s] %s\n", ysfx_log_level_string(level), message);
}

static int fail(const char *what)
{
    fprintf(stderr, "FAIL: %s\n", what);
    return 1;
}

static int check_process(ysfx_t *fx, float gain, float expected_l, float expected_r)
{
    enum { n = 64 };
    float in_l[n], in_r[n], out_l[n], out_r[n];
    const float *ins[2];
    float *outs[2];

    for (uint32_t i = 0; i < n; ++i) {
        in_l[i] = 0.25f;
        in_r[i] = -0.5f;
        out_l[i] = 123.0f;
        out_r[i] = 456.0f;
    }
    ins[0] = in_l;
    ins[1] = in_r;
    outs[0] = out_l;
    outs[1] = out_r;

    ysfx_slider_set_value(fx, 0, gain, true);
    ysfx_process_float(fx, ins, outs, 2, 2, n);

    if (out_l[0] != expected_l || out_l[n - 1] != expected_l ||
        out_r[0] != expected_r || out_r[n - 1] != expected_r) {
        fprintf(stderr, "FAIL: process gain=%.3f got %.6f/%.6f expected %.6f/%.6f\n",
                gain, out_l[0], out_r[0], expected_l, expected_r);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: ysfx_lib_smoke <file.jsfx>\n");
        return 2;
    }

    ysfx_config_t *config = ysfx_config_new();
    ysfx_set_log_reporter(config, log_cb);
    ysfx_set_user_data(config, 0);
    ysfx_register_builtin_audio_formats(config);

    ysfx_t *fx = ysfx_new(config);
    if (!ysfx_load_file(fx, argv[1], 0))
        return fail("ysfx_load_file");
    if (!ysfx_is_loaded(fx))
        return fail("ysfx_is_loaded");

    if (ysfx_get_num_inputs(fx) != 2 || ysfx_get_num_outputs(fx) != 2)
        return fail("expected a 2-in/2-out effect");

    if (!ysfx_has_section(fx, ysfx_section_init) ||
        !ysfx_has_section(fx, ysfx_section_slider) ||
        !ysfx_has_section(fx, ysfx_section_block) ||
        !ysfx_has_section(fx, ysfx_section_sample) ||
        !ysfx_has_section(fx, ysfx_section_serialize) ||
        !ysfx_has_section(fx, ysfx_section_gfx))
        return fail("missing expected JSFX section");

    if (!ysfx_compile(fx, 0))
        return fail("ysfx_compile");

    ysfx_set_block_size(fx, 64);
    ysfx_set_sample_rate(fx, 48000.0);
    ysfx_init(fx);

    if (!ysfx_slider_exists(fx, 0) || !ysfx_slider_exists(fx, 1))
        return fail("slider1/slider2 missing");

    if (!check_process(fx, 1.0f, 0.25f, -0.5f))
        return fail("unity-gain processing");
    if (!check_process(fx, 0.5f, 0.125f, -0.25f))
        return fail("half-gain processing");

    ysfx_slider_set_value(fx, 0, 1.0, true);
    ysfx_state_t *state = ysfx_save_state(fx);
    if (!state)
        return fail("ysfx_save_state");
    if (state->slider_count != 2 || state->data_size != 4)
        return fail("unexpected serialized state shape");

    ysfx_slider_set_value(fx, 0, 0.0, true);
    if (!ysfx_load_state(fx, state))
        return fail("ysfx_load_state");
    if (ysfx_slider_get_value(fx, 0) != 1.0)
        return fail("slider state was not restored");
    if (ysfx_read_var(fx, "custom") != 1234.0)
        return fail("custom @serialize value was not restored");
    ysfx_state_free(state);

    uint32_t dim[2] = {0, 0};
    if (!ysfx_get_gfx_dim(fx, dim) || dim[0] == 0 || dim[1] == 0) {
        dim[0] = 160;
        dim[1] = 120;
    }
    uint8_t *pixels = (uint8_t *)calloc((size_t)dim[0] * dim[1] * 4, 1);
    if (!pixels)
        return fail("calloc gfx buffer");

    ysfx_gfx_config_t gc;
    memset(&gc, 0, sizeof(gc));
    gc.pixel_width = dim[0];
    gc.pixel_height = dim[1];
    gc.pixel_stride = dim[0] * 4;
    gc.pixels = pixels;
    gc.scale_factor = 1.0;
    ysfx_gfx_setup(fx, &gc);
    ysfx_gfx_set_window_state(fx, true, true, false);
    if (!ysfx_gfx_run(fx))
        return fail("ysfx_gfx_run did not paint");
    if (ysfx_get_requested_framerate(fx) == 0)
        return fail("gfx framerate is zero");
    // The test shader fills the canvas with red.  Pixels are BGRA on
    // little-endian hosts, so byte 2 is the red channel.
    if (!(pixels[2] > 0x80 && pixels[1] < 0x40 && pixels[0] < 0x40))
        return fail("gfx did not render the red background");

    free(pixels);
    ysfx_free(fx);
    ysfx_config_free(config);
    printf("PASS: load/compile/process/state/@gfx\n");
    return 0;
}
