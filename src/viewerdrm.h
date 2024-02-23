#ifndef __VIEWER_DRM_H__
#define __VIEWER_DRM_H__

#define FOURCC(a,b,c,d)	((a << 0) | (b << 8) | (c << 16) | (d << 24))

typedef struct DisplayConf_s DisplayConf_t;
struct DisplayConf_s
{
	uint32_t width;
	uint32_t height;
	uint32_t fourcc;
};

typedef struct Display_s Display_t;

Display_t *display_create(const char *name, DisplayConf_t *config);
void *display_getbuf(Display_t *disp, void **image, size_t *size);
int display_flushbuf(Display_t *disp, void* id);
void display_destroy(Display_t *disp);

#endif
