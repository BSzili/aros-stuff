#ifndef __NETDB_WRAPPER_H
#define __NETDB_WRAPPER_H

#ifndef __AROS__
#include "../addrinfo.h"
#endif
#ifdef __MORPHOS__
#include <exec/types.h>
typedef LONG socklen_t;
#endif
#include_next "netdb.h"

#ifdef __cplusplus
extern "C" {
#endif

extern int
getaddrinfo(const char *host, const char *serv,
			const struct addrinfo *hintsptr, struct addrinfo **result);

extern void
freeaddrinfo(struct addrinfo *aihead);

extern int
getnameinfo(const struct sockaddr *sa, socklen_t salen,
		    char *host, size_t hostlen,
			char *serv, size_t servlen, int flags);

extern char *
gai_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif
