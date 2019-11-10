#include <sys/socket.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <errno.h>
#ifndef IPPROTO_TCP
#include <netinet/in.h>
#endif

#include "tmate.h"
#include "tmate-protocol.h"

/*
 * The websocket refers to the websocket server.
 * (https://github.com/tmate-io/tmate-websocket)
 */

#define CONTROL_PROTOCOL_VERSION 2

#define pack(what, ...) _pack(&tmate_session->websocket_encoder, what, ##__VA_ARGS__)

#define pack_string_or_nil(str) ({ \
	if (str) \
		pack(string, str); \
	else \
		pack(nil); \
})

static void ctl_daemon_fwd_msg(__unused struct tmate_session *session,
			       struct tmate_unpacker *uk)
{
	if (uk->argc != 1)
		tmate_decoder_error();
	tmate_send_mc_obj(&uk->argv[0]);
}

static void do_snapshot(__unused struct tmate_unpacker *uk,
			unsigned int max_history_lines,
			struct window_pane *pane)
{
	struct screen *screen;
	struct grid *grid;
	struct grid_line *line;
	struct grid_cell gc;
	unsigned int line_i, i;
	unsigned int max_lines;
	size_t str_len;

	screen = &pane->base;
	grid = screen->grid;

	pack(array, 4);
	pack(int, pane->id);

	pack(array, 2);
	pack(int, screen->cx);
	pack(int, screen->cy);

	pack(unsigned_int, screen->mode);

	max_lines = max_history_lines + grid->sy;

#define grid_num_lines(grid) (grid->hsize + grid->sy)

	if (grid_num_lines(grid) > max_lines)
		line_i = grid_num_lines(grid) - max_lines;
	else
		line_i = 0;

	pack(array, grid_num_lines(grid) - line_i);
	for (; line_i < grid_num_lines(grid); line_i++) {
		line = &grid->linedata[line_i];

		pack(array, 2);
		str_len = 0;
		for (i = 0; i < line->cellsize; i++) {
			grid_get_cell(grid, i, line_i, &gc);
			str_len += gc.data.size;
		}

		pack(str, str_len);
		for (i = 0; i < line->cellsize; i++) {
			grid_get_cell(grid, i, line_i, &gc);
			pack(str_body, gc.data.data, gc.data.size);
		}

		pack(array, line->cellsize);
		for (i = 0; i < line->cellsize; i++) {
			grid_get_cell(grid, i, line_i, &gc);
			pack(unsigned_int, ((gc.flags << 24) |
					    (gc.attr  << 16) |
					    (gc.bg    << 8)  |
					     gc.fg        ));
		}
	}
}

static void ctl_daemon_request_snapshot(__unused struct tmate_session *session,
					struct tmate_unpacker *uk)
{
	struct session *s;
	struct winlink *wl;
	struct window *w;
	struct window_pane *pane;
	int max_history_lines;
	int num_panes;

	max_history_lines = unpack_int(uk);

	pack(array, 2);
	pack(int, TMATE_CTL_SNAPSHOT);

	s = RB_MIN(sessions, &sessions);
	if (!s)
		tmate_fatal("no session?");

	num_panes = 0;
	RB_FOREACH(wl, winlinks, &s->windows) {
		w = wl->window;
		if (!w)
			continue;

		TAILQ_FOREACH(pane, &w->panes, entry)
			num_panes++;
	}

	pack(array, num_panes);
	RB_FOREACH(wl, winlinks, &s->windows) {
		w = wl->window;
		if (!w)
			continue;

		TAILQ_FOREACH(pane, &w->panes, entry)
			do_snapshot(uk, max_history_lines, pane);
	}
}

static void ctl_pane_keys(__unused struct tmate_session *session,
			  struct tmate_unpacker *uk)
{
	int i;
	int pane_id;
	char *str;

	pane_id = unpack_int(uk);
	str = unpack_string(uk);

	/* a new protocol might be useful :) */
	/* TODO Make pane_id active! the pane_id arg is ignored! */
	for (i = 0; str[i]; i++)
		tmate_client_pane_key(pane_id, str[i]);

	free(str);
}

static void ctl_resize(struct tmate_session *session,
		       struct tmate_unpacker *uk)
{
	session->websocket_sx = (u_int)unpack_int(uk);
	session->websocket_sy = (u_int)unpack_int(uk);
	recalculate_sizes();
}

static void ctl_ssh_exec_response(struct tmate_session *session,
				  struct tmate_unpacker *uk)
{
	int exit_code;
	char *message;

	exit_code = unpack_int(uk);
	message = unpack_string(uk);

	tmate_dump_exec_response(session, exit_code, message);
	free(message);
}

static void ctl_rename_session(struct tmate_session *session,
			       struct tmate_unpacker *uk)
{
	char *stoken = unpack_string(uk);
	char *stoken_ro = unpack_string(uk);

	set_session_token(session, stoken);

	free(stoken);
	free(stoken_ro);
}

static void tmate_dispatch_websocket_message(struct tmate_session *session,
					     struct tmate_unpacker *uk)
{
	int cmd = unpack_int(uk);
	switch (cmd) {
#define dispatch(c, f) case c: f(session, uk); break
	dispatch(TMATE_CTL_DEAMON_FWD_MSG,	ctl_daemon_fwd_msg);
	dispatch(TMATE_CTL_REQUEST_SNAPSHOT,	ctl_daemon_request_snapshot);
	dispatch(TMATE_CTL_PANE_KEYS,		ctl_pane_keys);
	dispatch(TMATE_CTL_RESIZE,		ctl_resize);
	dispatch(TMATE_CTL_EXEC_RESPONSE,	ctl_ssh_exec_response);
	dispatch(TMATE_CTL_RENAME_SESSION,	ctl_rename_session);
	default: tmate_info("Bad websocket server message type: %d", cmd);
	}
}

void tmate_websocket_exec(struct tmate_session *session, const char *command)
{
	struct tmate_ssh_client *client = &session->ssh_client;

	if (!tmate_has_websocket())
		return;

	pack(array, 5);
	pack(int, TMATE_CTL_EXEC);
	pack(string, client->username);
	pack(string, client->ip_address);
	pack_string_or_nil(client->pubkey);
	pack(string, command);
}

void tmate_notify_client_join(__unused struct tmate_session *session,
			      struct client *c)
{
	tmate_info("Client joined (cid=%d)", c->id);

	if (!tmate_has_websocket())
		return;

	c->flags |= CLIENT_TMATE_NOTIFIED_JOIN;

	pack(array, 5);
	pack(int, TMATE_CTL_CLIENT_JOIN);
	pack(int, c->id);
	pack(string, c->ip_address);
	pack_string_or_nil(c->pubkey);
	pack(boolean, c->readonly);
}

void tmate_notify_client_left(__unused struct tmate_session *session,
			      struct client *c)
{
	if (!(c->flags & CLIENT_IDENTIFIED))
		return;

	tmate_info("Client left (cid=%d)", c->id);

	if (!tmate_has_websocket())
		return;

	if (!(c->flags & CLIENT_TMATE_NOTIFIED_JOIN))
		return;

	c->flags &= ~CLIENT_TMATE_NOTIFIED_JOIN;

	pack(array, 2);
	pack(int, TMATE_CTL_CLIENT_LEFT);
	pack(int, c->id);
}

void tmate_send_websocket_daemon_msg(__unused struct tmate_session *session,
				     struct tmate_unpacker *uk)
{
	int i;

	if (!tmate_has_websocket())
		return;

	pack(array, 2);
	pack(int, TMATE_CTL_DEAMON_OUT_MSG);

	pack(array, uk->argc);
	for (i = 0; i < uk->argc; i++)
		pack(object, uk->argv[i]);
}

void tmate_send_websocket_header(struct tmate_session *session)
{
	if (!tmate_has_websocket())
		return;

	pack(array, 9);
	pack(int, TMATE_CTL_HEADER);
	pack(int, CONTROL_PROTOCOL_VERSION);
	pack(string, session->ssh_client.ip_address);
	pack_string_or_nil(session->ssh_client.pubkey);
	pack(string, session->session_token);
	pack(string, session->session_token_ro);

	char *ssh_cmd_fmt = get_ssh_conn_string("%s");
	pack(string, ssh_cmd_fmt);
	free(ssh_cmd_fmt);

	pack(string, session->client_version);
	pack(int, session->client_protocol_version);
}

static void on_websocket_decoder_read(void *userdata, struct tmate_unpacker *uk)
{
	struct tmate_session *session = userdata;
	tmate_dispatch_websocket_message(session, uk);
}

static void on_websocket_read(__unused struct bufferevent *bev, void *_session)
{
	struct tmate_session *session = _session;
	struct evbuffer *websocket_in;
	ssize_t written;
	char *buf;
	size_t len;

	websocket_in = bufferevent_get_input(session->bev_websocket);

	while (evbuffer_get_length(websocket_in)) {
		tmate_decoder_get_buffer(&session->websocket_decoder, &buf, &len);

		if (len == 0)
			tmate_fatal("No more room in client decoder. Message too big?");

		written = evbuffer_remove(websocket_in, buf, len);
		if (written < 0)
			tmate_fatal("Cannot read websocket buffer");

		tmate_decoder_commit(&session->websocket_decoder, written);
	}
}

static void on_websocket_encoder_write(void *userdata, struct evbuffer *buffer)
{
	struct tmate_session *session = userdata;
	struct evbuffer *websocket_out;

	websocket_out = bufferevent_get_output(session->bev_websocket);

	if (evbuffer_add_buffer(websocket_out, buffer) < 0)
		tmate_fatal("Cannot write to websocket server buffer");
}

static void on_websocket_event_default(__unused struct tmate_session *session, short events)
{
	if (events & BEV_EVENT_EOF) {
		if (session->fin_received) {
			/*
			 * This is expected. The websocket will close the
			 * connection upon receiving the fin message.
			 */
			exit(0);
		}
		tmate_fatal("Connection to websocket server closed");
	}

	if (events & BEV_EVENT_ERROR)
		tmate_fatal("Connection to websocket server error: %s",
			    evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR()));
}

static void on_websocket_event(__unused struct bufferevent *bev, short events, void *_session)
{
	struct tmate_session *session = _session;
	session->on_websocket_error(session, events);
}

void tmate_init_websocket(struct tmate_session *session,
			  on_websocket_error_cb on_websocket_error)
{
	if (!tmate_has_websocket())
		return;

	session->websocket_sx = -1;
	session->websocket_sy = -1;

	/* session->websocket_fd is already connected */
	session->bev_websocket = bufferevent_socket_new(session->ev_base, session->websocket_fd,
						    BEV_OPT_CLOSE_ON_FREE);
	if (!session->bev_websocket)
		tmate_fatal("Cannot setup socket bufferevent");

	session->on_websocket_error = on_websocket_error ?: on_websocket_event_default;

	bufferevent_setcb(session->bev_websocket,
			  on_websocket_read, NULL, on_websocket_event, session);
	bufferevent_enable(session->bev_websocket, EV_READ | EV_WRITE);

	tmate_encoder_init(&session->websocket_encoder, on_websocket_encoder_write, session);
	tmate_decoder_init(&session->websocket_decoder, on_websocket_decoder_read, session);
}

static int _tmate_connect_to_websocket(const char *hostname, int port)
{
	int sockfd = -1;
	struct sockaddr_in servaddr;
	struct hostent *host;

	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0)
		tmate_fatal("Cannot create socket");

	host = gethostbyname(hostname);
	if (!host)
		tmate_fatal("Cannot resolve %s", hostname);

	memset(&servaddr, 0, sizeof(servaddr));
	servaddr.sin_family = host->h_addrtype;
	memcpy(&servaddr.sin_addr, host->h_addr, host->h_length);
	servaddr.sin_port = htons(port);

	if (connect(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0)
		tmate_fatal("Cannot connect to websocket server at %s:%d", hostname, port);

	{
	int flag = 1;
	if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0)
		tmate_fatal("Can't set websocket server socket to TCP_NODELAY");
	}

	if (fcntl(sockfd, F_SETFL, O_NONBLOCK) < 0)
		tmate_fatal("Can't set websocket server socket to non-blocking");

	tmate_debug("Connected to websocket server at %s:%d", hostname, port);

	return sockfd;
}

int tmate_connect_to_websocket(void)
{
	return _tmate_connect_to_websocket(tmate_settings->websocket_hostname,
					   tmate_settings->websocket_port);
}
