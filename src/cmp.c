/* cmp -- compare two files.

   Copyright (C) 1990, 1991, 1992, 1993, 1994, 1995, 1996, 1998, 2001
   Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
   See the GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; see the file COPYING.
   If not, write to the Free Software Foundation,
   59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.  */

#include "system.h"

#include <stdio.h>
#include <cmpbuf.h>
#include <error.h>
#include <freesoft.h>
#include <getopt.h>
#include <inttostr.h>
#include <xalloc.h>

static char const authorship_msgid[] =
  N_("Written by Torbjorn Granlund and David MacKenzie.");

static char const copyright_string[] =
  "Copyright (C) 2001 Free Software Foundation, Inc.";

extern char const version_string[];

static int cmp (void);
static off_t file_position (int);
static size_t block_compare (word const *, word const *);
static size_t block_compare_and_count (word const *, word const *, off_t *);
static void sprintc (char *, int, unsigned char);

/* Name under which this program was invoked.  */
char *program_name;

/* Filenames of the compared files.  */
static char const *file[2];

/* File descriptors of the files.  */
static int file_desc[2];

/* Read buffers for the files.  */
static word *buffer[2];

/* Optimal block size for the files.  */
static size_t buf_size;

/* Initial prefix to ignore for each file.  */
static off_t ignore_initial;

/* Output format:
   type_first_diff
     to print the offset and line number of the first differing bytes
   type_all_diffs
     to print the (decimal) offsets and (octal) values of all differing bytes
   type_status
     to only return an exit status indicating whether the files differ */
static enum
  {
    type_first_diff, type_all_diffs, type_status
  } comparison_type;

/* If nonzero, print values of bytes quoted like cat -t does. */
static bool opt_print_bytes;

static struct option const long_options[] =
{
  {"print-bytes", 0, 0, 'b'},
  {"print-chars", 0, 0, 'c'}, /* obsolescent as of diffutils 2.7.3 */
  {"ignore-initial", 1, 0, 'i'},
  {"verbose", 0, 0, 'l'},
  {"silent", 0, 0, 's'},
  {"quiet", 0, 0, 's'},
  {"version", 0, 0, 'v'},
  {"help", 0, 0, CHAR_MAX + 1},
  {0, 0, 0, 0}
};

static void try_help (char const *, char const *) __attribute__((noreturn));
static void
try_help (char const *reason_msgid, char const *operand)
{
  if (reason_msgid)
    error (0, 0, _(reason_msgid), operand);
  error (2, 0, _("Try `%s --help' for more information."), program_name);
  abort ();
}

static void
check_stdout (void)
{
  if (ferror (stdout))
    error (2, 0, _("write failed"));
  else if (fclose (stdout) != 0)
    error (2, errno, _("standard output"));
}

static char const * const option_help_msgid[] = {
  N_("-b  --print-bytes  Output differing bytes as characters."),
  N_("-i N  --ignore-initial=N  Ignore differences in the first N bytes of input."),
  N_("-l  --verbose  Output offsets and codes of all differing bytes."),
  N_("-s  --quiet  --silent  Output nothing; yield exit status only."),
  N_("-v  --version  Output version info."),
  N_("--help  Output this help."),
  0
};

static void
usage (void)
{
  char const * const *p;

  printf (_("Usage: %s [OPTION]... FILE1 [FILE2]\n"), program_name);
  printf (_("If a FILE is `-' or missing, read standard input.\n"));
  for (p = option_help_msgid;  *p;  p++)
    printf ("  %s\n", _(*p));
  printf (_("Report bugs to <bug-gnu-utils@gnu.org>.\n"));
}

int
main (int argc, char **argv)
{
  int c, f, exit_status;
  struct stat stat_buf[2];
  size_t words_per_buffer;

  initialize_main (&argc, &argv);
  program_name = argv[0];
  setlocale (LC_ALL, "");
  bindtextdomain (PACKAGE, LOCALEDIR);
  textdomain (PACKAGE);
  xalloc_exit_failure = 2;

  /* Parse command line options.  */

  while ((c = getopt_long (argc, argv, "bci:lsv", long_options, 0))
	 != -1)
    switch (c)
      {
      case 'b':
      case 'c': /* 'c' is obsolescent as of diffutils 2.7.3 */
	opt_print_bytes = 1;
	break;

      case 'i':
	{
	  char *numend;
	  uintmax_t val;
	  errno = 0;
	  ignore_initial = val = strtoumax (optarg, &numend, 10);;
	  if (ignore_initial < 0 || ignore_initial != val || errno || *numend)
	    try_help ("invalid --ignore-initial value `%s'", optarg);
	}
	break;

      case 'l':
	comparison_type = type_all_diffs;
	break;

      case 's':
	comparison_type = type_status;
	break;

      case 'v':
	printf ("cmp %s\n%s\n\n%s\n\n%s\n",
		version_string, copyright_string,
		_(free_software_msgid), _(authorship_msgid));
	exit (0);

      case CHAR_MAX + 1:
	usage ();
	check_stdout ();
	exit (0);

      default:
	try_help (0, 0);
      }

  if (optind == argc)
    try_help ("missing operand after `%s'", argv[argc - 1]);

  file[0] = argv[optind++];
  file[1] = optind < argc ? argv[optind++] : "-";

  if (optind < argc)
    try_help ("extra operand `%s'", argv[optind]);

  for (f = 0; f < 2; f++)
    {
      /* If file[1] is "-", treat it first; this avoids a misdiagnostic if
	 stdin is closed and opening file[0] yields file descriptor 0.  */
      int f1 = f ^ (strcmp (file[1], "-") == 0);

      /* Two files with the same name are identical.
	 But wait until we open the file once, for proper diagnostics.  */
      if (f && file_name_cmp (file[0], file[1]) == 0)
	exit (0);

      file_desc[f1] = (strcmp (file[f1], "-") == 0
		       ? STDIN_FILENO
		       : open (file[f1], O_RDONLY, 0));
      if (file_desc[f1] < 0 || fstat (file_desc[f1], stat_buf + f1) != 0)
	{
	  if (file_desc[f1] < 0 && comparison_type == type_status)
	    exit (2);
	  else
	    error (2, errno, "%s", file[f1]);
	}
#if HAVE_SETMODE
      setmode (file_desc[f1], O_BINARY);
#endif
    }

  /* If the files are links to the same inode and have the same file position,
     they are identical.  */

  if (0 < same_file (&stat_buf[0], &stat_buf[1])
      && same_file_attributes (&stat_buf[0], &stat_buf[1])
      && file_position (0) == file_position (1))
    exit (0);

  /* If output is redirected to the null device, we may assume `-s'.  */

  if (comparison_type != type_status)
    {
      struct stat outstat, nullstat;

      if (fstat (STDOUT_FILENO, &outstat) == 0
	  && stat (NULL_DEVICE, &nullstat) == 0
	  && 0 < same_file (&outstat, &nullstat))
	comparison_type = type_status;
    }

  /* If only a return code is needed,
     and if both input descriptors are associated with plain files,
     conclude that the files differ if they have different sizes.  */

  if (comparison_type == type_status
      && S_ISREG (stat_buf[0].st_mode)
      && S_ISREG (stat_buf[1].st_mode))
    {
      off_t s0 = stat_buf[0].st_size - file_position (0);
      off_t s1 = stat_buf[1].st_size - file_position (1);

      if (MAX (0, s0) != MAX (0, s1))
	exit (1);
    }

  /* Get the optimal block size of the files.  */

  buf_size = buffer_lcm (STAT_BLOCKSIZE (stat_buf[0]),
			 STAT_BLOCKSIZE (stat_buf[1]));

  /* Allocate word-aligned buffers, with space for sentinels at the end.  */

  words_per_buffer = (buf_size + 2 * sizeof (word) - 1) / sizeof (word);
  buffer[0] = xmalloc (2 * sizeof (word) * words_per_buffer);
  buffer[1] = buffer[0] + words_per_buffer;

  exit_status = cmp ();

  for (f = 0; f < 2; f++)
    if (close (file_desc[f]) != 0)
      error (2, errno, "%s", file[f]);
  if (exit_status != 0  &&  comparison_type != type_status)
    check_stdout ();
  exit (exit_status);
  return exit_status;
}

/* Compare the two files already open on `file_desc[0]' and `file_desc[1]',
   using `buffer[0]' and `buffer[1]'.
   Return 0 if identical, 1 if different, >1 if error. */

static int
cmp (void)
{
  off_t line_number = 1;	/* Line number (1...) of first difference. */
  off_t char_number = ignore_initial + 1;
				/* Offset (1...) in files of 1st difference. */
  size_t read0, read1;		/* Number of bytes read from each file. */
  size_t first_diff;		/* Offset (0...) in buffers of 1st diff. */
  size_t smaller;		/* The lesser of `read0' and `read1'. */
  word *buffer0 = buffer[0];
  word *buffer1 = buffer[1];
  char *buf0 = (char *) buffer0;
  char *buf1 = (char *) buffer1;
  int ret = 0;
  int f;

  if (ignore_initial)
    for (f = 0; f < 2; f++)
      if (file_position (f) == -1)
	{
	  /* lseek failed; read and discard the ignored initial prefix.  */
	  off_t ig = ignore_initial;
	  do
	    {
	      ssize_t r = read (file_desc[f], buf0, MIN (ig, buf_size));
	      if (!r)
		break;
	      if (r < 0)
		error (2, errno, "%s", file[f]);
	      ig -= r;
	    }
	  while (ig);
	}

  do
    {
      read0 = block_read (file_desc[0], buf0, buf_size);
      if (read0 == (size_t) -1)
	error (2, errno, "%s", file[0]);
      read1 = block_read (file_desc[1], buf1, buf_size);
      if (read1 == (size_t) -1)
	error (2, errno, "%s", file[1]);

      /* Insert sentinels for the block compare.  */

      buf0[read0] = ~buf1[read0];
      buf1[read1] = ~buf0[read1];

      /* If the line number should be written for differing files,
	 compare the blocks and count the number of newlines
	 simultaneously.  */
      first_diff = (comparison_type == type_first_diff
		    ? block_compare_and_count (buffer0, buffer1, &line_number)
		    : block_compare (buffer0, buffer1));

      char_number += first_diff;
      smaller = MIN (read0, read1);

      if (first_diff < smaller)
	{
	  switch (comparison_type)
	    {
	    case type_first_diff:
	      {
		char char_buf[INT_BUFSIZE_BOUND (off_t)];
		char line_buf[INT_BUFSIZE_BOUND (off_t)];
		char const *char_num = offtostr (char_number, char_buf);
		char const *line_num = offtostr (line_number, line_buf);
		if (!opt_print_bytes)
		  /* See POSIX 1003.2-1992 section 4.10.6.1 for this
                     format.  */
		  printf (_("%s %s differ: char %s, line %s\n"),
			  file[0], file[1], char_num, line_num);
		else
		  {
		    unsigned char c0 = buf0[first_diff];
		    unsigned char c1 = buf1[first_diff];
		    char s0[5];
		    char s1[5];
		    sprintc (s0, 0, c0);
		    sprintc (s1, 0, c1);
		    printf (_("%s %s differ: byte %s, line %s is %3o %s %3o %s\n"),
			    file[0], file[1], char_num, line_num,
			    c0, s0, c1, s1);
		}
	      }
	      /* Fall through.  */
	    case type_status:
	      return 1;

	    case type_all_diffs:
	      do
		{
		  unsigned char c0 = buf0[first_diff];
		  unsigned char c1 = buf1[first_diff];
		  if (c0 != c1)
		    {
		      char char_buf[INT_BUFSIZE_BOUND (off_t)];
		      char const *char_num = offtostr (char_number, char_buf);
		      if (!opt_print_bytes)
			/* See POSIX 1003.2-1992 section 4.10.6.1 for
                           this format.  */
			printf ("%6s %3o %3o\n", char_num, c0, c1);
		      else
			{
			  char s0[5];
			  char s1[5];
			  sprintc (s0, 4, c0);
			  sprintc (s1, 0, c1);
			  printf ("%6s %3o %s %3o %s\n",
				  char_num, c0, s0, c1, s1);
			}
		    }
		  char_number++;
		  first_diff++;
		}
	      while (first_diff < smaller);
	      ret = 1;
	      break;
	    }
	}

      if (read0 != read1)
	{
	  if (comparison_type != type_status)
	    /* See POSIX 1003.2-1992 section 4.10.6.2 for this format.  */
	    fprintf (stderr, _("cmp: EOF on %s\n"), file[read1 < read0]);

	  return 1;
	}
    }
  while (read0 == buf_size);
  return ret;
}

/* Compare two blocks of memory P0 and P1 until they differ,
   and count the number of '\n' occurrences in the common
   part of P0 and P1.
   If the blocks are not guaranteed to be different, put sentinels at the ends
   of the blocks before calling this function.

   Return the offset of the first byte that differs.
   Increment *COUNT by the count of '\n' occurrences.  */

static size_t
block_compare_and_count (word const *p0, word const *p1, off_t *count)
{
  word l;		/* One word from first buffer. */
  word const *l0, *l1;	/* Pointers into each buffer. */
  char const *c0, *c1;	/* Pointers for finding exact address. */
  size_t cnt = 0;	/* Number of '\n' occurrences. */
  word nnnn;		/* Newline, sizeof (word) times.  */
  int i;

  nnnn = 0;
  for (i = 0; i < sizeof nnnn; i++)
    nnnn = (nnnn << CHAR_BIT) | '\n';

  /* Find the rough position of the first difference by reading words,
     not bytes.  */

  for (l0 = p0, l1 = p1;  (l = *l0) == *l1;  l0++, l1++)
    {
      l ^= nnnn;
      for (i = 0; i < sizeof l; i++)
	{
	  cnt += ! (unsigned char) l;
	  l >>= CHAR_BIT;
	}
    }

  /* Find the exact differing position (endianness independent).  */

  for (c0 = (char const *) l0, c1 = (char const *) l1;
       *c0 == *c1;
       c0++, c1++)
    cnt += *c0 == '\n';

  *count += cnt;
  return c0 - (char const *) p0;
}

/* Compare two blocks of memory P0 and P1 until they differ.
   If the blocks are not guaranteed to be different, put sentinels at the ends
   of the blocks before calling this function.

   Return the offset of the first byte that differs.  */

static size_t
block_compare (word const *p0, word const *p1)
{
  word const *l0, *l1;
  char const *c0, *c1;

  /* Find the rough position of the first difference by reading words,
     not bytes.  */

  for (l0 = p0, l1 = p1;  *l0 == *l1;  l0++, l1++)
    continue;

  /* Find the exact differing position (endianness independent).  */

  for (c0 = (char const *) l0, c1 = (char const *) l1;
       *c0 == *c1;
       c0++, c1++)
    continue;

  return c0 - (char const *) p0;
}

/* Put into BUF the unsigned char C, making unprintable chars
   visible by quoting like cat -t does.
   Pad with spaces on the right to WIDTH characters.  */

static void
sprintc (char *buf, int width, unsigned char c)
{
  if (! ISPRINT (c))
    {
      if (c >= 128)
	{
	  *buf++ = 'M';
	  *buf++ = '-';
	  c -= 128;
	  width -= 2;
	}
      if (c < 32)
	{
	  *buf++ = '^';
	  c += 64;
	  --width;
	}
      else if (c == 127)
	{
	  *buf++ = '^';
	  c = '?';
	  --width;
	}
    }

  *buf++ = c;
  while (--width > 0)
    *buf++ = ' ';
  *buf = 0;
}

/* Position file F to `ignore_initial' bytes from its initial position,
   and yield its new position.  Don't try more than once.  */

static off_t
file_position (int f)
{
  static bool positioned[2];
  static off_t position[2];

  if (! positioned[f])
    {
      positioned[f] = 1;
      position[f] = lseek (file_desc[f], ignore_initial, SEEK_CUR);
    }
  return position[f];
}
