/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
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

#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <getopt.h>
#include <time.h>
#include <sys/time.h>

#include <glib.h>

#define HUAWEI_VENDOR_ID   0x12D1
#define SIERRA_VENDOR_ID   0x1199
#define ZTE_VENDOR_ID      0x19D2

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
static gboolean nostdout = FALSE;
static FILE *logfile = NULL;

struct modem_caps {
	char *name;
	guint32 bits;
};

static struct modem_caps modem_caps[] = {
	{"+CGSM",     MODEM_CAP_GSM},
	{"+CIS707-A", MODEM_CAP_IS707_A},
	{"+CIS707A",  MODEM_CAP_IS707_A}, /* Cmotech */
	{"+CIS707",   MODEM_CAP_IS707_A},
	{"CIS707",    MODEM_CAP_IS707_A}, /* Qualcomm Gobi */
	{"+CIS707P",  MODEM_CAP_IS707_P},
	{"CIS-856",   MODEM_CAP_IS856},
	{"+IS-856",   MODEM_CAP_IS856},   /* Cmotech */
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
	struct timeval tv;

	gettimeofday (&tv, NULL);
	if (logfile)
		fprintf (logfile, "E: (%lu.%lu) %s", tv.tv_sec, tv.tv_usec, string);
	if (!nostdout)
		fprintf (stderr, "E: (%lu.%lu) %s", tv.tv_sec, tv.tv_usec, string);
}

static void
print_handler (const char *string)
{
	struct timeval tv;

	gettimeofday (&tv, NULL);
	if (logfile)
		fprintf (logfile, "L: (%lu.%lu) %s", tv.tv_sec, tv.tv_usec, string);
	if (!nostdout)
		fprintf (stdout, "L: (%lu.%lu) %s", tv.tv_sec, tv.tv_usec, string);
}

#define verbose(fmt, args...) \
if (verbose) { \
	g_print ("%s(): " fmt "\n", G_STRFUNC, ##args); \
}

static gboolean
modem_send_command (int fd, const char *cmd, gboolean *eio)
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
				if (eio)
					*eio = !!(errno == EIO);
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
#define CGMM_TAG "+CGMM:"
#define HUAWEI_EC121_TAG "+CIS707-A"

static int
parse_gcap (const char *tag, gboolean strip_tag, const char *buf)
{
	const char *p = buf;
	char **caps, **iter;
	int ret = 0;

	if (strip_tag)
		p += strlen (tag);

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
parse_cgmm (const char *buf)
{
	const char *p = buf + strlen (CGMM_TAG);
	char **cgmm, **iter;
	gboolean gsm = FALSE;

	cgmm = g_strsplit_set (p, " ,\t", 0);
	if (!cgmm)
		return 0;

	/* BUSlink SCWi275u USB GPRS modem and some Motorola phones */
	for (iter = cgmm; *iter && !gsm; iter++) {
		if (strstr (*iter, "GSM900") || strstr (*iter, "GSM1800") ||
		    strstr (*iter, "GSM1900") || strstr (*iter, "GSM850"))
			gsm = TRUE;
	}

	g_strfreev (cgmm);
	return gsm ? MODEM_CAP_GSM : 0;
}

static int
g_timeval_subtract (GTimeVal *result, GTimeVal *x, GTimeVal *y)
{
	int nsec;

	/* Perform the carry for the later subtraction by updating y. */
	if (x->tv_usec < y->tv_usec) {
		nsec = (y->tv_usec - x->tv_usec) / G_USEC_PER_SEC + 1;
		y->tv_usec -= G_USEC_PER_SEC * nsec;
		y->tv_sec += nsec;
	}
	if (x->tv_usec - y->tv_usec > G_USEC_PER_SEC) {
		nsec = (x->tv_usec - y->tv_usec) / G_USEC_PER_SEC;
		y->tv_usec += G_USEC_PER_SEC * nsec;
		y->tv_sec -= nsec;
	}

	/* Compute the time remaining to wait.
	   tv_usec is certainly positive. */
	result->tv_sec = x->tv_sec - y->tv_sec;
	result->tv_usec = x->tv_usec - y->tv_usec;

	/* Return 1 if result is negative. */
	return x->tv_sec < y->tv_sec;
}

static int
open_port (const char *port, glong *timeout_ms, struct termios *orig)
{
	int last_err = 0, fd = -1;
	struct termios tmp, attrs;

	/* If a timeout was specified, retry opening the serial port for that
	 * amount of time.  Some devices (nozomi) aren't ready to be opened
	 * even though their device node is created by udev already.
	 */
	do {
		fd = open (port, O_RDWR | O_EXCL | O_NONBLOCK);
		if (fd < 0) {
			last_err = errno;
			g_usleep (300000);
			if (timeout_ms)
				*timeout_ms -= 300;
		}
	} while (fd < 0 && timeout_ms && *timeout_ms > 0);

	if (fd < 0) {
		g_printerr ("open(%s) failed: %d\n", port, last_err);
		return -1;
	}

	if (tcgetattr (fd, orig ? orig : &tmp)) {
		g_printerr ("tcgetattr(%s): failed %d\n", port, errno);
		close (fd);
		return -1;
	}

	memcpy (&attrs, orig ? orig : &tmp, sizeof (attrs));
	attrs.c_iflag &= ~(IGNCR | ICRNL | IUCLC | INPCK | IXON | IXANY | IGNPAR);
	attrs.c_oflag &= ~(OPOST | OLCUC | OCRNL | ONLCR | ONLRET);
	attrs.c_lflag &= ~(ICANON | XCASE | ECHO | ECHOE | ECHONL);
	attrs.c_cc[VMIN] = 1;
	attrs.c_cc[VTIME] = 0;
	attrs.c_cc[VEOF] = 1;

	attrs.c_cflag &= ~(CBAUD | CSIZE | CSTOPB | CLOCAL | PARENB);
	attrs.c_cflag |= (B9600 | CS8 | CREAD | PARENB);

	tcsetattr (fd, TCSANOW, &attrs);
	return fd;
}

static int modem_probe_caps (const char *device, int *fd, glong timeout_ms, unsigned int vid)
{
	const char *gcap_responses[] = { GCAP_TAG, HUAWEI_EC121_TAG, NULL };
	const char *terminators[] = { "OK", "ERROR", "ERR", "+CME ERROR", NULL };
	const char *cpms_responses[] = { "+CPMS:", NULL };
	char *reply = NULL;
	int idx = -1, term_idx = -1, ret = 0;
	gboolean try_ati = FALSE;
	GTimeVal start, end;
	gboolean send_success;

	/* If a timeout was specified, delay just a bit */
	if (timeout_ms > 500) {
		g_usleep (500000);
		timeout_ms -= 500;
	}

	/* ZTE devices need to be told to shut up before probing */
	if (vid == ZTE_VENDOR_ID) {
		gboolean success = FALSE, got_response = FALSE;

		verbose ("Sending ZTE init string...");
		while (timeout_ms > 0 && !success) {
			GTimeVal diff;

			g_get_current_time (&start);

			if (modem_send_command (*fd, "ATE0+CPMS?\r", NULL)) {
				idx = modem_wait_reply (*fd, 2, cpms_responses, terminators, &term_idx, &reply);
				if (idx == 0)
					success = TRUE;

				/* Keep track of whether we get any response at all */
				if ((reply && strlen (reply)) || (term_idx >= 0))
					got_response = TRUE;
				g_free (reply);
				reply = NULL;
			}

			if (!success)
				g_usleep (500000);

			g_get_current_time (&end);
			g_timeval_subtract (&diff, &end, &start);
			timeout_ms -= (diff.tv_sec * 1000) + (diff.tv_usec / 1000);
		}
		verbose ("%s sending ZTE init string", success ? "Success" : "Error");

		/* +CPMS will return error when the card is booting up, and also when
		 * there isn't a SIM.  We can't really distinguish between these two
		 * cases, so we should run AT+GCAP after attempting the ZTE init string
		 * even if the timeout has been reached.  But if the port never returned
		 * a response (even ERROR) to the init string, it's probably not an AT
		 * port so don't take up more time probing it.
		 */
		if (!success && got_response) {
			if (timeout_ms < 1000)
				timeout_ms = 5000;
		}
	}

	while (timeout_ms > 0) {
		GTimeVal diff;
		gulong sleep_time = 100000;
		gboolean eio = FALSE;

		g_get_current_time (&start);

		idx = term_idx = 0;
		send_success = modem_send_command (*fd, "AT+GCAP\r", &eio);
		if (send_success)
			idx = modem_wait_reply (*fd, 2, gcap_responses, terminators, &term_idx, &reply);
		else {
			if (eio) {
				/* re-open the port if it was EIO */
				verbose ("Re-opening port due to EIO...");
				close (*fd);
				*fd = open_port (device, &timeout_ms, NULL);
				if (*fd < 0)
					return 0;  /* no capabilities */
			}
			sleep_time = 300000;
		}

		g_get_current_time (&end);
		g_timeval_subtract (&diff, &end, &start);
		timeout_ms -= (diff.tv_sec * 1000) + (diff.tv_usec / 1000);

		if (send_success) {
			if (0 == term_idx && 0 == idx) {
				/* Success */
				verbose ("GCAP response: %s", reply);
				ret = parse_gcap (gcap_responses[idx], TRUE, reply);
				break;
			} else if (0 == term_idx && 1 == idx) {
				/* Stupid Huawei EC121 that doesn't prefix response with +GCAP: */
				verbose ("GCAP response: %s", reply);
				ret = parse_gcap (gcap_responses[idx], FALSE, reply);
				break;
			} else if (0 == term_idx && -1 == idx) {
				/* Just returned "OK" but no GCAP (Sierra) */
				try_ati = TRUE;
				break;
			} else if (3 == term_idx && -1 == idx) {
				/* No SIM (Huawei) */
				try_ati = TRUE;
				break;
			} else if (1 == term_idx && -1 == idx) {
				/* No SIM (ZTE) */
				try_ati = TRUE;
				break;
			} else if (1 == term_idx || 2 == term_idx) {
				try_ati = TRUE;
			} else
				verbose ("timed out waiting for GCAP reply (idx %d, term_idx %d)", idx, term_idx);
			g_free (reply);
			reply = NULL;
		}

		g_usleep (sleep_time);
		timeout_ms -= sleep_time / 1000;
	}

	if (!ret && try_ati) {
		const char *ati_responses[] = { GCAP_TAG, HUAWEI_EC121_TAG, NULL };

		/* Many cards (ex Sierra 860 & 875) won't accept AT+GCAP but
		 * accept ATI when the SIM is missing.  Often the GCAP info is
		 * in the ATI response too.
		 */
		g_free (reply);
		reply = NULL;

		verbose ("GCAP failed, trying ATI...");
		if (modem_send_command (*fd, "ATI\r", NULL)) {
			idx = modem_wait_reply (*fd, 3, ati_responses, terminators, &term_idx, &reply);
			if (0 == term_idx && 0 == idx) {
				verbose ("ATI response: %s", reply);
				ret = parse_gcap (ati_responses[idx], TRUE, reply);
			} else if (0 == term_idx && 1 == idx) {
				verbose ("ATI response: %s", reply);
				ret = parse_gcap (ati_responses[idx], FALSE, reply);
			}
		}
	}

	g_free (reply);
	reply = NULL;

	/* Try an alternate method on some hardware (ex BUSlink SCWi275u) */
	if ((idx != -2) && !(ret & MODEM_CAP_GSM) && !(ret & MODEM_CAP_IS707_A)) {
		const char *cgmm_responses[] = { CGMM_TAG, NULL };

		if (modem_send_command (*fd, "AT+CGMM\r", NULL)) {
			idx = modem_wait_reply (*fd, 5, cgmm_responses, terminators, &term_idx, &reply);
			if (0 == term_idx && 0 == idx) {
				verbose ("CGMM response: %s", reply);
				ret |= parse_cgmm (reply);
			}
			g_free (reply);
		}
	}

	return ret;
}

static void
print_usage (const char *name)
{
	printf("Usage: %s [options] <device>\n"
	    " --export               export key/value pairs\n"
	    " --timeout <ms>         probing timeout (1 to 8000 ms inclusive)\n"
	    " --delay <ms>           delay before probing (1 to 5000 ms inclusive)\n"
	    " --verbose              print verbose debugging output\n"
	    " --no-stdout            suppress logging to stdout (does not affect logfile output)\n"
	    " --log <file>           log all output\n"
	    " --vid <vid>            USB Vendor ID (optional)\n"
	    " --pid <pid>            USB Product ID (optional)\n"
	    " --usb-interface <num>  USB device interface number (optional)\n"
	    " --driver <name>        Linux kernel device driver (optional)\n"
	    " --help\n\n",
	    name);
}

int
main(int argc, char *argv[])
{
	static const struct option options[] = {
		{ "export", 0, NULL, 'x' },
		{ "timeout", required_argument, NULL, 't' },
		{ "delay", required_argument, NULL, 'a' },
		{ "verbose", 0, NULL, 'v' },
		{ "no-stdout", 0, NULL, 'q' },
		{ "log", required_argument, NULL, 'l' },
		{ "vid", required_argument, NULL, 'e' },
		{ "pid", required_argument, NULL, 'p' },
		{ "usb-interface", required_argument, NULL, 'i' },
		{ "driver", required_argument, NULL, 'd' },
		{ "help", 0, NULL, 'h' },
		{}
	};

	const char *device = NULL;
	const char *logpath = NULL;
	const char *driver = NULL;
	gboolean export = 0;
	struct termios orig;
	int fd = -1, caps, ret = 0;
	glong timeout_ms = 0, delay_ms = 0;
	unsigned int vid = 0, pid = 0, usbif = 0;
	unsigned long int tmp;
	GTimeVal diff, start, end;

	while (1) {
		int option;

		option = getopt_long (argc, argv, "xvl:qh", options, NULL);
		if (option == -1)
			break;

		switch (option) {
		case 'x':
			export = TRUE;
			break;
		case 't':
			tmp = strtoul (optarg, NULL, 10);
			if (tmp < 1 || tmp > 8000) {
				fprintf (stderr, "Invalid timeout: %s\n", optarg);
				return 1;
			}
			timeout_ms = (glong) tmp;
			break;
		case 'a':
			tmp = strtoul (optarg, NULL, 10);
			if (tmp < 1 || tmp > 5000) {
				fprintf (stderr, "Invalid delay: %s\n", optarg);
				return 1;
			}
			delay_ms = (glong) tmp;
			break;
		case 'v':
			verbose = TRUE;
			break;
		case 'l':
			logpath = optarg;
			break;
		case 'e':
			vid = strtoul (optarg, NULL, 0);
			if (vid == 0) {
				fprintf (stderr, "Could not parse USB Vendor ID '%s'", optarg);
				return 1;
			}
			break;
		case 'p':
			pid = strtoul (optarg, NULL, 0);
			if (pid > G_MAXUINT32) {
				fprintf (stderr, "Could not parse USB Product ID '%s'", optarg);
				return 1;
			}
			break;
		case 'i':
			usbif = strtoul (optarg, NULL, 0);
			if (usbif > 50) {
				fprintf (stderr, "Could not parse USB interface number '%s'", optarg);
				return 1;
			}
			break;
		case 'd':
			driver = optarg;
			break;
		case 'q':
			nostdout = TRUE;
			break;
		case 'h':
			print_usage (argv[0]);
			return 0;
		default:
			print_usage (argv[0]);
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
		ret = 3;
		goto exit;
	}

	verbose ("(%s): usb-vid 0x%04x  usb-pid 0x%04x  usb-intf %d  driver '%s'",
	         device, vid, pid, usbif, driver);

	/* Some devices just shouldn't be touched */
	if (vid == HUAWEI_VENDOR_ID && usbif != 0) {
		verbose ("(%s) ignoring Huawei USB interface #%d", device, usbif);
		if (export)
			printf ("ID_NM_MODEM_PROBED=1\n");
		goto exit;
	}

	if (delay_ms) {
		verbose ("waiting %lu ms before probing", delay_ms);
		g_usleep (delay_ms * 1000);
	}

	verbose ("probing %s", device);

	g_get_current_time (&start);

	/* open the modem's port */
	fd = open_port (device, &timeout_ms, &orig);
	if (fd < 0)
		goto exit;

	/* probe it */
	caps = modem_probe_caps (device, &fd, timeout_ms, vid);

	/* note: fd may have been modified by modem_probe_caps */
	if (fd >= 0) {
		/* reset original port attributes */
		tcsetattr (fd, TCSANOW, &orig);
		close (fd);
	}

	g_get_current_time (&end);

	if (caps < 0) {
		g_printerr ("%s: couldn't get modem capabilities\n", device);
		if (export)
			printf ("ID_NM_MODEM_PROBED=1\n");
		goto exit;
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

	g_timeval_subtract (&diff, &end, &start);
	verbose ("%s: caps (0x%X)%s%s%s%s   time: %lums\n", device, caps,
	         caps & MODEM_CAP_GSM     ? " GSM" : "",
	         caps & (MODEM_CAP_IS707_A | MODEM_CAP_IS707_P) ? " CDMA-1x" : "",
	         caps & MODEM_CAP_IS856   ? " EVDOr0" : "",
	         caps & MODEM_CAP_IS856_A ? " EVDOrA" : "",
	         (diff.tv_sec * 1000) + (diff.tv_usec / 1000));

exit:
	if (logfile)
		fclose (logfile);
	return ret;
}

