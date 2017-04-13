/*
 * Copyright © 2007, 2011, 2013 Intel Corporation
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
 * Authors:
 *    Eric Anholt <eric@anholt.net>
 *    Daniel Vetter <daniel.vetter@ffwll.ch>
 *
 */

#ifndef ANDROID
#define _GNU_SOURCE
#else
#include <libgen.h>
#endif
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <string.h>
#include <sys/mman.h>
#include <signal.h>
#include <pciaccess.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <termios.h>

#include "drmtest.h"
#include "i915_drm.h"
#include "intel_chipset.h"
#include "intel_io.h"
#include "igt_gt.h"
#include "igt_debugfs.h"
#include "version.h"
#include "config.h"
#include "intel_reg.h"
#include "ioctl_wrappers.h"
#include "igt_dummyload.h"

/**
 * SECTION:drmtest
 * @short_description: Base library for drm tests and tools
 * @title: drmtest
 * @include: igt.h
 *
 * This library contains the basic support for writing tests, with the most
 * important part being the helper function to open drm device nodes.
 *
 * But there's also a bit of other assorted stuff here.
 *
 * Note that this library's header pulls in the [i-g-t core](intel-gpu-tools-i-g-t-core.html)
 * and [batchbuffer](intel-gpu-tools-intel-batchbuffer.html) libraries as dependencies.
 */

uint16_t __drm_device_id;

static int __get_drm_device_name(int fd, char *name)
{
	drm_version_t version;

	memset(&version, 0, sizeof(version));
	version.name_len = 4;
	version.name = name;

	if (!drmIoctl(fd, DRM_IOCTL_VERSION, &version)){
		return 0;
	}

	return -1;
}

static bool __is_device(int fd, const char *expect)
{
	char name[5] = "";

	if (__get_drm_device_name(fd, name))
		return false;

	return strcmp(expect, name) == 0;
}

bool is_i915_device(int fd)
{
	return __is_device(fd, "i915");
}

static bool is_vc4_device(int fd)
{
	return __is_device(fd, "vc4");
}

static bool is_vgem_device(int fd)
{
	return __is_device(fd, "vgem");
}

static bool is_virtio_device(int fd)
{
	return __is_device(fd, "virt");
}

static bool has_known_intel_chipset(int fd)
{
	struct drm_i915_getparam gp;
	int devid = 0;

	memset(&gp, 0, sizeof(gp));
	gp.param = I915_PARAM_CHIPSET_ID;
	gp.value = &devid;

	if (ioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp, sizeof(gp)))
		return false;

	if (!intel_gen(devid))
		return false;

	__drm_device_id = devid;
	return true;
}

#define LOCAL_I915_EXEC_VEBOX	(4 << 0)
/**
 * gem_quiescent_gpu:
 * @fd: open i915 drm file descriptor
 *
 * Ensure the gpu is idle by launching a nop execbuf and stalling for it. This
 * is automatically run when opening a drm device node and is also installed as
 * an exit handler to have the best assurance that the test is run in a pristine
 * and controlled environment.
 *
 * This function simply allows tests to make additional calls in-between, if so
 * desired.
 */
void gem_quiescent_gpu(int fd)
{
	uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj;
	unsigned ring;

	igt_terminate_spin_batches();

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, 4096);
	gem_write(fd, obj.handle, 0, &bbe, sizeof(&bbe));

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;

	for (ring = 0; ring < 1<<6; ring++) {
		execbuf.flags = ring;
		__gem_execbuf(fd, &execbuf);
	}

	if (gem_has_bsd2(fd)) {
		execbuf.flags = I915_EXEC_BSD | (2 << 13);
		__gem_execbuf(fd, &execbuf);
	}

	gem_sync(fd, obj.handle);
	gem_close(fd, obj.handle);

	igt_drop_caches_set(DROP_RETIRE | DROP_FREED);
}

/**
 * drm_get_card:
 *
 * Get an i915 drm card index number for use in /dev or /sys. The minor index of
 * the legacy node is returned, not of the control or render node.
 *
 * Returns:
 * The i915 drm index or -1 on error
 */
int drm_get_card(void)
{
	char *name;
	int i, fd;

	for (i = 0; i < 16; i++) {
		int ret;

		ret = asprintf(&name, "/dev/dri/card%u", i);
		igt_assert(ret != -1);

		fd = open(name, O_RDWR);
		free(name);

		if (fd == -1)
			continue;

		if (!is_i915_device(fd) || !has_known_intel_chipset(fd)) {
			close(fd);
			continue;
		}

		close(fd);
		return i;
	}

	igt_skip("No intel gpu found\n");

	return -1;
}

static int modprobe(const char *driver)
{
	char buf[128];
	snprintf(buf, sizeof(buf), "/sbin/modprobe -s %s", driver);
	return system(buf);
}

/**
 * __drm_open_driver:
 * @chipset: OR'd flags for each chipset to search, eg. #DRIVER_INTEL
 *
 * Open the first DRM device we can find, searching up to 16 device nodes
 *
 * Returns:
 * An open DRM fd or -1 on error
 */
int __drm_open_driver(int chipset)
{
	if (chipset & DRIVER_VGEM)
		modprobe("vgem");

	for (int i = 2; i >= 0; i--) {
		char name[80];
		int fd;

		sprintf(name, "/dev/dri/card%u", i);
		fd = open(name, O_RDWR);
		if (fd == -1)
			continue;

		if (chipset & DRIVER_INTEL && is_i915_device(fd) &&
		    has_known_intel_chipset(fd))
			return fd;

		if (chipset & DRIVER_VC4 &&
		    is_vc4_device(fd))
			return fd;

		if (chipset & DRIVER_VGEM &&
		    is_vgem_device(fd))
			return fd;

		if (chipset & DRIVER_VIRTIO &&
		    is_virtio_device(fd))
			return fd;

		/* Only VGEM-specific tests should be run on VGEM */
		if (chipset == DRIVER_ANY && !is_vgem_device(fd))
			return fd;

		close(fd);
	}

	return -1;
}

static int __drm_open_driver_render(int chipset)
{
	char *name;
	int i, fd;

	for (i = 128; i < (128 + 16); i++) {
		int ret;

		ret = asprintf(&name, "/dev/dri/renderD%u", i);
		igt_assert(ret != -1);

		fd = open(name, O_RDWR);
		free(name);

		if (fd == -1)
			continue;

		if (!is_i915_device(fd) || !has_known_intel_chipset(fd)) {
			close(fd);
			fd = -1;
			continue;
		}

		return fd;
	}

	return fd;
}

static int at_exit_drm_fd = -1;
static int at_exit_drm_render_fd = -1;

static void quiescent_gpu_at_exit(int sig)
{
	if (at_exit_drm_fd < 0)
		return;

	gem_quiescent_gpu(at_exit_drm_fd);
	close(at_exit_drm_fd);
	at_exit_drm_fd = -1;
}

static void quiescent_gpu_at_exit_render(int sig)
{
	if (at_exit_drm_render_fd < 0)
		return;

	gem_quiescent_gpu(at_exit_drm_render_fd);
	close(at_exit_drm_render_fd);
	at_exit_drm_render_fd = -1;
}

/**
 * drm_open_driver:
 * @chipset: OR'd flags for each chipset to search, eg. #DRIVER_INTEL
 *
 * Open a drm legacy device node. This function always returns a valid
 * file descriptor.
 *
 * Returns: a drm file descriptor
 */
int drm_open_driver(int chipset)
{
	static int open_count;
	int fd;

	fd = __drm_open_driver(chipset);
	igt_skip_on_f(fd<0, "No known gpu found\n");

	/* For i915, at least, we ensure that the driver is idle before
	 * starting a test and we install an exit handler to wait until
	 * idle before quitting.
	 */
	if (is_i915_device(fd)) {
		if (__sync_fetch_and_add(&open_count, 1) == 0) {
			gem_quiescent_gpu(fd);

			at_exit_drm_fd = __drm_open_driver(chipset);
			igt_install_exit_handler(quiescent_gpu_at_exit);
		}
	}

	return fd;
}

/**
 * drm_open_driver_master:
 * @chipset: OR'd flags for each chipset to search, eg. #DRIVER_INTEL
 *
 * Open a drm legacy device node and ensure that it is drm master.
 *
 * Returns:
 * The drm file descriptor or -1 on error
 */
int drm_open_driver_master(int chipset)
{
	int fd = drm_open_driver(chipset);

	igt_require_f(drmSetMaster(fd) == 0, "Can't become DRM master, "
		      "please check if no other DRM client is running.\n");

	return fd;
}

/**
 * drm_open_driver_render:
 * @chipset: OR'd flags for each chipset to search, eg. #DRIVER_INTEL
 *
 * Open a drm render device node.
 *
 * Returns:
 * The drm file descriptor or -1 on error
 */
int drm_open_driver_render(int chipset)
{
	static int open_count;
	int fd = __drm_open_driver_render(chipset);

	/* no render nodes, fallback to drm_open_driver() */
	if (fd == -1)
		return drm_open_driver(chipset);

	if (__sync_fetch_and_add(&open_count, 1))
		return fd;

	at_exit_drm_render_fd = __drm_open_driver(chipset);
	if(chipset & DRIVER_INTEL){
		gem_quiescent_gpu(fd);
		igt_install_exit_handler(quiescent_gpu_at_exit_render);
	}

	return fd;
}

void igt_require_intel(int fd)
{
	igt_require(is_i915_device(fd) && has_known_intel_chipset(fd));
}
