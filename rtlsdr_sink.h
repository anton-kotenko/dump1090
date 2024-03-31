#include <netinet/in.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/fcntl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netdb.h>
#include <errno.h>
#include <errno.h>
#include <unistd.h>
#include <stdarg.h>

#define SOCKET int
#define DEFAULT_MAX_NUM_BUFFERS 500
#define	NI_MAXHOST	1025
#define	NI_MAXSERV	32

struct llist {
	char *data;
	size_t len;
	struct llist *next;
};

typedef struct { /* structure size must be multiple of 2 bytes */
	char magic[4];
	uint32_t tuner_type;
	uint32_t tuner_gain_count;
} dongle_info_t;

struct command{
	unsigned char cmd;
	unsigned int param;
}__attribute__((packed));

typedef struct {
    pthread_mutex_t ll_mutex;
    pthread_cond_t cond;
    pthread_t main_thread;
    volatile unsigned char do_exit;

    struct llist *ll_buffers;
    int num_queued;
    
    SOCKET listensocket;
    SOCKET clientsocket;

    // 
    // Settings section
    //
    int llbuf_num;
    char * listenaddr;
    char * listenport;
    
    void (*logger)(const char *format, ...);

} rtl_sink;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
void noop_logger(const char *format, ...) {
}

#pragma clang diagnostic pop

void stdout_logger(const char *format, ...){
    int len = strlen(format);
    char * newfmt = (char*) malloc(len + 1);
    strcpy(newfmt, format);
    newfmt[len - 1] = '\n';
    newfmt[len] = '\0';
    va_list args;
    va_start(args, format);
    vfprintf(stderr, newfmt, args);
    va_end(args);
    free(newfmt);
}

void *_command_worker(void *arg);
void *_tcp_worker(void *arg);
void _handle_connection(rtl_sink* sink);
void _create_listen_socket(rtl_sink * sink);
void * _accept_worker(void * arg);

rtl_sink* init_rtl_sink(char *listenaddr, char*listenport, void (*logger)(const char *format, ...)){
    rtl_sink * sink = (rtl_sink*) malloc(sizeof(rtl_sink));
    if (sink == NULL) {
        fprintf(stderr, "can not allocate rtl sdr sink");
        exit(1);
    }

	pthread_mutex_init(&sink->ll_mutex, NULL);
	pthread_cond_init(&sink->cond, NULL);
    
    sink->do_exit = 0;

    sink->listensocket = 0;
    sink->clientsocket = 0;
    sink->ll_buffers = NULL;
    sink->num_queued = 0;
    sink->listenaddr = strdup(listenaddr);
    sink->listenport = strdup(listenport);

    sink->llbuf_num = DEFAULT_MAX_NUM_BUFFERS;
    sink->logger = logger;
    
    return sink;
}

void free_rtl_sink(rtl_sink** sink) {
    if (*sink == NULL) {
        return;
    }
    if (!(*sink)->do_exit) {
        fprintf(stderr, "freeing unstopped rtl-sink");
        exit(1);
    }

    free((*sink)->listenport);
    free((*sink)->listenaddr);

    struct llist * prev, *curelem = (*sink)->ll_buffers;

    while(curelem != NULL) {
	    prev = curelem;
	    curelem = curelem->next;
	    free(prev->data);
	    free(prev);
    }
    (*sink)->ll_buffers = NULL;

    free(*sink);
    *sink = NULL;
}


void start_rtl_sink(rtl_sink * sink) {
    _create_listen_socket(sink);
	pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    pthread_create(&sink->main_thread, &attr, _accept_worker, sink);
    pthread_attr_destroy(&attr);
}

void stop_rtl_sink(rtl_sink* sink) {
    sink->do_exit = 1;
	void *status;
    pthread_join(sink->main_thread, &status);
}

void push_iq_paris(rtl_sink* sink, unsigned char *buf, uint32_t len)
{
	if(!sink || sink->do_exit) {
        return;
    }

    struct llist *rpt = (struct llist*)malloc(sizeof(struct llist));
    rpt->data = (char*)malloc(len);
	memcpy(rpt->data, buf, len);
	rpt->len = len;
	rpt->next = NULL;

	pthread_mutex_lock(&sink->ll_mutex);

	if (sink->ll_buffers == NULL) {
	    sink->ll_buffers = rpt;
	} else {
        struct llist *cur = sink->ll_buffers;
        int num_queued = 0;

        while (cur->next != NULL) {
            cur = cur->next;
            num_queued++;
        }

        if(sink->llbuf_num && sink->llbuf_num == num_queued-2){
            struct llist *curelem;

            free(sink->ll_buffers->data);
            curelem = sink->ll_buffers->next;
            free(sink->ll_buffers);
            sink->ll_buffers = curelem;
        }

        cur->next = rpt;

        if (num_queued > sink->num_queued)
            sink->logger("ll+, now %d\n", num_queued);
        else if (num_queued < sink->num_queued)
            sink->logger("ll-, now %d\n", num_queued);

        sink->num_queued = num_queued;
    }
    pthread_cond_signal(&sink->cond);
    pthread_mutex_unlock(&sink->ll_mutex);
}

void _resolve_listen_addr(char * listenaddr, char * listenport, struct addrinfo ** aiHead) {
	struct addrinfo  hints = { 0 };
    hints.ai_flags  = AI_PASSIVE; /* Server mode. */
	hints.ai_family = PF_UNSPEC;  /* IPv4 or IPv6. */
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	int aiErr;
	if ((aiErr = getaddrinfo(listenaddr, listenport, &hints, aiHead)) != 0)
	{
		fprintf(stderr, "local address %s:%s ERROR - %s.\n", listenaddr, listenport, gai_strerror(aiErr));
        exit(1);
	}
}

void _create_listen_socket(rtl_sink * sink) {
	struct addrinfo *aiHead, *ai;
	char hostinfo[NI_MAXHOST];
	char portinfo[NI_MAXSERV];

    _resolve_listen_addr(sink->listenaddr, sink->listenport, &aiHead);
	struct sockaddr_storage local;
	memcpy(&local, aiHead->ai_addr, aiHead->ai_addrlen);

    for (ai = aiHead; ai != NULL; ai = ai->ai_next) {
		int aiErr = getnameinfo((struct sockaddr *)ai->ai_addr, ai->ai_addrlen,
				    hostinfo, NI_MAXHOST,
				    portinfo, NI_MAXSERV, NI_NUMERICSERV | NI_NUMERICHOST);
		if (aiErr)
			fprintf( stderr, "getnameinfo ERROR - %s.\n",hostinfo);
        sink->logger("resolved %s, %s, %d, %d, %d\n", hostinfo, portinfo, ai->ai_family, ai->ai_socktype, ai->ai_protocol);

		sink->listensocket = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (sink->listensocket < 0)
			continue;

        sink->logger("created socket\n");

		int r = 1;
		setsockopt(sink->listensocket, SOL_SOCKET, SO_REUSEADDR, (char *)&r, sizeof(int));
	    struct linger ling = {1,0};
		setsockopt(sink->listensocket, SOL_SOCKET, SO_LINGER, (char *)&ling, sizeof(ling));

		if (bind(sink->listensocket, (struct sockaddr *)&local, aiHead->ai_addrlen))
			fprintf(stderr, "rtl_tcp bind error: %s\n", strerror(errno));
		else
            sink->logger("nice! bind\n");
			break;
	}

    if (sink->listensocket <= 0) {
        fprintf(stderr, "was not able to create listen socket");
        exit(1);
    }

	int sockopts = fcntl(sink->listensocket, F_GETFL, 0);
	sockopts = fcntl(sink->listensocket, F_SETFL, sockopts | O_NONBLOCK);

    freeaddrinfo(aiHead);
}


void * _accept_worker(void * arg) {
    rtl_sink* sink  = (rtl_sink*) arg;
    
    sink->logger("listen socket\n");
    listen(sink->listensocket, 1);

    fd_set readfds;
    struct timeval tv = {1,0};
    struct sockaddr_storage remote;
	while(1) {
		FD_ZERO(&readfds);
		FD_SET(sink->listensocket, &readfds);
		tv.tv_sec = 1;
		tv.tv_usec = 0;
        sink->logger("accept/select\n");
		int ready_count = select(sink->listensocket+1, &readfds, NULL, NULL, &tv);
        if (sink->do_exit) {
            sink->logger("accept/exit\n");
            break;
        }
        sink->logger("select done: %d\n", ready_count);
		if(ready_count > 0) {
			socklen_t rlen = sizeof(remote);
            sink->logger("do accept\n");
			sink->clientsocket = accept(sink->listensocket,(struct sockaddr *)&remote, &rlen);

            sink->logger("go deeper\n");
            _handle_connection(sink);
		}
	}
    close(sink->listensocket);
    sink->listensocket = 0;
    return NULL;
}

void _handle_connection(rtl_sink* sink) {
	struct linger ling = {1,0};
    setsockopt(sink->clientsocket, SOL_SOCKET, SO_LINGER, (char *)&ling, sizeof(ling));

	dongle_info_t dongle_info;
    memset(&dongle_info, 0, sizeof(dongle_info));
    memcpy(&dongle_info.magic, "RTL0", 4);

    //r = rtlsdr_get_tuner_type(dev);
    //if (r >= 0)
    //    dongle_info.tuner_type = htonl(r);

    //r = rtlsdr_get_tuner_gains(dev, NULL);
    //if (r >= 0)
    //    dongle_info.tuner_gain_count = htonl(r);

    int bytes_sent = send(sink->clientsocket, (const char *)&dongle_info, sizeof(dongle_info), 0);
    if (sizeof(dongle_info) != bytes_sent)
        sink->logger("failed to send dongle information\n");

	pthread_attr_t attr;
    pthread_t tcp_worker_thread;
    pthread_t command_thread;
    sink->logger("forking workers\n");
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    pthread_create(&tcp_worker_thread, &attr, _tcp_worker, sink);
    pthread_create(&command_thread, &attr, _command_worker, sink);
    pthread_attr_destroy(&attr);

	void *status;
    pthread_join(tcp_worker_thread, &status);
    pthread_join(command_thread, &status);
    sink->logger("threads joined");
    close(sink->clientsocket);
    sink->clientsocket = 0;
}

void *_tcp_worker(void *arg)
{
    rtl_sink* sink  = (rtl_sink*) arg;

	struct llist *curelem,*prev;
	int bytesleft,bytessent, index;
	struct timeval tv= {1,0};
	struct timespec ts;
	struct timeval tp;
	fd_set writefds;
	int r = 0;


	while(1) { 
        if (sink->do_exit) {
            return NULL;
        }
		pthread_mutex_lock(&sink->ll_mutex);
		gettimeofday(&tp, NULL);
		ts.tv_sec  = tp.tv_sec+2;
		ts.tv_nsec = tp.tv_usec * 1000;
		r = pthread_cond_timedwait(&sink->cond, &sink->ll_mutex, &ts);
		if(r == ETIMEDOUT) {
            sink->logger("write thread condtimewait timeouted\n");
			pthread_mutex_unlock(&sink->ll_mutex);
            continue;
		}

		curelem = sink->ll_buffers;
		sink->ll_buffers = 0;
		pthread_mutex_unlock(&sink->ll_mutex);

        sink->logger("writeshit\n");
		while(curelem != 0) {
			bytesleft = curelem->len;
			index = 0;
			bytessent = 0;
			while(bytesleft > 0) {
				FD_ZERO(&writefds);
				FD_SET(sink->clientsocket, &writefds);
				tv.tv_sec = 1;
				tv.tv_usec = 0;
				r = select(sink->clientsocket+1, NULL, &writefds, NULL, &tv);
				if(r) {
					bytessent = send(sink->clientsocket,  &curelem->data[index], bytesleft, 0);
					bytesleft -= bytessent;
					index += bytessent;
				}
				if(bytessent == -1 || sink->do_exit) {
                    sink->logger("disconnect, write thread");
                    return NULL;
				}
			}
			prev = curelem;
			curelem = curelem->next;
			free(prev->data);
			free(prev);
		}
	}
}

void *_command_worker(void *arg)
{
    rtl_sink* sink  = (rtl_sink*) arg;

	int left, received = 0;
	fd_set readfds;
	struct command cmd={0, 0};
	struct timeval tv= {1, 0};
	int r = 0;
	uint32_t tmp;

	while(1) {
		left=sizeof(cmd);
		while(left >0) {
            if (sink->do_exit) {
                sink->logger("exit, read thread");
                return NULL;
            }
			FD_ZERO(&readfds);
			FD_SET(sink->clientsocket, &readfds);
			tv.tv_sec = 1;
			tv.tv_usec = 0;
			r = select(sink->clientsocket+1, &readfds, NULL, NULL, &tv);
            if (r == 0) {
                continue;
            }

            sink->logger("select, read thread, ready: %d/%d\n", r, left);
		    received = recv(sink->clientsocket, (char*)&cmd+(sizeof(cmd)-left), left, 0);
            sink->logger("select, read thread received: %d\n", received);
		    left -= received;
			if(received == -1 || received == 0) {
                sink->logger("disconnect, read thread\n");
                return NULL;
			}
		}
		switch(cmd.cmd) {
		case 0x01:
			sink->logger("set freq %d\n", ntohl(cmd.param));
			break;
		case 0x02:
			sink->logger("set sample rate %d\n", ntohl(cmd.param));
			break;
		case 0x03:
			sink->logger("set gain mode %d\n", ntohl(cmd.param));
			break;
		case 0x04:
			sink->logger("set gain %d\n", ntohl(cmd.param));
			break;
		case 0x05:
			sink->logger("set freq correction %d\n", ntohl(cmd.param));
			break;
		case 0x06:
			tmp = ntohl(cmd.param);
			sink->logger("set if stage %d gain %d\n", tmp >> 16, (short)(tmp & 0xffff));
			break;
		case 0x07:
			sink->logger("set test mode %d\n", ntohl(cmd.param));
			break;
		case 0x08:
			sink->logger("set agc mode %d\n", ntohl(cmd.param));
			break;
		case 0x09:
			sink->logger("set direct sampling %d\n", ntohl(cmd.param));
			break;
		case 0x0a:
			sink->logger("set offset tuning %d\n", ntohl(cmd.param));
			break;
		case 0x0b:
			sink->logger("set rtl xtal %d\n", ntohl(cmd.param));
			break;
		case 0x0c:
			sink->logger("set tuner xtal %d\n", ntohl(cmd.param));
			break;
		case 0x0d:
			sink->logger("set tuner gain by index %d\n", ntohl(cmd.param));
			break;
		case 0x0e:
			sink->logger("set bias tee %d\n", ntohl(cmd.param));
			break;
		default:
			break;
		}
		cmd.cmd = 0xff;
	}
}
