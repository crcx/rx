/*---------------------------------------------------------------------
  Copyright (c) 2008 - 2021, Charles Childers

  The virtual machine is based on the C implementation of Ngaro and
  RETRO11, which were also copyrighted by Luke Parrish, Mark Simpson,
  Jay Skeer, and Kenneth Keating.
  ---------------------------------------------------------------------*/

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <limits.h>
#include <fcntl.h>

#define CELL int32_t
#define CELL_MIN INT_MIN + 1
#define CELL_MAX INT_MAX - 1
#define IMAGE_SIZE  32000000      /* Amount of RAM, in cells           */
#define ADDRESSES    256          /* Depth of address stack            */
#define STACK_DEPTH  256          /* Depth of data stack               */
#define TIB          1024         /* Location of TIB                   */

#define D_OFFSET_LINK     0       /* Dictionary Format Info. Update if */
#define D_OFFSET_XT       1       /* you change the dictionary fields. */
#define D_OFFSET_CLASS    2
#define D_OFFSET_NAME     3

#define MAX_DEVICES      32
#define MAX_OPEN_FILES   32

CELL stack_pop();
void stack_push(CELL value);
CELL string_inject(char *str, CELL buffer);
char *string_extract(CELL at);
CELL d_xt_for(char *Name, CELL Dictionary);
void update_rx();
void include_file(char *fname);
void register_device(void *handler, void *query);
void io_output();
void query_output();
void io_keyboard();
void query_keyboard();
void query_filesystem();
void io_filesystem();
void query_unix();
void io_unix();
void io_scripting();
void query_scripting();
void io_rng();
void query_rng();

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

Handler IO_deviceHandlers[MAX_DEVICES];
Handler IO_queryHandlers[MAX_DEVICES];
int devices;

CELL Dictionary;
CELL interpret;

char string_data[8192];
char **sys_argv;
int sys_argc;

void register_device(void *handler, void *query) {
  IO_deviceHandlers[devices] = handler;
  IO_queryHandlers[devices] = query;
  devices++;
}


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
  CELL slot, c;
  slot = stack_pop();
  c = stack_pop();
  fputc(c, OpenFileHandles[slot]);
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
  fflush(OpenFileHandles[stack_pop()]);
}

Handler FileActions[10] = {
  file_open,          file_close,
  file_read,          file_write,
  file_get_position,  file_set_position,
  file_get_size,      file_delete,
  file_flush
};

void query_filesystem() {
  stack_push(0);
  stack_push(4);
}

void io_filesystem() {
  FileActions[stack_pop()]();
}

void unix_dir() {
  DIR *dir;
  struct dirent *entry;
  char files[65536], *src;
  CELL i, to;

  to = stack_pop();
  i = 0;
  files[i] = '\0';
  if ((dir = opendir(".")) == NULL)
    perror("opendir() error");
  else {
    while ((entry = readdir(dir)) != NULL) {
      if (entry->d_name[0] != '.' && entry->d_type !=  DT_DIR) {
        src = entry->d_name;
        while ((files[i] = *src++))
          i++;
        files[i++] = '\n';
      }
    }
    files[--i] = '\0';
    stack_push(string_inject(files, to));
    closedir(dir);
  }
}

void unix_system() {
  char *line, *args[128];
  int i, status;
  pid_t pid;

  char **argv = args;
  line = string_extract(stack_pop());

  for(i = 0; i < 128; i++)
    args[i] = 0;

  while (*line != '\0') {
    while (*line == ' ' || *line == '\t' || *line == '\n')
      *line++ = '\0';
    *argv++ = line;
    while (*line != '\0' && *line != ' ' && *line != '\t' && *line != '\n')
      line++;
  }

  if ((pid = fork()) < 0) {
    printf("*** ERROR: forking child process failed\n");
    exit(1);
  }
  else if (pid == 0) {
    int e = execvp(*args, args);
    if (e < 0) {
      printf("*** ERROR: exec failed with %d\n", e);
      exit(1);
    }
  } else {
  while (wait(&status) != pid)
    ;
  }
}

Handler UnixActions[] = {
  unix_system, unix_dir
};

void query_unix() {
  stack_push(1);
  stack_push(8);
}

void io_unix() {
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

void query_rng() {
  stack_push(0);
  stack_push(10);
}

void io_output() {
  putc(stack_pop(), stdout);
  fflush(stdout);
}

void query_output() {
  stack_push(0);
  stack_push(0);
}

void io_keyboard() {
  stack_push(getc(stdin));
  if (TOS == 127) TOS = 8;
}

void query_keyboard() {
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
  scripting_arg_count,  scripting_arg,
  scripting_include,    scripting_name,
};

void query_scripting() {
  stack_push(2);
  stack_push(9);
}

void io_scripting() {
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

  initialize();
  update_rx();
  register_device(io_output, query_output);
  register_device(io_keyboard, query_keyboard);
  register_device(io_filesystem, query_filesystem);
  register_device(io_unix, query_unix);
  register_device(io_scripting, query_scripting);


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
  devices = 0;
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
    NOS = NOS << (0 - TOS);
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
  TOS = devices;
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

int32_t ngaImageCells = 1011;
int32_t ngaImage[] = {
1793,-1,995,1536,202104,0,10,1,10,2,10,3,10,4,10,5,10,6,10,7,
10,8,10,9,10,10,11,10,12,10,13,10,14,10,15,10,16,10,17,10,
18,10,19,10,20,10,21,10,22,10,23,10,24,10,25,10,68223234,1,2575,85000450,
1,656912,0,0,268505089,63,62,285281281,0,63,2063,10,101384453,0,9,10,2049,56,25,459011,
76,524546,76,302256641,1,10,16974595,0,50529798,10,25,524547,95,50529798,10,17108738,1,251790353,101777669,1,
17565186,86,524545,90,64,167838467,-1,134287105,3,59,659457,3,459023,107,2049,56,25,2049,107,1793,
114,2049,114,117506307,0,107,0,524545,25,112,168820993,0,126,1642241,126,134283523,7,112,1793,107,
7,524545,2049,107,1793,107,16846593,126,141,140,1793,64,16846593,126,112,140,1793,64,7,10,
659713,1,659713,2,659713,3,1793,168,17108737,3,2,524559,107,2049,107,2049,107,2049,121,168820998,
2,0,0,167841793,181,5,17826049,0,181,2,15,25,524546,164,134287105,182,95,2305,183,459023,
191,134287361,182,186,659201,181,2049,56,25,84152833,48,286458116,10,459014,206,184618754,45,25,16974851,-1,
168886532,1,134284289,1,215,134284289,0,206,660227,32,0,0,115,105,103,105,108,58,95,0,
285278479,232,6,2576,524546,81,1641217,1,167838467,229,2049,244,2049,240,524545,232,201,17826050,231,0,
2572,2563,2049,222,1793,133,459023,133,17760513,146,3,166,8,251727617,3,2,2049,160,16,168820993,
-1,126,2049,201,2049,160,459023,133,285282049,3,2,134287105,126,279,524545,1793,107,16846593,3,0,
107,8,659201,3,524545,25,112,17043201,3,7,2049,112,2049,107,268505092,126,1642241,126,656131,659201,
3,524545,7,112,2049,107,459009,19,112,459009,54,112,459009,15,112,459009,17,112,1793,5,
10,524546,160,134284303,162,1807,0,0,0,1642241,231,285282049,346,1,459012,341,117509889,181,341,134287105,
346,201,16845825,0,356,338,1793,64,1793,370,17826050,346,250,8,117506305,347,359,64,2116,11340,
11700,11400,13685,13104,12432,12402,9603,9801,11514,11413,11110,12528,11948,10302,13340,9700,13455,12753,10500,10670,
12654,13320,11960,13908,10088,10605,11865,11025,0,2049,201,987393,1,1793,107,524546,446,2049,444,2049,
444,17891588,2,446,8,17045505,-24,-16,17043736,-8,1118488,1793,107,17043202,1,169021201,2049,56,25,33883396,
101450758,6404,459011,436,34668804,2,2049,433,524545,378,436,302056196,378,659969,1,0,9,152,100,117,
112,0,455,11,152,100,114,111,112,0,462,13,152,115,119,97,112,0,470,21,
152,99,97,108,108,0,478,26,152,101,113,63,0,486,28,152,45,101,113,63,
0,493,30,152,108,116,63,0,501,32,152,103,116,63,0,508,34,152,102,101,
116,99,104,0,515,36,152,115,116,111,114,101,0,524,38,152,43,0,533,40,
152,45,0,538,42,152,42,0,543,44,152,47,109,111,100,0,548,46,152,97,
110,100,0,556,48,152,111,114,0,563,50,152,120,111,114,0,569,52,152,115,
104,105,102,116,0,576,332,158,112,117,115,104,0,585,335,158,112,111,112,0,
593,329,158,48,59,0,600,56,146,102,101,116,99,104,45,110,101,120,116,0,
606,59,146,115,116,111,114,101,45,110,101,120,116,0,620,222,146,115,58,116,
111,45,110,117,109,98,101,114,0,634,95,146,115,58,101,113,63,0,649,81,
146,115,58,108,101,110,103,116,104,0,658,64,146,99,104,111,111,115,101,0,
670,74,152,105,102,0,680,72,146,45,105,102,0,686,261,158,115,105,103,105,
108,58,40,0,693,126,133,67,111,109,112,105,108,101,114,0,704,3,133,72,
101,97,112,0,716,107,146,44,0,724,121,146,115,44,0,729,127,158,59,0,
735,288,158,91,0,740,304,158,93,0,745,2,133,68,105,99,116,105,111,110,
97,114,121,0,750,159,146,100,58,108,105,110,107,0,764,160,146,100,58,120,
116,0,774,162,146,100,58,99,108,97,115,115,0,782,164,146,100,58,110,97,
109,101,0,793,146,146,99,108,97,115,115,58,119,111,114,100,0,803,158,146,
99,108,97,115,115,58,109,97,99,114,111,0,817,133,146,99,108,97,115,115,
58,100,97,116,97,0,832,166,146,100,58,97,100,100,45,104,101,97,100,101,
114,0,846,262,158,115,105,103,105,108,58,35,0,862,268,158,115,105,103,105,
108,58,58,0,873,282,158,115,105,103,105,108,58,38,0,884,266,158,115,105,
103,105,108,58,36,0,895,319,158,114,101,112,101,97,116,0,906,321,158,97,
103,97,105,110,0,916,368,146,105,110,116,101,114,112,114,101,116,0,925,201,
146,100,58,108,111,111,107,117,112,0,938,152,146,99,108,97,115,115,58,112,
114,105,109,105,116,105,118,101,0,950,4,133,86,101,114,115,105,111,110,0,
969,415,146,105,0,980,107,146,100,0,985,409,146,114,0,990,338,146,101,114,
114,58,110,111,116,102,111,117,110,100,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,288898,289938,133,70,73,68,0,1,289931,289952,146,114,101,97,100,45,
98,121,116,101,0,3841,289938,2049,9522,1,255,21,10,289939,289973,146,114,101,97,100,
45,99,101,108,108,0,2049,289952,2049,289952,2049,289952,2049,289952,1,-8,24,17,1,-8,
24,17,1,-8,24,17,10,289960,290003,133,67,101,108,108,115,0,1011,289994,290012,146,
115,105,122,101,0,3841,289938,2049,9595,1,4,197652,2,4097,290003,10,290004,290037,146,108,
111,97,100,45,105,109,97,103,101,0,1,0,2049,9485,4097,289938,1,288907,2049,290012,
1793,290056,2049,289973,67502597,16,2049,2884,10,1,290049,2049,2270,3,3841,289938,2049,9504,10,290023,
290075,133,67,111,117,110,116,0,8,290066,290084,146,69,79,76,63,0,1,290075,2049,
2982,3841,290075,1,20,11,1793,290102,2049,9203,1,0,4097,290075,10,1,290095,9,10,1793,
290221,3841,290003,2049,3640,105,110,116,51,50,92,32,116,32,110,103,97,73,109,97,
103,101,67,101,108,108,115,32,61,32,37,110,59,0,1,290112,2049,7217,2049,9240,
2049,9203,2049,3640,105,110,116,51,50,92,32,116,32,110,103,97,73,109,97,103,
101,91,93,32,61,32,123,0,1,290151,2049,7217,2049,9240,2049,9203,1,288907,1,288907,
1,3,17,15,1793,290204,2049,56,2049,9254,1,44,2049,9190,2049,290084,10,1,290193,2049,
2270,3,2049,3640,125,59,0,1,290211,2049,9240,2049,9203,10,1,290108,56,53,0,48,
57,50,0,54,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,};
