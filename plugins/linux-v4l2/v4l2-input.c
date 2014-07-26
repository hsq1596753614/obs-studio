/*
Copyright (C) 2014 by Leonhard Oelke <leonhard@in-verted.de>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/select.h>

#include <linux/videodev2.h>
#include <libv4l2.h>

#include <util/threading.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <obs-module.h>

#define V4L2_DATA(voidptr) struct v4l2_data *data = voidptr;

#define timeval2ns(tv) \
	(((uint64_t) tv.tv_sec * 1000000000) + ((uint64_t) tv.tv_usec * 1000))

#define blog(level, msg, ...) blog(level, "v4l2-input: " msg, ##__VA_ARGS__)

struct v4l2_buffer_data {
	size_t length;
	void *start;
};

/**
 * Data structure for the v4l2 source
 *
 * The data is divided into two sections, data being used inside and outside
 * the capture thread. Data used by the capture thread must not be modified
 * from the outside while the thread is running.
 *
 * Data members prefixed with "set_" are settings from the source properties
 * and may be used from outside the capture thread.
 */
struct v4l2_data {
	/* data used outside of the capture thread */
	obs_source_t source;

	pthread_t thread;
	os_event_t event;

	char *set_device;
	int_fast32_t set_pixfmt;
	int_fast32_t set_res;
	int_fast32_t set_fps;

	/* data used within the capture thread */
	int_fast32_t dev;

	uint64_t frames;
	int_fast32_t width;
	int_fast32_t height;
	int_fast32_t pixfmt;
	uint_fast32_t linesize;

	uint_fast32_t buf_count;
	struct v4l2_buffer_data *buf;
};

static enum video_format v4l2_to_obs_video_format(uint_fast32_t format)
{
	switch (format) {
	case V4L2_PIX_FMT_YVYU:   return VIDEO_FORMAT_YVYU;
	case V4L2_PIX_FMT_YUYV:   return VIDEO_FORMAT_YUY2;
	case V4L2_PIX_FMT_UYVY:   return VIDEO_FORMAT_UYVY;
	case V4L2_PIX_FMT_NV12:   return VIDEO_FORMAT_NV12;
	case V4L2_PIX_FMT_YUV420: return VIDEO_FORMAT_I420;
	case V4L2_PIX_FMT_YVU420: return VIDEO_FORMAT_I420;
	default:                  return VIDEO_FORMAT_NONE;
	}
}

/*
 * used to store framerate and resolution values
 */
static int pack_tuple(int a, int b)
{
	return (a << 16) | (b & 0xffff);
}

static void unpack_tuple(int *a, int *b, int packed)
{
	*a = packed >> 16;
	*b = packed & 0xffff;
}

/* fixed framesizes as fallback */
static const int fixed_framesizes[] =
{
	/* 4:3 */
	160<<16		| 120,
	320<<16		| 240,
	480<<16		| 320,
	640<<16		| 480,
	800<<16		| 600,
	1024<<16	| 768,
	1280<<16	| 960,
	1440<<16	| 1050,
	1440<<16	| 1080,
	1600<<16	| 1200,

	/* 16:9 */
	640<<16		| 360,
	960<<16		| 540,
	1280<<16	| 720,
	1600<<16	| 900,
	1920<<16	| 1080,
	1920<<16	| 1200,

	/* tv */
	432<<16		| 520,
	480<<16		| 320,
	480<<16		| 530,
	486<<16		| 440,
	576<<16		| 310,
	576<<16		| 520,
	576<<16		| 570,
	720<<16		| 576,
	1024<<16	| 576,

	0
};

/* fixed framerates as fallback */
static const int fixed_framerates[] =
{
	1<<16		| 60,
	1<<16		| 50,
	1<<16		| 30,
	1<<16		| 25,
	1<<16		| 20,
	1<<16		| 15,
	1<<16		| 10,
	1<<16		| 5,

	0
};

/*
 * start capture
 */
static int_fast32_t v4l2_start_capture(struct v4l2_data *data)
{
	enum v4l2_buf_type type;

	for (uint_fast32_t i = 0; i < data->buf_count; ++i) {
		struct v4l2_buffer buf;

		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;

		if (v4l2_ioctl(data->dev, VIDIOC_QBUF, &buf) < 0) {
			blog(LOG_ERROR, "unable to queue buffer");
			return -1;
		}
	}

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (v4l2_ioctl(data->dev, VIDIOC_STREAMON, &type) < 0) {
		blog(LOG_ERROR, "unable to start stream");
		return -1;
	}

	return 0;
}

/*
 * stop capture
 */
static int_fast32_t v4l2_stop_capture(struct v4l2_data *data)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if (v4l2_ioctl(data->dev, VIDIOC_STREAMOFF, &type) < 0) {
		blog(LOG_ERROR, "unable to stop stream");
	}

	return 0;
}

/**
 * Create memory mapping for buffers
 *
 * This tries to map at least 2, preferably 4, buffers to userspace.
 *
 * @return 0 on success, -1 on failure
 */
static int_fast32_t v4l2_create_mmap(struct v4l2_data *data)
{
	struct v4l2_requestbuffers req;

	req.count = 4;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;

	if (v4l2_ioctl(data->dev, VIDIOC_REQBUFS, &req) < 0) {
		blog(LOG_ERROR, "Request for buffers failed !");
		return -1;
	}

	if (req.count < 2) {
		blog(LOG_ERROR, "Device returned less than 2 buffers");
		return -1;
	}

	data->buf_count = req.count;
	data->buf = bzalloc(req.count * sizeof(struct v4l2_buffer_data));

	for (uint_fast32_t i = 0; i < req.count; ++i) {
		struct v4l2_buffer buf;

		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;

		if (v4l2_ioctl(data->dev, VIDIOC_QUERYBUF, &buf) < 0) {
			blog(LOG_ERROR, "Failed to query buffer details");
			return -1;
		}

		data->buf[i].length = buf.length;
		data->buf[i].start = v4l2_mmap(NULL, buf.length,
			PROT_READ | PROT_WRITE, MAP_SHARED,
			data->dev, buf.m.offset);

		if (data->buf[i].start == MAP_FAILED) {
			blog(LOG_ERROR, "mmap for buffer failed");
			return -1;
		}
	}

	return 0;
}

/**
 * Destroy memory mapping for buffers
 */
static void v4l2_destroy_mmap(struct v4l2_data *data)
{
	for(uint_fast32_t i = 0; i < data->buf_count; ++i) {
		if (data->buf[i].start != MAP_FAILED)
			v4l2_munmap(data->buf[i].start, data->buf[i].length);
	}

	data->buf_count = 0;
	bfree(data->buf);
}

/**
 * Prepare the output frame structure for obs and compute plane offsets
 *
 * Basically all data apart from memory pointers and the timestamp is known
 * before the capture starts. This function prepares the source_frame struct
 * with all the data that is already known.
 *
 * v4l2 uses a continuous memory segment for all planes so we simply compute
 * offsets to add to the start address in order to give obs the correct data
 * pointers for the individual planes.
 */
static void v4l2_prep_obs_frame(struct v4l2_data *data,
	struct source_frame *frame, size_t *plane_offsets)
{
	memset(frame, 0, sizeof(struct source_frame));
	memset(plane_offsets, 0, sizeof(size_t) * MAX_AV_PLANES);

	frame->width = data->width;
	frame->height = data->height;
	frame->format = v4l2_to_obs_video_format(data->pixfmt);
	video_format_get_parameters(VIDEO_CS_DEFAULT, VIDEO_RANGE_PARTIAL,
		frame->color_matrix, frame->color_range_min,
		frame->color_range_max);

	switch(data->pixfmt) {
	case V4L2_PIX_FMT_NV12:
		frame->linesize[0] = data->linesize;
		frame->linesize[1] = data->linesize / 2;
		plane_offsets[1] = data->linesize * data->height;
		break;
	case V4L2_PIX_FMT_YVU420:
		frame->linesize[0] = data->linesize;
		frame->linesize[1] = data->linesize / 2;
		frame->linesize[2] = data->linesize / 2;
		plane_offsets[1] = data->linesize * data->height * 5 / 4;
		plane_offsets[2] = data->linesize * data->height;
		break;
	case V4L2_PIX_FMT_YUV420:
		frame->linesize[0] = data->linesize;
		frame->linesize[1] = data->linesize / 2;
		frame->linesize[2] = data->linesize / 2;
		plane_offsets[1] = data->linesize * data->height;
		plane_offsets[2] = data->linesize * data->height * 5 / 4;
		break;
	default:
		frame->linesize[0] = data->linesize;
		break;
	}
}

/*
 * Worker thread to get video data
 */
static void *v4l2_thread(void *vptr)
{
	V4L2_DATA(vptr);
	int r;
	fd_set fds;
	uint8_t *start;
	struct timeval tv;
	struct v4l2_buffer buf;
	struct source_frame out;
	size_t plane_offsets[MAX_AV_PLANES];

	if (v4l2_start_capture(data) < 0)
		goto exit;

	data->frames = 0;

	FD_ZERO(&fds);
	FD_SET(data->dev, &fds);

	v4l2_prep_obs_frame(data, &out, plane_offsets);

	while (os_event_try(data->event) == EAGAIN) {
		tv.tv_sec = 1;
		tv.tv_usec = 0;

		r = select(data->dev + 1, &fds, NULL, NULL, &tv);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			blog(LOG_DEBUG, "select failed");
			break;
		} else if (r == 0) {
			blog(LOG_DEBUG, "select timeout");
			continue;
		}

		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;

		if (v4l2_ioctl(data->dev, VIDIOC_DQBUF, &buf) < 0) {
			if (errno == EAGAIN)
				continue;
			blog(LOG_DEBUG, "failed to dequeue buffer");
			break;
		}

		out.timestamp = timeval2ns(buf.timestamp);
		start = (uint8_t *) data->buf[buf.index].start;
		for (uint_fast32_t i = 0; i < MAX_AV_PLANES; ++i)
			out.data[i] = start + plane_offsets[i];
		obs_source_output_video(data->source, &out);

		if (v4l2_ioctl(data->dev, VIDIOC_QBUF, &buf) < 0) {
			blog(LOG_DEBUG, "failed to enqueue buffer");
			break;
		}

		data->frames++;
	}

	blog(LOG_INFO, "Stopped capture after %"PRIu64" frames", data->frames);

exit:
	v4l2_stop_capture(data);
	return NULL;
}

static const char* v4l2_getname(void)
{
	return obs_module_text("V4L2Input");
}

static void v4l2_defaults(obs_data_t settings)
{
	obs_data_set_default_int(settings, "pixelformat", V4L2_PIX_FMT_YUYV);
	obs_data_set_default_int(settings, "resolution",
			pack_tuple(640, 480));
	obs_data_set_default_int(settings, "framerate", pack_tuple(1, 30));
}

/*
 * List available devices
 */
static void v4l2_device_list(obs_property_t prop, obs_data_t settings)
{
	DIR *dirp;
	struct dirent *dp;
	int fd;
	struct v4l2_capability video_cap;
	struct dstr device;
	bool first = true;

	dirp = opendir("/sys/class/video4linux");
	if (!dirp)
		return;

	obs_property_list_clear(prop);

	dstr_init_copy(&device, "/dev/");

	while ((dp = readdir(dirp)) != NULL) {
		if (dp->d_type == DT_DIR)
			continue;

		dstr_resize(&device, 5);
		dstr_cat(&device, dp->d_name);

		if ((fd = v4l2_open(device.array, O_RDWR | O_NONBLOCK)) == -1) {
			blog(LOG_INFO, "Unable to open %s", device.array);
			continue;
		}

		if (v4l2_ioctl(fd, VIDIOC_QUERYCAP, &video_cap) == -1) {
			blog(LOG_INFO, "Failed to query capabilities for %s",
			     device.array);
		} else if (video_cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) {
			obs_property_list_add_string(prop,
					(char *) video_cap.card,
					device.array);
			if (first) {
				obs_data_setstring(settings,
					"device_id", device.array);
				first = false;
			}
			blog(LOG_INFO, "Found device '%s' at %s",
			     video_cap.card, device.array);
		}
		else {
			blog(LOG_INFO, "%s seems to not support video capture",
			     device.array);
		}

		close(fd);
	}

	closedir(dirp);
	dstr_free(&device);
}

/*
 * List formats for device
 */
static void v4l2_format_list(int dev, obs_property_t prop)
{
	struct v4l2_fmtdesc fmt;
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.index = 0;
	struct dstr buffer;
	dstr_init(&buffer);

	obs_property_list_clear(prop);

	while (v4l2_ioctl(dev, VIDIOC_ENUM_FMT, &fmt) == 0) {
		dstr_copy(&buffer, (char *) fmt.description);
		if (fmt.flags & V4L2_FMT_FLAG_EMULATED)
			dstr_cat(&buffer, " (Emulated)");

		if (v4l2_to_obs_video_format(fmt.pixelformat)
				!= VIDEO_FORMAT_NONE) {
			obs_property_list_add_int(prop, buffer.array,
					fmt.pixelformat);
			blog(LOG_INFO, "Pixelformat: %s (available)",
			     buffer.array);
		} else {
			blog(LOG_INFO, "Pixelformat: %s (unavailable)",
			     buffer.array);
		}
		fmt.index++;
	}

	dstr_free(&buffer);
}

/*
 * List resolutions for device and format
 */
static void v4l2_resolution_list(int dev, uint_fast32_t pixelformat,
		obs_property_t prop)
{
	struct v4l2_frmsizeenum frmsize;
	frmsize.pixel_format = pixelformat;
	frmsize.index = 0;
	struct dstr buffer;
	dstr_init(&buffer);

	obs_property_list_clear(prop);

	v4l2_ioctl(dev, VIDIOC_ENUM_FRAMESIZES, &frmsize);

	switch(frmsize.type) {
	case V4L2_FRMSIZE_TYPE_DISCRETE:
		while (v4l2_ioctl(dev, VIDIOC_ENUM_FRAMESIZES, &frmsize) == 0) {
			dstr_printf(&buffer, "%dx%d", frmsize.discrete.width,
					frmsize.discrete.height);
			obs_property_list_add_int(prop, buffer.array,
					pack_tuple(frmsize.discrete.width,
					frmsize.discrete.height));
			frmsize.index++;
		}
		break;
	default:
		blog(LOG_INFO, "Stepwise and Continuous framesizes "
			"are currently hardcoded");

		for (uint_fast32_t i = 0; ; ++i) {
			int packed = fixed_framesizes[i];
			if (!packed)
				break;
			int width;
			int height;
			unpack_tuple(&width, &height, packed);
			dstr_printf(&buffer, "%dx%d", width, height);
			obs_property_list_add_int(prop, buffer.array, packed);
		}
		break;
	}

	dstr_free(&buffer);
}

/*
 * List framerates for device and resolution
 */
static void v4l2_framerate_list(int dev, uint_fast32_t pixelformat,
		uint_fast32_t width, uint_fast32_t height, obs_property_t prop)
{
	struct v4l2_frmivalenum frmival;
	frmival.pixel_format = pixelformat;
	frmival.width = width;
	frmival.height = height;
	frmival.index = 0;
	struct dstr buffer;
	dstr_init(&buffer);

	obs_property_list_clear(prop);

	v4l2_ioctl(dev, VIDIOC_ENUM_FRAMEINTERVALS, &frmival);

	switch(frmival.type) {
	case V4L2_FRMIVAL_TYPE_DISCRETE:
		while (v4l2_ioctl(dev, VIDIOC_ENUM_FRAMEINTERVALS,
				&frmival) == 0) {
			float fps = (float) frmival.discrete.denominator /
				frmival.discrete.numerator;
			int pack = pack_tuple(frmival.discrete.numerator,
					frmival.discrete.denominator);
			dstr_printf(&buffer, "%.2f", fps);
			obs_property_list_add_int(prop, buffer.array, pack);
			frmival.index++;
		}
		break;
	default:
		blog(LOG_INFO, "Stepwise and Continuous framerates "
			"are currently hardcoded");
		for (uint_fast32_t i = 0; ; ++i) {
			int packed = fixed_framerates[i];
			if (!packed)
				break;
			int num;
			int denom;
			unpack_tuple(&num, &denom, packed);
			float fps = (float) denom / num;
			dstr_printf(&buffer, "%.2f", fps);
			obs_property_list_add_int(prop, buffer.array, packed);
		}
		break;
	}

	dstr_free(&buffer);
}

/*
 * Device selected callback
 */
static bool device_selected(obs_properties_t props, obs_property_t p,
		obs_data_t settings)
{
	UNUSED_PARAMETER(p);
	int dev = v4l2_open(obs_data_getstring(settings, "device_id"),
			O_RDWR | O_NONBLOCK);
	if (dev == -1)
		return false;

	obs_property_t prop = obs_properties_get(props, "pixelformat");
	v4l2_format_list(dev, prop);
	obs_property_modified(prop, settings);
	v4l2_close(dev);
	return true;
}

/*
 * Format selected callback
 */
static bool format_selected(obs_properties_t props, obs_property_t p,
		obs_data_t settings)
{
	UNUSED_PARAMETER(p);
	int dev = v4l2_open(obs_data_getstring(settings, "device_id"),
			O_RDWR | O_NONBLOCK);
	if (dev == -1)
		return false;

	obs_property_t prop = obs_properties_get(props, "resolution");
	v4l2_resolution_list(dev, obs_data_getint(settings, "pixelformat"),
			prop);
	obs_property_modified(prop, settings);
	v4l2_close(dev);
	return true;
}

/*
 * Resolution selected callback
 */
static bool resolution_selected(obs_properties_t props, obs_property_t p,
		obs_data_t settings)
{
	UNUSED_PARAMETER(p);
	int width, height;
	int dev = v4l2_open(obs_data_getstring(settings, "device_id"),
			O_RDWR | O_NONBLOCK);
	if (dev == -1)
		return false;

	obs_property_t prop = obs_properties_get(props, "framerate");
	unpack_tuple(&width, &height, obs_data_getint(settings,
				"resolution"));
	v4l2_framerate_list(dev, obs_data_getint(settings, "pixelformat"),
			width, height, prop);
	obs_property_modified(prop, settings);
	v4l2_close(dev);
	return true;
}

static obs_properties_t v4l2_properties(void)
{
	/* TODO: locale */
	obs_properties_t props = obs_properties_create();

	obs_property_t device_list = obs_properties_add_list(props,
			"device_id", obs_module_text("Device"),
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	obs_property_t format_list = obs_properties_add_list(props,
			"pixelformat", obs_module_text("VideoFormat"),
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

	obs_property_t resolution_list = obs_properties_add_list(props,
			"resolution", obs_module_text("Resolution"),
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

	obs_properties_add_list(props,
			"framerate", obs_module_text("FrameRate"),
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

	v4l2_device_list(device_list, NULL);
	obs_property_set_modified_callback(device_list, device_selected);
	obs_property_set_modified_callback(format_list, format_selected);
	obs_property_set_modified_callback(resolution_list,
			resolution_selected);
	return props;
}

static void v4l2_terminate(struct v4l2_data *data)
{
	if (data->thread) {
		os_event_signal(data->event);
		pthread_join(data->thread, NULL);
		os_event_destroy(data->event);
	}

	if (data->buf_count)
		v4l2_destroy_mmap(data);

	if (data->dev != -1) {
		v4l2_close(data->dev);
		data->dev = -1;
	}
}

static void v4l2_destroy(void *vptr)
{
	V4L2_DATA(vptr);

	if (!data)
		return;

	v4l2_terminate(data);

	if (data->set_device)
		bfree(data->set_device);
	bfree(data);
}

/**
 * Initialize the v4l2 device
 *
 * This function:
 * - tries to open the device
 * - sets pixelformat and requested resolution
 * - sets the requested framerate
 * - maps the buffers
 * - starts the capture thread
 */
static void v4l2_init(struct v4l2_data *data)
{
	struct v4l2_format fmt;
	struct v4l2_streamparm par;
	struct dstr fps;
	int width, height;
	int fps_num, fps_denom;

	blog(LOG_INFO, "Start capture from %s", data->set_device);
	data->dev = v4l2_open(data->set_device, O_RDWR | O_NONBLOCK);
	if (data->dev == -1) {
		blog(LOG_ERROR, "Unable to open device");
		goto fail;
	}

	/* set pixel format and resolution */
	unpack_tuple(&width, &height, data->set_res);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = width;
	fmt.fmt.pix.height = height;
	fmt.fmt.pix.pixelformat = data->set_pixfmt;
	fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;
	if (v4l2_ioctl(data->dev, VIDIOC_S_FMT, &fmt) < 0) {
		blog(LOG_ERROR, "Unable to set format");
		goto fail;
	}
	data->width = fmt.fmt.pix.width;
	data->height = fmt.fmt.pix.height;
	data->pixfmt = fmt.fmt.pix.pixelformat;
	data->linesize = fmt.fmt.pix.bytesperline;
	blog(LOG_INFO, "Resolution: %"PRIuFAST32"x%"PRIuFAST32,
	     data->width, data->height);
	blog(LOG_INFO, "Linesize: %"PRIuFAST32" Bytes", data->linesize);

	/* set framerate */
	unpack_tuple(&fps_num, &fps_denom, data->set_fps);
	par.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	par.parm.capture.timeperframe.numerator = fps_num;
	par.parm.capture.timeperframe.denominator = fps_denom;
	if (v4l2_ioctl(data->dev, VIDIOC_S_PARM, &par) < 0) {
		blog(LOG_ERROR, "Unable to set framerate");
		goto fail;
	}
	dstr_init(&fps);
	dstr_printf(&fps, "%.2f",
		(float) par.parm.capture.timeperframe.denominator
		/ par.parm.capture.timeperframe.numerator);
	blog(LOG_INFO, "Framerate: %s fps", fps.array);
	dstr_free(&fps);

	/* map buffers */
	if (v4l2_create_mmap(data) < 0) {
		blog(LOG_ERROR, "Failed to map buffers");
		goto fail;
	}

	/* start the capture thread */
	if (os_event_init(&data->event, OS_EVENT_TYPE_MANUAL) != 0)
		goto fail;
	if (pthread_create(&data->thread, NULL, v4l2_thread, data) != 0)
		goto fail;
	return;
fail:
	blog(LOG_ERROR, "Initialization failed");
	v4l2_terminate(data);
}

static void v4l2_update(void *vptr, obs_data_t settings)
{
	V4L2_DATA(vptr);
	bool restart = false;
	const char *new_device;

	new_device = obs_data_getstring(settings, "device_id");
	if (strlen(new_device) == 0) {
		v4l2_device_list(NULL, settings);
		new_device = obs_data_getstring(settings, "device_id");
	}

	if (!data->set_device || strcmp(data->set_device, new_device) != 0) {
		if (data->set_device)
			bfree(data->set_device);
		data->set_device = bstrdup(new_device);
		restart = true;
	}

	if (data->set_pixfmt != obs_data_getint(settings, "pixelformat")) {
		data->set_pixfmt = obs_data_getint(settings, "pixelformat");
		restart = true;
	}

	if (data->set_res != obs_data_getint(settings, "resolution")) {
		data->set_res = obs_data_getint(settings, "resolution");
		restart = true;
	}

	if (data->set_fps != obs_data_getint(settings, "framerate")) {
		data->set_fps = obs_data_getint(settings, "framerate");
		restart = true;
	}

	if (restart) {
		v4l2_terminate(data);
		v4l2_init(data);
	}
}

static void *v4l2_create(obs_data_t settings, obs_source_t source)
{
	UNUSED_PARAMETER(settings);

	struct v4l2_data *data = bzalloc(sizeof(struct v4l2_data));
	data->dev = -1;
	data->source = source;

	v4l2_update(data, settings);

	return data;
}

struct obs_source_info v4l2_input = {
	.id           = "v4l2_input",
	.type         = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO,
	.getname      = v4l2_getname,
	.create       = v4l2_create,
	.destroy      = v4l2_destroy,
	.update       = v4l2_update,
	.defaults     = v4l2_defaults,
	.properties   = v4l2_properties
};
