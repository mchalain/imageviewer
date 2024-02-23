#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>

#include "log.h"
#include "viewerdrm.h"

int main(int argc, char * const argv[])
{
	const char *device = "/dev/dri/card0";
	const char *image = "/dev/random";
	DisplayConf_t conf = {
		.width = 1024,
		.height = 768,
		.fourcc = FOURCC('A', 'R', '2', '4'),
	};

	int opt;
	do
	{
		opt = getopt(argc, argv, "d:i:s:");
		switch (opt)
		{
		case 'd':
			device = optarg;
		break;
		case 'i':
			image = optarg;
		break;
		case 's':
		{
			const char *second = strchr(optarg, 'x');
			if (second)
			{
				conf.width = atoi(optarg);
				conf.height = atoi(second + 1);
			}
		}
		break;
		}
	} while(opt != -1);
	
	Display_t *disp = NULL;
	disp = display_create(device, &conf);
	if (disp == NULL)
	{
		err("Display unavailable %m");
		return -1;
	}
	
	uint32_t *buffer = NULL;
	size_t size = 0;
	void *ctx = display_getbuf(disp, ((void **)&buffer), &size);
	int imgfd = open(image, O_RDONLY);
	if (imgfd == -1)
	{
		err("random device unavailable %m");
		display_destroy(disp);
		return -1;
	}
	size_t content = read(imgfd, buffer, size);
	if (content != size)
		err("random access error %m");
	close(imgfd);
	display_flushbuf(disp, ctx);
	getchar();
	display_destroy(disp);
	return 0;
}
