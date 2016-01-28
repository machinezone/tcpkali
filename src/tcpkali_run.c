/*
 * Copyright (c) 2014, 2015, 2016  Machine Zone, Inc.
 *
 * Original author: Lev Walkin <lwalkin@machinezone.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.

 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "tcpkali_run.h"
#include "tcpkali_mavg.h"
#include "tcpkali_events.h"
#include "tcpkali_pacefier.h"
#include "tcpkali_terminfo.h"

static const char *
time_progress(double start, double now, double stop) {
    const char *clocks[] = {"🕛  ", "🕐  ", "🕑  ", "🕒  ", "🕓  ", "🕔  ",
                            "🕕  ", "🕖  ", "🕗  ", "🕘  ", "🕙  ", "🕚  "};
    if(!tcpkali_is_utf8()) return "";
    double span = (stop - start) / (sizeof(clocks) / sizeof(clocks[0]));
    int pos = (now - start) / span;
    if(pos < 0)
        pos = 0;
    else if(pos > 11)
        pos = 11;
    return clocks[pos];
}

static void
print_connections_line(int conns, int max_conns, int conns_counter) {
    int terminal_width = tcpkali_terminal_width();

    char info[terminal_width + 1];
    ssize_t info_width = snprintf(info, sizeof(info), "| %d of %d (%d)", conns,
                                  max_conns, conns_counter);

    int ribbon_width = terminal_width - info_width - 1;
    if(ribbon_width > 0.6 * terminal_width) ribbon_width = 0.6 * terminal_width;
    if(ribbon_width > 50) ribbon_width = 50;

    if(info_width > terminal_width || ribbon_width < 5) {
        /* Can't fit stuff on the screen, make dumb print-outs */
        printf("| %d of %d (%d)\n", conns, max_conns, conns_counter);
        return;
    }

    char ribbon[ribbon_width + 1];
    ribbon[0] = '|';
    int at = 1 + ((ribbon_width - 2) * conns) / max_conns;
    for(int i = 1; i < ribbon_width; i++) {
        if(i < at)
            ribbon[i] = '=';
        else if(i > at)
            ribbon[i] = '-';
        else if(i == at)
            ribbon[i] = '>';
    }
    ribbon[ribbon_width] = 0;
    fprintf(stderr, "%s%s%s\r", ribbon, info, tcpkali_clear_eol());
}

static void
format_latencies(char *buf, size_t size, struct latency_snapshot *latency) {
    if(latency->connect_histogram || latency->firstbyte_histogram
       || latency->marker_histogram) {
        char *p = buf;
        p += snprintf(p, size, " (");
        if(latency->connect_histogram)
            p += snprintf(
                p, size - (p - buf), "c=%.1f ",
                hdr_value_at_percentile(latency->connect_histogram, 95.0)
                    / 10.0);
        if(latency->firstbyte_histogram)
            p += snprintf(
                p, size - (p - buf), "fb=%.1f ",
                hdr_value_at_percentile(latency->firstbyte_histogram, 95.0)
                    / 10.0);
        if(latency->marker_histogram)
            p += snprintf(
                p, size - (p - buf), "m=%.1f ",
                hdr_value_at_percentile(latency->marker_histogram, 95.0)
                    / 10.0);
        snprintf(p, size - (p - buf), "ms⁹⁵ᵖ)");
    } else {
        buf[0] = '\0';
    }
}

int
open_connections_until_maxed_out(struct engine *eng, double connect_rate,
                                 int max_connections, double epoch_end,
                                 struct stats_checkpoint *checkpoint,
                                 mavg traffic_mavgs[2], Statsd *statsd,
                                 int *term_flag, enum work_phase phase,
                                 int print_stats) {
    tk_now_update(TK_DEFAULT);
    double now = tk_now(TK_DEFAULT);

    /*
     * It is a little bit better to batch the starts by issuing several
     * start commands per small time tick. Ends up doing less write()
     * operations per batch.
     * Therefore, we round the timeout_us upwards to the nearest millisecond.
     */
    long timeout_us = 1000 * ceil(1000.0 / connect_rate);
    if(timeout_us > 250000) timeout_us = 250000;

    struct pacefier keepup_pace;
    pacefier_init(&keepup_pace, now);

    ssize_t conn_deficit = 1; /* Assume connections still have to be est. */

    while(now < epoch_end && !*term_flag
          /* ...until we have all connections established or
           * we're in a steady state. */
          && (phase == PHASE_STEADY_STATE || conn_deficit > 0)) {
        usleep(timeout_us);
        tk_now_update(TK_DEFAULT);
        now = tk_now(TK_DEFAULT);
        int update_stats = (now - checkpoint->last_update) >= 0.25;

        size_t connecting, conns_in, conns_out, conns_counter;
        engine_get_connection_stats(eng, &connecting, &conns_in, &conns_out,
                                    &conns_counter);
        conn_deficit = max_connections - (connecting + conns_out);

        size_t allowed = pacefier_allow(&keepup_pace, connect_rate, now);
        size_t to_start = allowed;
        if(conn_deficit <= 0) {
            to_start = 0;
        }
        if(to_start > (size_t)conn_deficit) {
            to_start = conn_deficit;
        }
        engine_initiate_new_connections(eng, to_start);
        pacefier_moved(&keepup_pace, connect_rate, allowed, now);

        /* Do not update/print checkpoint stats too often. */
        if(update_stats) {
            checkpoint->last_update = now;
            /* Fall through and do the chekpoint update. */
        } else {
            continue;
        }

        /*
         * traffic_delta.* contains traffic observed within the last
         * period (now - checkpoint->last_stats_sent).
         */
        non_atomic_traffic_stats _last = checkpoint->last_traffic_stats;
        checkpoint->last_traffic_stats = engine_traffic(eng);
        non_atomic_traffic_stats traffic_delta =
            subtract_traffic_stats(checkpoint->last_traffic_stats, _last);

        mavg_bump(&traffic_mavgs[0], now, (double)traffic_delta.bytes_rcvd);
        mavg_bump(&traffic_mavgs[1], now, (double)traffic_delta.bytes_sent);

        double bps_in = 8 * mavg_per_second(&traffic_mavgs[0], now);
        double bps_out = 8 * mavg_per_second(&traffic_mavgs[1], now);

        engine_prepare_latency_snapshot(eng);
        struct latency_snapshot *latency = engine_collect_latency_snapshot(eng);
        report_to_statsd(statsd,
                         &(statsd_feedback){.opened = to_start,
                                            .conns_in = conns_in,
                                            .conns_out = conns_out,
                                            .bps_in = bps_in,
                                            .bps_out = bps_out,
                                            .traffic_delta = traffic_delta,
                                            .latency = latency});

        if(print_stats) {
            if(phase == PHASE_ESTABLISHING_CONNECTIONS) {
                print_connections_line(conns_out, max_connections,
                                       conns_counter);
            } else {
                char latency_buf[256];
                format_latencies(latency_buf, sizeof(latency_buf), latency);

                fprintf(stderr,
                        "%sTraffic %.3f↓, %.3f↑ Mbps "
                        "(conns %ld↓ %ld↑ %ld⇡; seen %ld)%s%s\r",
                        time_progress(checkpoint->epoch_start, now, epoch_end),
                        bps_in / 1000000.0, bps_out / 1000000.0, (long)conns_in,
                        (long)conns_out, (long)connecting, (long)conns_counter,
                        latency_buf, tcpkali_clear_eol());
            }
        }

        engine_free_latency_snapshot(latency);
    }

    return (now >= epoch_end || *term_flag) ? -1 : 0;
}