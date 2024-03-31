#include "rtlsdr_sink.h"
#include <signal.h>

static rtl_sink * sink = NULL;

static void sighandler(int signum)
{
	fprintf(stderr, "Signal caught, exiting!\n");
    if (sink != NULL) {
        stop_rtl_sink(sink);
        free_rtl_sink(&sink);
    }

    exit(0);
}


int main(int argc, char **argv) {
    sink = init_rtl_sink("127.0.0.1", "1234", stdout_logger);

	struct sigaction sigact, sigign;
    sigact.sa_handler = sighandler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigign.sa_handler = SIG_IGN;
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGQUIT, &sigact, NULL);
	sigaction(SIGPIPE, &sigign, NULL);

    start_rtl_sink(sink);
    #define DEFAULT_SAMPLE_RATE_HZ 2048000
    void *buf = malloc(2 * DEFAULT_SAMPLE_RATE_HZ);
    for(int i =0; i< DEFAULT_SAMPLE_RATE_HZ; i++) {
        ((unsigned char*)buf)[2*i] = i % 128;
        ((unsigned char*)buf)[2*i + 1] = i % 127;
    }

    for(;;) {
        push_iq_paris(sink, buf, 2 * DEFAULT_SAMPLE_RATE_HZ);
        sleep(1);
    }
    return 0;
}
