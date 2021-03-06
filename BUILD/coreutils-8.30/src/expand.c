/* expand - convert tabs to spaces
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

/* By default, convert all tabs to spaces.
   Preserves backspace characters in the output; they decrement the
   column count for tab calculations.
   The default action is equivalent to -8.

   Options:
   --tabs=tab1[,tab2[,...]]
   -t tab1[,tab2[,...]]
   -tab1[,tab2[,...]]	If only one tab stop is given, set the tabs tab1
                        columns apart instead of the default 8.  Otherwise,
                        set the tabs at columns tab1, tab2, etc. (numbered from
                        0); replace any tabs beyond the tab stops given with
                        single spaces.
   --initial
   -i			Only convert initial tabs on each line to spaces.

   David MacKenzie <djm@gnu.ai.mit.edu> */

#include <config.h>

#include <stdio.h>
#include <getopt.h>
#include <sys/types.h>

#include <mbfile.h>

#include "system.h"
#include "die.h"
#include "xstrndup.h"

#include "expand-common.h"

/* The official name of this program (e.g., no 'g' prefix).  */
#define PROGRAM_NAME "expand"

#define AUTHORS proper_name ("David MacKenzie")

static char const shortopts[] = "it:0::1::2::3::4::5::6::7::8::9::";

static struct option const longopts[] =
{
  {"tabs", required_argument, NULL, 't'},
  {"initial", no_argument, NULL, 'i'},
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
Usage: %s [OPTION]... [FILE]...\n\
"),
              program_name);
      fputs (_("\
Convert tabs in each FILE to spaces, writing to standard output.\n\
"), stdout);

      emit_stdin_note ();
      emit_mandatory_arg_note ();

      fputs (_("\
  -i, --initial    do not convert tabs after non blanks\n\
  -t, --tabs=N     have tabs N characters apart, not 8\n\
"), stdout);
      emit_tab_list_info ();
      fputs (HELP_OPTION_DESCRIPTION, stdout);
      fputs (VERSION_OPTION_DESCRIPTION, stdout);
      emit_ancillary_info (PROGRAM_NAME);
    }
  exit (status);
}


/* Change tabs to spaces, writing to stdout.
   Read each file in 'file_list', in order.  */

static void
expand (void)
{
  /* Input stream.  */
  FILE *fp = next_file (NULL);
  mb_file_t mbf;
  mbf_char_t c;
  /* True if the starting locale is utf8.  */
  bool using_utf_locale;

  /* True if the first file contains BOM header.  */
  bool found_bom;
  using_utf_locale=check_utf_locale();

  if (!fp)
    return;
  mbf_init (mbf, fp);
  found_bom=check_bom(fp,&mbf);

  if (using_utf_locale == false && found_bom == true)
  {
    /*try using some predefined locale */

    if (set_utf_locale () != 0)
    {
      error (EXIT_FAILURE, errno, _("cannot set UTF-8 locale"));
    }
  }


  if (found_bom == true)
  {
    print_bom();
  }

  while (true)
    {
      /* If true, perform translations.  */
      bool convert = true;

      /* The following variables have valid values only when CONVERT
         is true:  */

      /* Column of next input character.  */
      uintmax_t column = 0;

      /* Index in TAB_LIST of next tab stop to examine.  */
      size_t tab_index = 0;

      /* Convert a line of text.  */

      do
        {
          while (true) {
            mbf_getc (c, mbf);
            if ((mb_iseof (c)) && (fp = next_file (fp)))
              {
                mbf_init (mbf, fp);
                if (fp!=NULL)
                {
                  if (check_bom(fp,&mbf)==true)
                  {
                    /*Not the first file - check BOM header*/
                    if (using_utf_locale==false && found_bom==false)
                    {
                      /*BOM header in subsequent file but not in the first one. */
                      error (EXIT_FAILURE, errno, _("combination of files with and without BOM header"));
                    }
                  }
                  else
                  {
                    if(using_utf_locale==false && found_bom==true)
                    {
                      /*First file conatined BOM header - locale was switched to UTF
                       *all subsequent files should contain BOM. */
                      error (EXIT_FAILURE, errno, _("combination of files with and without BOM header"));
                    }
                  }
                }
                continue;
              }
            else
              {
                break;
              }
            }


          if (convert)
            {
              if (mb_iseq (c, '\t'))
                {
                  /* Column the next input tab stop is on.  */
                  uintmax_t next_tab_column;
                  bool last_tab IF_LINT (=0);

                  next_tab_column = get_next_tab_column (column, &tab_index,
                                                         &last_tab);

                  if (last_tab)
                    next_tab_column = column + 1;

                  if (next_tab_column < column)
                    die (EXIT_FAILURE, 0, _("input line is too long"));

                  while (++column < next_tab_column)
                    if (putchar (' ') < 0)
                      die (EXIT_FAILURE, errno, _("write error"));

                  mb_setascii (&c, ' ');
                }
              else if (mb_iseq (c, '\b'))
                {
                  /* Go back one column, and force recalculation of the
                     next tab stop.  */
                  column -= !!column;
                  tab_index -= !!tab_index;
                }
              /* A leading control character could make us trip over.  */
              else if (!mb_iscntrl (c))
                {
                  column += mb_width (c);
                  if (!column)
                    die (EXIT_FAILURE, 0, _("input line is too long"));
                }

              convert &= convert_entire_line || mb_isblank (c);
            }

          if (mb_iseof (c))
            return;

          mb_putc (c, stdout);
          if (ferror (stdout))
            die (EXIT_FAILURE, errno, _("write error"));
        }
      while (!mb_iseq (c, '\n'));
    }
}

int
main (int argc, char **argv)
{
  int c;

  initialize_main (&argc, &argv);
  set_program_name (argv[0]);
  setlocale (LC_ALL, "");
  bindtextdomain (PACKAGE, LOCALEDIR);
  textdomain (PACKAGE);

  atexit (close_stdout);
  convert_entire_line = true;

  while ((c = getopt_long (argc, argv, shortopts, longopts, NULL)) != -1)
    {
      switch (c)
        {
        case 'i':
          convert_entire_line = false;
          break;

        case 't':
          parse_tab_stops (optarg);
          break;

        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
          if (optarg)
            parse_tab_stops (optarg - 1);
          else
            {
              char tab_stop[2];
              tab_stop[0] = c;
              tab_stop[1] = '\0';
              parse_tab_stops (tab_stop);
            }
          break;

        case_GETOPT_HELP_CHAR;

        case_GETOPT_VERSION_CHAR (PROGRAM_NAME, AUTHORS);

        default:
          usage (EXIT_FAILURE);
        }
    }

  finalize_tab_stops ();

  set_file_list ( (optind < argc) ? &argv[optind] : NULL);

  expand ();

  cleanup_file_list_stdin ();

  return exit_status;
}
