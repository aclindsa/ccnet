/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 *
 * Copyright (C) 2010 GongGeng
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, 
 * Boston, MA 02111-1307, USA.
 */

#include "common.h"

#include <event.h>
/* #include <event2/event.h> */
#include <stdio.h>
#include <stdlib.h>

#ifdef WIN32
    #include <winsock2.h>
#else
    #include <netinet/in.h>
    #include <arpa/inet.h>
#endif
#include "net.h"

#include <ctype.h>

#include "timer.h"

#include "peer.h"
#include "packet-io.h"

#include "rsa.h"

#include "session.h"
#include "peer-mgr.h"
#include "perm-mgr.h"
#include "processor.h"
#include "proc-factory.h"
#include "processors/service-proxy-proc.h"
#include "connect-mgr.h"

#include "utils.h"

#define DEBUG_FLAG  CCNET_DEBUG_PEER
#include "log.h"


enum {
    DOWN_SIG,                   /* connection down */
    AUTH_DONE_SIG,              /* peer become reachable and the auth state
                                   is AUTH_FULL, see keepalive-proc */

    AUTH_UPDATED_SIG,

    LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

#define OBJECT_TYPE_STRING "peer"

#include "../lib/peer-common.h"

void ccnet_peer_set_net_state (CcnetPeer *peer, int net_state);
static void ccnet_peer_finalize (GObject *object);

static void shutdown_processors (CcnetPeer *peer);

static void
set_property (GObject *object, guint property_id, 
              const GValue *v, GParamSpec *pspec)
{
    set_property_common (object, property_id, v, pspec);
}

void
ccnet_peer_finalize (GObject *object)
{
    CcnetPeer *peer = CCNET_PEER (object);

    g_free (peer->name);
    g_free (peer->addr_str);
    g_free (peer->service_url);
    g_free (peer->login_error);
    g_hash_table_unref (peer->processors);
    evbuffer_free (peer->packet);

    if (peer->pubkey)
        RSA_free (peer->pubkey);

    G_OBJECT_CLASS(ccnet_peer_parent_class)->finalize (object);
}

void
ccnet_peer_free (CcnetPeer *peer)
{
    ccnet_peer_shutdown (peer);
    g_object_unref (peer);
}

static void
ccnet_peer_class_init (CcnetPeerClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

    gobject_class->finalize = ccnet_peer_finalize;
    gobject_class->get_property = get_property;
    gobject_class->set_property = set_property;

    define_properties (gobject_class);

    signals[AUTH_DONE_SIG] = 
        g_signal_new ("auth-done", CCNET_TYPE_PEER, 
                      G_SIGNAL_RUN_LAST,
                      0,        /* no class singal handler */
                      NULL, NULL, /* no accumulator */
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);

    signals[DOWN_SIG] = 
        g_signal_new ("down", CCNET_TYPE_PEER, 
                      G_SIGNAL_RUN_LAST,
                      0,        /* no class singal handler */
                      NULL, NULL, /* no accumulator */
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);

    signals[AUTH_UPDATED_SIG] = 
        g_signal_new ("auth-updated", CCNET_TYPE_PEER, 
                      G_SIGNAL_RUN_LAST,
                      0,        /* no class singal handler */
                      NULL, NULL, /* no accumulator */
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);

}

static void
ccnet_peer_init (CcnetPeer *peer)
{

}


CcnetPeer*
ccnet_peer_new (const char *id)
{
    CcnetPeer *peer;

    peer = g_object_new (CCNET_TYPE_PEER, NULL);
    g_assert (strlen(id) == 40);
    memcpy (peer->id, id, 40);
    peer->id[40] = '\0';

    peer->net_state = PEER_DOWN;
    peer->public_port = 0;

    peer->processors = g_hash_table_new_full (g_direct_hash,  g_direct_equal, 
                                              NULL, NULL);

    peer->reqID = CCNET_USER_ID_START;

    peer->packet = evbuffer_new ();

    return peer;
}


inline static void append_string_property (GString *buf, const char *name, 
                                    const char *value)
{
    if (value && value[0])
        g_string_append_printf (buf, "%s %s\n", name, value);
}

inline static void append_int_property (GString *buf, const char *name, 
                                 int value)
{
    if (value != -1)
        g_string_append_printf (buf, "%s %d\n", name, value);
}

inline static void append_int64_property (GString *buf, const char *name, 
                                          gint64 value)
{
    g_string_append_printf (buf, "%s %"G_GINT64_FORMAT"\n", name, value);
}


GString*
ccnet_peer_to_string (CcnetPeer *peer)
{
    GString *buf = g_string_new (NULL);
    g_string_append (buf, "peer/");
    g_string_append (buf, peer->id);
    g_string_append (buf, "\n");

    append_string_property (buf, "name", peer->name);
    append_string_property (buf, "service-url", peer->service_url);

    if (peer->pubkey) {
        GString *str = public_key_to_gstring(peer->pubkey);
        g_string_append_printf (buf, "%s %s\n", "pubkey", str->str);
        g_string_free(str, TRUE);
    }

    return buf;
}


static void parse_field (CcnetPeer *peer, const char *key, char *value)
{
    if (strcmp(key, "name") == 0) {
        g_free (peer->name);
        peer->name = g_strdup(value);
        return;
    }
    
    if (strcmp(key, "service-url") == 0) {
        g_free (peer->service_url);
        peer->service_url = g_strdup(value);
        return;
    }

    if (strcmp(key, "pubkey") == 0) {
        if (peer->pubkey)
            RSA_free(peer->pubkey);
        peer->pubkey = public_key_from_string (value);
        return;
    }
}

CcnetPeer*
ccnet_peer_from_string (char *content)
{
    CcnetPeer *peer = NULL;

    char *ptr, *start = content;
    
    if ( !(ptr = strchr(start, '\n')) ) return NULL;
    *ptr = '\0';
    char *object_id = start;
    start = ptr + 1;
    
    char *object_type = ccnet_object_type_from_id (object_id);
    if (g_strcmp0(object_type, OBJECT_TYPE_STRING) != 0)
        goto out;

    char *pure_id = object_id + strlen(object_type) + 1;
    if (!peer_id_valid(pure_id)) {
        ccnet_warning ("Wrong peer id %s\n", pure_id);
        goto out;
    }

    peer = ccnet_peer_new (pure_id);
    parse_key_value_pairs (
        start, (KeyValueFunc)parse_field, peer);

out:
    g_free (object_type);
    return peer;
}

void
ccnet_peer_update_from_string (CcnetPeer *peer, char *content)
{
    char *ptr, *start = content;
    
    if ( !(ptr = strchr(start, '\n')) ) return;
    *ptr = '\0';
    char *object_id = start;
    start = ptr + 1;
    
    char *object_type = ccnet_object_type_from_id (object_id);
    if (strcmp(object_type, OBJECT_TYPE_STRING) != 0)
        goto out;

    char *pure_id = object_id + strlen(object_type) + 1;
    g_return_if_fail (strcmp(pure_id, peer->id) == 0);
    
    parse_key_value_pairs (
        start, (KeyValueFunc)parse_field, peer);

    peer->need_saving = 1;
out:
    g_free (object_type);
}

void
ccnet_peer_set_net_state (CcnetPeer *peer, int net_state)
{
    /* do not need saving */

    if (peer->net_state == net_state)
        return;

    ccnet_debug ("[Peer] Peer %s(%.8s) net state changed: %s->%s\n",
                 peer->name, peer->id,
                 ccnet_peer_get_net_state_string(peer->net_state),
                 ccnet_peer_get_net_state_string(net_state));
    peer->last_net_state = peer->net_state;

    if (net_state == PEER_DOWN) {
        g_assert (peer->io == NULL);
        g_assert (g_hash_table_size(peer->processors) == 0);
        if (!peer->is_local)
            --peer->manager->connected_peer;
    } else
        if (!peer->is_local)
            ++peer->manager->connected_peer;

    if (net_state == PEER_CONNECTED && !peer->io->is_incoming)
        g_object_set (peer, "can-connect", 1, NULL);

    g_object_set (peer, "net-state", net_state, NULL);
}

static void
ccnet_peer_set_addr_str (CcnetPeer *peer, const char *addr_str)
{
    /* do not need saving */
    if (!addr_str)
        return;
    if (peer->addr_str && strcmp(addr_str, peer->addr_str) == 0)
        return;

    g_object_set (peer, "ip", addr_str, NULL);
    ccnet_debug ("[Peer] Updated peer %s(%.10s) address %s\n",
                 peer->name, peer->id, addr_str);
}

void
ccnet_peer_update_address (CcnetPeer *peer, const char *addr_str,
                           unsigned short port)
{
    if (!is_valid_ipaddr (addr_str))
        return;

    ccnet_peer_set_addr_str (peer, addr_str);

    if (port == 0)
        return;
    peer->port = port;
}

void ccnet_peer_set_pubkey (CcnetPeer *peer, char *str)
{
    g_object_set (peer, "pubkey", str, NULL);
    if (!peer->pubkey)
        ccnet_warning("Wrong public key format\n");
    peer->need_saving = 1;
}


/* -------- role management -------- */

void
ccnet_peer_add_role (CcnetPeer *peer, const char *role)
{
    if (!ccnet_peer_has_role(peer, role)) {
        peer->role_list = string_list_append_sorted (
            peer->role_list, role);
    }
}

void
ccnet_peer_remove_role (CcnetPeer *peer, const char *role)
{
    g_return_if_fail (role != NULL);

    if (!string_list_is_exists(peer->role_list, role))
        return;

    peer->role_list = string_list_remove (peer->role_list, role);
}

gboolean
ccnet_peer_has_role (CcnetPeer *peer, const char *role)
{
    return string_list_is_exists(peer->role_list, role);
}

gboolean
ccnet_peer_has_my_role (CcnetPeer *peer, const char *role)
{
    return string_list_is_exists(peer->myrole_list, role);
}

void
ccnet_peer_set_roles (CcnetPeer *peer, const char *roles)
{
    GList *role_list = string_list_parse_sorted (roles, ",");
    string_list_free (peer->role_list);
    peer->role_list = role_list;
}

void
ccnet_peer_set_myroles (CcnetPeer *peer, const char *roles)
{
    GList *role_list = string_list_parse_sorted (roles, ",");
    string_list_free (peer->myrole_list);
    peer->myrole_list = role_list;

    /* ccnet_debug ("[Peer] Myrole on %s(%.8s) is set to %s\n",  */
    /*              peer->id, peer->name, roles); */
}

void
ccnet_peer_get_roles_str (CcnetPeer *peer, GString* buf)
{
    string_list_join (peer->role_list, buf, ",");
}

void
ccnet_peer_get_myroles_str (CcnetPeer *peer, GString* buf)
{
    string_list_join (peer->myrole_list, buf, ",");
}



/* ----------- Packet Handling & Networking   --------------------- */

static void remove_write_callbacks (CcnetPeer *peer)
{
    g_list_foreach (peer->write_cbs, (GFunc)g_free, NULL);
    g_list_free (peer->write_cbs);
}

void
ccnet_peer_shutdown (CcnetPeer *peer)
{
    /* if (peer->net_state == PEER_DOWN) */
    /*     return; */

    if (peer->in_shutdown)
        return;

    peer->in_shutdown = 1;

    if (peer->net_state == PEER_CONNECTED) {
        peer->last_down = time(NULL);
        ccnet_packet_io_free (peer->io);
        peer->io = NULL;
        g_object_set (peer, "can-connect", 0, NULL);
    }
    peer->is_ready = 0;
    peer->dns_done = 0;

    ccnet_debug ("Shutdown all processors for peer %s\n", peer->name);
    shutdown_processors (peer);
    remove_write_callbacks (peer);

    ccnet_peer_set_net_state (peer, PEER_DOWN);

    g_signal_emit (peer, signals[DOWN_SIG], 0);

    peer->in_shutdown = 0;
}


static void
create_remote_processor (CcnetPeer *peer, CcnetPeer *remote_peer,
                         int req_id, int argc, char **argv)
{
    CcnetProcessor *processor;
    CcnetProcFactory *factory = peer->manager->session->proc_factory;

    processor = ccnet_proc_factory_create_slave_processor (
        factory,"service-proxy", peer, req_id);
    ccnet_processor_start (processor, 0, NULL);

    ccnet_service_proxy_invoke_remote (processor, remote_peer, argc, argv);
}

static void
create_local_processor (CcnetPeer *peer, int req_id, int argc, char **argv)
{
    CcnetProcessor *processor;
    CcnetProcFactory *factory = peer->manager->session->proc_factory;

    processor = ccnet_proc_factory_create_slave_processor (
        factory, argv[0], peer, req_id);

    if (processor) {
        ccnet_processor_start (processor, argc-1, argv+1);
    } else {
        CcnetService *service;

        service = ccnet_session_get_service (peer->manager->session, argv[0]);
        if (service != NULL) {
            processor = ccnet_proc_factory_create_slave_processor (
                factory, "service-proxy", peer, req_id);
            ccnet_processor_start (processor, 0, NULL);
            ccnet_service_proxy_invoke_local (processor, service->provider,
                                              argc, argv);
        } else {
            ccnet_peer_send_response (peer, req_id, SC_UNKNOWN_SERVICE,
                                    SS_UNKNOWN_SERVICE,
                                    NULL, 0);
            ccnet_warning ("Unknown service %s invoke by %s(%.8s)\n",
                           argv[0], peer->name, peer->id);
        }
    }
}

static void create_processor (CcnetPeer *peer, int req_id,
                             int argc, char **argv)
{
    CcnetSession *session = peer->manager->session;

    if (strcmp(argv[0], "remote") == 0) {
        /* we have check this before (in permission checking) */
        g_assert (peer->is_local);

        CcnetPeer *remote_peer;

        remote_peer = ccnet_peer_manager_get_peer (peer->manager, argv[1]);
        if (!remote_peer) {
            ccnet_peer_send_response (peer, req_id, SC_UNKNOWN_PEER,
                                    SS_UNKNOWN_PEER, NULL, 0);
            ccnet_warning ("Unknown remote peer in invoking remote service\n");
            return;
        }
        /* if (remote_peer->net_state == PEER_DOWN) { */
        /*     ccnet_peerSendResponse (peer, req_id, SC_PEER_UNREACHABLE, */
        /*                             SS_PEER_UNREACHABLE, NULL, 0); */
        /*     ccnet_warning ("Unreachable remote peer in invoking remote service\n"); */
        /*     return; */
        /* } */

        /* To simplify caller's logic, we allow starting a remote processor to
         * local host. Translate this call into a local one.
         */
        if (session->myself == remote_peer) {
            create_local_processor (peer, req_id, argc-2, argv+2);
            g_object_unref (remote_peer);
            return;
        }
        
        create_remote_processor (peer, remote_peer, req_id, argc-2, argv+2);
        g_object_unref (remote_peer);
        return;
    }

    create_local_processor (peer, req_id, argc, argv);
}

static void
handle_request (CcnetPeer *peer, int req_id, char *data, int len)
{
    char *msg;
    gchar **commands;
    gchar **pcmd;
    int  i, perm;

    /* TODO: remove string copy */
    g_assert (len >= 1);
    msg = g_malloc (len+1);
    memcpy (msg, data, len);
    msg[len] = '\0';

    commands = g_strsplit_set (msg, " \t", 10);
    for (i=0, pcmd = commands; *pcmd; pcmd++)
        i++;
    g_assert (i > 0);
    g_free (msg);

    /* permission checking */
    if (!peer->is_local) {
        perm = ccnet_perm_manager_check_permission(peer->manager->session->perm_mgr,
                                                   peer, commands[0],
                                                   req_id,
                                                   i, commands);
        if (perm == PERM_CHECK_ERROR) {
            ccnet_peer_send_response (peer, req_id, SC_PERM_ERR, SS_PERM_ERR,
                                      NULL, 0);
            goto ret;
        } else if (perm == PERM_CHECK_DELAY) {
            ccnet_peer_send_response (peer, req_id, SC_PERM_ERR, SS_PERM_ERR,
                                      NULL, 0);
            goto ret;
        } else if (perm == PERM_CHECK_NOSERVICE) {
            ccnet_peer_send_response (peer, req_id, SC_UNKNOWN_SERVICE,
                                      SS_UNKNOWN_SERVICE, NULL, 0);
            goto ret;
        }
    }

    /* check duplication request */
    CcnetProcessor *processor;
    processor = ccnet_peer_get_processor (peer, SLAVE_ID(req_id));
    if (processor != NULL) {
        ccnet_warning ("Received duplication request, id is %d\n", req_id);
        goto ret;
    }

    create_processor (peer, req_id, i, commands);

ret:
    g_strfreev (commands);
}

static void
handle_response (CcnetPeer *peer, int req_id, char *data, int len)
{
    CcnetProcessor *processor;
    char *code, *code_msg = 0, *content = 0;
    int clen;
    char *ptr, *end;

    if (len < 4)
        goto error;

    code = data;
    
    ptr = data + 3;
    if (*ptr == '\n') {
        /* no code_msg */
        *ptr++ = '\0';
        content = ptr;
        clen = len - (ptr - data);
        goto parsed;
    }
    
    if (*ptr != ' ')
        goto error;
    
    *ptr++ = '\0';
    code_msg = ptr;

    end = data + len;
    for (ptr = data; *ptr != '\n' && ptr != end; ptr++) ;

    if (ptr == end)             /* must end with '\n' */
        goto error;

    /* if (*(ptr-1) == '\r') */
    /*     *(ptr-1) = '\0'; */
    *ptr++ = '\0';
    content = ptr;
    clen = len - (ptr - data);
    
parsed:
    processor = ccnet_peer_get_processor (peer, MASTER_ID (req_id));
    if (processor == NULL) {
        /* do nothing if receiving SC_PROC_DEAD and the processor on
         * this side is also not present. Otherwise send SC_PROC_DEAD
         */
        if (memcmp(code, SC_PROC_DEAD, 3) != 0) {
            ccnet_warning ("Delayed response from %s(%.10s), id is %d, %s %s\n",
                           peer->name, peer->id, req_id, code, code_msg);
            ccnet_peer_send_update (peer, req_id,
                                    SC_PROC_DEAD, SS_PROC_DEAD,
                                    NULL, 0);
        }
        return;
    }
    /* if (!peer->is_local) */
    /*     ccnet_debug ("[RECV] handle_response %s id is %d, %s %s\n", */
    /*                  GET_PNAME(processor), PRINT_ID(processor->id), */
    /*                  code, code_msg); */

    ccnet_processor_handle_response (processor, code, code_msg, content, clen);
    return;

error:
    ccnet_warning ("Bad response format from %s\n", peer->id);
}

static void
handle_update (CcnetPeer *peer, int req_id, char *data, int len)
{
    CcnetProcessor *processor;
    char *code, *code_msg = 0, *content = 0;
    int clen;
    char *ptr, *end;

    if (len < 4)
        goto error;
    
    code = data;
    
    ptr = data + 3;
    if (*ptr == '\n') {
        /* no code_msg */
        *ptr++ = '\0';
        content = ptr;
        clen = len - (ptr - data);
        goto parsed;
    }
    
    if (*ptr != ' ')
        goto error;
    
    *ptr++ = '\0';
    code_msg = ptr;

    end = data + len;
    for (ptr = data; *ptr != '\n' && ptr != end; ptr++) ;

    if (ptr == end)             /* must end with '\n' */
        goto error;

    /* if (*(ptr-1) == '\r') */
    /*     *(ptr-1) = '\0'; */
    *ptr++ = '\0';
    content = ptr;
    clen = len - (ptr - data);
    
parsed:
    processor = ccnet_peer_get_processor (peer, SLAVE_ID(req_id));
    if (processor == NULL) {
        if (memcmp(code, SC_PROC_DEAD, 3) != 0 
            && memcmp(code, SC_PROC_DONE, 3) != 0) {
            ccnet_warning ("Delayed update from %s(%.8s), id is %d, %s %s\n",
                           peer->name, peer->id, req_id, code, code_msg);
            ccnet_peer_send_response (peer, req_id,
                                      SC_PROC_DEAD, SS_PROC_DEAD,
                                      NULL, 0);
        }
        return;
    }

    /* if (!peer->is_local) */
    /*     ccnet_debug ("[RECV] handle_update %s id is %d, %s %s\n", */
    /*                  GET_PNAME(processor), PRINT_ID(processor->id), */
    /*                  code, code_msg); */
    
    ccnet_processor_handle_update (processor, code, code_msg, content, clen);
    return;

error:
    ccnet_warning ("Bad update format from %s\n", peer->id);
}

static void canRead (ccnet_packet *packet, void *vpeer);


static void
canRead (ccnet_packet *packet, void *vpeer)
{
    CcnetPeer *peer = vpeer;
    g_object_ref (peer);

    /* if (!peer->is_local) */
    /*     ccnet_debug ("[RECV] Recieve packat from %s type is %d, id is %d\n", */
    /*                  peer->id, packet->header.type, packet->header.id); */

    switch (packet->header.type) {
    case CCNET_MSG_REQUEST:
        handle_request (peer, packet->header.id, 
                       packet->data, packet->header.length);
        break;
    case CCNET_MSG_RESPONSE:
        handle_response (peer, packet->header.id, 
                        packet->data, packet->header.length);
        break;
    case CCNET_MSG_UPDATE:
        handle_update (peer, packet->header.id, 
                      packet->data, packet->header.length);
        break;
    default: 
        ccnet_warning ("Unknown header type %d\n", packet->header.type);
        g_assert (0);
    };

    g_object_unref (peer);
}

struct WriteCallback {
    int   removing : 1;

    PeerWriteCallback func;
    void *user_data;
};

void
ccnet_peer_add_write_callback (CcnetPeer *peer,
                               PeerWriteCallback func,
                               void *user_data)
{
    if (peer->net_state == PEER_CONNECTED) {
        struct WriteCallback *wcb = g_new0 (struct WriteCallback, 1);
        wcb->func = func;
        wcb->user_data = user_data;

        peer->write_cbs = g_list_prepend (peer->write_cbs, wcb);
    } else {
        ccnet_warning ("add_write_callback error: Peer not reachable\n");
    }
}

void
ccnet_peer_remove_write_callback (CcnetPeer *peer,
                                  PeerWriteCallback func,
                                  void *user_data)
{
    GList *ptr;

    g_assert (!peer->in_writecb);

    for (ptr = peer->write_cbs; ptr; ptr = ptr->next) {
        struct WriteCallback *wcb = ptr->data;
        if (wcb->func == func && wcb->user_data == user_data) {
            peer->write_cbs = g_list_delete_link (peer->write_cbs, ptr);
            g_free (wcb);
            return;
        }
    }
}


static void
didWrite(struct bufferevent * evin, void * vpeer)
{
    CcnetPeer *peer = vpeer;
    GList *ptr;

    g_object_ref (peer);
    peer->in_writecb = 1;

    for (ptr = peer->write_cbs; ptr; ) {
        struct WriteCallback *wcb = ptr->data;
        GList *cur = ptr;
        ptr = ptr->next;
        if (wcb->func(peer, wcb->user_data) == FALSE) {
            peer->write_cbs = g_list_delete_link (peer->write_cbs, cur);
            g_free (wcb);
        }
    }
    
    peer->in_writecb = 0;
    g_object_unref (peer);
}



static void
gotError (struct bufferevent *evbuf, short what, void *vpeer)
{
    CcnetPeer *peer = vpeer;
    g_object_ref (peer);
    /* ccnet_warning ("libevent got an error on peer %s what==%d, errno=%d (%s)\n", */
    /*                peer->name,  (int)what, errno, strerror(errno)); */

    if (what & EVBUFFER_TIMEOUT) {
        ccnet_warning ("libevent got a timeout for peer %s(%.8s), what=%hd, timeout secs=%ld\n",
                       peer->name, peer->id, what, *((time_t *)&(evbuf->timeout_read)));
        ccnet_message ("Peer %s (%.10s) down for timeout\n", peer->name, peer->id);
        peer->num_fails++;
        ccnet_peer_shutdown (peer);
    }

    if (what & (EVBUFFER_EOF | EVBUFFER_ERROR)) {
        if (what & EVBUFFER_ERROR)
            ccnet_warning ("libevent got an error! what=%hd, errno=%d (%s)\n",
                           what, errno, strerror(errno));
        if (peer->is_local) {
             ccnet_message ("Local peer down\n");
             ccnet_peer_shutdown (peer);
             ccnet_session_unregister_service (peer->manager->session, peer);
             ccnet_peer_manager_remove_local_peer (peer->manager,
                                                   peer);
        } else {
            ccnet_message ("[Net Error] Peer %s (%.10s) down\n", peer->name, peer->id);
            peer->num_fails++;
            ccnet_peer_shutdown (peer);
        }
    }

    g_object_unref (peer);
}

/* static void */
/* ccnet_peer_reset_io (CcnetPeer *peer) */
/* { */
/*     ccnet_packet_io_set_timeout_secs (peer->io, 0);  /\* disable timeout *\/ */
/*     ccnet_packet_io_set_iofuncs (peer->io, canRead, didWrite, gotError, peer); */
/* } */

void
ccnet_peer_set_io (CcnetPeer *peer, CcnetPacketIO *io)
{
    g_assert (peer->io == NULL);

    peer->io = io;
    /* libevent remove a previous timeout seems not work in libevent 2.0, 
       so we have to disable timeout by set it to a large value */
    if (!peer->is_local)
        ccnet_packet_io_set_timeout_secs (peer->io, 10000);
    ccnet_packet_io_set_iofuncs (peer->io, canRead, didWrite, gotError, peer);
}


int
ccnet_peer_get_request_id (CcnetPeer *peer)
{
    return (++peer->reqID);
}


#undef DEBUG_FLAG
/* #define DEBUG_FLAG  CCNET_DEBUG_NETIO */
#include "log.h"

void
ccnet_peer_packet_prepare (const CcnetPeer *peer, int type, int id)
{
    ccnet_header header;
    g_assert (peer->packet && EVBUFFER_LENGTH(peer->packet) == 0);

    header.version = 1;
    header.type = type;
    header.length = 0;
    header.id = htonl (id);
    evbuffer_add (peer->packet, &header, sizeof (header));
}

void
ccnet_peer_packet_write_string (const CcnetPeer *peer, const char *str)
{
    int len;

    g_assert (str);
    len = strlen(str);
    evbuffer_add (peer->packet, str, len);
}

void
ccnet_peer_packet_finish (const CcnetPeer *peer)
{
    ccnet_header *header;
    header = (ccnet_header *) EVBUFFER_DATA(peer->packet);
    header->length = htons (EVBUFFER_LENGTH(peer->packet)
                          - CCNET_PACKET_LENGTH_HEADER);
}

void
ccnet_peer_packet_send (const CcnetPeer *peer)
{
    int ret = 0;
    if (peer->is_local) {
        bufferevent_write_buffer (peer->io->bufev, peer->packet);
        return;
    }

    if (peer->net_state == PEER_CONNECTED) {
        ret = bufferevent_write_buffer (peer->io->bufev, peer->packet);
        if (ret < 0)
            ccnet_warning ("[SEND] bufferevent failed to send packet to peer(%.8s) \n",
                peer->id);
    } else {
        ccnet_warning ("Unable to send packet when peer is not connected.\n");
        evbuffer_drain (peer->packet, EVBUFFER_LENGTH(peer->packet));
    }
}

void
ccnet_peer_packet_finish_send (const CcnetPeer *peer)
{
    ccnet_peer_packet_finish (peer);
    ccnet_peer_packet_send (peer);
}

void
ccnet_peer_send_request (const CcnetPeer *peer, int req_id, const char *req)
{
    if (!peer->is_local)
        ccnet_debug ("[network] Send a request: id %d, cmd %s\n", req_id, req);
    ccnet_peer_packet_prepare (peer, CCNET_MSG_REQUEST, req_id);
    ccnet_peer_packet_write_string (peer, req);
    ccnet_peer_packet_finish_send (peer);
}


void
ccnet_peer_send_response (const CcnetPeer *peer, int req_id, 
                          const char *code, const char *reason,
                          const char *content, int clen)
{
    g_assert (req_id > 0);
    if ( (strlen(code) != 3) || !isdigit(code[0]) || !isdigit(code[1])
         || !isdigit(code[1]) ) {
        ccnet_warning ("Bad code number\n");
        return;
    }

    g_return_if_fail (clen < 65536);

    ccnet_peer_packet_prepare (peer, CCNET_MSG_RESPONSE, req_id);

    /* code line */
    evbuffer_add (peer->packet, code, 3);
    if (reason) {
        evbuffer_add (peer->packet, " ", 1);
        ccnet_peer_packet_write_string (peer, reason);
    }
    evbuffer_add (peer->packet, "\n", 1);

    if (content)
        evbuffer_add (peer->packet, content, clen);

    ccnet_peer_packet_finish_send (peer);

    if (!peer->is_local)
        ccnet_debug ("[SEND] Send a response: id %d code %s %s\n",
                     req_id, code, reason);
}

void
ccnet_peer_send_update (const CcnetPeer *peer, int req_id,
                        const char *code, const char *reason,
                        const char *content, int clen)
{
    g_assert (req_id > 0);

    ccnet_peer_packet_prepare (peer, CCNET_MSG_UPDATE, req_id);

    /* code line */
    evbuffer_add (peer->packet, code, 3);
    if (reason) {
        evbuffer_add (peer->packet, " ", 1);
        ccnet_peer_packet_write_string (peer, reason);
    }
    evbuffer_add (peer->packet, "\n", 1);

    if (content)
        evbuffer_add (peer->packet, content, clen);

    ccnet_peer_packet_finish_send (peer);

    if (!peer->is_local)
        ccnet_debug ("[SEND] Send an update: id %d code %s %s\n",
                     req_id, code, reason?reason:"NULL");
}


/* ----------------  Processors ---------------- */

#undef DEBUG_FLAG
#define DEBUG_FLAG  CCNET_DEBUG_PROCESSOR
#include "log.h"

void
ccnet_peer_add_processor (CcnetPeer *peer, CcnetProcessor *processor)
{
    ccnet_debug ("[Proc] Add %s(%d) to peer %s\n", GET_PNAME(processor), 
                 PRINT_ID(processor->id), peer->name);
    g_hash_table_insert (peer->processors, (gpointer)(long)processor->id, processor);
    processor->detached = 0;
}


void
ccnet_peer_remove_processor (CcnetPeer *peer, CcnetProcessor *processor)
{
    /* ccnet_debug ("[Proc] Remove %s(%d) from peer %s\n", GET_PNAME(processor),  */
    /*              PRINT_ID(processor->id), peer->name); */
    g_hash_table_remove (peer->processors, (gpointer)(long)processor->id);
    processor->detached = 1;
}


CcnetProcessor *
ccnet_peer_get_processor (CcnetPeer *peer, unsigned int id)
{
    return g_hash_table_lookup (peer->processors, (gpointer)(long)id);
}


static void shutdown_processors (CcnetPeer *peer)
{
    ccnet_proc_factory_shutdown_processors (
        peer->manager->session->proc_factory, peer);
}


/* -------- redirect related code -------- */

void
ccnet_peer_redirect_to (CcnetPeer *peer, CcnetPeer *to)
{
    if (peer->redirect_from) {
        ccnet_warning ("Peer can only redirect once\n");
        return;
    }

    if (to->redirect_from != NULL && to->redirect_from != peer) {
        ccnet_warning ("Another peer already redirect to it\n");
        return;
    }

    if (peer->redirect_to)
        ccnet_peer_unset_redirect (peer);

    peer->redirect_to = to;
    to->redirect_from = peer;
    g_object_ref (to);
    g_object_ref (peer);
}

void
ccnet_peer_unset_redirect (CcnetPeer *peer)
{
    g_assert (peer->redirect_to);

    g_object_unref (peer->redirect_to->redirect_from);
    peer->redirect_to->redirect_from = NULL;
    g_object_unref (peer->redirect_to);
    peer->redirect_to = NULL;
}
