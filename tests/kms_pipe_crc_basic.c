/*
 * Copyright © 2013 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include "igt.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>


typedef struct {
	int drm_fd;
	igt_display_t display;
	struct igt_fb fb;
} data_t;

static struct {
	double r, g, b;
	igt_crc_t crc;
} colors[2] = {
	{ .r = 0.0, .g = 1.0, .b = 0.0 },
	{ .r = 0.0, .g = 1.0, .b = 1.0 },
};

static void test_bad_command(data_t *data, const char *cmd)
{
	FILE *ctl;
	size_t written;

	ctl = igt_debugfs_fopen("vkms_display_crc_ctl", "r+");
	igt_require(ctl);

	written = fwrite(cmd, 1, strlen(cmd), ctl);
	fflush(ctl);
	igt_assert_eq(written, strlen(cmd));
	igt_assert(ferror(ctl));
	igt_assert_eq(errno, EINVAL);

	fclose(ctl);
}

static void test_bad_source(data_t *data)
{
	FILE *f;
	size_t written;
	const char *source = "foo";

	f = igt_debugfs_fopen("crtc-0/crc/control", "w");
	if (!f) {
		test_bad_command(data, "pipe A foo");
		return;
	}

	written = fwrite(source, 1, strlen(source), f);
	fflush(f);
	igt_assert_eq(written, strlen(source));
	igt_assert(!ferror(f));
	igt_assert(!errno);
	fclose(f);

	f = igt_debugfs_fopen("crtc-0/crc/data", "w");
	igt_assert(!f);
	igt_assert_eq(errno, EINVAL);
}

#define N_CRCS	3

#define TEST_SEQUENCE (1<<0)
#define TEST_NONBLOCK (1<<1)

static void
test_read_crc_for_output(data_t *data, int pipe, igt_output_t *output,
			 unsigned flags)
{
	igt_display_t *display = &data->display;
	igt_plane_t *primary;
	drmModeModeInfo *mode;
	igt_pipe_crc_t *pipe_crc;
	igt_crc_t *crcs = NULL;
	int c, j;

	for (c = 0; c < ARRAY_SIZE(colors); c++) {
		char *crc_str;
		int n_crcs;

		igt_output_set_pipe(output, pipe);

		igt_debug("Clearing the fb with color (%.02lf,%.02lf,%.02lf)\n",
			  colors[c].r, colors[c].g, colors[c].b);

		mode = igt_output_get_mode(output);
		igt_create_color_fb(data->drm_fd,
					mode->hdisplay, mode->vdisplay,
					DRM_FORMAT_XRGB8888,
					LOCAL_DRM_FORMAT_MOD_NONE,
					colors[c].r,
					colors[c].g,
					colors[c].b,
					&data->fb);

		primary = igt_output_get_plane(output, 0);
		igt_plane_set_fb(primary, &data->fb);

		igt_display_commit(display);

		if (flags & TEST_NONBLOCK)
			pipe_crc = igt_pipe_crc_new_nonblock(pipe, INTEL_PIPE_CRC_SOURCE_AUTO);
		else
			pipe_crc = igt_pipe_crc_new(pipe, INTEL_PIPE_CRC_SOURCE_AUTO);

		igt_pipe_crc_start(pipe_crc);

		/* wait for N_CRCS vblanks and the corresponding N_CRCS CRCs */
		if (flags & TEST_NONBLOCK) {
			int i;

			for (i = 0; i < N_CRCS; i++)
				igt_wait_for_vblank(data->drm_fd, pipe);

			n_crcs = igt_pipe_crc_get_crcs(pipe_crc, N_CRCS * 3, &crcs);
			/* allow a one frame difference */
			igt_assert_lte(n_crcs, N_CRCS + 1);
			igt_assert_lte(N_CRCS, n_crcs + 1);
		} else {
			n_crcs = igt_pipe_crc_get_crcs(pipe_crc, N_CRCS, &crcs);
			igt_assert_eq(n_crcs, N_CRCS);
		}

		igt_pipe_crc_stop(pipe_crc);

		/*
		 * save the CRC in colors so it can be compared to the CRC of
		 * other fbs
		 */
		colors[c].crc = crcs[0];

		crc_str = igt_crc_to_string(&crcs[0]);
		igt_debug("CRC for this fb: %s\n", crc_str);
		free(crc_str);

		/* and ensure that they'are all equal, we haven't changed the fb */
		for (j = 0; j < (n_crcs - 1); j++)
			igt_assert_crc_equal(&crcs[j], &crcs[j + 1]);

		if (flags & TEST_SEQUENCE)
			for (j = 0; j < (n_crcs - 1); j++)
				igt_assert_eq(crcs[j].frame + 1, crcs[j + 1].frame);

		free(crcs);
		igt_pipe_crc_free(pipe_crc);
		igt_remove_fb(data->drm_fd, &data->fb);
		igt_plane_set_fb(primary, NULL);

		igt_output_set_pipe(output, PIPE_ANY);
	}
}

static void test_read_crc(data_t *data, int pipe, unsigned flags)
{
	igt_display_t *display = &data->display;
	int valid_connectors = 0;
	igt_output_t *output;

	igt_skip_on(pipe >= data->display.n_pipes);

	for_each_valid_output_on_pipe(display, pipe, output) {

		igt_info("%s: Testing connector %s using pipe %s\n",
			 igt_subtest_name(), igt_output_name(output),
			 kmstest_pipe_name(pipe));

		test_read_crc_for_output(data, pipe, output, flags);
		valid_connectors ++;
	}

	igt_require_f(valid_connectors, "No connector found for pipe %i\n", pipe);
}

data_t data = {0, };

igt_main
{
	igt_skip_on_simulation();

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);

		igt_enable_connectors();

		kmstest_set_vt_graphics_mode();

		igt_require_pipe_crc();

		igt_display_init(&data.display, data.drm_fd);
	}

	igt_subtest("bad-pipe")
		test_bad_command(&data, "pipe D none");

	igt_subtest("bad-source")
		test_bad_source(&data);

	igt_subtest("bad-nb-words-1")
		test_bad_command(&data, "pipe foo");

	igt_subtest("bad-nb-words-3")
		test_bad_command(&data, "pipe A none option");

	for (int i = 0; i < 3; i++) {
		igt_subtest_f("read-crc-pipe-%c", 'A'+i)
			test_read_crc(&data, i, 0);

		igt_subtest_f("read-crc-pipe-%c-frame-sequence", 'A'+i)
			test_read_crc(&data, i, TEST_SEQUENCE);

		igt_subtest_f("nonblocking-crc-pipe-%c", 'A'+i)
			test_read_crc(&data, i, TEST_NONBLOCK);

		igt_subtest_f("nonblocking-crc-pipe-%c-frame-sequence", 'A'+i)
			test_read_crc(&data, i, TEST_SEQUENCE | TEST_NONBLOCK);

		igt_subtest_f("suspend-read-crc-pipe-%c", 'A'+i) {
			igt_skip_on(i >= data.display.n_pipes);
			igt_system_suspend_autoresume(SUSPEND_STATE_MEM,
						      SUSPEND_TEST_NONE);

			test_read_crc(&data, i, 0);
		}

		igt_subtest_f("hang-read-crc-pipe-%c", 'A'+i) {
			igt_hang_t hang =
				igt_hang_ring(data.drm_fd, I915_EXEC_RENDER);
			test_read_crc(&data, i, 0);
			igt_post_hang_ring(data.drm_fd, hang);
			test_read_crc(&data, i, 0);
		}
	}

	igt_fixture {
		igt_display_fini(&data.display);
	}
}
