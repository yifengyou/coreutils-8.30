/* cut - remove parts of lines of files
   Copyright (C) 1997-2018 Free Software Foundation, Inc.
   Copyright (C) 1984 David M. Ihnat

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* Written by David Ihnat.  */

/* POSIX changes, bug fixes, long-named options, and cleanup
   by David MacKenzie <djm@gnu.ai.mit.edu>.

   Rewrite cut_fields and cut_bytes -- Jim Meyering.  */

#include <config.h>

#include <stdio.h>
#include <assert.h>
#include <getopt.h>
#include <sys/types.h>

/* Get mbstate_t, mbrtowc().  */
#if HAVE_WCHAR_H
# include <wchar.h>
#endif
#include "system.h"

#include "error.h"
#include "fadvise.h"
#include "getndelim2.h"
#include "hash.h"
#include "xstrndup.h"

#include "set-fields.h"

/* MB_LEN_MAX is incorrectly defined to be 1 in at least one GCC
   installation; work around this configuration error.        */
#if !defined MB_LEN_MAX || MB_LEN_MAX < 2
# undef MB_LEN_MAX
# define MB_LEN_MAX 16
#endif

/* Some systems, like BeOS, have multibyte encodings but lack mbstate_t.  */
#if HAVE_MBRTOWC && defined mbstate_t
# define mbrtowc(pwc, s, n, ps) (mbrtowc) (pwc, s, n, 0)
#endif

/* The official name of this program (e.g., no 'g' prefix).  */
#define PROGRAM_NAME "cut"

#define AUTHORS \
  proper_name ("David M. Ihnat"), \
  proper_name ("David MacKenzie"), \
  proper_name ("Jim Meyering")

#define FATAL_ERROR(Message)						\
  do									\
    {									\
      error (0, 0, (Message));						\
      usage (EXIT_FAILURE);						\
    }									\
  while (0)

/* Refill the buffer BUF to get a multibyte character. */
#define REFILL_BUFFER(BUF, BUFPOS, BUFLEN, STREAM)                        \
  do                                                                        \
    {                                                                        \
      if (BUFLEN < MB_LEN_MAX && !feof (STREAM) && !ferror (STREAM))        \
        {                                                                \
          memmove (BUF, BUFPOS, BUFLEN);                                \
          BUFLEN += fread (BUF + BUFLEN, sizeof(char), BUFSIZ, STREAM); \
          BUFPOS = BUF;                                                        \
        }                                                                \
    }                                                                        \
  while (0)

/* Get wide character on BUFPOS. BUFPOS is not included after that.
   If byte sequence is not valid as a character, CONVFAIL is true. Otherwise false. */
#define GET_NEXT_WC_FROM_BUFFER(WC, BUFPOS, BUFLEN, MBLENGTH, STATE, CONVFAIL) \
  do                                                                        \
    {                                                                        \
      mbstate_t state_bak;                                                \
                                                                        \
      if (BUFLEN < 1)                                                        \
        {                                                                \
          WC = WEOF;                                                        \
          break;                                                        \
        }                                                                \
                                                                        \
      /* Get a wide character. */                                        \
      CONVFAIL = false;                                                        \
      state_bak = STATE;                                                \
      MBLENGTH = mbrtowc ((wchar_t *)&WC, BUFPOS, BUFLEN, &STATE);        \
                                                                        \
      switch (MBLENGTH)                                                        \
        {                                                                \
        case (size_t)-1:                                                \
        case (size_t)-2:                                                \
          CONVFAIL = true;                                                        \
          STATE = state_bak;                                                \
          /* Fall througn. */                                                \
                                                                        \
        case 0:                                                                \
          MBLENGTH = 1;                                                        \
          break;                                                        \
        }                                                                \
    }                                                                        \
  while (0)


/* Pointer inside RP.  When checking if a byte or field is selected
   by a finite range, we check if it is between CURRENT_RP.LO
   and CURRENT_RP.HI.  If the byte or field index is greater than
   CURRENT_RP.HI then we make CURRENT_RP to point to the next range pair. */
static struct field_range_pair *current_rp;

/* Length of the delimiter given as argument to -d.  */
size_t delimlen;

/* This buffer is used to support the semantics of the -s option
   (or lack of same) when the specified field list includes (does
   not include) the first field.  In both of those cases, the entire
   first field must be read into this buffer to determine whether it
   is followed by a delimiter or a newline before any of it may be
   output.  Otherwise, cut_fields can do the job without using this
   buffer.  */
static char *field_1_buffer;

/* The number of bytes allocated for FIELD_1_BUFFER.  */
static size_t field_1_bufsize;

enum operating_mode
  {
    undefined_mode,

    /* Output bytes that are at the given positions. */
    byte_mode,

    /* Output characters that are at the given positions. */
    character_mode,

    /* Output the given delimiter-separated fields. */
    field_mode
  };

static enum operating_mode operating_mode;

/* If nonzero, when in byte mode, don't split multibyte characters.  */
static int byte_mode_character_aware;

/* If nonzero, the function for single byte locale is work
   if this program runs on multibyte locale. */
static int force_singlebyte_mode;

/* If true do not output lines containing no delimiter characters.
   Otherwise, all such lines are printed.  This option is valid only
   with field mode.  */
static bool suppress_non_delimited;

/* If true, print all bytes, characters, or fields _except_
   those that were specified.  */
static bool complement;

/* The delimiter character for field mode. */
static unsigned char delim;
#if HAVE_WCHAR_H
static wchar_t wcdelim;
#endif

/* The delimiter for each line/record. */
static unsigned char line_delim = '\n';

/* True if the --output-delimiter=STRING option was specified.  */
static bool output_delimiter_specified;

/* The length of output_delimiter_string.  */
static size_t output_delimiter_length;

/* The output field separator string.  Defaults to the 1-character
   string consisting of the input delimiter.  */
static char *output_delimiter_string;

/* True if we have ever read standard input. */
static bool have_read_stdin;

/* For long options that have no equivalent short option, use a
   non-character as a pseudo short option, starting with CHAR_MAX + 1.  */
enum
{
  OUTPUT_DELIMITER_OPTION = CHAR_MAX + 1,
  COMPLEMENT_OPTION
};

static struct option const longopts[] =
{
  {"bytes", required_argument, NULL, 'b'},
  {"characters", required_argument, NULL, 'c'},
  {"fields", required_argument, NULL, 'f'},
  {"delimiter", required_argument, NULL, 'd'},
  {"only-delimited", no_argument, NULL, 's'},
  {"output-delimiter", required_argument, NULL, OUTPUT_DELIMITER_OPTION},
  {"complement", no_argument, NULL, COMPLEMENT_OPTION},
  {"zero-terminated", no_argument, NULL, 'z'},
  {GETOPT_HELP_OPTION_DECL},
  {GETOPT_VERSION_OPTION_DECL},
  {NULL, 0, NULL, 0}
};

void
usage (int status)
{
  if (status != EXIT_SUCCESS)
    emit_try_help ();
  else
    {
      printf (_("\
Usage: %s OPTION... [FILE]...\n\
"),
              program_name);
      fputs (_("\
Print selected parts of lines from each FILE to standard output.\n\
"), stdout);

      emit_stdin_note ();
      emit_mandatory_arg_note ();

      fputs (_("\
  -b, --bytes=LIST        select only these bytes\n\
  -c, --characters=LIST   select only these characters\n\
  -d, --delimiter=DELIM   use DELIM instead of TAB for field delimiter\n\
"), stdout);
      fputs (_("\
  -f, --fields=LIST       select only these fields;  also print any line\n\
                            that contains no delimiter character, unless\n\
                            the -s option is specified\n\
  -n                      with -b: don't split multibyte characters\n\
"), stdout);
      fputs (_("\
      --complement        complement the set of selected bytes, characters\n\
                            or fields\n\
"), stdout);
      fputs (_("\
  -s, --only-delimited    do not print lines not containing delimiters\n\
      --output-delimiter=STRING  use STRING as the output delimiter\n\
                            the default is to use the input delimiter\n\
"), stdout);
      fputs (_("\
  -z, --zero-terminated    line delimiter is NUL, not newline\n\
"), stdout);
      fputs (HELP_OPTION_DESCRIPTION, stdout);
      fputs (VERSION_OPTION_DESCRIPTION, stdout);
      fputs (_("\
\n\
Use one, and only one of -b, -c or -f.  Each LIST is made up of one\n\
range, or many ranges separated by commas.  Selected input is written\n\
in the same order that it is read, and is written exactly once.\n\
"), stdout);
      fputs (_("\
Each range is one of:\n\
\n\
  N     N'th byte, character or field, counted from 1\n\
  N-    from N'th byte, character or field, to end of line\n\
  N-M   from N'th to M'th (included) byte, character or field\n\
  -M    from first to M'th (included) byte, character or field\n\
"), stdout);
      emit_ancillary_info (PROGRAM_NAME);
    }
  exit (status);
}


/* Increment *ITEM_IDX (i.e., a field or byte index),
   and if required CURRENT_RP.  */

static inline void
next_item (uintmax_t *item_idx)
{
  (*item_idx)++;
  if ((*item_idx) > current_rp->hi)
    current_rp++;
}

/* Return nonzero if the K'th field or byte is printable. */

static inline bool
print_kth (uintmax_t k)
{
  return current_rp->lo <= k;
}

/* Return nonzero if K'th byte is the beginning of a range. */

static inline bool
is_range_start_index (uintmax_t k)
{
  return k == current_rp->lo;
}

/* Read from stream STREAM, printing to standard output any selected bytes.  */

static void
cut_bytes (FILE *stream)
{
  uintmax_t byte_idx;	/* Number of bytes in the line so far. */
  /* Whether to begin printing delimiters between ranges for the current line.
     Set after we've begun printing data corresponding to the first range.  */
  bool print_delimiter;

  byte_idx = 0;
  print_delimiter = false;
  current_rp = frp;
  while (true)
    {
      int c;		/* Each character from the file. */

      c = getc (stream);

      if (c == line_delim)
        {
          putchar (c);
          byte_idx = 0;
          print_delimiter = false;
          current_rp = frp;
        }
      else if (c == EOF)
        {
          if (byte_idx > 0)
            putchar (line_delim);
          break;
        }
      else
        {
          next_item (&byte_idx);
          if (print_kth (byte_idx))
            {
              if (output_delimiter_specified)
                {
                  if (print_delimiter && is_range_start_index (byte_idx))
                    {
                      fwrite (output_delimiter_string, sizeof (char),
                              output_delimiter_length, stdout);
                    }
                  print_delimiter = true;
                }

              putchar (c);
            }
        }
    }
}

#if HAVE_MBRTOWC
/* This function is in use for the following case.

   1. Read from the stream STREAM, printing to standard output any selected
   characters.

   2. Read from stream STREAM, printing to standard output any selected bytes,
   without splitting multibyte characters.  */

static void
cut_characters_or_cut_bytes_no_split (FILE *stream)
{
  uintmax_t idx;             /* number of bytes or characters in the line so far. */
  char buf[MB_LEN_MAX + BUFSIZ];  /* For spooling a read byte sequence. */
  char *bufpos;                /* Next read position of BUF. */
  size_t buflen;        /* The length of the byte sequence in buf. */
  wint_t wc;                /* A gotten wide character. */
  size_t mblength;        /* The byte size of a multibyte character which shows
                           as same character as WC. */
  mbstate_t state;        /* State of the stream. */
  bool convfail = false;  /* true, when conversion failed. Otherwise false. */
  /* Whether to begin printing delimiters between ranges for the current line.
     Set after we've begun printing data corresponding to the first range.  */
  bool print_delimiter = false;

  idx = 0;
  buflen = 0;
  bufpos = buf;
  memset (&state, '\0', sizeof(mbstate_t));

  current_rp = frp;

  while (1)
    {
      REFILL_BUFFER (buf, bufpos, buflen, stream);

      GET_NEXT_WC_FROM_BUFFER (wc, bufpos, buflen, mblength, state, convfail);
      (void) convfail;  /* ignore unused */

      if (wc == WEOF)
        {
          if (idx > 0)
            putchar (line_delim);
          break;
        }
      else if (wc == line_delim)
        {
          putchar (line_delim);
          idx = 0;
          print_delimiter = false;
          current_rp = frp;
        }
      else
        {
          next_item (&idx);
          if (print_kth (idx))
            {
              if (output_delimiter_specified)
                {
                  if (print_delimiter && is_range_start_index (idx))
                    {
                      fwrite (output_delimiter_string, sizeof (char),
                              output_delimiter_length, stdout);
                    }
                  print_delimiter = true;
                }
              fwrite (bufpos, mblength, sizeof(char), stdout);
            }
        }

      buflen -= mblength;
      bufpos += mblength;
    }
}
#endif

/* Read from stream STREAM, printing to standard output any selected fields.  */

static void
cut_fields (FILE *stream)
{
  int c;
  uintmax_t field_idx = 1;
  bool found_any_selected_field = false;
  bool buffer_first_field;

  current_rp = frp;

  c = getc (stream);
  if (c == EOF)
    return;

  ungetc (c, stream);
  c = 0;

  /* To support the semantics of the -s flag, we may have to buffer
     all of the first field to determine whether it is 'delimited.'
     But that is unnecessary if all non-delimited lines must be printed
     and the first field has been selected, or if non-delimited lines
     must be suppressed and the first field has *not* been selected.
     That is because a non-delimited line has exactly one field.  */
  buffer_first_field = (suppress_non_delimited ^ !print_kth (1));

  while (1)
    {
      if (field_idx == 1 && buffer_first_field)
        {
          ssize_t len;
          size_t n_bytes;

          len = getndelim2 (&field_1_buffer, &field_1_bufsize, 0,
                            GETNLINE_NO_LIMIT, delim, line_delim, stream);
          if (len < 0)
            {
              free (field_1_buffer);
              field_1_buffer = NULL;
              if (ferror (stream) || feof (stream))
                break;
              xalloc_die ();
            }

          n_bytes = len;
          assert (n_bytes != 0);

          c = 0;

          /* If the first field extends to the end of line (it is not
             delimited) and we are printing all non-delimited lines,
             print this one.  */
          if (to_uchar (field_1_buffer[n_bytes - 1]) != delim)
            {
              if (suppress_non_delimited)
                {
                  /* Empty.  */
                }
              else
                {
                  fwrite (field_1_buffer, sizeof (char), n_bytes, stdout);
                  /* Make sure the output line is newline terminated.  */
                  if (field_1_buffer[n_bytes - 1] != line_delim)
                    putchar (line_delim);
                  c = line_delim;
                }
              continue;
            }
          if (print_kth (1))
            {
              /* Print the field, but not the trailing delimiter.  */
              fwrite (field_1_buffer, sizeof (char), n_bytes - 1, stdout);

              /* With -d$'\n' don't treat the last '\n' as a delimiter.  */
              if (delim == line_delim)
                {
                  int last_c = getc (stream);
                  if (last_c != EOF)
                    {
                      ungetc (last_c, stream);
                      found_any_selected_field = true;
                    }
                }
              else
                found_any_selected_field = true;
            }
          next_item (&field_idx);
        }

      int prev_c = c;

      if (print_kth (field_idx))
        {
          if (found_any_selected_field)
            {
              fwrite (output_delimiter_string, sizeof (char),
                      output_delimiter_length, stdout);
            }
          found_any_selected_field = true;

          while ((c = getc (stream)) != delim && c != line_delim && c != EOF)
            {
              putchar (c);
              prev_c = c;
            }
        }
      else
        {
          while ((c = getc (stream)) != delim && c != line_delim && c != EOF)
            {
              prev_c = c;
            }
        }

      /* With -d$'\n' don't treat the last '\n' as a delimiter.  */
      if (delim == line_delim && c == delim)
        {
          int last_c = getc (stream);
          if (last_c != EOF)
            ungetc (last_c, stream);
          else
            c = last_c;
        }

      if (c == delim)
        next_item (&field_idx);
      else if (c == line_delim || c == EOF)
        {
          if (found_any_selected_field
              || !(suppress_non_delimited && field_idx == 1))
            {
              if (c == line_delim || prev_c != line_delim
                  || delim == line_delim)
                putchar (line_delim);
            }
          if (c == EOF)
            break;
          field_idx = 1;
          current_rp = frp;
          found_any_selected_field = false;
        }
    }
}

#if HAVE_MBRTOWC
static void
cut_fields_mb (FILE *stream)
{
  int c;
  uintmax_t field_idx;
  int found_any_selected_field;
  int buffer_first_field;
  int empty_input;
  char buf[MB_LEN_MAX + BUFSIZ];  /* For spooling a read byte sequence. */
  char *bufpos;                /* Next read position of BUF. */
  size_t buflen;        /* The length of the byte sequence in buf. */
  wint_t wc = 0;        /* A gotten wide character. */
  size_t mblength;        /* The byte size of a multibyte character which shows
                           as same character as WC. */
  mbstate_t state;        /* State of the stream. */
  bool convfail = false;  /* true, when conversion failed. Otherwise false. */

  current_rp = frp;

  found_any_selected_field = 0;
  field_idx = 1;
  bufpos = buf;
  buflen = 0;
  memset (&state, '\0', sizeof(mbstate_t));

  c = getc (stream);
  empty_input = (c == EOF);
  if (c != EOF)
  {
    ungetc (c, stream);
    wc = 0;
  }
  else
    wc = WEOF;

  /* To support the semantics of the -s flag, we may have to buffer
     all of the first field to determine whether it is `delimited.'
     But that is unnecessary if all non-delimited lines must be printed
     and the first field has been selected, or if non-delimited lines
     must be suppressed and the first field has *not* been selected.
     That is because a non-delimited line has exactly one field.  */
  buffer_first_field = (suppress_non_delimited ^ !print_kth (1));

  while (1)
    {
      if (field_idx == 1 && buffer_first_field)
        {
          int len = 0;

          while (1)
            {
              REFILL_BUFFER (buf, bufpos, buflen, stream);

              GET_NEXT_WC_FROM_BUFFER
                (wc, bufpos, buflen, mblength, state, convfail);

              if (wc == WEOF)
                break;

              field_1_buffer = xrealloc (field_1_buffer, len + mblength);
              memcpy (field_1_buffer + len, bufpos, mblength);
              len += mblength;
              buflen -= mblength;
              bufpos += mblength;

              if (!convfail && (wc == line_delim || wc == wcdelim))
                break;
            }

          if (len <= 0 && wc == WEOF)
            break;

          /* If the first field extends to the end of line (it is not
             delimited) and we are printing all non-delimited lines,
             print this one.  */
          if (convfail || (!convfail && wc != wcdelim))
            {
              if (suppress_non_delimited)
                {
                  /* Empty.        */
                }
              else
                {
                  fwrite (field_1_buffer, sizeof (char), len, stdout);
                  /* Make sure the output line is newline terminated.  */
                  if (convfail || (!convfail && wc != line_delim))
                    putchar (line_delim);
                }
              continue;
            }

          if (print_kth (1))
            {
              /* Print the field, but not the trailing delimiter.  */
              fwrite (field_1_buffer, sizeof (char), len - 1, stdout);
              found_any_selected_field = 1;
            }
          next_item (&field_idx);
        }

      if (wc != WEOF)
        {
          if (print_kth (field_idx))
            {
              if (found_any_selected_field)
                {
                  fwrite (output_delimiter_string, sizeof (char),
                          output_delimiter_length, stdout);
                }
              found_any_selected_field = 1;
            }

          while (1)
            {
              REFILL_BUFFER (buf, bufpos, buflen, stream);

              GET_NEXT_WC_FROM_BUFFER
                (wc, bufpos, buflen, mblength, state, convfail);

              if (wc == WEOF)
                break;
              else if (!convfail && (wc == wcdelim || wc == line_delim))
                {
                  buflen -= mblength;
                  bufpos += mblength;
                  break;
                }

              if (print_kth (field_idx))
                fwrite (bufpos, mblength, sizeof(char), stdout);

              buflen -= mblength;
              bufpos += mblength;
            }
        }

      if ((!convfail || wc == line_delim) && buflen < 1)
        wc = WEOF;

      if (!convfail && wc == wcdelim)
        next_item (&field_idx);
      else if (wc == WEOF || (!convfail && wc == line_delim))
        {
          if (found_any_selected_field
              || (!empty_input && !(suppress_non_delimited && field_idx == 1)))
            putchar (line_delim);
          if (wc == WEOF)
            break;
          field_idx = 1;
          current_rp = frp;
          found_any_selected_field = 0;
        }
    }
}
#endif

static void
cut_stream (FILE *stream)
{
#if HAVE_MBRTOWC
  if (MB_CUR_MAX > 1 && !force_singlebyte_mode)
    {
      switch (operating_mode)
        {
        case byte_mode:
          if (byte_mode_character_aware)
            cut_characters_or_cut_bytes_no_split (stream);
          else
            cut_bytes (stream);
          break;

        case character_mode:
          cut_characters_or_cut_bytes_no_split (stream);
          break;

        case field_mode:
          if (delimlen == 1)
            {
              /* Check if we have utf8 multibyte locale, so we can use this
                 optimization because of uniqueness of characters, which is
                 not true for e.g. SJIS */
              char * loc = setlocale(LC_CTYPE, NULL);
              if (loc && (strstr (loc, "UTF-8") || strstr (loc, "utf-8") ||
                  strstr (loc, "UTF8") || strstr (loc, "utf8")))
                {
                  cut_fields (stream);
                  break;
                }
            }
          cut_fields_mb (stream);
          break;

        default:
          abort ();
        }
    }
  else
#endif
    {
      if (operating_mode == field_mode)
        cut_fields (stream);
      else
        cut_bytes (stream);
    }
}

/* Process file FILE to standard output.
   Return true if successful.  */

static bool
cut_file (char const *file)
{
  FILE *stream;

  if (STREQ (file, "-"))
    {
      have_read_stdin = true;
      stream = stdin;
    }
  else
    {
      stream = fopen (file, "r");
      if (stream == NULL)
        {
          error (0, errno, "%s", quotef (file));
          return false;
        }
    }

  fadvise (stream, FADVISE_SEQUENTIAL);

  cut_stream (stream);

  if (ferror (stream))
    {
      error (0, errno, "%s", quotef (file));
      return false;
    }
  if (STREQ (file, "-"))
    clearerr (stream);		/* Also clear EOF. */
  else if (fclose (stream) == EOF)
    {
      error (0, errno, "%s", quotef (file));
      return false;
    }
  return true;
}

int
main (int argc, char **argv)
{
  int optc;
  bool ok;
  bool delim_specified = false;
  char *spec_list_string IF_LINT ( = NULL);
  char mbdelim[MB_LEN_MAX + 1];

  initialize_main (&argc, &argv);
  set_program_name (argv[0]);
  setlocale (LC_ALL, "");
  bindtextdomain (PACKAGE, LOCALEDIR);
  textdomain (PACKAGE);

  atexit (close_stdout);

  operating_mode = undefined_mode;

  /* By default, all non-delimited lines are printed.  */
  suppress_non_delimited = false;

  delim = '\0';
  have_read_stdin = false;

  while ((optc = getopt_long (argc, argv, "b:c:d:f:nsz", longopts, NULL)) != -1)
    {
      switch (optc)
        {
        case 'b':
          /* Build the byte list. */
          if (operating_mode != undefined_mode)
            FATAL_ERROR (_("only one type of list may be specified"));
          operating_mode = byte_mode;
          spec_list_string = optarg;
          break;

        case 'c':
          /* Build the character list. */
          if (operating_mode != undefined_mode)
            FATAL_ERROR (_("only one type of list may be specified"));
          operating_mode = character_mode;
          spec_list_string = optarg;
          break;

        case 'f':
          /* Build the field list. */
          if (operating_mode != undefined_mode)
            FATAL_ERROR (_("only one type of list may be specified"));
          operating_mode = field_mode;
          spec_list_string = optarg;
          break;

        case 'd':
          /* New delimiter. */
          /* Interpret -d '' to mean 'use the NUL byte as the delimiter.'  */
            {
#if HAVE_MBRTOWC
              if(MB_CUR_MAX > 1)
                {
                  mbstate_t state;

                  memset (&state, '\0', sizeof(mbstate_t));
                  delimlen = mbrtowc (&wcdelim, optarg, strnlen(optarg, MB_LEN_MAX), &state);

                  if (delimlen == (size_t)-1 || delimlen == (size_t)-2)
                    ++force_singlebyte_mode;
                  else
                    {
                      delimlen = (delimlen < 1) ? 1 : delimlen;
                      if (wcdelim != L'\0' && *(optarg + delimlen) != '\0')
                        FATAL_ERROR (_("the delimiter must be a single character"));
                      memcpy (mbdelim, optarg, delimlen);
                      mbdelim[delimlen] = '\0';
                      if (delimlen == 1)
                        delim = *optarg;
                    }
                }

              if (MB_CUR_MAX <= 1 || force_singlebyte_mode)
#endif
                {
                  if (optarg[0] != '\0' && optarg[1] != '\0')
                    FATAL_ERROR (_("the delimiter must be a single character"));
                  delim = (unsigned char) optarg[0];
                }
            delim_specified = true;
          }
          break;

        case OUTPUT_DELIMITER_OPTION:
          output_delimiter_specified = true;
          /* Interpret --output-delimiter='' to mean
             'use the NUL byte as the delimiter.'  */
          output_delimiter_length = (optarg[0] == '\0'
                                     ? 1 : strlen (optarg));
          output_delimiter_string = xstrdup (optarg);
          break;

        case 'n':
          byte_mode_character_aware = 1;
          break;

        case 's':
          suppress_non_delimited = true;
          break;

        case 'z':
          line_delim = '\0';
          break;

        case COMPLEMENT_OPTION:
          complement = true;
          break;

        case_GETOPT_HELP_CHAR;

        case_GETOPT_VERSION_CHAR (PROGRAM_NAME, AUTHORS);

        default:
          usage (EXIT_FAILURE);
        }
    }

  if (operating_mode == undefined_mode)
    FATAL_ERROR (_("you must specify a list of bytes, characters, or fields"));

  if (delim_specified && operating_mode != field_mode)
    FATAL_ERROR (_("an input delimiter may be specified only\
 when operating on fields"));

  if (suppress_non_delimited && operating_mode != field_mode)
    FATAL_ERROR (_("suppressing non-delimited lines makes sense\n\
\tonly when operating on fields"));

  set_fields (spec_list_string,
              ( (operating_mode == field_mode) ? 0 : SETFLD_ERRMSG_USE_POS)
              | (complement ? SETFLD_COMPLEMENT : 0) );

  if (!delim_specified)
    {
      delim = '\t';
#ifdef HAVE_MBRTOWC
      wcdelim = L'\t';
      mbdelim[0] = '\t';
      mbdelim[1] = '\0';
      delimlen = 1;
#endif
    }

  if (output_delimiter_string == NULL)
    {
#ifdef HAVE_MBRTOWC
      if (MB_CUR_MAX > 1 && !force_singlebyte_mode)
        {
          output_delimiter_string = xstrdup(mbdelim);
          output_delimiter_length = delimlen;
        }

      if (MB_CUR_MAX <= 1 || force_singlebyte_mode)
#endif
        {
          static char dummy[2];
          dummy[0] = delim;
          dummy[1] = '\0';
          output_delimiter_string = dummy;
          output_delimiter_length = 1;
        }
    }

  if (optind == argc)
    ok = cut_file ("-");
  else
    for (ok = true; optind < argc; optind++)
      ok &= cut_file (argv[optind]);


  if (have_read_stdin && fclose (stdin) == EOF)
    {
      error (0, errno, "-");
      ok = false;
    }

  IF_LINT (reset_fields ());

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
