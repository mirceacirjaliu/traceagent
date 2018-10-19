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
#include <syslog.h>
#include <assert.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

static bool interrupted = false;
static unsigned long progress = 0;
static unsigned long useful = 0;
static bool missed = false;

void intHandler(int signum, siginfo_t *info, void *ptr)
{
	syslog(LOG_INFO, "Interrupted...\n");
	interrupted = true;
}

void urgHandler(int signum, siginfo_t *info, void *ptr)
{
	syslog(LOG_ERR, "socket emergency: %s\n", strerror(errno));
}

void pollHandler(int signum, siginfo_t *info, void *ptr)
{
	float ratio = (float) useful / (float) progress;
	
	syslog(LOG_ERR, "progress: %lu/%lu bytes = %f, missed %s \n", 
		useful, progress, ratio, missed ? "true" : "false");
}

void skeleton_daemon()
{
    pid_t pid;

    /* Fork off the parent process */
    pid = fork();

    /* An error occurred */
    if (pid < 0)
        exit(EXIT_FAILURE);

    /* Success: Let the parent terminate */
    if (pid > 0)
        exit(EXIT_SUCCESS);

    /* On success: The child process becomes session leader */
    if (setsid() < 0)
        exit(EXIT_FAILURE);

    /* Catch, ignore and handle signals */
    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    /* Fork off for the second time*/
    pid = fork();

    /* An error occurred */
    if (pid < 0)
        exit(EXIT_FAILURE);

    /* Success: Let the parent terminate */
    if (pid > 0)
        exit(EXIT_SUCCESS);

    /* Set new file permissions */
    umask(0);

    /* Change the working directory to the root directory */
    /* or another appropriated directory */
    chdir("/");

    /* Close all open file descriptors */
    int x;
    for (x = sysconf(_SC_OPEN_MAX); x>=0; x--)
    {
        close (x);
    }

    /* Open the log file */
    openlog (NULL, LOG_PID, LOG_DAEMON);
}

#define RB_MISSED_EVENTS	(1 << 31)
#define RB_MISSED_STORED	(1 << 30)	/* Missed count stored at end */
#define RB_MISSED_FLAGS		(RB_MISSED_EVENTS | RB_MISSED_STORED)

struct trace_page_header
{
	unsigned long timestamp;
	unsigned long commit;
};

int main(int argc, char *argv[])
{
	int from = -1;
	int to = -1;
	int result = EXIT_SUCCESS;
	size_t toRead;
	ssize_t readData;
	ssize_t sizeWritten = 0;
	loff_t off_in = 0;
	loff_t off_out = 0;
	void *buf = NULL;
	struct sigaction intAct;
	struct sigaction urgAct;
	struct sigaction pollAct;
	char *port;
	struct hostent *host;
	struct sockaddr_in serv_addr;
	struct trace_page_header *head;
	
	assert(sizeof(unsigned long) == 8);
	
	if (argc != 3) {
		fprintf(stderr, "Usage: %s <from> <to>\n", argv[0]);
		return EXIT_FAILURE;
	}
	
	skeleton_daemon();
	
	from = open(argv[1], O_RDONLY);
	if (from == -1) {
		syslog(LOG_ERR, "%s: %s\n", strerror(errno), argv[1]);
		result = EXIT_FAILURE;
		goto exit;
	}
	
	port = strchr(argv[2], ':');
	if (port != NULL) {
		*port = 0;
		port++;

		host = gethostbyname(argv[2]);
		if (host == NULL) {
			syslog(LOG_ERR, "looking up host %s: %s\n", argv[2], strerror(errno));
			result = EXIT_FAILURE;
			goto exit;
		}
		
		memset(&serv_addr, 0, sizeof(serv_addr));
		serv_addr.sin_family = AF_INET;
		memcpy(&serv_addr.sin_addr.s_addr, host->h_addr, host->h_length);
	    serv_addr.sin_port = htons(atoi(port));

		syslog(LOG_DEBUG, "found server: %s at address %s\n", host->h_name, inet_ntoa(serv_addr.sin_addr));
		
		to = socket(AF_INET, SOCK_STREAM, 0);
		if (to == -1) {
			syslog(LOG_ERR, "creating socket %s: %s\n", argv[2], strerror(errno));
			result = EXIT_FAILURE;
			goto exit;
       	}

		result = connect(to, &serv_addr, sizeof(serv_addr));
		if (result == -1) {
			syslog(LOG_ERR, "connecting to %s: %s\n", argv[2], strerror(errno));
			result = EXIT_FAILURE;
			goto exit;
		}
	}
	else {
		to = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0666);
	 	if (to == -1) {
			syslog(LOG_ERR, "opening file %s: %s\n", argv[2], strerror(errno));
			result = EXIT_FAILURE;
			goto exit;
		}
	}
	
	buf = malloc(getpagesize());
	if (buf == NULL) {
		syslog(LOG_ERR, "allocating buffer: %s\n", strerror(errno));
		result = EXIT_FAILURE;
		goto exit;
	}
	head = (struct trace_page_header *) buf;
	
	intAct.sa_sigaction = intHandler;
	intAct.sa_flags = SA_SIGINFO;
	sigaction(SIGINT, &intAct, NULL);
	
	urgAct.sa_sigaction = urgHandler;
	urgAct.sa_flags = SA_SIGINFO;
	sigaction(SIGURG, &urgAct, NULL);
	
	pollAct.sa_sigaction = pollHandler;
	pollAct.sa_flags = SA_SIGINFO;
	sigaction(SIGIO, &pollAct, NULL);
	
	/* SE PARE CA SPLICE-U MERGE DOAR INTRE PIPE */
	//readData = splice(from, &off_in, to, &off_out, getpagesize(), SPLICE_F_MOVE);
	
	// interrupt loop at page granularity
	do {
		// prepare for new page
		toRead = getpagesize();
		readData = 0;
		
		do {
			// reading may be interrupted => less bytes received
			// abort reading (drop page) if SIGINT
			result = read(from, buf + readData, toRead - readData);
			if (result == -1) {
				if (errno == EINTR)
					if (!interrupted)
						continue;	// inner loop
					else
						break;		// inner loop
				
				syslog(LOG_INFO, "reading stopped: %s\n", strerror(errno));
				result = EXIT_FAILURE;
				goto exit;
			}
			else
				readData += result;
		} while (readData < toRead);
		
		// abort reading (drop page) if SIGINT
		if (interrupted)
			break;			// main loop
		
		// log useful bytes in this page
		useful += head->commit & ~RB_MISSED_FLAGS;
		missed |= !!(head->commit & RB_MISSED_EVENTS);
		
		// prepare for new write
		sizeWritten = 0;
		
		do {
			// finish sending even if SIGINT, then end the loop
			result = write(to, buf + sizeWritten, readData - sizeWritten);
			if (result == -1) {
				if (errno == EINTR)
					continue;	// inner loop
				
				syslog(LOG_INFO, "writing stopped: %s\n", strerror(errno));
				result = EXIT_FAILURE;
				goto exit;
			}
			else
				sizeWritten += result;
		} while (sizeWritten < readData);
		
		// log readData data
		progress += readData;
		
		fsync(to);
	} while (!interrupted);

exit:
	if (buf != NULL)
		free(buf);

	if (from != -1)
		close(from);
	
	if (to != -1)
		close(to);

	syslog(LOG_DEBUG, "exiting\n");

	return result;
}
