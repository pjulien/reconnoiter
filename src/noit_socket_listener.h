/*
 * Copyright (c) 2018, Circonus, Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name Circonnus, Inc. nor the names
 *       of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written
 *       permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. */

#include <mtev_defines.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <ctype.h>

#include <mtev_rest.h>
#include <mtev_hash.h>
#include <mtev_json.h>
#include <mtev_uuid.h>
#include <mtev_b64.h>
#include <mtev_dyn_buffer.h>
#include <ck_pr.h>

#include "noit_metric.h"
#include "noit_module.h"
#include "noit_check.h"
#include "noit_check_tools.h"
#include "noit_mtev_bridge.h"

typedef struct _mod_config {
  mtev_hash_table *options;
} listener_mod_config_t;

typedef struct listener_closure_s {
  noit_module_t *self;
  noit_check_t *check;
  mtev_boolean shutdown;
  int port;
  int rows_per_cycle;
  int ipv4_listen_fd;
  int ipv6_listen_fd;
  int32_t refcnt;
  mtev_log_stream_t nlerr;
  mtev_log_stream_t nldeb;
  char nlname[16];
  pthread_mutex_t flushlock;
  mtev_hash_table *immediate_metrics;
  int (*payload_handler)(noit_check_t *check, char *buffer, size_t len);
  int (*count_records)(char *buffer, size_t inlen, size_t *usedlen);
} listener_closure_t;

typedef struct listener_instance {
  mtev_dyn_buffer_t buffer;
  int subsequent_invocation;
  listener_closure_t *parent;
} listener_instance_t;

#define READ_CHUNK 32768

static inline size_t
count_integral_digits(const char *str, size_t len, mtev_boolean can_be_signed)
{
  size_t rval = 0;
  if (can_be_signed && len > 0 && *str == '-') {
    rval++;
    str++;
  }
  while (rval < len) {
    if (*str < '0' || *str > '9') return rval;
    str++;
    rval++;
  }
  return rval;
}

listener_closure_t *listener_closure_alloc(const char *name, noit_module_t *mod, noit_check_t *check,
                                           mtev_log_stream_t deb, mtev_log_stream_t err,
                                           int (*)(noit_check_t *check, char *buffer, size_t len),
                                           int (*)(char *buffer, size_t inlen, size_t *usedlen));
void listener_closure_ref(listener_closure_t *lc);
int listener_submit(noit_module_t *self, noit_check_t *check, noit_check_t *cause);
void listener_metric_track_or_log(void *vrxc, const char *name, 
                                  metric_type_t t, const void *vp, struct timeval *w);
int listener_handler(eventer_t e, int mask, void *closure, struct timeval *now);
int listener_listen_handler(eventer_t e, int mask, void *closure, struct timeval *now);
int listener_mtev_listener(eventer_t e, int mask, void *closure, struct timeval *now);
void listener_listen_describe_callback(char *buffer, int size, eventer_t e, void *closure);
void listener_describe_callback(char *buffer, int size, eventer_t e, void *closure);
void listener_describe_mtev_callback(char *buffer, int size, eventer_t e, void *closure);
int noit_listener_config(noit_module_t *self, mtev_hash_table *options);
void noit_listener_cleanup(noit_module_t *self, noit_check_t *check);
void listener_onload();
