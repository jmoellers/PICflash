# include	<stdint.h>
# include	<sys/types.h>
# include	<sys/stat.h>
# include	<stdio.h>
# include	<unistd.h>
# include	<stdlib.h>
# include	<string.h>
# include	<fcntl.h>
# include	<errno.h>
# include	<ctype.h>
# include	<time.h>
# include	<getopt.h>
# include	"picpgm.h"

# define	MSB_FIRST	(-1)
# define	LSB_FIRST	1

# define	W0	0
# define	W1	1
# define	W2	2
# define	W3	3
# define	W4	4
# define	W5	5
# define	W6	6
# define	W7	7
# define	W10	10
# define	TBLPAG	0x0032
# define	NVMCON	0x0760
# define	VISI	0x0784
# define	NVMCON_WR_bit	15	/* Bit number of WR bit in NVMCON */

enum adress_mode {
    REGISTER_DIRECT = 0,
    INDIRECT,
    INDIRECT_POST_DECREMENT,
    INDIRECT_POST_INCREMENT,
    INDIRECT_PRE_DECREMENT,
    INDIRECT_PRE_INCREMENT,
    INDERCT_WITH_REGISTER_OFFSET
};

struct part {
    uint16_t device_ID;
    char *name;
    unsigned int progmem_size;
    unsigned int sram_size;
    unsigned int config_addr;	/* This is a WORD ADDRESS! */
    unsigned int EEPROM_size;
} part[] = {
    { 0x0444, "PIC24FJ16GA002", 16*1024, 4*1024, 0x2bfc, 0 },
    { 0x0445, "PIC24FJ32GA002", 32*1024, 8*1024, 0x57fc, 0 },
    { 0x0446, "PIC24FJ48GA002", 48*1024, 8*1024, 0x83fc, 0 },
    { 0x0447, "PIC24FJ64GA002", 64*1024, 8*1024, 0xabfc, 0 },
    { 0x044c, "PIC24FJ16GA004", 16*1024, 4*1024, 0x2bfc, 0 },
    { 0x044d, "PIC24FJ32GA004", 32*1024, 8*1024, 0x57fc, 0 },
    { 0x044e, "PIC24FJ48GA004", 48*1024, 8*1024, 0x83fc, 0 },
    { 0x044f, "PIC24FJ64GA004", 64*1024, 8*1024, 0xabfc, 0 },
    { 0x0000, NULL, 0, 0 }
};


int debug;
int trace_SIX;
int dry_run = 0;

static int init_pins();
static int parse_operand(const char *op, unsigned int *mode, unsigned int *reg);
extern int load_config(const char *pathname, void *user_data);
static size_t load_hex(char *pathname, struct part *p, unsigned char *bytes, unsigned int memsize);
static int isallFF(unsigned char *bytes, unsigned int n);
static int do_unexport(struct pin *pin);
static int waitopen(const char *path, int flags);
static void send(unsigned int n, size_t bits, int direction);

static void enter_ICSP_mode();
static void exit_ICSP_mode();
static uint32_t *read_code_memory(unsigned int start, unsigned int nwords);
static unsigned char *read_code_memory_bytes(unsigned int start, unsigned int nwords);
static void read_configuration_memory(uint32_t addr, uint16_t *cw0, uint16_t *cw1);
static uint16_t read_application_ID();
static void chip_erase();
static int write_code_memory(unsigned int byte_addr, unsigned char *bytes);
static void write_configuration_registers(unsigned int addr, uint16_t cw1, uint16_t cw2);

struct pin pins[] = {
    { "MCLR", -1, -1, 0, "low", -1 },	/* For programming, MCLR/ must be pulled low */
    { "PGM", -1, -1, 0, "low", -1 },	/* Not used on PIC24 */
    { "CLK", -1, -1, 0, "low", -1 },
    { "DOUT", -1, -1, 0, "low", -1 },
    { "DIN", -1, -1, 0, "in", -1 },
    { NULL, 0, 0, 0, 0 }
};

struct option longopts[] = {
    { "debug", no_argument, NULL, 'D' },
    { "trace-SIX", no_argument, NULL, 'S' },
    { NULL, 0, NULL, '\0' }
};

int
main(int argc, char *argv[])
{
    int ch;
    int i;
    unsigned char *b;
    uint16_t cw[2], appID;
    unsigned int ninstr;
    struct part *p;
    int exitstat = 0;	/* assume no problems */
    uint16_t DEVID[2];
    int error;

    error = 0;
    while ((ch = getopt_long(argc, argv, "DS", longopts, NULL)) != -1)
        switch (ch)
	{
	case 'D': debug++; break;
	case 'S': trace_SIX++; break;
	default: error++; break;
	}

    if (error)
        exit(255);

    if (!load_config("/home/pi/pgmifcfg.xml", &pins) && !load_config("/opt/picpgm/pgmifcfg.xml", &pins))
        exit(1);

    if (!init_pins())
        exit(2);

    enter_ICSP_mode();

    b = read_code_memory_bytes(0xff0000, 1);
    DEVID[0] = b[0] | (b[1] << 8);
    DEVID[1] = b[4] | (b[5] << 8);
    if (debug)
	fprintf(stderr, "DEVICE ID=%04x DEVICE REVISION=%04x\n", DEVID[0], DEVID[1]);
    free(b);
    for (i = 0, p = part; p->name != NULL && p->device_ID != DEVID[0]; i++, p++)
        ;

    if (p->name == NULL)
    {
        fprintf(stderr, "Unknown chip with Device ID %04x\n", DEVID[0]);
	exit(3);
    }

    fprintf(stderr, "PIC name:    %s (rev %d.%d)\n",
    	p->name,
	(DEVID[1] >> 6) & 0x07, DEVID[1] & 0x07);
    fprintf(stderr, "Device ID:   0x%04x\n", DEVID[0]);
    fprintf(stderr, "Flash:       %u Bytes\n", p->progmem_size);
    fprintf(stderr, "SRAM:        %u bytes\n", p->sram_size);
    fprintf(stderr, "EEPROM:      %u Bytes\n", p->EEPROM_size);

    appID = read_application_ID();
    fprintf(stderr, "app ID:      0x%04x\n", appID);

    for (; optind < argc; optind++)
        if (strcmp(argv[optind], "erase") == 0)
	{
	    fprintf(stderr, "Erasing program memory ...");
	    chip_erase();
	    fprintf(stderr, " done\n");
	}
	else if (strncmp(argv[optind], "flash:", 6) == 0)
	{
	    char *hexpath = &argv[optind][6];
	    size_t nbytes;
	    unsigned char *bytes = malloc(p->progmem_size);

	    memset(bytes, 0xff, p->progmem_size);
	    fprintf(stderr, "Flashing \"%s\"\n", hexpath);
	    if ((nbytes = load_hex(hexpath, p, bytes, p->progmem_size)) != 0)
	    {
		uint16_t cw1, cw2;
		unsigned int cwaddr;
		int n;
		unsigned int ip;

		ninstr = nbytes / 4;

		cwaddr = p->config_addr * 2;	/* config_addr is a WORD ADDRESS */
		cw2 = bytes[cwaddr] | (bytes[cwaddr+1] << 8);
		cw1 = bytes[cwaddr+4] | (bytes[cwaddr+5] << 8);
		fprintf(stderr, "cw1=%04x, cw2=%04x\n", cw1, cw2);
		memset(&bytes[cwaddr], 0xff, 2 * sizeof(uint32_t));

		for (ip = 0; ip < ninstr; ip += 64)
		{
		    if (!isallFF(&bytes[ip*4], 64*4))
		    {
		    	fprintf(stdout, "\r%08x", ip * 4); fflush(stdout);
			write_code_memory(ip * 4, &bytes[ip * 4]);
		    }
		}
		fputc('\n', stdout);

		write_configuration_registers(p->config_addr, cw1, cw2);	/* config_addr is a WORD ADDRESS */
	    }
	}
	else if (strncmp(argv[optind], "verify:", 7) == 0)
	{
	    char *hexpath = &argv[optind][7];
	    size_t nbytes;
	    unsigned char *bytes = malloc(p->progmem_size);

	    memset(bytes, 0xff, p->progmem_size);
	    fprintf(stderr, "Verifying \"%s\"\n", hexpath);
	    if ((nbytes = load_hex(hexpath, p, bytes, p->progmem_size)) != 0)
	    {
		uint16_t cw1, cw2, PIC_cw1, PIC_cw2;
		unsigned int cwaddr;
		int n;
		unsigned int ip;

		ninstr = nbytes / 4;

		cwaddr = p->config_addr * 2;	/* config_addr is a WORD ADDRESS */
		cw2 = bytes[cwaddr] | (bytes[cwaddr+1] << 8);
		cw1 = bytes[cwaddr+4] | (bytes[cwaddr+5] << 8);
		if (debug)
		    fprintf(stderr, "cw1=%04x, cw2=%04x\n", cw1, cw2);
		memset(&bytes[cwaddr], 0xff, 2 * sizeof(uint32_t));

		for (ip = 0; ip < ninstr; ip += 64)
		{
		    fprintf(stdout, "\r%08x", ip * 4); fflush(stdout);
		}
		fputc('\n', stdout);

		read_configuration_memory(p->config_addr, &PIC_cw1, &PIC_cw2);
		if (cw1 != PIC_cw1)
		{
		    fprintf(stderr, "CW1 mismatch: file=%04x PIC=%04x\n", cw1, PIC_cw1);
		    exitstat = 1;
		}
		if (cw2 != PIC_cw2)
		{
		    fprintf(stderr, "CW2 mismatch: file=%04x PIC=%04x\n", cw2, PIC_cw2);
		    exitstat = 1;
		}
	    }
	}

    exit_ICSP_mode();

    for (i = 0; pins[i].name != NULL; i++)
        do_unexport(&pins[i]);

    exit(0);
}

/*
 * NAME: load_hex
 * PURPOSE: To load a file in Intel HEX format
 * ARGUMENTS: pathname: path name of file
 *	p: address of struct part of chip
 *	bytes: code storage in unsigned chars
 *	memsize: size of code
 * RETURNS: highest address loaded
 * SEE https://en.wikipedia.org/wiki/Intel_HEX
 */
static size_t
load_hex(char *pathname, struct part *p, unsigned char *bytes, unsigned int memsize)
{
    FILE *fp;
    char *line = NULL, *lp;
    size_t linesz;
    ssize_t linelen;
    unsigned int base = 0;	/* contains base address due to record type 04 */
    int lineno;
    size_t highest = 0;

    if ((fp = fopen(pathname, "rt")) == NULL)
    {
        perror(pathname);
	return 0;
    }

    for (lineno = 1; (linelen = getline(&line, &linesz, fp)) != -1; lineno++)
    {
	unsigned int reclen, load_offset, rectype, csum;
	int i;

        for (lp = line; *lp != '\0' && isspace(*lp); lp++)
	    ;
	if (*lp != ':')
	{
	    fprintf(stderr, "%d: line does not start with a colon\n", lineno);
	    continue;
	}
	lp++;
	if (sscanf(lp, "%02x%04x%02x", &reclen, &load_offset, &rectype) != 3)
	{
	    fprintf(stderr, "%d: Failed to pick up reclen, load_offset and rectype\na, lineno");
	    continue;
	}
	lp += 2+4+2;
	csum = reclen + ((load_offset >> 8) & 0xff) + (load_offset & 0xff) + rectype;
	for (i = 0; i <= reclen; i++)
	{
	    unsigned int byte;

	    if (sscanf(&lp[i*2], "%02x", &byte) != 1)
	        break;
	    csum += byte;
	}
	if (i <= reclen)
	    continue;
	if ((csum & 0xff) != 0)
	    fprintf(stderr, "%d: Checksum error\n", lineno);

        switch (rectype)
	{
	case 0x04: /* Extended Linear Address Record */
	    for (base = 0, i = 0; i < reclen; i++)
	    {
		unsigned int byte;
	        sscanf(&lp[i*2], "%02x", &byte);
		base = (base << 8) | byte;
	    }
	    base <<= 16;
	    break;
	case 0x00: /* Data Record */
	    for (i = 0; i < reclen; i++)
	    {
	        unsigned int byte;
		unsigned int shift;
		uint32_t mask;
		unsigned int addr;

		addr = base + load_offset + i;

		sscanf(&lp[i*2], "%02x", &byte);
		bytes[addr] = byte;
		shift = (i & 0x03) * 8;
		mask = ~(0xff << shift);
		/*
		 * The config words are at the very end of the program memory
		 * config_addr is a WORD ADDRESS
		 */
	        if (addr < p->config_addr*2)
		    if (addr > highest)
		        highest = addr;
	    }
	    break;
	case 0x01: /* End of File Record */
	    break;
	default:
	    fprintf(stderr, "%d: unknown record type %02x\n", lineno, rectype);
	    fclose(fp);
	    return 0;
	}

	if (rectype == 0x01)
	    break;
    }

    if (line != NULL)
	free(line);

    fclose(fp);
    return highest;
}

/*
 * NAME: isallFF
 * PURPOSE: To check if a number of instructions is FFFFFF
 * ARGUMENTS: instr: address of first instruction
 *	n: number of instructions to check
 * RETURNS: 1 if all n instructions are FFFFFF, else 0
 * NOTE: This is used to skip portions which will be set to FFFFFF
 *	which is the setting after erasing
 */
static int 
isallFF(unsigned char *bp, unsigned int n)
{
    for (; n > 0; n--, bp++)
        if (*bp != 0xff)
	    return 0;
    return 1;
}

/* **************************************************** */
/*
 * NAME: set_direction
 * PÜRPOSE: To set the direction (and possibly initial state)
 *	of a GPIO pin
 * ARGUMENTS: gpio: GPIO pin number
 *	direction: "in", "out", "high" or "low"
 * RETURNS: 0 on failure, non-0 on success
 */
static int
set_direction(unsigned int gpio, char *direction)
{
    char path[BUFSIZ];
    int fd;

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", gpio);
    if ((fd = waitopen(path, O_WRONLY)) == -1)
    {
        perror("direction");
	return 0;
    }
    if (write(fd, direction, strlen(direction)) != strlen(direction))
    {
        perror("write(direction)");
	close(fd);
	return 0;
    }
    close(fd);

    return 1;
}

/*
 * NAME: init_pins
 * PURPOSE: To initialize the pins this program needs
 * ARGUMENTS: None
 * RETURNS: 0 on failure, non-0 on success
 */
static int
init_pins()
{
    struct pin *pin;
    int fd;
    char path[BUFSIZ];
    int mode;

    for (pin = pins; pin->name != NULL; pin++)
    {
        if (debug)
	    fprintf(stderr, "%s => pin=%d, invert=%d\n", pin->name, pin->pin, pin->invert);

	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d", pin->pin);
	if (access(path, F_OK) == -1)
	{
	    char nn[5];

	    if ((fd = open("/sys/class/gpio/export", O_WRONLY)) == -1)
	    {
		perror("export");
		return 0;
	    }
	    snprintf(nn, sizeof(nn), "%d", pin->pin);
	    if (write(fd, nn, strlen(nn)) != strlen(nn))
	    {
		perror("write(export)");
		close(fd);
		return 0;
	    }
	    close(fd);

	    pin->exported = 1;
	}
	set_direction(pin->pin, pin->direction);

	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin->pin);
	if (strcmp(pin->direction, "low") == 0
	 || strcmp(pin->direction, "high") == 0
	 || strcmp(pin->direction, "out") == 0)
	    mode = O_WRONLY;
	else if (strcmp(pin->direction, "in") == 0)
	    mode = O_RDONLY;

	pin->fd = waitopen(path, mode);
	if (pin->fd == -1)
	{
	    perror("value.open");
	    return 0;
	}
    }

    return 1;
}

/*
 * NAME: do_unexport
 * PURPOSE: To un-export GPIO pins previously exported
 * ARGUMENTS: pin: address of struct pin describing the GPIO pin
 * RETURNS: 0 on failure, non-0 on success
 */
static int
do_unexport(struct pin *pin)
{
    int fd;
    char nn[5];

    if (!pin->exported)
        return 1;

    if (pin->fd != -1)
        close(pin->fd);

    if ((fd = open("/sys/class/gpio/unexport", O_WRONLY)) == -1)
    {
	perror("unexport");
	return 0;
    }
    snprintf(nn, sizeof(nn), "%d", pin->pin);
    if (write(fd, nn, strlen(nn)) != strlen(nn))
    {
	perror("write(unexport)");
	close(fd);
	return 0;
    }
    close(fd);

    return 1;
}

/*
 * NAME: waitopen
 * PURPOSE: To wait until a GPIO special file can be opened
 * ARGUMENTS: path, flags: see open(2)
 * RETURNS: see open(2)
 * NOTE: Sometimes it takes a short while until a GPIO pin is
 *	properly exported
 */
static int
waitopen(const char *path, int flags)
{
    int fd;

    while (((fd = open(path, flags)) == -1) && (errno == EACCES))
        usleep(1000);   /* wait a (milli) second! */

    return fd;
}

/*
 * NAME: filedescriptor
 * PURPOSE: To find a file descriptor of "value of a GPIO pin
 * ARGUMENTS: name: name of pin, eg "mclr" or "din"
 * RETURNS: file descriptor or -1 on error
 */
static int
filedescriptor(char *name)
{
    int i;

    for (i = 0; pins[i].name != NULL && strcmp(pins[i].name, name) != 0; i++)
        ;

    if (pins[i].name != NULL)
        return pins[i].fd;

    return -1;
}

/*
 * NAME: nsleep
 * PURPOSE: To delay execution for at least a given number of ns
 * ARGUMENTS: nsec: number of ns to delay
 * RETURNS: Nothing
 * NOTE: This is a busy-wait
 */
static inline void
nsleep(unsigned int nsec)
{
    struct timespec now, then;

    clock_gettime(CLOCK_REALTIME, &now);
    then = now;
    then.tv_nsec += nsec;
    while (then.tv_nsec >= 1000000000L)
    {
        then.tv_nsec -= 1000000000L;
	then.tv_sec++;
    }

    do {
        clock_gettime(CLOCK_REALTIME, &now);
    } while (now.tv_sec < then.tv_sec && now.tv_nsec < then.tv_nsec);

    return;
}

/*
 * NAME: Px
 * PURPOSE: To delay execution for a time given by the chip data sheet
 * ARGUMENTS: None
 * RETURNS: Nothing
 */
static inline void
P1()
{
    nsleep(100);	/* >= 100ns */
}

static inline void
P1A()
{
    nsleep(40);	/* >= 40ns */
}

static inline void
P1B()
{
    nsleep(40);	/* >= 40ns */
}

static inline void
P2()
{
    nsleep(15);	/* >= 15ns */
}

static inline void
P3()
{
    nsleep(15);	/* >= 15ns */
}

static inline void
P4()
{
    nsleep(40);	/* >= 40ns */
}

static inline void
P4A()
{
    nsleep(40);	/* >= 40ns */
}

static inline void
P5()
{
    nsleep(20);	/* >= 20ns */
}

static inline void
P16()
{
    /* >= 0s */
}

static inline void
P18()
{
    nsleep(40);	/* >= 40ns */
}

static inline void
P19()
{
    usleep(1000);	/* >= 1ms */
}

/*
 * NAME: send
 * PURPOSE: To send bits to the PIC24
 * ARGUMENTS: value: bits to send
 *	bits: number of bits from value to send
 *	direction: MSB_FIRST or LSB_FIRST
 * RETURNS: Nothing
 */
static void
send(unsigned int value, size_t bits, int direction)
{
    int clk, dout;
    unsigned int mask;
    int i;

    if (bits == 0)
    {
        fprintf(stderr, "send() called with bits==0\n");
	return;
    }
    if ((clk = filedescriptor("CLK")) == -1)
    {
        fprintf(stderr, "No pin defined for CLK\n");
	return;
    }
    if ((dout = filedescriptor("DOUT")) == -1)
    {
        fprintf(stderr, "No pin defined for DOUT\n");
	return;
    }

    mask = (direction == LSB_FIRST) ? 1 : (1 << (bits - 1));

    for (; bits > 0; bits--)
    {
	if ((value & mask))
	    write(dout, "1", 1);
	else
	    write(dout, "0", 1);

	write(clk, "1", 1);
	nsleep(40);
	write(clk, "0", 1);
	nsleep(40);

	if (direction == LSB_FIRST)
	    mask <<= 1;
	else
	    mask >>= 1;
    }

    return;
}

/*
 * NAME: enter_ICSP_mode
 * PURPOSE: To enter ICSP mode
 * ARGUMENTS: None
 * RETURNS: Nothing
 * NOTE: ICSP mode is entered by sending the value 0x4d434851
 *	with the MSB first
 */
static void
enter_ICSP_mode()
{
    int mclr, dout, clk;
    unsigned int mask, value;
    int i;

    if ((mclr = filedescriptor("MCLR")) == -1)
    {
        fprintf(stderr, "No pin defined for MCLR\n");
	return;
    }
    if ((clk = filedescriptor("CLK")) == -1)
    {
        fprintf(stderr, "No pin defined for CLK\n");
	return;
    }
    if ((dout = filedescriptor("DOUT")) == -1)
    {
        fprintf(stderr, "No pin defined for DOUT\n");
	return;
    }

    nsleep(100);
    write(mclr, "1", 1);
    nsleep(40);
    write(mclr, "0", 1);
    P18();
    send(0x4d434851, 32, MSB_FIRST);
    P19();
    write(mclr, "1", 1);
    usleep(25000);	/* >= 25ms */

    send(0, 5, MSB_FIRST);

    return;
}

/*
 * NAME: exit_ICSP_mode
 * PURPOSE: To leave ICSP mode
 * ARGUMENTS: None
 * RETURNS: Nothing
 * NOTE: ICSP is left by first pulling DOUT and CLK to 0
 *	and then MCLK to 0
 */
static void
exit_ICSP_mode()
{
    int clk, dout, mclr;
    
    if ((clk = filedescriptor("CLK")) == -1)
    {
        fprintf(stderr, "No pin defined for CLK\n");
	return;
    }
    if ((dout = filedescriptor("DOUT")) == -1)
    {
        fprintf(stderr, "No pin defined for DOUT\n");
	return;
    }
    if ((mclr = filedescriptor("MCLR")) == -1)
    {
        fprintf(stderr, "No pin defined for MCLR\n");
	return;
    }

    write(dout, "0", 1);
    write(clk, "0", 1);
    P16();
    write(mclr, "0", 1);
    sleep(1);
    write(mclr, "1", 1);

}

/*
 * NAME: SIX
 * PURPOSE: To execute a SIX serial instruction
 * ARGUMENTS: instr: instruction to execute
 * RETURNS: Nothing
 */
static void
SIX(unsigned int instr)
{
    if (debug || trace_SIX) fprintf(stderr, "SIX(%06X)\n", instr);

    send(0, 4, MSB_FIRST);	/* 0b0000 -> 4-bit control code */
    P4();
    send(instr, 24, LSB_FIRST);
}

/*
 * NAME: REGOUT
 * PURPOSE: To execute a REGOUT serial instruction
 * ARGUMENTS: None
 * RETURNS: Contents of VISI register
 */
static unsigned int
REGOUT()
{
    int clk, din;
    int i;
    unsigned int visi, mask;

    if (debug) fprintf(stderr, "<VISI>\n");
    if ((clk = filedescriptor("CLK")) == -1)
    {
        fprintf(stderr, "No pin defined for CLK\n");
	return 0;
    }
    if ((din = filedescriptor("DIN")) == -1)
    {
        fprintf(stderr, "No pin defined for DIN\n");
	return 0;
    }

    send(8, 4, MSB_FIRST);	/* 0b1000 -> REGOUT */
    P4();
    send(0, 8, MSB_FIRST);
    P5();
    for (visi = 0, mask = 1, i = 0; i < 16; i++, mask <<= 1)
    {
	char c;

	write(clk, "1", 1);
	read(din, &c, 1);
        if (c - '0')
	    visi |= mask;
	lseek(din, 0, SEEK_SET);
	P1A();
	write(clk, "0", 1);
	P1B();
    }

    if (debug) fprintf(stderr, "REGOUT returns %04x\n", visi);
    return visi;
}

/*
 * NAME: NOP
 * PURPOSE: To emit a NOP instruction
 * ARGUMENTS: None
 * RETURNS: Nothing
 */
static inline void
NOP()
{
    SIX(0x000000);	/* NOP */
}

/*
 * NAME: MOV_lit16_to_Wnd
 * PURPOSE: To emit a "Move 16-bit Literal to Wnd" instruction
 * ARGUMENTS: li16: 16 bit immediate value (0..65534/MOV)
 *	Wnd: Destination Working register number (0..15)
 * RETURNS: Nothing
 */
static inline void
MOV_lit16_to_Wnd(uint16_t lit16, int Wnd)
{
    SIX(0x200000 | (lit16 << 4) | (Wnd & 0xf));
}

static inline void
MOV_f_to_Wnd(unsigned int f, unsigned int Wnd)
{
    SIX(0x800000 | ((f & 0xfffe) << 3) | (Wnd & 0xf));
}

static inline void
MOV_Wns_to_f(unsigned int Wns, unsigned int f)
{
    SIX(0x880000 | ((f & 0xfffe) << 3) | (Wns & 0xf));
}

static inline void
CLR(unsigned int Wd)
{
    SIX(0xEB0000
        | (REGISTER_DIRECT << 11)	/* destination address mode */
	| ((Wd & 0x0f) << 7));
}

static inline void
GOTO(unsigned int addr)
{
    SIX(0x040000 | (addr & 0xfffe));
    SIX(0x000000 | ((addr >> 16) & 0x7f));
}

static inline void
BSET(unsigned int f, unsigned int bit4)
{
    SIX(0xA80000
        | ((bit4 & 0xe) << 12)
	| (f & 0xffe)
	| (bit4 & 0x01));
}

# if 0 /* { */
/*
 * This is just a test ... parsing text is not really needed
 * as the instructions are pretty fixed.
 * So encoding everything inline is better than parsing stuff.
 */
/*
 * Wn
 * [Wn]
 * [Wn++]
 * [Wn--]
 * [++Wn]
 * [--Wn]
 * [Wn+Wd]
 */
static int
parse_operand(const char *op, unsigned int *mode, unsigned int *reg)
{
    char *next;

    if (*op == 'W')	/* Wn */
    {
	op++;
        *mode = REGISTER_DIRECT;
	*reg = strtoul(op, &next, 10);
	return (next != op) && (*next == '\0');
    }
    if (*op++ != '[')
        return 0;
    if (op[0] == '+' && op[1] == '+' && op[2] == 'W')	/* [++Wn] */
    {
	op += 3;
        *mode = INDIRECT_PRE_INCREMENT;
	*reg = strtoul(op, &next, 10);
	return (next != op) && (strcmp(next, "]") == 0);
    }
    else if (op[0] == '-' && op[1] == '-' && op[2] == 'W')	/* [--Wn] */
    {
	op += 3;
        *mode = INDIRECT_PRE_DECREMENT;
	*reg = strtoul(op, &next, 10);
	return (next != op) && (strcmp(next, "]") == 0);
    }
    else if (op[0] == 'W')	/* [Wn] [Wn++] [Wn--] [Wn+Wd] */
    {
        op++;
	*reg = strtoul(op, &next, 10);
	if (next == op || strchr("]+-", *next) == NULL)
	    return 0;
	if (strcmp(next, "]") == 0)
	    *mode = INDIRECT;
	else if (strcmp(next, "++]") == 0)
	    *mode = INDIRECT_POST_INCREMENT;
	else if (strcmp(next, "--]") == 0)
	    *mode = INDIRECT_POST_DECREMENT;
	else
	    return 0;
	return 1;
    }

    return 0;
}
# endif /* } */

static inline void
TBLRDL(char b, unsigned int ppp, unsigned int Ws, unsigned int qqq, unsigned int Wd)
{
    SIX(0xBA0000
          | ((b == 'B') << 14)	/* B */
	  | ((qqq & 0x7) << 11)	/* destination Address mode */
	  | ((Wd & 0xf) << 7)	/* destination register */
	  | ((ppp & 0x7) << 4)	/* source Address mode */
	  | ((Ws & 0xf) << 0));	/* source register */
}

static inline void
TBLRDH(char b, unsigned int ppp, unsigned int Ws, unsigned int qqq, unsigned int Wd)
{
    SIX(0xBA8000
       | ((b == 'B') << 14)
       | ((qqq & 0x7) << 11)	/* destination Address mode */
       | ((Wd & 0xf) << 7)	/* destination register */
       | ((ppp & 0x7) << 4)	/* source Address mode */
       | ((Ws & 0xf) << 0));	/* source register */
}

static inline void
TBLWTL(char b, unsigned int ppp, unsigned int Ws, unsigned int qqq, unsigned int Wd)
{
    SIX(0xBB0000
       | ((b == 'B') << 14)
       | ((qqq & 0x7) << 11)	/* destination Address mode */
       | ((Wd & 0xf) << 7)	/* destination register */
       | ((ppp & 0x7) << 4)	/* source Address mode */
       | ((Ws & 0xf) << 0));	/* source register */
}

static inline void
TBLWTH(char b, unsigned int ppp, unsigned int Ws, unsigned int qqq, unsigned int Wd)
{
    SIX(0xBB8000
       | ((b == 'B') << 14)
       | ((qqq & 0x7) << 11)	/* destination Address mode */
       | ((Wd & 0xf) << 7)	/* destination register */
       | ((ppp & 0x7) << 4)	/* source Address mode */
       | ((Ws & 0xf) << 0));	/* source register */
}

/*
 * NAME: exit_reset_vector
 * PURPOSE: To cause the microcontroller to leave the reset state
 * ARGUMENTS: None
 * RETURNS: Nothing
 */
static inline void
exit_reset_vector()
{
    NOP();
    GOTO(0x200);
}

/*
 * NAME: init_TBLPAG_Wn
 * PURPOSE: To initialize the Read/Write Pointer (Wn) for TBLWT instruction.
 * ARGUMENTS: addr: address to set pointer to
 *	n: register number to use as the pointer
 * RETURNS: Nothing
 * NOTE: The upper 8 bits of the address are stored in the TBLPAG register
 *	The lower 16 bits are stored in Wn
 */
static inline void
init_TBLPAG_Wn(uint32_t addr, unsigned int n)
{
    MOV_lit16_to_Wnd(addr >> 16, W0);
    MOV_Wns_to_f(W0, TBLPAG);
    MOV_lit16_to_Wnd(addr & 0xffff, n);
}

/*
 * NAME init_Wn_to_VISI
 * PURPOSE: To initialize a working register with the address of the VISI register
 * ARGUMENTS: n: working register number (usually W7)
 * RETURNS: Nothing
 */
static inline void
init_Wn_to_VISI(unsigned int n)
{
    MOV_lit16_to_Wnd(0x0784, n);
    NOP();
}

/*
 * NAME: reset_device_internal_PC
 * PURPOSE: To reset the PC to a safe value
 * ARGUMENTS: None
 * RETURNS: Nothing
 */
static inline void
reset_device_internal_PC()
{
    GOTO(0x200);
}

static unsigned char *
read_code_memory_bytes(unsigned int start, unsigned int nwords)
{
    unsigned char *result;
    int i;

    nwords = (nwords + 3) & ~3;

    result = calloc(nwords, sizeof(uint32_t));
    if (result == NULL)
        return result;

    /* Step 1; Exit Reset vector */
    exit_reset_vector();

    /* Step 2: Initialize TBLPAG and the Read Pointer (W6) for TBLRD instruction. */
    init_TBLPAG_Wn(start, W6);

    /* Step 3: Initialize the Write Pointer (W7) to point to the VISI register. */
    init_Wn_to_VISI(W7);

    for (i = 0; i < nwords; i += 2)
    {
	unsigned short v;

	/* Step 4: Read and clock out the contents of the next two locations of code memory */
	/*
	 * 1011 1010 0000 1011 1001 0110
	 *             qq qddd dppp ssss
	 */
	TBLRDL('W', INDIRECT, W6, INDIRECT, W7);
	NOP();
	NOP();
	v = REGOUT();	/* |      LS.W0      | */
	result[4*i] = v & 0xff;
	result[4*i+1] = (v >> 8) & 0xff;
	NOP();
	TBLRDH('B', INDIRECT_POST_INCREMENT, W6, INDIRECT_POST_INCREMENT, W7);
	NOP();
	NOP();
	TBLRDH('B', INDIRECT_PRE_INCREMENT, W6, INDIRECT_POST_DECREMENT, W7);
	NOP();
	NOP();
	v = REGOUT();	/* |  MSB1  |  MSB0  | */
	result[4*i+2] |= v & 0xff;
	result[4*i+3] = 0x00;
	result[4*i+6] = (v >> 8) & 0xff;
	NOP();
	TBLRDL('W', INDIRECT_POST_INCREMENT, W6, INDIRECT, W7);
	NOP();
	NOP();
	v = REGOUT();	/* |      LW.W1      | */
	result[4*i+4] = v & 0xff;
	result[4*i+5] = (v >> 8) & 0xff;
	result[4*i+7] = 0x00;
	NOP();

	/* Step 5: Reset device internal PC. */
	reset_device_internal_PC();
    }

    return result;
}
/*
 * NAME: read_code_memory
 * PURPOSE: To read code memory
 * ARGUMENTS: start: first (byte) address to read
 *	nwords: number of words to read
 * RETURNS: dynamically allocated memory with code words
 * NOTE: The most significant byte of each word is 00
 */
static uint32_t *
read_code_memory(unsigned int start, unsigned int nwords)
{
    uint32_t *result;
    int i;

    if (nwords & 0x01)
        nwords++;

    result = calloc(nwords, sizeof(uint32_t));
    if (result == NULL)
        return result;

    /* Step 1; Exit Reset vector */
    exit_reset_vector();

    /* Step 2: Initialize TBLPAG and the Read Pointer (W6) for TBLRD instruction. */
    init_TBLPAG_Wn(start, W6);

    /* Step 3: Initialize the Write Pointer (W7) to point to the VISI register. */
    init_Wn_to_VISI(W7);

    for (i = 0; i < nwords; i += 2)
    {
	unsigned short v;

	/* Step 4: Read and clock out the contents of the next two locations of code memory */
	TBLRDL('W', INDIRECT, W6, INDIRECT, W7);
	NOP();
	NOP();
	v = REGOUT();	/* |      LS.W0      | */
	result[i] = v;
	NOP();
	TBLRDH('B', INDIRECT_POST_INCREMENT, W6, INDIRECT_POST_INCREMENT, W7);
	NOP();
	NOP();
	TBLRDH('B', INDIRECT_PRE_INCREMENT, W6, INDIRECT_POST_DECREMENT, W7);
	NOP();
	NOP();
	v = REGOUT();	/* |  MSB1  |  MSB0  | */
	result[i] |= (v << 16) & 0xff0000;
	result[i+1] = (v << 8) & 0xff0000;
	NOP();
	TBLRDL('W', INDIRECT_POST_INCREMENT, W6, INDIRECT, W7);
	NOP();
	NOP();
	v = REGOUT();	/* |      LW.W1      | */
	result[i+1] |= v;
	NOP();

	/* Step 5: Reset device internal PC. */
	reset_device_internal_PC();
    }

    return result;
}

/*
 * NAME: read_configuration_memory
 * PURPOSE: To read the two flash config words
 * ARGUMENTS: addr: address of CW2
 *	cw1p: address where to store CW1
 *	cw2p: address where to store CW2
 * RETURNS: Nothing
 * NOTE: The values read are returned in *cw0 and *cw1
 */
static void
read_configuration_memory(uint32_t addr, uint16_t *cw1p, uint16_t *cw2p)
{
    uint16_t v;

    /* Step 1: exit Reset vector. */
    exit_reset_vector();

    /* Step 2: Initialize TBLPAG, the Read Pointer (W6) and the Write Pointer (W7) for TBLRD instruction. */
    init_TBLPAG_Wn(addr, W6);
    init_Wn_to_VISI(W7);

    /*
     * Step 3: Read the Configuration register and write it to the VISI register (located at 784h), and clock out the
     * VISI register using the REGOUT command.
     */
    TBLRDL('W', INDIRECT_POST_INCREMENT, W6, INDIRECT, W7);
    NOP();
    NOP();
    v = REGOUT();
    if (cw2p != NULL)
        *cw2p = v;
    NOP();

    /* Step 4: Repeat Step 3 again to read Configuration Word 1. */
    TBLRDL('W', INDIRECT_POST_INCREMENT, W6, INDIRECT, W7);
    NOP();
    NOP();
    v = REGOUT();
    if (cw1p != NULL)
        *cw1p = v;
    NOP();

    /* Step 5: Reset device internal PC. */
    reset_device_internal_PC();
}

/*
 * NAME: read_application_ID
 * PURPOSE: To read the application ID
 * ARGUMENTS: None
 * RETURNED application ID
 */
static uint16_t
read_application_ID()
{
    uint16_t application_ID;

    /* Step 1: exit Reset vector. */
    exit_reset_vector();

    /* Step 2: Initialize TBLPAG and the Read Pointer (W0) for TBLRD instruction. */
    MOV_lit16_to_Wnd(0x80, W0);
    MOV_Wns_to_f(W0, TBLPAG);
    MOV_lit16_to_Wnd(0x05BE, W0);
    MOV_lit16_to_Wnd(VISI, W1);
    NOP();
    TBLRDL('W', INDIRECT, W0, INDIRECT, W1);
    NOP();
    NOP();

    /* Step 3: Output the VISI register using the REGOUT command. */
    application_ID = REGOUT();
    NOP();

    return application_ID;
}

/*
 * NAME: initiate_write_cycle
 * PURPOSE: To initiate a WRITE cycle by setting the WR bit in the NVMON register
 * ARGUMENTS: None
 * RETURNS: Nothing
 */
static inline void
initiate_write_cycle()
{
    BSET(NVMCON,NVMCON_WR_bit);
    NOP();
    NOP();
}

/*
 * NAME: waitfor_NVMCON_WR_clr
 * PURPOSE: To wait until the hardware has cleared the WR bit in the NVMON register
 * ARGUMENTS: None
 * RETURNS: Nothing
 * NOTE: This is a busy wait
 */
static inline void
waitfor_NVMCON_WR_clr()
{
    uint16_t nvmcon;

    do {
        GOTO(0x200);
	MOV_f_to_Wnd(NVMCON, W2);
	MOV_Wns_to_f(W2, VISI);
	NOP();
	nvmcon = REGOUT();
	NOP();
	if (debug)
	    fprintf(stderr, "%04x\n", nvmcon);
    } while (nvmcon & (1 << NVMCON_WR_bit));

    return;
}

/*
 * NAME: chip_erase
 * PURPOSE: To erase the chip's NVRAM
 * ARGUMENTS: None
 * RETURNS: Nothing
 */
static void
chip_erase()
{
    /* Step 1: exit Reset vector. */
    exit_reset_vector();

    /* Step 2: Set the NVMCON to erase all program memory */
    MOV_lit16_to_Wnd(0x404F, W10);
    MOV_Wns_to_f(W10, NVMCON);

    /* Step 3: Set TBLPAG and perform dummy table write to select what portions of memory are erased. */
    MOV_lit16_to_Wnd(0x0000, W0);
    MOV_Wns_to_f(W0, TBLPAG);
    MOV_lit16_to_Wnd(0x0000, W0);
    TBLWTL('W', REGISTER_DIRECT, W0, INDIRECT, W0);
    NOP();
    NOP();

    /* Step 4: Initiate the erase cycle. */
    initiate_write_cycle();

    /* Step 5: Repeat this step to poll the WR bit (bit 15 of NVMCON) until it is cleared by the hardware. */
    waitfor_NVMCON_WR_clr();

    return;
}

/*
 * NAME: write_code_memory
 * PURPOSE: To write the next 64 instruction words
 * ARGUMENTS: byte_addr: start of memory area
 *	b: address of instruction bytes to write
 * RETURNS: 0 on failure, non-0 on success
 */
static int
write_code_memory(unsigned int byte_addr, unsigned char *b)
{
    int i;

    /* Step 1: exit Reset vector. */
    exit_reset_vector();
    
    /* Step 2: Set the NVMCON to program 64 instruction words. */
    MOV_lit16_to_Wnd(0x4001, W10);
    MOV_Wns_to_f(W10, NVMCON);


    /* Step 3: Initialize the Write Pointer (W7) for TBLWT instruction. */
    init_TBLPAG_Wn(byte_addr / 2, W7);

    for (i = 0; i < 16; i++)
    {
	int j;

	/* Step 4: Load W0:W5 with the next 4 instruction words to program. */
	MOV_lit16_to_Wnd((b[1] << 8) | b[0], W0);
	MOV_lit16_to_Wnd((b[6] << 8) | b[2], W1);
	MOV_lit16_to_Wnd((b[5] << 8) | b[4], W2);
	b += 8;
	MOV_lit16_to_Wnd((b[1] << 8) | b[0], W3);
	MOV_lit16_to_Wnd((b[6] << 8) | b[2], W4);
	MOV_lit16_to_Wnd((b[5] << 8) | b[4], W5);
	b += 8;
	
	/* Step 5: Set the Read Pointer (W6) and load the (next set of) write latches. */
	CLR(W6);
	NOP();
	TBLWTL('W', INDIRECT_POST_INCREMENT, W6, INDIRECT, W7);
	NOP();
	NOP();
	TBLWTH('B', INDIRECT_POST_INCREMENT, W6, INDIRECT_POST_INCREMENT, W7);
	NOP();
	NOP();
	TBLWTH('B', INDIRECT_POST_INCREMENT, W6, INDIRECT_PRE_INCREMENT, W7);
	NOP();
	NOP();
	TBLWTL('W', INDIRECT_POST_INCREMENT, W6, INDIRECT_POST_INCREMENT, W7);
	NOP();
	NOP();
	TBLWTL('W', INDIRECT_POST_INCREMENT, W6, INDIRECT, W7);
	NOP();
	NOP();
	TBLWTH('B', INDIRECT_POST_INCREMENT, W6, INDIRECT_POST_INCREMENT, W7);
	NOP();
	NOP();
	TBLWTH('B', INDIRECT_POST_INCREMENT, W6, INDIRECT_PRE_INCREMENT, W7);
	NOP();
	NOP();
	TBLWTL('W', INDIRECT_POST_INCREMENT, W6, INDIRECT_POST_INCREMENT, W7);
	NOP();
	NOP();
    } /* Step 6: Repeat Steps 4 and 5, sixteen times, to load the write latches for 64 instructions. */

    /* Step 7: Initiate the write cycle. */
    initiate_write_cycle();

    /* Step 8: Repeat this step to poll the WR bit (bit 15 of NVMCON) until it is cleared by the hardware. */
    waitfor_NVMCON_WR_clr();

    /* Step 9: Reset device internal PC. */
    reset_device_internal_PC();

    return 1;
}

/*
 * NAME: write_configuration_registers
 * PURPOSE: To write the two configuration register words
 * ARGUMENTS: addr: WORD ADDRESS of the first word (CW2)
 *	cw1, cw2: configuration register words to write
 * RETURNS: Nothing
 * NOTE: CW2 is at the lower memory address, CW1 is at the higher memory address
 */
static void
write_configuration_registers(unsigned int addr, uint16_t cw1, uint16_t cw2)
{
    /* Step 1: exit Reset vector. */
    exit_reset_vector();
    
    /* Step 2: Initialize the Write Pointer (W7) for the TBLWT instruction. */
    /* NOTE: The address is a WORD ADDRESS! */
    MOV_lit16_to_Wnd(addr & 0xffff, W7);

    /* Step 3: Set the NVMCON register to program CW2. */
    /*
     * 0x4003: 01000000 00000011
     *   15: WR:    0
     *   14: WREN:  1
     *   13: WRERR: 0
     *    6: ERASE: 0
     * 0..3: NVMOP: Memory word program operation (as ERASE = 0)
     */
    MOV_lit16_to_Wnd(0x4003, W10);
    MOV_Wns_to_f(W10, NVMCON);

    /* Step 4: Initialize the TBLPAG register. */
    MOV_lit16_to_Wnd((addr >> 16) & 0xffff, W0);
    MOV_Wns_to_f(W0, TBLPAG);

    /* Step 5: Load the Configuration register data to W6. */
    MOV_lit16_to_Wnd(cw2, W6);

    /* Step 6: Write the Configuration register data to the write latch and increment the Write Pointer. */
    NOP();
    TBLWTL('W', REGISTER_DIRECT, W6, INDIRECT_POST_INCREMENT, W7);
    NOP();
    NOP();

    /* Step 7: Initiate the write cycle. */
    initiate_write_cycle();

    /* Step 8: Repeat this step to poll the WR bit (bit 15 of NVMCON) until it is cleared by the hardware. */
    waitfor_NVMCON_WR_clr();

    /* Step 9: Reset device internal PC. */
    reset_device_internal_PC();

    /* Step 10: Repeat Steps 5-9 to write CW1. */
    /* Step 5: Load the Configuration register data to W6. */
    MOV_lit16_to_Wnd(cw1, W6);

    /* Step 6: Write the Configuration register data to the write latch and increment the Write Pointer. */
    NOP();
    TBLWTL('W', REGISTER_DIRECT, W6, INDIRECT_POST_INCREMENT, W7);
    NOP();
    NOP();

    /* Step 7: Initiate the write cycle. */
    initiate_write_cycle();

    /* Step 8: Repeat this step to poll the WR bit (bit 15 of NVMCON) until it is cleared by the hardware. */
    waitfor_NVMCON_WR_clr();

    /* Step 9: Reset device internal PC. */
    reset_device_internal_PC();

    return;
}
