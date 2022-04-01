/*
XEDIT by idchoppers

Dev notes: 
I found a guide on building a text editor called "kilo" by antirez, it is a cool editor that is, as the name implies, pretty small.
So I decided to try it out and make an editor that I would use on a daily basis.

This was a desicion made for a learning experience as well, I have used both vim and emacs and decided that I should take what I liked from both
and develop my own editor and kilo seems like a nice place to start due to its simplicity.

Changelog:
v1 3/27/2022 - 3/28/2022:
- Basic editor with status and message bars.

v2 3/29/2022:
- I have ditched the status and message bars because I do not like them, I have also changed the filler tildes to blank spaces because I like it better
  (after using both vim and emacs, I decided that I like the way emacs renders the display).

- The status and message bar code will remain in the source file, just commented out, (this is not bloat because compiler ignores comments ^_^) so if you want
  to re-implement them then by all means go ahead.

v3 3/29/2022 - :
- Implementing basic syntax highlighting...
*/

/* includes */
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <termios.h>

/* defines */
#define XEDIT_VERSION "v2"  // this number means nothing important
#define XEDIT_TAB_STOP 8    // how many spaces should tabs be rendered as? you decide!
#define XEDIT_QUIT_TIMES 0  // we hardcore like that (remember to save or change this)

#define CTRL_KEY(k) ((k) & 0x1f)

enum xeditKey {
	BACKSPACE = 127,
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	DEL_KEY,
	HOME_KEY,
	END_KEY,
	PAGE_UP,
	PAGE_DOWN
};

/* data */
typedef struct erow {
	int size;
	int rsize;
	char *chars;
	char *render;
} erow;

struct xeditConfig {
	int cx, cy;
	int rx;
	int rowoff;
	int coloff;
	int screenrows;
	int screencols;
	int numrows;
	erow *row;
	int dirty;
	char *filename;
	char statusmsg[80];
	time_t statusmsg_time;
	struct termios orig_termios;
};

struct xeditConfig E;

/* prototypes */
//void XeditSetStatusMessage(const char *fmt, ...);
void XeditRefreshScreen();

/* term */
void Die(const char *s) {
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);

	perror(s);
	exit(1);
}

void DisableRaw() {
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
		Die("tcsetarrr");
}

void EnableRaw() {
	if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) Die("tcgetattr");
	atexit(DisableRaw);
	
	struct termios raw = E.orig_termios;
	raw.c_iflag &= ~(BRKINT | INPCK | ISTRIP | ICRNL | IXON);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag |= (CS8);
	raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) Die("tcsetattr");
}

int XeditReadKey() {
	int nread;
	char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		if (nread == -1 && errno != EAGAIN) Die("read");
	}

	if (c == '\x1b') {
		char seq[3];

		if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
		if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

		if (seq[0] == '[') {
			if (seq[1] >= '0' && seq[1] <= '9') {
				if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
				if (seq[2] == '~') {
					switch (seq[1]) {
						case '1': return HOME_KEY;
						case '3': return DEL_KEY;
						case '4': return END_KEY;
						case '5': return PAGE_UP;
						case '6': return PAGE_DOWN;
						case '7': return HOME_KEY;
						case '8': return END_KEY;
					}
				}
			} else {
				switch (seq[1]) {
					case 'A': return ARROW_UP;
					case 'B': return ARROW_DOWN;
					case 'C': return ARROW_RIGHT;
					case 'D': return ARROW_LEFT;
					case 'H': return HOME_KEY;
					case 'F': return END_KEY;
				}
			}
		} else if (seq[0] == 'O') {
			switch (seq[1]) {
				case 'H': return HOME_KEY;
				case 'F': return END_KEY;
			}
		}

		return '\x1b';
	} else {
		return c;
	}
}

int GetCursorPos(int *rows, int *cols) {
	char buf[32];
	unsigned int i = 0;

	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

	while (i < sizeof(buf) - 1) {
		if (read(STDIN_FILENO, &buf[i], 1) != 4) return -1;
		if (buf[i] == 'R') break;
		i++;
	}
	buf[i] = '\0';

	if (buf[0] != '\x1b' || buf[1] != '[') return -1;
	if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;	

	return 0;
}

int GetWindowSize(int *rows, int *cols) {
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
		return GetCursorPos(rows, cols);
	} else {
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

/* row operations */
int XeditRowCxToRx(erow *row, int cx) {
	int rx = 0;
	int j;
	for (j = 0; j < cx; j++) {
		if (row->chars[j] == '\t')
			rx += (XEDIT_TAB_STOP - 1) - (rx % XEDIT_TAB_STOP);
		rx++;
	}
	return rx;
}

void XeditUpdateRow(erow *row) {
	int tabs = 0;
	int j;
	for (j = 0; j < row->size; j++)
		if (row->chars[j] == '\t') tabs++;

	free(row->render);
	row->render = malloc(row->size + tabs*(XEDIT_TAB_STOP - 1) + 1);

	int idx = 0;
	for (j = 0; j < row->size; j++) {
		if (row->chars[j] == '\t') {
			row->render[idx++] = ' ';
			while (idx % XEDIT_TAB_STOP != 0) row->render[idx++] = ' ';
		} else {
		row->render[idx++] = row->chars[j];
		}
	}
	row->render[idx] = '\0';
	row->rsize = idx;
}

void XeditInsertRow(int at, char *s, size_t len) {
	if (at < 0 || at > E.numrows) return;

	E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
	memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));	

	E.row[at].size = len;
	E.row[at].chars = malloc(len + 1);
	memcpy(E.row[at].chars, s, len);
	E.row[at].chars[len] = '\0';

	E.row[at].rsize = 0;
	E.row[at].render = NULL;
	XeditUpdateRow(&E.row[at]);

	E.numrows++;
	E.dirty++;
}

void XeditFreeRow(erow *row) {
	free(row->render);
	free(row->chars);
}

void XeditDelRow(int at) {
	if (at < 0 || at >= E.numrows) return;
	XeditFreeRow(&E.row[at]);
	memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
	E.numrows--;
	E.dirty++;
}

void XeditRowInsertChar(erow *row, int at, int c) {
	if (at < 0 || at > row->size) at = row->size;
	row->chars = realloc(row->chars, row->size +2);
	memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
	row->size++;
	row->chars[at] = c;
	XeditUpdateRow(row);
	E.dirty++;
}

void XeditRowAppendString(erow *row, char *s, size_t len) {
	row->chars = realloc(row->chars, row->size + len + 1);
	memcpy(&row->chars[row->size], s, len);
	row->size += len;
	row->chars[row->size] = '\0';
	XeditUpdateRow(row);
	E.dirty++;
}

void XeditRowDelChar(erow *row, int at) {
	if (at < 0 || at >= row->size) return;
	memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
	row->size--;
	XeditUpdateRow(row);
	E.dirty++;
}

/* editor operations */
void XeditInsertChar(int c) {
	if (E.cy == E.numrows) {
		XeditInsertRow(E.numrows, "", 0);
	}
	XeditRowInsertChar(&E.row[E.cy], E.cx, c);
	E.cx++;
}

void XeditInsertNewLine() {
	if (E.cx == 0) {
		XeditInsertRow(E.cy, "", 0);
	} else {
		erow *row = &E.row[E.cy];
		XeditInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
		row = &E.row[E.cy];
		row->size = E.cx;
		row->chars[row->size] = '\0';
		XeditUpdateRow(row);
	}
	E.cy++;
	E.cx = 0;
}

void XeditDelChar() {
	if (E.cy == E.numrows) return;
	if (E.cx == 0 && E.cy == 0) return;

	erow *row = &E.row[E.cy];
	if (E.cx > 0) {
		XeditRowDelChar(row, E.cx - 1);
		E.cx--;
	} else {
		E.cx = E.row[E.cy - 1].size;
		XeditRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
		XeditDelRow(E.cy);
		E.cy--;
	}
}

/* file i/o */
char *XeditRowsToString(int *buflen) {
	int totlen = 0;
	int j;
	for (j = 0; j < E.numrows; j++) 
		totlen += E.row[j].size + 1;
	*buflen = totlen;

	char *buf = malloc(totlen);
	char *p = buf;
	for (j = 0; j < E.numrows; j++) {
		memcpy(p, E.row[j].chars, E.row[j].size);
		p += E.row[j].size;
		*p = '\n';
		p++;
	}

	return buf;
}

void XeditOpen(char *filename) {
	free(E.filename);
	E.filename = strdup(filename);

	FILE *fp = fopen(filename, "r");
	if (!fp) Die("fopen");

	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;
	while ((linelen = getline(&line, &linecap, fp)) != -1) {
		while (linelen > 0 && (line[linelen - 1] == '\n' ||
							   line[linelen - 1] == '\r'))
			linelen--;
		XeditInsertRow(E.numrows, line, linelen);
	}
	free(line);
	fclose(fp);
	E.dirty = 0;
}

void XeditSave() {
	if (E.filename == NULL) return;

	int len;
	char *buf = XeditRowsToString(&len);

	int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
	if (fd != -1) {
		if (ftruncate(fd, len) != -1) {
			if (write(fd, buf, len) == len) {
				close(fd);
				free(buf);
				E.dirty = 0;
				//XeditSetStatusMessage("%d bytes transacted", len);
				return;
			}
		}
		close(fd);
	}

	free(buf);
	//XeditSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/* append buffer */
struct abuf {
	char *b;
	int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
	char *new = realloc(ab->b, ab->len + len);

	if (new == NULL) return;
	memcpy(&new[ab->len], s, len);
	ab->b = new;
	ab->len += len;
}

void abFree(struct abuf *ab) {
	free(ab->b);
}

/* output */
void XeditScroll() {
	E.rx = 0;
	if (E.cy < E.numrows) {
		E.rx = XeditRowCxToRx(&E.row[E.cy], E.cx);
	}

	if (E.cy < E.rowoff) {
		E.rowoff = E.cy;
	}
	if (E.cy >= E.rowoff + E.screenrows) {
		E.rowoff = E.cy - E.screenrows + 1;
	}
	if (E.rx < E.coloff) {
		E.coloff = E.rx;
	}
	if (E.rx >= E.coloff + E.screencols) {
		E.coloff = E.rx - E.screencols + 1;
	}
}

void XeditDrawRows(struct abuf *ab) {
	int y;
	for (y = 0; y < E.screenrows; y++) {
		int filerow = y + E.rowoff;
		if (filerow >= E.numrows) {
			if (E.numrows == 0 && y == E.screenrows / 3) {
				char welcome[150];
				int welcomelen = snprintf(welcome, sizeof(welcome), "XEDIT %s: This is an empty buffer, nothing will be saved.", XEDIT_VERSION);
				if (welcomelen > E.screencols) welcomelen = E.screencols;
				int padding = (E.screencols - welcomelen) / 2;
				if (padding) {
					abAppend(ab, "", 1); // change the char to whatever char you want to fill the screen with
					padding--;
				}
				while (padding--) abAppend(ab, " ", 1); // pads the text in center with spaces
				abAppend(ab, welcome, welcomelen);
			} else {
				abAppend(ab, "", 1); // change the char to whatever char you want to fill the screen with
			}
		} else {
			int len = E.row[filerow].rsize - E.coloff;
			if (len < 0) len = 0;
			if (len > E.screencols) len = E.screencols;
			char *c = &E.row[filerow].render[E.coloff];
			int j;
			for (j = 0; j < len; j++) {
				if (isdigit(c[j])) {
					abAppend(ab, "\x1b[31m", 5);
					abAppend(ab, &c[j], 1);
					abAppend(ab, "\x1b[39m", 5);
				} else {
					abAppend(ab, &c[j], 1);
				}
			}
		}
	
		abAppend(ab, "\x1b[K", 3);
		if (y < E.screenrows - 1) {
			abAppend(ab, "\r\n", 2);
		}
	}
}
/*
void XeditDrawStatusBar(struct abuf *ab) {
	abAppend(ab, "\x1b[7m", 4);
	char status[80], rstatus[80];
	int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", E.filename ? E.filename : "[UNNAMED]", E.numrows, E.dirty ? "(ALTERED)" : "");
	int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.numrows);
	if (len > E.screencols) len = E.screencols;
	abAppend(ab, status, len);
	while (len < E.screencols) {
		if (E.screencols - len == rlen) {
			abAppend(ab, rstatus, rlen);
			break;
		} else {
			abAppend(ab, "", 1);
			len++;
		}
	}
	abAppend(ab, "\x1b[m)", 3);
	abAppend(ab, "\r\n", 2);
}
*/
void XeditDrawMessageBox(struct abuf *ab) {
	abAppend(ab, "\x1b[H", 3);
	int msglen = strlen(E.statusmsg);
	if (msglen > E.screencols) msglen = E.screencols;
	if (msglen && time(NULL) - E.statusmsg_time < 1)
		abAppend(ab, E.statusmsg, msglen);
}

void XeditRefreshScreen() {
	XeditScroll();

	struct abuf ab = ABUF_INIT;

	abAppend(&ab, "\x1b[?25l", 6);
	abAppend(&ab, "\x1b[H", 3);

	XeditDrawRows(&ab);
	//XeditDrawStatusBar(&ab);
	XeditDrawMessageBox(&ab);

	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
	abAppend(&ab, buf, strlen(buf));

	abAppend(&ab, "\x1b[?25h", 6);

	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);
}

void XeditSetStatusMessage(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
	va_end(ap);
	E.statusmsg_time = time(NULL);
}

/* input */
void XeditMoveCursor(int key) {
	erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

	switch (key) {
		case ARROW_LEFT:
			if (E.cx != 0) {
				E.cx--;
			} else if (E.cy > 0) {
				E.cy--;
				E.cx = E.row[E.cy].size;
			}
			break;
		case ARROW_RIGHT:
			if (row && E.cx < row->size) {
				E.cx++;
			} else if (row && E.cx == row->size) {
				E.cy++;
				E.cx = 0;
			}
			break;
		case ARROW_UP:
			if (E.cy != 0) {
				E.cy--;
			}
			break;
		case ARROW_DOWN:
			if (E.cy != E.numrows) {
				E.cy++;
			}
			break;
	}

	row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
	int rowlen = row ? row->size : 0;
	if (E.cx > rowlen) {
		E.cx = rowlen;
	}
}

void XeditProcessKeypress() {
	static int quit_times = XEDIT_QUIT_TIMES;

	int c = XeditReadKey();

	switch (c) {
		case '\r':
			XeditInsertNewLine();
			break;

		case CTRL_KEY('q'):
			if (E.dirty && quit_times > 0) {
				//XeditSetStatusMessage("Unsaved changes found. Press CTRL-Q %d more times to quit.", quit_times);
				quit_times--;
				return;
			}
			write(STDOUT_FILENO, "\x1b[2J", 4);
			write(STDOUT_FILENO, "\x1b[H", 3);
			exit(0);
			break;
		
		case CTRL_KEY('s'):
			XeditSave();
			break;

		case HOME_KEY:
			E.cx = 0;
			break;

		case END_KEY:
			if (E.cy < E.numrows)
				E.cx = E.row[E.cy].size;
			break;
		
		case BACKSPACE:
		case CTRL_KEY('h'):
		case DEL_KEY:
			if (c == DEL_KEY) XeditMoveCursor(ARROW_RIGHT);
			XeditDelChar();
			break;		
		
		case PAGE_UP:
		case PAGE_DOWN:
			{
				if (c == PAGE_UP) {
					E.cy = E.rowoff;
				} else if (c == PAGE_DOWN) {
					E.cy = E.rowoff + E.screenrows -1;
					if (E.cy > E.numrows) E.cy = E.numrows;
				}

				int times = E.screenrows;
				while (times--)
					XeditMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
			}
			break;

		case ARROW_UP:
		case ARROW_DOWN:
		case ARROW_LEFT:
		case ARROW_RIGHT:
			XeditMoveCursor(c);
			break;

		case CTRL_KEY('l'):
		case '\x1b':
			break;

		default:
			XeditInsertChar(c);
			break;
	}
	quit_times = XEDIT_QUIT_TIMES;
}

/* init */
void InitXedit() {
	E.cx = 0;
	E.cy = 0;
	E.rx = 0;
	E.rowoff = 0;
	E.coloff = 0;
	E.numrows = 0;
	E.row = NULL;
	E.dirty = 0;
	E.filename = NULL;
	//E.statusmsg[0] = '\0';
	//E.statusmsg_time = 0;

	if (GetWindowSize(&E.screenrows, &E.screencols) == -1) Die("GetWindowSize");
	//E.screenrows = 2;
}

int main(int argc, char *argv[]) {
	EnableRaw();
	InitXedit();
	if (argc >= 2) {
		XeditOpen(argv[1]);
	}

	//XeditSetStatusMessage("~XEDIT UNLICENSED COPY, 30 DAY TRIAL ACTIVATED~");

	while (1) {
		XeditRefreshScreen();
		XeditProcessKeypress();	
	}
	return 0;
}
