/*
   american fuzzy lop - LLVM instrumentation bootstrap
   ---------------------------------------------------

   Written by Laszlo Szekeres <lszekeres@google.com> and
              Michal Zalewski <lcamtuf@google.com>

   LLVM integration design comes from Laszlo Szekeres.

   Copyright 2015, 2016 Google Inc. All rights reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at:

     http://www.apache.org/licenses/LICENSE-2.0

   This code is the rewrite of afl-as.h's main_payload.

*/

#include "../config.h"
#include "../types.h"
#include "../debug.h"
#include "afl-rt.h"
#include "../path_type.h"

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>

#include <sys/file.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>

#include <stddef.h>
#include <execinfo.h>



/* This is a somewhat ugly hack for the experimental 'trace-pc-guard' mode.
   Basically, we need to make sure that the forkserver is initialized after
   the LLVM-generated runtime initialization pass, not before. */

#ifdef USE_TRACE_PC
#  define CONST_PRIO 5
#else
#  define CONST_PRIO 0
#endif /* ^USE_TRACE_PC */

/* Globals needed by the injected instrumentation. The __afl_area_initial region
   is used for instrumentation output before __afl_map_shm() has a chance to run.
   It will end up as .comm, so it shouldn't be too wasteful. */

extern int x;
extern uint32_t hash_vals[];

u8  __afl_area_initial[MAP_SIZE];
u8* __afl_area_ptr = __afl_area_initial;

uint64_t  __afl_max_initial[MAXMAP_SIZE];
uint64_t* __afl_max_ptr = __afl_max_initial;


shared_data_t* shared_data = NULL;

__thread u32 __afl_prev_loc;
__thread u32 __afl_state;
__thread u32 __afl_state_log;
__thread u32 __afl_mask = 0xffffffff;

void ijon_xor_state(uint32_t val){
  __afl_state = (__afl_state^val)%MAP_SIZE;
}

void ijon_push_state(uint32_t x){
  ijon_xor_state(__afl_state_log);
  __afl_state_log = (__afl_state_log << 8) | (x & 0xff);
  ijon_xor_state(__afl_state_log);
}

void ijon_max(uint32_t addr, uint64_t val){
  if(__afl_max_ptr[addr%MAXMAP_SIZE] < val) {
    __afl_max_ptr[addr%MAXMAP_SIZE] = val;
  }
}

void ijon_min(uint32_t addr, uint64_t val){
  val = 0xffffffffffffffff-val;
  ijon_max(addr, val);
}


void ijon_map_inc(uint32_t addr){ 
  __afl_area_ptr[(__afl_state^addr)%MAP_SIZE]+=1;
}

int fb_get_file_desc(char* name) {
  return shm_open(name, O_RDWR, S_IRWXU);
}

void* fb_get_shared_mem(int fd) {
  int FB_SHARED_MEM_SIZE = 8192;
  void* addr = mmap(NULL, FB_SHARED_MEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  return addr; 
}

char* control_char_to_hex[] = {
  "00", "01", "02", "03", "04", "05", "06", "07",
  "08", "09", "0A", "0B", "0C", "0D", "0E", "0F",
  "10", "11", "12", "13", "14", "15", "16", "17",
  "18", "19", "1A", "1B", "1C", "1D", "1E", "1F"
};

char* extended_ascii_to_hex[] = {
  "80", "81", "82", "83", "84", "85", "86", "87", "88",
  "89", "8A", "8B", "8C", "8D", "8E", "8F", "90", "91",
  "92", "93", "94", "95", "96", "97", "98", "99", "9A",
  "9B", "9C", "9D", "9E", "9F", "A0", "A1", "A2", "A3",
  "A4", "A5", "A6", "A7", "A8", "A9", "AA", "AB", "AC",
  "AD", "AE", "AF", "B0", "B1", "B2", "B3", "B4", "B5",
  "B6", "B7", "B8", "B9", "BA", "BB", "BC", "BD", "BE",
  "BF", "C0", "C1", "C2", "C3", "C4", "C5", "C6", "C7",
  "C8", "C9", "CA", "CB", "CC", "CD", "CE", "CF", "D0",
  "D1", "D2", "D3", "D4", "D5", "D6", "D7", "D8", "D9",
  "DA", "DB", "DC", "DD", "DE", "DF", "E0", "E1", "E2",
  "E3", "E4", "E5", "E6", "E7", "E8", "E9", "EA", "EB",
  "EC", "ED", "EE", "EF", "F0", "F1", "F2", "F3", "F4",
  "F5", "F6", "F7", "F8", "F9", "FA", "FB", "FC", "FD",
  "FE", "FF",
};

char* escape_path(const char* path, int length) {
  // the maximum possible length of the new path is
  // length*4 if all chars are in the 0-31 range.
  char* escaped_path = malloc(length * 4 * sizeof(char));

  int pos_in_dest = 0;
  for (int i = 0 ; i < length; i++) {
    uint8_t curr_val = path[i];

    if (curr_val <= 31) {
      char* hex_val = control_char_to_hex[curr_val];
      escaped_path[pos_in_dest] = '\\';
      escaped_path[pos_in_dest + 1] = 'x';
      escaped_path[pos_in_dest + 2] = hex_val[0];
      escaped_path[pos_in_dest + 3] = hex_val[1];
      pos_in_dest += 4;
    } else if (curr_val == 92) {
      // The character is a backslash that needs to be escaped.
      escaped_path[pos_in_dest] = '\\';
      escaped_path[pos_in_dest + 1] = '\\';
      pos_in_dest += 2;
    } else if (curr_val <= 127) {
      // Just a normal character, no special handling.
      escaped_path[pos_in_dest] = curr_val;
      pos_in_dest++;
    } else {
      // Above 127, escape with \x.
      char* hex_val = extended_ascii_to_hex[curr_val - 128];
      escaped_path[pos_in_dest] = '\\';
      escaped_path[pos_in_dest + 1] = 'x';
      escaped_path[pos_in_dest + 2] = hex_val[0];
      escaped_path[pos_in_dest + 3] = hex_val[1];
      pos_in_dest += 4;
    } 
  }

  escaped_path[pos_in_dest] = '\0';

  return escaped_path;
}

void state_path(int state, const char* path, int pos) {
  IJON_SET(ijon_simple_hash(state));
  int fd = fb_get_file_desc("/pathinfo");
  void* mem_ptr = fb_get_shared_mem(fd);
  PathInfo* path_info = (PathInfo*) mem_ptr;
  size_t original_length = pos + 1;

  char* escaped_path = escape_path(path, pos + 1);
  size_t path_length = strlen(escaped_path);
  if (path_length > MAX_PATH) {
    // Path length is higher than MAX_PATH (50) which means that it won't fit into the
    // PathInfo struct.
    return;
  }

  // Lock
  flock(fd, LOCK_EX);

  int current_length = path_info[state].length;

  if (current_length == -1) {
      strncpy(path_info[state].path, escaped_path, path_length);
      path_info[state].path[path_length] = '\0';
      path_info[state].length = original_length;
      path_info[state].state = state;
  } else if (original_length < current_length) {
      strncpy(path_info[state].path, escaped_path, path_length);
      path_info[state].path[path_length] = '\0';
      path_info[state].length = original_length;
  } 

  msync(mem_ptr, 8192, MS_SYNC);
  flock(fd, LOCK_UN);
  // Unlock

}

void ijon_map_set(uint32_t addr){
  int fd = fb_get_file_desc("/fuzzboys");
  
  // Lock
  flock(fd, LOCK_EX);

  void* shm_addr = fb_get_shared_mem(fd);
  uint32_t* shm_uint = (uint32_t*) shm_addr;
  
  for (int i = 0; i < 100; i++) {
    if (shm_uint[i] == addr) {
      // addr has already been seen.
      break;
    } else if (shm_uint[i] == 0) {
      // empty slot, insert.
      shm_uint[i] = addr;
      break;
    } 
  }

  msync(shm_addr, 8192, MS_SYNC);

  // Unlock.
  flock(fd, LOCK_UN);

  __afl_area_ptr[(__afl_state^addr)%MAP_SIZE]|=1;
}

uint32_t ijon_strdist(char* a,char* b){
  int i = 0;
  while(*a && *b && *a++==*b++){
    i++;
  }
  return i;
}

uint32_t ijon_memdist(char* a,char* b, size_t len){
  int i = 0;
  while(i < len && *a++==*b++){
    i++;
  }
  return i;
}

uint64_t ijon_simple_hash(uint64_t x) {
    x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
    x = x ^ (x >> 31);
    return x;
}

void ijon_enable_feedback(){
	__afl_mask = 0xffffffff;
}
void ijon_disable_feedback(){
	__afl_mask = 0x0;
}

uint32_t ijon_hashint(uint32_t old, uint32_t val){
  uint64_t input = (((uint64_t)(old))<<32) | ((uint64_t)(val));
  
  return (uint32_t)(ijon_simple_hash(input));
}
uint32_t ijon_hashstr(uint32_t old, char* val){
  return ijon_hashmem(old, val, strlen(val));
}
uint32_t ijon_hashmem(uint32_t old, char* val, size_t len){
  old = ijon_hashint(old,len);
  for(size_t i = 0; i < len ; i++){
    old = ijon_hashint(old, val[i]);
  }
  return old;
}

#ifdef __i386__
//WHY IS STACKUNWINDING NOT WORKING IN CGC BINARIES?
uint32_t ijon_hashstack_manual(){
	uint32_t *ebp=0;
	uint64_t res = 0;
	asm("\t movl %%ebp,%0" : "=r"(ebp));
	for(int i=0; i<16 && ebp; i++){
		//printf("ebp: %p\n", ebp);
		printf("ret: %x\n", ebp[1]);
		res ^= ijon_simple_hash((uint64_t)ebp[1]);
		ebp = (uint32_t*)ebp[0];
	}
 printf(">>>> Final Stackhash: %lx\n",res);
	return (uint32_t)res;
}
#endif

uint32_t ijon_hashstack_libgcc(){
 void* buffer[16] = {0,};
 int num = backtrace (buffer, 16);
 assert(num<16);
 uint64_t res = 0;
 for(int i =0; i < num; i++) {
	 printf("stack_frame %p\n", buffer[i]);
	 res ^= ijon_simple_hash((uint64_t)buffer[i]);
 }
 printf(">>>> Final Stackhash: %lx\n",res);
 return (uint32_t)res;
}

/* Running in persistent mode? */

static u8 is_persistent;


/* SHM setup. */

static void __afl_map_shm(void) {

  u8 *id_str = getenv(SHM_ENV_VAR);

  /* If we're running under AFL, attach to the appropriate region, replacing the
     early-stage __afl_area_initial region that is needed to allow some really
     hacky .init code to work correctly in projects such as OpenSSL. */

  if (id_str) {

    u32 shm_id = atoi(id_str);

    shared_data = shmat(shm_id, NULL, 0);

    if (shared_data == (void *)-1) _exit(1);

    __afl_area_ptr = &shared_data->afl_area[0];
    __afl_max_ptr = &shared_data->afl_max[0];

    /* Whooooops. */


    /* Write something into the bitmap so that even with low AFL_INST_RATIO,
       our parent doesn't give up on us. */

    __afl_area_ptr[0] = 1;

  }

}


/* Fork server logic. */

static void __afl_start_forkserver(void) {

  static u8 tmp[4];
  s32 child_pid;

  u8  child_stopped = 0;

  /* Phone home and tell the parent that we're OK. If parent isn't there,
     assume we're not running in forkserver mode and just execute program. */

  if (write(FORKSRV_FD + 1, tmp, 4) != 4) return;

  while (1) {

    u32 was_killed;
    int status;

    /* Wait for parent by reading from the pipe. Abort if read fails. */

    if (read(FORKSRV_FD, &was_killed, 4) != 4) _exit(1);

    /* If we stopped the child in persistent mode, but there was a race
       condition and afl-fuzz already issued SIGKILL, write off the old
       process. */

    if (child_stopped && was_killed) {
      child_stopped = 0;
      if (waitpid(child_pid, &status, 0) < 0) _exit(1);
    }

    if (!child_stopped) {

      /* Once woken up, create a clone of our process. */

      child_pid = fork();
      if (child_pid < 0) _exit(1);

      /* In child process: close fds, resume execution. */

      if (!child_pid) {

        close(FORKSRV_FD);
        close(FORKSRV_FD + 1);
        return;
  
      }

    } else {

      /* Special handling for persistent mode: if the child is alive but
         currently stopped, simply restart it with SIGCONT. */

      kill(child_pid, SIGCONT);
      child_stopped = 0;

    }

    /* In parent process: write PID to pipe, then wait for child. */

    if (write(FORKSRV_FD + 1, &child_pid, 4) != 4) _exit(1);

    if (waitpid(child_pid, &status, is_persistent ? WUNTRACED : 0) < 0)
      _exit(1);

    /* In persistent mode, the child stops itself with SIGSTOP to indicate
       a successful run. In this case, we want to wake it up without forking
       again. */

    if (WIFSTOPPED(status)) child_stopped = 1;

    /* Relay wait status to pipe, then loop back. */

    if (write(FORKSRV_FD + 1, &status, 4) != 4) _exit(1);

  }

}


/* A simplified persistent mode handler, used as explained in README.llvm. */

int __afl_persistent_loop(unsigned int max_cnt) {

  static u8  first_pass = 1;
  static u32 cycle_cnt;

  if (first_pass) {

    /* Make sure that every iteration of __AFL_LOOP() starts with a clean slate.
       On subsequent calls, the parent will take care of that, but on the first
       iteration, it's our job to erase any trace of whatever happened
       before the loop. */

    if (is_persistent) {

      memset(__afl_area_ptr, 0, MAP_SIZE);
      memset(__afl_max_ptr, 0, MAXMAP_SIZE*sizeof(uint64_t));
      __afl_area_ptr[0] = 1;
      __afl_prev_loc = 0;
      __afl_state = 0;
			__afl_mask = 0xffffffff;
      __afl_state_log = 0;
    }

    cycle_cnt  = max_cnt;
    first_pass = 0;
    return 1;

  }

  if (is_persistent) {

    if (--cycle_cnt) {

      raise(SIGSTOP);

      __afl_area_ptr[0] = 1;
      __afl_prev_loc = 0;
      __afl_state = 0;
			__afl_mask = 0xffffffff;
      __afl_state_log = 0;

      return 1;

    } else {

      /* When exiting __AFL_LOOP(), make sure that the subsequent code that
         follows the loop is not traced. We do that by pivoting back to the
         dummy output region. */

      __afl_area_ptr = __afl_area_initial;

    }

  }

  return 0;

}


/* This one can be called from user code when deferred forkserver mode
    is enabled. */

void __afl_manual_init(void) {

  static u8 init_done;

  if (!init_done) {

    __afl_map_shm();
    __afl_start_forkserver();
    init_done = 1;

  }

}


/* Proper initialization routine. */

__attribute__((constructor(CONST_PRIO))) void __afl_auto_init(void) {
  is_persistent = !!getenv(PERSIST_ENV_VAR);

  if (getenv(DEFER_ENV_VAR)) return;

  __afl_manual_init();

}


/* The following stuff deals with supporting -fsanitize-coverage=trace-pc-guard.
   It remains non-operational in the traditional, plugin-backed LLVM mode.
   For more info about 'trace-pc-guard', see README.llvm.

   The first function (__sanitizer_cov_trace_pc_guard) is called back on every
   edge (as opposed to every basic block). */

void __sanitizer_cov_trace_pc_guard(uint32_t* guard) {
  __afl_area_ptr[*guard]++;
}


/* Init callback. Populates instrumentation IDs. Note that we're using
   ID of 0 as a special value to indicate non-instrumented bits. That may
   still touch the bitmap, but in a fairly harmless way. */

void __sanitizer_cov_trace_pc_guard_init(uint32_t* start, uint32_t* stop) {

  u32 inst_ratio = 100;
  u8* x;

  if (start == stop || *start) return;

  x = getenv("AFL_INST_RATIO");
  if (x) inst_ratio = atoi(x);

  if (!inst_ratio || inst_ratio > 100) {
    fprintf(stderr, "[-] ERROR: Invalid AFL_INST_RATIO (must be 1-100).\n");
    abort();
  }

  /* Make sure that the first element in the range is always set - we use that
     to avoid duplicate calls (which can happen as an artifact of the underlying
     implementation in LLVM). */

  *(start++) = R(MAP_SIZE - 1) + 1;

  while (start < stop) {

    if (R(100) < inst_ratio) *start = R(MAP_SIZE - 1) + 1;
    else *start = 0;

    start++;

  }

}
