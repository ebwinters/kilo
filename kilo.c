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

enum editorKey {
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

/*** data ***/
struct editorConfig {
	int cx, cy;
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

int editorReadKey() {
	int nread;
	char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		if (nread == -1 && errno != EAGAIN) {
			die("read");
		}
	}
	if (c == '\x1b') {
		char seq[3];

		//check for timeout in first chars
		if (read(STDIN_FILENO, &seq[0], 1) != 1) {
			return '\x1b';
		}
		if (read(STDIN_FILENO, &seq[1], 1) != 1) {
			return '\x1b';
		}

		//check if first two chars make arrow esc sequence
		//then alias to WASD keys
		if (seq[0] == '[') {
			//if second byte is digit, we expect ~ as third
			//representing page up/down or home/end depending on digit
			if (seq[1] >= '0' && seq[1] <= '9') {
				if (read(STDIN_FILENO, &seq[2], 1) != 1) {
					return '\x1b';
				}
				if (seq[2] == '~') {
					switch (seq[1]) {
						case '1':
							return HOME_KEY;
						case '3':
							return DEL_KEY;
						case '4':
							return END_KEY;
						case '5':
							return PAGE_UP;
						case '6':
							return PAGE_DOWN;
						case '7':
							return HOME_KEY;
						case '8':
							return END_KEY;
					}
				}
			}
			else {
				switch (seq[1]) {
					case 'A': return ARROW_UP;
					case 'B': return ARROW_DOWN;
					case 'C': return ARROW_RIGHT;
					case 'D': return ARROW_LEFT;
					case 'H': return HOME_KEY;
					case 'F': return END_KEY;
				}
			}
		}
		else if (seq[0] == 'O') {
			switch (seq[1]) {
				case 'H': return HOME_KEY;
				case 'F': return END_KEY;
			}
		}
		return '\x1b';
	}
	else {
		return c;
	}
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

	//move cursor
	char buff[32];
	//terminal uses 1 indexed vals
	snprintf(buff, sizeof(buff), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
	abAppend(&ab, buff, strlen(buff));
	
	abAppend(&ab, "\x1b[?25h", 6);
	
	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);
}

/*** input ***/
void editorMoveCursor(int key) {
	switch(key) {
		//Logic in each case to make sure cursor stays on screen
		case ARROW_LEFT:
			if (E.cx != 0) {
				E.cx--;
			}
			break;
		case ARROW_RIGHT:
			if (E.cx != E.screencols - 1) {
				E.cx++;
			}
			break;
		case ARROW_UP:
			if (E.cy != 0) {
				E.cy--;
			}
			break;
		case ARROW_DOWN:
			if (E.cy != E.screenrows - 1) {
				E.cy++;
			}
			break;
	}
}

void editorProcessKeyPress() {
	int c = editorReadKey();

	switch (c) {
		case CTRL_KEY('q'):
			write(STDOUT_FILENO, "\x1b[2J", 4);
			write(STDOUT_FILENO, "\x1b[H", 3);
			exit(0);
			break;

		//home and end move all the way l/r respectively
		case HOME_KEY:
			E.cx = 0;
			break;
		case END_KEY:
			E.cx = E.screencols - 1;
			break;

		case PAGE_UP:
		case PAGE_DOWN:
			{
				//move cursor to top or bottom of screen for page up/down
				int times = E.screenrows;
				while (times--) {
					editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
				}
			}
			break;

		case ARROW_UP:
		case ARROW_DOWN:
		case ARROW_LEFT:
		case ARROW_RIGHT:
			editorMoveCursor(c);
			break;
	}
}

/*** init ***/
void initEditor() {
	E.cx = 0;
	E.cy = 0;

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
