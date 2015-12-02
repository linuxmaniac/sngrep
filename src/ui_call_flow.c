/**************************************************************************
 **
 ** sngrep - SIP Messages flow viewer
 **
 ** Copyright (C) 2013,2014 Ivan Alonso (Kaian)
 ** Copyright (C) 2013,2014 Irontec SL. All rights reserved.
 **
 ** This program is free software: you can redistribute it and/or modify
 ** it under the terms of the GNU General Public License as published by
 ** the Free Software Foundation, either version 3 of the License, or
 ** (at your option) any later version.
 **
 ** This program is distributed in the hope that it will be useful,
 ** but WITHOUT ANY WARRANTY; without even the implied warranty of
 ** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 ** GNU General Public License for more details.
 **
 ** You should have received a copy of the GNU General Public License
 ** along with this program.  If not, see <http://www.gnu.org/licenses/>.
 **
 ****************************************************************************/
/**
 * @file ui_call_flow.c
 * @author Ivan Alonso [aka Kaian] <kaian@irontec.com>
 *
 * @brief Source of functions defined in ui_call_flow.h
 */

#include "config.h"
#include <stdlib.h>
#include <string.h>
#include "capture.h"
#include "ui_manager.h"
#include "ui_call_flow.h"
#include "ui_call_raw.h"
#include "ui_msg_diff.h"
#include "util.h"
#include "vector.h"
#include "option.h"

/***
 *
 * Some basic ascii art of this panel.
 *
 * +--------------------------------------------------------+
 * |                     Title                              |
 * |   addr1  addr2  addr3  addr4 | Selected Raw Message    |
 * |   -----  -----  -----  ----- | preview                 |
 * | Tmst|      |      |      |   |                         |
 * | Tmst|----->|      |      |   |                         |
 * | Tmst|      |----->|      |   |                         |
 * | Tmst|      |<-----|      |   |                         |
 * | Tmst|      |      |----->|   |                         |
 * | Tmst|<-----|      |      |   |                         |
 * | Tmst|      |----->|      |   |                         |
 * | Tmst|      |<-----|      |   |                         |
 * | Tmst|      |------------>|   |                         |
 * | Tmst|      |<------------|   |                         |
 * |     |      |      |      |   |                         |
 * |     |      |      |      |   |                         |
 * |     |      |      |      |   |                         |
 * | Useful hotkeys                                         |
 * +--------------------------------------------------------+
 *
 */

/**
 * Ui Structure definition for Call Flow panel
 */
ui_t ui_call_flow = {
    .type = PANEL_CALL_FLOW,
    .panel = NULL,
    .create = call_flow_create,
    .destroy = call_flow_destroy,
    .draw = call_flow_draw,
    .handle_key = call_flow_handle_key,
    .help = call_flow_help
};

PANEL *
call_flow_create()
{
    PANEL *panel;
    WINDOW *win;
    int height, width;
    call_flow_info_t *info;

    // Create a new panel to fill all the screen
    panel = new_panel(newwin(LINES, COLS, 0, 0));

    // Initialize Call List specific data
    info = sng_malloc(sizeof(call_flow_info_t));

    // Store it into panel userptr
    set_panel_userptr(panel, (void*) info);

    // Let's draw the fixed elements of the screen
    win = panel_window(panel);
    getmaxyx(win, height, width);

    // Calculate available printable area for messages
    info->flow_win = subwin(win, height - 2 - 2 - 2, width - 2, 4, 0); // Header - Footer - Address
    info->raw_width = 0; // calculated with the available space after drawing columns
    info->last_msg = NULL;

    // Create vectors for columns and flow arrows
    info->columns = vector_create(2, 1);
    vector_set_destroyer(info->columns, vector_generic_destroyer);
    info->arrows = vector_create(20, 5);
    vector_set_destroyer(info->arrows, vector_generic_destroyer);

    return panel;
}

void
call_flow_destroy(PANEL *panel)
{
    call_flow_info_t *info;

    // Free the panel information
    if ((info = call_flow_info(panel))) {
        // Delete panel columns
        vector_destroy(info->columns);
        // Delete panel arrows
        vector_destroy(info->arrows);
        // Delete panel windows
        delwin(info->flow_win);
        delwin(info->raw_win);
        // Delete displayed call group
        call_group_destroy(info->group);
        sng_free(info);
    }
    // Delete panel window
    delwin(panel_window(panel));
    // Deallocate panel pointer
    del_panel(panel);
}

call_flow_info_t *
call_flow_info(PANEL *panel)
{
    return (call_flow_info_t*) panel_userptr(panel);
}

int
call_flow_draw(PANEL *panel)
{
    call_flow_info_t *info;
    WINDOW *win;
    call_flow_arrow_t *arrow = NULL;
    int height, width, cline = 0;
    char title[256];
    char callid[256];

    // Get panel information
    info = call_flow_info(panel);

    // Get window of main panel
    win = panel_window(panel);
    getmaxyx(win, height, width);
    werase(win);

    // Set title
    if (call_group_count(info->group) == 1) {
        sprintf(title, "Call flow for %s",
                call_get_attribute(vector_first(info->group->calls), SIP_ATTR_CALLID, callid));
    } else {
        sprintf(title, "Call flow for %d dialogs", call_group_count(info->group));
    }

    // Print color mode in title
    if (setting_has_value(SETTING_COLORMODE, "request"))
        strcat(title, " (Color by Request/Response)");
    if (setting_has_value(SETTING_COLORMODE, "callid"))
        strcat(title, " (Color by Call-Id)");
    if (setting_has_value(SETTING_COLORMODE, "cseq"))
        strcat(title, " (Color by CSeq)");

    // Draw panel title
    draw_title(panel, title);

    // Show some keybinding
    call_flow_draw_footer(panel);

    // Redraw columns
    call_flow_draw_columns(panel);

    for (arrow = info->first_arrow; arrow; arrow = call_flow_next_arrow(panel, arrow)) {
        if (arrow->type == CF_ARROW_SIP) {
            if (!call_flow_draw_message(panel, arrow, cline))
                break;
        } else if (arrow->type == CF_ARROW_RTP) {
            if (!call_flow_draw_rtp_stream(panel, arrow, cline))
                break;
        } else if (arrow->type == CF_ARROW_RTCP) {
            if (!call_flow_draw_rtcp_stream(panel, arrow, cline))
                break;
        }
        cline += arrow->height;
    }

    // If there are only three columns, then draw the raw message on this panel
    if (setting_enabled(SETTING_CF_FORCERAW)) {
        switch (info->cur_arrow->type) {
            case CF_ARROW_RTP:
                call_flow_draw_raw(panel, info->cur_arrow->stream->media->msg);
                break;
            case CF_ARROW_SIP:
                call_flow_draw_raw(panel, info->cur_arrow->msg);
                break;
            case CF_ARROW_RTCP:
                call_flow_draw_raw_rtcp(panel, info->cur_arrow->stream);
                break;
        }

    }

    // Draw the scrollbar
    draw_vscrollbar(info->flow_win,
                    call_group_msg_number(info->group, call_flow_arrow_message(info->first_arrow)) * 2,
                    call_group_msg_count(info->group) * 2, 1);

    // Redraw flow win
    wnoutrefresh(info->flow_win);

    return 0;

}

void
call_flow_draw_footer(PANEL *panel)
{
    call_flow_info_t *info;
    sip_call_t *call = NULL;
    WINDOW *win;
    int streamcnt = 0;
    int height, width;

    // Get panel information
    info = call_flow_info(panel);

    // Get window of main panel
    win = panel_window(panel);
    getmaxyx(win, height, width);

    const char *keybindings[] = {
        key_action_key_str(ACTION_PREV_SCREEN), "Calls List",
        key_action_key_str(ACTION_CONFIRM), "Raw",
        key_action_key_str(ACTION_SELECT), "Compare",
        key_action_key_str(ACTION_SHOW_HELP), "Help",
        key_action_key_str(ACTION_SDP_INFO), "SDP",
        key_action_key_str(ACTION_TOGGLE_MEDIA), "RTP",
        key_action_key_str(ACTION_SHOW_FLOW_EX), "Extended",
        key_action_key_str(ACTION_COMPRESS), "Compressed",
        key_action_key_str(ACTION_SHOW_RAW), "Raw",
        key_action_key_str(ACTION_CYCLE_COLOR), "Colour by",
        key_action_key_str(ACTION_INCREASE_RAW), "Increase Raw"
    };

    draw_keybindings(panel, keybindings, 22);

    // If any dialog has RTP streams and they are not visible
    if (!setting_enabled(SETTING_CF_MEDIA)) {
        while ((call = call_group_get_next(info->group, call)) ) {
            streamcnt += vector_count(call->streams);
        }
        // Highlight RTP keybinding
        if (streamcnt) {
            wattron(win, A_BOLD | COLOR_PAIR(CP_YELLOW_ON_CYAN));
            mvwprintw(win, height - 1, 64, "%s %s", key_action_key_str(ACTION_TOGGLE_MEDIA), "RTP");
            wattroff(win, A_BOLD | COLOR_PAIR(CP_YELLOW_ON_CYAN));
        }
    }
}

int
call_flow_draw_columns(PANEL *panel)
{
    call_flow_info_t *info;
    call_flow_column_t *column;
    sip_call_t *call = NULL;
    rtp_stream_t *stream;
    WINDOW *win;
    sip_msg_t *msg;
    vector_iter_t streams;
    vector_iter_t columns;
    int flow_height, flow_width;
    const char *coltext;
    char ip_src[80], ip_dst[80];

    // Get panel information
    info = call_flow_info(panel);

    // Get window of main panel
    win = panel_window(panel);
    getmaxyx(info->flow_win, flow_height, flow_width);

    // Load columns
    for (msg = call_group_get_next_msg(info->group, info->last_msg); msg;
         msg = call_group_get_next_msg(info->group, msg)) {
        memset(ip_src, 0, sizeof(ip_src));
        memset(ip_dst, 0, sizeof(ip_dst));
        msg_get_attribute(msg, SIP_ATTR_SRC, ip_src);
        msg_get_attribute(msg, SIP_ATTR_DST, ip_dst);
        call_flow_column_add(panel, msg->call->callid, ip_src);
        call_flow_column_add(panel, msg->call->callid, ip_dst);
        info->last_msg = msg;
    }

    // Add RTP columns FIXME Really
    if (!setting_disabled(SETTING_CF_MEDIA)) {
        while ((call = call_group_get_next(info->group, call)) ) {
            streams = vector_iterator(call->streams);
            while ((stream = vector_iterator_next(&streams))) {
                if (stream_get_count(stream)) {
                    call_flow_column_add(panel, NULL, stream->ip_src);
                    call_flow_column_add(panel, NULL, stream->ip_dst);
                }
            }
        }
    }

    // Draw vertical columns lines
    columns = vector_iterator(info->columns);
    while ((column = vector_iterator_next(&columns))) {
        mvwvline(info->flow_win, 0, 20 + 30 * column->colpos, ACS_VLINE, flow_height);
        mvwhline(win, 3, 10 + 30 * column->colpos, ACS_HLINE, 20);
        mvwaddch(win, 3, 20 + 30 * column->colpos, ACS_TTEE);

        // Set bold to this address if it's local
        if (is_local_address_str(column->addr) && setting_enabled(SETTING_CF_LOCALHIGHLIGHT))
            wattron(win, A_BOLD);

        coltext = sip_address_port_format(column->addr);
        mvwprintw(win, 2, 10 + 30 * column->colpos + (22 - strlen(coltext)) / 2, "%s", coltext);
        wattroff(win, A_BOLD);
    }

    return 0;
}

call_flow_arrow_t *
call_flow_draw_message(PANEL *panel, call_flow_arrow_t *arrow, int cline)
{
    call_flow_info_t *info;
    WINDOW *win;
    sdp_media_t *media;
    const char *msg_callid;
    char msg_method[128];
    char msg_time[80];
    char msg_src[80];
    char msg_dst[80];
    char sdp_address[80];
    char sdp_port[80];
    char method[80];
    char delta[15] = { };
    int height, width;
    char mediastr[40];
    sip_msg_t *msg = arrow->msg;
    vector_iter_t medias;
    int color = 0;
    int msglen;
    int arrow_dir = 0; /* 0: right, 1: left */

    // Get panel information
    info = call_flow_info(panel);
    // Get the messages window
    win = info->flow_win;
    getmaxyx(win, height, width);

    // Store arrow start line
    arrow->line = cline;

    // Calculate how many lines this message requires
    arrow->height = call_flow_arrow_height(panel, arrow);

    // Check this message fits on the panel
    if (cline > height + arrow->height)
        return NULL;

    // Get message attributes
    msg_callid = msg->call->callid;
    msg_get_attribute(msg, SIP_ATTR_METHOD, msg_method);
    msg_get_attribute(msg, SIP_ATTR_TIME, msg_time);
    msg_get_attribute(msg, SIP_ATTR_SRC, msg_src);
    msg_get_attribute(msg, SIP_ATTR_DST, msg_dst);

    // Get Message method (include extra info)
    sprintf(method, "%s", msg_method);

    // If message has sdp information
    if (msg_has_sdp(msg) && setting_has_value(SETTING_CF_SDP_INFO, "off")) {
        // Show sdp tag in title
        sprintf(method, "%s (SDP)", msg_method);
    }

    // If message has sdp information
    if (setting_has_value(SETTING_CF_SDP_INFO, "compressed")) {
        // Show sdp tag in title
        if (msg_has_sdp(msg)) {
            sprintf(method, "%.*s (SDP)", 12, msg_method);
        } else {
            sprintf(method, "%.*s", 17, msg_method);
        }
    }

    if (msg_has_sdp(msg) && setting_has_value(SETTING_CF_SDP_INFO, "first")) {
        sprintf(method, "%.3s (%s:%s)",
                msg_method,
                msg_get_attribute(msg, SIP_ATTR_SDP_ADDRESS, sdp_address),
                msg_get_attribute(msg, SIP_ATTR_SDP_PORT, sdp_port));
    }
    if (msg_has_sdp(msg) && setting_has_value(SETTING_CF_SDP_INFO, "full")) {
        sprintf(method, "%.3s (%s)",
                msg_method,
                msg_get_attribute(msg, SIP_ATTR_SDP_ADDRESS, sdp_address));
    }

    // Draw message type or status and line
    msglen = (strlen(method) > 24) ? 24 : strlen(method);

    // Get origin and destination column
    call_flow_column_t *column1 = call_flow_column_get(panel, msg_callid, msg_src);
    call_flow_column_t *column2 = call_flow_column_get(panel, msg_callid, msg_dst);

    call_flow_column_t *tmp;
    if (column1->colpos > column2->colpos) {
        tmp = column1;
        column1 = column2;
        column2 = tmp;
        arrow_dir = 1; /* swap arrow direction */
    }

    int startpos = 20 + 30 * column1->colpos;
    int endpos = 20 + 30 * column2->colpos;
    int distance = abs(endpos - startpos) - 3;

    // Highlight current message
    if (arrow == info->cur_arrow) {
        if (setting_has_value(SETTING_CF_HIGHTLIGHT, "reverse")) {
            wattron(win, A_REVERSE);
        }
        if (setting_has_value(SETTING_CF_HIGHTLIGHT, "bold")) {
            wattron(win, A_BOLD);
        }
        if (setting_has_value(SETTING_CF_HIGHTLIGHT, "reversebold")) {
            wattron(win, A_REVERSE);
            wattron(win, A_BOLD);
        }
    }

    // Color the message {
    if (setting_has_value(SETTING_COLORMODE, "request")) {
        // Color by request / response
        color = (msg_is_request(msg)) ? CP_RED_ON_DEF : CP_GREEN_ON_DEF;
    } else if (setting_has_value(SETTING_COLORMODE, "callid")) {
        // Color by call-id
        color = call_group_color(info->group, msg->call);
    } else if (setting_has_value(SETTING_COLORMODE, "cseq")) {
        // Color by CSeq within the same call
        color = msg->cseq % 7 + 1;
    }

    // Turn on the message color
    wattron(win, COLOR_PAIR(color));

    // Clear the line
    mvwprintw(win, cline, startpos + 2, "%*s", distance, "");
    // Draw method
    mvwprintw(win, cline, startpos + distance / 2 - msglen / 2 + 2, "%.26s", method);

    if (!setting_has_value(SETTING_CF_SDP_INFO, "compressed"))
        cline++;

    // Draw media information
    if (msg_has_sdp(msg) && setting_has_value(SETTING_CF_SDP_INFO, "full")) {
        medias = vector_iterator(msg->medias);
        while ((media = vector_iterator_next(&medias))) {
            sprintf(mediastr, "%s %d (%s)",
                    media_get_type(media),
                    media_get_port(media),
                    media_get_prefered_format(media));
            mvwprintw(win, cline++, startpos + distance / 2 - strlen(mediastr) / 2 + 2, mediastr);
        }
    }

    if (arrow == info->selected) {
        mvwhline(win, cline, startpos + 2, '=', distance);
    } else {
        mvwhline(win, cline, startpos + 2, ACS_HLINE, distance);
    }

    // Write the arrow at the end of the message (two arros if this is a retrans)
    if (arrow_dir == 0 /* right */) {
        mvwaddch(win, cline, endpos - 2, '>');
        if (call_msg_is_retrans(msg)) {
            mvwaddch(win, cline, endpos - 3, '>');
            mvwaddch(win, cline, endpos - 4, '>');
        }
    } else {
        mvwaddch(win, cline, startpos + 2, '<');
        if (call_msg_is_retrans(msg)) {
            mvwaddch(win, cline, startpos + 3, '<');
            mvwaddch(win, cline, startpos + 4, '<');
        }
    }

    if (setting_has_value(SETTING_CF_SDP_INFO, "compressed"))
        mvwprintw(win, cline, startpos + distance / 2 - msglen / 2 + 2, " %.26s ", method);

    // Turn off colors
    wattroff(win, COLOR_PAIR(CP_RED_ON_DEF));
    wattroff(win, COLOR_PAIR(CP_GREEN_ON_DEF));
    wattroff(win, COLOR_PAIR(CP_CYAN_ON_DEF));
    wattroff(win, COLOR_PAIR(CP_YELLOW_ON_DEF));
    wattroff(win, A_BOLD | A_REVERSE);

    // Print timestamp
    if (info->selected == arrow)
        wattron(win, COLOR_PAIR(CP_CYAN_ON_DEF));
    mvwprintw(win, cline, 2, "%s", msg_time);

    // Print delta from selected message
    if (!setting_has_value(SETTING_CF_SDP_INFO, "compressed")) {
        if (!info->selected) {
            if (setting_enabled(SETTING_CF_DELTA))
                timeval_to_delta(msg_get_time(call_group_get_prev_msg(info->group, msg)), msg_get_time(msg), delta);
        } else if (info->cur_arrow == arrow) {
            timeval_to_delta(msg_get_time(call_flow_arrow_message(info->selected)), msg_get_time(msg), delta);
        }

        if (strlen(delta)) {
            wattron(win, COLOR_PAIR(CP_CYAN_ON_DEF));
            mvwprintw(win, cline - 1 , 2, "%15s", delta);
        }
        wattroff(win, COLOR_PAIR(CP_CYAN_ON_DEF));
    }

    wattroff(win, COLOR_PAIR(CP_CYAN_ON_DEF));

    return arrow;
}


call_flow_arrow_t *
call_flow_draw_rtp_stream(PANEL *panel, call_flow_arrow_t *arrow, int cline)
{
    call_flow_info_t *info;
    WINDOW *win;
    char text[50], time[20];
    int height, width;
    const char *callid;
    char msg_dst[80], msg_src[80];
    call_flow_column_t *column1, *column2;
    rtp_stream_t *stream = arrow->stream;
    int arrow_dir = 0; /* 0: right, 1: left */

    // Get panel information
    info = call_flow_info(panel);
    // Get the messages window
    win = info->flow_win;
    getmaxyx(win, height, width);

    // Store arrow start line
    arrow->line = cline;

    // Calculate how many lines this message requires
    arrow->height = call_flow_arrow_height(panel, arrow);

    // Check this media fits on the panel
    if (cline > height + arrow->height)
        return NULL;

    // Get Message method (include extra info)
    sprintf(text, "RTP (%s) %d", stream_get_format(stream), stream_get_count(stream));

    // Get message data
    msg_get_attribute(stream->media->msg, SIP_ATTR_SRC, msg_src);
    msg_get_attribute(stream->media->msg, SIP_ATTR_DST, msg_dst);
    // Remove port from address. We only look for columns no matter if it matches port or not
    sip_address_strip_port(msg_src);
    sip_address_strip_port(msg_dst);

    callid = stream->media->msg->call->callid;

    // Get origin column for this stream.
    // If we share the same Address from its setup SIP packet, use that column instead.
    if (!strcmp(stream->ip_src, msg_src)) {
        column1 = call_flow_column_get(panel, callid, msg_src);
    } else if (!strcmp(stream->ip_src, msg_dst)) {
        column1 = call_flow_column_get(panel, callid, msg_dst);
    } else {
        column1 = call_flow_column_get(panel, 0, stream->ip_src);
    }

    // Get destination column for this stream.
    // If we share the same Address from its setup SIP packet, use that column instead.
    if (!strcmp(stream->ip_dst, msg_dst)) {
        column2 = call_flow_column_get(panel, callid, msg_dst);
    } else if (!strcmp(stream->ip_dst, msg_src)) {
        column2 = call_flow_column_get(panel, callid, msg_src);
    } else {
        column2 = call_flow_column_get(panel, 0, stream->ip_dst);
    }

    call_flow_column_t *tmp;
    if (column1->colpos > column2->colpos) {
        tmp = column1;
        column1 = column2;
        column2 = tmp;
        arrow_dir = 1; /* swap arrow direction */
    }

    int startpos = 20 + 30 * column1->colpos;
    int endpos = 20 + 30 * column2->colpos;

    // In compressed mode, we display the src and dst port inside the arrow
    // so fixup the stard and end position
    if (!setting_has_value(SETTING_CF_SDP_INFO, "compressed")) {
        startpos += 5;
        endpos -= 5;
    }

    int distance = abs(endpos - startpos) - 4 + 1;

    // Highlight current message
    if (arrow == info->cur_arrow) {
        if (setting_has_value(SETTING_CF_HIGHTLIGHT, "reverse")) {
            wattron(win, A_REVERSE);
        }
        if (setting_has_value(SETTING_CF_HIGHTLIGHT, "bold")) {
            wattron(win, A_BOLD);
        }
        if (setting_has_value(SETTING_CF_HIGHTLIGHT, "reversebold")) {
            wattron(win, A_REVERSE);
            wattron(win, A_BOLD);
        }
    }

    // Clear the line
    mvwprintw(win, cline, startpos + 2, "%*s", distance, "");
    // Draw method
    mvwprintw(win, cline, startpos + (distance) / 2 - strlen(text) / 2 + 2, "%s", text);

    if (!setting_has_value(SETTING_CF_SDP_INFO, "compressed"))
        cline++;

    // Check if this rtpstream seems to be alive
    if (arrow->rtp_count == stream_get_count(stream)) {
        arrow->rtp_alive--;
    } else {
        arrow->rtp_alive = 5;
    }

    // Update internal packet counter for this stream
    arrow->rtp_count = stream_get_count(stream);

    // If stream is alive, change color
    if (arrow->rtp_alive > 0)
        wattron(win, COLOR_PAIR(CP_BLUE_ON_DEF));

    // Draw line between columns
    mvwhline(win, cline, startpos + 2, ACS_HLINE, distance);
    // Write the arrow at the end of the message (two arrows if this is a retrans)
    if (arrow_dir == 0 /* right */) {
        if (!setting_has_value(SETTING_CF_SDP_INFO, "compressed")) {
            mvwprintw(win, cline, startpos - 4, "%d", stream->sport);
            mvwprintw(win, cline, endpos, "%d", stream->dport);
        }
        mvwaddch(win, cline, endpos - 2, '>');
        if (arrow->rtp_alive > 0) {
            arrow->rtp_ind_pos++;
            mvwaddch(win, cline, startpos + (arrow->rtp_ind_pos % distance), '>');
        }
    } else {
        if (!setting_has_value(SETTING_CF_SDP_INFO, "compressed")) {
            mvwprintw(win, cline, endpos, "%d", stream->sport);
            mvwprintw(win, cline, startpos - 4, "%d", stream->dport);
        }
        mvwaddch(win, cline, startpos + 2, '<');
        if (arrow->rtp_alive > 0) {
            arrow->rtp_ind_pos++;
            mvwaddch(win, cline, endpos - (arrow->rtp_ind_pos % distance) - 2, '<');
        }
    }

    if (setting_has_value(SETTING_CF_SDP_INFO, "compressed"))
        mvwprintw(win, cline, startpos + (distance) / 2 - strlen(text) / 2 + 2, " %s ", text);

    wattroff(win, A_BOLD | A_REVERSE | COLOR_PAIR(CP_BLUE_ON_DEF));

    // Print timestamp
    timeval_to_time(stream->time, time);
    mvwprintw(win, cline, 2, "%s", time);


    return arrow;
}

call_flow_arrow_t *
call_flow_draw_rtcp_stream(PANEL *panel, call_flow_arrow_t *arrow, int cline)
{
    call_flow_info_t *info;
    WINDOW *win;
    char text[50], time[20];
    int height, width;
    const char *callid;
    char msg_dst[80], msg_src[80];
    call_flow_column_t *column1, *column2;
    rtp_stream_t *stream = arrow->stream;
    int arrow_dir = 0; /* 0: right, 1: left */

    // Get panel information
    info = call_flow_info(panel);
    // Get the messages window
    win = info->flow_win;
    getmaxyx(win, height, width);

    // Store arrow start line
    arrow->line = cline;

    // Calculate how many lines this message requires
    arrow->height = call_flow_arrow_height(panel, arrow);

    // Check this media fits on the panel
    if (cline > height + arrow->height)
        return NULL;

    // Arrow text
    sprintf(text, "RTCP (%.1f) %d", (float) stream->rtcpinfo.mosc / 10, stream_get_count(stream));

    // Get message data
    msg_get_attribute(stream->media->msg, SIP_ATTR_SRC, msg_src);
    msg_get_attribute(stream->media->msg, SIP_ATTR_DST, msg_dst);
    // Remove port from address. We only look for columns no matter if it matches port or not
    sip_address_strip_port(msg_src);
    sip_address_strip_port(msg_dst);

    callid = stream->media->msg->call->callid;

    // Get origin column for this stream.
    // If we share the same Address from its setup SIP packet, use that column instead.
    if (!strcmp(stream->ip_src, msg_src)) {
        column1 = call_flow_column_get(panel, callid, msg_src);
    } else if (!strcmp(stream->ip_src, msg_dst)) {
        column1 = call_flow_column_get(panel, callid, msg_dst);
    } else {
        column1 = call_flow_column_get(panel, 0, stream->ip_src);
    }

    // Get destination column for this stream.
    // If we share the same Address from its setup SIP packet, use that column instead.
    if (!strcmp(stream->ip_dst, msg_dst)) {
        column2 = call_flow_column_get(panel, callid, msg_dst);
    } else if (!strcmp(stream->ip_dst, msg_src)) {
        column2 = call_flow_column_get(panel, callid, msg_src);
    } else {
        column2 = call_flow_column_get(panel, 0, stream->ip_dst);
    }

    call_flow_column_t *tmp;
    if (column1->colpos > column2->colpos) {
        tmp = column1;
        column1 = column2;
        column2 = tmp;
        arrow_dir = 1; /* swap arrow direction */
    }

    int startpos = 20 + 30 * column1->colpos;
    int endpos = 20 + 30 * column2->colpos;

    // In compressed mode, we display the src and dst port inside the arrow
    // so fixup the stard and end position
    if (!setting_has_value(SETTING_CF_SDP_INFO, "compressed")) {
        startpos += 5;
        endpos -= 5;
    }

    int distance = abs(endpos - startpos) - 4 + 1;

    // Highlight current message
    if (arrow == info->cur_arrow) {
        if (setting_has_value(SETTING_CF_HIGHTLIGHT, "reverse")) {
            wattron(win, A_REVERSE);
        }
        if (setting_has_value(SETTING_CF_HIGHTLIGHT, "bold")) {
            wattron(win, A_BOLD);
        }
        if (setting_has_value(SETTING_CF_HIGHTLIGHT, "reversebold")) {
            wattron(win, A_REVERSE);
            wattron(win, A_BOLD);
        }
    }

    // Clear the line
    mvwprintw(win, cline, startpos + 2, "%*s", distance, "");
    // Draw method
    mvwprintw(win, cline, startpos + (distance) / 2 - strlen(text) / 2 + 2, "%s", text);
    if (!setting_has_value(SETTING_CF_SDP_INFO, "compressed"))
        cline++;

    // Draw line between columns
    mvwhline(win, cline, startpos + 2, '-', distance);
    // Write the arrow at the end of the message (two arrows if this is a retrans)
    if (arrow_dir == 0 /* right */) {
        if (!setting_has_value(SETTING_CF_SDP_INFO, "compressed")) {
            mvwprintw(win, cline, startpos - 4, "%d", stream->sport);
            mvwprintw(win, cline, endpos, "%d", stream->dport);
        }
        mvwaddch(win, cline, endpos - 2, '>');
        if (arrow->rtp_count != stream_get_count(stream)) {
            arrow->rtp_count = stream_get_count(stream);
            arrow->rtp_ind_pos = (arrow->rtp_ind_pos + 1) % distance;
            mvwaddch(win, cline, startpos + arrow->rtp_ind_pos + 2, '>');
        }
    } else {
        if (!setting_has_value(SETTING_CF_SDP_INFO, "compressed")) {
            mvwprintw(win, cline, endpos, "%d", stream->sport);
            mvwprintw(win, cline, startpos - 4, "%d", stream->dport);
        }
        mvwaddch(win, cline, startpos + 2, '<');
        if (arrow->rtp_count != stream_get_count(stream)) {
            arrow->rtp_count = stream_get_count(stream);
            arrow->rtp_ind_pos = (arrow->rtp_ind_pos + 1) % distance;
            mvwaddch(win, cline, endpos - arrow->rtp_ind_pos - 2, '<');
        }
    }

    if (setting_has_value(SETTING_CF_SDP_INFO, "compressed"))
        mvwprintw(win, cline, startpos + (distance) / 2 - strlen(text) / 2 + 2, " %s ", text);

    wattroff(win, A_BOLD | A_REVERSE);

    // Print timestamp
    timeval_to_time(stream->time, time);
    mvwprintw(win, cline, 2, "%s", time);

    return arrow;
}

call_flow_arrow_t *
call_flow_next_arrow(PANEL *panel, const call_flow_arrow_t *cur)
{
    sip_msg_t *msg = NULL;
    rtp_stream_t *stream = NULL;
    struct timeval cur_time;
    call_flow_info_t *info;
    call_flow_arrow_t *next;

    // Get panel information
    info = call_flow_info(panel);

    // Return next arrow if already parsed
    if (cur && (next = vector_item(info->arrows, cur->index + 1)))
        return next;

    // Get current arrow timestamp
    if (!cur) {
        memset(&cur_time, 0, sizeof(struct timeval));
    } else if (cur->type == CF_ARROW_SIP) {
        cur_time = msg_get_time(cur->msg);
    } else if (cur->type == CF_ARROW_RTP || cur->type == CF_ARROW_RTCP) {
        cur_time = cur->stream->time;
    }

    // Look for the next message
    while ((msg = call_group_get_next_msg(info->group, msg))) {
        if (timeval_is_older(msg_get_time(msg), cur_time)) {
            break;
        }
    }

    if (!setting_disabled(SETTING_CF_MEDIA)) {
        // Look for the next stream
        while ((stream = call_group_get_next_stream(info->group, stream))) {
            // Only handle RTCP when required
            if (!setting_has_value(SETTING_CF_MEDIA, "rtcp") && stream->type == CAPTURE_PACKET_RTCP)
                continue;

            if (timeval_is_older(stream->time, cur_time)) {
                break;
            }
        }
    }

    if (!msg && !stream)
        return NULL;  /* Nothing goes next */

    /* a rtp stream goes next */
    if (!msg) {
        // Create a new arrow to store next info
        next = sng_malloc(sizeof(call_flow_arrow_t));
        next->type = (stream->type == CAPTURE_PACKET_RTP) ? CF_ARROW_RTP : CF_ARROW_RTCP;
        next->stream = stream;
        next->rtp_alive = 5;
    } else if (!stream) {
        /* a sip message goes next */
        // Create a new arrow to store next info
        next = sng_malloc(sizeof(call_flow_arrow_t));
        next->type = CF_ARROW_SIP;
        next->msg = msg;
    } else {
        /* Determine what goes next */
        if (timeval_is_older(msg_get_time(msg), stream->time)) {
            // Create a new arrow to store next info
            next = sng_malloc(sizeof(call_flow_arrow_t));
            next->type = (stream->type == CAPTURE_PACKET_RTP) ? CF_ARROW_RTP : CF_ARROW_RTCP;
            next->stream = stream;
            next->rtp_alive = 5;
        } else {
            // Create a new arrow to store next info
            next = sng_malloc(sizeof(call_flow_arrow_t));
            next->type = CF_ARROW_SIP;
            next->msg = msg;
        }
    }

    // Add this arrow to the list and return it
    next->index = vector_append(info->arrows, next);

    return next;
}

call_flow_arrow_t *
call_flow_prev_arrow(PANEL *panel, const call_flow_arrow_t *cur)
{
    call_flow_arrow_t *prev;
    call_flow_info_t *info;

    if (!cur)
        return NULL;

    // Get panel information
    if (!(info = call_flow_info(panel)))
        return NULL;

    if ((prev = vector_item(info->arrows, cur->index - 1)))
        return prev;

    return NULL;
}

int
call_flow_arrow_height(PANEL *panel, const call_flow_arrow_t *arrow)
{
    if (arrow->type == CF_ARROW_SIP) {
        if (setting_has_value(SETTING_CF_SDP_INFO, "compressed"))
            return 1;
        if (!msg_has_sdp(arrow->msg))
            return 2;
        if (setting_has_value(SETTING_CF_SDP_INFO, "off"))
            return 2;
        if (setting_has_value(SETTING_CF_SDP_INFO, "first"))
            return 2;
        if (setting_has_value(SETTING_CF_SDP_INFO, "full"))
            return msg_media_count(arrow->msg) + 2;
    } else if (arrow->type == CF_ARROW_RTP || arrow->type == CF_ARROW_RTCP) {
        if (setting_has_value(SETTING_CF_SDP_INFO, "compressed"))
            return 1;
        return 2;
    }

    return 0;
}

call_flow_arrow_t *
call_flow_arrow_find(PANEL *panel, const void *data)
{
    call_flow_info_t *info;
    call_flow_arrow_t *arrow;
    vector_iter_t arrows;

    if (!data)
        return NULL;

    if (!(info = call_flow_info(panel)))
        return NULL;

    arrows = vector_iterator(info->arrows);
    while ((arrow = vector_iterator_next(&arrows)))
        if (arrow->msg == data || arrow->stream == data)
            return arrow;

    return arrow;
}

sip_msg_t *
call_flow_arrow_message(const  call_flow_arrow_t *arrow)
{
    if (!arrow)
        return NULL;
    if (arrow->type == CF_ARROW_SIP)
        return arrow->msg;
    if (arrow->type == CF_ARROW_RTP || arrow->type == CF_ARROW_RTCP)
        return arrow->stream->media->msg;
    return NULL;
}

int
call_flow_draw_raw(PANEL *panel, sip_msg_t *msg)
{
    call_flow_info_t *info;
    WINDOW *win, *raw_win;
    int raw_width, raw_height, height, width;
    int min_raw_width, fixed_raw_width;

    // Get panel information
    if (!(info = call_flow_info(panel)))
        return 1;

    // Get window of main panel
    win = panel_window(panel);
    getmaxyx(win, height, width);

    // Get min raw width
    min_raw_width = setting_get_intvalue(SETTING_CF_RAWMINWIDTH);
    fixed_raw_width = setting_get_intvalue(SETTING_CF_RAWFIXEDWIDTH);

    // Calculate the raw data width (width - used columns for flow - vertical lines)
    raw_width = width - (30 * vector_count(info->columns)) - 2;
    // We can define a mininum size for rawminwidth
    if (raw_width < min_raw_width) {
        raw_width = min_raw_width;
    }
    // We can configure an exact raw size
    if (fixed_raw_width > 0) {
        raw_width = fixed_raw_width;
    }

    // Height of raw window is always available size minus 6 lines for header/footer
    raw_height = height - 3;

    // If we already have a raw window
    raw_win = info->raw_win;
    if (raw_win) {
        // Check it has the correct size
        if (getmaxx(raw_win) != raw_width) {
            // We need a new raw window
            delwin(raw_win);
            info->raw_win = raw_win = newwin(raw_height, raw_width, 0, 0);
        } else {
            // We have a valid raw win, clear its content
            werase(raw_win);
        }
    } else {
        // Create the raw window of required size
        info->raw_win = raw_win = newwin(raw_height, raw_width, 0, 0);
    }

    // Draw raw box lines
    wattron(win, COLOR_PAIR(CP_BLUE_ON_DEF));
    mvwvline(win, 1, width - raw_width - 2, ACS_VLINE, height - 2);
    wattroff(win, COLOR_PAIR(CP_BLUE_ON_DEF));

    // Print msg payload
    draw_message(info->raw_win, msg);

    // Copy the raw_win contents into the panel
    copywin(raw_win, win, 0, 0, 1, width - raw_width - 1, raw_height, width - 2, 0);

    return 0;
}


int
call_flow_draw_raw_rtcp(PANEL *panel, rtp_stream_t *stream)
{
    call_flow_info_t *info;
    WINDOW *win, *raw_win;
    int raw_width, raw_height, height, width;
    int min_raw_width, fixed_raw_width;

    // Get panel information
    if (!(info = call_flow_info(panel)))
        return 1;

    // Get window of main panel
    win = panel_window(panel);
    getmaxyx(win, height, width);

    // Get min raw width
    min_raw_width = setting_get_intvalue(SETTING_CF_RAWMINWIDTH);
    fixed_raw_width = setting_get_intvalue(SETTING_CF_RAWFIXEDWIDTH);

    // Calculate the raw data width (width - used columns for flow - vertical lines)
    raw_width = width - (30 * vector_count(info->columns)) - 2;
    // We can define a mininum size for rawminwidth
    if (raw_width < min_raw_width) {
        raw_width = min_raw_width;
    }
    // We can configure an exact raw size
    if (fixed_raw_width > 0) {
        raw_width = fixed_raw_width;
    }

    // Height of raw window is always available size minus 6 lines for header/footer
    raw_height = height - 3;

    // If we already have a raw window
    raw_win = info->raw_win;
    if (raw_win) {
        // Check it has the correct size
        if (getmaxx(raw_win) != raw_width) {
            // We need a new raw window
            delwin(raw_win);
            info->raw_win = raw_win = newwin(raw_height, raw_width, 0, 0);
        } else {
            // We have a valid raw win, clear its content
            werase(raw_win);
        }
    } else {
        // Create the raw window of required size
        info->raw_win = raw_win = newwin(raw_height, raw_width, 0, 0);
    }

    // Draw raw box lines
    wattron(win, COLOR_PAIR(CP_BLUE_ON_DEF));
    mvwvline(win, 1, width - raw_width - 2, ACS_VLINE, height - 2);
    wattroff(win, COLOR_PAIR(CP_BLUE_ON_DEF));

    mvwprintw(raw_win, 0, 0, "============ RTCP Information ============");
    mvwprintw(raw_win, 2, 0, "Sender's packet count: %d", stream->rtcpinfo.spc);
    mvwprintw(raw_win, 3, 0, "Fraction Lost: %d / 256", stream->rtcpinfo.flost);
    mvwprintw(raw_win, 4, 0, "Fraction discarded: %d / 256", stream->rtcpinfo.fdiscard);
    mvwprintw(raw_win, 6, 0, "MOS - Listening Quality: %.1f", (float) stream->rtcpinfo.mosl / 10);
    mvwprintw(raw_win, 7, 0, "MOS - Conversational Quality: %.1f", (float) stream->rtcpinfo.mosc / 10);



    // Copy the raw_win contents into the panel
    copywin(raw_win, win, 0, 0, 1, width - raw_width - 1, raw_height, width - 2, 0);

    return 0;
}

int
call_flow_handle_key(PANEL *panel, int key)
{
    int i, raw_width, height, width;
    call_flow_info_t *info = call_flow_info(panel);
    call_flow_arrow_t *next, *prev;
    ui_t *next_panel;
    sip_call_t *call;
    int rnpag_steps = setting_get_intvalue(SETTING_CF_SCROLLSTEP);
    int action = -1;

    // Sanity check, this should not happen
    if (!info)
        return -1;

    getmaxyx(info->flow_win, height, width);

    // Check actions for this key
    while ((action = key_find_action(key, action)) != ERR) {
        // Check if we handle this action
        switch(action) {
            case ACTION_DOWN:
                // Check if there is a call below us
                if (!(next = call_flow_next_arrow(panel, info->cur_arrow)))
                    break;
                info->cur_line += call_flow_arrow_height(panel, info->cur_arrow);

                // If we are out of the bottom of the displayed list
                // refresh it starting in the next call
                if (info->cur_line >= height) {
                    info->cur_line -= call_flow_arrow_height(panel, info->first_arrow);
                    info->first_arrow = call_flow_next_arrow(panel, info->first_arrow);
                }
                info->cur_arrow = next;
                break;
            case ACTION_UP:
                // Get previous message
                if (!(prev = call_flow_prev_arrow(panel, info->cur_arrow)))
                    break;
                info->cur_line -= call_flow_arrow_height(panel, info->cur_arrow);
                info->cur_arrow = prev;
                if (info->cur_line <= 0) {
                    info->cur_line += call_flow_arrow_height(panel, prev);
                    info->first_arrow = prev;
                }
                break;
            case ACTION_HNPAGE:
                rnpag_steps = rnpag_steps / 2;
                /* no break */
            case ACTION_NPAGE:
                // Next page => N key down strokes
                for (i = 0; i < rnpag_steps; i++)
                    call_flow_handle_key(panel, KEY_DOWN);
                break;
            case ACTION_HPPAGE:
                rnpag_steps = rnpag_steps / 2;
                /* no break */
            case ACTION_PPAGE:
                // Prev page => N key up strokes
                for (i = 0; i < rnpag_steps; i++)
                    call_flow_handle_key(panel, KEY_UP);
                break;
            case ACTION_BEGIN:
                call_flow_set_group(info->group);
                break;
            case ACTION_END:
                call_flow_set_group(info->group);
                for (i=0; i < call_group_msg_count(info->group); i++)
                    call_flow_handle_key(panel, KEY_DOWN);
                break;
            case ACTION_SHOW_FLOW_EX:
                werase(panel_window(panel));
                if (call_group_count(info->group) == 1) {
                    call_group_add(info->group, call_get_xcall(vector_first(info->group->calls)));
                } else {
                    call = vector_first(info->group->calls);
                    vector_clear(info->group->calls);
                    call_group_add(info->group, call);
                }
                call_flow_set_group(info->group);
                break;
            case ACTION_SHOW_RAW:
                // KEY_R, display current call in raw mode
                ui_create_panel(PANEL_CALL_RAW);
                call_raw_set_group(info->group);
                break;
            case ACTION_DECREASE_RAW:
                raw_width = getmaxx(info->raw_win);
                if (raw_width - 2 > 1) {
                    setting_set_intvalue(SETTING_CF_RAWFIXEDWIDTH, raw_width - 2);
                }
                break;
            case ACTION_INCREASE_RAW:
                raw_width = getmaxx(info->raw_win);
                if (raw_width + 2 < COLS - 1) {
                    setting_set_intvalue(SETTING_CF_RAWFIXEDWIDTH, raw_width + 2);
                }
                break;
            case ACTION_RESET_RAW:
                setting_set_intvalue(SETTING_CF_RAWFIXEDWIDTH, -1);
                break;
            case ACTION_ONLY_SDP:
                // Toggle SDP mode
                info->group->sdp_only = !(info->group->sdp_only);
                // Disable sdp_only if there are not messages with sdp
                if (call_group_msg_count(info->group) == 0)
                    info->group->sdp_only = 0;
                // Reset screen
                call_flow_set_group(info->group);
                break;
            case ACTION_SDP_INFO:
                setting_toggle(SETTING_CF_SDP_INFO);
                break;
            case ACTION_TOGGLE_MEDIA:
                setting_toggle(SETTING_CF_MEDIA);
                // Force reload arrows
                call_flow_set_group(info->group);
                break;
            case ACTION_TOGGLE_RAW:
                setting_toggle(SETTING_CF_FORCERAW);
                break;
            case ACTION_COMPRESS:
                setting_toggle(SETTING_CF_SPLITCALLID);
                // Force columns reload
                call_flow_set_group(info->group);
                break;
            case ACTION_SELECT:
                if (!info->selected) {
                    info->selected = info->cur_arrow;
                } else {
                    if (info->selected == info->cur_arrow) {
                        info->selected = NULL;
                    } else {
                        // Show diff panel
                        next_panel = ui_create_panel(PANEL_MSG_DIFF);
                        msg_diff_set_msgs(ui_get_panel(next_panel),
                                          call_flow_arrow_message(info->selected),
                                          call_flow_arrow_message(info->cur_arrow));
                    }
                }
                break;
            case ACTION_CONFIRM:
                // KEY_ENTER, display current message in raw mode
                ui_create_panel(PANEL_CALL_RAW);
                call_raw_set_group(info->group);
                call_raw_set_msg(call_flow_arrow_message(info->cur_arrow));
                break;
            default:
                // Parse next action
                continue;
        }

        // We've handled this key, stop checking actions
        break;
    }

    // Return if this panel has handled or not the key
    return (action == ERR) ? key : 0;
}

int
call_flow_help(PANEL *panel)
{
    WINDOW *help_win;
    int height, width;

    // Create a new panel and show centered
    height = 27;
    width = 65;
    help_win = newwin(height, width, (LINES - height) / 2, (COLS - width) / 2);

    // Set the window title
    mvwprintw(help_win, 1, 18, "Call Flow Help");

    // Write border and boxes around the window
    wattron(help_win, COLOR_PAIR(CP_BLUE_ON_DEF));
    box(help_win, 0, 0);
    mvwhline(help_win, 2, 1, ACS_HLINE, 63);
    mvwhline(help_win, 7, 1, ACS_HLINE, 63);
    mvwhline(help_win, height - 3, 1, ACS_HLINE, 63);
    mvwaddch(help_win, 2, 0, ACS_LTEE);
    mvwaddch(help_win, 7, 0, ACS_LTEE);
    mvwaddch(help_win, height - 3, 0, ACS_LTEE);
    mvwaddch(help_win, 2, 64, ACS_RTEE);
    mvwaddch(help_win, 7, 64, ACS_RTEE);
    mvwaddch(help_win, height - 3, 64, ACS_RTEE);

    // Set the window footer (nice blue?)
    mvwprintw(help_win, height - 2, 20, "Press any key to continue");

    // Some brief explanation abotu what window shows
    wattron(help_win, COLOR_PAIR(CP_CYAN_ON_DEF));
    mvwprintw(help_win, 3, 2, "This window shows the messages from a call and its relative");
    mvwprintw(help_win, 4, 2, "ordered by sent or received time.");
    mvwprintw(help_win, 5, 2, "This panel is mosly used when capturing at proxy systems that");
    mvwprintw(help_win, 6, 2, "manages incoming and outgoing request between calls.");
    wattroff(help_win, COLOR_PAIR(CP_CYAN_ON_DEF));

    // A list of available keys in this window
    mvwprintw(help_win, 8, 2, "Available keys:");
    mvwprintw(help_win, 9, 2, "Esc/Q       Go back to Call list window");
    mvwprintw(help_win, 10, 2, "Enter       Show current message Raw");
    mvwprintw(help_win, 11, 2, "F1/h        Show this screen");
    mvwprintw(help_win, 12, 2, "F2/d        Toggle SDP Address:Port info");
    mvwprintw(help_win, 13, 2, "F3/m        Toggle RTP arrows display");
    mvwprintw(help_win, 14, 2, "F4/X        Show call-flow with X-CID/X-Call-ID dialog");
    mvwprintw(help_win, 15, 2, "F5/s        Toggle compressed view (One address <=> one column");
    mvwprintw(help_win, 16, 2, "F6/R        Show original call messages in raw mode");
    mvwprintw(help_win, 17, 2, "F7/c        Cycle between available color modes");
    mvwprintw(help_win, 18, 2, "F8/C        Turn on/off message syntax highlighting");
    mvwprintw(help_win, 19, 2, "F9/l        Turn on/off resolved addresses");
    mvwprintw(help_win, 20, 2, "9/0         Increase/Decrease raw preview size");
    mvwprintw(help_win, 21, 2, "t           Toggle raw preview display");
    mvwprintw(help_win, 22, 2, "T           Restore raw preview size");
    mvwprintw(help_win, 23, 2, "D           Only show SDP messages");


    // Press any key to close
    wgetch(help_win);

    return 0;
}

int
call_flow_set_group(sip_call_group_t *group)
{
    PANEL *panel;
    call_flow_info_t *info;

    if (!(panel = ui_get_panel(ui_find_by_type(PANEL_CALL_FLOW))))
        return -1;

    if (!(info = call_flow_info(panel)))
        return -1;

    vector_clear(info->columns);
    vector_clear(info->arrows);

    info->group = group;
    info->cur_arrow = info->first_arrow = info->selected = call_flow_next_arrow(panel, NULL);
    info->cur_line = 1;
    info->selected = NULL;
    info->last_msg = NULL;

    return 0;
}

void
call_flow_column_add(PANEL *panel, const char *callid, const char *address)
{
    call_flow_info_t *info;
    call_flow_column_t *column;
    vector_iter_t columns;
    char addr[ADDRESSLEN + 6];

    if (!(info = call_flow_info(panel)))
        return;

    if (!address || !strlen(address))
        return;

    // Coppy address to local var
    strcpy(addr, address);

    // when compressed view is enabled
    if (setting_enabled(SETTING_CF_SPLITCALLID)) {
        // Remove the port from the address
        sip_address_strip_port(addr);
        // Display the alias value of the address
        strcpy(addr, get_alias_value(addr));
    }

    if (call_flow_column_get(panel, callid, addr))
        return;

    columns = vector_iterator(info->columns);
    while ((column = vector_iterator_next(&columns))) {
        if (!strcasecmp(addr, column->addr) && column->colpos != 0 && !column->callid2) {
            column->callid2 = callid;
            return;
        }
    }

    column = sng_malloc(sizeof(call_flow_column_t));
    column->callid = callid;
    strcpy(column->addr, addr);
    column->colpos = vector_count(info->columns);
    vector_append(info->columns, column);
}

call_flow_column_t *
call_flow_column_get(PANEL *panel, const char *callid, const char *address)
{
    call_flow_info_t *info;
    call_flow_column_t *column;
    vector_iter_t columns;
    int match_port;
    char coladdr[ADDRESSLEN + 6];
    char addr[ADDRESSLEN + 6];
    char *dots;

    if (!(info = call_flow_info(panel)))
        return NULL;

    // Coppy address to local var
    strcpy(addr, address);

    // when compressed view is enabled
    if (setting_enabled(SETTING_CF_SPLITCALLID)) {
        // Remove the port from the address
        sip_address_strip_port(addr);
        // Display the alias value of the address
        strcpy(addr, get_alias_value(addr));
    }

    // Look for address or address:port ?
    match_port = (strchr(addr, ':') != NULL);

    columns = vector_iterator(info->columns);
    while ((column = vector_iterator_next(&columns))) {
        // Copy address:port column label
        strcpy(coladdr, column->addr);

        // Remove port if we want to match only address
        if (!match_port && (dots = strchr(coladdr, ':')))
            *dots = '\0';

        if (!strcasecmp(addr, coladdr)) {
            if (!match_port)
                return column;
            if (setting_enabled(SETTING_CF_SPLITCALLID))
                return column;
            if (column->callid && !strcasecmp(callid, column->callid))
                return column;
            if (column->callid2 && !strcasecmp(callid, column->callid2))
                return column;
        }
    }
    return NULL;
}

