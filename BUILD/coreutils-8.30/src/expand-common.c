/* expand-common - common functionality for expand/unexapnd
   Copyright (C) 1989-2018 Free Software Foundation, Inc.

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

#include <config.h>

#include <assert.h>
#include <stdio.h>
#include <sys/types.h>
#include <mbfile.h>
#include "system.h"
#include "die.h"
#include "error.h"
#include "fadvise.h"
#include "quote.h"
#include "xstrndup.h"

#include "expand-common.h"

/* If true, convert blanks even after nonblank characters have been
   read on the line.  */
bool convert_entire_line = false;

/* If nonzero, the size of all tab stops.  If zero, use 'tab_list' instead.  */
static uintmax_t tab_size = 0;

/* If nonzero, the size of all tab stops after the last specifed.  */
static uintmax_t extend_size = 0;

/* If nonzero, an increment for additional tab stops after the last specified.*/
static uintmax_t increment_size = 0;

/* The maximum distance between tab stops.  */
size_t max_column_width;

/* Array of the explicit column numbers of the tab stops;
   after 'tab_list' is exhausted, each additional tab is replaced
   by a space.  The first column is column 0.  */
static uintmax_t *tab_list = NULL;

/* The number of allocated entries in 'tab_list'.  */
static size_t n_tabs_allocated = 0;

/* The index of the first invalid element of 'tab_list',
   where the next element can be added.  */
static size_t first_free_tab = 0;

/* Null-terminated array of input filenames.  */
static char **file_list = NULL;

/* Default for 'file_list' if no files are given on the command line.  */
static char *stdin_argv[] =
{
  (char *) "-", NULL
};

/* True if we have ever read standard input.  */
static bool have_read_stdin = false;

/* The desired exit status.  */
int exit_status = EXIT_SUCCESS;



/* Add tab stop TABVAL to the end of 'tab_list'.  */
extern void
add_tab_stop (uintmax_t tabval)
{
  uintmax_t prev_column = first_free_tab ? tab_list[first_free_tab - 1] : 0;
  uintmax_t column_width = prev_column <= tabval ? tabval - prev_column : 0;

  if (first_free_tab == n_tabs_allocated)
    tab_list = X2NREALLOC (tab_list, &n_tabs_allocated);
  tab_list[first_free_tab++] = tabval;

  if (max_column_width < column_width)
    {
      if (SIZE_MAX < column_width)
        die (EXIT_FAILURE, 0, _("tabs are too far apart"));
      max_column_width = column_width;
    }
}

static bool
set_extend_size (uintmax_t tabval)
{
  bool ok = true;

  if (extend_size)
    {
      error (0, 0,
             _("'/' specifier only allowed"
               " with the last value"));
      ok = false;
    }
  extend_size = tabval;

  return ok;
}

static bool
set_increment_size (uintmax_t tabval)
{
  bool ok = true;

  if (increment_size)
    {
      error (0,0,
             _("'+' specifier only allowed"
               " with the last value"));
      ok = false;
    }
  increment_size = tabval;

  return ok;
}

extern int
set_utf_locale (void)
{
      /*try using some predefined locale */
      const char* predef_locales[] = {"C.UTF8","en_US.UTF8","en_GB.UTF8"};

      const int predef_locales_count=3;
      for (int i=0;i<predef_locales_count;i++)
        {
          if (setlocale(LC_ALL,predef_locales[i])!=NULL)
          {
            break;
          }
          else if (i==predef_locales_count-1)
          {
            return 1;
            error (EXIT_FAILURE, errno, _("cannot set UTF-8 locale"));
          }
        }
        return 0;
}

extern bool
check_utf_locale(void)
{
  char* locale = setlocale (LC_CTYPE , NULL);
  if (locale == NULL)
  {
    return false;
  }
  else if (strcasestr(locale, "utf8") == NULL && strcasestr(locale, "utf-8") == NULL)
  {
    return false;
  }
  return true;
}

extern bool
check_bom(FILE* fp, mb_file_t *mbf)
{
  int c;


  c=fgetc(fp);

  /*test BOM header of the first file */
  mbf->bufcount=0;
  if (c == 0xEF)
  {
    c=fgetc(fp);
  }
  else
  {
    if (c != EOF)
    {
      ungetc(c,fp);
    }
    return false;
  }

  if (c == 0xBB)
  {
    c=fgetc(fp);
  }
  else
  {
    if ( c!= EOF )
    {
      mbf->buf[0]=(unsigned char) 0xEF;
      mbf->bufcount=1;
      ungetc(c,fp);
      return false;
    }
    else
    {
      ungetc(0xEF,fp);
      return false;
    }
  }
  if (c == 0xBF)
  {
    mbf->bufcount=0;
    return true;
  }
  else
  {
    if (c != EOF)
    {
      mbf->buf[0]=(unsigned char) 0xEF;
      mbf->buf[1]=(unsigned char) 0xBB;
      mbf->bufcount=2;
      ungetc(c,fp);
      return false;
    }
    else
    {
      mbf->buf[0]=(unsigned char) 0xEF;
      mbf->bufcount=1;
      ungetc(0xBB,fp);
      return false;
    }
  }
  return false;
}

extern void
print_bom(void)
{
  putc (0xEF, stdout);
  putc (0xBB, stdout);
  putc (0xBF, stdout);
}

/* Add the comma or blank separated list of tab stops STOPS
   to the list of tab stops.  */
extern void
parse_tab_stops (char const *stops)
{
  bool have_tabval = false;
  uintmax_t tabval = 0;
  bool extend_tabval = false;
  bool increment_tabval = false;
  char const *num_start = NULL;
  bool ok = true;

  for (; *stops; stops++)
    {
      if (*stops == ',' || isblank (to_uchar (*stops)))
        {
          if (have_tabval)
            {
              if (extend_tabval)
                {
                  if (! set_extend_size (tabval))
                    {
                      ok = false;
                      break;
                    }
                }
              else if (increment_tabval)
                {
                  if (! set_increment_size (tabval))
                    {
                      ok = false;
                      break;
                    }
                }
              else
                add_tab_stop (tabval);
            }
          have_tabval = false;
        }
      else if (*stops == '/')
        {
          if (have_tabval)
            {
              error (0, 0, _("'/' specifier not at start of number: %s"),
                     quote (stops));
              ok = false;
            }
          extend_tabval = true;
          increment_tabval = false;
        }
      else if (*stops == '+')
        {
          if (have_tabval)
            {
              error (0, 0, _("'+' specifier not at start of number: %s"),
                     quote (stops));
              ok = false;
            }
          increment_tabval = true;
          extend_tabval = false;
        }
      else if (ISDIGIT (*stops))
        {
          if (!have_tabval)
            {
              tabval = 0;
              have_tabval = true;
              num_start = stops;
            }

          /* Detect overflow.  */
          if (!DECIMAL_DIGIT_ACCUMULATE (tabval, *stops - '0', uintmax_t))
            {
              size_t len = strspn (num_start, "0123456789");
              char *bad_num = xstrndup (num_start, len);
              error (0, 0, _("tab stop is too large %s"), quote (bad_num));
              free (bad_num);
              ok = false;
              stops = num_start + len - 1;
            }
        }
      else
        {
          error (0, 0, _("tab size contains invalid character(s): %s"),
                 quote (stops));
          ok = false;
          break;
        }
    }

  if (ok && have_tabval)
    {
      if (extend_tabval)
        ok &= set_extend_size (tabval);
      else if (increment_tabval)
        ok &= set_increment_size (tabval);
      else
        add_tab_stop (tabval);
    }

  if (! ok)
    exit (EXIT_FAILURE);
}

/* Check that the list of tab stops TABS, with ENTRIES entries,
   contains only nonzero, ascending values.  */

static void
validate_tab_stops (uintmax_t const *tabs, size_t entries)
{
  uintmax_t prev_tab = 0;

  for (size_t i = 0; i < entries; i++)
    {
      if (tabs[i] == 0)
        die (EXIT_FAILURE, 0, _("tab size cannot be 0"));
      if (tabs[i] <= prev_tab)
        die (EXIT_FAILURE, 0, _("tab sizes must be ascending"));
      prev_tab = tabs[i];
    }

  if (increment_size && extend_size)
    die (EXIT_FAILURE, 0, _("'/' specifier is mutually exclusive with '+'"));
}

/* Called after all command-line options have been parsed,
   and add_tab_stop/parse_tab_stops have been called.
   Will validate the tab-stop values,
   and set the final values to:
   tab-stops = 8 (if no tab-stops given on command line)
   tab-stops = N (if value N specified as the only value).
   tab-stops = distinct values given on command line (if multiple values given).
*/
extern void
finalize_tab_stops (void)
{
  validate_tab_stops (tab_list, first_free_tab);

  if (first_free_tab == 0)
    tab_size = max_column_width = extend_size
                                  ? extend_size : increment_size
                                                  ? increment_size : 8;
  else if (first_free_tab == 1 && ! extend_size && ! increment_size)
    tab_size = tab_list[0];
  else
    tab_size = 0;
}


extern uintmax_t
get_next_tab_column (const uintmax_t column, size_t* tab_index,
                     bool* last_tab)
{
  *last_tab = false;

  /* single tab-size - return multiples of it */
  if (tab_size)
    return column + (tab_size - column % tab_size);

  /* multiple tab-sizes - iterate them until the tab position is beyond
     the current input column. */
  for ( ; *tab_index < first_free_tab ; (*tab_index)++ )
    {
        uintmax_t tab = tab_list[*tab_index];
        if (column < tab)
            return tab;
    }

  /* relative last tab - return multiples of it */
  if (extend_size)
    return column + (extend_size - column % extend_size);

  /* incremental last tab - add increment_size to the previous tab stop */
  if (increment_size)
    {
      uintmax_t end_tab = tab_list[first_free_tab - 1];

      return column + (increment_size - ((column - end_tab) % increment_size));
    }

  *last_tab = true;
  return 0;
}




/* Sets new file-list */
extern void
set_file_list (char **list)
{
  have_read_stdin = false;

  if (!list)
    file_list = stdin_argv;
  else
    file_list = list;
}

/* Close the old stream pointer FP if it is non-NULL,
   and return a new one opened to read the next input file.
   Open a filename of '-' as the standard input.
   Return NULL if there are no more input files.  */

extern FILE *
next_file (FILE *fp)
{
  static char *prev_file;
  char *file;

  if (fp)
    {
      assert (prev_file);
      if (ferror (fp))
        {
          error (0, errno, "%s", quotef (prev_file));
          exit_status = EXIT_FAILURE;
        }
      if (STREQ (prev_file, "-"))
        clearerr (fp);		/* Also clear EOF.  */
      else if (fclose (fp) != 0)
        {
          error (0, errno, "%s", quotef (prev_file));
          exit_status = EXIT_FAILURE;
        }
    }

  while ((file = *file_list++) != NULL)
    {
      if (STREQ (file, "-"))
        {
          have_read_stdin = true;
          fp = stdin;
        }
      else
        fp = fopen (file, "r");
      if (fp)
        {
          prev_file = file;
          fadvise (fp, FADVISE_SEQUENTIAL);
          return fp;
        }
      error (0, errno, "%s", quotef (file));
      exit_status = EXIT_FAILURE;
    }
  return NULL;
}

/* */
extern void
cleanup_file_list_stdin (void)
{
    if (have_read_stdin && fclose (stdin) != 0)
      die (EXIT_FAILURE, errno, "-");
}


extern void
emit_tab_list_info (void)
{
  /* suppress syntax check for emit_mandatory_arg_note() */
  fputs (_("\
  -t, --tabs=LIST  use comma separated list of tab positions\n\
"), stdout);
  fputs (_("\
                     The last specified position can be prefixed with '/'\n\
                     to specify a tab size to use after the last\n\
                     explicitly specified tab stop.  Also a prefix of '+'\n\
                     can be used to align remaining tab stops relative to\n\
                     the last specified tab stop instead of the first column\n\
"), stdout);
}
