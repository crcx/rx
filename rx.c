/*---------------------------------------------------------------------
  RETRO is a clean, elegant, and pragmatic dialect of Forth. It
  provides a simple alternative for those willing to make a break
  from legacy systems.

  Copyright (c) 2008 - 2021, Charles Childers

  The virtual machine is based on the C implementation of Ngaro and
  RETRO11, which were also copyrighted by Luke Parrish, Mark Simpson,
  Jay Skeer, and Kenneth Keating.
  ---------------------------------------------------------------------*/

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <limits.h>
#include <fcntl.h>

#define CELL int32_t
#define CELL_MIN INT_MIN + 1
#define CELL_MAX INT_MAX - 1

#define IMAGE_SIZE   2000000      /* Amount of RAM, in cells           */
#define ADDRESSES    128000       /* Depth of address stack            */
#define STACK_DEPTH  128000       /* Depth of data stack               */

#define TIB            1024       /* Location of TIB                   */

#define D_OFFSET_LINK     0       /* Dictionary Format Info. Update if */
#define D_OFFSET_XT       1       /* you change the dictionary fields. */
#define D_OFFSET_CLASS    2
#define D_OFFSET_NAME     3

#define NUM_DEVICES       6       /* Set the number of I/O devices     */

#define MAX_OPEN_FILES   32

CELL stack_pop();
void stack_push(CELL value);
CELL string_inject(char *str, CELL buffer);
char *string_extract(CELL at);
CELL d_xt_for(char *Name, CELL Dictionary);
void update_rx();
void include_file(char *fname);
void io_output_handler();
void io_output_query();
void io_keyboard_handler();
void io_keyboard_query();
void io_filesystem_query();
void io_filesystem_handler();
void io_unix_query();
void io_unix_handler();
void io_scripting_handler();
void io_scripting_query();
void io_random();
void io_random_query();

CELL load_image();
void prepare_vm();
void process_opcode(CELL opcode);
void process_opcode_bundle(CELL opcode);
int validate_opcode_bundle(CELL opcode);

CELL sp, rp, ip;                  /* Stack & instruction pointers      */
CELL data[STACK_DEPTH];           /* The data stack                    */
CELL address[ADDRESSES];          /* The address stack                 */
CELL memory[IMAGE_SIZE + 1];      /* The memory for the image          */

#define TOS  data[sp]             /* Shortcut for top item on stack    */
#define NOS  data[sp-1]           /* Shortcut for second item on stack */
#define TORS address[rp]          /* Shortcut for top item on address stack */

typedef void (*Handler)(void);

Handler IO_deviceHandlers[] = {
  io_output_handler,
  io_keyboard_handler,
  io_filesystem_handler,
  io_unix_handler,
  io_scripting_handler,
  io_random,
};

Handler IO_queryHandlers[] = {
  io_output_query,
  io_keyboard_query,
  io_filesystem_query,
  io_unix_query,
  io_scripting_query,
  io_random_query,
};

CELL Dictionary;
CELL interpret;

char string_data[8192];
char **sys_argv;
int sys_argc;

FILE *OpenFileHandles[MAX_OPEN_FILES];

CELL files_get_handle() {
  CELL i;
  for(i = 1; i < MAX_OPEN_FILES; i++)
    if (OpenFileHandles[i] == 0)
      return i;
  return 0;
}

void file_open() {
  CELL slot, mode, name;
  char *request;
  slot = files_get_handle();
  mode = stack_pop();
  name = stack_pop();
  request = string_extract(name);
  if (slot > 0) {
    if (mode == 0)  OpenFileHandles[slot] = fopen(request, "rb");
    if (mode == 1)  OpenFileHandles[slot] = fopen(request, "w");
    if (mode == 2)  OpenFileHandles[slot] = fopen(request, "a");
    if (mode == 3)  OpenFileHandles[slot] = fopen(request, "rb+");
  }
  if (OpenFileHandles[slot] == NULL) {
    OpenFileHandles[slot] = 0;
    slot = 0;
  }
  stack_push(slot);
}

void file_read() {
  CELL c;
  CELL slot = stack_pop();
  c = fgetc(OpenFileHandles[slot]);
  stack_push(feof(OpenFileHandles[slot]) ? 0 : c);
}

void file_write() {
  CELL slot, c, r;
  slot = stack_pop();
  c = stack_pop();
  r = fputc(c, OpenFileHandles[slot]);
}

void file_close() {
  CELL slot = stack_pop();
  fclose(OpenFileHandles[slot]);
  OpenFileHandles[slot] = 0;
}

void file_get_position() {
  CELL slot = stack_pop();
  stack_push((CELL) ftell(OpenFileHandles[slot]));
}

void file_set_position() {
  CELL slot, pos;
  slot = stack_pop();
  pos  = stack_pop();
  fseek(OpenFileHandles[slot], pos, SEEK_SET);
}

void file_get_size() {
  CELL slot, current, r, size;
  struct stat buffer;
  slot = stack_pop();
  fstat(fileno(OpenFileHandles[slot]), &buffer);
  if (!S_ISDIR(buffer.st_mode)) {
    current = ftell(OpenFileHandles[slot]);
    r = fseek(OpenFileHandles[slot], 0, SEEK_END);
    size = ftell(OpenFileHandles[slot]);
    fseek(OpenFileHandles[slot], current, SEEK_SET);
  } else {
    r = -1;
    size = 0;
  }
  stack_push((r == 0) ? size : 0);
}

void file_delete() {
  char *request;
  CELL name = stack_pop();
  request = string_extract(name);
  unlink(request);
}

void file_flush() {
  CELL slot;
  slot = stack_pop();
  fflush(OpenFileHandles[slot]);
}

Handler FileActions[10] = {
  file_open,
  file_close,
  file_read,
  file_write,
  file_get_position,
  file_set_position,
  file_get_size,
  file_delete,
  file_flush
};

void io_filesystem_query() {
  stack_push(0);
  stack_push(4);
}

void io_filesystem_handler() {
  FileActions[stack_pop()]();
}

void unix_open_pipe() {
  CELL slot, mode, name;
  char *request;
  slot = files_get_handle();
  mode = stack_pop();
  name = stack_pop();
  request = string_extract(name);
  if (slot > 0) {
    if (mode == 0)  OpenFileHandles[slot] = popen(request, "r");
    if (mode == 1)  OpenFileHandles[slot] = popen(request, "w");
    if (mode == 3)  OpenFileHandles[slot] = popen(request, "r+");
  }
  if (OpenFileHandles[slot] == NULL) {
    OpenFileHandles[slot] = 0;
    slot = 0;
  }
  stack_push(slot);
}

void unix_close_pipe() {
  pclose(OpenFileHandles[TOS]);
  OpenFileHandles[TOS] = 0;
  sp--;
}

void unix_system() {
  system(string_extract(stack_pop()));
}

void unix_chdir() {
  chdir(string_extract(stack_pop()));
}

void unix_getenv() {
  CELL a, b;
  a = stack_pop();
  b = stack_pop();
  string_inject(getenv(string_extract(b)), a);
}

void unix_putenv() {
  putenv(string_extract(stack_pop()));
}

Handler UnixActions[] = {
  unix_system, unix_open_pipe, unix_close_pipe,
  unix_chdir,  unix_getenv,    unix_putenv,
};

void io_unix_query() {
  stack_push(1);
  stack_push(8);
}

void io_unix_handler() {
  UnixActions[stack_pop()]();
}

void io_random() {
  int64_t r = 0;
  char buffer[8];
  int i;
  int fd = open("/dev/urandom", O_RDONLY);
  read(fd, buffer, 8);
  close(fd);
  for(i = 0; i < 8; ++i) {
    r = r << 8;
    r += ((int64_t)buffer[i] & 0xFF);
  }
  stack_push(llabs(r));
}

void io_random_query() {
  stack_push(0);
  stack_push(10);
}

void io_output_handler() {
  putc(stack_pop(), stdout);
  fflush(stdout);
}

void io_output_query() {
  stack_push(0);
  stack_push(0);
}

void io_keyboard_handler() {
  stack_push(getc(stdin));
  if (TOS == 127) TOS = 8;
}

void io_keyboard_query() {
  stack_push(0);
  stack_push(1);
}

void scripting_arg() {
  CELL a, b;
  a = stack_pop();
  b = stack_pop();
  stack_push(string_inject(sys_argv[a + 2], b));
}

void scripting_arg_count() {
  stack_push(sys_argc - 2);
}

void scripting_include() {
  include_file(string_extract(stack_pop()));
}

void scripting_name() {
  stack_push(string_inject(sys_argv[1], stack_pop()));
}

Handler ScriptingActions[] = {
  scripting_arg_count,
  scripting_arg,
  scripting_include,
  scripting_name,
};

void io_scripting_query() {
  stack_push(2);
  stack_push(9);
}

void io_scripting_handler() {
  ScriptingActions[stack_pop()]();
}

void execute(CELL cell) {
  CELL a, b, i;
  CELL opcode;
  if (rp == 0) {
    rp = 1;
  }
  ip = cell;
  while (ip < IMAGE_SIZE) {
    opcode = memory[ip];
    if (validate_opcode_bundle(opcode) != 0) {
      process_opcode_bundle(opcode);
    } else {
      printf("\nERROR (nga/execute): Invalid instruction!\n");
      printf("At %d, opcode %d\n", ip, opcode);
      printf("Instructions: ");
      a = opcode;
      for (i = 0; i < 4; i++) {
        b = a & 0xFF;
        printf("%d ", b);
        a = a >> 8;
      }
      printf("\n");
      exit(1);
    }
    ip++;
    if (rp == 0) {
      ip = IMAGE_SIZE;
    }
  }
}

void evaluate(char *s) {
  if (strlen(s) == 0) return;
  string_inject(s, TIB);
  stack_push(TIB);
  execute(interpret);
}

int not_eol(int c) {
  return (c != 10) && (c != 13) && (c != 32) && (c != EOF) && (c != 0);
}

void read_token(FILE *file, char *token_buffer, int echo) {
  int ch = getc(file);
  int count = 0;
  if (echo != 0)
    putchar(ch);
  while (not_eol(ch)) {
    if ((ch == 8 || ch == 127) && count > 0) {
      count--;
      if (echo != 0) {
        putchar(8);
        putchar(32);
        putchar(8);
      }
    } else {
      token_buffer[count++] = ch;
    }
    ch = getc(file);
    if (echo != 0)
      putchar(ch);
  }
  token_buffer[count] = '\0';
}

void dump_stack() {
  CELL i;
  if (sp == 0)  return;
  printf("\nStack: ");
  for (i = 1; i <= sp; i++) {
    if (i == sp)
      printf("[ TOS: %d ]", data[i]);
    else
      printf("%d ", data[i]);
  }
  printf("\n");
}

int fence_boundary(char *buffer) {
  int flag = 1;
  if (buffer[0] == '~' && buffer[1] == '~' && buffer[2] == '~') { flag = -1; }
  return flag;
}

void read_line(FILE *file, char *token_buffer) {
  int ch = getc(file);
  int count = 0;
  while ((ch != 10) && (ch != 13) && (ch != EOF) && (ch != 0)) {
    token_buffer[count++] = ch;
    ch = getc(file);
  }
  token_buffer[count] = '\0';
}

int count_tokens(char *line) {
  char ch = line[0];
  int count = 1;
  while (*line++) {
    ch = line[0];
    if (isspace(ch))
      count++;
  }
  return count;
}

void include_file(char *fname) {
  int inBlock = 0;                 /* Tracks status of in/out of block */
  char source[64 * 1024];          /* Token buffer [about 64K]         */
  char line[64 * 1024];            /* Line buffer [about 64K]          */
  char fence[33];                  /* Used with `fence_boundary()`     */

  long offset = 0;
  int tokens = 0;
  CELL ReturnStack[ADDRESSES];
  CELL arp, aip;

  FILE *fp;                        /* Open the file. If not found,     */
  fp = fopen(fname, "r");          /* exit.                            */
  if (fp == NULL)
    return;

  arp = rp;
  aip = ip;
  for(rp = 0; rp <= arp; rp++)
    ReturnStack[rp] = address[rp];
  rp = 0;

  while (!feof(fp)) { /* Loop through the file   */

    offset = ftell(fp);
    read_line(fp, line);
    fseek(fp, offset, SEEK_SET);

    tokens = count_tokens(line);

    while (tokens > 0) {
      tokens--;
      read_token(fp, source, 0);
      strlcpy(fence, source, 32); /* Copy the first three characters  */
      if (fence_boundary(fence) == -1) {
        if (inBlock == 0)
          inBlock = 1;
        else
          inBlock = 0;
      } else {
        if (inBlock == 1) {
          evaluate(source);
        }
      }
    }
  }
  fclose(fp);

  for(rp = 0; rp <= arp; rp++)
    address[rp] = ReturnStack[rp];
  rp = arp;
  ip = aip;
}

void initialize() {
  prepare_vm();
  load_image();
}

int arg_is(char *argv, char *t) {
  return strcmp(argv, t) == 0;
}

int main(int argc, char **argv) {
  int i, fsp;
  int modes[16];
  char *files[16];

  initialize();                           /* Initialize Nga & image    */
  update_rx();

  /* Setup variables related to the scripting device */
  sys_argc = argc;                        /* Point the global argc and */
  sys_argv = argv;                        /* argv to the actual ones   */

  include_file(argv[0]);

  if (argc >= 2 && argv[1][0] != '-') {
    include_file(argv[1]);                /* If no flags were passed,  */
    if (sp >= 1)  dump_stack();           /* load the file specified,  */
    exit(0);                              /* and exit                  */
  }

  /* Clear startup modes       */
  for (i = 0; i < 16; i++)
    modes[i] = 0;

  /* Clear startup files       */
  for (i = 0; i < 16; i++)
    files[i] = "\0";
  fsp = 0;

  /* Process Arguments */
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-f") == 0) {
      files[fsp] = argv[i + 1];
      fsp++;
      i++;
    }
  }

  /* Include Startup Files */
  for (i = 0; i < fsp; i++) {
    if (strcmp(files[i], "\0") != 0)
      include_file(files[i]);
  }
}

CELL stack_pop() {
  sp--;
  return data[sp + 1];
}

void stack_push(CELL value) {
  sp++;
  data[sp] = value;
}

CELL string_inject(char *str, CELL buffer) {
  CELL m, i;
  if (!str) {
    memory[buffer] = 0;
    return 0;
  }
  m = strlen(str);
  i = 0;
  while (m > 0) {
    memory[buffer + i] = (CELL)str[i];
    memory[buffer + i + 1] = 0;
    m--; i++;
  }
  return buffer;
}

char *string_extract(CELL at) {
  CELL starting = at;
  CELL i = 0;
  while(memory[starting] && i < 8192)
    string_data[i++] = (char)memory[starting++];
  string_data[i] = 0;
  return (char *)string_data;
}

CELL d_xt(CELL dt) {
  return dt + D_OFFSET_XT;
}

CELL d_name(CELL dt) {
  return dt + D_OFFSET_NAME;
}

CELL d_lookup(CELL Dictionary, char *name) {
  CELL dt = 0;
  CELL i = Dictionary;
  char *dname;
  while (memory[i] != 0 && i != 0) {
    dname = string_extract(d_name(i));
    if (strcmp(dname, name) == 0) {
      dt = i;
      i = 0;
    } else {
      i = memory[i];
    }
  }
  return dt;
}

CELL d_xt_for(char *Name, CELL Dictionary) {
  return memory[d_xt(d_lookup(Dictionary, Name))];
}

void update_rx() {
  Dictionary = memory[2];
  interpret = d_xt_for("interpret", Dictionary);
}

/*=====================================================================*/

CELL load_image() {
  int i;
  extern CELL ngaImageCells;
  extern CELL ngaImage[];
  for (i = 0; i < ngaImageCells; i++) {
    memory[i] = ngaImage[i];
  }
  return ngaImageCells;
}

void prepare_vm() {
  ip = sp = rp = 0;
  for (ip = 0; ip < IMAGE_SIZE; ip++)
    memory[ip] = 0; /* NO - nop instruction */
  for (ip = 0; ip < STACK_DEPTH; ip++)
    data[ip] = 0;
  for (ip = 0; ip < ADDRESSES; ip++)
    address[ip] = 0;
}

void inst_no() {
}

void inst_li() {
  sp++;
  ip++;
  TOS = memory[ip];
}

void inst_du() {
  sp++;
  data[sp] = NOS;
}

void inst_dr() {
  data[sp] = 0;
   if (--sp < 0)
     ip = IMAGE_SIZE;
}

void inst_sw() {
  CELL a;
  a = TOS;
  TOS = NOS;
  NOS = a;
}

void inst_pu() {
  rp++;
  TORS = TOS;
  inst_dr();
}

void inst_po() {
  sp++;
  TOS = TORS;
  rp--;
}

void inst_ju() {
  ip = TOS - 1;
  inst_dr();
}

void inst_ca() {
  rp++;
  TORS = ip;
  ip = TOS - 1;
  inst_dr();
}

void inst_cc() {
  CELL a, b;
  a = TOS; inst_dr();  /* Target */
  b = TOS; inst_dr();  /* Flag   */
  if (b != 0) {
    rp++;
    TORS = ip;
    ip = a - 1;
  }
}

void inst_re() {
  ip = TORS;
  rp--;
}

void inst_eq() {
  NOS = (NOS == TOS) ? -1 : 0;
  inst_dr();
}

void inst_ne() {
  NOS = (NOS != TOS) ? -1 : 0;
  inst_dr();
}

void inst_lt() {
  NOS = (NOS < TOS) ? -1 : 0;
  inst_dr();
}

void inst_gt() {
  NOS = (NOS > TOS) ? -1 : 0;
  inst_dr();
}

void inst_fe() {
    switch (TOS) {
      case -1: TOS = sp - 1; break;
      case -2: TOS = rp; break;
      case -3: TOS = IMAGE_SIZE; break;
      case -4: TOS = CELL_MIN; break;
      case -5: TOS = CELL_MAX; break;
      default: TOS = memory[TOS]; break;
    }
}

void inst_st() {
    memory[TOS] = NOS;
    inst_dr();
    inst_dr();
}

void inst_ad() {
  NOS += TOS;
  inst_dr();
}

void inst_su() {
  NOS -= TOS;
  inst_dr();
}

void inst_mu() {
  NOS *= TOS;
  inst_dr();
}

void inst_di() {
  CELL a, b;
  a = TOS;
  b = NOS;
  TOS = b / a;
  NOS = b % a;
}

void inst_an() {
  NOS = TOS & NOS;
  inst_dr();
}

void inst_or() {
  NOS = TOS | NOS;
  inst_dr();
}

void inst_xo() {
  NOS = TOS ^ NOS;
  inst_dr();
}

void inst_sh() {
  CELL y = TOS;
  CELL x = NOS;
  if (TOS < 0)
    NOS = NOS << (TOS * -1);
  else {
    if (x < 0 && y > 0)
      NOS = x >> y | ~(~0U >> y);
    else
      NOS = x >> y;
  }
  inst_dr();
}

void inst_zr() {
  if (TOS == 0) {
    inst_dr();
    ip = TORS;
    rp--;
  }
}

void inst_ha() {
  ip = IMAGE_SIZE;
}

void inst_ie() {
  sp++;
  TOS = NUM_DEVICES;
}

void inst_iq() {
  CELL Device = TOS;
  inst_dr();
  IO_queryHandlers[Device]();
}

void inst_ii() {
  CELL Device = TOS;
  inst_dr();
  IO_deviceHandlers[Device]();
}

Handler instructions[] = {
  inst_no, inst_li, inst_du, inst_dr, inst_sw, inst_pu, inst_po,
  inst_ju, inst_ca, inst_cc, inst_re, inst_eq, inst_ne, inst_lt,
  inst_gt, inst_fe, inst_st, inst_ad, inst_su, inst_mu, inst_di,
  inst_an, inst_or, inst_xo, inst_sh, inst_zr, inst_ha, inst_ie,
  inst_iq, inst_ii
};

void process_opcode(CELL opcode) {
  if (opcode != 0)
    instructions[opcode]();
}

int validate_opcode_bundle(CELL opcode) {
  CELL raw = opcode;
  CELL current;
  int valid = -1;
  int i;
  for (i = 0; i < 4; i++) {
    current = raw & 0xFF;
    if (!(current >= 0 && current <= 29))
      valid = 0;
    raw = raw >> 8;
  }
  return valid;
}

void process_opcode_bundle(CELL opcode) {
  CELL raw = opcode;
  int i;
  for (i = 0; i < 4; i++) {
    process_opcode(raw & 0xFF);
    raw = raw >> 8;
  }
}

CELL ngaImageCells = 1017;
CELL ngaImage[] = { 1793,-1,1001,1536,202104,0,10,1,10,2,10,3,10,4,10,5,10,6,10,
                       7,10,8,10,9,10,10,11,10,12,10,13,10,14,10,15,10,16,10,17,
                       10,18,10,19,10,20,10,21,10,22,10,23,10,24,10,25,10,68223234,1,2575,
                       85000450,1,656912,0,0,268505089,63,62,285281281,0,63,2063,10,101384453,0,9,10,2049,56,25,
                       459011,76,524546,76,302256641,1,10,16974595,0,50529798,10,25,524547,95,50529798,10,17108738,1,251790353,101777669,
                       1,17565186,86,524545,90,64,167838467,-1,134287105,3,59,659457,3,459023,107,2049,56,25,2049,107,
                       1793,114,2049,114,117506307,0,107,0,524545,25,112,168820993,0,126,1642241,126,134283523,7,112,1793,
                       107,7,524545,2049,107,1793,107,16846593,126,141,140,1793,64,16846593,126,112,140,1793,64,7,
                       10,659713,1,659713,2,659713,3,1793,168,17108737,3,2,524559,107,2049,107,2049,107,2049,121,
                       168820998,2,0,0,167841793,181,5,17826049,0,181,2,15,25,524546,164,134287105,182,95,2305,183,
                       459023,191,134287361,182,186,659201,181,2049,56,25,84152833,48,286458116,10,459014,206,184618754,45,25,16974851,
                       -1,168886532,1,134284289,1,215,134284289,0,206,660227,32,0,0,112,114,101,102,105,120,58,
                       95,0,285278479,232,7,2576,524546,81,1641217,1,167838467,229,2049,245,2049,241,524545,232,201,17826050,
                       231,0,2572,2563,2049,222,1793,133,459023,133,17760513,146,3,166,8,251727617,3,2,2049,160,
                       16,168820993,-1,126,2049,201,2049,160,459023,133,285282049,3,2,134287105,126,280,524545,1793,107,16846593,
                       3,0,107,8,659201,3,524545,25,112,17043201,3,7,2049,112,2049,107,268505092,126,1642241,126,
                       656131,659201,3,524545,7,112,2049,107,459009,19,112,459009,54,112,459009,15,112,459009,17,112,
                       1793,5,10,524546,160,134284303,162,1807,0,0,0,1642241,231,285282049,347,1,459012,342,117509889,181,
                       342,134287105,347,201,16845825,0,357,339,1793,64,1793,371,17826050,347,251,8,117506305,348,360,64,
                       2116,11340,11700,11400,13685,13104,12432,12402,9603,9801,11514,11413,11110,12528,11948,10302,13340,9700,13455,12753,
                       10500,10670,12654,13320,11960,13908,10088,10605,11865,11025,0,2049,201,987393,1,1793,107,524546,447,2049,
                       445,2049,445,17891588,2,447,8,17045505,-24,-16,17043736,-8,1118488,1793,107,17043202,1,169021201,2049,56,
                       25,33883396,101450758,6404,459011,437,34668804,2,2049,434,524545,379,437,302056196,379,659969,1,0,9,152,
                       100,117,112,0,456,11,152,100,114,111,112,0,463,13,152,115,119,97,112,0,
                       471,21,152,99,97,108,108,0,479,26,152,101,113,63,0,487,28,152,45,101,
                       113,63,0,494,30,152,108,116,63,0,502,32,152,103,116,63,0,509,34,152,
                       102,101,116,99,104,0,516,36,152,115,116,111,114,101,0,525,38,152,43,0,
                       534,40,152,45,0,539,42,152,42,0,544,44,152,47,109,111,100,0,549,46,
                       152,97,110,100,0,557,48,152,111,114,0,564,50,152,120,111,114,0,570,52,
                       152,115,104,105,102,116,0,577,333,158,112,117,115,104,0,586,336,158,112,111,
                       112,0,594,330,158,48,59,0,601,56,146,102,101,116,99,104,45,110,101,120,
                       116,0,607,59,146,115,116,111,114,101,45,110,101,120,116,0,621,222,146,115,
                       58,116,111,45,110,117,109,98,101,114,0,635,95,146,115,58,101,113,63,0,
                       650,81,146,115,58,108,101,110,103,116,104,0,659,64,146,99,104,111,111,115,
                       101,0,671,74,152,105,102,0,681,72,146,45,105,102,0,687,262,158,112,114,
                       101,102,105,120,58,40,0,694,126,133,67,111,109,112,105,108,101,114,0,706,
                       3,133,72,101,97,112,0,718,107,146,44,0,726,121,146,115,44,0,731,127,
                       158,59,0,737,289,158,91,0,742,305,158,93,0,747,2,133,68,105,99,116,
                       105,111,110,97,114,121,0,752,159,146,100,58,108,105,110,107,0,766,160,146,
                       100,58,120,116,0,776,162,146,100,58,99,108,97,115,115,0,784,164,146,100,
                       58,110,97,109,101,0,795,146,146,99,108,97,115,115,58,119,111,114,100,0,
                       805,158,146,99,108,97,115,115,58,109,97,99,114,111,0,819,133,146,99,108,
                       97,115,115,58,100,97,116,97,0,834,166,146,100,58,97,100,100,45,104,101,
                       97,100,101,114,0,848,263,158,112,114,101,102,105,120,58,35,0,864,269,158,
                       112,114,101,102,105,120,58,58,0,876,283,158,112,114,101,102,105,120,58,38,
                       0,888,267,158,112,114,101,102,105,120,58,36,0,900,320,158,114,101,112,101,
                       97,116,0,912,322,158,97,103,97,105,110,0,922,369,146,105,110,116,101,114,
                       112,114,101,116,0,931,201,146,100,58,108,111,111,107,117,112,0,944,152,146,
                       99,108,97,115,115,58,112,114,105,109,105,116,105,118,101,0,956,4,133,86,
                       101,114,115,105,111,110,0,975,416,146,105,0,986,107,146,100,0,991,410,146,
                       114,0,996,339,146,101,114,114,58,110,111,116,102,111,117,110,100,0 };
