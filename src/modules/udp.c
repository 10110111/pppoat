/* modules/udp.c
 * PPP over Any Transport -- UDP transport
 *
 * Copyright (C) 2012-2015 Dmitry Podgorny <pasis.ua@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "trace.h"
#include "conf.h"
#include "log.h"
#include "memory.h"
#include "pppoat.h"
#include "util.h"

#define UDP_PORT_MASTER 0xc001
#define UDP_PORT_SLAVE  0xc001
#define UDP_HOST_MASTER "192.168.4.1"
#define UDP_HOST_SLAVE  "192.168.4.10"

struct pppoat_udp_ctx {
	pppoat_node_type_t  uc_type;
	struct addrinfo    *uc_ainfo;
	int                 uc_sock;
};

static int udp_ainfo_get(struct addrinfo **ainfo,
			 const char       *host,
			 unsigned short    port)
{
	struct addrinfo hints;
	char            service[6];
	int             rc;

	memset(&hints, 0, sizeof(hints));
	hints.ai_flags    = host == NULL ? AI_PASSIVE : 0;
#ifdef AI_ADDRCONFIG
	hints.ai_flags   |= AI_ADDRCONFIG;
#endif /* AI_ADDRCONFIG */
	hints.ai_family   = AF_UNSPEC;
	hints.ai_protocol = IPPROTO_UDP;
	hints.ai_socktype = SOCK_DGRAM;

	snprintf(service, sizeof(service), "%u", port);
	rc = getaddrinfo(host, service, &hints, ainfo);
	if (rc != 0) {
		pppoat_error("udp", "getaddrinfo rc=%d: %s",
			     rc, gai_strerror(rc));
		rc = P_ERR(-ENOPROTOOPT);
		*ainfo = NULL;
	}
	return rc;
}

static void udp_ainfo_put(struct addrinfo *ainfo)
{
	freeaddrinfo(ainfo);
}

static int udp_sock_new(unsigned short port, int *sock)
{
	struct addrinfo *ainfo;
	int              rc;

	rc = udp_ainfo_get(&ainfo, NULL, port);
	if (rc == 0) {
		*sock = socket(ainfo->ai_family, ainfo->ai_socktype,
			       ainfo->ai_protocol);
		rc = *sock < 0 ? P_ERR(-errno) : 0;
	}
	if (rc == 0) {
		rc = bind(*sock, ainfo->ai_addr, ainfo->ai_addrlen);
		rc = rc != 0 ? P_ERR(-errno) : 0;
		if (rc != 0)
			(void)close(*sock);
	}
	if (ainfo != NULL) {
		udp_ainfo_put(ainfo);
	}
	return rc;
}

static int module_udp_init(struct pppoat_conf *conf, void **userdata)
{
	struct pppoat_udp_ctx *ctx;
	pppoat_node_type_t     type;
	unsigned short         sport;
	unsigned short         dport;
	const char            *dhost;
	const char            *opt;
	int                    rc;

	opt = pppoat_conf_get(conf, "server");
	type = opt != NULL && pppoat_conf_obj_is_true(opt) ?
	       PPPOAT_NODE_MASTER : PPPOAT_NODE_SLAVE;

	/* XXX use hardcoded config for now */
	if (type == PPPOAT_NODE_MASTER) {
		sport = UDP_PORT_MASTER;
		dport = UDP_PORT_SLAVE;
		dhost = UDP_HOST_SLAVE;
	} else {
		sport = UDP_PORT_SLAVE;
		dport = UDP_PORT_MASTER;
		dhost = UDP_HOST_MASTER;
	}

	ctx = pppoat_alloc(sizeof(*ctx));
	rc  = ctx == NULL ? P_ERR(-ENOMEM) : 0;
	if (rc == 0) {
		ctx->uc_type = type;
		rc = udp_ainfo_get(&ctx->uc_ainfo, dhost, dport);
		rc = rc ?: udp_sock_new(sport, &ctx->uc_sock);
		if (rc != 0) {
			if (ctx->uc_ainfo != NULL)
				udp_ainfo_put(ctx->uc_ainfo);
			pppoat_free(ctx);
		}
	}
	if (rc == 0) {
		*userdata = ctx;
	}
	return rc;
}

static void module_udp_fini(void *userdata)
{
	struct pppoat_udp_ctx *ctx = userdata;

	(void)close(ctx->uc_sock);
	udp_ainfo_put(ctx->uc_ainfo);
	pppoat_free(ctx);
}

static bool udp_error_is_recoverable(int error)
{
	return (error == -EAGAIN ||
		error == -EINTR  ||
		error == -EWOULDBLOCK);
}

static int udp_buf_send(struct pppoat_udp_ctx *ctx,
			unsigned char         *buf,
			ssize_t                len)
{
	struct addrinfo *ainfo = ctx->uc_ainfo;
	ssize_t          len2  = 0;
	fd_set           wfds;
	int              rc    = 0;

	do {
		len2 = sendto(ctx->uc_sock, buf, len, 0, ainfo->ai_addr,
			      ainfo->ai_addrlen);
		if (len2 < 0 && errno == EINTR)
			continue;
		if (len2 < 0 && !udp_error_is_recoverable(-errno))
			rc = P_ERR(-errno);
		if (len2 < 0 && udp_error_is_recoverable(-errno)) {
			FD_ZERO(&wfds);
			FD_SET(ctx->uc_sock, &wfds);
			rc = pppoat_util_select(ctx->uc_sock, NULL, &wfds);
		}
		if (len2 > 0) {
			buf += len2;
			len -= len2;
		}
	} while (rc == 0 && len > 0);

	return rc;
}

static int module_udp_run(int rd, int wr, int ctrl, void *userdata)
{
	struct pppoat_udp_ctx *ctx = userdata;
	unsigned char          buf[4096]; /* XXX allocate during init */
	ssize_t                len;
	fd_set                 rfds;
	int                    sock = ctx->uc_sock;
	int                    max;
	int                    rc = 0;

	rc = pppoat_util_fd_nonblock_set(rd,   true)
	  ?: pppoat_util_fd_nonblock_set(sock, true);

	while (rc == 0) {
		FD_ZERO(&rfds);
		FD_SET(rd,   &rfds);
		FD_SET(sock, &rfds);
		max = pppoat_max(rd, sock);
		rc  = pppoat_util_select(max, &rfds, NULL);
		rc  = rc > 0 ? 0 : rc;

		if (FD_ISSET(rd, &rfds)) {
			len = read(rd, buf, sizeof(buf));
			if (len == 0)
				rc = P_ERR(-EPIPE);
			if (len < 0 && !udp_error_is_recoverable(-errno))
				rc = P_ERR(-errno);
			if (len > 0)
				rc = udp_buf_send(ctx, buf, len);
		}
		if (rc == 0 && FD_ISSET(sock, &rfds)) {
			/* XXX use recvfrom() */
			len = recv(sock, buf, sizeof(buf), 0);
			if (len < 0 && !udp_error_is_recoverable(-errno))
				rc = P_ERR(-errno);
			if (len > 0)
				rc = pppoat_util_write(wr, buf, (size_t)len);
		}
	}
	return rc;
}

const struct pppoat_module pppoat_module_udp = {
	.m_name  = "udp",
	.m_descr = "PPP over UDP",
	.m_init  = &module_udp_init,
	.m_fini  = &module_udp_fini,
	.m_run   = &module_udp_run,
};
