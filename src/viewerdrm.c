#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm.h>
#include <drm_fourcc.h>

#include "log.h"
#include "viewerdrm.h"

#define MAX_BUFFERS 4

typedef struct DisplayFormat_s DisplayFormat_t;
struct DisplayFormat_s
{
	union
	{
		uint32_t	drm;
		uint32_t	fourcc;
	} type;
	int		depth;
} g_formats[] =
{
	{
		.type.drm = DRM_FORMAT_ARGB8888,
		.depth = 4,
	},
	{0}
};

typedef struct DisplayBuffer_s DisplayBuffer_t;
struct DisplayBuffer_s
{
	int bo_handle;
	int dma_fd;
	uint32_t fb_id;
	uint32_t *memory;
	uint32_t pitch;
	uint32_t size;
};

typedef struct Display_s Display_t;
struct Display_s
{
	uint32_t connector_id;
	uint32_t encoder_id;
	uint32_t crtc_id;
	uint32_t plane_id;
	drmModeCrtc *crtc;
	DisplayFormat_t *format;
	int fd;
	drmModeModeInfo mode;
	DisplayBuffer_t buffers[MAX_BUFFERS];
	int buf_id;
};

static int display_ids(Display_t *disp, uint32_t *conn_id, uint32_t *enc_id, uint32_t *crtc_id, drmModeModeInfo *mode)
{
	drmModeResPtr resources;
	resources = drmModeGetResources(disp->fd);

	uint32_t connector_id = 0;
	for(int i = 0; i < resources->count_connectors; ++i)
	{
		connector_id = resources->connectors[i];
		drmModeConnectorPtr connector = drmModeGetConnector(disp->fd, connector_id);
		if (connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0)
		{
			drmModeModeInfo *preferred = NULL;
			if (mode->hdisplay && mode->vdisplay)
				preferred = mode;
			*conn_id = connector_id;
			for (int m = 0; m < connector->count_modes; m++)
			{
				dbg("mode: %dx%d %s",
						connector->modes[m].hdisplay,
						connector->modes[m].vdisplay,
						connector->modes[m].type & DRM_MODE_TYPE_PREFERRED ? "*" : "");
				if (!preferred && connector->modes[m].type & DRM_MODE_TYPE_PREFERRED)
				{
					preferred = &connector->modes[m];
				}
				if (preferred && connector->modes[m].hdisplay == preferred->hdisplay &&
								connector->modes[m].vdisplay == preferred->vdisplay)
				{
					preferred = &connector->modes[m];
				}
			}
			if (preferred == NULL || preferred == mode)
				preferred = &connector->modes[0];
			memcpy(mode, preferred, sizeof(*mode));
			*enc_id = connector->encoder_id;
			drmModeFreeConnector(connector);
			break;
		}
		drmModeFreeConnector(connector);
	}

	if (*conn_id == 0 || *enc_id == 0)
	{
		drmModeFreeResources(resources);
		return -1;
	}

	for(int i=0; i < resources->count_encoders; ++i)
	{
		drmModeEncoderPtr encoder;
		encoder = drmModeGetEncoder(disp->fd, resources->encoders[i]);
		if(encoder != NULL)
		{
			dbg("encoder %d found", encoder->encoder_id);
			if(encoder->encoder_id == *enc_id)
			{
				*crtc_id = encoder->crtc_id;
				drmModeFreeEncoder(encoder);
				break;
			}
			drmModeFreeEncoder(encoder);
		}
		else
			err("get a null encoder pointer");
	}

	int crtcindex = -1;
	for(int i=0; i < resources->count_crtcs; ++i)
	{
		if (resources->crtcs[i] == *crtc_id)
		{
			crtcindex = i;
			break;
		}
	}
	if (crtcindex == -1)
	{
		drmModeFreeResources(resources);
		err("crtc mot available");
		return -1;
	}
	dbg("disp: screen size %ux%u", disp->mode.hdisplay, disp->mode.vdisplay);
	drmModeFreeResources(resources);
	return 0;
}

static int display_plane(Display_t *disp, uint32_t *plane_id)
{
	drmModePlaneResPtr planes;

	planes = drmModeGetPlaneResources(disp->fd);

	drmModePlanePtr plane;
	for (int i = 0; i < planes->count_planes; ++i)
	{
		int found = 0;
		plane = drmModeGetPlane(disp->fd, planes->planes[i]);
		for (int j = 0; j < plane->count_formats; ++j)
		{
			uint32_t fourcc = plane->formats[j];
			dbg("Plane[%d] %u: 4cc %c%c%c%c", i, plane->plane_id,
					fourcc,
					fourcc >> 8,
					fourcc >> 16,
					fourcc >> 24);
			if (plane->formats[j] == disp->format->type.fourcc)
			{
				found = 1;
				break;
			}
		}
		*plane_id = plane->plane_id;
		drmModeFreePlane(plane);
		if (found)
			break;
	}
	drmModeFreePlaneResources(planes);
	return 0;
}

static int display_buffer(Display_t *disp, uint32_t width, uint32_t height, uint32_t size, uint32_t fourcc, DisplayBuffer_t *buffer)
{
	buffer->size = size;
	buffer->pitch = size / height;

	struct drm_mode_create_dumb gem = {
		.width = width,
		.height = height,
		.size = size,
		.bpp = 32,
	};
	if (drmIoctl(disp->fd, DRM_IOCTL_MODE_CREATE_DUMB, &gem) == -1)
	{
		err("dumb allocation error %m");
		return -1;
	}

	buffer->bo_handle = gem.handle;

	struct drm_mode_map_dumb map = {
		.handle = gem.handle,
	};
	if (drmIoctl(disp->fd, DRM_IOCTL_MODE_MAP_DUMB, &map) == -1)
	{
		err("dumb map error %m");
		return -1;
	}
	buffer->memory = (uint32_t *)mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
		disp->fd, map.offset);

	uint32_t offsets[4] = { 0 };
	uint32_t pitches[4] = { buffer->pitch };
	uint32_t bo_handles[4] = { buffer->bo_handle };
	if (drmModeAddFB2(disp->fd, width, height, disp->format->type.fourcc, bo_handles,
		pitches, offsets, &buffer->fb_id, 0) == -1)
	{
		err("Frame buffer unavailable %m");
		return -1;
	}
	return 0;
}

static void display_freebuffer(Display_t *disp, DisplayBuffer_t *buffer)
{
	struct drm_mode_destroy_dumb dumb = {
		.handle = buffer->bo_handle,
	};
	munmap(buffer->memory, buffer->size);
	drmIoctl(disp->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dumb);
	drmModeRmFB(disp->fd, buffer->fb_id);
}

Display_t *display_create(const char *name, DisplayConf_t *config)
{
	int fd = 0;
	if (!access(name, R_OK | W_OK))
		fd = open(name, O_RDWR);
	else
		fd = drmOpen(name, NULL);
	if (fd < 0)
	{
		err("bad device argument %m");
		return NULL;
	}

	Display_t *disp = calloc(1, sizeof(*disp));
	disp->fd = fd;
	disp->format = &g_formats[0];

	disp->mode.hdisplay = config->width;
	disp->mode.vdisplay = config->height;
	if (display_ids(disp, &disp->connector_id, &disp->encoder_id, &disp->crtc_id, &disp->mode) == -1)
	{
		free(disp);
		return NULL;
	}
	if (display_plane(disp, &disp->plane_id) == -1)
	{
		free(disp);
		return NULL;
	}

	size_t size = config->width * config->height * disp->format->depth;
	for (int i = 0; i < MAX_BUFFERS; i++)
	{
		if (display_buffer(disp, config->width, config->height, size, disp->format->type.fourcc, &disp->buffers[i]) == -1)
		{
			for (int j = 0; j < i; j++)
				display_freebuffer(disp, &disp->buffers[j]);
			free(disp);
			return NULL;
		}
	}
	disp->crtc = drmModeGetCrtc(disp->fd, disp->crtc_id);
	if (drmModeSetCrtc(disp->fd, disp->crtc_id, disp->buffers[0].fb_id, 0, 0, &disp->connector_id, 1, &disp->mode))
	{
		err("Crtc setting error %m");
		for (int j = 0; j < MAX_BUFFERS; j++)
			display_freebuffer(disp, &disp->buffers[j]);
		free(disp);
		return NULL;
	}
	drmModePageFlip(disp->fd, disp->crtc_id, disp->buffers[0].fb_id, DRM_MODE_PAGE_FLIP_EVENT, disp);
	return disp;
}

void *display_getbuf(Display_t *disp, void **image, size_t *size)
{
	disp->buf_id++;
	if (disp->buf_id == MAX_BUFFERS)
		disp->buf_id = 0;
	*image = disp->buffers[disp->buf_id].memory;
	*size = disp->buffers[disp->buf_id].size;
	return (void*)disp->buf_id;
}

int display_flushbuf(Display_t *disp, void* id)
{
	drmModePageFlip(disp->fd, disp->crtc_id, disp->buffers[(int)id].fb_id, DRM_MODE_PAGE_FLIP_EVENT, disp);
	return 0;
}

void display_destroy(Display_t *disp)
{
	drmModeFreeCrtc(disp->crtc);
	for (int j = 0; j < MAX_BUFFERS; j++)
		display_freebuffer(disp, &disp->buffers[j]);
	close(disp->fd);
	free(disp);
}
