#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <inttypes.h>
#include <sys/socket.h>

#if defined(linux)
#include <sys/sendfile.h>
#endif

#include "httpd.h"

#define wrap_xsendfile(req,off,bytes)  xsendfile(req->fd,req->bfd,off,bytes)
#define wrap_write(req,buf,bytes)      write(req->fd,buf,bytes);

static inline size_t off_to_size(off_t off_bytes)
{
    if (off_bytes > SSIZE_MAX) {
        return SSIZE_MAX;
    }

    return off_bytes;
}

#if defined(__CYGWIN32)
ssize_t sendfile(int out, int in, off_t *offset, size_t bytes)
{
	  char buff[4096];
	  int len, sum = 0;
	  if (lseek(in, *offset, SEEK_SET)) {
	  	return -1;
	  }
	  
	  while((len = read(in, buff, sizeof(buff))) >0) {
	  	send(out, buff, len ,0);
	  	sum += len;
	  }
	  
	  return sum;
}
#endif

static ssize_t xsendfile(int out, int in, off_t offset, off_t off_bytes)
{
    size_t bytes = off_to_size(off_bytes);
    return sendfile(out, in, &offset, bytes);
}

static struct HTTP_STATUS {
    int   status;
    char *head;
    char *body;
}

http[] = {
    { 200, "200 OK",                       NULL },
    { 206, "206 Partial Content",          NULL },
    { 304, "304 Not Modified",             NULL },
    { 400, "400 Bad Request",              "*PLONK*\n" },
    { 401, "401 Authentication required",  "Authentication required\n" },
    { 403, "403 Forbidden",                "Access denied\n" },
    { 404, "404 Not Found",                "File or directory not found\n" },
    { 408, "408 Request Timeout",          "Request Timeout\n" },
    { 412, "412 Precondition failed.",     "Precondition failed\n" },
    { 500, "500 Internal Server Error",    "Sorry folks\n" },
    { 501, "501 Not Implemented",          "Sorry folks\n" },
    {   0, NULL,                        NULL }
};

#define RESPONSE_START          \
    "HTTP/1.1 %s\r\n"       \
    "Server: %s\r\n"        \
    "Connection: %s\r\n"        \
    "Accept-Ranges: bytes\r\n"

#define BOUNDARY            \
    "XXX_CUT_HERE_%ld_XXX"

void mkerror(struct REQUEST *req, int status, int ka)
{
    int i;

    for (i = 0; http[i].status != 0; i++)
        if (http[i].status == status) {
            break;
        }

    req->status = status;
    req->body   = http[i].body;
    req->lbody  = strlen(req->body);

    if (!ka) {
        req->keep_alive = 0;
    }

    req->lres = sprintf(req->hres,
                        RESPONSE_START
                        "Content-Type: text/plain\r\n"
                        "Content-Length: %" PRId64 "\r\n",
                        http[i].head, server_name,
                        req->keep_alive ? "Keep-Alive" : "Close",
                        (int64_t)req->lbody);

    if (401 == status)
        req->lres += sprintf(req->hres + req->lres,
                             "WWW-Authenticate: Basic realm=\"gx\"\r\n");

    req->lres += strftime(req->hres + req->lres, 80,
                          "Date: " RFC1123 "\r\n\r\n",
                          gmtime(&now));
    req->state = STATE_WRITE_HEADER;
}

void mkredirect(struct REQUEST *req)
{
    req->status = 302;
    req->body   = req->path;
    req->lbody  = strlen(req->body);
    req->lres = sprintf(req->hres,
                        RESPONSE_START
                        "Location: http://%s:%d%s\r\n"
                        "Content-Type: text/plain\r\n"
                        "Content-Length: %" PRId64 "\r\n",
                        "302 Redirect", server_name,
                        req->keep_alive ? "Keep-Alive" : "Close",
                        req->hostname, tcp_port,
                        quote((unsigned char *) req->path, 9999),
                        (int64_t)req->lbody);
    req->lres += strftime(req->hres + req->lres, 80,
                          "Date: " RFC1123 "\r\n\r\n",
                          gmtime(&now));
    req->state = STATE_WRITE_HEADER;
}

static int mkmulti(struct REQUEST *req, int i)
{
    req->r_hlen[i] = sprintf(req->r_head + i * BR_HEADER,
                             "\r\n--" BOUNDARY "\r\n"
                             "Content-type: %s\r\n"
                             "Content-range: bytes %" PRId64 "-%" PRId64 "/%" PRId64 "\r\n"
                             "\r\n",
                             now, req->mime,
                             (int64_t)req->r_start[i],
                             (int64_t)req->r_end[i] - 1,
                             (int64_t)req->bst.st_size);
    return req->r_hlen[i];
}

void mkheader(struct REQUEST *req, int status)
{
    int    i;
    off_t  len;

    for (i = 0; http[i].status != 0; i++)
        if (http[i].status == status) {
            break;
        }

    req->status = status;
    req->lres = sprintf(req->hres,
                        RESPONSE_START,
                        http[i].head, server_name,
                        req->keep_alive ? "Keep-Alive" : "Close");

    if (req->ranges == 0) {
        req->lres += sprintf(req->hres + req->lres,
                             "Content-Type: %s\r\n"
                             "Content-Length: %" PRId64 "\r\n",
                             req->mime,
                             (int64_t)(req->body ? req->lbody : req->bst.st_size));
    } else if (req->ranges == 1) {
        req->lres += sprintf(req->hres + req->lres,
                             "Content-Type: %s\r\n"
                             "Content-Range: bytes %" PRId64 "-%" PRId64 "/%" PRId64 "\r\n"
                             "Content-Length: %" PRId64 "\r\n",
                             req->mime,
                             (int64_t)req->r_start[0],
                             (int64_t)req->r_end[0] - 1,
                             (int64_t)req->bst.st_size,
                             (int64_t)(req->r_end[0] - req->r_start[0]));
    } else {
        for (i = 0, len = 0; i < req->ranges; i++) {
            len += mkmulti(req, i);
            len += req->r_end[i] - req->r_start[i];
        }

        req->r_hlen[i] = sprintf(req->r_head + i * BR_HEADER,
                                 "\r\n--" BOUNDARY "--\r\n",
                                 now);
        len += req->r_hlen[i];
        req->lres += sprintf(req->hres + req->lres,
                             "Content-Type: multipart/byteranges;"
                             " boundary=" BOUNDARY "\r\n"
                             "Content-Length: %" PRId64 "\r\n",
                             now, (int64_t)len);
    }

    if (req->mtime[0] != '\0') {
        req->lres += sprintf(req->hres + req->lres,
                             "Last-Modified: %s\r\n",
                             req->mtime);
    }

    req->lres += strftime(req->hres + req->lres, 80,
                          "Date: " RFC1123 "\r\n\r\n",
                          gmtime(&now));
    req->state = STATE_WRITE_HEADER;
}

void write_request(struct REQUEST *req)
{
    int rc;

    for (;;) {
        switch (req->state) {
            case STATE_WRITE_HEADER:
                rc = wrap_write(req, req->hres + req->written,
                                req->lres - req->written);

                switch (rc) {
                    case -1:

                        if (errno == EAGAIN) {
                            return;
                        }

                        if (errno == EINTR) {
                            continue;
                        }

                        /* fall through */
                    case 0:
                        req->state = STATE_CLOSE;
                        return;
                    default:
                        req->written += rc;
                        req->bc += rc;

                        if (req->written != req->lres) {
                            return;
                        }
                }

                req->written = 0;

                if (req->head_only) {
                    req->state = STATE_FINISHED;
                    return;
                } else if (req->body) {
                    req->state = STATE_WRITE_BODY;
                } else if (req->ranges == 1) {
                    req->state = STATE_WRITE_RANGES;
                    req->rh = -1;
                    req->rb = 0;
                    req->written = req->r_start[0];
                } else if (req->ranges > 1) {
                    req->state = STATE_WRITE_RANGES;
                    req->rh = 0;
                    req->rb = -1;
                } else {
                    req->state = STATE_WRITE_FILE;
                }

                break;
            case STATE_WRITE_BODY:
                rc = wrap_write(req, req->body + req->written,
                                req->lbody - req->written);

                switch (rc) {
                    case -1:

                        if (errno == EAGAIN) {
                            return;
                        }

                        if (errno == EINTR) {
                            continue;
                        }

                        /* fall through */
                    case 0:
                        req->state = STATE_CLOSE;
                        return;
                    default:
                        req->written += rc;
                        req->bc += rc;

                        if (req->written != req->lbody) {
                            return;
                        }
                }

                req->state = STATE_FINISHED;
                return;
            case STATE_WRITE_FILE:
                rc = wrap_xsendfile(req, req->written,
                                    req->bst.st_size - req->written);

                switch (rc) {
                    case -1:

                        if (errno == EAGAIN) {
                            return;
                        }

                        if (errno == EINTR) {
                            continue;
                        }

                        /* fall through */
                    case 0:
                        req->state = STATE_CLOSE;
                        return;
                    default:
                        req->written += rc;
                        req->bc += rc;

                        if (req->written != req->bst.st_size) {
                            return;
                        }
                }

                req->state = STATE_FINISHED;
                return;
            case STATE_WRITE_RANGES:

                if (-1 != req->rh) {
                    /* write header */
                    rc = wrap_write(req,
                                    req->r_head + req->rh * BR_HEADER + req->written,
                                    req->r_hlen[req->rh] - req->written);

                    switch (rc) {
                        case -1:

                            if (errno == EAGAIN) {
                                return;
                            }

                            if (errno == EINTR) {
                                continue;
                            }

                            /* fall through */
                        case 0:
                            req->state = STATE_CLOSE;
                            return;
                        default:
                            req->written += rc;
                            req->bc += rc;

                            if (req->written != req->r_hlen[req->rh]) {
                                return;
                            }
                    }

                    if (req->rh == req->ranges) {
                        /* done -- no more ranges */
                        req->state = STATE_FINISHED;
                        return;
                    }

                    /* prepare for body writeout */
                    req->rb      = req->rh;
                    req->rh      = -1;
                    req->written = req->r_start[req->rb];
                }

                if (-1 != req->rb) {
                    /* write body */
                    rc = wrap_xsendfile(req, req->written,
                                        req->r_end[req->rb] - req->written);

                    switch (rc) {
                        case -1:

                            if (errno == EAGAIN) {
                                return;
                            }

                            if (errno == EINTR) {
                                continue;
                            }

                            /* fall through */
                        case 0:
                            req->state = STATE_CLOSE;
                            return;
                        default:
                            req->written += rc;
                            req->bc += rc;

                            if (req->written != req->r_end[req->rb]) {
                                return;
                            }
                    }

                    /* prepare for next subheader writeout */
                    req->rh      = req->rb + 1;
                    req->rb      = -1;
                    req->written = 0;

                    if (req->ranges == 1) {
                        /* single range only */
                        req->state = STATE_FINISHED;
                        return;
                    }
                }

                break;
        }
    }
}
