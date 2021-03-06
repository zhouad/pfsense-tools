/*
        Copyright (C) 2010 Ermal Lu�i
        All rights reserved.

        Redistribution and use in source and binary forms, with or without
        modification, are permitted provided that the following conditions are met:

        1. Redistributions of source code must retain the above copyright notice,
           this list of conditions and the following disclaimer.

        2. Redistributions in binary form must reproduce the above copyright
           notice, this list of conditions and the following disclaimer in the
           documentation and/or other materials provided with the distribution.

        THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
        INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
        AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
        AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
        OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
        SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
        INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
        CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
        ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
        POSSIBILITY OF SUCH DAMAGE.

*/

#include <sys/types.h>
#include <sys/event.h>
#include <sys/sbuf.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/queue.h>

#include <ctype.h>

#include <stdio.h>
#include <errno.h>
#include <err.h>
#include <fcntl.h>
#include <stdlib.h>
#include <signal.h>
#include <syslog.h>
#include <unistd.h>
#include <strings.h>
#include <string.h>

#include <event.h>

#include "server.h"
#include "common.h"

#include <sys/utsname.h>

#include "fastcgi.h"

/* function definitions */
static void			handle_signal(int);
static void			run_command(struct command *, char *);
static void			set_blockmode(int socket, int cmd);
struct command *	match_command(struct command *target, char *wordpassed);
struct command *	parse_command(int fd, int argc, char **argv);
static void			socket_read_command(int socket, short event, void *arg);
static void			show_command_list(int fd, const struct command *list);
static void			socket_accept_command(int socket, short event, void *arg);
static void			socket_close_command(int fd, struct event *ev);
static void			socket_read_fcgi(int, short, void *);
static void			fcgi_send_command(int, short, void *);
static int			fcgi_open_socket(void);

/*
 * Internal representation of a packet.
 */
struct runq {
	TAILQ_ENTRY(runq) rq_link;
	struct event ev;
	char   command[2048];
	char   params[256];
	int requestId;
	int aggregate;
	int dontexec;
};
TAILQ_HEAD(runqueue, runq) cmds = TAILQ_HEAD_INITIALIZER(cmds);;

static pid_t ppid = -1;
static int child = 0;
static struct utsname uts;
static int fcgifd = -1;
static int keepalive = 1;
static char *fcgipath = (char *)FCGI_SOCK_PATH;
struct event fcgiev;

static int
prepare_packet(FCGI_Header *header, int type, int lcontent, int requestId)
{
        header->version = (unsigned char)FCGI_VERSION_1;
        header->type = (unsigned char)type;
        header->requestIdB1 = (unsigned char)((requestId >> 8) & 0xFF);
        header->requestIdB0 = (unsigned char)(requestId & 0xFF);
        header->contentLengthB1 = (unsigned char)((lcontent >> 8) & 0xFF);
        header->contentLengthB0 = (unsigned char)(lcontent & 0xFF);

        return (0);
}

static int
build_nvpair(struct sbuf *sb, int lkey, int lvalue, const char *key, char *svalue)
{
        if (lkey < 128)
                sbuf_putc(sb, lkey);
        else
                sbuf_printf(sb, "%c%c%c%c", (u_char)((lkey >> 24) | 0x80), (u_char)((lkey >> 16) & 0xFF), (u_char)((lkey >> 16) & 0xFF), (u_char)(lkey & 0xFF));

        if (lvalue < 128 || lvalue > 65535)
                sbuf_putc(sb, lvalue);
        else
                sbuf_printf(sb, "%c%c%c%c", (u_char)((lvalue >> 24) | 0x80), (u_char)((lvalue >> 16) & 0xFF), (u_char)((lvalue >> 16) & 0xFF), (u_char)(lvalue & 0xFF));

        if (lkey > 0)
                sbuf_printf(sb, "%s", key);
        if (lvalue > 0)
                sbuf_printf(sb, "%s", svalue);

        return (0);
}

static int
fcgi_open_socket()
{
        struct sockaddr_un sun;

	fcgifd = socket(PF_UNIX, SOCK_STREAM, 0);
	if (fcgifd < 0) {
		printf("Could not socket\n");
		return (-1);
	}

	bzero(&sun, sizeof(sun));
	sun.sun_family = PF_UNIX;
        strlcpy(sun.sun_path, fcgipath, sizeof(sun.sun_path));
        if (connect(fcgifd, (struct sockaddr *)&sun, sizeof(sun)) < 0) {
                printf("Could not bind\n");
                close(fcgifd);
                return (-1);
        }

        set_blockmode(fcgifd, O_NONBLOCK | FD_CLOEXEC);

	event_set(&fcgiev, fcgifd, EV_READ | EV_PERSIST, socket_read_fcgi, &fcgiev);
	event_add(&fcgiev, NULL);

	return (fcgifd);
}

static void
show_command_list(int fd, const struct command *list)
{
        int     i;
	char	value[2048];

	if (list == NULL)
		return;

        for (i = 0; list[i].action != NULLOPT; i++) {
                switch (list[i].type) {
                case NON:
			bzero(value, sizeof(value));
			snprintf(value, sizeof(value), "\t%s <cr>\n", list[i].keyword);
                        write(fd, value, strlen(value));
                        break;
                case COMPOUND:
			bzero(value, sizeof(value));
			snprintf(value, sizeof(value), "\t%s\n", list[i].keyword);
                        write(fd, value, strlen(value));
                        break;
                case ADDRESS:
			bzero(value, sizeof(value));
			snprintf(value, sizeof(value), "\t%s <address>\n", list[i].keyword);
                        write(fd, value, strlen(value));
                        break;
                case PREFIX:
			bzero(value, sizeof(value));
			snprintf(value, sizeof(value), "\t%s <address>[/len]\n", list[i].keyword);
                        write(fd, value, strlen(value));
                        break;
		case INTEGER:
			bzero(value, sizeof(value));
			snprintf(value, sizeof(value), "\t%s <number>\n", list[i].keyword);
                        write(fd, value, strlen(value));
			break;
                case IFNAME:
			bzero(value, sizeof(value));
			snprintf(value, sizeof(value), "\t%s <interface>\n", list[i].keyword);
                        write(fd, value, strlen(value));
                        break;
                case STRING:
			bzero(value, sizeof(value));
			snprintf(value, sizeof(value), "\t%s <string>\n", list[i].keyword);
                        write(fd, value, strlen(value));
                        break;
                }
        }
}

struct command *
parse_command(int fd, int argc, char **argv)
{
	struct command	*start = first_level;
	struct command	*match = NULL;
	char *errstring = (char *)"ERROR:\tvalid commands are:\n";

	while (argc >= 0) {
		match = match_command(start, *argv);
		if (match == NULL) {
			errstring = (char *)"ERROR:\tNo match found.\n";
			goto error3;
		}

		argc--;
		argv++;

		if (argc > 0 && match->next == NULL) {
			errstring = (char *)"ERROR:\textra arguments passed.\n";
			goto error3;
		}
		if (argc < 0 && match->type != NON) {
			if (match->next != NULL)
				start = match->next;
			errstring = (char *)"ERROR:\tincomplete command.\n";
			goto error3;
		}
		if (argc == 0 && *argv == NULL && match->type != NON) {
			if (match->next != NULL)
				start = match->next;
			errstring = (char *)"ERROR:\tincomplete command.\n";
			goto error3;
		}

		if ( match->next == NULL)
			break;

		start = match->next;	
	}

	return (match);
error3:
	write(fd, errstring, strlen(errstring));
	show_command_list(fd, start);
	return (NULL);
}

struct command *
match_command(struct command *target, char *wordpassed)
{
	int i;

	if (wordpassed == NULL)
		return NULL;

	for (i = 0; target[i].action != NULLOPT; i++) {
		if (strcmp(target[i].keyword, wordpassed) == 0)
			return &target[i];
	}

	return (NULL);
}

static void
handle_signal(int sig)
{
        switch(sig) {
        case SIGHUP:
        case SIGTERM:
		if (child)
			exit(0);
                break;
        }
}

static void
fcgi_send_command(int fd __unused , short event __unused, void *arg)
{
	FCGI_BeginRequestRecord *bHeader;
        FCGI_Header *tmpl;
        struct sbuf sb;
	struct runq *cmd;
	struct timeval tv = { 8, 0 };
	static int requestId = 0;
	int len, result;
	char *p, sbuf[2048], buf[2048];

	cmd = arg;
	if (cmd == NULL)
		return;

	if (cmd->dontexec) {
		TAILQ_REMOVE(&cmds, cmd, rq_link);
		child = 0;
		timeout_del(&cmd->ev);
		free(cmd);
		return;
	}

	requestId++;
	cmd->requestId = requestId;

	memset(sbuf, 0, sizeof(sbuf));
	sbuf_new(&sb, sbuf, 2048, 0);
	/* TODO: Use hardcoded length instead of strlen allover later on */
	/* TODO: Remove some env variables since might not be needed at all!!! */
	build_nvpair(&sb, strlen("GATEWAY_INTERFACE"), strlen("FastCGI/1.0"), "GATEWAY_INTERFACE", (char *)"FastCGI/1.0");
	build_nvpair(&sb, strlen("REQUEST_METHOD"), strlen("GET"), "REQUEST_METHOD", (char *)"GET");
	build_nvpair(&sb, strlen("SCRIPT_FILENAME"), strlen(cmd->command), "SCRIPT_FILENAME", cmd->command);
	build_nvpair(&sb, strlen("NO_HEADERS"), strlen("1"), "NO_HEADERS", (char *)"1");
	p = strrchr(cmd->command, '/');
	build_nvpair(&sb, strlen("SCRIPT_NAME"), strlen(p), "SCRIPT_NAME", p);
	p++;
	build_nvpair(&sb, strlen("DOCUMENT_URI"), strlen(p), "DOCUMENT_URI", p);
	build_nvpair(&sb, strlen("QUERY_STRING"), strlen(cmd->params), "QUERY_STRING", cmd->params);
	if (!cmd->params[0]) {
		build_nvpair(&sb, strlen("REQUEST_URI"), 0, "REQUEST_URI", p);
	} else {
		/* XXX: Hack in sight to avoid using another sbuf */
		build_nvpair(&sb, strlen("REQUEST_URI"), 1 + strlen(p) + strlen(cmd->params) + 1, "REQUEST_URI", (char *)"/");
		sbuf_printf(&sb, "%s?%s", p, cmd->params);
	}
	sbuf_finish(&sb);

	len = (3 * sizeof(FCGI_Header)) + sizeof(FCGI_BeginRequestRecord) + sbuf_len(&sb);
#if 0
	if (len > 2048) {
		buf = calloc(1, len);
		if (buf == NULL) {
			tv.tv_sec = 1;
			timeout_add(&cmd->ev, &tv);
			sbuf_delete(sbtmp2);
			return;
		}
	} else
#endif
		memset(buf, 0, sizeof(buf));

	bHeader = (FCGI_BeginRequestRecord *)buf;
	prepare_packet(&bHeader->header, FCGI_BEGIN_REQUEST, sizeof(bHeader->body), requestId);
	bHeader->body.roleB0 = (unsigned char)FCGI_RESPONDER;
	bHeader->body.flags = (unsigned char)(keepalive ? FCGI_KEEP_CONN : 0);
	bHeader++;
	tmpl = (FCGI_Header *)bHeader;
	prepare_packet(tmpl, FCGI_PARAMS, sbuf_len(&sb), requestId);
	tmpl++;
	memcpy((char *)tmpl, sbuf_data(&sb), sbuf_len(&sb));
	tmpl = (FCGI_Header *)(((char *)tmpl) + sbuf_len(&sb));
        prepare_packet(tmpl, FCGI_PARAMS, 0, requestId);
        tmpl++;
        prepare_packet(tmpl, FCGI_STDIN, 0, requestId);
	if (fcgifd < 0) {
		syslog(LOG_ERR, "Reopening fcgi socket");
		if (fcgi_open_socket() < 0) {
			/* Reschedule */
			tv.tv_sec = 1;
			timeout_add(&cmd->ev, &tv);
			return;
		}
	}

	result = write(fcgifd, buf, len);
	if (result < 0) {
		syslog(LOG_ERR, "Something wrong happened while sending request: %m\n");
		timeout_add(&cmd->ev, &tv);
	} else if (cmd->aggregate > 0) {
		cmd->dontexec = 1;
		timeout_add(&cmd->ev, &tv);
	} else {
		TAILQ_REMOVE(&cmds, cmd, rq_link);
		timeout_del(&cmd->ev);
		free(cmd);
	}
}

static void
run_command_detailed(int fd __unused, short event __unused, void *arg) {
	struct runq *cmd;
	struct timeval tv = { 8, 0 };

	cmd = (struct runq *)arg;

	if (cmd == NULL)
		return;

	if (cmd->dontexec) {
		TAILQ_REMOVE(&cmds, cmd, rq_link);
		child = 0;
		timeout_del(&cmd->ev);
		free(cmd);
		return;
	}


	switch (vfork()) {
	case -1:
		syslog(LOG_ERR, "Could not vfork() error %d - %s!!!", errno, strerror(errno));
		break;
	case 0:
		child = 1;
		/* Possibly optimize by creating argument list and calling execve. */
		if (cmd->params[0])
			execl("/bin/sh", "/bin/sh", "-c", cmd->command, cmd->params, (char *)NULL);
		else
			execl("/bin/sh", "/bin/sh", "-c", cmd->command, (char *)NULL);
		syslog(LOG_ERR, "could not run: %s", cmd->command);
		_exit(127); /* Protect in case execl errors out */
		break;
	default:
		if (cmd->aggregate > 0) {
			cmd->dontexec = 1;
			timeout_add(&cmd->ev, &tv);
		} else {
			TAILQ_REMOVE(&cmds, cmd, rq_link);
			timeout_del(&cmd->ev);
			free(cmd);
		}
		break;
	}
}

static void
run_command(struct command *cmd, char *argv) {
	struct runq *command, *tmpcmd;
	struct timeval tv = { 1, 0 };
	int aggregate = 0;

	TAILQ_FOREACH(tmpcmd, &cmds, rq_link) {
		if (cmd->cmd.flags & AGGREGATE && !strcmp(tmpcmd->command, cmd->cmd.command)) {
			aggregate += tmpcmd->aggregate;
			if (aggregate > 1) {
				/* Rexec the command so the event is not lost. */
				if (tmpcmd->dontexec && aggregate < 3) {
					//syslog(LOG_ERR, "Rescheduling command %s", tmpcmd->command);
					syslog(LOG_NOTICE, cmd->cmd.syslog, argv);
					tmpcmd->dontexec = 0;
					tv.tv_sec = 5;
					timeout_del(&tmpcmd->ev);
					timeout_add(&tmpcmd->ev, &tv);
				}
				return;
			}
		}
	}

	command = calloc(1, sizeof(*command));
	if (command == NULL) {
		syslog(LOG_ERR, "Calloc failure for command %s", argv);
		return;
	}

	command->aggregate = aggregate + 1;
	memcpy(command->command, cmd->cmd.command, sizeof(command->command));
	if (cmd->cmd.params)
		snprintf(command->params, sizeof(command->params), cmd->cmd.params, argv);

	if (!(cmd->cmd.flags & AGGREGATE))
		command->aggregate = 0;

	TAILQ_INSERT_HEAD(&cmds, command, rq_link);

	switch (cmd->type) {
	case NON:
		syslog(LOG_NOTICE, "%s", cmd->cmd.syslog);
		break;
	case COMPOUND: /* XXX: Should never happen. */
		syslog(LOG_ERR, "trying to execute COMPOUND entry!!! Please report it.");
		return;
		/* NOTREACHED */
		break;
	case ADDRESS:
	case PREFIX:
	case INTEGER:
	case IFNAME:
	case STRING:
		syslog(LOG_NOTICE, cmd->cmd.syslog, argv);
		break;
	}

	if (cmd->cmd.flags & FCGICMD)
		timeout_set(&command->ev, fcgi_send_command, command);
	else
		timeout_set(&command->ev, run_command_detailed, command);
	timeout_add(&command->ev, &tv);

	return;
}

static void
socket_close_command(int fd, struct event *ev)
{
	event_del(ev);
	free(ev);
        close(fd);
}

static void
socket_read_fcgi(int fd, short event, void *arg __unused)
{
	struct runq *tmpcmd = NULL;
	FCGI_Header header;
	char buf[2048];
        int len, terr, success = 0;

	if (event == EV_TIMEOUT) {
		close(fd);
		fcgi_open_socket();
		return;
	}

	len = 0;
	memset(&header, 0, sizeof(header));
	if (recv(fd, &header, sizeof(header), 0) > 0) {
		len = (header.requestIdB1 << 8) | header.requestIdB0;
		TAILQ_FOREACH(tmpcmd, &cmds, rq_link) {
			if (tmpcmd->requestId == len)
				break;
		}
		len = (header.contentLengthB1 << 8) | header.contentLengthB0;
		len += header.paddingLength;
		
		//syslog(LOG_ERR, "LEN: %d, %d, %d\n", len, header.type, (header.requestIdB1 << 8) | header.requestIdB0);
		if (len > 0) {
			memset(buf, 0, sizeof(buf));

			/* XXX: Should check if len > sizeof(buf)? */
			terr = recv(fd, buf, len, 0);
			if (terr < 0) {
				//syslog(LOG_ERR, "Something happened during recv of data");
				return;
			}
		}
	}

	switch (header.type) {
	case FCGI_DATA:
	case FCGI_STDOUT:
	case FCGI_STDERR:
		break;
	case FCGI_ABORT_REQUEST:
		syslog(LOG_ERR, "Request aborted\n");
		break;
	case FCGI_END_REQUEST:
		if (len >= (int)sizeof(FCGI_EndRequestBody)) {
			switch (((FCGI_EndRequestBody *)buf)->protocolStatus) {
			case FCGI_CANT_MPX_CONN:
				syslog(LOG_ERR, "The FCGI server cannot multiplex\n");
				success = 0;
				break;
			case FCGI_OVERLOADED:
				syslog(LOG_ERR, "The FCGI server is overloaded\n");
				success = 0;
				break;
			case FCGI_UNKNOWN_ROLE:
				syslog(LOG_ERR, "FCGI role is unknown\n");
				success = 0;
				break;
			case FCGI_REQUEST_COMPLETE:
				//syslog(LOG_ERR, "FCGI request completed");
				success = 1;
				break;
			}
			if (tmpcmd != NULL)  {
				if (success) {
					TAILQ_REMOVE(&cmds, tmpcmd, rq_link);
					timeout_del(&tmpcmd->ev);
					free(tmpcmd);
				} else {
				       /* Rexec the command so the event is not lost. */
					syslog(LOG_ERR, "Repeating event %s/%s because it was not triggered.", tmpcmd->command, tmpcmd->params);
					if (tmpcmd->dontexec)
						tmpcmd->dontexec = 0;
				}
			}
		}
		break;
	}
}

static void
socket_read_command(int fd, short event, void *arg)
{
	struct command *cmd;
	struct event *ev = arg;
	enum { bufsize = 2048 };
	char buf[bufsize];
	register int n;
	char **ap, *argv[bufsize], *p;
	int i, loop = 0;

	if (event == EV_TIMEOUT)
		socket_close_command(fd, ev);
		
tryagain:
	bzero(buf, sizeof(buf));
	if ((n = read (fd, buf, bufsize)) == -1) {
		if (errno != EWOULDBLOCK && errno != EINTR) {
			return;
		} else {
			if (loop > 3) {
				return;
			}
			loop++;
			goto tryagain;
		}
	} else if (n == 0) {
		return;
	}
	
	if (buf[n - 1] == '\n')
		buf[n - 1] = '\0'; /* remove stray \n */
	if (n > 1 && buf[n - 2] == '\r') {
		n--;
		buf[n - 1] = '\0';
	}
	for (i = 0; i < n - 1; i++) {
		if (!isalpha(buf[i]) && !isspace(buf[i]) && !isdigit(buf[i]) && !ispunct(buf[i])) {
			write(fd, "ERROR:\tonly alphanumeric chars allowd", 37);
			socket_close_command(fd, ev);
			return;
		}
	}
	p = buf; /* blah, compiler workaround */

	i = 0;
	for (ap = argv; (*ap = strsep(&p, " \t")) != NULL;) {
		if (**ap != '\0') {
			if (++ap >= &argv[bufsize])
				break;
		}
		i++;
	}
	if (i > 0) {
		p = argv[i - 1];
		i = i - 1;
	} else {
		p = argv[i];
	}
	cmd = parse_command(fd, i, argv);
	if (cmd != NULL) {
		write(fd, "OK\n", 3);
		run_command(cmd, p);
	}

	return;
}

static void
socket_accept_command(int fd, __unused short event, __unused void *arg)
{
	struct sockaddr_un sun;
	struct timeval tv = { 10, 0 };
	struct event *ev;
	socklen_t len;
	int newfd;

	if ((newfd = accept(fd, (struct sockaddr *)&sun, &len)) < 0) {
		if (errno != EWOULDBLOCK && errno != EINTR)
			syslog(LOG_ERR, "problems on accept");
		return;
	}
	set_blockmode(newfd, O_NONBLOCK | FD_CLOEXEC);

	if ((ev = malloc(sizeof(*ev))) == NULL) {
		syslog(LOG_ERR, "Cannot allocate new struct event.");
		close(newfd);
		return;
	}

	event_set(ev, newfd, EV_READ | EV_PERSIST, socket_read_command, ev);
	event_add(ev, &tv);
}

static void
set_blockmode(int fd, int cmd)
{
        int     flags;

        if ((flags = fcntl(fd, F_GETFL, 0)) == -1)
                errx(errno, "fcntl F_GETFL");

	flags |= cmd;

        if ((flags = fcntl(fd, F_SETFL, flags)) == -1)
                errx(errno, "fcntl F_SETFL");
}

int
main(void)
{
	struct event ev;
	struct sockaddr_un sun;
	struct sigaction sa;
	mode_t *mset, mode;
	sigset_t set;
	int fd, errcode = 0;
	char *p;

	tzset();

	/* daemonize */
	if (daemon(0, 0) < 0) {
		syslog(LOG_ERR, "check_reload_status could not start.");
		errcode = 1;
		goto error;
	}

	syslog(LOG_NOTICE, "check_reload_status is starting.");

	uname(&uts);

	if ((p = getenv("fcgipath")) != NULL) {
		fcgipath = p;
		syslog(LOG_ERR, "fcgipath %s", fcgipath);
	}

	sigemptyset(&set);
	sigfillset(&set);
	sigdelset(&set, SIGHUP);
	sigdelset(&set, SIGTERM);
	sigdelset(&set, SIGCHLD);
	sigprocmask(SIG_BLOCK, &set, NULL);
	signal(SIGCHLD, SIG_IGN);

	sa.sa_handler = handle_signal;
        sa.sa_flags = SA_SIGINFO|SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	ppid = getpid();
	if (fork() == 0) {
		setproctitle("Monitoring daemon of check_reload_status");
		/* Prepare code to monitor the parent :) */
		struct kevent kev;
		int kq;

		while (1) {
			kq = kqueue();
			EV_SET(&kev, ppid, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, NULL);
			kevent(kq, &kev, 1, NULL, 0, NULL);
			switch (kevent(kq, NULL, 0, &kev, 1, NULL)) {
			case 1:
				syslog(LOG_ERR, "Reloading check_reload_status because it exited from an error!");
				execl("/usr/local/sbin/check_reload_status", "/usr/local/sbin/check_reload_status", (char *)NULL);
				_exit(127);
				syslog(LOG_ERR, "could not run check_reload_status again");
				/* NOTREACHED */
				break;
			default:
				/* XXX: Should report any event?! */
				break;
			}
			close(kq);
		}
		exit(2);
	}

	fd = socket(PF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		errcode = -1;
		printf("Could not socket\n");
		goto error;
	}

#if 0
	if (unlink(PATH) == -1) {
		errcode = -2;
		printf("Could not unlink\n");
		close(fd);
		goto error;
	}
#else
	unlink(PATH);
#endif

	bzero(&sun, sizeof(sun));
        sun.sun_family = PF_UNIX;
        strlcpy(sun.sun_path, PATH, sizeof(sun.sun_path));
	if (bind(fd, (struct sockaddr *)&sun, sizeof(sun)) < 0) {
		errcode = -2;
		printf("Could not bind\n");
		close(fd);
		goto error;
	}

	set_blockmode(fd, O_NONBLOCK | FD_CLOEXEC);

        if (listen(fd, 30) == -1) {
                printf("control_listen: listen");
		close(fd);
                return (-1);
        }

	/* 0666 */
	if ((mset = setmode("0666")) != NULL) {
		mode = getmode(mset, S_IRUSR|S_IWUSR | S_IRGRP|S_IWGRP | S_IROTH|S_IWOTH);
		chmod(PATH, mode);
		free(mset);
	}

	TAILQ_INIT(&cmds);

	event_init();
	event_set(&ev, fd, EV_READ | EV_PERSIST, socket_accept_command, &ev);
	event_add(&ev, NULL);
	event_dispatch();

	return (0);
error:
	syslog(LOG_NOTICE, "check_reload_status is stopping.");

	return (errcode);
}
