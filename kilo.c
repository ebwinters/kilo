/*** includes ***/
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/
#define CTRL_KEY(k) ((k) & 0x1f)
#define KILO_VERSION "0.0.1"

/*** data ***/
struct editorConfig {
	int screenrows;
	int screencols;
	struct termios orig_termios;
};

struct editorConfig E;

/*** terminal ***/
void die(const char *s) {
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);

	perror(s);
	exit(1);
}

void disableRawMode() {
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
		die("tcsetattr");
	}
}

void enableRawMode() {
	if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) {
		die("tcgetattr");
	}
	atexit(disableRawMode);

	struct termios raw = E.orig_termios;
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON); 
	raw.c_iflag &= ~(OPOST);
	raw.c_lflag |= (CS8);
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
		die("tcsetattr");
	}
}

char editorReadKey() {
	int nread;
	char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		if (nread == -1 && errno != EAGAIN) {
			die("read");
		}
	}
	return c;
}

int getCursorPosition(int *rows, int *cols) {
	char buff[32];
	unsigned int i = 0;

	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
		return -1;
	}

	while (i < sizeof(buff) -1) {
		if (read(STDIN_FILENO, &buff[i], 1) != 1) {
			break;
		}
		if (buff[i] == 'R') {
			break;
		}
		i++;
	}

	//assign null char to final byte of buff so printf terminates
	buff[i] = '\0';
	
	//make sure buff starts with escape sequence
	if (buff[0] != '\x1b' || buff [1] != '[') {
		return -1;
	}
	//scan from 2nd char, skipping \x1b and [ to pass xx;xx as dimensions to rows, cols
	if (sscanf(&buff[2], "%d;%d", rows, cols) != 2) {
		return -1;
	}

	return 0;
}

int getWindowSize(int *rows, int *cols) {
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
			return -1;
		}
		return getCursorPosition(rows, cols);
		return -1;
	}
	else {
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

/*** append buffer ***/
//solves problem of a lot of small writes causing screen to flicker.
//append all output to buffer then write
struct abuff {
	//pointer to buffer in memory
	char *b;
	int len;
};

//constructor
#define ABUFF_INIT {NULL, 0}

void abAppend(struct abuff *ab, const char *s, int len) {
	//realloc buffer to extend for incoming string we are appending
	char *new = realloc(ab->b, ab->len + len);

	if (new == NULL) {
		return;
	}

	//copy new string to end of newly allocated buffer space
	memcpy(&new[ab->len], s, len);
	//update buffer and length to reflect appended string
	ab->b = new;
	ab->len += len;
}

void abFree(struct abuff *ab) {
	free(ab->b);
}

/*** output ***/
void editorDrawRows(struct abuff *ab) {
	int y;
	for (y = 0; y < E.screenrows; y++) {
		//draw welcome message 1/3 down screen
		if (y == E.screenrows / 3) {
			char welcome[80];
			int welcomelen = snprintf(welcome, sizeof(welcome),
				"Kilo editor -- version %s", KILO_VERSION);
			//truncate if too long
			if (welcomelen > E.screencols) {
				welcomelen = E.screencols;
			}
			//center welcome
			int padding = (E.screencols - welcomelen) / 2;
			if (padding) {
				abAppend(ab, "~" , 1);
				padding--;
			}
			while (padding--) {
				abAppend(ab, " ", 1);
			}
			abAppend(ab, welcome, welcomelen);
		}
		else {
			abAppend(ab, "~", 1);
		}
		//clear line as we redraw
		abAppend(ab, "\x1b[K", 3);
		//make sure last line gets ~
		if (y < E.screenrows - 1) {
			abAppend(ab, "\r\n", 2);
		}
	}
}

void editorRefreshScreen() {
	struct abuff ab = ABUFF_INIT;

	//clear cursor and return cursor to home position
	abAppend(&ab, "\x1b[?25l", 6);
	abAppend(&ab, "\x1b[H", 3);

	editorDrawRows(&ab);
	
	abAppend(&ab, "\x1b[H", 3);
	abAppend(&ab, "\x1b[?25h", 6);
	
	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);
}

/*** input ***/
void editorProcessKeyPress() {
	char c = editorReadKey();

	switch (c) {
		case CTRL_KEY('q'):
			write(STDOUT_FILENO, "\x1b[2J", 4);
			write(STDOUT_FILENO, "\x1b[H", 3);
			exit(0);
			break;
	}
}

/*** init ***/
void initEditor() {
	if (getWindowSize(&E.screenrows, &E.screencols) == -1) {
		die("getWindowSize");
	}
}

int main() {
	enableRawMode();
	initEditor();

	while (1) {
		editorRefreshScreen();
		editorProcessKeyPress();
	}

	return 0;
}
