static void client_sigwinch_handler(int sig) {
	client.need_resize = true;
}

static bool client_send_packet(Packet *pkt) {
	print_packet("client-send:", pkt);
	if (send_packet(server.socket, pkt))
		return true;
	debug("FAILED\n");
	server.running = false;
	return false;
}

static bool client_recv_packet(Packet *pkt) {
	if (recv_packet(server.socket, pkt)) {
		print_packet("client-recv:", pkt);
		return true;
	}
	debug("client-recv: FAILED\n");
	server.running = false;
	return false;
}

static void client_restore_terminal(void) {
	if (!has_term)
		return;
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_term);
	if (alternate_buffer) {
		printf("\033[?25h\033[?1049l");
		fflush(stdout);
		alternate_buffer = false;
	}
}

static void client_setup_terminal(void) {
	if (!has_term)
		return;
	atexit(client_restore_terminal);

	cur_term = orig_term;
	cur_term.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL|IXON|IXOFF);
	cur_term.c_oflag &= ~(OPOST);
	cur_term.c_lflag &= ~(ECHO|ECHONL|ICANON|ISIG|IEXTEN);
	cur_term.c_cflag &= ~(CSIZE|PARENB);
	cur_term.c_cflag |= CS8;
	cur_term.c_cc[VLNEXT] = _POSIX_VDISABLE;
	cur_term.c_cc[VMIN] = 1;
	cur_term.c_cc[VTIME] = 0;
	tcsetattr(STDIN_FILENO, TCSANOW, &cur_term);

	if (!alternate_buffer) {
		printf("\033[?1049h\033[H");
		fflush(stdout);
		alternate_buffer = true;
	}
}


/* Kitty keyboard protocol support.
 *
 * Modern terminals (kitty, wezterm, foot, ghostty) and applications (fish,
 * neovim, helix) increasingly use the kitty keyboard protocol, which encodes
 * key presses as CSI sequences instead of raw control bytes:
 *
 *   Ctrl-\  ->  legacy: 0x1C  ->  kitty protocol: ESC [ 92 ; <mod> u
 *
 * The modifier value encodes all active modifiers plus lock keys:
 *   modifier = 1 + (shift:1|alt:2|ctrl:4|super:8|...|capslock:64|numlock:128)
 * So Ctrl-\ with numlock on = ESC [ 92 ; 133 u.
 *
 * abduco's detach key detection only checks for the single-byte value, so
 * detaching fails whenever the keyboard protocol is active.  This adds a
 * parser that recognizes the CSI u encoding with ctrl set in the modifier.
 */

static unsigned int csi_detach_codepoint;

static void init_csi_detach(void) {
	/* Recover the base character from the control byte.
	 * CTRL(k) = k & 0x1F, so base = KEY_DETACH | 0x40 for @[\]^_
	 * and base = KEY_DETACH | 0x60 for letters (a-z). */
	if (KEY_DETACH >= 1 && KEY_DETACH <= 26)
		csi_detach_codepoint = KEY_DETACH + 0x60;
	else
		csi_detach_codepoint = KEY_DETACH + 0x40;
}

/* Parse a CSI u sequence: ESC [ <number> ; <number> [: <number>] u
 * Returns true if it encodes the detach key with ctrl modifier.
 * Accepts any additional modifiers (numlock, capslock, etc.). */
static bool is_csi_detach(const char *buf, ssize_t len) {
	if (len < 6 || buf[0] != '\033' || buf[1] != '[')
		return false;
	if (buf[len - 1] != 'u')
		return false;

	/* Parse codepoint */
	unsigned int codepoint = 0;
	int i = 2;
	while (i < len && buf[i] >= '0' && buf[i] <= '9')
		codepoint = codepoint * 10 + (buf[i++] - '0');
	if (codepoint != csi_detach_codepoint)
		return false;

	/* Expect semicolon */
	if (i >= len || buf[i] != ';')
		return false;
	i++;

	/* Parse modifier value */
	unsigned int mod = 0;
	while (i < len && buf[i] >= '0' && buf[i] <= '9')
		mod = mod * 10 + (buf[i++] - '0');

	/* Skip optional event type (:1=press, :2=repeat, :3=release) */
	if (i < len && buf[i] == ':') {
		i++;
		while (i < len && buf[i] >= '0' && buf[i] <= '9')
			i++;
	}

	/* Must end with 'u' */
	if (i != len - 1)
		return false;

	/* Check ctrl is set: modifier = 1 + bitmask, ctrl = bit 2 (value 4) */
	if (mod < 1)
		return false;
	return ((mod - 1) & 4) != 0;
}

static int client_mainloop(void) {
	sigset_t emptyset, blockset;
	sigemptyset(&emptyset);
	sigemptyset(&blockset);
	sigaddset(&blockset, SIGWINCH);
	sigprocmask(SIG_BLOCK, &blockset, NULL);

	init_csi_detach();

	client.need_resize = true;
	Packet pkt = {
		.type = MSG_ATTACH,
		.u.i = client.flags,
		.len = sizeof(pkt.u.i),
	};
	client_send_packet(&pkt);

	while (server.running) {
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(STDIN_FILENO, &fds);
		FD_SET(server.socket, &fds);

		if (client.need_resize) {
			struct winsize ws;
			if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != -1) {
				Packet pkt = {
					.type = MSG_RESIZE,
					.u = { .ws = { .rows = ws.ws_row, .cols = ws.ws_col } },
					.len = sizeof(pkt.u.ws),
				};
				if (client_send_packet(&pkt))
					client.need_resize = false;
			}
		}

		if (pselect(server.socket+1, &fds, NULL, NULL, NULL, &emptyset) == -1) {
			if (errno == EINTR)
				continue;
			die("client-mainloop");
		}

		if (FD_ISSET(server.socket, &fds)) {
			Packet pkt;
			if (client_recv_packet(&pkt)) {
				switch (pkt.type) {
				case MSG_CONTENT:
					if (!passthrough)
						write_all(STDOUT_FILENO, pkt.u.msg, pkt.len);
					break;
				case MSG_RESIZE:
					client.need_resize = true;
					break;
				case MSG_EXIT:
					client_send_packet(&pkt);
					close(server.socket);
					return pkt.u.i;
				}
			}
		}

		if (FD_ISSET(STDIN_FILENO, &fds)) {
			Packet pkt = { .type = MSG_CONTENT };
			ssize_t len = read(STDIN_FILENO, pkt.u.msg, sizeof(pkt.u.msg));
			if (len == -1 && errno != EAGAIN && errno != EINTR)
				die("client-stdin");
			if (len > 0) {
				debug("client-stdin: %c\n", pkt.u.msg[0]);
				pkt.len = len;
				if (KEY_REDRAW && pkt.u.msg[0] == KEY_REDRAW) {
					client.need_resize = true;
				} else if (pkt.u.msg[0] == KEY_DETACH || is_csi_detach(pkt.u.msg, len)) {
					pkt.type = MSG_DETACH;
					pkt.len = 0;
					client_send_packet(&pkt);
					close(server.socket);
					return -1;
				} else if (!(client.flags & CLIENT_READONLY)) {
					client_send_packet(&pkt);
				}
			} else if (len == 0) {
				debug("client-stdin: EOF\n");
				return -1;
			}
		}
	}

	return -EIO;
}
