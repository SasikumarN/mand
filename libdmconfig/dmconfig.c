/*
 *    __                        __      _
 *   / /__________ __   _____  / /___  (_)___  ____ _
 *  / __/ ___/ __ `/ | / / _ \/ / __ \/ / __ \/ __ `/
 * / /_/ /  / /_/ /| |/ /  __/ / /_/ / / / / / /_/ /
 * \__/_/   \__,_/ |___/\___/_/ .___/_/_/ /_/\__, /
 *                           /_/            /____/
 *
 * (c) Travelping GmbH <info@travelping.com>
 *
 */

/*
 * dmconfig library
 * based on diammsg - diameter protocol subset to encode/decode diameter packets
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <time.h>
#include <arpa/inet.h>

#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <poll.h>
#include <fcntl.h>
#include <event.h>
#include <syslog.h>
#include <signal.h>

#ifdef LIBDMCONFIG_DEBUG
#include "debug.h"
#endif

#include <talloc/talloc.h>

#include "diammsg.h"
#include "codes.h"
#include "dmconfig.h"

#include "utils/logx.h"
#include "utils/binary.h"

int dmconfig_debug_level = 1;

#define debug(format, ...)						\
	do {								\
		struct timeval tv;					\
		int _errno = errno;					\
									\
		gettimeofday(&tv, NULL);				\
		logx(LOG_DEBUG, "%ld.%06ld: %s" format, tv.tv_sec, tv.tv_usec, __FUNCTION__, ## __VA_ARGS__); \
		errno = _errno;						\
	} while (0)

static inline void postprocessRequest(COMMCONTEXT *ctx);

static void connectEvent(int fd, short event, void *arg);
static void writeEvent(int fd, short event, void *arg);
static void readEvent(int fd, short event, void *arg);

static inline int process_active_notification(DMCONTEXT *dmCtx);
static inline int process_fwupdate_finish(DMCONTEXT *dmCtx);
static inline int process_fwupdate_progress(DMCONTEXT *dmCtx);
static inline int process_ping(DMCONTEXT *dmCtx);
static inline int process_ping_completed(DMCONTEXT *dmCtx);
static inline int process_traceroute(DMCONTEXT *dmCtx);
static inline int process_traceroute_completed(DMCONTEXT *dmCtx);
static inline int process_pcap_aborted(DMCONTEXT *dmCtx);

		/* callbacks used by the blocking API automatically */

static void generic_connectHandler(DMCONFIG_EVENT event,
				   DMCONTEXT *dmCtx __attribute__((unused)),
				   void *userdata);
static void generic_answerHandler(DMCONFIG_EVENT event __attribute__((unused)),
				  DMCONTEXT *dmCtx __attribute__((unused)),
				  void *user_data, uint32_t answer_rc,
				  DIAM_AVPGRP *answer_grp);

		/* global variables */

static uint32_t hopid = 0;
static uint32_t endid = 0;

#define aux_RET(STATUS, RC) {			\
	*status = STATUS;			\
	return RC;				\
}
#define aux_RET_SIG(STATUS, RC) {		\
	sigaction(SIGPIPE, &oldaction, NULL);	\
	*status = STATUS;			\
	return RC;				\
}

#define CALLBACK(WHERE, ...) {			\
	if ((WHERE)->callback)			\
		(WHERE)->callback(__VA_ARGS__);	\
}
#define NOTIFY_CALLBACK(EVENT, GRP) {					\
	dmCtx->callbacks.active_notification.callback(EVENT, dmCtx, 	\
		dmCtx->callbacks.active_notification.user_data, GRP);	\
}
#define FWUPDATE_FINISH_CALLBACK(EVENT, ...) {					\
	dmCtx->callbacks.fwupdate_feedback.finish_cb(EVENT, dmCtx,		\
		dmCtx->callbacks.fwupdate_feedback.finish_ud, __VA_ARGS__);	\
}
#define FWUPDATE_PROGRESS_CALLBACK(EVENT, ...) {				\
	dmCtx->callbacks.fwupdate_feedback.progress_cb(EVENT, dmCtx,		\
		dmCtx->callbacks.fwupdate_feedback.progress_ud,	__VA_ARGS__);	\
}
#define PING_CALLBACK(EVENT, ...) {					\
	dmCtx->callbacks.ping_feedback.ping_cb(EVENT, dmCtx,		\
		dmCtx->callbacks.ping_feedback.ping_ud, __VA_ARGS__);	\
}
#define PING_COMPLETED_CALLBACK(EVENT, ...) {					\
	dmCtx->callbacks.ping_feedback.completed_cb(EVENT, dmCtx,		\
		dmCtx->callbacks.ping_feedback.completed_ud, __VA_ARGS__);	\
}
#define TRACEROUTE_CALLBACK(EVENT, ...) {					\
	dmCtx->callbacks.traceroute_feedback.traceroute_cb(EVENT, dmCtx,	\
		dmCtx->callbacks.traceroute_feedback.traceroute_ud,		\
		__VA_ARGS__);							\
}
#define TRACEROUTE_COMPLETED_CALLBACK(EVENT, ...) {			\
	dmCtx->callbacks.traceroute_feedback.completed_cb(EVENT, dmCtx,	\
		dmCtx->callbacks.traceroute_feedback.completed_ud,	\
		__VA_ARGS__);						\
}
#define PCAP_ABORTED_CALLBACK(EVENT, ...) {					\
	dmCtx->callbacks.pcap_feedback.aborted_cb(EVENT, dmCtx,			\
		dmCtx->callbacks.pcap_feedback.aborted_ud, __VA_ARGS__);	\
}

		/* communication auxiliary functions */

		/* our buffer is already a DIAM_REQUEST structure (ctx->buffer starts at the PACKET part)
		   we only have to set the INFO structure */
static inline void
postprocessRequest(COMMCONTEXT *ctx)
{
	ctx->req->info.avpptr = (DIAM_AVP *)(ctx->buffer + sizeof(DIAM_PACKET));
	ctx->req->info.size = diam_packet_length(&ctx->req->packet) +
							sizeof(DIAM_REQUEST_INFO);
}

uint32_t
event_aux_diamRead(int fd, short event, COMMCONTEXT *readCtx,
		   uint8_t *alreadyRead, COMMSTATUS *status)
{
	ssize_t		length;
	uint32_t	bufsize;
	uint32_t	len;

	if (event == EV_TIMEOUT)
		aux_RET(CONNRESET, RC_ERR_CONNECTION);

	if (event != EV_READ)	/* a number of events can be ignored */
		aux_RET(INCOMPLETE, RC_OK);

				/* nothing or not enough read -> read as much as possible */

	if (!*alreadyRead) {
		*alreadyRead = 1;

		bufsize = readCtx->cAlloc * BUFFER_CHUNK_SIZE;
		do {
					/* don't reallocate if there's still space */
			if (readCtx->bytes == bufsize) {
				readCtx->cAlloc++;
				bufsize += BUFFER_CHUNK_SIZE;

					/* allocate a DIAM_REQUEST structure (we're reading into the REQUEST structure) */
				if (!(readCtx->req = talloc_realloc_size(NULL, readCtx->req,
									 sizeof(DIAM_REQUEST_INFO) + bufsize)))
					aux_RET(ERROR, RC_ERR_ALLOC);

				readCtx->buffer = (uint8_t*)readCtx->req +
							sizeof(DIAM_REQUEST_INFO);
			}

			do {
				if ((length = read(fd, readCtx->buffer + readCtx->bytes,
						   bufsize - readCtx->bytes)) == -1) {
					debug(": read error: %d (%m)", errno);
					switch (errno) {
					case EWOULDBLOCK:	/* happens if data to read is multiple of BUFFER_CHUNK_SIZE */
						length = 0;
						break;
					case EINTR:
						break;
					case ETIMEDOUT:
					case ECONNRESET:
						aux_RET(CONNRESET,
							RC_ERR_CONNECTION);
					default:
						aux_RET(ERROR, RC_ERR_CONNECTION);
					}
				}
				else if (!length)
					aux_RET(CONNRESET, RC_ERR_CONNECTION);
				/* if length is nevertheless 0, there was an EWOULDBLOCK */
			} while (length == -1);	/* errno is EINTR */

			readCtx->bytes += length;
			debug(": read length: %d, total: %d (from %d)", (int)length, readCtx->bytes, bufsize);
		} while (length && readCtx->bytes == bufsize);
	} else if (readCtx->bytes >= 4 &&
		   readCtx->bytes >=
		   	(len = diam_packet_length(&readCtx->req->packet))) {

		debug(": read continue, got total: %d, req: %d", readCtx->bytes, len);

		/* foremost request was also the last one */
		if (!(readCtx->bytes - len)) {
			debug(": remove request");
			talloc_free(readCtx->req);
			readCtx->req = NULL;
			readCtx->cAlloc = readCtx->bytes = 0;
			aux_RET(NOTHING, RC_OK);
		}

		/* remove request */
		readCtx->bytes -= len;
		memmove(readCtx->buffer, readCtx->buffer + len, readCtx->bytes);
	}

			/* process read data */

						/* less than a part of one request's header to process */
	if (readCtx->bytes < 4 ||
	    readCtx->bytes < diam_packet_length(&readCtx->req->packet))
		aux_RET(INCOMPLETE, RC_OK);

	postprocessRequest(readCtx);		/* one or more requests to process */
	aux_RET(COMPLETE, RC_OK);
}

uint32_t
event_aux_diamWrite(int fd, short event, COMMCONTEXT *writeCtx,
		    COMMSTATUS *status)
{
	ssize_t			length;

	struct sigaction	action, oldaction;

	if (event == EV_TIMEOUT)
		aux_RET(CONNRESET, RC_ERR_CONNECTION);

	if (event != EV_WRITE)
		aux_RET(INCOMPLETE, RC_OK);

	if (!writeCtx->buffer) {
		writeCtx->buffer = (uint8_t *)&writeCtx->req->packet;
		writeCtx->bytes = writeCtx->req->info.size -
						sizeof(DIAM_REQUEST_INFO);
	}

	memset(&action, 0, sizeof(struct sigaction));
	action.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &action, &oldaction);

	do {
		if ((length = write(fd, writeCtx->buffer,
				    writeCtx->bytes)) == -1)
			switch (errno) {
			case EAGAIN:	/* can only happen when it tries to write the second request in one write event */
				aux_RET_SIG(NOTHING, RC_OK);
			case EPIPE:
			case ECONNRESET:
				aux_RET_SIG(CONNRESET, RC_ERR_CONNECTION);
			case EINTR:
				break;
			default:
				aux_RET_SIG(ERROR, RC_ERR_CONNECTION);
			}
		else if (!length)
			aux_RET_SIG(CONNRESET, RC_ERR_CONNECTION);
	} while (length == -1);	/* errno is EINTR */

	writeCtx->buffer += length;
	writeCtx->bytes -= length;

	aux_RET_SIG((writeCtx->bytes ? INCOMPLETE : COMPLETE), RC_OK);
}

void
dm_free_requests(DMCONTEXT *dmCtx)
{
	talloc_free(dmCtx->requestlist_head);
	dmCtx->requestlist_head = NULL;

	if (event_pending(&dmCtx->writeCtx.event, EV_WRITE | EV_PERSIST, NULL))
		event_del(&dmCtx->writeCtx.event);
	if (event_pending(&dmCtx->readCtx.event, EV_READ | EV_PERSIST, NULL))
		event_del(&dmCtx->readCtx.event);

	dmCtx->writeCtx.buffer = NULL;
	if (dmCtx->readCtx.req) {
		talloc_free(dmCtx->readCtx.req);
		dmCtx->readCtx.req = NULL;
		dmCtx->readCtx.cAlloc = dmCtx->readCtx.bytes = 0;
	}
}

		/* callback register functions */

uint32_t
dm_register_connect_callback(DMCONTEXT *dmCtx, int type,
			     DMCONFIG_CONNECT_CALLBACK callback, void *userdata)
{
	CONNEVENTCTX		*ctx;
	struct timeval		timeout;

	int			fd = dm_context_get_socket(dmCtx);
	int			flags;

	struct sockaddr_un	sockaddr_un;
	struct sockaddr_in	sockaddr_in;

	struct sockaddr		*sockaddr;
	socklen_t		sockaddr_len;

	if ((flags = fcntl(fd, F_GETFL)) == -1 ||
	    fcntl(fd, F_SETFL, flags | O_NONBLOCK))
		return RC_ERR_CONNECTION;

	if (type == AF_UNIX) {
		memset(&sockaddr_un, 0, sizeof(sockaddr_un));

		sockaddr_un.sun_family = AF_UNIX;
		strncpy(sockaddr_un.sun_path + 1, SERVER_LOCAL,
			sizeof(sockaddr_un.sun_path) - 1);

		sockaddr = (struct sockaddr*)&sockaddr_un;
		sockaddr_len = sizeof(sockaddr_un);
	} else { /* AF_INET */
		memset(&sockaddr_in, 0, sizeof(sockaddr_in));

		sockaddr_in.sin_family = AF_INET;
		sockaddr_in.sin_port = htons(SERVER_PORT);
		sockaddr_in.sin_addr.s_addr = htonl(SERVER_IP);

		sockaddr = (struct sockaddr*)&sockaddr_in;
		sockaddr_len = sizeof(sockaddr_in);
	}

	while (connect(fd, sockaddr, sockaddr_len) == -1)
		if (errno == EINPROGRESS)
			break;
		else if (errno == EAGAIN)
			continue;
		else
			return RC_ERR_CONNECTION;

	if (!(ctx = talloc(NULL, CONNEVENTCTX)))
		return RC_ERR_ALLOC;

	ctx->callback = callback;
	ctx->user_data = userdata;
	ctx->dmCtx = dmCtx;

	event_set(&ctx->event, fd, EV_WRITE, connectEvent, ctx);
	event_base_set(dm_context_get_event_base(dmCtx), &ctx->event);

	timeout.tv_sec = TIMEOUT_WRITE_REQUESTS;	/* if the connect was already successful, there should be no delay */
	timeout.tv_usec = 0;

	if (event_add(&ctx->event, &timeout)) {
		talloc_free(ctx);
		return RC_ERR_ALLOC;
	}

	return RC_OK;
}

uint32_t
dm_generic_register_request(DMCONTEXT *dmCtx, uint32_t code, DIAM_AVPGRP *grp,
			    DMCONFIG_CALLBACK callback, void *callback_ud)
{
	DIAM_AVPGRP	*completegrp;

	REQUESTINFO	*new, *cur;

	struct timeval	timeout;

	switch (hopid) {		/* one never knows... */
	case 0:		srand((unsigned int)time(NULL));
	case MAX_INT:	hopid = endid = (float)rand()/RAND_MAX * (MAX_INT-1) + 1;
			break;
	default:	hopid = ++endid;
	}

	if (!dmCtx->requestlist_head) {
		if (!(dmCtx->requestlist_head = talloc(NULL, REQUESTINFO)))
			return RC_ERR_ALLOC;
		memset(dmCtx->requestlist_head, 0, sizeof(REQUESTINFO));
	}
	if (!(new = talloc(dmCtx->requestlist_head, REQUESTINFO))) {
		talloc_free(dmCtx->requestlist_head);
		return RC_ERR_ALLOC;
	}

	if (!(new->request = new_diam_request(new, code, CMD_FLAG_REQUEST,
					      APP_ID, hopid, endid))) {
		talloc_free(dmCtx->requestlist_head);
		return RC_ERR_ALLOC;
	}

	if (!(completegrp = new_diam_avpgrp(new->request)) ||
	    diam_avpgrp_add_uint32(new->request, &completegrp, AVP_SESSIONID, 0,
				   VP_TRAVELPING, dmCtx->sessionid) ||
	    (grp && diam_avpgrp_add_avpgrp(new->request, &completegrp,
	    				   AVP_CONTAINER, 0, VP_TRAVELPING,
					   grp)) ||
	    build_diam_request(new, &new->request, completegrp)) {
		talloc_free(dmCtx->requestlist_head);
		return RC_ERR_ALLOC;
	}
	talloc_free(completegrp);

#ifdef LIBDMCONFIG_DEBUG
	if (dmconfig_debug_level) {
		fprintf(stderr, "Send request:\n");
		dump_diam_packet(new->request);
	}
#endif

	new->callback = callback;
	new->user_data = callback_ud;
	new->dmCtx = dmCtx;
	new->status = REQUEST_SHALL_WRITE;
	new->code = code;
	new->hopid = hopid;
	new->next = NULL;

	for (cur = dmCtx->requestlist_head; cur->next; cur = cur->next);
	cur->next = new;

	if (!event_pending(&dmCtx->writeCtx.event, EV_WRITE | EV_PERSIST, NULL)) {
		event_set(&dmCtx->writeCtx.event, dmCtx->socket,
			  EV_WRITE | EV_PERSIST, writeEvent, dmCtx);
		event_base_set(dm_context_get_event_base(dmCtx), &dmCtx->writeCtx.event);

		timeout.tv_sec = TIMEOUT_WRITE_REQUESTS;
		timeout.tv_usec = 0;

		if (event_add(&dmCtx->writeCtx.event, &timeout)) {
			talloc_free(dmCtx->requestlist_head);
			return RC_ERR_ALLOC;
		}
	}

	return RC_OK;
}

uint32_t
dm_generic_register_request_bool_grp(DMCONTEXT *dmCtx, uint32_t code,
				     uint8_t bool, DIAM_AVPGRP *grp,
				     DMCONFIG_CALLBACK callback,
				     void *callback_ud)
{
	DIAM_AVPGRP	*new;
	uint32_t	rc;

	if (!(new = dm_grp_new()) ||
	    diam_avpgrp_add_uint8(NULL, &new, AVP_BOOL, 0, VP_TRAVELPING, bool) ||
	    diam_avpgrp_add_avpgrp(NULL, &new, AVP_CONTAINER, 0,
	    			   VP_TRAVELPING, grp)) {
		dm_grp_free(new);
		return RC_ERR_ALLOC;
	}

	rc = dm_generic_register_request(dmCtx, code, new, callback,
					 callback_ud);

	dm_grp_free(new);
	return rc;
}

uint32_t
dm_generic_register_request_uint32_timeouts(DMCONTEXT *dmCtx, uint32_t code,
					    uint32_t val, struct timeval *timeval1,
					    struct timeval *timeval2,
					    DMCONFIG_CALLBACK callback,
					    void *callback_ud)
{
	DIAM_AVPGRP	*grp;
	uint32_t	rc;

	if (!(grp = dm_grp_new()) ||
	    diam_avpgrp_add_uint32(NULL, &grp, AVP_UINT32, 0, VP_TRAVELPING, val) ||
	    (timeval1 &&
	     diam_avpgrp_add_timeval(NULL, &grp, AVP_TIMEOUT_SESSION, 0,
	     			     VP_TRAVELPING, *timeval1)) ||
	    (timeval2 &&
	     diam_avpgrp_add_timeval(NULL, &grp, AVP_TIMEOUT_REQUEST, 0,
	     			     VP_TRAVELPING, *timeval2))) {
		dm_grp_free(grp);
		return RC_ERR_ALLOC;
	}

	rc = dm_generic_register_request(dmCtx, code, grp, callback,
					 callback_ud);

	dm_grp_free(grp);
	return rc;
}

uint32_t
dm_generic_register_request_string(DMCONTEXT *dmCtx, uint32_t code,
				   const char *str, DMCONFIG_CALLBACK callback,
				   void *callback_ud)
{
	DIAM_AVPGRP	*grp;
	uint32_t	rc;

	if (!(grp = dm_grp_new()) ||
	    diam_avpgrp_add_string(NULL, &grp, AVP_STRING, 0,
				   VP_TRAVELPING, str)) {
		dm_grp_free(grp);
		return RC_ERR_ALLOC;
	}

	rc = dm_generic_register_request(dmCtx, code, grp, callback, callback_ud);

	dm_grp_free(grp);
	return rc;
}

uint32_t
dm_generic_register_request_path(DMCONTEXT *dmCtx, uint32_t code,
				 const char *path, DMCONFIG_CALLBACK callback,
				 void *callback_ud)
{
	DIAM_AVPGRP	*grp;
	uint32_t	rc;

	if (!(grp = dm_grp_new()) ||
	    diam_avpgrp_add_string(NULL, &grp, AVP_PATH, 0,
				   VP_TRAVELPING, path)) {
		dm_grp_free(grp);
		return RC_ERR_ALLOC;
	}

	rc = dm_generic_register_request(dmCtx, code, grp, callback, callback_ud);

	dm_grp_free(grp);
	return rc;
}

uint32_t
dm_generic_register_request_char_address(DMCONTEXT *dmCtx, uint32_t code,
					 const char *str, struct in_addr addr,
					 DMCONFIG_CALLBACK callback,
					 void *callback_ud)
{
	DIAM_AVPGRP	*grp;
	uint32_t	rc;

	if (!(grp = dm_grp_new()) ||
	    diam_avpgrp_add_string(NULL, &grp, AVP_STRING, 0,
				   VP_TRAVELPING, str) ||
	    diam_avpgrp_add_address(NULL, &grp, AVP_ADDRESS, 0, VP_TRAVELPING,
	    			    AF_INET, &addr)) {
		dm_grp_free(grp);
		return RC_ERR_ALLOC;
	}

	rc = dm_generic_register_request(dmCtx, code, grp, callback, callback_ud);

	dm_grp_free(grp);
	return rc;
}

		/* generic connection / send request functions (blocking API) */

uint32_t
dm_init_socket(DMCONTEXT *dmCtx, int type)
{
	uint32_t rc;

	if ((rc = dm_create_socket(dmCtx, type)))
		return rc;

	ev_now_update(dm_context_get_ev_loop(dmCtx));	/* workaround for libev time update problem
							   otherwise ev_now() time is updated only at ev_loop */

	if ((rc = dm_register_connect_callback(dmCtx, type,
					       generic_connectHandler, &rc)))
		return rc;

	if (event_base_dispatch(dm_context_get_event_base(dmCtx)) == -1)
		return RC_ERR_MISC;

	return rc;
}

		/* merely to return a code */
static void
generic_connectHandler(DMCONFIG_EVENT event,
		       DMCONTEXT *dmCtx __attribute__((unused)), void *userdata)
{
	uint32_t *rc = userdata;

	*rc = event == DMCONFIG_ERROR_CONNECTING ? RC_ERR_CONNECTION : RC_OK;
}


uint32_t
dm_generic_send_request(DMCONTEXT *dmCtx, uint32_t code, DIAM_AVPGRP *grp,
			DIAM_AVPGRP **ret)
{
	struct _result {
		uint32_t	rc;
		DIAM_AVPGRP	*grp;
		DIAM_AVPGRP	**ret;
	} result;
	uint32_t rc;

	ev_now_update(dm_context_get_ev_loop(dmCtx));	/* workaround for libev time update problem
							   otherwise ev_now() time is updated only at ev_loop */

	if ((rc = dm_generic_register_request(dmCtx, code, grp,
					      generic_answerHandler, &result)))
		return rc;

	result.rc = RC_ERR_CONNECTION;
	result.grp = grp;
	result.ret = ret;

	return event_base_dispatch(dm_context_get_event_base(dmCtx)) == -1 ?
							RC_ERR_MISC : result.rc;
}

uint32_t
dm_generic_send_request_bool_grp(DMCONTEXT *dmCtx, uint32_t code, uint8_t bool,
				 DIAM_AVPGRP *grp)
{
	DIAM_AVPGRP	*new;
	uint32_t	rc;

	if (!(new = dm_grp_new()) ||
	    diam_avpgrp_add_uint8(NULL, &new, AVP_BOOL, 0, VP_TRAVELPING, bool) ||
	    diam_avpgrp_add_avpgrp(NULL, &new, AVP_CONTAINER, 0,
	    			   VP_TRAVELPING, grp)) {
		dm_grp_free(new);
		return RC_ERR_ALLOC;
	}
	rc = dm_generic_send_request(dmCtx, code, new, NULL);

	dm_grp_free(new);
	return rc;
}

uint32_t
dm_generic_send_request_uint32_timeouts_get_grp(DMCONTEXT *dmCtx, uint32_t code,
						uint32_t val,
						struct timeval *timeval1,
						struct timeval *timeval2,
						DIAM_AVPGRP **ret)
{
	uint32_t	rc;
	DIAM_AVPGRP	*grp;

	if (!(grp = dm_grp_new()) ||
	    diam_avpgrp_add_uint32(NULL, &grp, AVP_UINT32, 0, VP_TRAVELPING, val) ||
	    (timeval1 &&
	     diam_avpgrp_add_timeval(NULL, &grp, AVP_TIMEOUT_SESSION, 0,
	     			     VP_TRAVELPING, *timeval1)) ||
	    (timeval2 &&
	     diam_avpgrp_add_timeval(NULL, &grp, AVP_TIMEOUT_REQUEST, 0,
	     			     VP_TRAVELPING, *timeval2))) {
		dm_grp_free(grp);
		return RC_ERR_ALLOC;
	}

	if ((rc = dm_generic_send_request(dmCtx, code, grp, ret))) {
		dm_grp_free(grp);
		return rc;
	}

	if (ret)
		talloc_steal(NULL, *ret);
	dm_grp_free(grp);

	return RC_OK;
}

uint32_t
dm_generic_send_request_string(DMCONTEXT *dmCtx, uint32_t code, const char *str)
{
	DIAM_AVPGRP	*grp;
	uint32_t	rc;

	if (!(grp = dm_grp_new()) ||
	    diam_avpgrp_add_string(NULL, &grp, AVP_STRING, 0, VP_TRAVELPING, str)) {
		dm_grp_free(grp);
		return RC_ERR_ALLOC;
	}

	rc = dm_generic_send_request(dmCtx, code, grp, NULL);

	dm_grp_free(grp);
	return rc;
}

uint32_t
dm_generic_send_request_path_get_grp(DMCONTEXT *dmCtx, uint32_t code,
				     const char *path, DIAM_AVPGRP **answer)
{
	DIAM_AVPGRP	*grp;
	uint32_t	rc;

	if (!(grp = dm_grp_new()) ||
	    diam_avpgrp_add_string(NULL, &grp, AVP_PATH, 0, VP_TRAVELPING, path)) {
		dm_grp_free(grp);
		return RC_ERR_ALLOC;
	}
	if ((rc = dm_generic_send_request(dmCtx, code, grp, answer))) {
		dm_grp_free(grp);
		return rc;
	}

	if (answer)
		talloc_steal(NULL, *answer);
	dm_grp_free(grp);

	return RC_OK;
}

uint32_t
dm_generic_send_request_path_get_char(DMCONTEXT *dmCtx, uint32_t code,
				      const char *path, char **data)
{
	DIAM_AVPGRP	*ret;
	uint32_t	rc;

	if ((rc = dm_generic_send_request_path_get_grp(dmCtx, code, path, &ret)))
		return rc;
	rc = dm_decode_string(ret, data);
	dm_grp_free(ret);
	return rc;
}

uint32_t
dm_generic_send_request_char_address_get_char(DMCONTEXT *dmCtx, uint32_t code,
					      const char *str,
					      struct in_addr addr, char **data)
{
	DIAM_AVPGRP	*grp;
	DIAM_AVPGRP	*answer;
	uint32_t	rc;

	if (!(grp = dm_grp_new()) ||
	    diam_avpgrp_add_string(NULL, &grp, AVP_STRING, 0, VP_TRAVELPING, str) ||
	    diam_avpgrp_add_address(NULL, &grp, AVP_ADDRESS, 0, VP_TRAVELPING,
	    			    AF_INET, &addr)) {
		dm_grp_free(grp);
		return RC_ERR_ALLOC;
	}
	if ((rc = dm_generic_send_request(dmCtx, code, grp, &answer))) {
		dm_grp_free(grp);
		return rc;
	}
	rc = dm_decode_string(answer, data);
	dm_grp_free(grp);
	return rc;
}

static void
generic_answerHandler(DMCONFIG_EVENT event __attribute__((unused)),
		      DMCONTEXT *dmCtx __attribute__((unused)), void *user_data,
		      uint32_t answer_rc, DIAM_AVPGRP *answer_grp)
{
	struct _result {
		uint32_t	rc;
		DIAM_AVPGRP	*grp;
		DIAM_AVPGRP	**ret;
	} *result = user_data;

	if ((result->rc = answer_rc))
		return;

	if (result->ret) {
		if (answer_grp)
			talloc_steal(result->grp, answer_grp);
		*result->ret = answer_grp;
	}
}

		/* auxiliary event handler */

static void
connectEvent(int fd, short event, void *arg)
{
	CONNEVENTCTX	*ctx = arg;
	int		rc;
	socklen_t	size = sizeof(rc);

	if (fd != -1 &&
	    (event != EV_WRITE ||
	     getsockopt(fd, SOL_SOCKET, SO_ERROR, &rc, &size) ||
	     size != sizeof(rc) || rc)) {
	    	CALLBACK(ctx, DMCONFIG_ERROR_CONNECTING, ctx->dmCtx,
			 ctx->user_data);
	} else
		CALLBACK(ctx, DMCONFIG_CONNECTED, ctx->dmCtx, ctx->user_data);

	talloc_free(ctx);
}

static void
writeEvent(int fd, short event, void *arg)
{
	DMCONTEXT	*dmCtx = arg;
	COMMCONTEXT	*ctx = &dmCtx->writeCtx;

	COMMSTATUS	status;
	uint32_t	rc;

	struct timeval	timeout;

	do {
		if (!ctx->buffer) {
			for (ctx->cur_request = dmCtx->requestlist_head->next;
			     ctx->cur_request &&
			     ctx->cur_request->status != REQUEST_SHALL_WRITE;
			     ctx->cur_request = ctx->cur_request->next);
			if (!ctx->cur_request) {	/* all requests written */
				event_del(&ctx->event);
				return;
			}
			ctx->cur_request->status = REQUEST_WRITING;
			ctx->req = ctx->cur_request->request;
		}

		rc = event_aux_diamWrite(fd, event, ctx, &status);
		switch (status) {
		case COMPLETE:
			talloc_free(ctx->cur_request->request);
			ctx->buffer = NULL;

			ctx->cur_request->status = REQUEST_SHALL_READ;

			if (!event_pending(&dmCtx->readCtx.event,
					   EV_READ | EV_PERSIST, NULL)) {
				event_set(&dmCtx->readCtx.event, dmCtx->socket,
					  EV_READ | EV_PERSIST, readEvent, dmCtx);
				event_base_set(dm_context_get_event_base(dmCtx),
					       &dmCtx->readCtx.event);

				timeout.tv_sec = TIMEOUT_READ_REQUESTS;
				timeout.tv_usec = 0;

				if (event_add(&dmCtx->readCtx.event, &timeout)) {
					rc = RC_ERR_ALLOC;
					break;
				}
			}

			timeout.tv_sec = TIMEOUT_WRITE_REQUESTS;
			timeout.tv_usec = 0;

			if (event_add(&ctx->event, &timeout))	/* increase writeEvent's timeout */
				rc = RC_ERR_ALLOC;

			break;
		case INCOMPLETE:
			timeout.tv_sec = TIMEOUT_CHUNKS;
			timeout.tv_usec = 0;

			if (event_add(&ctx->event, &timeout)) {	/* reduce writeEvent's timeout */
				rc = RC_ERR_ALLOC;
				break;
			}
		case NOTHING:
			return;
		default:	/* CONNRESET or ERROR */
			break;
		}
	} while (!rc);

	CALLBACK(ctx->cur_request, DMCONFIG_ERROR_WRITING,
		 ctx->cur_request->dmCtx, ctx->cur_request->user_data, rc, NULL);

	L_FOREACH(REQUESTINFO, cur, dmCtx->requestlist_head)
		if (cur->status != REQUEST_SHALL_READ)
			talloc_free(cur->request);
	talloc_free(dmCtx->requestlist_head);
	dmCtx->requestlist_head = NULL;

	ctx->cur_request = NULL;
	ctx->req = NULL;
	ctx->buffer = NULL;
	event_del(&ctx->event);

	talloc_free(dmCtx->readCtx.req);
	dmCtx->readCtx.req = NULL;

	if (event_pending(&dmCtx->readCtx.event, EV_READ | EV_PERSIST, NULL))
		event_del(&dmCtx->readCtx.event);
}

static void
readEvent(int fd, short event, void *arg)
{
	DMCONTEXT	*dmCtx = arg;
	COMMCONTEXT	*ctx = &dmCtx->readCtx;

	uint32_t	avpcode;
	uint8_t		flags;
	uint32_t	vendor_id;
	void		*data;
	size_t		len;

	uint8_t		alreadyRead = 0;

	struct timeval	timeout;

	for (;;) {
		uint32_t	hopid;
		REQUESTINFO	*cur, *reqEl;

		COMMSTATUS	status;
		uint32_t	rc;

		rc = event_aux_diamRead(fd, event, ctx, &alreadyRead, &status);
		switch (status) {
		case INCOMPLETE:
			timeout.tv_sec = TIMEOUT_CHUNKS;
			timeout.tv_usec = 0;

			if (event_add(&ctx->event, &timeout))	/* reduce readEvent's timeout */
				goto abort;
		case NOTHING:
			return;
		case COMPLETE:
			break;
		default:	/* CONNRESET or ERROR */
			goto abort;
		}

#ifdef LIBDMCONFIG_DEBUG
		if (dmconfig_debug_level) {
			fprintf(stderr, "Recieved %s:\n",
				diam_packet_flags(&ctx->req->packet) &
					CMD_FLAG_REQUEST ? "request" : "answer");
			dump_diam_packet(ctx->req);
			diam_request_reset_avp(ctx->req);
		}
#endif

					/* server request */
		if (diam_packet_flags(&ctx->req->packet) & CMD_FLAG_REQUEST) {
			switch (diam_packet_code(&ctx->req->packet)) {
			case CMD_CLIENT_GATEWAY_NOTIFY:
			case CMD_CLIENT_ACTIVE_NOTIFY:
				if (process_active_notification(dmCtx))
					goto abort;
				break;

			case CMD_CLIENT_FWUPDATE_FINISH:
				if (process_fwupdate_finish(dmCtx))
					goto abort;
				break;
			case CMD_CLIENT_FWUPDATE_PROGRESS:
				if (process_fwupdate_progress(dmCtx))
					goto abort;
				break;

			case CMD_CLIENT_PING:
				if (process_ping(dmCtx))
					goto abort;
				break;
			case CMD_CLIENT_PING_COMPLETED:
				if (process_ping_completed(dmCtx))
					goto abort;
				break;

			case CMD_CLIENT_TRACEROUTE:
				if (process_traceroute(dmCtx))
					goto abort;
				break;
			case CMD_CLIENT_TRACEROUTE_COMPLETED:
				if (process_traceroute_completed(dmCtx))
					goto abort;
				break;

			case CMD_CLIENT_PCAP_ABORTED:
				if (process_pcap_aborted(dmCtx))
					goto abort;
				break;

			default:
				goto abort;
			}
		} else {
			if (!dmCtx->requestlist_head || !dmCtx->requestlist_head->next)
				goto abort;

			hopid = diam_hop2hop_id(&ctx->req->packet);

			for (cur = dmCtx->requestlist_head;
			     cur->next && cur->next->hopid != hopid;
			     cur = cur->next);
			if (!cur->next || cur->next->status != REQUEST_SHALL_READ)
				goto abort;
			reqEl = cur->next;
			cur->next = reqEl->next;

			if (diam_request_get_avp(ctx->req, &avpcode, &flags,
						 &vendor_id, &data, &len) ||
			    avpcode != AVP_RC || len != sizeof(uint32_t)) {
				CALLBACK(reqEl, DMCONFIG_ERROR_READING, reqEl->dmCtx, reqEl->user_data, RC_ERR_MISC, NULL);
				goto cleanup;
			}

			if ((rc = diam_get_uint32_avp(data))) {
				CALLBACK(reqEl, DMCONFIG_ANSWER_READY, reqEl->dmCtx, reqEl->user_data, rc, NULL);

						/* clean up callback structures if necessary, so the read event is deleted */
				switch (reqEl->code) {
				case CMD_SUBSCRIBE_NOTIFY:
				case CMD_SUBSCRIBE_GW_NOTIFY:
					memset(&dmCtx->callbacks.active_notification, 0,
					       sizeof(ACTIVE_NOTIFY_INFO));
					break;

				case CMD_DEV_FWUPDATE:
					memset(&dmCtx->callbacks.fwupdate_feedback, 0,
					       sizeof(struct _callback_fwupdate_feedback));
					break;

				case CMD_DEV_PING:
					memset(&dmCtx->callbacks.ping_feedback, 0,
					       sizeof(struct _callback_ping_feedback));
					break;

				case CMD_DEV_TRACEROUTE:
					memset(&dmCtx->callbacks.traceroute_feedback, 0,
					       sizeof(struct _callback_traceroute_feedback));
					break;

				case CMD_DEV_PCAP:
					memset(&dmCtx->callbacks.pcap_feedback, 0,
					       sizeof(struct _callback_pcap_feedback));
					break;
				}

				goto cleanup;
			} else /* RC_OK */ {
				switch (reqEl->code) {
				case CMD_UNSUBSCRIBE_NOTIFY:
				case CMD_UNSUBSCRIBE_GW_NOTIFY:
					memset(&dmCtx->callbacks.active_notification, 0,
					       sizeof(ACTIVE_NOTIFY_INFO));
					break;

				case CMD_ENDSESSION:	/*
							 * allows the implicit abortion (deletion of read event -> event loop returns) of
							 * asynchronous processes (ping, active notify, etc)
							 */
					memset(&dmCtx->callbacks, 0,
					       sizeof(struct _dmContext_callbacks));
					break;
				}
			}

			if (diam_request_get_avp(ctx->req, &avpcode, &flags, &vendor_id, &data, &len)) {
				CALLBACK(reqEl, DMCONFIG_ANSWER_READY, reqEl->dmCtx, reqEl->user_data, RC_OK, NULL);
			} else if (avpcode == AVP_CONTAINER) {
				DIAM_AVPGRP *answer;

				if ((answer = diam_decode_avpgrp(ctx->req, data, len))) {
					CALLBACK(reqEl, DMCONFIG_ANSWER_READY, reqEl->dmCtx, reqEl->user_data, RC_OK, answer);
				} else {
					CALLBACK(reqEl, DMCONFIG_ERROR_READING, reqEl->dmCtx, reqEl->user_data, RC_ERR_ALLOC, NULL);
				}
			} else {
				CALLBACK(reqEl, DMCONFIG_ERROR_READING, reqEl->dmCtx, reqEl->user_data, RC_ERR_MISC, NULL);
			}

cleanup:

			if (dmCtx->requestlist_head->next)
				talloc_free(reqEl);
			else {
				talloc_free(dmCtx->requestlist_head);
				dmCtx->requestlist_head = NULL;
			}
		}

		for (cur = dmCtx->requestlist_head;
		     cur && cur->status != REQUEST_SHALL_READ;
		     cur = cur->next);
		if (!cur &&						/* nothing more to read (at least not expected) */
		    !dmCtx->callbacks.active_notification.callback &&	/* FIXME: a reference counter would be cleaner */
		    !dmCtx->callbacks.fwupdate_feedback.finish_cb &&	/* if it's 0, 'callback' requests aren't expected and the event can be deleted */
		    !dmCtx->callbacks.ping_feedback.ping_cb &&
		    !dmCtx->callbacks.traceroute_feedback.traceroute_cb &&
		    !dmCtx->callbacks.pcap_feedback.aborted_cb) {
			event_del(&ctx->event);
				/* if there's nothing more to read, the fields are reset and NOTHING is returned */
			event_aux_diamRead(fd, EV_READ, ctx,
					   &alreadyRead, &status);
			if (status != NOTHING)
				goto abort;

			return;
		}

		timeout.tv_sec = TIMEOUT_READ_REQUESTS;
		timeout.tv_usec = 0;

		if (event_add(&ctx->event, &timeout))	/* increase readEvent's timeout */
			break;
	}

abort:

	event_del(&ctx->event);
	talloc_free(ctx->req);
	ctx->req = NULL;
	ctx->cAlloc = ctx->bytes = 0;
	memset(&dmCtx->callbacks, 0, sizeof(struct _dmContext_callbacks));
}

static inline int
process_active_notification(DMCONTEXT *dmCtx)
{
	COMMCONTEXT	*ctx = &dmCtx->readCtx;

	uint32_t	avpcode;
	uint8_t		flags;
	uint32_t	vendor_id;
	void		*data;
	size_t		len;

	DIAM_AVPGRP	*notify;

	if (!dmCtx->callbacks.active_notification.callback)
		return -1;

	if (diam_request_get_avp(ctx->req, &avpcode, &flags, &vendor_id, &data, &len) ||
	    avpcode != AVP_CONTAINER || !len ||
	    !(notify = diam_decode_avpgrp(ctx->req, data, len))) {
		NOTIFY_CALLBACK(DMCONFIG_ERROR_READING, NULL);
	} else {
		NOTIFY_CALLBACK(DMCONFIG_ANSWER_READY, notify);
	}

	return 0;
}

static inline int
process_fwupdate_finish(DMCONTEXT *dmCtx)
{
	COMMCONTEXT	*ctx = &dmCtx->readCtx;

	uint32_t	avpcode;
	uint8_t		flags;
	uint32_t	vendor_id;
	void		*data;
	size_t		len;

	int32_t		cb_code;
	char		*cb_msg = NULL;

	if (!dmCtx->callbacks.fwupdate_feedback.finish_cb)
		return -1;

	if (diam_request_get_avp(ctx->req, &avpcode, &flags, &vendor_id, &data, &len) ||
	    avpcode != AVP_INT32 || len != sizeof(int32_t)) {
		FWUPDATE_FINISH_CALLBACK(DMCONFIG_ERROR_READING, 0, NULL);
		return 0;
	}
	cb_code = diam_get_int32_avp(data);

	if (diam_request_get_avp(ctx->req, &avpcode, &flags, &vendor_id, &data, &len) ||
	    avpcode != AVP_STRING || !(cb_msg = strndup(data, len))) {
		FWUPDATE_FINISH_CALLBACK(DMCONFIG_ERROR_READING, cb_code, NULL);
	} else {
		FWUPDATE_FINISH_CALLBACK(DMCONFIG_ANSWER_READY, cb_code, cb_msg);
	}

	free(cb_msg);
	if (cb_code == -1) /* special code, used at the end of the process (regardless of success/failure)  */
		memset(&dmCtx->callbacks.fwupdate_feedback, 0,
		       sizeof(struct _callback_fwupdate_feedback));

	return 0;
}

static inline int
process_fwupdate_progress(DMCONTEXT *dmCtx)
{
	COMMCONTEXT	*ctx = &dmCtx->readCtx;

	uint32_t	avpcode;
	uint8_t		flags;
	uint32_t	vendor_id;
	void		*data;
	size_t		len;

	char		*cb_msg, *cb_unit = NULL;
	uint32_t	cb_state;
	int32_t		cb_current, cb_total;

	if (!dmCtx->callbacks.fwupdate_feedback.progress_cb)
		return -1;

	if (diam_request_get_avp(ctx->req, &avpcode, &flags, &vendor_id, &data, &len) ||
	    avpcode != AVP_STRING || !(cb_msg = strndup(data, len))) {
		FWUPDATE_PROGRESS_CALLBACK(DMCONFIG_ERROR_READING, NULL, 0, 0, 0, NULL);
		return 0;
	}

	if (diam_request_get_avp(ctx->req, &avpcode, &flags, &vendor_id, &data, &len) ||
	    avpcode != AVP_FWUPDATE_STEP || len != sizeof(uint32_t)) {
		FWUPDATE_PROGRESS_CALLBACK(DMCONFIG_ERROR_READING, cb_msg, 0, 0, 0, NULL);
		free(cb_msg);
		return 0;
	}
	cb_state = diam_get_uint32_avp(data);

	if (diam_request_get_avp(ctx->req, &avpcode, &flags, &vendor_id, &data, &len) ||
	    avpcode != AVP_INT32 || len != sizeof(int32_t)) {
		FWUPDATE_PROGRESS_CALLBACK(DMCONFIG_ERROR_READING, cb_msg, cb_state, 0, 0, NULL);
		free(cb_msg);
		return 0;
	}
	cb_current = diam_get_int32_avp(data);

	if (diam_request_get_avp(ctx->req, &avpcode, &flags, &vendor_id, &data, &len) ||
	    avpcode != AVP_INT32 || len != sizeof(int32_t)) {
		FWUPDATE_PROGRESS_CALLBACK(DMCONFIG_ERROR_READING, cb_msg, cb_state, cb_current, 0, NULL);
		free(cb_msg);
		return 0;
	}
	cb_total = diam_get_int32_avp(data);

	if (diam_request_get_avp(ctx->req, &avpcode, &flags, &vendor_id, &data, &len) ||
	    avpcode != AVP_STRING || !(cb_unit = strndup(data, len))) {
		FWUPDATE_PROGRESS_CALLBACK(DMCONFIG_ERROR_READING, cb_msg, cb_state, cb_current, cb_total, NULL);
	} else {
		FWUPDATE_PROGRESS_CALLBACK(DMCONFIG_ANSWER_READY, cb_msg, cb_state, cb_current, cb_total, cb_unit);
	}

	free(cb_msg);
	free(cb_unit);

	return 0;
}

static inline int
process_ping(DMCONTEXT *dmCtx)
{
	COMMCONTEXT	*ctx = &dmCtx->readCtx;

	uint32_t	avpcode;
	uint8_t		flags;
	uint32_t	vendor_id;
	void		*data;
	size_t		len;

	uint32_t	bytes;
	int		af;
	union {
		struct in_addr	in;
		struct in6_addr	in6;
	} result_addr;
	uint16_t	seq;

	if (!dmCtx->callbacks.ping_feedback.ping_cb)
		return -1;

	if (diam_request_get_avp(ctx->req, &avpcode, &flags, &vendor_id, &data, &len) ||
	    avpcode != AVP_UINT32 || len != sizeof(uint32_t)) {
		PING_CALLBACK(DMCONFIG_ERROR_READING, 0, NULL, 0, 0);
		return 0;
	}
	bytes = diam_get_uint32_avp(data);

	if (diam_request_get_avp(ctx->req, &avpcode, &flags, &vendor_id, &data, &len) ||
	    avpcode != AVP_ADDRESS || len != sizeof(uint16_t) + sizeof(struct in_addr) ||
	    !diam_get_address_avp(&af, &result_addr, data) || af != AF_INET) {
		PING_CALLBACK(DMCONFIG_ERROR_READING, bytes, NULL, 0, 0);
	   	return 0;
	}

	if (diam_request_get_avp(ctx->req, &avpcode, &flags, &vendor_id, &data, &len) ||
	    avpcode != AVP_UINT16 || len != sizeof(uint16_t)) {
		PING_CALLBACK(DMCONFIG_ERROR_READING, bytes, &result_addr.in, 0, 0);
		return 0;
	}
	seq = diam_get_uint16_avp(data);

	if (diam_request_get_avp(ctx->req, &avpcode, &flags, &vendor_id, &data, &len) ||
	    avpcode != AVP_UINT32 || len != sizeof(uint32_t)) {
		PING_CALLBACK(DMCONFIG_ERROR_READING, bytes, &result_addr.in, seq, 0);
	} else {
		PING_CALLBACK(DMCONFIG_ANSWER_READY, bytes, &result_addr.in, seq, diam_get_uint32_avp(data));
	}

	return 0;
}

static inline int
process_ping_completed(DMCONTEXT *dmCtx)
{
	COMMCONTEXT	*ctx = &dmCtx->readCtx;

	uint32_t	avpcode;
	uint8_t		flags;
	uint32_t	vendor_id;
	void		*data;
	size_t		len;

	uint32_t	succ_cnt, fail_cnt, tavg, tmin;

	if (!dmCtx->callbacks.ping_feedback.completed_cb)
		return -1;

	if (diam_request_get_avp(ctx->req, &avpcode, &flags, &vendor_id, &data, &len) ||
	    avpcode != AVP_UINT32 || len != sizeof(uint32_t)) {
		PING_COMPLETED_CALLBACK(DMCONFIG_ERROR_READING, 0, 0, 0, 0, 0);
		goto clean_up;
	}
	succ_cnt = diam_get_uint32_avp(data);

	if (diam_request_get_avp(ctx->req, &avpcode, &flags, &vendor_id, &data, &len) ||
	    avpcode != AVP_UINT32 || len != sizeof(uint32_t)) {
		PING_COMPLETED_CALLBACK(DMCONFIG_ERROR_READING, succ_cnt, 0, 0, 0, 0);
		goto clean_up;
	}
	fail_cnt = diam_get_uint32_avp(data);

	if (diam_request_get_avp(ctx->req, &avpcode, &flags, &vendor_id, &data, &len) ||
	    avpcode != AVP_UINT32 || len != sizeof(uint32_t)) {
		PING_COMPLETED_CALLBACK(DMCONFIG_ERROR_READING, succ_cnt, fail_cnt, 0, 0, 0);
		goto clean_up;
	}
	tavg = diam_get_uint32_avp(data);

	if (diam_request_get_avp(ctx->req, &avpcode, &flags, &vendor_id, &data, &len) ||
	    avpcode != AVP_UINT32 || len != sizeof(uint32_t)) {
		PING_COMPLETED_CALLBACK(DMCONFIG_ERROR_READING, succ_cnt, fail_cnt, tavg, 0, 0);
		goto clean_up;
	}
	tmin = diam_get_uint32_avp(data);

	if (diam_request_get_avp(ctx->req, &avpcode, &flags, &vendor_id, &data, &len) ||
	    avpcode != AVP_UINT32 || len != sizeof(uint32_t)) {
		PING_COMPLETED_CALLBACK(DMCONFIG_ERROR_READING, succ_cnt, fail_cnt, tavg, tmin, 0);
	} else {
		PING_COMPLETED_CALLBACK(DMCONFIG_ANSWER_READY, succ_cnt, fail_cnt, tavg, tmin, diam_get_uint32_avp(data));
	}

clean_up:

	memset(&dmCtx->callbacks.ping_feedback, 0, sizeof(struct _callback_ping_feedback));
	return 0;
}

static inline int
process_traceroute(DMCONTEXT *dmCtx)
{
	COMMCONTEXT	*ctx = &dmCtx->readCtx;

	uint32_t	avpcode;
	uint8_t		flags;
	uint32_t	vendor_id;
	void		*data;
	size_t		len;

	int32_t		code;
	uint8_t		hop;
	char		*hostname = NULL;
	int		af;
	union {
		struct in_addr	in;
		struct in6_addr	in6;
	} result_addr;

	if (!dmCtx->callbacks.traceroute_feedback.traceroute_cb)
		return -1;

	if (diam_request_get_avp(ctx->req, &avpcode, &flags, &vendor_id, &data, &len) ||
	    avpcode != AVP_INT32 || len != sizeof(int32_t)) {
		TRACEROUTE_CALLBACK(DMCONFIG_ERROR_READING, 0, 0, NULL, NULL, 0);
		return 0;
	}
	code = diam_get_int32_avp(data);

	if (diam_request_get_avp(ctx->req, &avpcode, &flags, &vendor_id, &data, &len) ||
	    avpcode != AVP_UINT8 || len != sizeof(uint8_t)) {
		TRACEROUTE_CALLBACK(DMCONFIG_ERROR_READING, code, 0, NULL, NULL, 0);
		return 0;
	}
	hop = diam_get_uint8_avp(data);

	if (diam_request_get_avp(ctx->req, &avpcode, &flags, &vendor_id, &data, &len) ||
	    avpcode != AVP_STRING ||
	    (len && !(hostname = strndup(data, len)))) {
		TRACEROUTE_CALLBACK(DMCONFIG_ERROR_READING, code, hop, NULL, NULL, 0);
		return 0;
	}

	if (diam_request_get_avp(ctx->req, &avpcode, &flags, &vendor_id, &data, &len) ||
	    avpcode != AVP_ADDRESS || len != sizeof(uint16_t) + sizeof(struct in_addr) ||
	    !diam_get_address_avp(&af, &result_addr, data) || af != AF_INET) {
		TRACEROUTE_CALLBACK(DMCONFIG_ERROR_READING, code, hop, hostname, NULL, 0);
		free(hostname);
	   	return 0;
	}

	if (diam_request_get_avp(ctx->req, &avpcode, &flags, &vendor_id, &data, &len) ||
	    avpcode != AVP_INT32 || len != sizeof(int32_t)) {
		TRACEROUTE_CALLBACK(DMCONFIG_ERROR_READING, code, hop,
				    hostname, &result_addr.in, 0);
	} else {
		TRACEROUTE_CALLBACK(DMCONFIG_ANSWER_READY, code, hop,
				    hostname, &result_addr.in, diam_get_int32_avp(data));
	}

	free(hostname);

	return 0;
}

static inline int
process_traceroute_completed(DMCONTEXT *dmCtx)
{
	COMMCONTEXT	*ctx = &dmCtx->readCtx;

	uint32_t	avpcode;
	uint8_t		flags;
	uint32_t	vendor_id;
	void		*data;
	size_t		len;

	if (!dmCtx->callbacks.traceroute_feedback.completed_cb)
		return -1;

	if (diam_request_get_avp(ctx->req, &avpcode, &flags, &vendor_id, &data, &len) ||
	    avpcode != AVP_INT32 || len != sizeof(int32_t)) {
		TRACEROUTE_COMPLETED_CALLBACK(DMCONFIG_ERROR_READING, 0);
	} else {
		TRACEROUTE_COMPLETED_CALLBACK(DMCONFIG_ANSWER_READY, diam_get_int32_avp(data));
	}

	memset(&dmCtx->callbacks.traceroute_feedback, 0, sizeof(struct _callback_traceroute_feedback));
	return 0;
}

static inline int
process_pcap_aborted(DMCONTEXT *dmCtx)
{
	COMMCONTEXT	*ctx = &dmCtx->readCtx;

	uint32_t	avpcode;
	uint8_t		flags;
	uint32_t	vendor_id;
	void		*data;
	size_t		len;

	if (!dmCtx->callbacks.pcap_feedback.aborted_cb)
		return -1;

	if (diam_request_get_avp(ctx->req, &avpcode, &flags, &vendor_id, &data, &len) ||
	    avpcode != AVP_UINT32 || len != sizeof(uint32_t)) {
		PCAP_ABORTED_CALLBACK(DMCONFIG_ERROR_READING, 0);
	} else {
		PCAP_ABORTED_CALLBACK(DMCONFIG_ANSWER_READY, diam_get_uint32_avp(data));
	}

	memset(&dmCtx->callbacks.pcap_feedback, 0, sizeof(struct _callback_pcap_feedback));
	return 0;
}

		/* enduser API (both blocking and nonblocking) */

		/* build AVP group for SET packet */

uint32_t
dm_grp_set(DIAM_AVPGRP **grp, const char *name, int type,
	   void *value, size_t size)
{
	DIAM_AVPGRP *pair;

	if (!(pair = new_diam_avpgrp(*grp)) ||
	    diam_avpgrp_add_string(*grp, &pair, AVP_PATH, 0, VP_TRAVELPING, name) ||
	    diam_avpgrp_add_raw(*grp, &pair, type, 0, VP_TRAVELPING, value, size) ||
	    diam_avpgrp_add_avpgrp(NULL, grp, AVP_CONTAINER, 0,
	    			   VP_TRAVELPING, pair)) {
		dm_grp_free(pair);
		return RC_ERR_ALLOC;
	}

	dm_grp_free(pair);
	return RC_OK;
}

		/* send and evaluate packet */

uint32_t
dm_send_gw_get_client(DMCONTEXT *dmCtx, uint16_t zone, uint8_t isNATIP,
		      struct in_addr addr, uint16_t port, DIAM_AVPGRP **ret)
{
	DIAM_AVPGRP	*grp;
	uint32_t	rc;

	if (!(grp = dm_grp_new()) ||
	    diam_avpgrp_add_uint16(NULL, &grp, AVP_UINT16, 0, VP_TRAVELPING, zone) ||
	    diam_avpgrp_add_address(NULL, &grp, isNATIP ? AVP_GW_NATIPADDRESS : AVP_GW_IPADDRESS,
				    0, VP_TRAVELPING, AF_INET, &addr) ||
	    diam_avpgrp_add_uint16(NULL, &grp, AVP_UINT16, 0, VP_TRAVELPING, port)) {
		dm_grp_free(grp);
		return RC_ERR_ALLOC;
	}

	if (!(rc = dm_generic_send_request(dmCtx, CMD_GW_GET_CLIENT, grp, ret)))
		talloc_steal(NULL, *ret);
	dm_grp_free(grp);
	return rc;
}

uint32_t
dm_send_gw_get_all_clients(DMCONTEXT *dmCtx, uint16_t zone, DIAM_AVPGRP **ret)
{
	DIAM_AVPGRP	*grp;
	uint32_t	rc;

	if (!(grp = dm_grp_new()) ||
	    diam_avpgrp_add_uint16(NULL, &grp, AVP_UINT16, 0, VP_TRAVELPING, zone)) {
		dm_grp_free(grp);
		return RC_ERR_ALLOC;
	}

	if (!(rc = dm_generic_send_request(dmCtx, CMD_GW_GET_ALL_CLIENTS, grp, ret)))
		talloc_steal(NULL, *ret);
	dm_grp_free(grp);
	return rc;
}

uint32_t
dm_send_gw_req_client_accessclass(DMCONTEXT *dmCtx, const char *path,
				  const char *username, const char *password,
				  const char *class, const char *useragent,
				  struct timeval *timeout, int32_t *authReqState,
				  int32_t *authResult, uint32_t *replyCode,
				  DIAM_AVPGRP **messages)
{
	DIAM_AVPGRP	*grp, *answer;
	uint32_t	rc;

	if (!(grp = dm_grp_new()) ||
	    diam_avpgrp_add_string(NULL, &grp, AVP_GW_OBJ_ID, 0,
				   VP_TRAVELPING, path) ||
	    diam_avpgrp_add_string(NULL, &grp, AVP_GW_USERNAME, 0,
	    			   VP_TRAVELPING, username) ||
	    diam_avpgrp_add_string(NULL, &grp, AVP_GW_PASSWORD, 0,
	    			   VP_TRAVELPING, password) ||
	    diam_avpgrp_add_string(NULL, &grp, AVP_GW_ACCESSCLASS, 0,
	    			   VP_TRAVELPING, class) ||
	    (useragent && diam_avpgrp_add_string(NULL, &grp, AVP_GW_USERAGENT, 0,
	    			   		 VP_TRAVELPING, useragent)) ||
	    (timeout && diam_avpgrp_add_timeval(NULL, &grp, AVP_GW_TIMEOUT, 0,
	    			   		VP_TRAVELPING, *timeout))) {
		dm_grp_free(grp);
		return RC_ERR_ALLOC;
	}
	if ((rc = dm_generic_send_request(dmCtx, CMD_GW_CLIENT_REQ_ACCESSCLASS,
					  grp, &answer))) {
		dm_grp_free(grp);
		return rc;
	}

	rc = dm_decode_gw_req_client_accessclass(answer, authReqState, authResult,
						 replyCode, messages);
	dm_grp_free(grp);
	return rc;
}

uint32_t
dm_send_gw_set_client_accessclass(DMCONTEXT *dmCtx, const char *path,
				  const char *username, const char *class)
{
	DIAM_AVPGRP	*grp;
	uint32_t	rc;

	if (!(grp = dm_grp_new()) ||
	    diam_avpgrp_add_string(NULL, &grp, AVP_GW_OBJ_ID, 0,
				   VP_TRAVELPING, path) ||
	    (username && diam_avpgrp_add_string(NULL, &grp, AVP_GW_USERNAME, 0,
	    				        VP_TRAVELPING, username)) ||
	    diam_avpgrp_add_string(NULL, &grp, AVP_GW_ACCESSCLASS, 0,
	    			   VP_TRAVELPING, class)) {
		dm_grp_free(grp);
		return RC_ERR_ALLOC;
	}

	rc = dm_generic_send_request(dmCtx, CMD_GW_CLIENT_SET_ACCESSCLASS,
				     grp, NULL);
	dm_grp_free(grp);
	return rc;
}

uint32_t
dm_send_cmd_hotplug(DMCONTEXT *dmCtx, const char *cmd)
{
	DIAM_AVPGRP	*grp;
	uint32_t	rc;

	if (!(grp = dm_grp_new()) ||
	    diam_avpgrp_add_string(NULL, &grp, AVP_HOTPLUGCMD, 0,
				   VP_TRAVELPING, cmd)) {
		dm_grp_free(grp);
		return RC_ERR_ALLOC;
	}
	rc = dm_generic_send_request(dmCtx, CMD_DEV_HOTPLUG, grp, NULL);
	dm_grp_free(grp);
	return rc;
}

uint32_t
dm_send_add_instance(DMCONTEXT *dmCtx, const char *path, uint16_t *instance)
{
	uint32_t	rc;
	DIAM_AVPGRP	*grp;
	DIAM_AVPGRP	*answer;

	if (!(grp = dm_grp_new()) ||
	    diam_avpgrp_add_string(NULL, &grp, AVP_PATH, 0, VP_TRAVELPING, path) ||
	    diam_avpgrp_add_uint16(NULL, &grp, AVP_UINT16, 0, VP_TRAVELPING, *instance)) {
		dm_grp_free(grp);
		return RC_ERR_ALLOC;
	}
	if ((rc = dm_generic_send_request(dmCtx, CMD_DB_ADDINSTANCE, grp, &answer))) {
		dm_grp_free(grp);
		return rc;
	}

	rc = dm_decode_add_instance(answer, instance);
	dm_grp_free(grp);
	return rc;
}

uint32_t
dm_send_list(DMCONTEXT *dmCtx, const char *name, uint16_t level,
	     DIAM_AVPGRP **answer)
{
	uint32_t	rc;
	DIAM_AVPGRP	*grp;

	if (!(grp = dm_grp_new()) ||
	    diam_avpgrp_add_uint16(NULL, &grp, AVP_UINT16, 0, VP_TRAVELPING, level) ||
	    diam_avpgrp_add_string(NULL, &grp, AVP_PATH, 0, VP_TRAVELPING, name)) {
		dm_grp_free(grp);
		return RC_ERR_ALLOC;
	}

	if (!(rc = dm_generic_send_request(dmCtx, CMD_DB_LIST, grp, answer)))
		talloc_steal(NULL, *answer);
	dm_grp_free(grp);
	return rc;
}

		/* register requests (nonblocking API) */

uint32_t
dm_register_gw_get_client(DMCONTEXT *dmCtx, uint16_t zone, uint8_t isNATIP,
			  struct in_addr addr, uint16_t port,
			  DMCONFIG_CALLBACK callback, void *callback_ud)
{
	DIAM_AVPGRP	*grp;
	uint32_t	rc;

	if (!(grp = dm_grp_new()) ||
	    diam_avpgrp_add_uint16(NULL, &grp, AVP_UINT16, 0, VP_TRAVELPING, zone) ||
	    diam_avpgrp_add_address(NULL, &grp, isNATIP ? AVP_GW_NATIPADDRESS : AVP_GW_IPADDRESS,
				    0, VP_TRAVELPING, AF_INET, &addr) ||
	    diam_avpgrp_add_uint16(NULL, &grp, AVP_UINT16, 0, VP_TRAVELPING, port)) {
		dm_grp_free(grp);
		return RC_ERR_ALLOC;
	}

	rc = dm_generic_register_request(dmCtx, CMD_GW_GET_CLIENT, grp,
					 callback, callback_ud);
	dm_grp_free(grp);
	return rc;
}

uint32_t
dm_register_gw_get_all_clients(DMCONTEXT *dmCtx, uint16_t zone,
			       DMCONFIG_CALLBACK callback, void *callback_ud)
{
	DIAM_AVPGRP	*grp;
	uint32_t	rc;

	if (!(grp = dm_grp_new()) ||
	    diam_avpgrp_add_uint16(NULL, &grp, AVP_UINT16, 0, VP_TRAVELPING, zone)) {
		dm_grp_free(grp);
		return RC_ERR_ALLOC;
	}

	rc = dm_generic_register_request(dmCtx, CMD_GW_GET_ALL_CLIENTS, grp,
					 callback, callback_ud);
	dm_grp_free(grp);
	return rc;
}

uint32_t
dm_register_gw_req_client_accessclass(DMCONTEXT *dmCtx, const char *path,
				      const char *username, const char *password,
				      const char *class,
				      const char *useragent,
				      struct timeval *timeout,
				      DMCONFIG_CALLBACK callback,
				      void *callback_ud)
{
	DIAM_AVPGRP	*grp;
	uint32_t	rc;

	if (!(grp = dm_grp_new()) ||
	    diam_avpgrp_add_string(NULL, &grp, AVP_GW_OBJ_ID, 0,
				   VP_TRAVELPING, path) ||
	    diam_avpgrp_add_string(NULL, &grp, AVP_GW_USERNAME, 0,
	    			   VP_TRAVELPING, username) ||
	    diam_avpgrp_add_string(NULL, &grp, AVP_GW_PASSWORD, 0,
	    			   VP_TRAVELPING, password) ||
	    diam_avpgrp_add_string(NULL, &grp, AVP_GW_ACCESSCLASS, 0,
	    			   VP_TRAVELPING, class) ||
	    (useragent && diam_avpgrp_add_string(NULL, &grp, AVP_GW_USERAGENT, 0,
	    			   		 VP_TRAVELPING, useragent)) ||
	    (timeout && diam_avpgrp_add_timeval(NULL, &grp, AVP_GW_TIMEOUT, 0,
	    			   		VP_TRAVELPING, *timeout))) {
		dm_grp_free(grp);
		return RC_ERR_ALLOC;
	}

	rc = dm_generic_register_request(dmCtx, CMD_GW_CLIENT_REQ_ACCESSCLASS,
					 grp, callback, callback_ud);
	dm_grp_free(grp);
	return rc;
}

uint32_t
dm_register_gw_set_client_accessclass(DMCONTEXT *dmCtx, const char *path,
				      const char *username,
				      const char *class,
				      DMCONFIG_CALLBACK callback,
				      void *callback_ud)
{
	DIAM_AVPGRP	*grp;
	uint32_t	rc;

	if (!(grp = dm_grp_new()) ||
	    diam_avpgrp_add_string(NULL, &grp, AVP_GW_OBJ_ID, 0,
				   VP_TRAVELPING, path) ||
	    (username && diam_avpgrp_add_string(NULL, &grp, AVP_GW_USERNAME, 0,
				   	        VP_TRAVELPING, username)) ||
	    diam_avpgrp_add_string(NULL, &grp, AVP_GW_ACCESSCLASS, 0,
	    			   VP_TRAVELPING, class)) {
		dm_grp_free(grp);
		return RC_ERR_ALLOC;
	}

	rc = dm_generic_register_request(dmCtx, CMD_GW_CLIENT_SET_ACCESSCLASS,
					 grp, callback, callback_ud);
	dm_grp_free(grp);
	return rc;
}

uint32_t
dm_register_gw_sol_inform(DMCONTEXT *dmCtx, uint16_t zone, uint16_t accessclass,
			  struct in_addr addr, const char *mac,
			  DMCONFIG_CALLBACK callback, void *callback_ud)
{
	DIAM_AVPGRP	*grp;
	uint32_t	rc;

	if (!(grp = dm_grp_new()) ||
	    diam_avpgrp_add_uint16(NULL, &grp, AVP_UINT16, 0, VP_TRAVELPING, zone) ||
	    diam_avpgrp_add_uint16(NULL, &grp, AVP_UINT16, 0, VP_TRAVELPING, accessclass) ||
	    diam_avpgrp_add_address(NULL, &grp, AVP_ADDRESS, 0, VP_TRAVELPING, AF_INET, &addr) ||
	    diam_avpgrp_add_string(NULL, &grp, AVP_STRING, 0, VP_TRAVELPING, mac)) {
		dm_grp_free(grp);
		return RC_ERR_ALLOC;
	}

	rc = dm_generic_register_request(dmCtx, CMD_GW_SOL_INFORM, grp,
					 callback, callback_ud);
	dm_grp_free(grp);
	return rc;
}

uint32_t
dm_register_cmd_hotplug(DMCONTEXT *dmCtx, const char *cmd,
			DMCONFIG_CALLBACK callback, void *callback_ud)
{
	DIAM_AVPGRP	*grp;
	uint32_t	rc;

	if (!(grp = dm_grp_new()) ||
	    diam_avpgrp_add_string(NULL, &grp, AVP_HOTPLUGCMD, 0,
				   VP_TRAVELPING, cmd)) {
		dm_grp_free(grp);
		return RC_ERR_ALLOC;
	}
	rc = dm_generic_register_request(dmCtx, CMD_DEV_HOTPLUG, grp,
					 callback, callback_ud);
	dm_grp_free(grp);
	return rc;
}

uint32_t
dm_register_subscribe_notify(DMCONTEXT *dmCtx,
			     DMCONFIG_ACTIVE_NOTIFY notify_callback,
			     void *notify_callback_ud,
			     DMCONFIG_CALLBACK callback,
			     void *callback_ud)
{
	uint32_t rc;

	if ((rc = dm_generic_register_request(dmCtx, CMD_SUBSCRIBE_NOTIFY, NULL,
					      callback, callback_ud)))
		return rc;

	dmCtx->callbacks.active_notification.callback = notify_callback;
	dmCtx->callbacks.active_notification.user_data = notify_callback_ud;

	return RC_OK;
}

uint32_t
dm_register_subscribe_gw_notify(DMCONTEXT *dmCtx,
				DMCONFIG_ACTIVE_NOTIFY notify_callback,
				void *notify_callback_ud,
				DMCONFIG_CALLBACK callback, void *callback_ud)
{
	uint32_t rc;

	if ((rc = dm_generic_register_request(dmCtx, CMD_SUBSCRIBE_GW_NOTIFY,
					      NULL, callback, callback_ud)))
		return rc;

	dmCtx->callbacks.active_notification.callback = notify_callback;
	dmCtx->callbacks.active_notification.user_data = notify_callback_ud;

	return RC_OK;
}

uint32_t
dm_register_add_instance(DMCONTEXT *dmCtx, const char *path, uint16_t instance,
			 DMCONFIG_CALLBACK callback, void *callback_ud)
{
	uint32_t	rc;
	DIAM_AVPGRP	*grp;

	if (!(grp = dm_grp_new()) ||
	    diam_avpgrp_add_string(NULL, &grp, AVP_PATH, 0, VP_TRAVELPING, path) ||
	    diam_avpgrp_add_uint16(NULL, &grp, AVP_UINT16, 0, VP_TRAVELPING, instance)) {
		dm_grp_free(grp);
		return RC_ERR_ALLOC;
	}

	rc = dm_generic_register_request(dmCtx, CMD_DB_ADDINSTANCE, grp, callback, callback_ud);
	dm_grp_free(grp);
	return rc;
}

uint32_t
dm_register_list(DMCONTEXT *dmCtx, const char *name, uint16_t level,
		 DMCONFIG_CALLBACK callback, void *callback_ud)
{
	uint32_t	rc;
	DIAM_AVPGRP	*grp;

	if (!(grp = dm_grp_new()) ||
	    diam_avpgrp_add_uint16(NULL, &grp, AVP_UINT16, 0, VP_TRAVELPING, level) ||
	    diam_avpgrp_add_string(NULL, &grp, AVP_PATH, 0, VP_TRAVELPING, name)) {
		dm_grp_free(grp);
		return RC_ERR_ALLOC;
	}

	rc = dm_generic_register_request(dmCtx, CMD_DB_LIST, grp, callback, callback_ud);
	dm_grp_free(grp);
	return rc;
}

uint32_t
dm_register_cmd_fwupdate(DMCONTEXT *dmCtx, const char *fwfile,
			 const char *device, uint32_t flags,
			 DMCONFIG_FWUPDATE_FINISH finish_cb, void *finish_ud,
			 DMCONFIG_FWUPDATE_PROGRESS progress_cb, void *progress_ud,
			 DMCONFIG_CALLBACK callback, void *callback_ud)
{
	DIAM_AVPGRP	*grp;
	uint32_t	rc;

	if (!(grp = dm_grp_new()) ||
	    diam_avpgrp_add_string(NULL, &grp, AVP_STRING, 0, VP_TRAVELPING, fwfile) ||
	    diam_avpgrp_add_string(NULL, &grp, AVP_STRING, 0, VP_TRAVELPING, device) ||
	    diam_avpgrp_add_uint32(NULL, &grp, AVP_UINT32, 0, VP_TRAVELPING, flags)) {
		dm_grp_free(grp);
		return RC_ERR_ALLOC;
	}

	rc = dm_generic_register_request(dmCtx, CMD_DEV_FWUPDATE, grp,
					 callback, callback_ud);
	dm_grp_free(grp);
	if (!rc) {
		dmCtx->callbacks.fwupdate_feedback.finish_cb = finish_cb;
		dmCtx->callbacks.fwupdate_feedback.finish_ud = finish_ud;

		dmCtx->callbacks.fwupdate_feedback.progress_cb = progress_cb;
		dmCtx->callbacks.fwupdate_feedback.progress_ud = progress_ud;
	}

	return rc;
}

uint32_t
dm_register_cmd_ping(DMCONTEXT *dmCtx, const char *hostname,
		     uint32_t send_cnt, uint32_t timeout,
		     DMCONFIG_PING ping_cb, void *ping_ud,
		     DMCONFIG_PING_COMPLETED completed_cb, void *completed_ud,
		     DMCONFIG_CALLBACK callback, void *callback_ud)
{
	DIAM_AVPGRP	*grp;
	uint32_t	rc;

	if (!(grp = dm_grp_new()) ||
	    diam_avpgrp_add_string(NULL, &grp, AVP_STRING, 0, VP_TRAVELPING, hostname) ||
	    diam_avpgrp_add_uint32(NULL, &grp, AVP_UINT32, 0, VP_TRAVELPING, send_cnt) ||
	    diam_avpgrp_add_uint32(NULL, &grp, AVP_UINT32, 0, VP_TRAVELPING, timeout)) {
		dm_grp_free(grp);
		return RC_ERR_ALLOC;
	}

	rc = dm_generic_register_request(dmCtx, CMD_DEV_PING, grp,
					 callback, callback_ud);
	dm_grp_free(grp);
	if (!rc) {
		dmCtx->callbacks.ping_feedback.ping_cb = ping_cb;
		dmCtx->callbacks.ping_feedback.ping_ud = ping_ud;

		dmCtx->callbacks.ping_feedback.completed_cb = completed_cb;
		dmCtx->callbacks.ping_feedback.completed_ud = completed_ud;
	}

	return rc;
}

uint32_t
dm_register_cmd_traceroute(DMCONTEXT *dmCtx, const char *hostname,
		     	   uint8_t tries, uint32_t timeout, uint16_t size, uint8_t maxhop,
			   DMCONFIG_TRACEROUTE traceroute_cb, void *traceroute_ud,
			   DMCONFIG_TRACEROUTE_COMPLETED completed_cb, void *completed_ud,
			   DMCONFIG_CALLBACK callback, void *callback_ud)
{
	DIAM_AVPGRP	*grp;
	uint32_t	rc;

	if (!(grp = dm_grp_new()) ||
	    diam_avpgrp_add_string(NULL, &grp, AVP_STRING, 0, VP_TRAVELPING, hostname) ||
	    diam_avpgrp_add_uint8(NULL, &grp, AVP_UINT8, 0, VP_TRAVELPING, tries) ||
	    diam_avpgrp_add_uint32(NULL, &grp, AVP_UINT32, 0, VP_TRAVELPING, timeout) ||
	    diam_avpgrp_add_uint16(NULL, &grp, AVP_UINT16, 0, VP_TRAVELPING, size) ||
	    diam_avpgrp_add_uint8(NULL, &grp, AVP_UINT8, 0, VP_TRAVELPING, maxhop)) {
		dm_grp_free(grp);
		return RC_ERR_ALLOC;
	}

	rc = dm_generic_register_request(dmCtx, CMD_DEV_TRACEROUTE, grp,
					 callback, callback_ud);
	dm_grp_free(grp);
	if (!rc) {
		dmCtx->callbacks.traceroute_feedback.traceroute_cb = traceroute_cb;
		dmCtx->callbacks.traceroute_feedback.traceroute_ud = traceroute_ud;

		dmCtx->callbacks.traceroute_feedback.completed_cb = completed_cb;
		dmCtx->callbacks.traceroute_feedback.completed_ud = completed_ud;
	}

	return rc;
}

uint32_t
dm_register_cmd_pcap(DMCONTEXT *dmCtx, const char *interface, const char *url,
		     uint32_t timeout, uint16_t packets, uint16_t kbytes,
		     DMCONFIG_PCAP_ABORTED aborted_cb, void *aborted_ud,
		     DMCONFIG_CALLBACK callback, void *callback_ud)
{
	uint32_t	rc;
	DIAM_AVPGRP	*grp;

	if (!(grp = dm_grp_new()) ||
	    diam_avpgrp_add_string(NULL, &grp, AVP_PATH, 0, VP_TRAVELPING, interface) ||
	    diam_avpgrp_add_string(NULL, &grp, AVP_STRING, 0, VP_TRAVELPING, url) ||
	    diam_avpgrp_add_uint32(NULL, &grp, AVP_UINT32, 0, VP_TRAVELPING, timeout) ||
	    diam_avpgrp_add_uint16(NULL, &grp, AVP_UINT16, 0, VP_TRAVELPING, packets) ||
	    diam_avpgrp_add_uint16(NULL, &grp, AVP_UINT16, 0, VP_TRAVELPING, kbytes)) {
		dm_grp_free(grp);
		return RC_ERR_ALLOC;
	}

	rc = dm_generic_register_request(dmCtx, CMD_DEV_PCAP, grp, callback, callback_ud);
	dm_grp_free(grp);
	if (!rc) {
		dmCtx->callbacks.pcap_feedback.aborted_cb = aborted_cb;
		dmCtx->callbacks.pcap_feedback.aborted_ud = aborted_ud;
	}

	return rc;
}

		/* "decode" returned AVP groups */

uint32_t
dm_decode_gw_req_client_accessclass(DIAM_AVPGRP *grp, int32_t *authReqState,
				    int32_t *authResult, uint32_t *replyCode,
				    DIAM_AVPGRP **messages)
{
	uint32_t	code;
	uint8_t		flags;
	uint32_t	vendor_id;
	void		*data;
	size_t		len;

	uint32_t	rc;

	if ((rc = dm_decode_enumid(grp, authReqState)) ||
	    (rc = dm_decode_enumid(grp, authResult)) ||
	    (rc = dm_decode_uint32(grp, replyCode)))
	    	return rc;

	if (diam_avpgrp_get_avp(grp, &code, &flags, &vendor_id, &data, &len))
		*messages = NULL;
	else {
		if (code != AVP_CONTAINER || !len)
			return RC_ERR_MISC;
		if (!(*messages = diam_decode_avpgrp(NULL, data, len)))
			return RC_ERR_ALLOC;
	}

	return RC_OK;
}

		/* process AVP group returned by dm_send|register_get_passive_notifications or
		   received as an active notification callback parameter */

uint32_t
dm_decode_notifications(DIAM_AVPGRP *grp, uint32_t *type, DIAM_AVPGRP **notify)
{
	DIAM_AVPGRP	*ev_container;

	uint32_t	code;
	uint8_t		flags;
	uint32_t	vendor_id;
	void		*data;
	size_t		len;

	if (diam_avpgrp_get_avp(grp, &code, &flags, &vendor_id, &data, &len)) {
		*type = NOTIFY_NOTHING;	/* special notify type - queue was empty */
		if (notify)
			*notify = NULL;
		return RC_OK;
	}
	if (code != AVP_CONTAINER || !len)
		return RC_ERR_MISC;

	if (!(ev_container = diam_decode_avpgrp(grp, data, len)))
		return RC_ERR_ALLOC;

	if (diam_avpgrp_get_avp(ev_container, &code, &flags, &vendor_id,
				&data, &len) ||
	    code != AVP_NOTIFY_TYPE || len != sizeof(uint32_t)) {
		dm_grp_free(ev_container);
		return RC_ERR_MISC;
	}
	*type = diam_get_uint32_avp(data);

	if (notify)
		*notify = ev_container;
	else
		dm_grp_free(ev_container);

	return RC_OK;
}

		/* converts an arbitrary typed AVP data to an ASCII string */
uint32_t
dm_decode_unknown_as_string(uint32_t type, void *data, size_t len, char **val)
{
	int		af;
	union {
		struct in_addr	in;
		struct in6_addr	in6;
	} addr;
	char *dum;

	switch (type) {
	case AVP_BOOL:
		return (*val = strdup(diam_get_uint8_avp(data) ? "1" : "0"))
							? RC_OK : RC_ERR_ALLOC;
	case AVP_ENUMID:
	case AVP_INT32:
		return asprintf(val, "%d", diam_get_int32_avp(data)) == -1
							? RC_ERR_ALLOC : RC_OK;
	case AVP_COUNTER:
	case AVP_UINT32:
		return asprintf(val, "%u", diam_get_uint32_avp(data)) == -1
							? RC_ERR_ALLOC : RC_OK;
	case AVP_ABSTICKS:
	case AVP_RELTICKS:
	case AVP_INT64:
		return asprintf(val, "%" PRIi64, diam_get_int64_avp(data)) == -1
							? RC_ERR_ALLOC : RC_OK;
	case AVP_UINT64:
		return asprintf(val, "%" PRIu64, diam_get_uint64_avp(data)) == -1
							? RC_ERR_ALLOC : RC_OK;
	case AVP_ENUM:
	case AVP_PATH:
	case AVP_STRING:
		return (*val = strndup(data, len)) ? RC_OK : RC_ERR_ALLOC;
	case AVP_BINARY: {
		*val = malloc(((len + 3) * 4) / 3);
		if (!*val)
			return RC_ERR_ALLOC;

		dm_to64(data, len, *val);
		return RC_OK;
	}
	case AVP_ADDRESS:
		if (!diam_get_address_avp(&af, &addr, data) ||
		    af != AF_INET || !(dum = inet_ntoa(addr.in)))
			return RC_ERR_MISC;
		return (*val = strdup(dum)) ? RC_OK : RC_ERR_ALLOC;
	case AVP_DATE:
		return asprintf(val, "%u", (uint32_t)diam_get_time_avp(data)) == -1
							? RC_ERR_ALLOC : RC_OK;
	default:
		return RC_ERR_MISC;
	}

	/* never reached */
}

		/* process AVP group returned by dm_send|register_list */
		/* NOTE: this is mainly for backwards compatibility, LISTs can be recursive now. */
		/* it aborts if there's a node containing children, so it should only be used for "level 1" lists */

uint32_t
dm_decode_node_list(DIAM_AVPGRP *grp, char **name, uint32_t *type,
		    uint32_t *size, uint32_t *datatype)
{
	uint32_t	code;
	uint8_t		flags;
	uint32_t	vendor_id;
	void		*data;
	size_t		len;

	DIAM_AVPGRP	*node_container;

	if (diam_avpgrp_get_avp(grp, &code, &flags, &vendor_id, &data, &len) ||
	    code != AVP_CONTAINER || !len)
		return RC_ERR_MISC;

	if (!(node_container = diam_decode_avpgrp(NULL, data, len)))
		return RC_ERR_ALLOC;

	if (diam_avpgrp_get_avp(node_container, &code, &flags, &vendor_id,
				&data, &len) ||
	    code != AVP_NODE_NAME || !len) {
		dm_grp_free(node_container);
		return RC_ERR_MISC;
	}
	if (!(*name = strndup(data, len))) {
		dm_grp_free(node_container);
		return RC_ERR_ALLOC;
	}

	if (diam_avpgrp_get_avp(node_container, &code, &flags, &vendor_id,
				&data, &len) ||
	    code != AVP_NODE_TYPE || len != sizeof(uint32_t)) {
		dm_grp_free(node_container);
		return RC_ERR_MISC;
	}
	*type = diam_get_uint32_avp(data);

	if (*type == NODE_PARAMETER) {
		if (datatype) {
			if (diam_avpgrp_get_avp(node_container, &code, &flags,
						&vendor_id, &data, &len) ||
			    (code == AVP_NODE_DATATYPE && len != sizeof(uint32_t))) {
				dm_grp_free(node_container);
				return RC_ERR_MISC;
			}
			*datatype = code == AVP_NODE_DATATYPE ? diam_get_uint32_avp(data)
							      : code;
		}
	} else if (*type == NODE_OBJECT && size) {
		if (diam_avpgrp_get_avp(node_container, &code, &flags,
					&vendor_id, &data, &len) ||
		    code != AVP_NODE_SIZE || len != sizeof(uint32_t)) {
			dm_grp_free(node_container);
			return RC_ERR_MISC;
		}
		*size = diam_get_uint32_avp(data);
	}

	dm_grp_free(node_container);
	return RC_OK;
}
