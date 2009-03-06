/*
 * modem_caps - probe Hayes-compatible modem capabilities
 *
 * Copyright (C) 2008 Dan Williams <dcbw@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details:
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <getopt.h>
#include <time.h>

#include <glib.h>

#define MODEM_CAP_GSM         0x0001 /* GSM */
#define MODEM_CAP_IS707_A     0x0002 /* CDMA Circuit Switched Data */
#define MODEM_CAP_IS707_P     0x0004 /* CDMA Packet Switched Data */
#define MODEM_CAP_DS          0x0008 /* Data compression selection (v.42bis) */
#define MODEM_CAP_ES          0x0010 /* Error control selection (v.42) */
#define MODEM_CAP_FCLASS      0x0020 /* Group III Fax */
#define MODEM_CAP_MS          0x0040 /* Modulation selection */
#define MODEM_CAP_W           0x0080 /* Wireless commands */      
#define MODEM_CAP_IS856       0x0100 /* CDMA 3G EVDO rev 0 */
#define MODEM_CAP_IS856_A     0x0200 /* CDMA 3G EVDO rev A */

static gboolean verbose = FALSE;
static gboolean quiet = FALSE;
static FILE *logfile = NULL;

struct modem_caps {
	char *name;
	guint32 bits;
};

static struct modem_caps modem_caps[] = {
	{"+CGSM",     MODEM_CAP_GSM},
	{"+CIS707-A", MODEM_CAP_IS707_A},
	{"+CIS707",   MODEM_CAP_IS707_A},
	{"+CIS707P",  MODEM_CAP_IS707_P},
	{"CIS-856",   MODEM_CAP_IS856},
	{"CIS-856-A", MODEM_CAP_IS856_A},
	{"CIS-856A",  MODEM_CAP_IS856_A}, /* Kyocera KPC680 */
	{"+DS",       MODEM_CAP_DS},
	{"+ES",       MODEM_CAP_ES},
	{"+MS",       MODEM_CAP_MS},
	{"+FCLASS",   MODEM_CAP_FCLASS},
	{NULL}
};

static void
printerr_handler (const char *string)
{
	if (logfile)
		fprintf (logfile, "E: %s", string);
	if (!quiet)
		fprintf (stderr, "E: %s", string);
}

static void
print_handler (const char *string)
{
	if (logfile)
		fprintf (logfile, "L: %s", string);
	if (!quiet)
		fprintf (stdout, "L: %s", string);
}

#define verbose(fmt, args...) \
if (verbose) { \
	g_print ("%s(): " fmt "\n", G_STRFUNC, ##args); \
}

static gboolean
modem_send_command (int fd, const char *cmd)
{
	int eagain_count = 1000;
	guint32 i;
	ssize_t written;

	verbose ("Sending: '%s'", cmd);

	for (i = 0; i < strlen (cmd) && eagain_count > 0;) {
		written = write (fd, cmd + i, 1);

		if (written > 0)
			i += written;
		else {
			/* Treat written == 0 as EAGAIN to ensure we break out of the
			 * for() loop eventually.
			 */
			if ((written < 0) && (errno != EAGAIN)) {
				g_printerr ("error writing command: %d\n", errno);
				return FALSE;
			}
			eagain_count--;
			g_usleep (G_USEC_PER_SEC / 10000);
		}
	}

	return eagain_count <= 0 ? FALSE : TRUE;
}

static int
find_terminator (const char *line, const char **terminators)
{
	int i;

	for (i = 0; terminators[i]; i++) {
		if (!strncasecmp (line, terminators[i], strlen (terminators[i])))
			return i;
	}
	return -1;
}

static const char *
find_response (const char *line, const char **responses, int *idx)
{
	int i;

	/* Don't look for a result again if we got one previously */
	for (i = 0; responses[i]; i++) {
		if (strstr (line, responses[i])) {
			*idx = i;
			return line;
		}
	}
	return NULL;
}

#define RESPONSE_LINE_MAX 128
#define SERIAL_BUF_SIZE 2048

/* Return values:
 *
 * -2:    timeout
 * -1:    read error or response not found
 * 0...N: response index in **needles array
 */
static int
modem_wait_reply (int fd,
                  guint32 timeout_secs,
                  const char **needles,
                  const char **terminators,
                  int *out_terminator,
                  char **out_response)
{
	char buf[SERIAL_BUF_SIZE + 1];
	int reply_index = -1, bytes_read;
	GString *result = g_string_sized_new (RESPONSE_LINE_MAX * 2);
	time_t end;
	const char *response = NULL;
	gboolean done = FALSE;

	*out_terminator = -1;
	end = time (NULL) + timeout_secs;
	do {
		bytes_read = read (fd, buf, SERIAL_BUF_SIZE);
		if (bytes_read < 0 && errno != EAGAIN) {
			g_string_free (result, TRUE);
			g_printerr ("read error: %d\n", errno);
			return -1;
		}

		if (bytes_read == 0)
			break; /* EOF */
		else if (bytes_read > 0) {
			char **lines, **iter, *tmp;

			buf[bytes_read] = 0;
			g_string_append (result, buf);

			verbose ("Got: '%s'", result->str);

			lines = g_strsplit_set (result->str, "\n\r", 0);

			/* Find response terminators */
			for (iter = lines; *iter && !done; iter++) {
				tmp = g_strstrip (*iter);
				if (tmp && strlen (tmp)) {
					*out_terminator = find_terminator (tmp, terminators);
					if (*out_terminator >= 0)
						done = TRUE;
				}
			}

			/* If the terminator is found, look for expected responses */
			if (done) {
				for (iter = lines; *iter && (reply_index < 0); iter++) {
					tmp = g_strstrip (*iter);
					if (tmp && strlen (tmp)) {
						response = find_response (tmp, needles, &reply_index);
						if (response) {
							g_free (*out_response);
							*out_response = g_strdup (response);
						}
					}
				}
			}
			g_strfreev (lines);
		}

		if (!done)
			g_usleep (1000);
	} while (!done && (time (NULL) < end) && (result->len <= SERIAL_BUF_SIZE));

	/* Handle timeout */
	if (*out_terminator < 0 && !*out_response)
		reply_index = -2;

	g_string_free (result, TRUE);
	return reply_index;
}

#define GCAP_TAG "+GCAP:"
#define GMM_TAG "+GMM:"

static int
parse_gcap (const char *buf)
{
	const char *p = buf + strlen (GCAP_TAG);
	char **caps, **iter;
	int ret = 0;

	caps = g_strsplit_set (p, " ,\t", 0);
	if (!caps)
		return 0;

	for (iter = caps; *iter; iter++) {
		struct modem_caps *cap = modem_caps;

		while (cap->name) {
			if (!strcmp(cap->name, *iter)) {
				ret |= cap->bits;
				break;
			}
			cap++;
		}
	}

	g_strfreev (caps);
	return ret;
}

static int
parse_gmm (const char *buf)
{
	const char *p = buf + strlen (GMM_TAG);
	char **gmm, **iter;
	gboolean gsm = FALSE;

	gmm = g_strsplit_set (p, " ,\t", 0);
	if (!gmm)
		return 0;

	/* BUSlink SCWi275u USB GPRS modem */
	for (iter = gmm; *iter && !gsm; iter++) {
		if (strstr (*iter, "GSM900") || strstr (*iter, "GSM1800") ||
		    strstr (*iter, "GSM1900") || strstr (*iter, "GSM850"))
			gsm = TRUE;
	}

	g_strfreev (gmm);
	return gsm ? MODEM_CAP_GSM : 0;
}

static int modem_probe_caps(int fd)
{
	const char *gcap_responses[] = { GCAP_TAG, NULL };
	const char *terminators[] = { "OK", "ERROR", "ERR", NULL };
	char *reply = NULL;
	int idx, term_idx, ret = 0;

	if (!modem_send_command (fd, "AT+GCAP\r\n"))
		return -1;

	idx = modem_wait_reply (fd, 3, gcap_responses, terminators, &term_idx, &reply);
	if (0 == term_idx && 0 == idx) {
		/* Success */
		verbose ("GCAP response: %s", reply);
		ret = parse_gcap (reply);
	} else if (1 == term_idx || 2 == term_idx) {
		const char *ati_responses[] = { GCAP_TAG, NULL };

		/* Many cards (ex Sierra 860 & 875) won't accept AT+GCAP but
		 * accept ATI when the SIM is missing.  Often the GCAP info is
		 * in the ATI response too.
		 */
		g_free (reply);
		reply = NULL;

		verbose ("GCAP failed, trying ATI...");
		if (modem_send_command (fd, "ATI\r\n")) {
			idx = modem_wait_reply (fd, 3, ati_responses, terminators, &term_idx, &reply);
			if (0 == term_idx && 0 == idx) {
				verbose ("ATI response: %s", reply);
				ret = parse_gcap (reply);
			}
		}
	} else
		verbose ("timed out waiting for GCAP reply (idx %d, term_idx %d)", idx, term_idx);

	g_free (reply);
	reply = NULL;

	/* Try an alternate method on some hardware (ex BUSlink SCWi275u) */
	if ((idx != -2) && !(ret & MODEM_CAP_GSM) && !(ret & MODEM_CAP_IS707_A)) {
		const char *gmm_responses[] = { GMM_TAG, NULL };

		if (modem_send_command (fd, "AT+GMM\r\n")) {
			idx = modem_wait_reply (fd, 5, gmm_responses, terminators, &term_idx, &reply);
			if (0 == term_idx && 0 == idx) {
				verbose ("GMM response: %s", reply);
				ret |= parse_gmm (reply);
			}
			g_free (reply);
		}
	}

	return ret;
}

static void
print_usage (void)
{
	printf("Usage: probe-modem [options] <device>\n"
	    " --export        export key/value pairs\n"
	    " --delay <ms>    delay before probing (1 to 3000 ms inclusive)\n"
	    " --verbose       print verbose debugging output\n"
	    " --log <file>    log all output\n"
	    " --help\n\n");
}

static void
do_exit (int val)
{
	if (logfile) fclose (logfile);
	exit (val);
}

int
main(int argc, char *argv[])
{
	static const struct option options[] = {
		{ "export", 0, NULL, 'x' },
		{ "delay", required_argument, NULL, 'a' },
		{ "verbose", 0, NULL, 'v' },
		{ "quiet", 0, NULL, 'q' },
		{ "log", required_argument, NULL, 'l' },
		{ "help", 0, NULL, 'h' },
		{}
	};

	const char *device = NULL;
	const char *logpath = NULL;
	const char *delay_str = NULL;
	gboolean export = 0;
	struct termios orig, attrs;
	int fd, caps;
	guint32 delay_ms = 0;

	while (1) {
		int option;

		option = getopt_long (argc, argv, "xvl:qh", options, NULL);
		if (option == -1)
			break;

		switch (option) {
		case 'x':
			export = TRUE;
			break;
		case 'a':
			delay_str = optarg;
			break;
		case 'v':
			verbose = TRUE;
			break;
		case 'l':
			logpath = optarg;
			break;
		case 'q':
			quiet = TRUE;
			break;
		case 'h':
			print_usage ();
			return 0;
		default:
			return 1;
		}
	}

	if (logpath) {
		time_t t = time (NULL);

		logfile = fopen (logpath, "a+");
		if (!logfile) {
			fprintf (stderr, "Couldn't open/create logfile %s", logpath);
			return 2;
		}

		fprintf (logfile, "\n**** Started: %s\n", ctime (&t));
		g_set_printerr_handler (printerr_handler);
	}

	g_set_print_handler (print_handler);

	device = argv[optind];
	if (device == NULL) {
		g_printerr ("no node specified\n");
		do_exit (3);
	}

	verbose ("probing %s", device);

	if (delay_str) {
		unsigned long int tmp;

		tmp = strtoul (delay_str, NULL, 10);
		if (tmp < 1 || tmp > 3000) {
			g_printerr ("Invalid delay: %s\n", delay_str);
			do_exit (3);
		}
		delay_ms = (guint32) tmp;
	}

	if (delay_ms) {
		verbose ("waiting %ums before probing", delay_ms);
		g_usleep (delay_ms * 1000);
	}

	fd = open (device, O_RDWR | O_EXCL | O_NONBLOCK);
	if (fd < 0) {
		g_printerr ("open(%s) failed: %d\n", device, errno);
		do_exit (4);
	}

	if (tcgetattr (fd, &orig)) {
		g_printerr ("tcgetattr(%s): failed %d\n", device, errno);
		do_exit (5);
	}

	memcpy (&attrs, &orig, sizeof (attrs));
	attrs.c_iflag &= ~(IGNCR | ICRNL | IUCLC | INPCK | IXON | IXANY | IGNPAR);
	attrs.c_oflag &= ~(OPOST | OLCUC | OCRNL | ONLCR | ONLRET);
	attrs.c_lflag &= ~(ICANON | XCASE | ECHO | ECHOE | ECHONL);
	attrs.c_lflag &= ~(ECHO | ECHOE);
	attrs.c_cc[VMIN] = 1;
	attrs.c_cc[VTIME] = 0;
	attrs.c_cc[VEOF] = 1;

	attrs.c_cflag &= ~(CBAUD | CSIZE | CSTOPB | CLOCAL | PARENB);
	attrs.c_cflag |= (B9600 | CS8 | CREAD | PARENB);
	
	tcsetattr (fd, TCSANOW, &attrs);
	caps = modem_probe_caps (fd);
	tcsetattr (fd, TCSANOW, &orig);

	if (caps < 0) {
		g_printerr ("%s: couldn't get modem capabilities\n", device);
		printf ("ID_NM_MODEM_PROBED=1\n");
		do_exit (0);
	}

	if (export) {
		if (caps & MODEM_CAP_GSM)
			printf ("ID_NM_MODEM_GSM=1\n");
		if (caps & MODEM_CAP_IS707_A)
			printf ("ID_NM_MODEM_IS707_A=1\n");
		if (caps & MODEM_CAP_IS707_P)
			printf ("ID_NM_MODEM_IS707P=1\n");
		if (caps & MODEM_CAP_IS856)
			printf ("ID_NM_MODEM_IS856=1\n");
		if (caps & MODEM_CAP_IS856_A)
			printf ("ID_NM_MODEM_IS856_A=1\n");
		printf ("ID_NM_MODEM_PROBED=1\n");
	}

	verbose ("%s: caps (0x%X)%s%s%s%s\n", device, caps,
	         caps & MODEM_CAP_GSM     ? " GSM" : "",
	         caps & (MODEM_CAP_IS707_A | MODEM_CAP_IS707_P) ? " CDMA-1x" : "",
	         caps & MODEM_CAP_IS856   ? " EVDOr0" : "",
	         caps & MODEM_CAP_IS856_A ? " EVDOrA" : "");

	do_exit (0);
	return 0;
}

