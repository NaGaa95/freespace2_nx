/*
 * nxmain.cpp - Nintendo Switch entry point. Brings up libnx services, points the
 * working directory at the game data, then calls the shared WinMain().
 */

#ifdef SWITCH

#include <switch.h>

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "SDL.h"

#include "pstypes.h"
#include "osregistry.h"
#include "osapi.h"

#undef malloc
#undef free

int PASCAL WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR szCmdLine, int nCmdShow);

void vm_dump();

#define NX_DEFAULT_DATA_DIR	"sdmc:/switch/freespace2"

// chdir to the folder holding the data; from hbmenu argv[0] is the .nro path
static void nx_set_working_directory(const char *argv0)
{
	char dir[MAX_PATH] = {0};

	if (argv0 != NULL) {
		strncpy(dir, argv0, sizeof(dir) - 1);

		char *slash = strrchr(dir, '/');
		if (slash != NULL) {
			*slash = '\0';
			if (chdir(dir) == 0)
				return;
		}
	}

	chdir(NX_DEFAULT_DATA_DIR);
}

// optional extra command-line switches from cmdline.cfg in the data directory
static char *nx_read_cmdline(void)
{
	FILE *fp = fopen("cmdline.cfg", "r");
	if (fp == NULL)
		return NULL;

	static char buf[1024] = {0};
	if (fgets(buf, sizeof(buf) - 1, fp) == NULL)
		buf[0] = '\0';
	fclose(fp);

	size_t len = strlen(buf);
	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
		buf[--len] = '\0';

	return buf;
}

int main(int argc, char **argv)
{
	socketInitialize(socketGetDefaultInitConfig());	// BSD sockets for psnet

	romfsInit();

	nx_set_working_directory(argc > 0 ? argv[0] : NULL);

	char *cmdline = nx_read_cmdline();

	int retr = WinMain(1, 0, cmdline ? cmdline : (char *)"", 0);

	vm_dump();

	romfsExit();
	socketExit();

	return retr;
}

#endif // SWITCH
