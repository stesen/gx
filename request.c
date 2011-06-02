#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <netinet/in.h>

#include "httpd.h"

/* ---------------------------------------------------------------------- */

void read_request(struct REQUEST *req, int pipelined)
{
    int rc;
    char *h;
restart:
    rc = read(req->fd, req->hreq + req->hdata, MAX_HEADER - req->hdata);

    switch (rc) {
        case -1:

            if (errno == EAGAIN) {
                if (pipelined) {
                    break;    /* check if there is already a full request */
                } else {
                    return;
                }
            }

            if (errno == EINTR) {
                goto restart;
            }

            /* fall through */
        case 0:
            req->state = STATE_CLOSE;
            return;
        default:
            req->hdata += rc;
            req->hreq[req->hdata] = 0;
    }

    /* check if this looks like a http request after
       the first few bytes... */
    if (req->hdata < 5) {
        return;
    }

    if (strncmp(req->hreq, "GET ", 4)  != 0  && strncmp(req->hreq, "PUT ", 4)  != 0  && strncmp(req->hreq, "HEAD ", 5) != 0  && strncmp(req->hreq, "POST ", 5) != 0) {
        mkerror(req, 400, 0);
        return;
    }

    /* header complete ?? */
    if (NULL != (h = strstr(req->hreq, "\r\n\r\n")) || NULL != (h = strstr(req->hreq, "\n\n"))) {
        if (*h == '\r') {
            h += 4;
            *(h - 2) = 0;
        } else {
            h += 2;
            *(h - 1) = 0;
        }

        req->lreq  = h - req->hreq;
        req->state = STATE_PARSE_HEADER;
        return;
    }

    if (req->hdata == MAX_HEADER) {
        mkerror(req, 400, 0);
        return;
    }

    return;
}


static off_t parse_off_t(char *str, int *pos)
{
    off_t value = 0;

    while (isdigit(str[*pos])) {
        value *= 10;
        value += str[*pos] - '0';
        (*pos)++;
    }

    return value;
}

static int
parse_ranges(struct REQUEST *req)
{
    char *h, *line = req->range_hdr;
    int  i, off;

    for (h = line, req->ranges = 1; *h != '\n' && *h != '\0'; h++)
        if (*h == ',') {
            req->ranges++;
        }

    req->r_start = malloc(req->ranges * sizeof(off_t));
    req->r_end   = malloc(req->ranges * sizeof(off_t));
    req->r_head  = malloc((req->ranges + 1) * BR_HEADER);
    req->r_hlen  = malloc((req->ranges + 1) * sizeof(int));

    if (NULL == req->r_start || NULL == req->r_end ||
        NULL == req->r_head  || NULL == req->r_hlen) {
        if (req->r_start) {
            free(req->r_start);
        }

        if (req->r_end) {
            free(req->r_end);
        }

        if (req->r_head) {
            free(req->r_head);
        }

        if (req->r_hlen) {
            free(req->r_hlen);
        }

        return 500;
    }

    for (i = 0, off = 0; i < req->ranges; i++) {
        if (line[off] == '-') {
            off++;

            if (!isdigit(line[off])) {
                goto parse_error;
            }

            req->r_start[i] = req->bst.st_size - parse_off_t(line, &off);
            req->r_end[i]   = req->bst.st_size;
        } else {
            if (!isdigit(line[off])) {
                goto parse_error;
            }

            req->r_start[i] = parse_off_t(line, &off);

            if (line[off] != '-') {
                goto parse_error;
            }

            off++;

            if (isdigit(line[off])) {
                req->r_end[i] = parse_off_t(line, &off) + 1;
            } else {
                req->r_end[i] = req->bst.st_size;
            }
        }

        off++; /* skip "," */

        /* ranges ok? */
        if (req->r_start[i] > req->r_end[i] ||
            req->r_end[i]   > req->bst.st_size) {
            goto parse_error;
        }
    }

    return 0;
parse_error:
    req->ranges = 0;
    return 400;
}

static int unhex(unsigned char c)
{
    if (c < '@') {
        return c - '0';
    }

    return (c & 0x0f) + 9;
}

/* handle %hex quoting, also split path / querystring */
static void unquote(unsigned char *path, unsigned char *qs, unsigned char *src)
{
    int q;
    unsigned char *dst;
    q = 0;
    dst = path;

    while (src[0] != 0) {
        if (!q && *src == '?') {
            q = 1;
            *dst = 0;
            dst = qs;
            src++;
            continue;
        }

        if (q && *src == '+') {
            *dst = ' ';
        } else if ((*src == '%') && isxdigit(src[1]) && isxdigit(src[2])) {
            *dst = (unhex(src[1]) << 4) | unhex(src[2]);
            src += 2;
        } else {
            *dst = *src;
        }

        dst++;
        src++;
    }

    *dst = 0;
}

/* delete unneeded path elements */
static void fixpath(char *path)
{
    char *dst = path;
    char *src = path;

    for (; *src;) {
        if (0 == strncmp(src, "//", 2)) {
            src++;
            continue;
        }

        if (0 == strncmp(src, "/./", 3)) {
            src += 2;
            continue;
        }

        *(dst++) = *(src++);
    }

    *dst = 0;
}

static int sanity_checks(struct REQUEST *req)
{
    int i;

    /* path: must start with a '/' */
    if (req->path[0] != '/') {
        mkerror(req, 400, 0);
        return -1;
    }

    /* path: must not contain "/../" */
    if (strstr(req->path, "/../")) {
        mkerror(req, 403, 1);
        return -1;
    }

    if (req->hostname[0] == '\0')
        /* no hostname specified */
    {
        return 0;
    }

    /* validate hostname */
    for (i = 0; req->hostname[i] != '\0'; i++) {
        switch (req->hostname[i]) {
            case 'A' ... 'Z':
                req->hostname[i] += 32; /* lowercase */
            case 'a' ... 'z':
            case '0' ... '9':
            case '-':
                /* these are fine as-is */
                break;
            case '.':

                /* some extra checks */
                if (0 == i) {
                    /* don't allow a dot as first character */
                    mkerror(req, 400, 0);
                    return -1;
                }

                if ('.' == req->hostname[i - 1]) {
                    /* don't allow two dots in sequence */
                    mkerror(req, 400, 0);
                    return -1;
                }

                break;
            default:
                /* invalid character */
                mkerror(req, 400, 0);
                return -1;
        }
    }

    return 0;
}

void parse_request(struct REQUEST *req)
{
    char filename[MAX_PATH + 1], proto[MAX_MISC + 1], *h;
    int  port, rc, len;

    /* parse request. Hehe, scanf is powerfull :-) */
    if (4 != sscanf(req->hreq,
                    "%" S(MAX_MISC) "[A-Z] "
                    "%" S(MAX_PATH) "[^ \t\r\n] HTTP/%d.%d",
                    req->type, filename, &(req->major), &(req->minor))) {
        mkerror(req, 400, 0);
        return;
    }

    if (filename[0] == '/') {
        strncpy(req->uri, filename, sizeof(req->uri) - 1);
    } else {
        port = 0;
        *proto = 0;

        if (4 != sscanf(filename,
                        "%" S(MAX_MISC) "[a-zA-Z]://"
                        "%" S(MAX_HOST) "[a-zA-Z0-9.-]:%d"
                        "%" S(MAX_PATH) "[^ \t\r\n]",
                        proto, req->hostname, &port, req->uri) &&
            3 != sscanf(filename,
                        "%" S(MAX_MISC) "[a-zA-Z]://"
                        "%" S(MAX_HOST) "[a-zA-Z0-9.-]"
                        "%" S(MAX_PATH) "[^ \t\r\n]",
                        proto, req->hostname, req->uri)) {
            mkerror(req, 400, 0);
            return;
        }

        if (*proto != 0 && 0 != strcasecmp(proto, "http")) {
            mkerror(req, 400, 0);
            return;
        }
    }

    unquote((unsigned char *) req->path,
            (unsigned char *) req->query,
            (unsigned char *) req->uri);
    fixpath(req->path);

    if (0 != strcmp(req->type, "GET") &&
        0 != strcmp(req->type, "HEAD")) {
        mkerror(req, 501, 0);
        return;
    }

    if (0 == strcmp(req->type, "HEAD")) {
        req->head_only = 1;
    }

    /* parse header lines */
    req->keep_alive = req->minor;

    for (h = req->hreq; h - req->hreq < req->lreq;) {
        h = strchr(h, '\n');

        if (NULL == h) {
            break;
        }

        h++;
        h[-2] = 0;
        h[-1] = 0;
        list_add(&req->header, h, 0);

        if (0 == strncasecmp(h, "Connection: ", 12)) {
            req->keep_alive = (0 == strncasecmp(h + 12, "Keep-Alive", 10));
        } else if (0 == strncasecmp(h, "Host: ", 6)) {
            if (2 != sscanf(h + 6, "%" S(MAX_HOST) "[a-zA-Z0-9.-]:%d",
                            req->hostname, &port))
                sscanf(h + 6, "%" S(MAX_HOST) "[a-zA-Z0-9.-]",
                       req->hostname);
        } else if (0 == strncasecmp(h, "If-Modified-Since: ", 19)) {
            req->if_modified = h + 19;
        } else if (0 == strncasecmp(h, "If-Unmodified-Since: ", 21)) {
            req->if_unmodified = h + 21;
        } else if (0 == strncasecmp(h, "If-Range: ", 10)) {
            req->if_range = h + 10;
        } else if (0 == strncasecmp(h, "Range: bytes=", 13)) {
            /* parsing must be done after fstat, we need the file size
               for the boundary checks */
            req->range_hdr = h + 13;
        }
    }

    /* checks */
    if (0 != sanity_checks(req)) {
        return;
    }

    len = snprintf(filename, sizeof(filename) - 1,
                   "%s%s%s%s",
                   doc_root,
                   "",
                   "",
                   req->path);
    h = filename + len - 1;

    if (*h == '/') {
        /* looks like the client asks for a directory */
        if (-1 == stat(filename, &(req->bst))) {
            if (errno == EACCES) {
                mkerror(req, 403, 1);
            } else {
                mkerror(req, 404, 1);
            }

            return;
        }

        strftime(req->mtime, sizeof(req->mtime), RFC1123, gmtime(&req->bst.st_mtime));
        req->mime = "text/html";
        req->dir = get_dir(req, filename);

        if (NULL == req->body) {
            /* We arrive here if opendir failed, probably due to -EPERM
             * It does exist (see the stat() call above) */
            mkerror(req, 403, 1);
            return;
        } else if (NULL != req->if_modified && 0 == strcmp(req->if_modified, req->mtime)) {
            /* 304 not modified */
            mkheader(req, 304);
            req->head_only = 1;
        } else {
            /* 200 OK */
            mkheader(req, 200);
        }

        return;
    }

    /* it is /probably/ a regular file */
    if (-1 == (req->bfd = open(filename, O_RDONLY))) {
        if (errno == EACCES) {
            mkerror(req, 403, 1);
        } else {
            mkerror(req, 404, 1);
        }

        return;
    }

    fstat(req->bfd, &(req->bst));

    if (req->range_hdr)
        if (0 != (rc = parse_ranges(req))) {
            mkerror(req, rc, 1);
            return;
        }

    if (!S_ISREG(req->bst.st_mode)) {
        /* /not/ a regular file */
        close(req->bfd);
        req->bfd = -1;

        if (S_ISDIR(req->bst.st_mode)) {
            /* oops: a directory without trailing slash */
            strcat(req->path, "/");
            mkredirect(req);
        } else {
            /* anything else is'nt allowed here */
            mkerror(req, 403, 1);
        }

        return;
    }

    /* it is /really/ a regular file */
    req->mime = get_mime(filename);
    strftime(req->mtime, sizeof(req->mtime), RFC1123, gmtime(&req->bst.st_mtime));

    if (NULL != req->if_range  &&  0 != strcmp(req->if_range, req->mtime))
        /* mtime mismatch -> no ranges */
    {
        req->ranges = 0;
    }

    if (NULL != req->if_unmodified && 0 != strcmp(req->if_unmodified, req->mtime)) {
        /* 412 precondition failed */
        mkerror(req, 412, 1);
    } else if (NULL != req->if_modified && 0 == strcmp(req->if_modified, req->mtime)) {
        /* 304 not modified */
        mkheader(req, 304);
        req->head_only = 1;
    } else if (req->ranges > 0) {
        /* send byte range(s) */
        mkheader(req, 206);
    } else {
        /* normal */
        mkheader(req, 200);
    }

    return;
}
