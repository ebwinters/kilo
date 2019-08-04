/*** includes ***/
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/
#define CTRL_KEY(k) ((k) & 0x1f)
#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8

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
//hold data for a row of text
typedef struct erow {
	int size;
	int rsize;
	char *chars;
	char *render;
} erow;

struct editorConfig {
	int cx, cy;
	int rx;	//will be greater than cx by however many extra stabs
	int rowoff;	//what row are we scrolled to
	int coloff; //what col are we scrolled to
	int screenrows;
	int screencols;
	int numrows;
	erow *row;
	char *filename;
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

/*** row operations ***/
int editorRowCxToRx(erow *row, int cx) {
	int rx = 0;
	int j;
	for (j = 0; j < cx; j++) {
		if (row->chars[j] == '\t') {
			//tabstop - how many cols to right of last tabstop we are
			//add to rx to get just to let of next tabstop and rx++ to get on tabstop
			rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
		}
		rx++;
	}
	return rx;
}

//take chars from row and fill render string
void editorUpdateRow(erow *row) {
	free(row->render);
	
	int tabs = 0;
	int j;
	for (j = 0; j < row->size; j++) {
		if (row->chars[j] == '\t') {
			tabs++;
		}
	}

	//set correct memory including spaces for amount of tabs found
	row->render = malloc(row->size + tabs*(KILO_TAB_STOP - 1) + 1);

	int idx = 0;
	for (j = 0; j < row->size; j++) {
		if (row->chars[j] == '\t') {
			//append spaces until tabstop at col divisible by 8
			row->render[idx++] = ' ';
			while (idx % KILO_TAB_STOP != 0) {
				row->render[idx++] = ' ';
			}
		}
		else {
			row->render[idx++] = row->chars[j];
		}
	}	
	row->render[idx] = '\0';
	row->rsize = idx;
}

void editorAppendRow(char *line_text, size_t len) {
	//make space in array for new row entry
	E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
	int at = E.numrows;	//store number rows before appending
	E.row[at].size = len;
	E.row[at].chars = malloc(len+1);
	memcpy(E.row[at].chars, line_text, len);
	E.row[at].chars[len] = '\0';
	E.row[at].rsize = 0;
	E.row[at].render = NULL;
	editorUpdateRow(&E.row[at]);
	E.numrows++;
}

/*** file i/o ***/
//open and read file from disk
void editorOpen(char *filename) {
	free(E.filename);
	E.filename = strdup(filename);

	FILE *fp = fopen(filename, "r");
	if (!fp) {
		die("fopen");
	}
	
	char *line = NULL;
	size_t linecap = 0; //how much memory is allocated for line
	ssize_t linelen;
	//read file line by line
	while ((linelen = getline(&line, &linecap, fp)) != -1) {
		while (linelen > 0 && (line[linelen-1] == '\n' || line[linelen-1] == '\r')) {
			linelen--;
		}
		editorAppendRow(line, linelen);
	}
	//EOF or no more lines to read
	free(line);
	fclose(fp);
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
//check if cursor is outside screen on scrolling, and if it is, put it just inside
void editorScroll() {
	E.rx = 0;
	if (E.cy < E.numrows) {
		E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
	}
	//remember rowoff refers to top of screen content
	if (E.cy < E.rowoff) {
		//cursor above visible window, adjust
		E.rowoff = E.cy;
	}
	if (E.cy >= E.rowoff + E.screenrows) {
		//cursor is below the visible window, adjust
		E.rowoff = E.cy - E.screenrows + 1;
	}
	if (E.rx < E.coloff) {
		E.coloff = E.rx;
	}
	if (E.rx >= E.coloff + E.screencols) {
		E.coloff = E.rx - E.screencols + 1;
	}
}

void editorDrawRows(struct abuff *ab) {
	int y;
	for (y = 0; y < E.screenrows; y++) {
		//set filerow to display scrolled rows
		int filerow = y + E.rowoff;
		if (filerow >= E.numrows) {
			if (y >= E.numrows) {
				//only display welcome if no file specified
				if (E.numrows == 0 && y == E.screenrows/3) {
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
				}
				//draw welcome message 1/3 down screen
				else {
					abAppend(ab, "~", 1);
				}
			}
		}
		//are we drawing a row that is after a text bufer or before
		//if after do this, else go to else
		else {
			int len = E.row[filerow].rsize - E.coloff;
			//user scrolled past EOL, display nothing
			if (len < 0) {
				len = 0;
			}
			if (len > E.screencols) {
				len = E.screencols;
			}
			//print current line
			abAppend(ab, &E.row[filerow].render[E.coloff], len);
		}
		//clear line as we redraw
		abAppend(ab, "\x1b[K", 3);
		//make sure last line gets ~
		abAppend(ab, "\r\n", 2);
	}
}

void editorDrawStatusBar(struct abuff *ab) {
	//switch to inverted text coloring using 7m and draw a inverted row
	abAppend(ab, "\x1b[7m", 4);
	char status[80], rtstatus[80];
	int len = snprintf(status, sizeof(status), "%.20s - %d lines",
		E.filename ? E.filename : "[No Name", E.numrows);
	int rtlen = snprintf(rtstatus, sizeof(rtstatus), "%d/%d", E.cy + 1, E.numrows);
	if (len > E.screencols) {
		len = E.screencols;
	}
	abAppend(ab, status, len);

	//still draw spaces until EOL, so all inverted
	while (len < E.screencols) {
		//keep printing spaces until just enough space for right status
		if (E.screencols - len == rtlen) {
			abAppend(ab, rtstatus, rtlen);
			break;
		}
		else {
			abAppend(ab, " ", 1);
			len++;
		}
	}
	//go back to normal text formatting
	abAppend(ab, "\x1b[m", 3);
}

void editorRefreshScreen() {
	editorScroll();

	struct abuff ab = ABUFF_INIT;

	//clear cursor and return cursor to home position
	abAppend(&ab, "\x1b[?25l", 6);
	abAppend(&ab, "\x1b[H", 3);

	editorDrawRows(&ab);
	editorDrawStatusBar(&ab);

	//move cursor
	char buff[32];
	//terminal uses 1 indexed vals
	//cursor needs to represent screen not file position
	snprintf(buff, sizeof(buff), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
	abAppend(&ab, buff, strlen(buff));
	
	abAppend(&ab, "\x1b[?25h", 6);
	
	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);
}

/*** input ***/
void editorMoveCursor(int key) {
	//check if cursor on actual line, and set to line cursor is on
	erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
	switch(key) {
		//Logic in each case to make sure cursor stays on screen
		case ARROW_LEFT:
			if (E.cx != 0) {
				E.cx--;
			}
			//move cursor to end of line above if not first line
			else if (E.cy > 0) {
				E.cy --;
				E.cx = E.row[E.cy].size;
			}
			break;
		case ARROW_RIGHT:
			//check cursor to the left of EOL
			if (row && E.cx < row->size) {
				E.cx++;
			}
			//move cursor to next line
			else if(row && E.cx == row->size) {
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
			//don't scroll below EOF
			if (E.cy < E.numrows) {
				E.cy++;
			}
			break;
	}
	
	//want to make sure we can't go longer line to shorter and be past EOL
	row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
	int rowlen = row ? row->size : 0;
	if (E.cx > rowlen) {
		E.cx = rowlen;
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
	E.rx = 0;
	E.rowoff = 0;
	E.coloff = 0;
	E.numrows = 0;
	E.row = NULL;
	E.filename = NULL;

	if (getWindowSize(&E.screenrows, &E.screencols) == -1) {
		die("getWindowSize");
	}
	//make space for status bar
	E.screenrows -= 1;
}

int main(int argc, char *argv[]) {
	enableRawMode();
	initEditor();
	if (argc >= 2) {
		editorOpen(argv[1]);
	}

	while (1) {
		editorRefreshScreen();
		editorProcessKeyPress();
	}

	return 0;
}
