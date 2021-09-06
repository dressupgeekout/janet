/*
* Copyright (c) 2021 Calvin Rose and contributors.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to
* deal in the Software without restriction, including without limitation the
* rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
* sell copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
*/

#ifndef JANET_AMALG
#include "features.h"
#include <janet.h>
#include "util.h"
#endif

#ifdef JANET_NET

#include <math.h>
#ifdef JANET_WINDOWS
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#pragma comment (lib, "Ws2_32.lib")
#pragma comment (lib, "Mswsock.lib")
#pragma comment (lib, "Advapi32.lib")
#else
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <fcntl.h>
#endif

const JanetAbstractType janet_address_type = {
    "core/socket-address",
    JANET_ATEND_NAME
};

#ifdef JANET_WINDOWS
#define JSOCKCLOSE(x) closesocket((SOCKET) x)
#define JSOCKDEFAULT INVALID_SOCKET
#define JSOCKVALID(x) ((x) != INVALID_SOCKET)
#define JSock SOCKET
#define JSOCKFLAGS 0
#else
#define JSOCKCLOSE(x) close(x)
#define JSOCKDEFAULT 0
#define JSOCKVALID(x) ((x) >= 0)
#define JSock int
#ifdef SOCK_CLOEXEC
#define JSOCKFLAGS SOCK_CLOEXEC
#else
#define JSOCKFLAGS 0
#endif
#endif

static JanetStream *make_stream(JSock handle, uint32_t flags);

/* We pass this flag to all send calls to prevent sigpipe */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* Make sure a socket doesn't block */
static void janet_net_socknoblock(JSock s) {
#ifdef JANET_WINDOWS
    unsigned long arg = 1;
    ioctlsocket(s, FIONBIO, &arg);
#else
#if !defined(SOCK_CLOEXEC) && defined(O_CLOEXEC)
    int extra = O_CLOEXEC;
#else
    int extra = 0;
#endif
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK | extra);
#ifdef SO_NOSIGPIPE
    int enable = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &enable, sizeof(int));
#endif
#endif
}

/* State machine for accepting connections. */

#ifdef JANET_WINDOWS

typedef struct {
    JanetListenerState head;
    WSAOVERLAPPED overlapped;
    JanetFunction *function;
    JanetStream *lstream;
    JanetStream *astream;
    char buf[1024];
} NetStateAccept;

static int net_sched_accept_impl(NetStateAccept *state, Janet *err);

JanetAsyncStatus net_machine_accept(JanetListenerState *s, JanetAsyncEvent event) {
    NetStateAccept *state = (NetStateAccept *)s;
    switch (event) {
        default:
            break;
        case JANET_ASYNC_EVENT_MARK: {
            if (state->lstream) janet_mark(janet_wrap_abstract(state->lstream));
            if (state->astream) janet_mark(janet_wrap_abstract(state->astream));
            if (state->function) janet_mark(janet_wrap_abstract(state->function));
            break;
        }
        case JANET_ASYNC_EVENT_CLOSE:
            janet_schedule(s->fiber, janet_wrap_nil());
            return JANET_ASYNC_STATUS_DONE;
        case JANET_ASYNC_EVENT_COMPLETE: {
            int seconds;
            int bytes = sizeof(seconds);
            if (NO_ERROR != getsockopt((SOCKET) state->astream->handle, SOL_SOCKET, SO_CONNECT_TIME,
                                       (char *)&seconds, &bytes)) {
                janet_cancel(s->fiber, janet_cstringv("failed to accept connection"));
                return JANET_ASYNC_STATUS_DONE;
            }
            if (NO_ERROR != setsockopt((SOCKET) state->astream->handle, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                                       (char *) & (state->lstream->handle), sizeof(SOCKET))) {
                janet_cancel(s->fiber, janet_cstringv("failed to accept connection"));
                return JANET_ASYNC_STATUS_DONE;
            }

            Janet streamv = janet_wrap_abstract(state->astream);
            if (state->function) {
                /* Schedule worker */
                JanetFiber *fiber = janet_fiber(state->function, 64, 1, &streamv);
                fiber->supervisor_channel = s->fiber->supervisor_channel;
                janet_schedule(fiber, janet_wrap_nil());
                /* Now listen again for next connection */
                Janet err;
                if (net_sched_accept_impl(state, &err)) {
                    janet_cancel(s->fiber, err);
                    return JANET_ASYNC_STATUS_DONE;
                }
            } else {
                janet_schedule(s->fiber, streamv);
                return JANET_ASYNC_STATUS_DONE;
            }
        }
    }
    return JANET_ASYNC_STATUS_NOT_DONE;
}

JANET_NO_RETURN static void janet_sched_accept(JanetStream *stream, JanetFunction *fun) {
    Janet err;
    SOCKET lsock = (SOCKET) stream->handle;
    JanetListenerState *s = janet_listen(stream, net_machine_accept, JANET_ASYNC_LISTEN_READ, sizeof(NetStateAccept), NULL);
    NetStateAccept *state = (NetStateAccept *)s;
    memset(&state->overlapped, 0, sizeof(WSAOVERLAPPED));
    memset(&state->buf, 0, 1024);
    state->function = fun;
    state->lstream = stream;
    s->tag = &state->overlapped;
    if (net_sched_accept_impl(state, &err)) janet_panicv(err);
    janet_await();
}

static int net_sched_accept_impl(NetStateAccept *state, Janet *err) {
    SOCKET lsock = (SOCKET) state->lstream->handle;
    SOCKET asock = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (asock == INVALID_SOCKET) {
        *err = janet_ev_lasterr();
        return 1;
    }
    JanetStream *astream = make_stream(asock, JANET_STREAM_READABLE | JANET_STREAM_WRITABLE);
    state->astream = astream;
    int socksize = sizeof(SOCKADDR_STORAGE) + 16;
    if (FALSE == AcceptEx(lsock, asock, state->buf, 0, socksize, socksize, NULL, &state->overlapped)) {
        int code = WSAGetLastError();
        if (code == WSA_IO_PENDING) return 0; /* indicates io is happening async */
        *err = janet_ev_lasterr();
        return 1;
    }
    return 0;
}

#else

typedef struct {
    JanetListenerState head;
    JanetFunction *function;
} NetStateAccept;

JanetAsyncStatus net_machine_accept(JanetListenerState *s, JanetAsyncEvent event) {
    NetStateAccept *state = (NetStateAccept *)s;
    switch (event) {
        default:
            break;
        case JANET_ASYNC_EVENT_MARK: {
            if (state->function) janet_mark(janet_wrap_function(state->function));
            break;
        }
        case JANET_ASYNC_EVENT_CLOSE:
            janet_schedule(s->fiber, janet_wrap_nil());
            return JANET_ASYNC_STATUS_DONE;
        case JANET_ASYNC_EVENT_READ: {
            JSock connfd = accept(s->stream->handle, NULL, NULL);
            if (JSOCKVALID(connfd)) {
                janet_net_socknoblock(connfd);
                JanetStream *stream = make_stream(connfd, JANET_STREAM_READABLE | JANET_STREAM_WRITABLE);
                Janet streamv = janet_wrap_abstract(stream);
                if (state->function) {
                    JanetFiber *fiber = janet_fiber(state->function, 64, 1, &streamv);
                    fiber->supervisor_channel = s->fiber->supervisor_channel;
                    janet_schedule(fiber, janet_wrap_nil());
                } else {
                    janet_schedule(s->fiber, streamv);
                    return JANET_ASYNC_STATUS_DONE;
                }
            }
            break;
        }
    }
    return JANET_ASYNC_STATUS_NOT_DONE;
}

JANET_NO_RETURN static void janet_sched_accept(JanetStream *stream, JanetFunction *fun) {
    NetStateAccept *state = (NetStateAccept *) janet_listen(stream, net_machine_accept, JANET_ASYNC_LISTEN_READ, sizeof(NetStateAccept), NULL);
    state->function = fun;
    janet_await();
}


#endif

/* Adress info */

static int janet_get_sockettype(Janet *argv, int32_t argc, int32_t n) {
    JanetKeyword stype = janet_optkeyword(argv, argc, n, NULL);
    int socktype = SOCK_DGRAM;
    if ((NULL == stype) || !janet_cstrcmp(stype, "stream")) {
        socktype = SOCK_STREAM;
    } else if (janet_cstrcmp(stype, "datagram")) {
        janet_panicf("expected socket type as :stream or :datagram, got %v", argv[n]);
    }
    return socktype;
}

/* Needs argc >= offset + 2 */
/* For unix paths, just rertuns a single sockaddr and sets *is_unix to 1, otherwise 0 */
static struct addrinfo *janet_get_addrinfo(Janet *argv, int32_t offset, int socktype, int passive, int *is_unix) {
    /* Unix socket support - not yet supported on windows. */
#ifndef JANET_WINDOWS
    if (janet_keyeq(argv[offset], "unix")) {
        const char *path = janet_getcstring(argv, offset + 1);
        struct sockaddr_un *saddr = janet_calloc(1, sizeof(struct sockaddr_un));
        if (saddr == NULL) {
            JANET_OUT_OF_MEMORY;
        }
        saddr->sun_family = AF_UNIX;
        size_t path_size = sizeof(saddr->sun_path);
#ifdef JANET_LINUX
        if (path[0] == '@') {
            saddr->sun_path[0] = '\0';
            snprintf(saddr->sun_path + 1, path_size - 1, "%s", path + 1);
        } else
#endif
        {
            snprintf(saddr->sun_path, path_size, "%s", path);
        }
        *is_unix = 1;
        return (struct addrinfo *) saddr;
    }
#endif
    /* Get host and port */
    const char *host = janet_getcstring(argv, offset);
    const char *port;
    if (janet_checkint(argv[offset + 1])) {
        port = (const char *)janet_to_string(argv[offset + 1]);
    } else {
        port = janet_optcstring(argv, offset + 2, offset + 1, NULL);
    }
    /* getaddrinfo */
    struct addrinfo *ai = NULL;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = socktype;
    hints.ai_flags = passive ? AI_PASSIVE : 0;
    int status = getaddrinfo(host, port, &hints, &ai);
    if (status) {
        janet_panicf("could not get address info: %s", gai_strerror(status));
    }
    *is_unix = 0;
    return ai;
}

/*
 * C Funs
 */

JANET_CORE_FN(cfun_net_sockaddr,
              "(net/address host port &opt type)",
              "Look up the connection information for a given hostname, port, and connection type. Returns "
              "a handle that can be used to send datagrams over network without establishing a connection. "
              "On Posix platforms, you can use :unix for host to connect to a unix domain socket, where the name is "
              "given in the port argument. On Linux, abstract "
              "unix domain sockets are specified with a leading '@' character in port.") {
    janet_arity(argc, 2, 4);
    int socktype = janet_get_sockettype(argv, argc, 2);
    int is_unix = 0;
    int make_arr = (argc >= 3 && janet_truthy(argv[3]));
    struct addrinfo *ai = janet_get_addrinfo(argv, 0, socktype, 0, &is_unix);
#ifndef JANET_WINDOWS
    /* no unix domain socket support on windows yet */
    if (is_unix) {
        void *abst = janet_abstract(&janet_address_type, sizeof(struct sockaddr_un));
        memcpy(abst, ai, sizeof(struct sockaddr_un));
        Janet ret = janet_wrap_abstract(abst);
        return make_arr ? janet_wrap_array(janet_array_n(&ret, 1)) : ret;
    }
#endif
    if (make_arr) {
        /* Select all */
        JanetArray *arr = janet_array(10);
        struct addrinfo *iter = ai;
        while (NULL != iter) {
            void *abst = janet_abstract(&janet_address_type, iter->ai_addrlen);
            memcpy(abst, iter->ai_addr, iter->ai_addrlen);
            janet_array_push(arr, janet_wrap_abstract(abst));
            iter = iter->ai_next;
        }
        freeaddrinfo(ai);
        return janet_wrap_array(arr);
    } else {
        /* Select first */
        if (NULL == ai) {
            janet_panic("no data for given address");
        }
        void *abst = janet_abstract(&janet_address_type, ai->ai_addrlen);
        memcpy(abst, ai->ai_addr, ai->ai_addrlen);
        freeaddrinfo(ai);
        return janet_wrap_abstract(abst);
    }
}

JANET_CORE_FN(cfun_net_connect,
              "(net/connect host port &opt type)",
              "Open a connection to communicate with a server. Returns a duplex stream "
              "that can be used to communicate with the server. Type is an optional keyword "
              "to specify a connection type, either :stream or :datagram. The default is :stream. ") {
    janet_arity(argc, 2, 3);

    int socktype = janet_get_sockettype(argv, argc, 2);
    int is_unix = 0;
    struct addrinfo *ai = janet_get_addrinfo(argv, 0, socktype, 0, &is_unix);

    /* Create socket */
    JSock sock = JSOCKDEFAULT;
    void *addr = NULL;
    socklen_t addrlen = 0;
#ifndef JANET_WINDOWS
    if (is_unix) {
        sock = socket(AF_UNIX, socktype | JSOCKFLAGS, 0);
        if (!JSOCKVALID(sock)) {
            janet_panicf("could not create socket: %V", janet_ev_lasterr());
        }
        addr = (void *) ai;
        addrlen = sizeof(struct sockaddr_un);
    } else
#endif
    {
        struct addrinfo *rp = NULL;
        for (rp = ai; rp != NULL; rp = rp->ai_next) {
#ifdef JANET_WINDOWS
            sock = WSASocketW(rp->ai_family, rp->ai_socktype | JSOCKFLAGS, rp->ai_protocol, NULL, 0, WSA_FLAG_OVERLAPPED);
#else
            sock = socket(rp->ai_family, rp->ai_socktype | JSOCKFLAGS, rp->ai_protocol);
#endif
            if (JSOCKVALID(sock)) {
                addr = rp->ai_addr;
                addrlen = (socklen_t) rp->ai_addrlen;
                break;
            }
        }
        if (NULL == addr) {
            freeaddrinfo(ai);
            janet_panicf("could not create socket: %V", janet_ev_lasterr());
        }
    }

    /* Connect to socket */
#ifdef JANET_WINDOWS
    int status = WSAConnect(sock, addr, addrlen, NULL, NULL, NULL, NULL);
    freeaddrinfo(ai);
#else
    int status = connect(sock, addr, addrlen);
    if (is_unix) {
        janet_free(ai);
    } else {
        freeaddrinfo(ai);
    }
#endif

    if (status == -1) {
        JSOCKCLOSE(sock);
        janet_panicf("could not connect to socket: %V", janet_ev_lasterr());
    }

    /* Set up the socket for non-blocking IO after connect - TODO - non-blocking connect? */
    janet_net_socknoblock(sock);

    /* Wrap socket in abstract type JanetStream */
    JanetStream *stream = make_stream(sock, JANET_STREAM_READABLE | JANET_STREAM_WRITABLE);
    return janet_wrap_abstract(stream);
}

static const char *serverify_socket(JSock sfd) {
    /* Set various socket options */
    int enable = 1;
    if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (char *) &enable, sizeof(int)) < 0) {
        return "setsockopt(SO_REUSEADDR) failed";
    }
#ifdef SO_REUSEPORT
    if (setsockopt(sfd, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(int)) < 0) {
        return "setsockopt(SO_REUSEPORT) failed";
    }
#endif
    janet_net_socknoblock(sfd);
    return NULL;
}

#ifdef JANET_WINDOWS
#define JANET_SHUTDOWN_RW SD_BOTH
#define JANET_SHUTDOWN_R SD_RECEIVE
#define JANET_SHUTDOWN_W SD_SEND
#else
#define JANET_SHUTDOWN_RW SHUT_RDWR
#define JANET_SHUTDOWN_R SHUT_RD
#define JANET_SHUTDOWN_W SHUT_WR
#endif

JANET_CORE_FN(cfun_net_shutdown,
              "(net/shutdown stream &opt mode)",
              "Stop communication on this socket in a graceful manner, either in both directions or just "
              "reading/writing from the stream. The `mode` parameter controls which communication to stop on the socket. "
              "\n\n* `:wr` is the default and prevents both reading new data from the socket and writing new data to the socket.\n"
              "* `:r` disables reading new data from the socket.\n"
              "* `:w` disable writing data to the socket.\n\n"
              "Returns the original socket.") {
    janet_arity(argc, 1, 2);
    JanetStream *stream = janet_getabstract(argv, 0, &janet_stream_type);
    janet_stream_flags(stream, JANET_STREAM_SOCKET);
    int shutdown_type = JANET_SHUTDOWN_RW;
    if (argc == 2) {
        const uint8_t *kw = janet_getkeyword(argv, 1);
        if (0 == janet_cstrcmp(kw, "rw")) {
            shutdown_type = JANET_SHUTDOWN_RW;
        } else if (0 == janet_cstrcmp(kw, "r")) {
            shutdown_type = JANET_SHUTDOWN_R;
        } else if (0 == janet_cstrcmp(kw, "w")) {
            shutdown_type = JANET_SHUTDOWN_W;
        } else {
            janet_panicf("unexpected keyword %v", argv[1]);
        }
    }
    int status;
#ifdef JANET_WINDOWS
    status = shutdown((SOCKET) stream->handle, shutdown_type);
#else
    do {
        status = shutdown(stream->handle, shutdown_type);
    } while (status == -1 && errno == EINTR);
#endif
    if (status) {
        janet_panicf("could not shutdown socket: %V", janet_ev_lasterr());
    }
    return argv[0];
}

JANET_CORE_FN(cfun_net_listen,
              "(net/listen host port &opt type)",
              "Creates a server. Returns a new stream that is neither readable nor "
              "writeable. Use net/accept or net/accept-loop be to handle connections and start the server. "
              "The type parameter specifies the type of network connection, either "
              "a :stream (usually tcp), or :datagram (usually udp). If not specified, the default is "
              ":stream. The host and port arguments are the same as in net/address.") {
    janet_arity(argc, 2, 3);

    /* Get host, port, and handler*/
    int socktype = janet_get_sockettype(argv, argc, 2);
    int is_unix = 0;
    struct addrinfo *ai = janet_get_addrinfo(argv, 0, socktype, 1, &is_unix);

    JSock sfd = JSOCKDEFAULT;
#ifndef JANET_WINDOWS
    if (is_unix) {
        sfd = socket(AF_UNIX, socktype | JSOCKFLAGS, 0);
        if (!JSOCKVALID(sfd)) {
            janet_free(ai);
            janet_panicf("could not create socket: %V", janet_ev_lasterr());
        }
        const char *err = serverify_socket(sfd);
        if (NULL != err || bind(sfd, (struct sockaddr *)ai, sizeof(struct sockaddr_un))) {
            JSOCKCLOSE(sfd);
            janet_free(ai);
            if (err) {
                janet_panic(err);
            } else {
                janet_panicf("could not bind socket: %V", janet_ev_lasterr());
            }
        }
        janet_free(ai);
    } else
#endif
    {
        /* Check all addrinfos in a loop for the first that we can bind to. */
        struct addrinfo *rp = NULL;
        for (rp = ai; rp != NULL; rp = rp->ai_next) {
#ifdef JANET_WINDOWS
            sfd = WSASocketW(rp->ai_family, rp->ai_socktype | JSOCKFLAGS, rp->ai_protocol, NULL, 0, WSA_FLAG_OVERLAPPED);
#else
            sfd = socket(rp->ai_family, rp->ai_socktype | JSOCKFLAGS, rp->ai_protocol);
#endif
            if (!JSOCKVALID(sfd)) continue;
            const char *err = serverify_socket(sfd);
            if (NULL != err) {
                JSOCKCLOSE(sfd);
                continue;
            }
            /* Bind */
            if (bind(sfd, rp->ai_addr, (int) rp->ai_addrlen) == 0) break;
            JSOCKCLOSE(sfd);
        }
        freeaddrinfo(ai);
        if (NULL == rp) {
            janet_panic("could not bind to any sockets");
        }
    }

    if (socktype == SOCK_DGRAM) {
        /* Datagram server (UDP) */
        JanetStream *stream = make_stream(sfd, JANET_STREAM_UDPSERVER | JANET_STREAM_READABLE);
        return janet_wrap_abstract(stream);
    } else {
        /* Stream server (TCP) */

        /* listen */
        int status = listen(sfd, 1024);
        if (status) {
            JSOCKCLOSE(sfd);
            janet_panicf("could not listen on file descriptor: %V", janet_ev_lasterr());
        }

        /* Put sfd on our loop */
        JanetStream *stream = make_stream(sfd, JANET_STREAM_ACCEPTABLE);
        return janet_wrap_abstract(stream);
    }
}

#define SO_MAX(a, b) (((a) > (b))? (a) : (b))
#define SA_PORT_NONE (&(in_port_t){ 0 })
#define SO_MIN(a, b) (((a) < (b))? (a) : (b))
#define SA_ADDRSTRLEN SO_MAX(INET6_ADDRSTRLEN, (sizeof ((struct sockaddr_un *)0)->sun_path) + 1)
#define sa_ntoa(sa)  sa_ntoa_((char [SA_ADDRSTRLEN]){ 0 }, SA_ADDRSTRLEN, (sa))
#define sa_aton(str) sa_aton_(&(struct sockaddr_storage){ 0 }, sizeof (struct sockaddr_storage), (str))
#define sa_family(...) sa_family(__VA_ARGS__)
#define sa_port(...) sa_port(__VA_ARGS__)

union sockaddr_arg {
	struct sockaddr *sa;
	const struct sockaddr *c_sa;

	struct sockaddr_storage *ss;
	struct sockaddr_storage *c_ss;

	struct sockaddr_in *sin;
	struct sockaddr_in *c_sin;

	struct sockaddr_in6 *sin6;
	struct sockaddr_in6 *c_sin6;

	struct sockaddr_un *sun;
	struct sockaddr_un *c_sun;
	union sockaddr_any *any;
	union sockaddr_any *c_any;

	void *ptr;
	void *c_ptr;
};

union sockaddr_any {
	struct sockaddr sa;
	struct sockaddr_storage ss;
	struct sockaddr_in sin;
	struct sockaddr_in6 sin6;
	struct sockaddr_un sun;
};

static inline union sockaddr_arg sockaddr_ref(void* arg) {
	return (union sockaddr_arg){ arg };
}

static inline sa_family_t *(sa_family)(void* arg) {
	return &sockaddr_ref(arg).sa->sa_family;
}

static inline in_port_t *(sa_port)(void* arg, const in_port_t *def, int *error) {
	switch (*sa_family(arg)) {
	case AF_INET:
		return &sockaddr_ref(arg).sin->sin_port;
	case AF_INET6:
		return &sockaddr_ref(arg).sin6->sin6_port;
	default:
		if (error)
			*error = EAFNOSUPPORT;

		return (in_port_t *)def;
	}
}

size_t janet_socket_strlcpy(char *dst, const char *src, size_t lim) {
	char *d		= dst;
	char *e		= &dst[lim];
	const char *s	= src;

	if (d < e) {
		do {
			if ('\0' == (*d++ = *s++))
				return s - src - 1;
		} while (d < e);

		d[-1]	= '\0';
	}

	while (*s++ != '\0')
		;;

	return s - src - 1;
}

char *sa_ntop(char *dst, size_t lim, const void *src, const char *def, int *_error) {
	union sockaddr_any *any = (void *)src;
	const char *unspec = "0.0.0.0";
	char text[SA_ADDRSTRLEN];
	int error;

	switch (*sa_family(&any->sa)) {
	case AF_INET:
		unspec = "0.0.0.0";

		if (!inet_ntop(AF_INET, &any->sin.sin_addr, text, sizeof text))
			goto syerr;

		break;
	case AF_INET6:
		unspec = "::";

		if (!inet_ntop(AF_INET6, &any->sin6.sin6_addr, text, sizeof text))
			goto syerr;

		break;
	case AF_UNIX:
		unspec = "/nonexistent";

		memset(text, 0, sizeof text);
		memcpy(text, any->sun.sun_path, SO_MIN(sizeof text - 1, sizeof any->sun.sun_path));

		break;
	default:
		error = EAFNOSUPPORT;

		goto error;
	}

	if (janet_socket_strlcpy(dst, text, lim) >= lim) {
		error = ENOSPC;

		goto error;
	}

	return dst;
syerr:
	error = errno;
error:
	if (_error)
		*_error = error;

	/*
	 * NOTE: Always write something in case caller ignores errors, such
	 * as when caller is using the sa_ntoa() macro.
	 */
	safe_memcpy(dst, (def)? def : unspec, lim);

	return (char *)def;
}

void *sa_pton(void *, size_t, const char *, const void *, int *);

static inline char *sa_ntoa_(char *dst, size_t lim, const void *src) {
	return sa_ntop(dst, lim, src, NULL, &(int){ 0 }), dst;
}

/*
static inline void *sa_aton_(void *dst, size_t lim, const char *src) {
	return sa_pton(dst, lim, src, NULL, &(int){ 0 }), dst;
}
*/

static Janet janet_so_getname(const struct sockaddr_storage *ss, socklen_t slen) {
    uint8_t *hn = NULL;
    uint16_t hp = 0;
    size_t plen = SA_ADDRSTRLEN;

    switch(ss->ss_family) {
        case AF_INET:
            /* fall through */
        case AF_INET6:
            /* hn = hostname, hp = hostport */
            hn = (uint8_t *)sa_ntoa(ss);
            hp = ntohs(*sa_port((void *)ss, SA_PORT_NONE, NULL));
            break;
        case AF_UNIX:
            /* support nameless sockets, linux-ism */
            if (slen > offsetof(struct sockaddr_un, sun_path)) {
                struct sockaddr_un *sun = (struct sockaddr_un *)ss;
                char *pe = (char *)sun + SO_MIN(sizeof *sun, slen);
                size_t plen;

                while (pe > sun->sun_path && pe[-1] == '\0')
                    --pe;

                if ((plen = pe - sun->sun_path) > 0) {
                    hn = (uint8_t *)sun->sun_path;
                } else {
                    hn = (uint8_t *)"@";
                    plen = 1;
                }
            } else {
                hn = (uint8_t *)"@";
                plen = 1;
            }
            break;
        default:
            hn = (uint8_t *)"";
            plen = 0;
            break;
    }

    Janet name[2];
    int32_t len = 1;
    name[0] = janet_wrap_string(janet_cstring((const char *)hn));
    if (hp > 0) {
        len++;
        name[1] = janet_wrap_integer(hp);
    }

    return janet_wrap_tuple(janet_tuple_n(name, len));
}

JANET_CORE_FN(cfun_net_getsockname,
              "(net/localname stream)",
              "Document me!") {
    janet_arity(argc, 1, 1);
    JanetStream *js = janet_getabstract(argv, 0, &janet_stream_type);
    struct sockaddr_storage ss;
    socklen_t slen = sizeof ss;
    memset(&ss, 0, slen);

    int error;
    if(0 != (error = getsockname(js->handle, (struct sockaddr *) &ss, &slen)))
        janet_panicf("Failed to get peername on fd %d, error: %s", js->handle, janet_ev_lasterr());

    return janet_so_getname(&ss, slen);
}


JANET_CORE_FN(cfun_net_getpeername,
              "(net/peername stream)",
              "Document me!") {
    janet_arity(argc, 1, 1);
    JanetStream *js = janet_getabstract(argv, 0, &janet_stream_type);
    struct sockaddr_storage ss;
    socklen_t slen = sizeof ss;
	memset(&ss, 0, slen);

    int error;
    if (0 != (error = getpeername(js->handle, (struct sockaddr *)&ss, &slen))) {
        janet_panicf("Failed to get peername on fd %d, error: %s", js->handle, janet_ev_lasterr());
    }

    return janet_so_getname(&ss, slen);
}

JANET_CORE_FN(cfun_stream_accept_loop,
              "(net/accept-loop stream handler)",
              "Shorthand for running a server stream that will continuously accept new connections. "
              "Blocks the current fiber until the stream is closed, and will return the stream.") {
    janet_fixarity(argc, 2);
    JanetStream *stream = janet_getabstract(argv, 0, &janet_stream_type);
    janet_stream_flags(stream, JANET_STREAM_ACCEPTABLE | JANET_STREAM_SOCKET);
    JanetFunction *fun = janet_getfunction(argv, 1);
    janet_sched_accept(stream, fun);
}

JANET_CORE_FN(cfun_stream_accept,
              "(net/accept stream &opt timeout)",
              "Get the next connection on a server stream. This would usually be called in a loop in a dedicated fiber. "
              "Takes an optional timeout in seconds, after which will return nil. "
              "Returns a new duplex stream which represents a connection to the client.") {
    janet_arity(argc, 1, 2);
    JanetStream *stream = janet_getabstract(argv, 0, &janet_stream_type);
    janet_stream_flags(stream, JANET_STREAM_ACCEPTABLE | JANET_STREAM_SOCKET);
    double to = janet_optnumber(argv, argc, 1, INFINITY);
    if (to != INFINITY) janet_addtimeout(to);
    janet_sched_accept(stream, NULL);
}

JANET_CORE_FN(cfun_stream_read,
              "(net/read stream nbytes &opt buf timeout)",
              "Read up to n bytes from a stream, suspending the current fiber until the bytes are available. "
              "`n` can also be the keyword `:all` to read into the buffer until end of stream. "
              "If less than n bytes are available (and more than 0), will push those bytes and return early. "
              "Takes an optional timeout in seconds, after which will return nil. "
              "Returns a buffer with up to n more bytes in it, or raises an error if the read failed.") {
    janet_arity(argc, 2, 4);
    JanetStream *stream = janet_getabstract(argv, 0, &janet_stream_type);
    janet_stream_flags(stream, JANET_STREAM_READABLE | JANET_STREAM_SOCKET);
    JanetBuffer *buffer = janet_optbuffer(argv, argc, 2, 10);
    double to = janet_optnumber(argv, argc, 3, INFINITY);
    if (janet_keyeq(argv[1], "all")) {
        if (to != INFINITY) janet_addtimeout(to);
        janet_ev_recvchunk(stream, buffer, INT32_MAX, MSG_NOSIGNAL);
    } else {
        int32_t n = janet_getnat(argv, 1);
        if (to != INFINITY) janet_addtimeout(to);
        janet_ev_recv(stream, buffer, n, MSG_NOSIGNAL);
    }
    janet_await();
}

JANET_CORE_FN(cfun_stream_chunk,
              "(net/chunk stream nbytes &opt buf timeout)",
              "Same a net/read, but will wait for all n bytes to arrive rather than return early. "
              "Takes an optional timeout in seconds, after which will return nil.") {
    janet_arity(argc, 2, 4);
    JanetStream *stream = janet_getabstract(argv, 0, &janet_stream_type);
    janet_stream_flags(stream, JANET_STREAM_READABLE | JANET_STREAM_SOCKET);
    int32_t n = janet_getnat(argv, 1);
    JanetBuffer *buffer = janet_optbuffer(argv, argc, 2, 10);
    double to = janet_optnumber(argv, argc, 3, INFINITY);
    if (to != INFINITY) janet_addtimeout(to);
    janet_ev_recvchunk(stream, buffer, n, MSG_NOSIGNAL);
    janet_await();
}

JANET_CORE_FN(cfun_stream_recv_from,
              "(net/recv-from stream nbytes buf &opt timoeut)",
              "Receives data from a server stream and puts it into a buffer. Returns the socket-address the "
              "packet came from. Takes an optional timeout in seconds, after which will return nil.") {
    janet_arity(argc, 3, 4);
    JanetStream *stream = janet_getabstract(argv, 0, &janet_stream_type);
    janet_stream_flags(stream, JANET_STREAM_UDPSERVER | JANET_STREAM_SOCKET);
    int32_t n = janet_getnat(argv, 1);
    JanetBuffer *buffer = janet_getbuffer(argv, 2);
    double to = janet_optnumber(argv, argc, 3, INFINITY);
    if (to != INFINITY) janet_addtimeout(to);
    janet_ev_recvfrom(stream, buffer, n, MSG_NOSIGNAL);
    janet_await();
}

JANET_CORE_FN(cfun_stream_write,
              "(net/write stream data &opt timeout)",
              "Write data to a stream, suspending the current fiber until the write "
              "completes. Takes an optional timeout in seconds, after which will return nil. "
              "Returns nil, or raises an error if the write failed.") {
    janet_arity(argc, 2, 3);
    JanetStream *stream = janet_getabstract(argv, 0, &janet_stream_type);
    janet_stream_flags(stream, JANET_STREAM_WRITABLE | JANET_STREAM_SOCKET);
    double to = janet_optnumber(argv, argc, 2, INFINITY);
    if (janet_checktype(argv[1], JANET_BUFFER)) {
        if (to != INFINITY) janet_addtimeout(to);
        janet_ev_send_buffer(stream, janet_getbuffer(argv, 1), MSG_NOSIGNAL);
    } else {
        JanetByteView bytes = janet_getbytes(argv, 1);
        if (to != INFINITY) janet_addtimeout(to);
        janet_ev_send_string(stream, bytes.bytes, MSG_NOSIGNAL);
    }
    janet_await();
}

JANET_CORE_FN(cfun_stream_send_to,
              "(net/send-to stream dest data &opt timeout)",
              "Writes a datagram to a server stream. dest is a the destination address of the packet. "
              "Takes an optional timeout in seconds, after which will return nil. "
              "Returns stream.") {
    janet_arity(argc, 3, 4);
    JanetStream *stream = janet_getabstract(argv, 0, &janet_stream_type);
    janet_stream_flags(stream, JANET_STREAM_UDPSERVER | JANET_STREAM_SOCKET);
    void *dest = janet_getabstract(argv, 1, &janet_address_type);
    double to = janet_optnumber(argv, argc, 3, INFINITY);
    if (janet_checktype(argv[2], JANET_BUFFER)) {
        if (to != INFINITY) janet_addtimeout(to);
        janet_ev_sendto_buffer(stream, janet_getbuffer(argv, 2), dest, MSG_NOSIGNAL);
    } else {
        JanetByteView bytes = janet_getbytes(argv, 2);
        if (to != INFINITY) janet_addtimeout(to);
        janet_ev_sendto_string(stream, bytes.bytes, dest, MSG_NOSIGNAL);
    }
    janet_await();
}

JANET_CORE_FN(cfun_stream_flush,
              "(net/flush stream)",
              "Make sure that a stream is not buffering any data. This temporarily disables Nagle's algorithm. "
              "Use this to make sure data is sent without delay. Returns stream.") {
    janet_fixarity(argc, 1);
    JanetStream *stream = janet_getabstract(argv, 0, &janet_stream_type);
    janet_stream_flags(stream, JANET_STREAM_WRITABLE | JANET_STREAM_SOCKET);
    /* Toggle no delay flag */
    int flag = 1;
    setsockopt((JSock) stream->handle, IPPROTO_TCP, TCP_NODELAY, (char *) &flag, sizeof(int));
    flag = 0;
    setsockopt((JSock) stream->handle, IPPROTO_TCP, TCP_NODELAY, (char *) &flag, sizeof(int));
    return argv[0];
}

static const JanetMethod net_stream_methods[] = {
    {"chunk", cfun_stream_chunk},
    {"close", janet_cfun_stream_close},
    {"read", cfun_stream_read},
    {"write", cfun_stream_write},
    {"flush", cfun_stream_flush},
    {"accept", cfun_stream_accept},
    {"accept-loop", cfun_stream_accept_loop},
    {"send-to", cfun_stream_send_to},
    {"recv-from", cfun_stream_recv_from},
    {"evread", janet_cfun_stream_read},
    {"evchunk", janet_cfun_stream_chunk},
    {"evwrite", janet_cfun_stream_write},
    {"shutdown", cfun_net_shutdown},
    {NULL, NULL}
};

static JanetStream *make_stream(JSock handle, uint32_t flags) {
    return janet_stream((JanetHandle) handle, flags | JANET_STREAM_SOCKET, net_stream_methods);
}


void janet_lib_net(JanetTable *env) {
    JanetRegExt net_cfuns[] = {
        JANET_CORE_REG("net/address", cfun_net_sockaddr),
        JANET_CORE_REG("net/listen", cfun_net_listen),
        JANET_CORE_REG("net/accept", cfun_stream_accept),
        JANET_CORE_REG("net/accept-loop", cfun_stream_accept_loop),
        JANET_CORE_REG("net/read", cfun_stream_read),
        JANET_CORE_REG("net/chunk", cfun_stream_chunk),
        JANET_CORE_REG("net/write", cfun_stream_write),
        JANET_CORE_REG("net/send-to", cfun_stream_send_to),
        JANET_CORE_REG("net/recv-from", cfun_stream_recv_from),
        JANET_CORE_REG("net/flush", cfun_stream_flush),
        JANET_CORE_REG("net/connect", cfun_net_connect),
        JANET_CORE_REG("net/shutdown", cfun_net_shutdown),
        JANET_CORE_REG("net/peername", cfun_net_getpeername),
        JANET_CORE_REG("net/localname", cfun_net_getsockname),
        JANET_REG_END
    };
    janet_core_cfuns_ext(env, NULL, net_cfuns);
}

void janet_net_init(void) {
#ifdef JANET_WINDOWS
    WSADATA wsaData;
    janet_assert(!WSAStartup(MAKEWORD(2, 2), &wsaData), "could not start winsock");
#endif
}

void janet_net_deinit(void) {
#ifdef JANET_WINDOWS
    WSACleanup();
#endif
}

#endif
