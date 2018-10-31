#include <stdio.h>
#define __USE_GNU
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <syslog.h>
#include <assert.h>

const char *marker = "/sys/kernel/debug/tracing/trace_marker_raw";

int main(int argc, char *argv[])
{
	int result = EXIT_SUCCESS;
	struct timeval tv;
	struct timezone tz;
	int markerFd = -1;
	
	markerFd = open(marker, O_WRONLY);
	if (markerFd == -1) {
		syslog(LOG_ERR, "%s: %s\n", strerror(errno), marker);
		result = EXIT_FAILURE;
		goto exit;
	}
	
	result = gettimeofday(&tv, &tz);	
	if (result == -1) {
		syslog(LOG_ERR, "%s: %s\n", strerror(errno), "gettimeofday()");
		result = EXIT_FAILURE;
		goto exit;
	}
	
	result = write(markerFd, &tv, sizeof(struct timeval));
	if (result != sizeof(struct timeval)) {
		syslog(LOG_ERR, "%s: %s\n", strerror(errno), "write()");
		result = EXIT_FAILURE;
		goto exit;
	}
	
	// reset result value
	result = EXIT_SUCCESS;
	
exit:
	if (markerFd != -1)
		close(markerFd);
	
	return result;
}
