[NAME]
date \- print or set the system date and time
[DESCRIPTION]
.\" Add any additional description here
[DATE STRING]
.\" NOTE: keep this paragraph in sync with the one in touch.x
The --date=STRING is a mostly free format human readable date string
such as "Sun, 29 Feb 2004 16:21:42 -0800" or "2004-02-29 16:21:42" or
even "next Thursday".  A date string may contain items indicating
calendar date, time of day, time zone, day of week, relative time,
relative date, and numbers.  An empty string indicates the beginning
of the day.  The date string format is more complex than is easily
documented here but is fully described in the info documentation.
[ENVIRONMENT]
.TP
TZ
Specifies the timezone, unless overridden by command line parameters.
If neither is specified, the setting from /etc/localtime is used.
