/*
 * Functions dedicated to statistics output
 *
 * Copyright 2000-2008 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>
#include <grp.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <common/cfgparse.h>
#include <common/compat.h>
#include <common/config.h>
#include <common/debug.h>
#include <common/memory.h>
#include <common/mini-clist.h>
#include <common/standard.h>
#include <common/ticks.h>
#include <common/time.h>
#include <common/uri_auth.h>
#include <common/version.h>

#include <types/global.h>

#include <proto/backend.h>
#include <proto/buffers.h>
#include <proto/dumpstats.h>
#include <proto/fd.h>
#include <proto/proto_uxst.h>
#include <proto/session.h>
#include <proto/server.h>
#include <proto/stream_interface.h>

/* This function parses a "stats" statement in the "global" section. It returns
 * -1 if there is any error, otherwise zero. If it returns -1, it may write an
 * error message into ther <err> buffer, for at most <errlen> bytes, trailing
 * zero included. The trailing '\n' must not be written. The function must be
 * called with <args> pointing to the first word after "stats".
 */
static int stats_parse_global(char **args, int section_type, struct proxy *curpx,
			      struct proxy *defpx, char *err, int errlen)
{
	args++;
	if (!strcmp(args[0], "socket")) {
		struct sockaddr_un su;
		int cur_arg;

		if (*args[1] == 0) {
			snprintf(err, errlen, "'stats socket' in global section expects a path to a UNIX socket");
			return -1;
		}

		if (global.stats_sock.state != LI_NEW) {
			snprintf(err, errlen, "'stats socket' already specified in global section");
			return -1;
		}

		su.sun_family = AF_UNIX;
		strncpy(su.sun_path, args[1], sizeof(su.sun_path));
		su.sun_path[sizeof(su.sun_path) - 1] = 0;
		memcpy(&global.stats_sock.addr, &su, sizeof(su)); // guaranteed to fit

		global.stats_sock.state = LI_INIT;
		global.stats_sock.options = LI_O_NONE;
		global.stats_sock.accept = uxst_event_accept;
		global.stats_sock.handler = process_uxst_stats;
		global.stats_sock.private = NULL;

		cur_arg = 2;
		while (*args[cur_arg]) {
			if (!strcmp(args[cur_arg], "uid")) {
				global.stats_sock.perm.ux.uid = atol(args[cur_arg + 1]);
				cur_arg += 2;
			}
			else if (!strcmp(args[cur_arg], "gid")) {
				global.stats_sock.perm.ux.gid = atol(args[cur_arg + 1]);
				cur_arg += 2;
			}
			else if (!strcmp(args[cur_arg], "mode")) {
				global.stats_sock.perm.ux.mode = strtol(args[cur_arg + 1], NULL, 8);
				cur_arg += 2;
			}
			else if (!strcmp(args[cur_arg], "user")) {
				struct passwd *user;
				user = getpwnam(args[cur_arg + 1]);
				if (!user) {
					snprintf(err, errlen, "unknown user '%s' in 'global' section ('stats user')",
						 args[cur_arg + 1]);
					return -1;
				}
				global.stats_sock.perm.ux.uid = user->pw_uid;
				cur_arg += 2;
			}
			else if (!strcmp(args[cur_arg], "group")) {
				struct group *group;
				group = getgrnam(args[cur_arg + 1]);
				if (!group) {
					snprintf(err, errlen, "unknown group '%s' in 'global' section ('stats group')",
						 args[cur_arg + 1]);
					return -1;
				}
				global.stats_sock.perm.ux.gid = group->gr_gid;
				cur_arg += 2;
			}
			else {
				snprintf(err, errlen, "'stats socket' only supports 'user', 'uid', 'group', 'gid', and 'mode'");
				return -1;
			}
		}
			
		uxst_add_listener(&global.stats_sock);
		global.maxsock++;
	}
	else if (!strcmp(args[0], "timeout")) {
		unsigned timeout;
		const char *res = parse_time_err(args[1], &timeout, TIME_UNIT_MS);

		if (res) {
			snprintf(err, errlen, "unexpected character '%c' in 'stats timeout' in 'global' section", *res);
			return -1;
		}

		if (!timeout) {
			snprintf(err, errlen, "a positive value is expected for 'stats timeout' in 'global section'");
			return -1;
		}
		global.stats_timeout = MS_TO_TICKS(timeout);
	}
	else if (!strcmp(args[0], "maxconn")) {
		int maxconn = atol(args[1]);

		if (maxconn <= 0) {
			snprintf(err, errlen, "a positive value is expected for 'stats maxconn' in 'global section'");
			return -1;
		}
		global.maxsock -= global.stats_sock.maxconn;
		global.stats_sock.maxconn = maxconn;
		global.maxsock += global.stats_sock.maxconn;
	}
	else {
		snprintf(err, errlen, "'stats' only supports 'socket', 'maxconn' and 'timeout' in 'global' section");
		return -1;
	}
	return 0;
}

int print_csv_header(struct chunk *msg, int size)
{
	return chunk_printf(msg, size,
			    "# pxname,svname,"
			    "qcur,qmax,"
			    "scur,smax,slim,stot,"
			    "bin,bout,"
			    "dreq,dresp,"
			    "ereq,econ,eresp,"
			    "wretr,wredis,"
			    "status,weight,act,bck,"
			    "chkfail,chkdown,lastchg,downtime,qlimit,"
			    "pid,iid,sid,throttle,lbtot,tracked,type,"
			    "\n");
}

/*
 * Produces statistics data for the session <s>. Expects to be called with
 * client socket shut down on input. It *may* make use of informations from
 * <uri>. s->data_ctx must have been zeroed first, and the flags properly set.
 * It returns 0 if it had to stop writing data and an I/O is needed, 1 if the
 * dump is finished and the session must be closed, or -1 in case of any error.
 */
int stats_dump_raw(struct session *s, struct uri_auth *uri)
{
	struct buffer *rep = s->rep;
	struct proxy *px;
	struct chunk msg;
	unsigned int up;

	msg.len = 0;
	msg.str = trash;

	switch (s->data_state) {
	case DATA_ST_INIT:
		/* the function had not been called yet, let's prepare the
		 * buffer for a response.
		 */
		stream_int_retnclose(rep->cons, &msg);
		s->data_state = DATA_ST_HEAD;
		/* fall through */

	case DATA_ST_HEAD:
		if (s->data_ctx.stats.flags & STAT_SHOW_STAT) {
			print_csv_header(&msg, sizeof(trash));
			if (buffer_write_chunk(rep, &msg) >= 0)
				return 0;
		}

		s->data_state = DATA_ST_INFO;
		/* fall through */

	case DATA_ST_INFO:
		up = (now.tv_sec - start_date.tv_sec);
		if (s->data_ctx.stats.flags & STAT_SHOW_INFO) {
			chunk_printf(&msg, sizeof(trash),
				     "Name: " PRODUCT_NAME "\n"
				     "Version: " HAPROXY_VERSION "\n"
				     "Release_date: " HAPROXY_DATE "\n"
				     "Nbproc: %d\n"
				     "Process_num: %d\n"
				     "Pid: %d\n"
				     "Uptime: %dd %dh%02dm%02ds\n"
				     "Uptime_sec: %d\n"
				     "Memmax_MB: %d\n"
				     "Ulimit-n: %d\n"
				     "Maxsock: %d\n"
				     "Maxconn: %d\n"
				     "CurrConns: %d\n"
				     "",
				     global.nbproc,
				     relative_pid,
				     pid,
				     up / 86400, (up % 86400) / 3600, (up % 3600) / 60, (up % 60),
				     up,
				     global.rlimit_memmax,
				     global.rlimit_nofile,
				     global.maxsock,
				     global.maxconn,
				     actconn
				     );
			if (buffer_write_chunk(rep, &msg) >= 0)
				return 0;
		}

		s->data_ctx.stats.px = proxy;
		s->data_ctx.stats.px_st = DATA_ST_PX_INIT;

		s->data_ctx.stats.sv = NULL;
		s->data_ctx.stats.sv_st = 0;

		s->data_state = DATA_ST_LIST;
		/* fall through */

	case DATA_ST_LIST:
		/* dump proxies */
		if (s->data_ctx.stats.flags & STAT_SHOW_STAT) {
			while (s->data_ctx.stats.px) {
				px = s->data_ctx.stats.px;
				/* skip the disabled proxies and non-networked ones */
				if (px->state != PR_STSTOPPED &&
				    (px->cap & (PR_CAP_FE | PR_CAP_BE)))
					if (stats_dump_proxy(s, px, NULL) == 0)
						return 0;

				s->data_ctx.stats.px = px->next;
				s->data_ctx.stats.px_st = DATA_ST_PX_INIT;
			}
			/* here, we just have reached the last proxy */
		}

		s->data_state = DATA_ST_END;
		/* fall through */

	case DATA_ST_END:
		s->data_state = DATA_ST_FIN;
		return 1;

	case DATA_ST_FIN:
		return 1;

	default:
		/* unknown state ! */
		return -1;
	}
}


/*
 * Produces statistics data for the session <s>. Expects to be called with
 * client socket shut down on input. It stops by itself by unsetting the
 * BF_HIJACK flag from the buffer, which it uses to keep on being called
 * when there is free space in the buffer, of simply by letting an empty buffer
 * upon return.s->data_ctx must have been zeroed before the first call, and the
 * flags set. It returns 0 if it had to stop writing data and an I/O is needed,
 * 1 if the dump is finished and the session must be closed, or -1 in case of
 * any error.
 */
int stats_dump_http(struct session *s, struct uri_auth *uri)
{
	struct buffer *rep = s->rep;
	struct proxy *px;
	struct chunk msg;
	unsigned int up;

	msg.len = 0;
	msg.str = trash;

	switch (s->data_state) {
	case DATA_ST_INIT:
		/* the function had not been called yet */
		buffer_start_hijack(rep);

		chunk_printf(&msg, sizeof(trash),
			     "HTTP/1.0 200 OK\r\n"
			     "Cache-Control: no-cache\r\n"
			     "Connection: close\r\n"
			     "Content-Type: %s\r\n",
			     (s->data_ctx.stats.flags & STAT_FMT_CSV) ? "text/plain" : "text/html");

		if (uri->refresh > 0 && !(s->data_ctx.stats.flags & STAT_NO_REFRESH))
			chunk_printf(&msg, sizeof(trash), "Refresh: %d\r\n",
				     uri->refresh);

		chunk_printf(&msg, sizeof(trash), "\r\n");

		s->txn.status = 200;
		stream_int_retnclose(rep->cons, &msg); // send the start of the response.
		msg.len = 0;

		if (!(s->flags & SN_ERR_MASK))  // this is not really an error but it is
			s->flags |= SN_ERR_PRXCOND; // to mark that it comes from the proxy
		if (!(s->flags & SN_FINST_MASK))
			s->flags |= SN_FINST_R;

		if (s->txn.meth == HTTP_METH_HEAD) {
			/* that's all we return in case of HEAD request */
			s->data_state = DATA_ST_FIN;
			buffer_stop_hijack(rep);
			return 1;
		}

		s->data_state = DATA_ST_HEAD; /* let's start producing data */
		/* fall through */

	case DATA_ST_HEAD:
		if (!(s->data_ctx.stats.flags & STAT_FMT_CSV)) {
			/* WARNING! This must fit in the first buffer !!! */	    
			chunk_printf(&msg, sizeof(trash),
			     "<html><head><title>Statistics Report for " PRODUCT_NAME "</title>\n"
			     "<meta http-equiv=\"content-type\" content=\"text/html; charset=iso-8859-1\">\n"
			     "<style type=\"text/css\"><!--\n"
			     "body {"
			     " font-family: helvetica, arial;"
			     " font-size: 12px;"
			     " font-weight: normal;"
			     " color: black;"
			     " background: white;"
			     "}\n"
			     "th,td {"
			     " font-size: 0.8em;"
			     " align: center;"
			     "}\n"
			     "h1 {"
			     " font-size: xx-large;"
			     " margin-bottom: 0.5em;"
			     "}\n"
			     "h2 {"
			     " font-family: helvetica, arial;"
			     " font-size: x-large;"
			     " font-weight: bold;"
			     " font-style: italic;"
			     " color: #6020a0;"
			     " margin-top: 0em;"
			     " margin-bottom: 0em;"
			     "}\n"
			     "h3 {"
			     " font-family: helvetica, arial;"
			     " font-size: 16px;"
			     " font-weight: bold;"
			     " color: #b00040;"
			     " background: #e8e8d0;"
			     " margin-top: 0em;"
			     " margin-bottom: 0em;"
			     "}\n"
			     "li {"
			     " margin-top: 0.25em;"
			     " margin-right: 2em;"
			     "}\n"
			     ".hr {margin-top: 0.25em;"
			     " border-color: black;"
			     " border-bottom-style: solid;"
			     "}\n"
			     ".pxname	{background: #b00040;color: #ffff40;font-weight: bold;}\n"
			     ".titre	{background: #20D0D0;color: #000000;font-weight: bold;}\n"
			     ".total	{background: #20D0D0;color: #ffff80;}\n"
			     ".frontend	{background: #e8e8d0;}\n"
			     ".backend	{background: #e8e8d0;}\n"
			     ".active0	{background: #ff9090;}\n"
			     ".active1	{background: #ffd020;}\n"
			     ".active2	{background: #ffffa0;}\n"
			     ".active3	{background: #c0ffc0;}\n"
			     ".active4	{background: #ffffa0;}\n"  /* NOLB state shows same as going down */
			     ".active5	{background: #a0e0a0;}\n"  /* NOLB state shows darker than up */
			     ".active6	{background: #e0e0e0;}\n"
			     ".backup0	{background: #ff9090;}\n"
			     ".backup1	{background: #ff80ff;}\n"
			     ".backup2	{background: #c060ff;}\n"
			     ".backup3	{background: #b0d0ff;}\n"
			     ".backup4	{background: #c060ff;}\n"  /* NOLB state shows same as going down */
			     ".backup5	{background: #90b0e0;}\n"  /* NOLB state shows same as going down */
			     ".backup6	{background: #e0e0e0;}\n"
			     "table.tbl { border-collapse: collapse; border-style: none;}\n"
			     "table.tbl td { border-width: 1px 1px 1px 1px; border-style: solid solid solid solid; padding: 2px 3px; border-color: gray;}\n"
			     "table.tbl th { border-width: 1px; border-style: solid solid solid solid; border-color: gray;}\n"
			     "table.tbl th.empty { border-style: none; empty-cells: hide;}\n"
			     "table.lgd { border-collapse: collapse; border-width: 1px; border-style: none none none solid; border-color: black;}\n"
			     "table.lgd td { border-width: 1px; border-style: solid solid solid solid; border-color: gray; padding: 2px;}\n"
			     "table.lgd td.noborder { border-style: none; padding: 2px; white-space: nowrap;}\n"
			     "-->\n"
			     "</style></head>\n");
		} else {
			print_csv_header(&msg, sizeof(trash));
		}
		if (buffer_write_chunk(rep, &msg) >= 0)
			return 0;

		s->data_state = DATA_ST_INFO;
		/* fall through */

	case DATA_ST_INFO:
		up = (now.tv_sec - start_date.tv_sec);

		/* WARNING! this has to fit the first packet too.
			 * We are around 3.5 kB, add adding entries will
			 * become tricky if we want to support 4kB buffers !
			 */
		if (!(s->data_ctx.stats.flags & STAT_FMT_CSV)) {
			chunk_printf(&msg, sizeof(trash),
			     "<body><h1><a href=\"" PRODUCT_URL "\" style=\"text-decoration: none;\">"
			     PRODUCT_NAME "%s</a></h1>\n"
			     "<h2>Statistics Report for pid %d</h2>\n"
			     "<hr width=\"100%%\" class=\"hr\">\n"
			     "<h3>&gt; General process information</h3>\n"
			     "<table border=0 cols=4><tr><td align=\"left\" nowrap width=\"1%%\">\n"
			     "<p><b>pid = </b> %d (process #%d, nbproc = %d)<br>\n"
			     "<b>uptime = </b> %dd %dh%02dm%02ds<br>\n"
			     "<b>system limits :</b> memmax = %s%s ; ulimit-n = %d<br>\n"
			     "<b>maxsock = </b> %d<br>\n"
			     "<b>maxconn = </b> %d (current conns = %d)<br>\n"
			     "</td><td align=\"center\" nowrap>\n"
			     "<table class=\"lgd\"><tr>\n"
			     "<td class=\"active3\">&nbsp;</td><td class=\"noborder\">active UP </td>"
			     "<td class=\"backup3\">&nbsp;</td><td class=\"noborder\">backup UP </td>"
			     "</tr><tr>\n"
			     "<td class=\"active2\"></td><td class=\"noborder\">active UP, going down </td>"
			     "<td class=\"backup2\"></td><td class=\"noborder\">backup UP, going down </td>"
			     "</tr><tr>\n"
			     "<td class=\"active1\"></td><td class=\"noborder\">active DOWN, going up </td>"
			     "<td class=\"backup1\"></td><td class=\"noborder\">backup DOWN, going up </td>"
			     "</tr><tr>\n"
			     "<td class=\"active0\"></td><td class=\"noborder\">active or backup DOWN &nbsp;</td>"
			     "<td class=\"active6\"></td><td class=\"noborder\">not checked </td>"
			     "</tr></table>\n"
			     "Note: UP with load-balancing disabled is reported as \"NOLB\"."
			     "</td>"
			     "<td align=\"left\" valign=\"top\" nowrap width=\"1%%\">"
			     "<b>Display option:</b><ul style=\"margin-top: 0.25em;\">"
			     "",
			     (uri->flags&ST_HIDEVER)?"":(STATS_VERSION_STRING),
			     pid, pid,
			     relative_pid, global.nbproc,
			     up / 86400, (up % 86400) / 3600,
			     (up % 3600) / 60, (up % 60),
			     global.rlimit_memmax ? ultoa(global.rlimit_memmax) : "unlimited",
			     global.rlimit_memmax ? " MB" : "",
			     global.rlimit_nofile,
			     global.maxsock,
			     global.maxconn,
			     actconn
			     );
	    
			if (s->data_ctx.stats.flags & STAT_HIDE_DOWN)
				chunk_printf(&msg, sizeof(trash),
				     "<li><a href=\"%s%s%s\">Show all servers</a><br>\n",
				     uri->uri_prefix,
				     "",
				     (s->data_ctx.stats.flags & STAT_NO_REFRESH) ? ";norefresh" : "");
			else
				chunk_printf(&msg, sizeof(trash),
				     "<li><a href=\"%s%s%s\">Hide 'DOWN' servers</a><br>\n",
				     uri->uri_prefix,
				     ";up",
				     (s->data_ctx.stats.flags & STAT_NO_REFRESH) ? ";norefresh" : "");

			if (uri->refresh > 0) {
				if (s->data_ctx.stats.flags & STAT_NO_REFRESH)
					chunk_printf(&msg, sizeof(trash),
					     "<li><a href=\"%s%s%s\">Enable refresh</a><br>\n",
					     uri->uri_prefix,
					     (s->data_ctx.stats.flags & STAT_HIDE_DOWN) ? ";up" : "",
					     "");
				else
					chunk_printf(&msg, sizeof(trash),
					     "<li><a href=\"%s%s%s\">Disable refresh</a><br>\n",
					     uri->uri_prefix,
					     (s->data_ctx.stats.flags & STAT_HIDE_DOWN) ? ";up" : "",
					     ";norefresh");
			}

			chunk_printf(&msg, sizeof(trash),
			     "<li><a href=\"%s%s%s\">Refresh now</a><br>\n",
			     uri->uri_prefix,
			     (s->data_ctx.stats.flags & STAT_HIDE_DOWN) ? ";up" : "",
			     (s->data_ctx.stats.flags & STAT_NO_REFRESH) ? ";norefresh" : "");

			chunk_printf(&msg, sizeof(trash),
			     "<li><a href=\"%s;csv%s\">CSV export</a><br>\n",
			     uri->uri_prefix,
			     (uri->refresh > 0) ? ";norefresh" : "");

			chunk_printf(&msg, sizeof(trash),
			     "</td>"
			     "<td align=\"left\" valign=\"top\" nowrap width=\"1%%\">"
			     "<b>External ressources:</b><ul style=\"margin-top: 0.25em;\">\n"
			     "<li><a href=\"" PRODUCT_URL "\">Primary site</a><br>\n"
			     "<li><a href=\"" PRODUCT_URL_UPD "\">Updates (v" PRODUCT_BRANCH ")</a><br>\n"
			     "<li><a href=\"" PRODUCT_URL_DOC "\">Online manual</a><br>\n"
			     "</ul>"
			     "</td>"
			     "</tr></table>\n"
			     ""
			     );
	    
			if (buffer_write_chunk(rep, &msg) >= 0)
				return 0;
		}

		s->data_ctx.stats.px = proxy;
		s->data_ctx.stats.px_st = DATA_ST_PX_INIT;
		s->data_state = DATA_ST_LIST;
		/* fall through */

	case DATA_ST_LIST:
		/* dump proxies */
		while (s->data_ctx.stats.px) {
			px = s->data_ctx.stats.px;
			/* skip the disabled proxies and non-networked ones */
			if (px->state != PR_STSTOPPED && (px->cap & (PR_CAP_FE | PR_CAP_BE)))
				if (stats_dump_proxy(s, px, uri) == 0)
					return 0;

			s->data_ctx.stats.px = px->next;
			s->data_ctx.stats.px_st = DATA_ST_PX_INIT;
		}
		/* here, we just have reached the last proxy */

		s->data_state = DATA_ST_END;
		/* fall through */

	case DATA_ST_END:
		if (!(s->data_ctx.stats.flags & STAT_FMT_CSV)) {
			chunk_printf(&msg, sizeof(trash), "</body></html>\n");
			if (buffer_write_chunk(rep, &msg) >= 0)
				return 0;
		}

		s->data_state = DATA_ST_FIN;
		/* fall through */

	case DATA_ST_FIN:
		buffer_stop_hijack(rep);
		return 1;

	default:
		/* unknown state ! */
		buffer_stop_hijack(rep);
		return -1;
	}
}


/*
 * Dumps statistics for a proxy.
 * Returns 0 if it had to stop dumping data because of lack of buffer space,
 * ot non-zero if everything completed.
 */
int stats_dump_proxy(struct session *s, struct proxy *px, struct uri_auth *uri)
{
	struct buffer *rep = s->rep;
	struct server *sv, *svs;	/* server and server-state, server-state=server or server->tracked */
	struct chunk msg;

	msg.len = 0;
	msg.str = trash;

	switch (s->data_ctx.stats.px_st) {
	case DATA_ST_PX_INIT:
		/* we are on a new proxy */

		if (uri && uri->scope) {
			/* we have a limited scope, we have to check the proxy name */
			struct stat_scope *scope;
			int len;

			len = strlen(px->id);
			scope = uri->scope;

			while (scope) {
				/* match exact proxy name */
				if (scope->px_len == len && !memcmp(px->id, scope->px_id, len))
					break;

				/* match '.' which means 'self' proxy */
				if (!strcmp(scope->px_id, ".") && px == s->be)
					break;
				scope = scope->next;
			}

			/* proxy name not found : don't dump anything */
			if (scope == NULL)
				return 1;
		}

		if ((s->data_ctx.stats.flags & STAT_BOUND) && (s->data_ctx.stats.iid != -1) &&
			(px->uuid != s->data_ctx.stats.iid))
			return 1;

		s->data_ctx.stats.px_st = DATA_ST_PX_TH;
		/* fall through */

	case DATA_ST_PX_TH:
		if (!(s->data_ctx.stats.flags & STAT_FMT_CSV)) {
			/* print a new table */
			chunk_printf(&msg, sizeof(trash),
				     "<table cols=\"26\" class=\"tbl\" width=\"100%%\">\n"
				     "<tr align=\"center\" class=\"titre\">"
				     "<th colspan=2 class=\"pxname\">%s</th>"
				     "<th colspan=24 class=\"empty\"></th>"
				     "</tr>\n"
				     "<tr align=\"center\" class=\"titre\">"
				     "<th rowspan=2></th>"
				     "<th colspan=3>Queue</th><th colspan=5>Sessions</th>"
				     "<th colspan=2>Bytes</th><th colspan=2>Denied</th>"
				     "<th colspan=3>Errors</th><th colspan=2>Warnings</th>"
				     "<th colspan=8>Server</th>"
				     "</tr>\n"
				     "<tr align=\"center\" class=\"titre\">"
				     "<th>Cur</th><th>Max</th><th>Limit</th><th>Cur</th><th>Max</th>"
				     "<th>Limit</th><th>Total</th><th>LbTot</th><th>In</th><th>Out</th>"
				     "<th>Req</th><th>Resp</th><th>Req</th><th>Conn</th>"
				     "<th>Resp</th><th>Retr</th><th>Redis</th>"
				     "<th>Status</th><th>Wght</th><th>Act</th>"
				     "<th>Bck</th><th>Chk</th><th>Dwn</th><th>Dwntme</th>"
				     "<th>Thrtle</th>\n"
				     "</tr>",
				     px->id);

			if (buffer_write_chunk(rep, &msg) >= 0)
				return 0;
		}

		s->data_ctx.stats.px_st = DATA_ST_PX_FE;
		/* fall through */

	case DATA_ST_PX_FE:
		/* print the frontend */
		if ((px->cap & PR_CAP_FE) &&
		    (!(s->data_ctx.stats.flags & STAT_BOUND) || (s->data_ctx.stats.type & (1 << STATS_TYPE_FE)))) {
			if (!(s->data_ctx.stats.flags & STAT_FMT_CSV)) {
				chunk_printf(&msg, sizeof(trash),
				     /* name, queue */
				     "<tr align=center class=\"frontend\"><td>Frontend</td><td colspan=3></td>"
				     /* sessions : current, max, limit, total, lbtot */
				     "<td align=right>%d</td><td align=right>%d</td>"
				     "<td align=right>%d</td><td align=right>%d</td>"
				     "<td align=right></td>"
				     /* bytes : in, out */
				     "<td align=right>%lld</td><td align=right>%lld</td>"
				     /* denied: req, resp */
				     "<td align=right>%d</td><td align=right>%d</td>"
				     /* errors : request, connect, response */
				     "<td align=right>%d</td><td align=right></td><td align=right></td>"
				     /* warnings: retries, redispatches */
				     "<td align=right></td><td align=right></td>"
				     /* server status : reflect frontend status */
				     "<td align=center>%s</td>"
				     /* rest of server: nothing */
				     "<td align=center colspan=7></td></tr>"
				     "",
				     px->feconn, px->feconn_max, px->maxconn, px->cum_feconn,
				     px->bytes_in, px->bytes_out,
				     px->denied_req, px->denied_resp,
				     px->failed_req,
				     px->state == PR_STRUN ? "OPEN" :
				     px->state == PR_STIDLE ? "FULL" : "STOP");
			} else {
				chunk_printf(&msg, sizeof(trash),
				     /* pxid, name, queue cur, queue max, */
				     "%s,FRONTEND,,,"
				     /* sessions : current, max, limit, total */
				     "%d,%d,%d,%d,"
				     /* bytes : in, out */
				     "%lld,%lld,"
				     /* denied: req, resp */
				     "%d,%d,"
				     /* errors : request, connect, response */
				     "%d,,,"
				     /* warnings: retries, redispatches */
				     ",,"
				     /* server status : reflect frontend status */
				     "%s,"
				     /* rest of server: nothing */
				     ",,,,,,,,"
				     /* pid, iid, sid, throttle, lbtot, tracked, type */
				     "%d,%d,0,,,,%d,"
				     "\n",
				     px->id,
				     px->feconn, px->feconn_max, px->maxconn, px->cum_feconn,
				     px->bytes_in, px->bytes_out,
				     px->denied_req, px->denied_resp,
				     px->failed_req,
				     px->state == PR_STRUN ? "OPEN" :
				     px->state == PR_STIDLE ? "FULL" : "STOP",
				     relative_pid, px->uuid, STATS_TYPE_FE);
			}

			if (buffer_write_chunk(rep, &msg) >= 0)
				return 0;
		}

		s->data_ctx.stats.sv = px->srv; /* may be NULL */
		s->data_ctx.stats.px_st = DATA_ST_PX_SV;
		/* fall through */

	case DATA_ST_PX_SV:
		/* stats.sv has been initialized above */
		for (; s->data_ctx.stats.sv != NULL; s->data_ctx.stats.sv = sv->next) {

			int sv_state; /* 0=DOWN, 1=going up, 2=going down, 3=UP, 4,5=NOLB, 6=unchecked */

			sv = s->data_ctx.stats.sv;

			if (s->data_ctx.stats.flags & STAT_BOUND) {
				if (!(s->data_ctx.stats.type & (1 << STATS_TYPE_SV)))
					break;

				if (s->data_ctx.stats.sid != -1 && sv->puid != s->data_ctx.stats.sid)
					continue;
			}

			if (sv->tracked)
				svs = sv->tracked;
			else
				svs = sv;

			/* FIXME: produce some small strings for "UP/DOWN x/y &#xxxx;" */
			if (!(svs->state & SRV_CHECKED))
				sv_state = 6;
			else if (svs->state & SRV_RUNNING) {
				if (svs->health == svs->rise + svs->fall - 1)
					sv_state = 3; /* UP */
				else
					sv_state = 2; /* going down */

				if (svs->state & SRV_GOINGDOWN)
					sv_state += 2;
			}
			else
				if (svs->health)
					sv_state = 1; /* going up */
				else
					sv_state = 0; /* DOWN */

			if ((sv_state == 0) && (s->data_ctx.stats.flags & STAT_HIDE_DOWN)) {
				/* do not report servers which are DOWN */
				s->data_ctx.stats.sv = sv->next;
				continue;
			}

			if (!(s->data_ctx.stats.flags & STAT_FMT_CSV)) {
				static char *srv_hlt_st[7] = { "DOWN", "DN %d/%d &uarr;",
							       "UP %d/%d &darr;", "UP",
							       "NOLB %d/%d &darr;", "NOLB",
							       "<i>no check</i>" };
				chunk_printf(&msg, sizeof(trash),
				     /* name */
				     "<tr align=\"center\" class=\"%s%d\"><td>%s</td>"
				     /* queue : current, max, limit */
				     "<td align=right>%d</td><td align=right>%d</td><td align=right>%s</td>"
				     /* sessions : current, max, limit, total, lbtot */
				     "<td align=right>%d</td><td align=right>%d</td>"
				     "<td align=right>%s</td><td align=right>%d</td>"
				     "<td align=right>%d</td>"
				     /* bytes : in, out */
				     "<td align=right>%lld</td><td align=right>%lld</td>"
				     /* denied: req, resp */
				     "<td align=right></td><td align=right>%d</td>"
				     /* errors : request, connect, response */
				     "<td align=right></td><td align=right>%d</td><td align=right>%d</td>\n"
				     /* warnings: retries, redispatches */
				     "<td align=right>%u</td><td align=right>%u</td>"
				     "",
				     (sv->state & SRV_BACKUP) ? "backup" : "active",
				     sv_state, sv->id,
				     sv->nbpend, sv->nbpend_max, LIM2A0(sv->maxqueue, "-"),
				     sv->cur_sess, sv->cur_sess_max, LIM2A1(sv->maxconn, "-"),
				     sv->cum_sess, sv->cum_lbconn,
				     sv->bytes_in, sv->bytes_out,
				     sv->failed_secu,
				     sv->failed_conns, sv->failed_resp,
				     sv->retries, sv->redispatches);
				     
				/* status */
				chunk_printf(&msg, sizeof(trash), "<td nowrap>");

				if (sv->state & SRV_CHECKED)
					chunk_printf(&msg, sizeof(trash), "%s ",
						human_time(now.tv_sec - sv->last_change, 1));

				chunk_printf(&msg, sizeof(trash),
				     srv_hlt_st[sv_state],
				     (svs->state & SRV_RUNNING) ? (svs->health - svs->rise + 1) : (svs->health),
				     (svs->state & SRV_RUNNING) ? (svs->fall) : (svs->rise));

				chunk_printf(&msg, sizeof(trash),
				     /* weight */
				     "</td><td>%d</td>"
				     /* act, bck */
				     "<td>%s</td><td>%s</td>"
				     "",
				     (sv->eweight * px->lbprm.wmult + px->lbprm.wdiv - 1) / px->lbprm.wdiv,
				     (sv->state & SRV_BACKUP) ? "-" : "Y",
				     (sv->state & SRV_BACKUP) ? "Y" : "-");

				/* check failures: unique, fatal, down time */
				if (sv->state & SRV_CHECKED)
					chunk_printf(&msg, sizeof(trash),
					     "<td align=right>%d</td><td align=right>%d</td>"
					     "<td nowrap align=right>%s</td>"
					     "",
					     svs->failed_checks, svs->down_trans,
					     human_time(srv_downtime(sv), 1));
				else if (sv != svs)
					chunk_printf(&msg, sizeof(trash),
					     "<td nowrap colspan=3>via %s/%s</td>", svs->proxy->id, svs->id );
				else
					chunk_printf(&msg, sizeof(trash),
					     "<td colspan=3></td>");

				/* throttle */
				if ((sv->state & SRV_WARMINGUP) &&
				    now.tv_sec < sv->last_change + sv->slowstart &&
				    now.tv_sec >= sv->last_change) {
					unsigned int ratio;
					ratio = MAX(1, 100 * (now.tv_sec - sv->last_change) / sv->slowstart);
					chunk_printf(&msg, sizeof(trash),
						     "<td>%d %%</td></tr>\n", ratio);
				} else {
					chunk_printf(&msg, sizeof(trash),
						     "<td>-</td></tr>\n");
				}
			} else {
				static char *srv_hlt_st[7] = { "DOWN,", "DOWN %d/%d,",
							       "UP %d/%d,", "UP,",
							       "NOLB %d/%d,", "NOLB,",
							       "no check," };
				chunk_printf(&msg, sizeof(trash),
				     /* pxid, name */
				     "%s,%s,"
				     /* queue : current, max */
				     "%d,%d,"
				     /* sessions : current, max, limit, total */
				     "%d,%d,%s,%d,"
				     /* bytes : in, out */
				     "%lld,%lld,"
				     /* denied: req, resp */
				     ",%d,"
				     /* errors : request, connect, response */
				     ",%d,%d,"
				     /* warnings: retries, redispatches */
				     "%u,%u,"
				     "",
				     px->id, sv->id,
				     sv->nbpend, sv->nbpend_max,
				     sv->cur_sess, sv->cur_sess_max, LIM2A0(sv->maxconn, ""), sv->cum_sess,
				     sv->bytes_in, sv->bytes_out,
				     sv->failed_secu,
				     sv->failed_conns, sv->failed_resp,
				     sv->retries, sv->redispatches);
				     
				/* status */
				chunk_printf(&msg, sizeof(trash),
				     srv_hlt_st[sv_state],
				     (sv->state & SRV_RUNNING) ? (sv->health - sv->rise + 1) : (sv->health),
				     (sv->state & SRV_RUNNING) ? (sv->fall) : (sv->rise));

				chunk_printf(&msg, sizeof(trash),
				     /* weight, active, backup */
				     "%d,%d,%d,"
				     "",
				     (sv->eweight * px->lbprm.wmult + px->lbprm.wdiv - 1) / px->lbprm.wdiv,
				     (sv->state & SRV_BACKUP) ? 0 : 1,
				     (sv->state & SRV_BACKUP) ? 1 : 0);

				/* check failures: unique, fatal; last change, total downtime */
				if (sv->state & SRV_CHECKED)
					chunk_printf(&msg, sizeof(trash),
					     "%d,%d,%d,%d,",
					     sv->failed_checks, sv->down_trans,
					     now.tv_sec - sv->last_change, srv_downtime(sv));
				else
					chunk_printf(&msg, sizeof(trash),
					     ",,,,");

				/* queue limit, pid, iid, sid, */
				chunk_printf(&msg, sizeof(trash),
				     "%s,"
				     "%d,%d,%d,",
				     LIM2A0(sv->maxqueue, ""),
				     relative_pid, px->uuid, sv->puid);

				/* throttle */
				if ((sv->state & SRV_WARMINGUP) &&
				    now.tv_sec < sv->last_change + sv->slowstart &&
				    now.tv_sec >= sv->last_change) {
					unsigned int ratio;
					ratio = MAX(1, 100 * (now.tv_sec - sv->last_change) / sv->slowstart);
					chunk_printf(&msg, sizeof(trash), "%d", ratio);
				}

				/* sessions: lbtot */
				chunk_printf(&msg, sizeof(trash), ",%d,", sv->cum_lbconn);

				/* tracked */
				if (sv->tracked)
					chunk_printf(&msg, sizeof(trash), "%s/%s,",
						sv->tracked->proxy->id, sv->tracked->id);
				else
					chunk_printf(&msg, sizeof(trash), ",");

				/* type, then EOL */
				chunk_printf(&msg, sizeof(trash), "%d,\n", STATS_TYPE_SV);
			}
			if (buffer_write_chunk(rep, &msg) >= 0)
				return 0;
		} /* for sv */

		s->data_ctx.stats.px_st = DATA_ST_PX_BE;
		/* fall through */

	case DATA_ST_PX_BE:
		/* print the backend */
		if ((px->cap & PR_CAP_BE) &&
		    (!(s->data_ctx.stats.flags & STAT_BOUND) || (s->data_ctx.stats.type & (1 << STATS_TYPE_BE)))) {
			if (!(s->data_ctx.stats.flags & STAT_FMT_CSV)) {
				chunk_printf(&msg, sizeof(trash),
				     /* name */
				     "<tr align=center class=\"backend\"><td>Backend</td>"
				     /* queue : current, max */
				     "<td align=right>%d</td><td align=right>%d</td><td></td>"
				     /* sessions : current, max, limit, total, lbtot */
				     "<td align=right>%d</td><td align=right>%d</td>"
				     "<td align=right>%d</td><td align=right>%d</td>"
				     "<td align=right>%d</td>"
				     /* bytes : in, out */
				     "<td align=right>%lld</td><td align=right>%lld</td>"
				     /* denied: req, resp */
				     "<td align=right>%d</td><td align=right>%d</td>"
				     /* errors : request, connect, response */
				     "<td align=right></td><td align=right>%d</td><td align=right>%d</td>\n"
				     /* warnings: retries, redispatches */
				     "<td align=right>%u</td><td align=right>%u</td>"
				     /* backend status: reflect backend status (up/down): we display UP
				      * if the backend has known working servers or if it has no server at
				      * all (eg: for stats). Then we display the total weight, number of
				      * active and backups. */
				     "<td align=center nowrap>%s %s</td><td align=center>%d</td>"
				     "<td align=center>%d</td><td align=center>%d</td>",
				     px->nbpend /* or px->totpend ? */, px->nbpend_max,
				     px->beconn, px->beconn_max, px->fullconn, px->cum_beconn, px->cum_lbconn,
				     px->bytes_in, px->bytes_out,
				     px->denied_req, px->denied_resp,
				     px->failed_conns, px->failed_resp,
				     px->retries, px->redispatches,
				     human_time(now.tv_sec - px->last_change, 1),
				     (px->lbprm.tot_weight > 0 || !px->srv) ? "UP" :
					     "<font color=\"red\"><b>DOWN</b></font>",
				     (px->lbprm.tot_weight * px->lbprm.wmult + px->lbprm.wdiv - 1) / px->lbprm.wdiv,
				     px->srv_act, px->srv_bck);

				chunk_printf(&msg, sizeof(trash),
				     /* rest of backend: nothing, down transitions, total downtime, throttle */
				     "<td align=center>&nbsp;</td><td align=\"right\">%d</td>"
				     "<td align=\"right\" nowrap>%s</td>"
				     "<td></td>"
				     "</tr>",
				     px->down_trans,
				     px->srv?human_time(be_downtime(px), 1):"&nbsp;");
			} else {
				chunk_printf(&msg, sizeof(trash),
				     /* pxid, name */
				     "%s,BACKEND,"
				     /* queue : current, max */
				     "%d,%d,"
				     /* sessions : current, max, limit, total */
				     "%d,%d,%d,%d,"
				     /* bytes : in, out */
				     "%lld,%lld,"
				     /* denied: req, resp */
				     "%d,%d,"
				     /* errors : request, connect, response */
				     ",%d,%d,"
				     /* warnings: retries, redispatches */
				     "%u,%u,"
				     /* backend status: reflect backend status (up/down): we display UP
				      * if the backend has known working servers or if it has no server at
				      * all (eg: for stats). Then we display the total weight, number of
				      * active and backups. */
				     "%s,"
				     "%d,%d,%d,"
				     /* rest of backend: nothing, down transitions, last change, total downtime */
				     ",%d,%d,%d,,"
				     /* pid, iid, sid, throttle, lbtot, tracked, type */
				     "%d,%d,0,,%d,,%d,"
				     "\n",
				     px->id,
				     px->nbpend /* or px->totpend ? */, px->nbpend_max,
				     px->beconn, px->beconn_max, px->fullconn, px->cum_beconn,
				     px->bytes_in, px->bytes_out,
				     px->denied_req, px->denied_resp,
				     px->failed_conns, px->failed_resp,
				     px->retries, px->redispatches,
				     (px->lbprm.tot_weight > 0 || !px->srv) ? "UP" : "DOWN",
				     (px->lbprm.tot_weight * px->lbprm.wmult + px->lbprm.wdiv - 1) / px->lbprm.wdiv,
				     px->srv_act, px->srv_bck,
				     px->down_trans, now.tv_sec - px->last_change,
				     px->srv?be_downtime(px):0,
				     relative_pid, px->uuid,
				     px->cum_lbconn, STATS_TYPE_BE);
			}
			if (buffer_write_chunk(rep, &msg) >= 0)
				return 0;
		}
		
		s->data_ctx.stats.px_st = DATA_ST_PX_END;
		/* fall through */

	case DATA_ST_PX_END:
		if (!(s->data_ctx.stats.flags & STAT_FMT_CSV)) {
			chunk_printf(&msg, sizeof(trash), "</table><p>\n");

			if (buffer_write_chunk(rep, &msg) >= 0)
				return 0;
		}

		s->data_ctx.stats.px_st = DATA_ST_PX_FIN;
		/* fall through */

	case DATA_ST_PX_FIN:
		return 1;

	default:
		/* unknown state, we should put an abort() here ! */
		return 1;
	}
}

static struct cfg_kw_list cfg_kws = {{ },{
	{ CFG_GLOBAL, "stats", stats_parse_global },
	{ 0, NULL, NULL },
}};

__attribute__((constructor))
static void __dumpstats_module_init(void)
{
	cfg_register_keywords(&cfg_kws);
}

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
