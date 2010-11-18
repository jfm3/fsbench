// fsbench.c

// Copyright (C) 2010 Joseph F. Miklojcik III.
//
// This file is part of fsbench.
//
// fsbench is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// fsbench is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with fsbench.  If not, see <http://www.gnu.org/licenses/>.

// fsbench is a program to take straightforward file i/o benchmarks
// from a user space process to a single file in the file system.  It
// reports benchmarks in the form of elapsed times for several
// individual tests, for which file sizes, chunk sizes, and random
// seek counts can be specified on the command line.
//
// The FALLOCATE benchmark measures how long it takes to
// posix_fallocate() and then fsync() a file of the name and size
// specified on the command line.  The APPEND benchmark measures the
// same operation as fallocate() when performed using explicit write()
// system calls in specified chunk sizes.  This both indicates whether
// the filesystem and C library implements posix_fallocate() using
// special filesystem features, and benchmarks how long it takes to
// generate a (presumably large) file filled with zeros.
//
// The UNLINK benchmark measures how long it takes to delete a
// (presumably large) file from the filesystem.
//
// The SEEK READ and SEEK WRITE benchmarks measure how long it takes
// to do a certain number of lseek()/read() or lseek()/write()
// operations over a file of random data.  No file buffer cache
// invalidation is performed, although a single fsync() is performed
// in the SEEK WRITE benchmark after all seeking and writing is
// finished.
//
// These benchmarks were chosen to mimic typical expensive filesystem
// operations performed by various databases and object stores.  A run
// of these benchmarks serves as part of a battery of sanity tests for
// new servers.

// We presume that a long int will hold an off_t, a size_t, and other
// integer types used in arguments to library calls that wrap system
// calls, and without any signedness shenanigans.  If your POSIX
// implementor is pathological, you've got bigger problems.

#ifndef VERSION
#define VERSION "1"
#endif

#define _XOPEN_SOURCE 600
#define _BSD_SOURCE
#include <sys/types.h>
#include <sys/times.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <strings.h> // for rindex() on older Linuxen

// In this program, when library calls fail, we generally want to
// print an error message and exit().  We never want to try something
// else or continue on.  So we wrap the library functions that wrap
// system calls with our own foo_or_exit() function which checks for
// error conditions.  If errors are found, these functions perror()
// and exit() with one of the following codes.  As with all UNIX
// programs, the zero exit code should be reserved for success.  We'd
// use sysexits.h here, but it would require too much errno analysis
// for only a more vague notion of what went wrong in return.
#define EXIT_OUT_OF_MEMORY 1
#define EXIT_COMMAND_LINE_BORKED 2
#define EXIT_CANT_OPEN 3
#define EXIT_CANT_FALLOCATE 4
#define EXIT_CANT_CLOSE 5
#define EXIT_CANT_WRITE 6
#define EXIT_CANT_UNLINK 7
#define EXIT_CANT_LSEEK 8
#define EXIT_CANT_READ 9
#define EXIT_CANT_FSYNC 10
#define EXIT_CANT_GETPRIORITY 11
#define EXIT_CANT_OPEN_LOADAVG 12

static void *malloc_or_exit(size_t size)
{
  void *p = NULL;

  p = malloc(size);
  if (p == NULL) {
    fprintf(stderr, "Out of memory.\n");
    exit(EXIT_OUT_OF_MEMORY);
  }
  memset(p, 0, size);
  return (p);
}

static int open_or_exit(const char *file_name)
{
  int result = -1;

  errno = 0;
  result = open(file_name,
		O_RDWR | O_CREAT | O_EXCL,
		S_IRUSR | S_IWUSR);
  if (result < 0) {
    perror("Can't open");
    exit(EXIT_CANT_OPEN);
  }
  return (result);
}

static void close_or_exit(int fd)
{
  int result = -1;

  errno = 0;
  result = close(fd);
  if (result < 0) {
    perror("Can't close");
    exit(EXIT_CANT_CLOSE);
  }
}

static void write_or_exit(int fd, char *buffer, size_t count)
{
  int result = -1;

  errno = 0;
  result = write(fd, buffer, count);
  if (result < 0) {
    perror("Can't write");
    exit(EXIT_CANT_WRITE);
  }
}

static void posix_fallocate_or_exit(int fd, off_t offset, off_t length)
{
  int result = 0;

  result = posix_fallocate(fd, offset, length);
  // posix_fallocate() doesn't use errno/perror.  Lovely.
  switch (result) {
    case EBADF:
      fprintf(stderr,
	      "Not a valid file descriptor, or not opened for writing.\n");
      break;
    case EFBIG:
      fprintf(stderr, "Offset + length > maximum file size.\n");
      break;
    case EINVAL:
      fprintf(stderr, "Offset was < 0, or length was <= 0.\n");
      break;
    case ENODEV:
      fprintf(stderr, "Not a regular file.\n");
      break;
    case ENOSPC:
      fprintf(stderr, "No space left on device.\n");
      break;
    case ESPIPE:
      fprintf(stderr, "How did a pipe get in here?\n");
      break;
    case 0:
    default:
      return;
  }
  exit(EXIT_CANT_FALLOCATE);
}

static void unlink_or_exit(const char *file_name)
{
  int result = -1;

  errno = 0;
  result = unlink(file_name);
  if (result < 0) {
    perror("Can't unlink");
    exit(EXIT_CANT_UNLINK);
  }
}

static void lseek_or_exit(int fd, off_t offset) 
{
  off_t result = -1;

  errno = 0;
  result = lseek(fd, offset, SEEK_SET);
  if (result < 0) {
    perror("Can't lseek");
    exit(EXIT_CANT_LSEEK);
  }
}

static void read_or_exit(int fd, void *buffer, int size)
{
  int result = -1;

  errno = 0;
  result = read(fd, buffer, size);
  if (result < 0) {
    perror("Can't read");
    exit(EXIT_CANT_READ);
  }
}

static void fsync_or_exit(int fd)
{
  int result = -1;

  errno = 0;
  result = fsync(fd);
  if (result < 0) {
    perror("Can't fsync");
    exit(EXIT_CANT_FSYNC);
  }
}

static int getpriority_or_exit()
{
  int result = 0;

  errno = 0;
  result = getpriority(PRIO_PROCESS, 0);
  // result can legitimately be -1, so check errno explicitly.
  if (result == -1 && errno != 0) {
    perror("Can't setpriority");
    exit(EXIT_CANT_GETPRIORITY);
  }
  return (result);
}

// We represent moments in time with this moment struct.  The idea is
// to hold the output of both times() and gettimeofday(), both of
// which hold durations since some known time, and that two moments
// get compared to compute a relative duration, which is the result of
// a benchmark.
struct moment {
  struct tms cpu;
  struct timeval real;
};

static void print_moment(struct moment *t)
{
  float ticks_per_second = (float) sysconf(_SC_CLK_TCK);
  
  printf("user   = %.2fs\n",
	 ((float) t->cpu.tms_utime) / ticks_per_second);
  printf("system = %.2fs\n",
	 ((float) t->cpu.tms_stime) / ticks_per_second);
  printf("real   = %.2fs\n",
	 ((float) t->real.tv_sec)
	 + ((float) t->real.tv_usec / 1000000.0));
}

static struct moment duration(struct moment start, struct moment end)
{
  struct moment result;
  memset(&result, 0, sizeof (struct moment));
  
  result.cpu.tms_utime = end.cpu.tms_utime - start.cpu.tms_utime;
  result.cpu.tms_stime = end.cpu.tms_stime - start.cpu.tms_stime;
  result.cpu.tms_cutime = end.cpu.tms_cutime - start.cpu.tms_cutime;
  result.cpu.tms_cstime = end.cpu.tms_cstime - start.cpu.tms_cstime;
  timerclear(&result.real);
  timersub(&end.real, &start.real, &result.real);
  return (result);
}

// General notes on benchmark implementations:
//
// Nest calls to gettimeofday() within the calls to times().  They
// both should be fast operations, and the whole show is not really
// accurate enough for this to matter, but for the sake of somber
// measurement we should at least do it the same way in each
// benchmark.
//
// Some benchmarks fill files with random data.  Some benchmarks seek
// around to random locations within a file.  Randomness is obtained
// by calls to srandom() and random(), which should be good enough to
// foil any compression in filesystem dataflow, and to generate a
// challenge for a physical disk and an I/O scheduler.  Benchmark
// implementations should not cause calls to srandom(); they should
// run exactly the same based on a particular seed.

// Generate random % foo without bias.  Uses `long int` to match
// random() from library.
static long int random_to(long int n)
{
  long int result = 0;

  if (n == 0) return (0);
  result = random();
  while (result > RAND_MAX - (RAND_MAX % n))
    result = random();
  return (result);
}

// Fill a chunk with randomness.
static void random_fill(char *p, int size)
{
  int i = 0;

  for (i = 0; i < size; i++) p[i] = (char) random();
}

static struct moment benchmark_fallocate(const char *file_name,
  long int file_size, long int chunk_size, long int n_seeks)
{
  int fd = -1;
  struct moment start;
  struct moment end;
  struct moment result;
  memset(&start, 0, sizeof (struct moment));
  memset(&end, 0, sizeof (struct moment));
  memset(&result, 0, sizeof (struct moment));

  fd = open_or_exit(file_name);
  times(&start.cpu);
  gettimeofday(&start.real, NULL);
  posix_fallocate_or_exit(fd, 0, file_size * chunk_size);
  fsync_or_exit(fd);
  gettimeofday(&end.real, NULL);
  times(&end.cpu);
  close_or_exit(fd);
  unlink_or_exit(file_name);
  result = duration(start, end);
  return (result);
}

static struct moment benchmark_unlink(const char *file_name,
  long int file_size, long int chunk_size, long int n_seeks)
{
  int fd = -1;
  struct moment start;
  struct moment end;
  struct moment result;
  memset(&start, 0, sizeof (struct moment));
  memset(&end, 0, sizeof (struct moment));
  memset(&result, 0, sizeof (struct moment));

  fd = open_or_exit(file_name);
  posix_fallocate_or_exit(fd, 0, file_size * chunk_size);
  fsync_or_exit(fd);
  close_or_exit(fd);
  times(&start.cpu);
  gettimeofday(&start.real, NULL);
  unlink_or_exit(file_name);
  gettimeofday(&end.real, NULL);
  times(&end.cpu);
  result = duration(start, end);
  return (result);
}

static struct moment benchmark_append(const char *file_name,
  long int file_size, long int chunk_size, long int n_seeks)
{
  int fd = -1;
  int i = 0;
  char *chunk_buffer = NULL;
  struct moment start;
  struct moment end;
  struct moment result;
  memset(&start, 0, sizeof (struct moment));
  memset(&end, 0, sizeof (struct moment));
  memset(&result, 0, sizeof (struct moment));

  chunk_buffer = malloc_or_exit(chunk_size);
  // Fill will zero to mimic posix_fallocate().
  memset(chunk_buffer, 0, chunk_size);
  fd = open_or_exit(file_name);
  times(&start.cpu);
  gettimeofday(&start.real, NULL);
  for (i = 0; i < file_size; i++) {
    write_or_exit(fd, chunk_buffer, chunk_size);
  }
  fsync_or_exit(fd);
  gettimeofday(&end.real, NULL);
  times(&end.cpu);
  free(chunk_buffer);
  close_or_exit(fd);
  unlink_or_exit(file_name);
  result = duration(start, end);
  return (result);
}

static struct moment benchmark_seek_read(const char *file_name,
  long int file_size, long int chunk_size, long int n_seeks)
{
  int fd = -1;
  int i = 0;
  int seek_max = (file_size - 1) * chunk_size;
  char *chunk_buffer = NULL;
  struct moment start;
  struct moment end;
  struct moment result;
  memset(&start, 0, sizeof (struct moment));
  memset(&end, 0, sizeof (struct moment));
  memset(&result, 0, sizeof (struct moment));

  fd = open_or_exit(file_name);
  chunk_buffer = malloc_or_exit(chunk_size);
  for (i = 0; i < file_size; i++) {
    random_fill(chunk_buffer, chunk_size);
    write_or_exit(fd, chunk_buffer, chunk_size);
  }
  fsync_or_exit(fd);
  times(&start.cpu);
  gettimeofday(&start.real, NULL);
  for (i = 0; i < n_seeks; i++) {
    lseek_or_exit(fd, random_to(seek_max));
    read_or_exit(fd, chunk_buffer, chunk_size);
  }
  gettimeofday(&end.real, NULL);
  times(&end.cpu);
  free(chunk_buffer);
  close_or_exit(fd);
  unlink_or_exit(file_name);
  result = duration(start, end);
  return (result);
}

static struct moment benchmark_seek_write(const char *file_name,
  long int file_size, long int chunk_size, long int n_seeks)
{
  int fd = -1;
  int i = 0;
  int seek_to = 0;
  char *chunk_buffer = NULL;
  struct moment start;
  struct moment end;
  struct moment result;
  memset(&start, 0, sizeof (struct moment));
  memset(&end, 0, sizeof (struct moment));
  memset(&result, 0, sizeof (struct moment));

  fd = open_or_exit(file_name);
  chunk_buffer = malloc_or_exit(chunk_size);
  memset(chunk_buffer, 0, chunk_size);
  for (i = 0; i < file_size; i++) {
    write_or_exit(fd, chunk_buffer, chunk_size);
  }
  fsync_or_exit(fd);
  times(&start.cpu);
  gettimeofday(&start.real, NULL);
  for (i = 0; i < n_seeks; i++) {
    seek_to = random_to((file_size * chunk_size) - chunk_size);
    lseek_or_exit(fd, seek_to);
    write_or_exit(fd, chunk_buffer, chunk_size);
  }
  fsync_or_exit(fd);
  gettimeofday(&end.real, NULL);
  times(&end.cpu);
  free(chunk_buffer);
  close_or_exit(fd);
  unlink_or_exit(file_name);
  result = duration(start, end);
  return (result);
}

static void check_priority()
{
  int nice = getpriority_or_exit();

  if (nice >= 0) {
    printf("WARNING: priority is %d.  Consider running as root, nice -20.\n", nice);
  }
}

static void check_load()
{
  FILE *load = fopen("/proc/loadavg", "r");
  float one_minute = 0.0;
  float five_minutes = 0.0;
  float fifteen_minutes = 0.0;

  if (load == NULL) {
    fprintf(stderr, "Can't open /proc/loadavg for reading.\n");
    exit(EXIT_CANT_OPEN_LOADAVG);
  }
  // fscanf() is plenty of parser for this job
  fscanf(load, "%f %f %f ", &one_minute, &five_minutes, &fifteen_minutes);
  if (one_minute > 1.0 || five_minutes > 1.0 || fifteen_minutes > 1.0) {
    printf("WARNING: Load average is above 1.0 (%.2f %.2f %.2f).\n",
	   one_minute, five_minutes, fifteen_minutes);
    printf("         Consider running on an unloaded system.\n");

  }
}

static void perform(const char *print_name, long int random_seed,
  struct moment (*benchmark)(const char *, long int, long int, long int),
  const char *file_name, long int file_size, long int chunk_size,
  long int n_seeks)
{
  struct moment result;
  memset(&result, 0, sizeof (struct moment));

  srandom(random_seed);
  sync();
  result = benchmark(file_name, file_size, chunk_size, n_seeks);
  printf("\nBENCHMARK: %s\n", print_name);
  print_moment(&result);
}

static void print_usage(const char *const name)
{
  fprintf(stderr, "Usage: %s\n", name);
  fprintf(stderr, "  [-f <file name>]\n");
  fprintf(stderr, "  [-c <chunk size>]\n");
  fprintf(stderr, "  [-s <file size in chunks>]\n");
  fprintf(stderr, "  [-r <random seed>]\n");
  fprintf(stderr, "  [-n <n seeks>]\n");
}

int main(int argc, char *const argv[])
{
  int opt = 0;
  int file_name_size = 0;
  char *file_name = NULL;
  char *base_name = NULL;
  long int file_size = 1024 * 512; // .5M chunks default
  long int chunk_size = 4 * 1024; // 4K bytes default
  long int total_size = file_size * chunk_size;
  long int random_seed = 9291969; // author's birthday default
  long int n_seeks = 1024; // 1K default
  struct moment result;
  memset(&result, 0, sizeof (struct moment));

  while ((opt = getopt(argc, argv, "f:s:c:r:n:")) != -1) {
    switch (opt) {
      case 'f':
	// copy optarg into malloc'ed storage at file_name, and leak it
	file_name_size = strlen(optarg);
	file_name = malloc_or_exit(file_name_size);
	memset(file_name, 0, file_name_size);
	strncpy(file_name, optarg, file_name_size);
	break;
      case 'c':
	chunk_size = atoi(optarg);
	if (chunk_size <= 0) goto borken;
	break;
      case 's':
	file_size = atoi(optarg);
	if (file_size <= 0) goto borken;
	break;
      case 'r':
	random_seed = atoi(optarg);
	break;
      case 'n':
	n_seeks = atoi(optarg);
	if (n_seeks <= 0) goto borken;
	break;
      case '?':
      borken:
      default:
	print_usage(argv[0]);
        exit(EXIT_COMMAND_LINE_BORKED);
	break;
    }
  }
  if (file_name == NULL) {
    // file name was not specified by user option
    // malloc up room for basename + pid, and leak it
    file_name_size = strlen(argv[0]) + 20;
    file_name = malloc_or_exit(file_name_size);
    memset(file_name, 0, file_name_size);
    base_name = rindex(argv[0], '/');
    if (* base_name == '\0') {
      // there was no '/' in argv[0]
      base_name = argv[0];
    } else {
      // skip leading '/'
      base_name++; 
    }
    sprintf(file_name, "./%s-%u~", base_name, getpid());
  }

  printf("This is fsbench, version %s, built on %s %s.\n",
	 VERSION, __DATE__, __TIME__);
  printf("file name (-f) = %s\n", file_name);
  printf("chunk size (-c) = %ld\n", chunk_size);
  printf("file size in chunks (-s) = %ld\n", file_size);
  total_size = file_size * chunk_size;
  printf("  (file size * chunk size = %ld (%ldG))\n",
	 total_size, total_size / (1024 * 1024 * 1024));
  printf("random seed (-r) = %ld\n", random_seed);
  printf("number of seeks to try (-n) = %ld\n", n_seeks);
  check_priority();
  check_load();
#define ARGS(N, F) \
  (N), random_seed, (F), file_name, file_size, chunk_size, n_seeks
  perform(ARGS("FALLOCATE", benchmark_fallocate));
  perform(ARGS("APPEND", benchmark_append));
  perform(ARGS("UNLINK", benchmark_unlink));
  perform(ARGS("SEEK READ", benchmark_seek_read));
  perform(ARGS("SEEK WRITE", benchmark_seek_write));
#undef ARGS
  printf("\nDone.\n");

  return (0);
}
