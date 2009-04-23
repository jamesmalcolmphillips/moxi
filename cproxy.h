/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */

#ifndef CPROXY_H
#define CPROXY_H

#include <glib.h>
#include <libmemcached/memcached.h>
#include "work.h"

int cproxy_init(const char *cfg, int nthreads, int downstream_max);

#define IS_PROXY(x) (x == proxy_upstream_ascii_prot || \
                     x == proxy_downstream_ascii_prot)

// -------------------------------

typedef struct proxy       proxy;
typedef struct proxy_td    proxy_td;
typedef struct proxy_main  proxy_main;
typedef struct proxy_stats proxy_stats;
typedef struct downstream  downstream;

/* Owned by main listener thread.
 */
struct proxy_main {
    int nthreads;       // Immutable.
    int downstream_max; // Immutable.

    // Start of proxy list.  Only the main listener thread
    // should access or modify this field.
    //
    proxy *proxy_head;

    uint64_t stat_configs;
    uint64_t stat_config_fails;
    uint64_t stat_proxy_starts;
    uint64_t stat_proxy_start_fails;
    uint64_t stat_proxy_existings;
    uint64_t stat_proxy_shutdowns;
};

/* Owned by main listener thread.
 */
struct proxy {
    int   port;   // Immutable.
    char *name;   // Mutable, covered by proxy_lock, for debugging, NULL-able.
    char *config; // Mutable, covered by proxy_lock, mem owned by proxy,
                  // might be NULL if the proxy is shutting down.

    // Mutable, covered by proxy_lock, incremented
    // whenever config changes.
    //
    uint32_t config_ver;

    // Any thread that accesses the mutable fields should
    // first acquire the proxy_lock.
    //
    pthread_mutex_t proxy_lock;

    // Number of listening conn's acting as a proxy,
    // where (((proxy *) conn->extra) == this).
    //
    uint64_t listening;
    uint64_t listening_failed; // When server_socket() failed.

    proxy *next; // Modified/accessed only by main listener thread.

    proxy_td *thread_data;     // Immutable.
    int       thread_data_num; // Immutable.
};

struct proxy_stats {
    // Naming convention is that num_xxx's go up and down,
    // while tot_xxx's only increase.
    //
    uint64_t num_upstream; // Current # of upstreams conns using this proxy.
    uint64_t tot_upstream; // Total # upstream conns that used this proxy.

    uint64_t num_downstream_conn;
    uint64_t tot_downstream_conn;
    uint64_t tot_downstream_released;
    uint64_t tot_downstream_reserved;
    uint64_t tot_downstream_freed;
    uint64_t tot_downstream_quit_server;
    uint64_t tot_downstream_max_reached;
    uint64_t tot_downstream_create_failed;
    uint64_t tot_assign_downstream;
    uint64_t tot_assign_upstream;
    uint64_t tot_reset_upstream_avail;
    uint64_t tot_oom;
    uint64_t tot_retry;
};

/* Owned by worker thread.
 */
struct proxy_td { // Per proxy, per worker-thread data struct.
    proxy *proxy; // Immutable parent pointer.

    // Upstream conns that are paused, waiting for
    // an available, released downstream.
    //
    conn *waiting_any_downstream_head;
    conn *waiting_any_downstream_tail;

    downstream *downstream_reserved; // Downstreams assigned to upstreams.
    downstream *downstream_released; // Downstreams unassigned to upstreams.
    uint64_t    downstream_tot;      // Total lifetime downstreams created.
    int         downstream_num;      // Number downstreams existing.
    int         downstream_max;      // Max downstream concurrency number.

    proxy_stats stats;
};

/* Owned by worker thread.
 */
struct downstream {
    proxy_td     *ptd;        // Immutable parent pointer.
    char         *config;     // Immutable, mem owned by downstream.
    uint32_t      config_ver; // Immutable, snapshot of proxy->config_ver.
    memcached_st  mst;        // Immutable, from libmemcached.

    downstream *next;         // To track reserved/free lists.

    conn **downstream_conns;  // Wraps the fd's of mst with conns.
    int    downstream_used;   // Number of in-use downstream conns, might
                              // be >1 during scatter-gather commands.
    int    downstream_used_start;
    conn  *upstream_conn;     // Non-NULL when downstream is reserved.
    char  *upstream_suffix;   // Last bit to write when downstreams are done.

    GHashTable *multiget; // Keyed by string.
    GHashTable *merger;   // Keyed by string, for merging replies like STATS.
};

// Functions.
//
proxy *cproxy_create(char *name, int port,
                     char *config, uint32_t config_ver,
                     int nthreads, int downstream_max);
int    cproxy_listen(proxy *p);

proxy_td *cproxy_find_thread_data(proxy *p, pthread_t thread_id);
void      cproxy_init_upstream_conn(conn *c);
void      cproxy_init_downstream_conn(conn *c);
void      cproxy_on_close_upstream_conn(conn *c);
void      cproxy_on_close_downstream_conn(conn *c);
void      cproxy_on_pause_downstream_conn(conn *c);

void        cproxy_add_downstream(proxy_td *ptd);
void        cproxy_free_downstream(downstream *d);
downstream *cproxy_create_downstream(char *config, uint32_t config_ver);
downstream *cproxy_reserve_downstream(proxy_td *ptd);
bool        cproxy_release_downstream(downstream *d, bool force);
void        cproxy_release_downstream_conn(downstream *d, conn *c);
bool        cproxy_check_downstream_config(downstream *d);

int   cproxy_connect_downstream(downstream *d, LIBEVENT_THREAD *thread);
void  cproxy_wait_any_downstream(proxy_td *ptd, conn *c);
void  cproxy_assign_downstream(proxy_td *ptd);

bool  cproxy_forward_ascii_downstream(downstream *d);
bool  cproxy_forward_ascii_multiget_downstream(downstream *d, conn *uc);
bool  cproxy_forward_ascii_simple_downstream(downstream *d, char *command,
                                             conn *uc);
bool  cproxy_forward_ascii_item_downstream(downstream *d, short cmd,
                                           item *it, conn *uc);
bool  cproxy_broadcast_ascii_downstream(downstream *d, char *command,
                                        conn *uc, char *suffix);

void  cproxy_pause_upstream_for_downstream(proxy_td *ptd, conn *upstream);
conn *cproxy_find_downstream_conn(downstream *d, char *key, int key_length);
int   cproxy_server_index(downstream *d, char *key, size_t key_length);
bool  cproxy_prep_conn_for_write(conn *c);
bool  cproxy_dettach_if_noreply(downstream *d, conn *uc);

void cproxy_reset_upstream(conn *uc);

void cproxy_process_upstream_ascii(conn *c, char *line);
void cproxy_process_upstream_ascii_nread(conn *c);

void cproxy_process_downstream_ascii(conn *c, char *line);
void cproxy_process_downstream_ascii_nread(conn *c);

rel_time_t cproxy_realtime(const time_t exptime);

void cproxy_close_conn(conn *c);

// Multiget key de-duplication.
//
typedef struct multiget_entry multiget_entry;

struct multiget_entry {
    conn           *upstream_conn;
    uint32_t        opaque; // For binary protocol.
    multiget_entry *next;
};

size_t   multiget_key_len(const char *key);
guint    multiget_key_hash(gconstpointer v);
gboolean multiget_key_equal(gconstpointer v1, gconstpointer v2);

void multiget_foreach_free(gpointer key,
                           gpointer value,
                           gpointer user_data);

void multiget_remove_upstream(gpointer key,
                              gpointer value,
                              gpointer user_data);

// Stats handling.
//
bool protocol_stats_merge(GHashTable *merger, char *line);

size_t   protocol_stats_key_len(const char *key);
guint    protocol_stats_key_hash(gconstpointer v);
gboolean protocol_stats_key_equal(gconstpointer v1, gconstpointer v2);

void protocol_stats_foreach_free(gpointer key,
                                 gpointer value,
                                 gpointer user_data);

void protocol_stats_foreach_write(gpointer key,
                                  gpointer value,
                                  gpointer user_data);

// TODO: The following generic items should be broken out into util file.
//
bool  add_conn_item(conn *c, item *it);
char *add_conn_suffix(conn *c);

size_t scan_tokens(char *command, token_t *tokens, const size_t max_tokens);

char *nread_text(short x);

#endif // CPROXY_H
