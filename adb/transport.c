/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include "sysdeps.h"

#define   TRACE_TAG  TRACE_TRANSPORT
#include "adb.h"

static void transport_unref(atransport *t);

static atransport transport_list = {
    .next = &transport_list,
    .prev = &transport_list,
};

ADB_MUTEX_DEFINE( transport_lock );

#if ADB_TRACE
static void  dump_hex( const unsigned char*  ptr, size_t  len )
{
    int  nn, len2 = len;

    if (len2 > 16) len2 = 16;

    for (nn = 0; nn < len2; nn++)
        D("%02x", ptr[nn]);
    D("  ");

    for (nn = 0; nn < len2; nn++) {
        int  c = ptr[nn];
        if (c < 32 || c > 127)
            c = '.';
        D("%c", c);
    }
    D("\n");
    fflush(stdout);
}
#endif

void
kick_transport(atransport*  t)
{
    if (t && !t->kicked)
    {
        int  kicked;

        adb_mutex_lock(&transport_lock);
        kicked = t->kicked;
        if (!kicked)
            t->kicked = 1;
        adb_mutex_unlock(&transport_lock);

        if (!kicked)
            t->kick(t);
    }
}

void
run_transport_disconnects(atransport*  t)
{
    adisconnect*  dis = t->disconnects.next;

    D("run_transport_disconnects: %p (%s)\n", t, t->serial ? t->serial : "unknown" );
    while (dis != &t->disconnects) {
        adisconnect*  next = dis->next;
        dis->func( dis->opaque, t );
        dis = next;
    }
}

static int
read_packet(int  fd, apacket** ppacket)
{
    char *p = (char*)ppacket;  /* really read a packet address */
    int   r;
    int   len = sizeof(*ppacket);
    while(len > 0) {
        r = adb_read(fd, p, len);
        if(r > 0) {
            len -= r;
            p   += r;
        } else {
            D("read_packet: %d error %d %d\n", fd, r, errno);
            if((r < 0) && (errno == EINTR)) continue;
            return -1;
        }
    }

#if ADB_TRACE
    if (ADB_TRACING)
    {
        unsigned  command = (*ppacket)->msg.command;
        int       len     = (*ppacket)->msg.data_length;
        char      cmd[5];
        int       n;

        for (n = 0; n < 4; n++) {
            int  b = (command >> (n*8)) & 255;
            if (b >= 32 && b < 127)
                cmd[n] = (char)b;
            else
                cmd[n] = '.';
        }
        cmd[4] = 0;

        D("read_packet: %d ok: [%08x %s] %08x %08x (%d) ",
          fd, command, cmd, (*ppacket)->msg.arg0, (*ppacket)->msg.arg1, len);
        dump_hex((*ppacket)->data, len);
    }
#endif
    return 0;
}

static int
write_packet(int  fd, apacket** ppacket)
{
    char *p = (char*) ppacket;  /* we really write the packet address */
    int r, len = sizeof(ppacket);

#if ADB_TRACE
    if (ADB_TRACING)
    {
        unsigned  command = (*ppacket)->msg.command;
        int       len     = (*ppacket)->msg.data_length;
        char      cmd[5];
        int       n;

        for (n = 0; n < 4; n++) {
            int  b = (command >> (n*8)) & 255;
            if (b >= 32 && b < 127)
                cmd[n] = (char)b;
            else
                cmd[n] = '.';
        }
        cmd[4] = 0;

        D("write_packet: %d [%08x %s] %08x %08x (%d) ",
          fd, command, cmd, (*ppacket)->msg.arg0, (*ppacket)->msg.arg1, len);
        dump_hex((*ppacket)->data, len);
    }
#endif
    len = sizeof(ppacket);
    while(len > 0) {
        r = adb_write(fd, p, len);
        if(r > 0) {
            len -= r;
            p += r;
        } else {
            D("write_packet: %d error %d %d\n", fd, r, errno);
            if((r < 0) && (errno == EINTR)) continue;
            return -1;
        }
    }
    return 0;
}

static void transport_socket_events(int fd, unsigned events, void *_t)
{
    if(events & FDE_READ){
        apacket *p = 0;
        if(read_packet(fd, &p)){
            D("failed to read packet from transport socket on fd %d\n", fd);
        } else {
            handle_packet(p, (atransport *) _t);
        }
    }
}

void send_packet(apacket *p, atransport *t)
{
    unsigned char *x;
    unsigned sum;
    unsigned count;

    p->msg.magic = p->msg.command ^ 0xffffffff;

    count = p->msg.data_length;
    x = (unsigned char *) p->data;
    sum = 0;
    while(count-- > 0){
        sum += *x++;
    }
    p->msg.data_check = sum;

    print_packet("send", p);

    if (t == NULL) {
        fatal_errno("Transport is null");
        D("Transport is null \n");
    }

    if(write_packet(t->transport_socket, &p)){
        fatal_errno("cannot enqueue packet on transport socket");
    }
}

/* The transport is opened by transport_register_func before
** the input and output threads are started.
**
** The output thread issues a SYNC(1, token) message to let
** the input thread know to start things up.  In the event
** of transport IO failure, the output thread will post a
** SYNC(0,0) message to ensure shutdown.
**
** The transport will not actually be closed until both
** threads exit, but the input thread will kick the transport
** on its way out to disconnect the underlying device.
*/

static void *output_thread(void *_t)
{
    atransport *t = _t;
    apacket *p;

    D("from_remote: starting thread for transport %p, on fd %d\n", t, t->fd );

    D("from_remote: transport %p SYNC online (%d)\n", t, t->sync_token + 1);
    p = get_apacket();
    p->msg.command = A_SYNC;
    p->msg.arg0 = 1;
    p->msg.arg1 = ++(t->sync_token);
    p->msg.magic = A_SYNC ^ 0xffffffff;
    if(write_packet(t->fd, &p)) {
        put_apacket(p);
        D("from_remote: failed to write SYNC apacket to transport %p", t);
        goto oops;
    }

    D("from_remote: data pump  for transport %p\n", t);
    for(;;) {
        p = get_apacket();

        if(t->read_from_remote(p, t) == 0){
            D("from_remote: received remote packet, sending to transport %p\n",
              t);
            if(write_packet(t->fd, &p)){
                put_apacket(p);
                D("from_remote: failed to write apacket to transport %p", t);
                goto oops;
            }
        } else {
            D("from_remote: remote read failed for transport %p\n", p);
            put_apacket(p);
            break;
        }
    }

    D("from_remote: SYNC offline for transport %p\n", t);
    p = get_apacket();
    p->msg.command = A_SYNC;
    p->msg.arg0 = 0;
    p->msg.arg1 = 0;
    p->msg.magic = A_SYNC ^ 0xffffffff;
    if(write_packet(t->fd, &p)) {
        put_apacket(p);
        D("from_remote: failed to write SYNC apacket to transport %p", t);
    }

oops:
    D("from_remote: thread is exiting for transport %p\n", t);
    kick_transport(t);
    transport_unref(t);
    return 0;
}

static void *input_thread(void *_t)
{
    atransport *t = _t;
    apacket *p;
    int active = 0;

    D("to_remote: starting input_thread for %p, reading from fd %d\n",
       t, t->fd);

    for(;;){
        if(read_packet(t->fd, &p)) {
            D("to_remote: failed to read apacket from transport %p on fd %d\n", 
               t, t->fd );
            break;
        }
        if(p->msg.command == A_SYNC){
            if(p->msg.arg0 == 0) {
                D("to_remote: transport %p SYNC offline\n", t);
                put_apacket(p);
                break;
            } else {
                if(p->msg.arg1 == t->sync_token) {
                    D("to_remote: transport %p SYNC online\n", t);
                    active = 1;
                } else {
                    D("to_remote: trandport %p ignoring SYNC %d != %d\n",
                      t, p->msg.arg1, t->sync_token);
                }
            }
        } else {
            if(active) {
                D("to_remote: transport %p got packet, sending to remote\n", t);
                t->write_to_remote(p, t);
            } else {
                D("to_remote: transport %p ignoring packet while offline\n", t);
            }
        }

        put_apacket(p);
    }

    // this is necessary to avoid a race condition that occured when a transport closes
    // while a client socket is still active.
    close_all_sockets(t);

    D("to_remote: thread is exiting for transport %p, fd %d\n", t, t->fd);
    kick_transport(t);
    transport_unref(t);
    return 0;
}


static int transport_registration_send = -1;
static int transport_registration_recv = -1;
static fdevent transport_registration_fde;


#if ADB_HOST
static int list_transports_msg(char*  buffer, size_t  bufferlen)
{
    char  head[5];
    int   len;

    len = list_transports(buffer+4, bufferlen-4);
    snprintf(head, sizeof(head), "%04x", len);
    memcpy(buffer, head, 4);
    len += 4;
    return len;
}

/* this adds support required by the 'track-devices' service.
 * this is used to send the content of "list_transport" to any
 * number of client connections that want it through a single
 * live TCP connection
 */
typedef struct device_tracker  device_tracker;
struct device_tracker {
    asocket          socket;
    int              update_needed;
    device_tracker*  next;
};

/* linked list of all device trackers */
static device_tracker*   device_tracker_list;

static void
device_tracker_remove( device_tracker*  tracker )
{
    device_tracker**  pnode = &device_tracker_list;
    device_tracker*   node  = *pnode;

    adb_mutex_lock( &transport_lock );
    while (node) {
        if (node == tracker) {
            *pnode = node->next;
            break;
        }
        pnode = &node->next;
        node  = *pnode;
    }
    adb_mutex_unlock( &transport_lock );
}

static void
device_tracker_close( asocket*  socket )
{
    device_tracker*  tracker = (device_tracker*) socket;
    asocket*         peer    = socket->peer;

    D( "device tracker %p removed\n", tracker);
    if (peer) {
        peer->peer = NULL;
        peer->close(peer);
    }
    device_tracker_remove(tracker);
    free(tracker);
}

static int
device_tracker_enqueue( asocket*  socket, apacket*  p )
{
    /* you can't read from a device tracker, close immediately */
    put_apacket(p);
    device_tracker_close(socket);
    return -1;
}

static int
device_tracker_send( device_tracker*  tracker,
                     const char*      buffer,
                     int              len )
{
    apacket*  p = get_apacket();
    asocket*  peer = tracker->socket.peer;

    memcpy(p->data, buffer, len);
    p->len = len;
    return peer->enqueue( peer, p );
}


static void
device_tracker_ready( asocket*  socket )
{
    device_tracker*  tracker = (device_tracker*) socket;

    /* we want to send the device list when the tracker connects
    * for the first time, even if no update occured */
    if (tracker->update_needed > 0) {
        char  buffer[1024];
        int   len;

        tracker->update_needed = 0;

        len = list_transports_msg(buffer, sizeof(buffer));
        device_tracker_send(tracker, buffer, len);
    }
}


asocket*
create_device_tracker(void)
{
    device_tracker*  tracker = calloc(1,sizeof(*tracker));

    if(tracker == 0) fatal("cannot allocate device tracker");

    D( "device tracker %p created\n", tracker);

    tracker->socket.enqueue = device_tracker_enqueue;
    tracker->socket.ready   = device_tracker_ready;
    tracker->socket.close   = device_tracker_close;
    tracker->update_needed  = 1;

    tracker->next       = device_tracker_list;
    device_tracker_list = tracker;

    return &tracker->socket;
}


/* call this function each time the transport list has changed */
void  update_transports(void)
{
    char             buffer[1024];
    int              len;
    device_tracker*  tracker;

    len = list_transports_msg(buffer, sizeof(buffer));

    tracker = device_tracker_list;
    while (tracker != NULL) {
        device_tracker*  next = tracker->next;
        /* note: this may destroy the tracker if the connection is closed */
        device_tracker_send(tracker, buffer, len);
        tracker = next;
    }
}
#else
void  update_transports(void)
{
    // nothing to do on the device side
}
#endif // ADB_HOST

typedef struct tmsg tmsg;
struct tmsg
{
    atransport *transport;
    int         action;
};

static int
transport_read_action(int  fd, struct tmsg*  m)
{
    char *p   = (char*)m;
    int   len = sizeof(*m);
    int   r;

    while(len > 0) {
        r = adb_read(fd, p, len);
        if(r > 0) {
            len -= r;
            p   += r;
        } else {
            if((r < 0) && (errno == EINTR)) continue;
            D("transport_read_action: on fd %d, error %d: %s\n", 
              fd, errno, strerror(errno));
            return -1;
        }
    }
    return 0;
}

static int
transport_write_action(int  fd, struct tmsg*  m)
{
    char *p   = (char*)m;
    int   len = sizeof(*m);
    int   r;

    while(len > 0) {
        r = adb_write(fd, p, len);
        if(r > 0) {
            len -= r;
            p   += r;
        } else {
            if((r < 0) && (errno == EINTR)) continue;
            D("transport_write_action: on fd %d, error %d: %s\n", 
              fd, errno, strerror(errno));
            return -1;
        }
    }
    return 0;
}

static void transport_registration_func(int _fd, unsigned ev, void *data)
{
    tmsg m;
    adb_thread_t output_thread_ptr;
    adb_thread_t input_thread_ptr;
    int s[2];
    atransport *t;

    if(!(ev & FDE_READ)) {
        return;
    }

    if(transport_read_action(_fd, &m)) {
        fatal_errno("cannot read transport registration socket");
    }

    t = m.transport;

    if(m.action == 0){
        D("transport: %p removing and free'ing %d\n", t, t->transport_socket);

            /* IMPORTANT: the remove closes one half of the
            ** socket pair.  The close closes the other half.
            */
        fdevent_remove(&(t->transport_fde));
        adb_close(t->fd);

        adb_mutex_lock(&transport_lock);
        t->next->prev = t->prev;
        t->prev->next = t->next;
        adb_mutex_unlock(&transport_lock);

        run_transport_disconnects(t);

        if (t->product)
            free(t->product);
        if (t->serial)
            free(t->serial);

        memset(t,0xee,sizeof(atransport));
        free(t);

        update_transports();
        return;
    }

    /* don't create transport threads for inaccessible devices */
    if (t->connection_state != CS_NOPERM) {
        /* initial references are the two threads */
        t->ref_count = 2;

        if(adb_socketpair(s)) {
            fatal_errno("cannot open transport socketpair");
        }

        D("transport: %p (%d,%d) starting\n", t, s[0], s[1]);

        t->transport_socket = s[0];
        t->fd = s[1];

        D("transport: %p install %d\n", t, t->transport_socket );
        fdevent_install(&(t->transport_fde),
                        t->transport_socket,
                        transport_socket_events,
                        t);

        fdevent_set(&(t->transport_fde), FDE_READ);

        if(adb_thread_create(&input_thread_ptr, input_thread, t)){
            fatal_errno("cannot create input thread");
        }

        if(adb_thread_create(&output_thread_ptr, output_thread, t)){
            fatal_errno("cannot create output thread");
        }
    }

        /* put us on the master device list */
    adb_mutex_lock(&transport_lock);
    t->next = &transport_list;
    t->prev = transport_list.prev;
    t->next->prev = t;
    t->prev->next = t;
    adb_mutex_unlock(&transport_lock);

    t->disconnects.next = t->disconnects.prev = &t->disconnects;

    update_transports();
}

void init_transport_registration(void)
{
    int s[2];

    if(adb_socketpair(s)){
        fatal_errno("cannot open transport registration socketpair");
    }

    transport_registration_send = s[0];
    transport_registration_recv = s[1];

    fdevent_install(&transport_registration_fde,
                    transport_registration_recv,
                    transport_registration_func,
                    0);

    fdevent_set(&transport_registration_fde, FDE_READ);
}

/* the fdevent select pump is single threaded */
static void register_transport(atransport *transport)
{
    tmsg m;
    m.transport = transport;
    m.action = 1;
    D("transport: %p registered\n", transport);
    if(transport_write_action(transport_registration_send, &m)) {
        fatal_errno("cannot write transport registration socket\n");
    }
}

static void remove_transport(atransport *transport)
{
    tmsg m;
    m.transport = transport;
    m.action = 0;
    D("transport: %p removed\n", transport);
    if(transport_write_action(transport_registration_send, &m)) {
        fatal_errno("cannot write transport registration socket\n");
    }
}


static void transport_unref(atransport *t)
{
    if (t) {
        adb_mutex_lock(&transport_lock);
        t->ref_count--;
        D("transport: %p R- (ref=%d)\n", t, t->ref_count);
        if (t->ref_count == 0) {
            D("transport: %p kicking and closing\n", t);
            if (!t->kicked) {
                t->kicked = 1;
                t->kick(t);
            }
            t->close(t);
            remove_transport(t);
        }
        adb_mutex_unlock(&transport_lock);
    }
}

void add_transport_disconnect(atransport*  t, adisconnect*  dis)
{
    adb_mutex_lock(&transport_lock);
    dis->next       = &t->disconnects;
    dis->prev       = dis->next->prev;
    dis->prev->next = dis;
    dis->next->prev = dis;
    adb_mutex_unlock(&transport_lock);
}

void remove_transport_disconnect(atransport*  t, adisconnect*  dis)
{
    dis->prev->next = dis->next;
    dis->next->prev = dis->prev;
    dis->next = dis->prev = dis;
}


atransport *acquire_one_transport(int state, transport_type ttype, const char* serial, char** error_out)
{
    atransport *t;
    atransport *result = NULL;
    int ambiguous = 0;

retry:
    if (error_out)
        *error_out = "device not found";

    adb_mutex_lock(&transport_lock);
    for (t = transport_list.next; t != &transport_list; t = t->next) {
        if (t->connection_state == CS_NOPERM) {
        if (error_out)
            *error_out = "insufficient permissions for device";
            continue;
        }

        /* check for matching serial number */
        if (serial) {
            if (t->serial && !strcmp(serial, t->serial)) {
                result = t;
                break;
            }
        } else {
            if (ttype == kTransportUsb && t->type == kTransportUsb) {
                if (result) {
                    if (error_out)
                        *error_out = "more than one device";
                    ambiguous = 1;
                    result = NULL;
                    break;
                }
                result = t;
            } else if (ttype == kTransportLocal && t->type == kTransportLocal) {
                if (result) {
                    if (error_out)
                        *error_out = "more than one emulator";
                    ambiguous = 1;
                    result = NULL;
                    break;
                }
                result = t;
            } else if (ttype == kTransportAny) {
                if (result) {
                    if (error_out)
                        *error_out = "more than one device and emulator";
                    ambiguous = 1;
                    result = NULL;
                    break;
                }
                result = t;
            }
        }
    }
    adb_mutex_unlock(&transport_lock);

    if (result) {
         /* offline devices are ignored -- they are either being born or dying */
        if (result && result->connection_state == CS_OFFLINE) {
            if (error_out)
                *error_out = "device offline";
            result = NULL;
        }
         /* check for required connection state */
        if (result && state != CS_ANY && result->connection_state != state) {
            if (error_out)
                *error_out = "invalid device state";
            result = NULL;
        }
    }

    if (result) {
        /* found one that we can take */
        if (error_out)
            *error_out = NULL;
    } else if (state != CS_ANY && (serial || !ambiguous)) {
        adb_sleep_ms(1000);
        goto retry;
    }

    return result;
}

#if ADB_HOST
static const char *statename(atransport *t)
{
    switch(t->connection_state){
    case CS_OFFLINE: return "offline";
    case CS_BOOTLOADER: return "bootloader";
    case CS_DEVICE: return "device";
    case CS_HOST: return "host";
    case CS_RECOVERY: return "recovery";
    case CS_NOPERM: return "no permissions";
    default: return "unknown";
    }
}

int list_transports(char *buf, size_t  bufsize)
{
    char*       p   = buf;
    char*       end = buf + bufsize;
    int         len;
    atransport *t;

        /* XXX OVERRUN PROBLEMS XXX */
    adb_mutex_lock(&transport_lock);
    for(t = transport_list.next; t != &transport_list; t = t->next) {
        const char* serial = t->serial;
        if (!serial || !serial[0])
            serial = "????????????";
        len = snprintf(p, end - p, "%s\t%s\n", serial, statename(t));

        if (p + len >= end) {
            /* discard last line if buffer is too short */
            break;
        }
        p += len;
    }
    p[0] = 0;
    adb_mutex_unlock(&transport_lock);
    return p - buf;
}


/* hack for osx */
void close_usb_devices()
{
    atransport *t;

    adb_mutex_lock(&transport_lock);
    for(t = transport_list.next; t != &transport_list; t = t->next) {
        if ( !t->kicked ) {
            t->kicked = 1;
            t->kick(t);
        }
    }
    adb_mutex_unlock(&transport_lock);
}
#endif // ADB_HOST

void register_socket_transport(int s, const char *serial, int  port)
{
    atransport *t = calloc(1, sizeof(atransport));
    D("transport: %p init'ing for socket %d, on port %d\n", t, s, port);
    if ( init_socket_transport(t, s, port) < 0 ) {
        adb_close(s);
        free(t);
        return;
    }
    if(serial) {
        t->serial = strdup(serial);
    }
    register_transport(t);
}

void register_usb_transport(usb_handle *usb, const char *serial, unsigned writeable)
{
    atransport *t = calloc(1, sizeof(atransport));
    D("transport: %p init'ing for usb_handle %p (sn='%s')\n", t, usb,
      serial ? serial : "");
    init_usb_transport(t, usb, (writeable ? CS_OFFLINE : CS_NOPERM));
    if(serial) {
        t->serial = strdup(serial);
    }
    register_transport(t);
}

/* this should only be used for transports with connection_state == CS_NOPERM */
void unregister_usb_transport(usb_handle *usb)
{
    atransport *t;
    adb_mutex_lock(&transport_lock);
    for(t = transport_list.next; t != &transport_list; t = t->next) {
        if (t->usb == usb && t->connection_state == CS_NOPERM) {
            t->next->prev = t->prev;
            t->prev->next = t->next;
            break;
        }
     }
    adb_mutex_unlock(&transport_lock);
}

#undef TRACE_TAG
#define TRACE_TAG  TRACE_RWX

int readx(int fd, void *ptr, size_t len)
{
    char *p = ptr;
    int r;
#if ADB_TRACE
    int  len0 = len;
#endif
    D("readx: %d %p %d\n", fd, ptr, (int)len);
    while(len > 0) {
        r = adb_read(fd, p, len);
        if(r > 0) {
            len -= r;
            p += r;
        } else {
            D("readx: %d %d %s\n", fd, r, strerror(errno));
            if((r < 0) && (errno == EINTR)) continue;
            return -1;
        }
    }

#if ADB_TRACE
    D("readx: %d ok: ", fd);
    dump_hex( ptr, len0 );
#endif
    return 0;
}

int writex(int fd, const void *ptr, size_t len)
{
    char *p = (char*) ptr;
    int r;

#if ADB_TRACE
    D("writex: %d %p %d: ", fd, ptr, (int)len);
    dump_hex( ptr, len );
#endif
    while(len > 0) {
        r = adb_write(fd, p, len);
        if(r > 0) {
            len -= r;
            p += r;
        } else {
            D("writex: %d %d %s\n", fd, r, strerror(errno));
            if((r < 0) && (errno == EINTR)) continue;
            return -1;
        }
    }

    D("writex: %d ok\n", fd);
    return 0;
}

int check_header(apacket *p)
{
    if(p->msg.magic != (p->msg.command ^ 0xffffffff)) {
        D("check_header(): invalid magic\n");
        return -1;
    }

    if(p->msg.data_length > MAX_PAYLOAD) {
        D("check_header(): %d > MAX_PAYLOAD\n", p->msg.data_length);
        return -1;
    }

    return 0;
}

int check_data(apacket *p)
{
    unsigned count, sum;
    unsigned char *x;

    count = p->msg.data_length;
    x = p->data;
    sum = 0;
    while(count-- > 0) {
        sum += *x++;
    }

    if(sum != p->msg.data_check) {
        return -1;
    } else {
        return 0;
    }
}

