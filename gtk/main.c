/* main.c
 *
 * $Id: main.c,v 1.96 2000/01/25 04:44:33 guy Exp $
 *
 * Ethereal - Network traffic analyzer
 * By Gerald Combs <gerald@zing.org>
 * Copyright 1998 Gerald Combs
 *
 * Richard Sharpe, 13-Feb-1999, added support for initializing structures
 *                              needed by dissect routines
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 *
 * To do:
 * - Graphs
 * - Check for end of packet in dissect_* routines.
 * - Playback window
 * - Multiple window support
 * - Add cut/copy/paste
 * - Create header parsing routines
 * - Make byte view selections more fancy?
 *
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <gtk/gtk.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef HAVE_IO_H
#include <io.h> /* open/close on win32 */
#endif

#ifdef HAVE_DIRECT_H
#include <direct.h>
#endif

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif

#include <signal.h>

#ifdef NEED_SNPRINTF_H
# ifdef HAVE_STDARG_H
#  include <stdarg.h>
# else
#  include <varargs.h>
# endif
# include "snprintf.h"
#endif

#if defined(HAVE_UCD_SNMP_SNMP_H)
#ifdef HAVE_UCD_SNMP_VERSION_H
#include <ucd-snmp/version.h>
#endif /* HAVE_UCD_SNMP_VERSION_H */
#elif defined(HAVE_SNMP_SNMP_H)
#ifdef HAVE_SNMP_VERSION_H
#include <snmp/version.h>
#endif /* HAVE_SNMP_VERSION_H */
#endif /* SNMP */

#ifdef NEED_STRERROR_H
#include "strerror.h"
#endif

#include "main.h"
#include "timestamp.h"
#include "packet.h"
#include "capture.h"
#include "summary.h"
#include "file.h"
#include "menu.h"
#include "prefs_dlg.h"
#include "column.h"
#include "print.h"
#include "resolv.h"
#include "follow.h"
#include "util.h"
#include "simple_dialog.h"
#include "proto_draw.h"
#include "dfilter.h"
#include "keys.h"
#include "gtkglobals.h"
#include "plugins.h"

FILE        *data_out_file = NULL;
packet_info  pi;
capture_file cf;
GtkWidget   *top_level, *file_sel, *packet_list, *tree_view, *byte_view,
            *prog_bar, *info_bar, *tv_scrollw, *pkt_scrollw;
static GtkWidget	*bv_vscroll_left, *bv_vscroll_right;
GdkFont     *m_r_font, *m_b_font;
guint        main_ctx, file_ctx;
gchar        comp_info_str[256];
gchar       *ethereal_path = NULL;
gchar       *medium_font = MONO_MEDIUM_FONT;
gchar       *bold_font = MONO_BOLD_FONT;
gchar       *last_open_dir = NULL;

ts_type timestamp_type = RELATIVE;

GtkStyle *item_style;

/* Specifies the field currently selected in the GUI protocol tree */
field_info *finfo_selected = NULL;

static void follow_destroy_cb(GtkWidget *win, gpointer data);
static void follow_charset_toggle_cb(GtkWidget *w, gpointer parent_w);
static void follow_load_text(GtkWidget *text, char *filename, guint8 show_type);
static void follow_print_stream(GtkWidget *w, gpointer parent_w);
static char* hfinfo_numeric_format(header_field_info *hfinfo);
static void create_main_window(gint, gint, gint, e_prefs*);

/* About Ethereal window */
void
about_ethereal( GtkWidget *w, gpointer data ) {
  simple_dialog(ESD_TYPE_INFO, NULL,
		"Ethereal - Network Protocol Analyzer\n"
		"Version " VERSION " (C) 1998-2000 Gerald Combs <gerald@zing.org>\n"
                "Compiled with %s\n\n"

		"Check the man page for complete documentation and\n"
		"for the list of contributors.\n"

		"\nSee http://ethereal.zing.org/ for more information.",
                 comp_info_str);
}

/* Follow the TCP stream, if any, to which the last packet that we called
   a dissection routine on belongs (this might be the most recently
   selected packet, or it might be the last packet in the file). */
void
follow_stream_cb( GtkWidget *w, gpointer data ) {
  char      filename1[128+1];
  GtkWidget *streamwindow, *box, *text, *vscrollbar, *table,
  		*filter_te;
  GtkWidget *hbox, *close_bt, *print_bt;
  GtkWidget *b_ascii, *b_ebcdic, *b_hexdump;
  int        tmp_fd;
  gchar     *follow_filter;

  if( pi.ipproto == 6 ) {
    /* we got tcp so we can follow */
    /* Create a temporary file into which to dump the reassembled data
       from the TCP stream, and set "data_out_file" to refer to it, so
       that the TCP code will write to it.

       XXX - it might be nicer to just have the TCP code directly
       append stuff to the text widget for the TCP stream window,
       if we can arrange that said window not pop up until we're
       done. */
    tmp_fd = create_tempfile( filename1, sizeof filename1, "follow");
    if (tmp_fd == -1) {
      simple_dialog(ESD_TYPE_WARN, NULL,
        "Could not create temporary file %s: %s", filename1, strerror(errno));
      return;
    }
    data_out_file = fdopen( tmp_fd, "w" );
    if( data_out_file == NULL ) {
      simple_dialog(ESD_TYPE_WARN, NULL,
        "Could not create temporary file %s: %s", filename1, strerror(errno));
      close(tmp_fd);
      unlink(filename1);
      return;
    }

    /* Create a new filter that matches all packets in the TCP stream,
       and set the display filter entry accordingly */
    reset_tcp_reassembly();
    follow_filter = build_follow_filter( &pi );

    /* set the display filter entry accordingly */
    filter_te = gtk_object_get_data(GTK_OBJECT(w), E_DFILTER_TE_KEY);
    gtk_entry_set_text(GTK_ENTRY(filter_te), follow_filter);

    /* Run the display filter so it goes in effect. */
    filter_packets(&cf, follow_filter);

    /* the data_out_file should now be full of the streams information */
    fclose( data_out_file );

    /* the filename1 file now has all the text that was in the session */
    streamwindow = gtk_window_new( GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_name( streamwindow, "TCP stream window" );

    gtk_signal_connect( GTK_OBJECT(streamwindow), "delete_event",
			GTK_SIGNAL_FUNC(follow_destroy_cb), NULL);
    gtk_signal_connect( GTK_OBJECT(streamwindow), "destroy",
			GTK_SIGNAL_FUNC(follow_destroy_cb), NULL);
			
    if( incomplete_tcp_stream ) {
      gtk_window_set_title( GTK_WINDOW(streamwindow), 
			    "Contents of TCP stream (incomplete)" );
    } else {
      gtk_window_set_title( GTK_WINDOW(streamwindow),
			    "Contents of TCP stream" );
    }
    gtk_widget_set_usize( GTK_WIDGET(streamwindow), DEF_WIDTH, DEF_HEIGHT );
    gtk_container_border_width( GTK_CONTAINER(streamwindow), 2 );

    /* setup the container */
    box = gtk_vbox_new( FALSE, 0 );
    gtk_container_add( GTK_CONTAINER(streamwindow), box );
    gtk_widget_show( box );

    /* set up the table we attach to */
    table = gtk_table_new( 1, 2, FALSE );
    gtk_table_set_col_spacing( GTK_TABLE(table), 0, 2);
    gtk_box_pack_start( GTK_BOX(box), table, TRUE, TRUE, 0 );
    gtk_widget_show( table );

    /* create a text box */
    text = gtk_text_new( NULL, NULL );
    gtk_text_set_editable( GTK_TEXT(text), FALSE);
    gtk_table_attach( GTK_TABLE(table), text, 0, 1, 0, 1,
		      GTK_EXPAND | GTK_SHRINK | GTK_FILL,
		      GTK_EXPAND | GTK_SHRINK | GTK_FILL, 0, 0 );
    gtk_widget_show(text);

    /* Create hbox */
    hbox = gtk_hbox_new( FALSE, 1 );
    gtk_box_pack_end( GTK_BOX(box), hbox, FALSE, FALSE, 0);
    gtk_widget_show(hbox);

#define E_FOLLOW_ASCII_KEY "follow_ascii_key"
#define E_FOLLOW_EBCDIC_KEY "follow_ebcdic_key"
#define E_FOLLOW_HEXDUMP_KEY "follow_hexdump_key"

    /* Create Radio Buttons */
    b_ascii = gtk_radio_button_new_with_label(NULL, "ASCII");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b_ascii), TRUE);
    gtk_object_set_data(GTK_OBJECT(streamwindow), E_FOLLOW_ASCII_KEY, b_ascii);
    gtk_box_pack_start(GTK_BOX(hbox), b_ascii, FALSE, FALSE, 0);
    gtk_signal_connect(GTK_OBJECT(b_ascii), "toggled",
		    GTK_SIGNAL_FUNC(follow_charset_toggle_cb),
		    GTK_OBJECT(streamwindow));
    gtk_widget_show(b_ascii);

    b_ebcdic = gtk_radio_button_new_with_label(
		    gtk_radio_button_group(GTK_RADIO_BUTTON(b_ascii)),
		    "EBCDIC");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b_ebcdic), FALSE);
    gtk_object_set_data(GTK_OBJECT(streamwindow), E_FOLLOW_EBCDIC_KEY, b_ebcdic);
    gtk_box_pack_start(GTK_BOX(hbox), b_ebcdic, FALSE, FALSE, 0);
    gtk_signal_connect(GTK_OBJECT(b_ebcdic), "toggled",
		    GTK_SIGNAL_FUNC(follow_charset_toggle_cb),
		    GTK_OBJECT(streamwindow));
    gtk_widget_show(b_ebcdic);

    b_hexdump = gtk_radio_button_new_with_label(
		    gtk_radio_button_group(GTK_RADIO_BUTTON(b_ascii)),
		    "Hex. Dump");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b_hexdump), FALSE);
    gtk_object_set_data(GTK_OBJECT(streamwindow), E_FOLLOW_HEXDUMP_KEY, b_hexdump);
    gtk_box_pack_start(GTK_BOX(hbox), b_hexdump, FALSE, FALSE, 0);
    gtk_signal_connect(GTK_OBJECT(b_hexdump), "toggled",
		    GTK_SIGNAL_FUNC(follow_charset_toggle_cb),
		    GTK_OBJECT(streamwindow));
    gtk_widget_show(b_hexdump);

    /* Create Close Button */
    close_bt = gtk_button_new_with_label("Close");
    gtk_signal_connect_object(GTK_OBJECT(close_bt), "clicked",
		    GTK_SIGNAL_FUNC(gtk_widget_destroy),
		    GTK_OBJECT(streamwindow));
    gtk_box_pack_end( GTK_BOX(hbox), close_bt, FALSE, FALSE, 0);
    gtk_widget_show( close_bt );

    /* Create Print Button */
    print_bt = gtk_button_new_with_label("Print");
    gtk_signal_connect(GTK_OBJECT(print_bt), "clicked",
                   GTK_SIGNAL_FUNC(follow_print_stream),
                   GTK_OBJECT(streamwindow));
    gtk_box_pack_end( GTK_BOX(hbox), print_bt, FALSE, FALSE, 0);
    gtk_widget_show( print_bt );

    /* create the scrollbar */
    vscrollbar = gtk_vscrollbar_new( GTK_TEXT(text)->vadj );
    gtk_table_attach( GTK_TABLE(table), vscrollbar, 1, 2, 0, 1,
		      GTK_FILL, GTK_EXPAND | GTK_SHRINK | GTK_FILL, 0, 0 );
    gtk_widget_show( vscrollbar );
    gtk_widget_realize( text );

    /* Tuck away the filename and textbox into streamwindow */
#define E_FOLLOW_FILENAME_KEY "follow_filename_key"
#define E_FOLLOW_TEXT_KEY "follow_text_key"

    gtk_object_set_data(GTK_OBJECT(streamwindow), E_FOLLOW_FILENAME_KEY,
		    g_strdup(filename1));
    gtk_object_set_data(GTK_OBJECT(streamwindow), E_FOLLOW_TEXT_KEY, text);

    follow_load_text(text, filename1, 0);

    data_out_file = NULL;
    gtk_widget_show( streamwindow );
  } else {
    simple_dialog(ESD_TYPE_WARN, NULL,
      "Error following stream.  Please make\n"
      "sure you have a TCP packet selected.");
  }
}

/* The destroy call back has the responsibility of
 * unlinking the temporary file */
static void
follow_destroy_cb(GtkWidget *win, gpointer data)
{
	char	*filename;

	filename = (char*) gtk_object_get_data(GTK_OBJECT(win),
						E_FOLLOW_FILENAME_KEY);
	g_assert(filename);

	unlink(filename);
	gtk_widget_destroy(win);
}

#define E_FOLLOW_ASCII_TYPE	0
#define E_FOLLOW_EBCDIC_TYPE	1
#define E_FOLLOW_HEXDUMP_TYPE	2

/* Handles the ASCII/EBCDIC toggling */
static void
follow_charset_toggle_cb(GtkWidget *w, gpointer parent_w)
{
	guint8		show_type = E_FOLLOW_ASCII_TYPE;
	GtkWidget	*b_ascii, *b_ebcdic, *b_hexdump, *text;
	char		*filename;

	b_ascii = (GtkWidget*) gtk_object_get_data(GTK_OBJECT(parent_w),
						   E_FOLLOW_ASCII_KEY);
	b_ebcdic = (GtkWidget*) gtk_object_get_data(GTK_OBJECT(parent_w),
						    E_FOLLOW_EBCDIC_KEY);
	b_hexdump = (GtkWidget*) gtk_object_get_data(GTK_OBJECT(parent_w),
						     E_FOLLOW_HEXDUMP_KEY);
	text = (GtkWidget*) gtk_object_get_data(GTK_OBJECT(parent_w),
						E_FOLLOW_TEXT_KEY);
	filename = (char*) gtk_object_get_data(GTK_OBJECT(parent_w),
						E_FOLLOW_FILENAME_KEY);

	g_assert(b_ascii);
	g_assert(b_ebcdic);
	g_assert(b_hexdump);
	g_assert(text);
	g_assert(filename);

	if (GTK_TOGGLE_BUTTON(b_ebcdic)->active)
		show_type = E_FOLLOW_EBCDIC_TYPE;
	else if (GTK_TOGGLE_BUTTON(b_hexdump)->active)
		show_type = E_FOLLOW_HEXDUMP_TYPE;

	follow_load_text(text, filename, show_type);
}

#define FLT_BUF_SIZE 1024
static void
follow_read_stream(char *filename, guint8 show_type,
   void (*print_line)(char *, int, gboolean, void *), void *arg)
{
  tcp_stream_chunk sc;
  int bcount;
  guint32 client_addr = 0;
  guint16 client_port = 0;
  gboolean is_server;
  guint16 current_pos, global_client_pos = 0, global_server_pos = 0;
  guint16 *global_pos;

  data_out_file = fopen( filename, "r" );
  if( data_out_file ) {
    char buffer[FLT_BUF_SIZE];
    int nchars;
    while(fread(&sc.src_addr, 1, sizeof(sc), data_out_file)) {
      if (client_addr == 0) {
        client_addr = sc.src_addr;
        client_port = sc.src_port;
      }
      if (client_addr == sc.src_addr && client_port == sc.src_port) {
	is_server = FALSE;
	global_pos = &global_client_pos;
      }
      else {
	is_server = TRUE;
	global_pos = &global_server_pos;
      }
        
      while (sc.dlen > 0) {
        bcount = (sc.dlen < FLT_BUF_SIZE) ? sc.dlen : FLT_BUF_SIZE;
        nchars = fread( buffer, 1, bcount, data_out_file );
        if (nchars == 0)
          break;
        sc.dlen -= bcount;
	switch (show_type) {
	case E_FOLLOW_EBCDIC_TYPE:
		/* If our native arch is ASCII, call: */
		EBCDIC_to_ASCII(buffer, nchars);
	case E_FOLLOW_ASCII_TYPE:
		/* If our native arch is EBCDIC, call:
		 * ASCII_TO_EBCDIC(buffer, nchars);
		 */
	  	(*print_line)( buffer, nchars, is_server, arg );
		break;
	case E_FOLLOW_HEXDUMP_TYPE:
		current_pos = 0;
		while (current_pos < nchars)
		{
		    gchar hexbuf[256];
		    gchar hexchars[] = "0123456789abcdef";
		    int i, cur;
		    /* is_server indentation : put 63 spaces at the begenning
		     * of the string */
		    sprintf(hexbuf, is_server ?
			    "                                 "
			    "                              %08X  " :
			    "%08X  ", *global_pos);
		    cur = strlen(hexbuf);
		    for (i=0; i < 16 && current_pos+i < nchars; i++) {
			hexbuf[cur++] = hexchars[(buffer[current_pos+i] & 0xf0) >> 4];
			hexbuf[cur++] = hexchars[buffer[current_pos+i] & 0x0f];
			if (i == 7) {
			    hexbuf[cur++] = ' '; hexbuf[cur++] = ' ';
			}
			else if (i != 15)
			    hexbuf[cur++] = ' ';
		    }
		    current_pos += i;
		    (*global_pos) += i;
		    hexbuf[cur++] = '\n';
		    hexbuf[cur] = 0;
		    (*print_line)( hexbuf, strlen(hexbuf), is_server, arg );
		}
		break;
	}
      }
    }
    if( ferror( data_out_file ) ) {
      simple_dialog(ESD_TYPE_WARN, NULL,
        "Error reading temporary file %s: %s", filename, strerror(errno));
    }
    fclose( data_out_file );
  } else {
    simple_dialog(ESD_TYPE_WARN, NULL,
      "Could not open temporary file %s: %s", filename, strerror(errno));
  }
}

/*
 * XXX - for text printing, we probably want to wrap lines at 80 characters;
 * for PostScript printing, we probably want to wrap them at the appropriate
 * width, and perhaps put some kind of dingbat (to use the technical term)
 * to indicate a wrapped line, along the lines of what's done when displaying
 * this in a window, as per Warren Young's suggestion.
 *
 * For now, we support only text printing.
 */
static void
follow_print_text(char *buffer, int nchars, gboolean is_server, void *arg)
{
  FILE *fh = arg;

  fwrite(buffer, nchars, 1, fh);
}

static void
follow_print_stream(GtkWidget *w, gpointer parent_w)
{
       FILE *fh;
       gboolean to_file;
       char* print_dest;
       char* filename;
       guint8 show_type = E_FOLLOW_ASCII_TYPE;
       GtkWidget *button;

       switch (prefs.pr_dest) {
               case PR_DEST_CMD:
                       print_dest = prefs.pr_cmd;
                       to_file = FALSE;
                       break;

               case PR_DEST_FILE:
                       print_dest = prefs.pr_file;
                       to_file = TRUE;
                       break;
               default: /* "Can't happen" */
                       simple_dialog(ESD_TYPE_WARN, NULL,
                               "Couldn't figure out where to send the print "
                               "job. Check your preferences.");
                       return;
       }

       fh = open_print_dest(to_file, print_dest);
       if (fh == NULL) {
               switch (to_file) {
                       case FALSE:
                               simple_dialog(ESD_TYPE_WARN, NULL,
                                               "Couldn't run print command %s.", prefs.pr_cmd);
                               break;

                       case TRUE:
                               simple_dialog(ESD_TYPE_WARN, NULL, 
                                               file_write_error_message(errno),
                                               prefs.pr_file);
                               break;
               }
               return;
       }

       button = (GtkWidget*) gtk_object_get_data(GTK_OBJECT(parent_w),
                       E_FOLLOW_EBCDIC_KEY);
       if (GTK_TOGGLE_BUTTON(button)->active)
               show_type = E_FOLLOW_EBCDIC_TYPE;
       button = (GtkWidget*) gtk_object_get_data(GTK_OBJECT(parent_w),
                       E_FOLLOW_HEXDUMP_KEY);
       if (GTK_TOGGLE_BUTTON(button)->active)
               show_type = E_FOLLOW_HEXDUMP_TYPE;

       filename = (char*) gtk_object_get_data(GTK_OBJECT(parent_w),
                       E_FOLLOW_FILENAME_KEY);

       if (filename != NULL) {
               print_preamble(fh, PR_FMT_TEXT);
               follow_read_stream(filename, show_type, follow_print_text, fh);
               print_finale(fh, PR_FMT_TEXT);
               close_print_dest(to_file, fh);
       }
       else {
               simple_dialog(ESD_TYPE_WARN, NULL, "Could not find data to print.");
       }
}

static void
follow_add_to_gtk_text(char *buffer, int nchars, gboolean is_server, void *arg)
{
  GtkWidget *text = arg;

  if (is_server)
    gtk_text_insert( GTK_TEXT(text), m_r_font, &prefs.st_server_fg,
            &prefs.st_server_bg, buffer, nchars );
  else
    gtk_text_insert( GTK_TEXT(text), m_r_font, &prefs.st_client_fg,
            &prefs.st_client_bg, buffer, nchars );
}

static void
follow_load_text(GtkWidget *text, char *filename, guint8 show_type)
{
  int bytes_already;

  /* Delete any info already in text box */
  bytes_already = gtk_text_get_length(GTK_TEXT(text));
  if (bytes_already > 0) {
    gtk_text_set_point(GTK_TEXT(text), 0);
    gtk_text_forward_delete(GTK_TEXT(text), bytes_already);
  }

  /* stop the updates while we fill the text box */
  gtk_text_freeze( GTK_TEXT(text) );
  follow_read_stream(filename, show_type, follow_add_to_gtk_text, text);
  gtk_text_thaw( GTK_TEXT(text) );
}

/* Match selected byte pattern */
void
match_selected_cb(GtkWidget *w, gpointer data)
{
    char		*buf;
    GtkWidget		*filter_te;
    char		*ptr, *format, *stringified;
    int			i, dfilter_len, abbrev_len;
    guint8		*c;
    header_field_info	*hfinfo;

    filter_te = gtk_object_get_data(GTK_OBJECT(w), E_DFILTER_TE_KEY);

    if (!finfo_selected) {
	simple_dialog(ESD_TYPE_WARN, NULL,
		      "Error determining selected bytes.  Please make\n"
		      "sure you have selected a field within the tree\n"
		      "view to be matched.");
	return;
    }

    hfinfo = finfo_selected->hfinfo;
    g_assert(hfinfo);
    abbrev_len = strlen(hfinfo->abbrev);

	switch(hfinfo->type) {

		case FT_BOOLEAN:
		        dfilter_len = abbrev_len + 2;
		        buf = g_malloc0(dfilter_len);
			snprintf(buf, dfilter_len, "%s%s", finfo_selected->value.numeric ? "" : "!",
					hfinfo->abbrev);
			break;

		case FT_UINT8:
		case FT_UINT16:
		case FT_UINT24:
		case FT_UINT32:
		case FT_INT8:
		case FT_INT16:
		case FT_INT24:
		case FT_INT32:
			dfilter_len = abbrev_len + 20;
		        buf = g_malloc0(dfilter_len);
			format = hfinfo_numeric_format(hfinfo);
		        snprintf(buf, dfilter_len, format, hfinfo->abbrev, finfo_selected->value.numeric);
			break;

		case FT_IPv4:
			dfilter_len = abbrev_len + 4 + 15 + 1;
		        buf = g_malloc0(dfilter_len);
		        snprintf(buf, dfilter_len, "%s == %s", hfinfo->abbrev,
					ipv4_addr_str(&(finfo_selected->value.ipv4)));
			break;

		case FT_IPXNET:
			dfilter_len = abbrev_len + 15;
			buf = g_malloc0(dfilter_len);
			snprintf(buf, dfilter_len, "%s == 0x%08x", hfinfo->abbrev,
					finfo_selected->value.numeric);
			break;

		case FT_IPv6:
			stringified = ip6_to_str((struct e_in6_addr*) &(finfo_selected->value.ipv6));
			dfilter_len = abbrev_len + 4 + strlen(stringified) + 1;
			buf = g_malloc0(dfilter_len);
			snprintf(buf, dfilter_len, "%s == %s", hfinfo->abbrev,
					stringified);
			break;

		case FT_DOUBLE:
			dfilter_len = abbrev_len + 30;
			buf = g_malloc0(dfilter_len);
			snprintf(buf, dfilter_len, "%s == %f", hfinfo->abbrev,
					finfo_selected->value.floating);
			break;

		case FT_ETHER:
			dfilter_len = abbrev_len + 22;
			buf = g_malloc0(dfilter_len);
			snprintf(buf, dfilter_len, "%s == %s",
					hfinfo->abbrev,
					ether_to_str(finfo_selected->value.ether));
			break;
#if 0

		case FT_ABSOLUTE_TIME:
		case FT_RELATIVE_TIME:
			memcpy(&fi->value.time, va_arg(ap, struct timeval*),
				sizeof(struct timeval));
			break;

		case FT_STRING:
			/* This g_strdup'ed memory is freed in proto_tree_free_node() */
			fi->value.string = g_strdup(va_arg(ap, char*));
			break;

		case FT_TEXT_ONLY:
			; /* nothing */
			break;
#endif
		default:
		    c = cf.pd + finfo_selected->start;
		    buf = g_malloc0(32 + finfo_selected->length * 3);
		    ptr = buf;

		    sprintf(ptr, "frame[%d] == ", finfo_selected->start);
		    ptr = buf+strlen(buf);

		    if (finfo_selected->length == 1) {
			sprintf(ptr, "0x%02x", *c++);
		    }
		    else {
			    for (i=0;i<finfo_selected->length; i++) {
				if (i == 0 ) {
					sprintf(ptr, "%02x", *c++);
				}
				else {
					sprintf(ptr, ":%02x", *c++);
				}
				ptr = buf+strlen(buf);
			    }
		    }
		    break;
	}

    /* create a new one and set the display filter entry accordingly */
    gtk_entry_set_text(GTK_ENTRY(filter_te), buf);

    /* Run the display filter so it goes in effect. */
    filter_packets(&cf, buf);

    /* Don't g_free(buf) here. filter_packets() will do it the next time it's called */
}

static char*
hfinfo_numeric_format(header_field_info *hfinfo)
{
	char *format = NULL;

	/* Pick the proper format string */
	switch(hfinfo->display) {
		case BASE_DEC:
		case BASE_NONE:
		case BASE_OCT: /* I'm lazy */
		case BASE_BIN: /* I'm lazy */
			switch(hfinfo->type) {
				case FT_UINT8:
				case FT_UINT16:
				case FT_UINT24:
				case FT_UINT32:
					format = "%s == %u";
					break;
				case FT_INT8:
				case FT_INT16:
				case FT_INT24:
				case FT_INT32:
					format = "%s == %d";
					break;
				default:
					g_assert_not_reached();
					;
			}
			break;
		case BASE_HEX:
			switch(hfinfo->type) {
				case FT_UINT8:
					format = "%s == 0x%02x";
					break;
				case FT_UINT16:
					format = "%s == 0x%04x";
					break;
				case FT_UINT24:
					format = "%s == 0x%06x";
					break;
				case FT_UINT32:
					format = "%s == 0x%08x";
					break;
				default:
					g_assert_not_reached();
					;
			}
			break;
		default:
			g_assert_not_reached();
			;
	}
	return format;
}


/* Run the current display filter on the current packet set, and
   redisplay. */
static void
filter_activate_cb(GtkWidget *w, gpointer data)
{
  GtkCombo  *filter_cm = gtk_object_get_data(GTK_OBJECT(w), E_DFILTER_CM_KEY);
  GList     *filter_list = gtk_object_get_data(GTK_OBJECT(w), E_DFILTER_FL_KEY);
  GList     *li, *nl = NULL;
  gboolean   add_filter = TRUE;
  
  char *s = gtk_entry_get_text(GTK_ENTRY(w));
  
  /* GtkCombos don't let us get at their list contents easily, so we maintain
     our own filter list, and feed it to gtk_combo_set_popdown_strings when
     a new filter is added. */
  if (filter_packets(&cf, g_strdup(s))) {
    li = g_list_first(filter_list);
    while (li) {
      if (li->data && strcmp(s, li->data) == 0)
        add_filter = FALSE;
      li = li->next;
    }

    if (add_filter) {
      filter_list = g_list_append(filter_list, g_strdup(s));
      li = g_list_first(filter_list);
      while (li) {
        nl = g_list_append(nl, strdup(li->data));
        li = li->next;
      }
      gtk_combo_set_popdown_strings(filter_cm, nl);
      gtk_entry_set_text(GTK_ENTRY(filter_cm->entry), g_list_last(filter_list)->data);
    }
  }
}

/* redisplay with no display filter */
static void
filter_reset_cb(GtkWidget *w, gpointer data)
{
  GtkWidget *filter_te = NULL;

  if ((filter_te = gtk_object_get_data(GTK_OBJECT(w), E_DFILTER_TE_KEY))) {
    gtk_entry_set_text(GTK_ENTRY(filter_te), "");
  }

  filter_packets(&cf, NULL);
}

/* What to do when a list item is selected/unselected */
static void
packet_list_select_cb(GtkWidget *w, gint row, gint col, gpointer evt) {

#ifdef HAVE_LIBPCAP
  if (!sync_mode) {
#endif
    if (cf.wth)
      return; 
#ifdef HAVE_LIBPCAP
  }
#endif
  blank_packetinfo();
  select_packet(&cf, row);
}

static void
packet_list_unselect_cb(GtkWidget *w, gint row, gint col, gpointer evt) {
  unselect_packet(&cf);
}

static void
tree_view_select_row_cb(GtkCTree *ctree, GList *node, gint column, gpointer user_data)
{
	field_info	*finfo;
	int		tree_selected_start = -1;
	int		tree_selected_len = -1;

	g_assert(node);
	finfo = gtk_ctree_node_get_row_data( ctree, GTK_CTREE_NODE(node) );
	if (!finfo) return;

	finfo_selected = finfo;
	tree_selected_start = finfo->start;
	tree_selected_len   = finfo->length;

	packet_hex_print(GTK_TEXT(byte_view), cf.pd, cf.current_frame->cap_len, 
		tree_selected_start, tree_selected_len, cf.current_frame->encoding);
}

static void
tree_view_unselect_row_cb(GtkCTree *ctree, GList *node, gint column, gpointer user_data)
{
	finfo_selected = NULL;
	packet_hex_print(GTK_TEXT(byte_view), cf.pd, cf.current_frame->cap_len, 
		-1, -1, cf.current_frame->encoding);
}

void collapse_all_cb(GtkWidget *widget, gpointer data) {
  if (cf.protocol_tree)
    collapse_all_tree(cf.protocol_tree, tree_view);
}

void expand_all_cb(GtkWidget *widget, gpointer data) {
  if (cf.protocol_tree)
    expand_all_tree(cf.protocol_tree, tree_view);
}

void
set_scrollbar_placement(int pos) /* 0=left, 1=right */
{
	if (pos) {
		gtk_scrolled_window_set_placement( GTK_SCROLLED_WINDOW(pkt_scrollw),
				GTK_CORNER_TOP_LEFT );
		gtk_scrolled_window_set_placement( GTK_SCROLLED_WINDOW(tv_scrollw),
				GTK_CORNER_TOP_LEFT );
		gtk_widget_hide(bv_vscroll_left);
		gtk_widget_show(bv_vscroll_right);
	}
	else {
		gtk_scrolled_window_set_placement( GTK_SCROLLED_WINDOW(pkt_scrollw),
				GTK_CORNER_TOP_RIGHT );
		gtk_scrolled_window_set_placement( GTK_SCROLLED_WINDOW(tv_scrollw),
				GTK_CORNER_TOP_RIGHT );
		gtk_widget_hide(bv_vscroll_right);
		gtk_widget_show(bv_vscroll_left);
	}
}

void
set_plist_sel_browse(gboolean val)
{
	if (finfo_selected)
		unselect_packet(&cf);

	/* Yeah, GTK uses "browse" in the case where we do not, but oh well. I think
	 * "browse" in Ethereal makes more sense than "SINGLE" in GTK+ */
	if (val) {
		gtk_clist_set_selection_mode(GTK_CLIST(packet_list), GTK_SELECTION_SINGLE);
	}
	else {
		gtk_clist_set_selection_mode(GTK_CLIST(packet_list), GTK_SELECTION_BROWSE);
	}
}

void
set_ptree_sel_browse(gboolean val)
{
	/* Yeah, GTK uses "browse" in the case where we do not, but oh well. I think
	 * "browse" in Ethereal makes more sense than "SINGLE" in GTK+ */
	if (val) {
		gtk_clist_set_selection_mode(GTK_CLIST(tree_view), GTK_SELECTION_SINGLE);
	}
	else {
		gtk_clist_set_selection_mode(GTK_CLIST(tree_view), GTK_SELECTION_BROWSE);
	}
}

void
set_ptree_line_style(gint style)
{
	/* I'm using an assert here since the preferences code limits
	 * the user input, both in the GUI and when reading the preferences file.
	 * If the value is incorrect, it's a program error, not a user-initiated error.
	 */
	g_assert(style >= GTK_CTREE_LINES_NONE && style <= GTK_CTREE_LINES_TABBED);
	gtk_ctree_set_line_style( GTK_CTREE(tree_view), style );
}

void
set_ptree_expander_style(gint style)
{
	/* I'm using an assert here since the preferences code limits
	 * the user input, both in the GUI and when reading the preferences file.
	 * If the value is incorrect, it's a program error, not a user-initiated error.
	 */
	g_assert(style >= GTK_CTREE_EXPANDER_NONE && style <= GTK_CTREE_EXPANDER_CIRCULAR);
	gtk_ctree_set_expander_style( GTK_CTREE(tree_view), style );
}
	

void
file_quit_cmd_cb (GtkWidget *widget, gpointer data) {
  /* If we have a capture file open, and it's a temporary file,
     unlink it. */
  if (cf.filename != NULL && cf.is_tempfile)
	unlink(cf.filename);
  gtk_exit(0);
}

/* call initialization routines at program startup time */
static void
ethereal_proto_init(void) {
  init_dissect_rpc();
  proto_init();
  init_dissect_udp();
  dfilter_init();
#ifdef HAVE_PLUGINS
  init_plugins();
#endif
}

static void
ethereal_proto_cleanup(void) {
	proto_cleanup();
	dfilter_cleanup();
}

static void 
print_usage(void) {

  fprintf(stderr, "This is GNU " PACKAGE " " VERSION ", compiled with %s\n",
	  comp_info_str);
#ifdef HAVE_LIBPCAP
  fprintf(stderr, "%s [ -vh ] [ -kQS ] [ -b <bold font> ] [ -B <byte view height> ]\n",
	  PACKAGE);
  fprintf(stderr, "\t[ -c count ] [ -D ] [ -f <capture filter> ] [ -i interface ]\n");
  fprintf(stderr, "\t[ -m <medium font> ] [ -n ] [ -P <packet list height> ] [ -r infile ]\n");
  fprintf(stderr, "\t[ -R <read filter> ] [ -s snaplen ] [ -t <time stamp format> ]\n");
  fprintf(stderr, "\t[ -T <tree view height> ] [ -w savefile ]\n");
#else
  fprintf(stderr, "%s [ -vh ] [ -b <bold font> ] [ -B <byte view height> ]\n",
	  PACKAGE);
  fprintf(stderr, "\t[ -m <medium font> ] [ -n ] [ -P <packet list height> ] [ -r infile ]\n");
  fprintf(stderr, "\t[ -R <read filter> ] [ -t <time stamp format> ]\n");
  fprintf(stderr, "\t[ -T <tree view height> ]\n");
#endif
}

/* And now our feature presentation... [ fade to music ] */
int
main(int argc, char *argv[])
{
#ifdef HAVE_LIBPCAP
  char                *command_name;
#endif
  char                *s;
  int                  i;
#ifndef WIN32
  int                  opt;
  extern char         *optarg;
  gboolean             arg_error = FALSE;
#endif
#ifdef HAVE_LIBPCAP
  extern char          pcap_version[];
#endif
  char                *pf_path;
  int                  pf_open_errno = 0;
  int                  err;
#ifdef HAVE_LIBPCAP
  gboolean             start_capture = FALSE;
  gchar               *save_file = NULL;
  GList               *if_list;
  gchar                err_str[PCAP_ERRBUF_SIZE];
#else
  gboolean             capture_option_specified = FALSE;
#endif
  gint                 pl_size = 280, tv_size = 95, bv_size = 75;
  gchar               *rc_file, *cf_name = NULL, *rfilter = NULL;
  dfilter             *rfcode = NULL;
  gboolean             rfilter_parse_failed = FALSE;
  e_prefs             *prefs;

  ethereal_path = argv[0];

#ifdef HAVE_LIBPCAP
  command_name = get_basename(ethereal_path);
  /* Set "capture_child" to indicate whether this is going to be a child
     process for a "-S" capture. */
  capture_child = (strcmp(command_name, CHILD_NAME) == 0);
#endif

  /* If invoked with the "-G" flag, we dump out a glossary of
     display filter symbols.

     We must do this before calling "gtk_init()", because "gtk_init()"
     tries to open an X display, and we don't want to have to do any X
     stuff just to do a build.

     Given that we call "gtk_init()" before doing the regular argument
     list processing, so that it can handle X and GTK+ arguments and
     remove them from the list at which we look, this means we must do
     this before doing the regular argument list processing, as well.

     This means that:

	you must give the "-G" flag as the first flag on the command line;

	you must give it as "-G", nothing more, nothing less;

	any arguments after the "-G" flag will not be used. */
  if (argc >= 2 && strcmp(argv[1], "-G") == 0) {
    ethereal_proto_init();
    proto_registrar_dump();
    exit(0);
  }

  /* Let GTK get its args */
  gtk_init (&argc, &argv);
  
  prefs = read_prefs(&pf_path);
  if (pf_path != NULL) {
    /* The preferences file exists, but couldn't be opened; "pf_path" is
       its pathname.  Remember "errno", as that says why the attempt
       failed. */
    pf_open_errno = errno;
  }

  /* Initialize the capture file struct */
  cf.plist		= NULL;
  cf.plist_end		= NULL;
  cf.wth		= NULL;
  cf.fh			= NULL;
  cf.filename		= NULL;
  cf.user_saved		= FALSE;
  cf.is_tempfile	= FALSE;
  cf.rfcode		= NULL;
  cf.dfilter		= NULL;
  cf.dfcode		= NULL;
#ifdef HAVE_LIBPCAP
  cf.cfilter		= g_strdup(EMPTY_FILTER);
#endif
  cf.iface		= NULL;
  cf.save_file		= NULL;
  cf.save_file_fd	= -1;
  cf.snap		= WTAP_MAX_PACKET_SIZE;
  cf.count		= 0;
  cf.cinfo.num_cols	= prefs->num_cols;
  cf.cinfo.col_fmt      = (gint *) g_malloc(sizeof(gint) * cf.cinfo.num_cols);
  cf.cinfo.fmt_matx	= (gboolean **) g_malloc(sizeof(gboolean *) * cf.cinfo.num_cols);
  cf.cinfo.col_width	= (gint *) g_malloc(sizeof(gint) * cf.cinfo.num_cols);
  cf.cinfo.col_title    = (gchar **) g_malloc(sizeof(gchar *) * cf.cinfo.num_cols);
  cf.cinfo.col_data	= (gchar **) g_malloc(sizeof(gchar *) * cf.cinfo.num_cols);

  /* Assemble the compile-time options */
  snprintf(comp_info_str, 256,
#ifdef GTK_MAJOR_VERSION
    "GTK+ %d.%d.%d, %s%s, %s%s, %s%s", GTK_MAJOR_VERSION, GTK_MINOR_VERSION,
    GTK_MICRO_VERSION,
#else
    "GTK+ (version unknown), %s%s, %s%s, %s%s",
#endif

#ifdef HAVE_LIBPCAP
   "with libpcap ", pcap_version,
#else
   "without libpcap", "",
#endif

#ifdef HAVE_LIBZ
#ifdef ZLIB_VERSION
   "with libz ", ZLIB_VERSION,
#else /* ZLIB_VERSION */
   "with libz ", "(version unknown)",
#endif /* ZLIB_VERSION */
#else /* HAVE_LIBZ */
   "without libz", "",
#endif /* HAVE_LIBZ */

/* Oh, this is pretty */
#if defined(HAVE_UCD_SNMP_SNMP_H)
#ifdef HAVE_UCD_SNMP_VERSION_H
   "with UCD SNMP ", VersionInfo
#else /* HAVE_UCD_SNMP_VERSION_H */
   "with UCD SNMP ", "(version unknown)"
#endif /* HAVE_UCD_SNMP_VERSION_H */
#elif defined(HAVE_SNMP_SNMP_H)
#ifdef HAVE_SNMP_VERSION_H
   "with CMU SNMP ", snmp_Version()
#else /* HAVE_SNMP_VERSION_H */
   "with CMU SNMP ", "(version unknown)"
#endif /* HAVE_SNMP_VERSION_H */
#else /* no SNMP */
   "without SNMP", ""
#endif
   );

#ifndef WIN32
  /* Now get our args */
  while ((opt = getopt(argc, argv, "b:B:c:Df:hi:km:nP:Qr:R:Ss:t:T:w:W:v")) != EOF) {
    switch (opt) {
      case 'b':	       /* Bold font */
	bold_font = g_strdup(optarg);
	break;
      case 'B':        /* Byte view pane height */
        bv_size = atoi(optarg);
        break;
      case 'c':        /* Capture xxx packets */
#ifdef HAVE_LIBPCAP
        cf.count = atoi(optarg);
#else
        capture_option_specified = TRUE;
        arg_error = TRUE;
#endif
        break;
      case 'D':        /* Turn off DSCP printing */
	g_ip_dscp_actif = FALSE;
	break;
      case 'f':
#ifdef HAVE_LIBPCAP
	if (cf.cfilter)
		g_free(cf.cfilter);
	cf.cfilter = g_strdup(optarg);
#else
        capture_option_specified = TRUE;
        arg_error = TRUE;
#endif
	break;
      case 'h':        /* Print help and exit */
	print_usage();
	exit(0);
        break;
      case 'i':        /* Use interface xxx */
#ifdef HAVE_LIBPCAP
        cf.iface = g_strdup(optarg);
#else
        capture_option_specified = TRUE;
        arg_error = TRUE;
#endif
        break;
      case 'k':        /* Start capture immediately */
#ifdef HAVE_LIBPCAP
        start_capture = TRUE;
#else
        capture_option_specified = TRUE;
        arg_error = TRUE;
#endif
        break;
      case 'm':        /* Medium font */
	medium_font = g_strdup(optarg);
	break;
      case 'n':        /* No name resolution */
	g_resolving_actif = 0;
	break;
      case 'P':        /* Packet list pane height */
        pl_size = atoi(optarg);
        break;
      case 'Q':        /* Quit after capture (just capture to file) */
#ifdef HAVE_LIBPCAP
        quit_after_cap = 1;
        start_capture = TRUE;  /*** -Q implies -k !! ***/
#else
        capture_option_specified = TRUE;
        arg_error = TRUE;
#endif
        break;
      case 'r':        /* Read capture file xxx */
	/* We may set "last_open_dir" to "cf_name", and if we change
	   "last_open_dir" later, we free the old value, so we have to
	   set "cf_name" to something that's been allocated. */
        cf_name = g_strdup(optarg);
        break;
      case 'R':        /* Read file filter */
        rfilter = optarg;
        break;
      case 's':        /* Set the snapshot (capture) length */
#ifdef HAVE_LIBPCAP
        cf.snap = atoi(optarg);
#else
        capture_option_specified = TRUE;
        arg_error = TRUE;
#endif
        break;
      case 'S':        /* "Sync" mode: used for following file ala tail -f */
#ifdef HAVE_LIBPCAP
        sync_mode = TRUE;
#else
        capture_option_specified = TRUE;
        arg_error = TRUE;
#endif
        break;
      case 't':        /* Time stamp type */
        if (strcmp(optarg, "r") == 0)
          timestamp_type = RELATIVE;
        else if (strcmp(optarg, "a") == 0)
          timestamp_type = ABSOLUTE;
        else if (strcmp(optarg, "d") == 0)
          timestamp_type = DELTA;
        else {
          fprintf(stderr, "ethereal: Invalid time stamp type \"%s\"\n",
            optarg);
          fprintf(stderr, "It must be \"r\" for relative, \"a\" for absolute,\n");
          fprintf(stderr, "or \"d\" for delta.\n");
          exit(1);
        }
        break;
      case 'T':        /* Tree view pane height */
        tv_size = atoi(optarg);
        break;
      case 'v':        /* Show version and exit */
        printf("%s %s, with %s\n", PACKAGE, VERSION, comp_info_str);
        exit(0);
        break;
      case 'w':        /* Write to capture file xxx */
#ifdef HAVE_LIBPCAP
        save_file = g_strdup(optarg);
#else
        capture_option_specified = TRUE;
        arg_error = TRUE;
#endif
	break;
      case 'W':        /* Write to capture file FD xxx */
#ifdef HAVE_LIBPCAP
        cf.save_file_fd = atoi(optarg);
#else
        capture_option_specified = TRUE;
        arg_error = TRUE;
#endif
	break;
      default:
      case '?':        /* Bad flag - print usage message */
        arg_error = TRUE;
        break;
    }
  }
#endif

#ifndef HAVE_LIBPCAP
  if (capture_option_specified)
    fprintf(stderr, "This version of Ethereal was not built with support for capturing packets.\n");
#endif
#ifndef WIN32
  if (arg_error)
    print_usage();
#endif
#ifdef HAVE_LIBPCAP
  if (start_capture) {
    /* We're supposed to do a live capture; did the user specify an interface
       to use? */
    if (cf.iface == NULL) {
      /* No - pick the first one from the list of interfaces. */
      if_list = get_interface_list(&err, err_str);
      if (if_list == NULL) {
        switch (err) {

        case CANT_GET_INTERFACE_LIST:
            fprintf(stderr, "ethereal: Can't get list of interfaces: %s\n",
			err_str);
            break;

        case NO_INTERFACES_FOUND:
            fprintf(stderr, "ethereal: There are no interfaces on which a capture can be done\n");
            break;
        }
        exit(2);
      }
      cf.iface = g_strdup(if_list->data);	/* first interface */
      free_interface_list(if_list);
    }
  }
  if (capture_child) {
    if (cf.save_file_fd == -1) {
      /* XXX - send this to the standard output as something our parent
         should put in an error message box? */
      fprintf(stderr, "%s: \"-W\" flag not specified\n", CHILD_NAME);
      exit(1);
    }
  }
#endif

  /* Build the column format array */  
  for (i = 0; i < cf.cinfo.num_cols; i++) {
    cf.cinfo.col_fmt[i] = get_column_format(i);
    cf.cinfo.col_title[i] = g_strdup(get_column_title(i));
    cf.cinfo.fmt_matx[i] = (gboolean *) g_malloc0(sizeof(gboolean) *
      NUM_COL_FMTS);
    get_column_format_matches(cf.cinfo.fmt_matx[i], cf.cinfo.col_fmt[i]);
    if (cf.cinfo.col_fmt[i] == COL_INFO)
      cf.cinfo.col_data[i] = (gchar *) g_malloc(sizeof(gchar) * COL_MAX_INFO_LEN);
    else
      cf.cinfo.col_data[i] = (gchar *) g_malloc(sizeof(gchar) * COL_MAX_LEN);
  }

  if (cf.snap < 1)
    cf.snap = WTAP_MAX_PACKET_SIZE;
  else if (cf.snap < MIN_PACKET_SIZE)
    cf.snap = MIN_PACKET_SIZE;
  
  rc_file = (gchar *) g_malloc(strlen(getenv("HOME")) + strlen(RC_FILE) + 4);
  sprintf(rc_file, "%s/%s", getenv("HOME"), RC_FILE);
  gtk_rc_parse(rc_file);

  if ((m_r_font = gdk_font_load(medium_font)) == NULL) {
    fprintf(stderr, "ethereal: Error font %s not found (use -m option)\n", medium_font);
    exit(1);
  }

  if ((m_b_font = gdk_font_load(bold_font)) == NULL) {
    fprintf(stderr, "ethereal: Error font %s not found (use -b option)\n", bold_font);
    exit(1);
  }

  create_main_window(pl_size, tv_size, bv_size, prefs);

/* 
   Hmmm should we do it here
*/

  ethereal_proto_init();   /* Init anything that needs initializing */

#ifdef HAVE_LIBPCAP
  /* Is this a "child" ethereal, which is only supposed to pop up a
     capture box to let us stop the capture, and run a capture
     to a file that our parent will read? */
  if (!capture_child) {
#endif
    /* No.  Pop up the main window, and read in a capture file if
       we were told to. */

    gtk_widget_show(top_level);

    cf.colors = colfilter_new();

    /* If we were given the name of a capture file, read it in now;
       we defer it until now, so that, if we can't open it, and pop
       up an alert box, the alert box is more likely to come up on
       top of the main window - but before the preference-file-error
       alert box, so, if we get one of those, it's more likely to come
       up on top of us. */
    if (cf_name) {
      if (rfilter != NULL) {
        if (dfilter_compile(rfilter, &rfcode) != 0) {
          simple_dialog(ESD_TYPE_WARN, NULL, dfilter_error_msg);
          rfilter_parse_failed = TRUE;
        }
      }
      if (!rfilter_parse_failed) {
        if ((err = open_cap_file(cf_name, FALSE, &cf)) == 0) {
          /* "open_cap_file()" succeeded, so it closed the previous
	     capture file, and thus destroyed any previous read filter
	     attached to "cf". */
          cf.rfcode = rfcode;
          err = read_cap_file(&cf);
          s = strrchr(cf_name, '/');
          if (s) {
            last_open_dir = cf_name;
            *s = '\0';
          }
        } else {
          dfilter_destroy(rfcode);
          cf.rfcode = NULL;
        }
      }
    }
#ifdef HAVE_LIBPCAP
  }
#endif

  /* If we failed to open the preferences file, pop up an alert box;
     we defer it until now, so that the alert box is more likely to
     come up on top of the main window. */
  if (pf_path != NULL) {
      simple_dialog(ESD_TYPE_WARN, NULL,
        "Could not open preferences file\n\"%s\": %s.", pf_path,
        strerror(pf_open_errno));
  }

#ifdef HAVE_LIBPCAP
  if (capture_child) {
    /* This is the child process for a sync mode or fork mode capture,
       so just do the low-level work of a capture - don't create
       a temporary file and fork off *another* child process (so don't
       call "do_capture()"). */

       capture();

       /* The capture is done; there's nothing more for us to do. */
       gtk_exit(0);
  } else {
    if (start_capture) {
      /* "-k" was specified; start a capture. */
      do_capture(save_file);
    }
  }
#endif

  gtk_main();

  ethereal_proto_cleanup();
  g_free(rc_file);

  exit(0);
}

static void
create_main_window (gint pl_size, gint tv_size, gint bv_size, e_prefs *prefs)
{
  GtkWidget           *main_vbox, *menubar, *u_pane, *l_pane,
                      *bv_table, *stat_hbox,
                      *filter_bt, *filter_cm, *filter_te,
                      *filter_reset;
  GList               *filter_list = NULL;
  GtkStyle            *pl_style;
  GtkAccelGroup       *accel;
  int			i;

  /* Main window */  
  top_level = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_widget_set_name(top_level, "main window");
  gtk_signal_connect(GTK_OBJECT(top_level), "delete_event",
    GTK_SIGNAL_FUNC(file_quit_cmd_cb), "WM destroy");
  gtk_signal_connect(GTK_OBJECT(top_level), "destroy", 
    GTK_SIGNAL_FUNC(file_quit_cmd_cb), "WM destroy");
  gtk_window_set_title(GTK_WINDOW(top_level), "The Ethereal Network Analyzer");
  gtk_widget_set_usize(GTK_WIDGET(top_level), DEF_WIDTH, -1);
  gtk_window_set_policy(GTK_WINDOW(top_level), TRUE, TRUE, FALSE);

  /* Container for menu bar, paned windows and progress/info box */
  main_vbox = gtk_vbox_new(FALSE, 1);
  gtk_container_border_width(GTK_CONTAINER(main_vbox), 1);
  gtk_container_add(GTK_CONTAINER(top_level), main_vbox);
  gtk_widget_show(main_vbox);

  /* Menu bar */
  get_main_menu(&menubar, &accel);
  gtk_window_add_accel_group(GTK_WINDOW(top_level), accel);
  gtk_box_pack_start(GTK_BOX(main_vbox), menubar, FALSE, TRUE, 0);
  gtk_widget_show(menubar);

  /* Panes for the packet list, tree, and byte view */
  u_pane = gtk_vpaned_new();
  gtk_paned_gutter_size(GTK_PANED(u_pane), (GTK_PANED(u_pane))->handle_size);
  l_pane = gtk_vpaned_new();
  gtk_paned_gutter_size(GTK_PANED(l_pane), (GTK_PANED(l_pane))->handle_size);
  gtk_container_add(GTK_CONTAINER(main_vbox), l_pane);
  gtk_widget_show(u_pane);
  gtk_paned_add1 (GTK_PANED(l_pane), u_pane);
  gtk_widget_show(l_pane);

  /* Packet list */
  pkt_scrollw = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW(pkt_scrollw),
    GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_widget_show(pkt_scrollw);
  gtk_paned_add1(GTK_PANED(u_pane), pkt_scrollw);

  packet_list = gtk_clist_new_with_titles(cf.cinfo.num_cols, cf.cinfo.col_title);
  gtk_container_add(GTK_CONTAINER(pkt_scrollw), packet_list);
  gtk_clist_column_titles_passive(GTK_CLIST(packet_list));
  set_plist_sel_browse(prefs->gui_plist_sel_browse);
  pl_style = gtk_style_new();
  gdk_font_unref(pl_style->font);
  pl_style->font = m_r_font;
  gtk_widget_set_style(packet_list, pl_style);
  gtk_widget_set_name(packet_list, "packet list");
  gtk_signal_connect(GTK_OBJECT(packet_list), "select_row",
    GTK_SIGNAL_FUNC(packet_list_select_cb), NULL);
  gtk_signal_connect(GTK_OBJECT(packet_list), "unselect_row",
    GTK_SIGNAL_FUNC(packet_list_unselect_cb), NULL);
  for (i = 0; i < cf.cinfo.num_cols; i++) {
    if (get_column_resize_type(cf.cinfo.col_fmt[i]) != RESIZE_MANUAL)
      gtk_clist_set_column_auto_resize(GTK_CLIST(packet_list), i, TRUE);

    /* Right-justify the packet number column. */
    if (cf.cinfo.col_fmt[i] == COL_NUMBER)
      gtk_clist_set_column_justification(GTK_CLIST(packet_list), i, 
        GTK_JUSTIFY_RIGHT);

    /* Save static column sizes to use during a "-S" capture, so that
       the columns don't resize during a live capture. */
    cf.cinfo.col_width[i] = gdk_string_width(pl_style->font,
			        get_column_longest_string(get_column_format(i)));
  }
  gtk_widget_set_usize(packet_list, -1, pl_size);
  gtk_signal_connect_object(GTK_OBJECT(packet_list), "button_press_event",
    GTK_SIGNAL_FUNC(popup_menu_handler), gtk_object_get_data(GTK_OBJECT(popup_menu_object), PM_PACKET_LIST_KEY));
  gtk_widget_show(packet_list);
  
  /* Tree view */
  tv_scrollw = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW(tv_scrollw),
    GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_paned_add2(GTK_PANED(u_pane), tv_scrollw);
  gtk_widget_set_usize(tv_scrollw, -1, tv_size);
  gtk_widget_show(tv_scrollw);
  
  tree_view = gtk_ctree_new(1, 0);
  /* I need this next line to make the widget work correctly with hidden
   * column titles and GTK_SELECTION_BROWSE */
  gtk_clist_set_column_auto_resize( GTK_CLIST(tree_view), 0, TRUE );
  gtk_container_add( GTK_CONTAINER(tv_scrollw), tree_view );
  set_ptree_sel_browse(prefs->gui_ptree_sel_browse);
  set_ptree_line_style(prefs->gui_ptree_line_style);
  set_ptree_expander_style(prefs->gui_ptree_expander_style);

  gtk_signal_connect(GTK_OBJECT(tree_view), "tree-select-row",
    GTK_SIGNAL_FUNC(tree_view_select_row_cb), NULL);
  gtk_signal_connect(GTK_OBJECT(tree_view), "tree-unselect-row",
    GTK_SIGNAL_FUNC(tree_view_unselect_row_cb), NULL);
  gtk_signal_connect_object(GTK_OBJECT(tree_view), "button_press_event",
    GTK_SIGNAL_FUNC(popup_menu_handler), gtk_object_get_data(GTK_OBJECT(popup_menu_object), PM_TREE_VIEW_KEY));
  gtk_widget_show(tree_view);

  item_style = gtk_style_new();
  gdk_font_unref(item_style->font);
  item_style->font = m_r_font;

  /* Byte view. The table is only one row high, but 3 columns
   * wide. The middle column contains the GtkText with the hex dump.
   * The left and right columns contain vertical scrollbars. They both
   * do the same thing, but only one will be shown at a time, in accordance
   * with where the user wants the other vertical scrollbars places
   * (on the left or the right).
   */
  bv_table = gtk_table_new (1, 3, FALSE);
  gtk_paned_pack2(GTK_PANED(l_pane), bv_table, FALSE, FALSE);
  gtk_widget_set_usize(bv_table, -1, bv_size);
  gtk_widget_show(bv_table);

  byte_view = gtk_text_new(NULL, NULL);
  gtk_text_set_editable(GTK_TEXT(byte_view), FALSE);
  gtk_text_set_word_wrap(GTK_TEXT(byte_view), FALSE);
  gtk_table_attach (GTK_TABLE (bv_table), byte_view, 1, 2, 0, 1,
    GTK_FILL | GTK_EXPAND, GTK_FILL | GTK_EXPAND | GTK_SHRINK, 0, 0);
  gtk_widget_show(byte_view);

  /* The gtk_text widget doesn't scroll horizontally (see gtktext.c)
   * in the GTK+ distribution, so I removed the horizontal scroll bar
   * that used to be here. I tried the gtk_text widget with a 
   * gtk_scrolled_window w/ viewport, but the vertical scrollowing
   * did not work well, and sometimes a few pixels were cut off on
   * the bottom. */

  bv_vscroll_left = gtk_vscrollbar_new(GTK_TEXT(byte_view)->vadj);
  gtk_table_attach(GTK_TABLE(bv_table), bv_vscroll_left, 0, 1, 0, 1,
    GTK_FILL, GTK_EXPAND | GTK_FILL | GTK_SHRINK, 0, 0);

  bv_vscroll_right = gtk_vscrollbar_new(GTK_TEXT(byte_view)->vadj);
  gtk_table_attach(GTK_TABLE(bv_table), bv_vscroll_right, 2, 3, 0, 1,
    GTK_FILL, GTK_EXPAND | GTK_FILL | GTK_SHRINK, 0, 0);
 
  /* Now that the 3 panes are created, set the vertical scrollbar
   * on the left or right according to the user's preference */
  set_scrollbar_placement(prefs->gui_scrollbar_on_right);

  /* Progress/filter/info box */
  stat_hbox = gtk_hbox_new(FALSE, 1);
  gtk_container_border_width(GTK_CONTAINER(stat_hbox), 0);
  gtk_box_pack_start(GTK_BOX(main_vbox), stat_hbox, FALSE, TRUE, 0);
  gtk_widget_show(stat_hbox);

  prog_bar = gtk_progress_bar_new();
  gtk_box_pack_start(GTK_BOX(stat_hbox), prog_bar, FALSE, TRUE, 3);
  gtk_widget_show(prog_bar);

  filter_bt = gtk_button_new_with_label("Filter:");
  gtk_signal_connect(GTK_OBJECT(filter_bt), "clicked",
    GTK_SIGNAL_FUNC(filter_dialog_cb), NULL);
  gtk_box_pack_start(GTK_BOX(stat_hbox), filter_bt, FALSE, TRUE, 0);
  gtk_widget_show(filter_bt);
  
  filter_cm = gtk_combo_new();
  filter_list = g_list_append (filter_list, "");
  gtk_combo_set_popdown_strings(GTK_COMBO(filter_cm), filter_list);
  gtk_combo_disable_activate(GTK_COMBO(filter_cm));
  filter_te = GTK_COMBO(filter_cm)->entry;
  gtk_object_set_data(GTK_OBJECT(filter_bt), E_FILT_TE_PTR_KEY, filter_te);
  gtk_object_set_data(GTK_OBJECT(filter_te), E_DFILTER_CM_KEY, filter_cm);
  gtk_object_set_data(GTK_OBJECT(filter_te), E_DFILTER_FL_KEY, filter_list);
  gtk_box_pack_start(GTK_BOX(stat_hbox), filter_cm, TRUE, TRUE, 3);
  gtk_signal_connect(GTK_OBJECT(filter_te), "activate",
    GTK_SIGNAL_FUNC(filter_activate_cb), (gpointer) NULL);
  gtk_widget_show(filter_cm);

  filter_reset = gtk_button_new_with_label("Reset");
  gtk_object_set_data(GTK_OBJECT(filter_reset), E_DFILTER_TE_KEY, filter_te);
  gtk_signal_connect(GTK_OBJECT(filter_reset), "clicked",
		     GTK_SIGNAL_FUNC(filter_reset_cb), (gpointer) NULL);
  gtk_box_pack_start(GTK_BOX(stat_hbox), filter_reset, FALSE, TRUE, 1);
  gtk_widget_show(filter_reset);

  /* Sets the text entry widget pointer as the E_DILTER_TE_KEY data
   * of any widget that ends up calling a callback which needs
   * that text entry pointer */
  set_menu_object_data("/File/Open...", E_DFILTER_TE_KEY, filter_te);
  set_menu_object_data("/File/Reload", E_DFILTER_TE_KEY, filter_te);
  set_menu_object_data("/Edit/Filters...", E_FILT_TE_PTR_KEY, filter_te);
  set_menu_object_data("/Display/Match Selected", E_DFILTER_TE_KEY, filter_te);
  set_menu_object_data("/Tools/Follow TCP Stream", E_DFILTER_TE_KEY, filter_te);

  info_bar = gtk_statusbar_new();
  main_ctx = gtk_statusbar_get_context_id(GTK_STATUSBAR(info_bar), "main");
  file_ctx = gtk_statusbar_get_context_id(GTK_STATUSBAR(info_bar), "file");
  gtk_statusbar_push(GTK_STATUSBAR(info_bar), main_ctx, DEF_READY_MESSAGE);
  gtk_box_pack_start(GTK_BOX(stat_hbox), info_bar, TRUE, TRUE, 0);
  gtk_widget_show(info_bar);
}
