// Console input and output.
// Input is from the keyboard or serial port.
// Output is written to the screen and serial port.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "traps.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"

static void consputc(int);

static int panicked = 0;

static struct {
	struct spinlock lock;
	int locking;
} cons;

static void
printint(int xx, int base, int sign)
{
	static char digits[] = "0123456789abcdef";
	char buf[16];
	int i;
	uint x;

	if(sign && (sign = xx < 0))
		x = -xx;
	else
		x = xx;

	i = 0;
	do{
		buf[i++] = digits[x % base];
	}while((x /= base) != 0);

	if(sign)
		buf[i++] = '-';

	while(--i >= 0)
		consputc(buf[i]);
}

// Print to the console. only understands %d, %x, %p, %s.
void
cprintf(char *fmt, ...)
{
	int i, c, locking;
	uint *argp;
	char *s;

	locking = cons.locking;
	if(locking)
		acquire(&cons.lock);

	if (fmt == 0)
		panic("null fmt");

	argp = (uint*)(void*)(&fmt + 1);
	for(i = 0; (c = fmt[i] & 0xff) != 0; i++){
		if(c != '%'){
			consputc(c);
			continue;
		}
		c = fmt[++i] & 0xff;
		if(c == 0)
			break;
		switch(c){
		case 'd':
			printint(*argp++, 10, 1);
			break;
		case 'x':
		case 'p':
			printint(*argp++, 16, 0);
			break;
		case 's':
			if((s = (char*)*argp++) == 0)
				s = "(null)";
			for(; *s; s++)
				consputc(*s);
			break;
		case '%':
			consputc('%');
			break;
		default:
			// Print unknown % sequence to draw attention.
			consputc('%');
			consputc(c);
			break;
		}
	}

	if(locking)
		release(&cons.lock);
}

void
panic(char *s)
{
	int i;
	uint pcs[10];

	cli();
	cons.locking = 0;
	// use lapiccpunum so that we can call panic from mycpu()
	cprintf("lapicid %d: panic: ", lapicid());
	cprintf(s);
	cprintf("\n");
	getcallerpcs(&s, pcs);
	for(i=0; i<10; i++)
		cprintf(" %p", pcs[i]);
	panicked = 1; // freeze other CPU
	for(;;)
		;
}

#define INPUT_BUF 128
struct {
	char buf[INPUT_BUF];
	uint r;  // Read index
	uint w;  // Write index
	uint e;  // Edit index
} input;

#define SAVED_MAX 3
char command_stack[SAVED_MAX][INPUT_BUF];
int command_ptr = -1;
int saved = 0;

#define BACKSPACE 0x100
#define KEY_UP          0xE2
#define KEY_DN          0xE3
#define C(x)  ((x) - '@')  // Control-x
#define S(x) ((x) + '@') // Shift-x for KEY_UP and KEY_DN


int history_color = 0;
void moveCommandsUp() {
	for (int i = SAVED_MAX - 1; i >= 1; i--) {
		for (int j = 0; j < INPUT_BUF; j++) {
			command_stack[i][j] = command_stack[i - 1][j];
		}
	}
}

void copyCommand() {
	moveCommandsUp();
	memset(command_stack[0], '\0', INPUT_BUF);
	int ptr = 0, buffer_ptr = input.w;
    while (buffer_ptr != input.e && ptr < INPUT_BUF)
    {
        char ch = input.buf[buffer_ptr++ % INPUT_BUF];
        command_stack[0][ptr++] = (ch != '\n' ? ch : '\0');
    }
}


void clearCommandLine() {
	while(input.e != input.w &&
		input.buf[(input.e-1) % INPUT_BUF] != '\n'){
		input.e--;
		consputc(BACKSPACE);
	}
}

void writeCommand() {
	clearCommandLine();

	history_color = 1;
	for (int i = 0; i < INPUT_BUF; i++) {
		char c = command_stack[command_ptr][i];
		if (c == '\n')
			break;
		input.buf[input.e++ % INPUT_BUF] = c;
		consputc(c);
	}
	history_color = 0;
}

void moveThroughCommandHistory(int upOrDown) {
	if (command_ptr == -1) {
		if (upOrDown == S(KEY_UP)) {
			if (saved >= 1) {
				command_ptr = 0;
				writeCommand();
			}
		}
	}
	else {
		if (upOrDown == S(KEY_UP)) {
			if (command_ptr + 1 < saved) {
				command_ptr++;
				writeCommand();
				if (command_ptr == SAVED_MAX)
					command_ptr = SAVED_MAX - 1;
			}
		}
		else if (upOrDown == S(KEY_DN)) {
			if (command_ptr > 0) {
				command_ptr--;
				writeCommand();
			}
			else {
				clearCommandLine();
				command_ptr = -1;
			}
		}
	}
}


#define CRTPORT 0x3d4
static ushort *crt = (ushort*)P2V(0xb8000);  // CGA memory

static void
cgaputc(int c)
{
	int pos;

	static int black_on_white = 0x0700;
	static int green_on_white = 0x0200;
	int code_color = black_on_white;
	if (history_color) {
		code_color = green_on_white;
	}

	// Cursor position: col + 80*row.
	outb(CRTPORT, 14);
	pos = inb(CRTPORT+1) << 8;
	outb(CRTPORT, 15);
	pos |= inb(CRTPORT+1);

	if(c == '\n')
		pos += 80 - pos%80;
	else if(c == BACKSPACE){
		if(pos > 0) --pos;
	} else
		crt[pos++] = (c&0xff) | code_color;  // black on white

	if(pos < 0 || pos > 25*80)
		panic("pos under/overflow");

	if((pos/80) >= 24){  // Scroll up.
		memmove(crt, crt+80, sizeof(crt[0])*23*80);
		pos -= 80;
		memset(crt+pos, 0, sizeof(crt[0])*(24*80 - pos));
	}

	outb(CRTPORT, 14);
	outb(CRTPORT+1, pos>>8);
	outb(CRTPORT, 15);
	outb(CRTPORT+1, pos);
	crt[pos] = ' ' | 0x0700;
}

void
consputc(int c)
{
	if(panicked){
		cli();
		for(;;)
			;
	}

	if(c == BACKSPACE){
		uartputc('\b'); uartputc(' '); uartputc('\b');
	} else
		uartputc(c);
	cgaputc(c);
}

void
consoleintr(int (*getc)(void))
{
	int c, doprocdump = 0;

	acquire(&cons.lock);
	while((c = getc()) >= 0){
		switch(c){
		case C('P'):  // Process listing.
			// procdump() locks cons.lock indirectly; invoke later
			doprocdump = 1;
			break;
		case C('U'):  // Kill line.
			clearCommandLine();
			break;
		case C('H'): case '\x7f':  // Backspace
			if(input.e != input.w){
				input.e--;
				consputc(BACKSPACE);
				command_ptr = -1;
			}
			break;
		case S(KEY_UP): case S(KEY_DN):
			moveThroughCommandHistory(c);
			break;

		default:
			if(c != 0 && input.e-input.r < INPUT_BUF){
				c = (c == '\r') ? '\n' : c;
				input.buf[input.e++ % INPUT_BUF] = c;
				command_ptr = -1;
				consputc(c);
				if(c == '\n' || c == C('D') || input.e == input.r+INPUT_BUF){
					if (input.e != input.w + 1) { // it is not an empty line
						copyCommand(input.w, input.e);
						if (saved < SAVED_MAX)
							saved++;
					}
					input.w = input.e;
					wakeup(&input.r);
				}
			}
			break;
		}
	}

	release(&cons.lock);
	if(doprocdump) {
		procdump();  // now call procdump() wo. cons.lock held
	}
}



int
consoleread(struct inode *ip, char *dst, int n)
{
	uint target;
	int c;

	iunlock(ip);
	target = n;
	acquire(&cons.lock);

	while(n > 0){
		while(input.r == input.w){
			if(myproc()->killed){
				release(&cons.lock);
				ilock(ip);
				return -1;
			}
			sleep(&input.r, &cons.lock);
		}
		c = input.buf[input.r++ % INPUT_BUF];

		/// print input.buf[input.r % 128]
		if(c == C('D')){  // EOF
			if(n < target){
				// Save ^D for next time, to make sure
				// caller gets a 0-byte result.
				input.r--;
			}
			break;
		}
		*dst++ = c;
		--n;
		if(c == '\n') {
			break;
		}
	}

	release(&cons.lock);
	ilock(ip);

	return target - n;
}

int
consolewrite(struct inode *ip, char *buf, int n)
{
	int i;

	iunlock(ip);
	acquire(&cons.lock);
	for(i = 0; i < n; i++)
		consputc(buf[i] & 0xff);
	release(&cons.lock);
	ilock(ip);

	return n;
}

void
consoleinit(void)
{
	initlock(&cons.lock, "console");

	devsw[CONSOLE].write = consolewrite;
	devsw[CONSOLE].read = consoleread;
	cons.locking = 1;

	ioapicenable(IRQ_KBD, 0);
}

