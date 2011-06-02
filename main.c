#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netdb.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <time.h>

#include "httpd.h"

char *server_name   = "gx-0.01";

int  timeout        = 60;
int  keepalive_time = 5;
int  tcp_port       = 0;
int  max_dircache   = 128;
int  max_conn       = 10;
int  nthreads       = 4;
char *doc_root      = ".";
char *listen_ip     = NULL;
char *listen_port   = "8000";
char user[17];
char group[17];

time_t  now;
int     slisten;
char time_buf[64];

pthread_t *threads;
static int termsig, got_sighup;

int host_info(void);

static void catchsig(int sig)
{
    if (SIGTERM == sig || SIGINT == sig) {
        termsig = sig;
    }

    if (SIGHUP == sig) {
        got_sighup = 1;
    }
}

char *get_time(void)
{
    struct tm *newtime;
    time_t aclock;
    time(&aclock);
    newtime = localtime(&aclock);
    strftime(time_buf, 64, "%F %T", newtime);
    return time_buf;
}

static void usage(char *name)
{
    char           *h;
    h = strrchr(name, '/');
    printf("This is a lightweight http server for sharing files\n"
           "\n"
           "usage: %s [ options ]\n"
           "\n"
           "Options:\n"
           "  -h       print this text\n"
           "  -p port  use tcp-port >port<                 [%s]\n",
           h ? h + 1 : name,
           listen_port);
    exit(1);
}

int host_info(void)
{
    register int fd, intrface;
    struct ifreq buf[MAXINTERFACES];
    struct ifconf ifc;

    if ((fd = socket (AF_INET, SOCK_DGRAM, 0)) >= 0) {
        ifc.ifc_len = sizeof buf;
        ifc.ifc_buf = (caddr_t) buf;

        if (!ioctl (fd, SIOCGIFCONF, (char *) &ifc)) {
            intrface = ifc.ifc_len / sizeof (struct ifreq);

            while (intrface-- > 0) {
                printf ("\t%s\n", buf[intrface].ifr_name);

                /*Jugde whether the net card status is promisc  */
                if (!(ioctl (fd, SIOCGIFFLAGS, (char *) &buf[intrface]))) {
                    if (buf[intrface].ifr_flags & IFF_PROMISC) {
                        puts ("the interface is PROMISC");
                    }
                } else {
                    char str[256];
                    sprintf (str, "cpm: ioctl device %s", buf[intrface].ifr_name);
                    perror (str);
                }

                /*Get IP of the net card */
                if (!(ioctl (fd, SIOCGIFADDR, (char *) &buf[intrface]))) {
                    printf("\t%s:%d\n", inet_ntoa(((struct sockaddr_in *)(&buf[intrface].ifr_addr))->sin_addr), tcp_port);
                    puts("");
                } else {
                    char str[256];
                    sprintf (str, "cpm: ioctl device %s", buf[intrface].ifr_name);
                    perror (str);
                    return -1;
                }
            }
        } else {
            perror ("cpm: ioctl");
            return -1;
        }
    } else {
        perror ("cpm: socket");
        return -1;
    }

    close (fd);
    return 0;
}

static void *mainloop(void *thread_arg)
{
    struct REQUEST *conns = NULL;
    int curr_conn = 0;
    struct REQUEST      *req, *prev, *tmp;
    struct timeval      tv;
    int                 max;
    socklen_t           length;
    fd_set              rd, wr;

    for (; !termsig;) {
        if (got_sighup) {
            got_sighup = 0;
        }

        FD_ZERO(&rd);
        FD_ZERO(&wr);
        max = 0;

        /* add listening socket */
        if (curr_conn < max_conn) {
            FD_SET(slisten, &rd);
            max = slisten;
        }

        /* add connection sockets */
        for (req = conns; req != NULL; req = req->next) {
            switch (req->state) {
                case STATE_KEEPALIVE:
                case STATE_READ_HEADER:
                    FD_SET(req->fd, &rd);

                    if (req->fd > max) {
                        max = req->fd;
                    }

                    break;
                case STATE_WRITE_HEADER:
                case STATE_WRITE_BODY:
                case STATE_WRITE_FILE:
                case STATE_WRITE_RANGES:
                    FD_SET(req->fd, &wr);

                    if (req->fd > max) {
                        max = req->fd;
                    }

                    break;
            }
        }

        /* go! */
        tv.tv_sec  = keepalive_time;
        tv.tv_usec = 0;

        if (-1 == select(max + 1, &rd, &wr, NULL, (curr_conn > 0) ? &tv : NULL)) {
            perror("select");
            continue;
        }

        now = time(NULL);

        /* new connection ? */
        if (FD_ISSET(slisten, &rd)) {
            req = malloc(sizeof(struct REQUEST));

            if (NULL != req) {
                memset(req, 0, sizeof(struct REQUEST));

                if (-1 == (req->fd = accept(slisten, NULL, NULL))) {
                    if (EAGAIN != errno) {
                        free(req);
                    }
                } else {
                    close_on_exec(req->fd);
                    fcntl(req->fd, F_SETFL, O_NONBLOCK);
                    req->bfd = -1;
                    req->state = STATE_READ_HEADER;
                    req->ping = now;
                    req->next = conns;
                    conns = req;
                    curr_conn++;

                    /* Make sure the request has not been cancelled!
                     * Otherwise just ignore it. */
                    if (req) {
                        length = sizeof(req->peer);

                        if (-1 == getpeername(req->fd, (struct sockaddr *) & (req->peer), &length)) {
                            req->state = STATE_CLOSE;
                        }

                        getnameinfo((struct sockaddr *)&req->peer, length,
                                    req->peerhost, MAX_HOST,
                                    req->peerserv, MAX_MISC,
                                    NI_NUMERICHOST | NI_NUMERICSERV);
                        printf("%s:\tfd: %03d; connect from %s\n", get_time(), req->fd , req->peerhost);
                    }
                }
            }
        }

        /* check active connections */
        for (req = conns, prev = NULL; req != NULL;) {
            /* handle I/O */
            switch (req->state) {
                case STATE_KEEPALIVE:
                case STATE_READ_HEADER:

                    if (FD_ISSET(req->fd, &rd)) {
                        req->state = STATE_READ_HEADER;
                        read_request(req, 0);
                        req->ping = now;
                    }

                    break;
                case STATE_WRITE_HEADER:
                case STATE_WRITE_BODY:
                case STATE_WRITE_FILE:
                case STATE_WRITE_RANGES:

                    if (FD_ISSET(req->fd, &wr)) {
                        write_request(req);
                        req->ping = now;
                    }

                    break;
            }

            /* check timeouts */
            if (req->state == STATE_KEEPALIVE) {
                if (now > req->ping + keepalive_time || curr_conn > max_conn * 9 / 10) {
                    req->state = STATE_CLOSE;
                }
            } else if (req->state > 0) {
                if (now > req->ping + timeout) {
                    if (req->state == STATE_READ_HEADER) {
                        mkerror(req, 408, 0);
                    } else {
                        req->state = STATE_CLOSE;
                    }
                }
            }

            /* header parsing */
header_parsing:

            if (req->state == STATE_PARSE_HEADER) {
                parse_request(req);

                if (req->state == STATE_WRITE_HEADER) {
                    write_request(req);
                }
            }

            /* handle finished requests */
            if (req->state == STATE_FINISHED && !req->keep_alive) {
                req->state = STATE_CLOSE;
            }

            if (req->state == STATE_FINISHED) {
                req->auth[0]       = 0;
                req->if_modified   = NULL;
                req->if_unmodified = NULL;
                req->if_range      = NULL;
                req->range_hdr     = NULL;
                req->ranges        = 0;

                if (req->r_start) {
                    free(req->r_start);
                    req->r_start = NULL;
                }

                if (req->r_end) {
                    free(req->r_end);
                    req->r_end   = NULL;
                }

                if (req->r_head) {
                    free(req->r_head);
                    req->r_head  = NULL;
                }

                if (req->r_hlen) {
                    free(req->r_hlen);
                    req->r_hlen  = NULL;
                }

                list_free(&req->header);
                memset(req->mtime,   0, sizeof(req->mtime));

                if (req->bfd != -1) {
                    close(req->bfd);
                    req->bfd  = -1;
                }

                req->body      = NULL;
                req->written   = 0;
                req->head_only = 0;
                req->rh        = 0;
                req->rb        = 0;

                if (req->dir) {
                    free_dir(req->dir);
                    req->dir = NULL;
                }

                req->hostname[0] = 0;
                req->path[0]     = 0;
                req->query[0]    = 0;

                if (req->hdata == req->lreq) {
                    /* ok, wait for the next one ... */
                    req->state = STATE_KEEPALIVE;
                    req->hdata = 0;
                    req->lreq  = 0;
                } else {
                    /* there is a pipelined request in the queue ... */
                    req->state = STATE_READ_HEADER;
                    memmove(req->hreq, req->hreq + req->lreq,
                            req->hdata - req->lreq);
                    req->hdata -= req->lreq;
                    req->lreq  =  0;
                    read_request(req, 1);
                    goto header_parsing;
                }
            }

            /* connections to close */
            if (req->state == STATE_CLOSE) {
                close(req->fd);

                if (req->bfd != -1) {
                    close(req->bfd);
                }

                if (req->dir) {
                    free_dir(req->dir);
                }

                curr_conn--;
                printf("%s:\tfd: %03d; current connections: %d\n", get_time(), req->fd, curr_conn);
                /* unlink from list */
                tmp = req;

                if (prev == NULL) {
                    conns = req->next;
                    req = conns;
                } else {
                    prev->next = req->next;
                    req = req->next;
                }

                /* free memory  */
                if (tmp->r_start) {
                    free(tmp->r_start);
                }

                if (tmp->r_end) {
                    free(tmp->r_end);
                }

                if (tmp->r_head) {
                    free(tmp->r_head);
                }

                if (tmp->r_hlen) {
                    free(tmp->r_hlen);
                }

                list_free(&tmp->header);
                free(tmp);
            } else {
                prev = req;
                req = req->next;
            }
        }
    }

    return NULL;
}

/* ---------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    struct sigaction         act, old;
    struct addrinfo          ask, *res;
    struct sockaddr_storage  ss;
    int c, opt, rc, ss_len;
    char host[INET6_ADDRSTRLEN + 1];
    char serv[16];
    const char options[] = "hd" "p:";
    memset(&ask, 0, sizeof(ask));

    /* parse options */
    while(1) {
        if (-1 == (c = getopt(argc, argv, options))) {
            break;
        }

        switch (c) {
            case 'h':
                usage(argv[0]);
                break;
            case 'p':
                listen_port = optarg;
                break;
            default:
                exit(1);
        }
    }

    /* bind to socket */
    memset(&ask, 0, sizeof(ask));
    ask.ai_flags = AI_PASSIVE;

    if (listen_ip) {
        ask.ai_flags |= AI_CANONNAME;
    }

    ask.ai_socktype = SOCK_STREAM;
    ask.ai_family = PF_INET;

    if (0 != (rc = getaddrinfo(listen_ip, listen_port, &ask, &res))) {
        fprintf(stderr, "getaddrinfo (ipv4): %s\n", gai_strerror(rc));
        exit(1);
    }

    if (-1 == (slisten = socket(res->ai_family, res->ai_socktype, res->ai_protocol))) {
        exit(1);
    }

    if (-1 == slisten) {
        exit(1);
    }

    close_on_exec(slisten);
    memcpy(&ss, res->ai_addr, res->ai_addrlen);
    ss_len = res->ai_addrlen;

    if (0 != (rc = getnameinfo((struct sockaddr *)&ss, ss_len,
                               host, INET6_ADDRSTRLEN, serv, 15,
                               NI_NUMERICHOST | NI_NUMERICSERV))) {
        exit(1);
    }

    tcp_port = atoi(serv);
    opt = 1;
    setsockopt(slisten, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    fcntl(slisten, F_SETFL, O_NONBLOCK);

    if (-1 == bind(slisten, (struct sockaddr *) &ss, ss_len)) {
        exit(1);
    }

    if (-1 == listen(slisten, 2 * max_conn)) {
        exit(1);
    }

    init_quote();
    printf("gx start!\n\n");
    printf("###############################\n");
    printf("HOST INFO:\n");
    host_info();
    printf("###############################\n");
    printf("SERVER INFO:\n");
    /* setup signal handler */
    memset(&act, 0, sizeof(act));
    sigemptyset(&act.sa_mask);
    act.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &act, &old);
    sigaction(SIGCHLD, &act, &old);
    act.sa_handler = catchsig;
    sigaction(SIGHUP, &act, &old);
    sigaction(SIGTERM, &act, &old);
    sigaction(SIGINT, &act, &old);

    /* go! */
    if (nthreads > 1) {
        int i;
        threads = malloc(sizeof(pthread_t) * nthreads);

        for (i = 1; i < nthreads; i++) {
            pthread_create(threads + i, NULL, mainloop, threads + i);
            pthread_detach(threads[i]);
        }
    }

    mainloop(NULL);
    fprintf(stderr, "bye...\n");
    exit(0);
}
