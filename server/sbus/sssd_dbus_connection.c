#include <sys/time.h>
#include "util/util.h"
#include "dbus/dbus.h"
#include "sbus/sssd_dbus.h"
#include "sbus/sssd_dbus_private.h"

/* Types */
struct dbus_ctx_list;

struct sbus_conn_ctx {
    struct tevent_context *ev;
    DBusConnection *dbus_conn;
    char *address;
    int connection_type;
    int disconnect;
    struct sbus_method_ctx *method_ctx_list;
    sbus_conn_destructor_fn destructor;
    void *pvt_data; /* Private data for this connection */

    int retries;
    int max_retries;
    sbus_conn_reconn_callback_fn reconnect_callback;
    /* Private data needed to reinit after reconnection */
    void *reconnect_pvt;
};

struct sbus_message_handler_ctx {
    struct sbus_conn_ctx *conn_ctx;
    struct sbus_method_ctx *method_ctx;
};

static int _method_list_contains_path(struct sbus_method_ctx *list,
                                      struct sbus_method_ctx *method);
static void sbus_unreg_object_paths(struct sbus_conn_ctx *conn_ctx);

static int sbus_auto_reconnect(struct sbus_conn_ctx *conn_ctx);

static void sbus_dispatch(struct tevent_context *ev,
                          struct tevent_timer *te,
                          struct timeval tv, void *data)
{
    struct tevent_timer *new_event;
    struct sbus_conn_ctx *conn_ctx;
    DBusConnection *dbus_conn;
    int ret;

    if (data == NULL) return;

    conn_ctx = talloc_get_type(data, struct sbus_conn_ctx);

    dbus_conn = conn_ctx->dbus_conn;
    DEBUG(6, ("dbus conn: %lX\n", dbus_conn));

    if (conn_ctx->retries > 0) {
        DEBUG(6, ("SBUS is reconnecting. Deferring.\n"));
        /* Currently trying to reconnect, defer dispatch */
        new_event = tevent_add_timer(ev, conn_ctx, tv, sbus_dispatch, conn_ctx);
        if (new_event == NULL) {
            DEBUG(0,("Could not defer dispatch!\n"));
        }
        return;
    }

    if ((!dbus_connection_get_is_connected(dbus_conn)) &&
        (conn_ctx->max_retries != 0)) {
        /* Attempt to reconnect automatically */
        ret = sbus_auto_reconnect(conn_ctx);
        if (ret == EOK) {
            DEBUG(1, ("Performing auto-reconnect\n"));
            return;
        }

        DEBUG(0, ("Cannot start auto-reconnection.\n"));
        conn_ctx->reconnect_callback(conn_ctx,
                                     SBUS_RECONNECT_ERROR,
                                     conn_ctx->reconnect_pvt);
        return;
    }

    if ((conn_ctx->disconnect) ||
        (!dbus_connection_get_is_connected(dbus_conn))) {
        DEBUG(3,("Connection is not open for dispatching.\n"));
        /*
         * Free the connection object.
         * This will invoke the destructor for the connection
         */
        talloc_free(conn_ctx);
        conn_ctx = NULL;
        return;
    }

    /* Dispatch only once each time through the mainloop to avoid
     * starving other features
     */
    ret = dbus_connection_get_dispatch_status(dbus_conn);
    if (ret != DBUS_DISPATCH_COMPLETE) {
        DEBUG(6,("Dispatching.\n"));
        dbus_connection_dispatch(dbus_conn);
    }

    /* If other dispatches are waiting, queue up the dispatch function
     * for the next loop.
     */
    ret = dbus_connection_get_dispatch_status(dbus_conn);
    if (ret != DBUS_DISPATCH_COMPLETE) {
        new_event = tevent_add_timer(ev, conn_ctx, tv, sbus_dispatch, conn_ctx);
        if (new_event == NULL) {
            DEBUG(2,("Could not add dispatch event!\n"));

            /* TODO: Calling exit here is bad */
            exit(1);
        }
    }
}

/* dbus_connection_wakeup_main
 * D-BUS makes a callback to the wakeup_main function when
 * it has data available for dispatching.
 * In order to avoid blocking, this function will create a now()
 * timed event to perform the dispatch during the next iteration
 * through the mainloop
 */
static void sbus_conn_wakeup_main(void *data)
{
    struct sbus_conn_ctx *conn_ctx;
    struct timeval tv;
    struct tevent_timer *te;

    conn_ctx = talloc_get_type(data, struct sbus_conn_ctx);
    gettimeofday(&tv, NULL);

    /* D-BUS calls this function when it is time to do a dispatch */
    te = tevent_add_timer(conn_ctx->ev, conn_ctx, tv, sbus_dispatch, conn_ctx);
    if (te == NULL) {
        DEBUG(2,("Could not add dispatch event!\n"));
        /* TODO: Calling exit here is bad */
        exit(1);
    }
}

static int sbus_add_connection_int(struct sbus_conn_ctx **conn_ctx);

/*
 * integrate_connection_with_event_loop
 * Set up a D-BUS connection to use the libevents mainloop
 * for handling file descriptor and timed events
 */
int sbus_add_connection(TALLOC_CTX *ctx,
                        struct tevent_context *ev,
                        DBusConnection *dbus_conn,
                        struct sbus_conn_ctx **_conn_ctx,
                        int connection_type)
{
    struct sbus_conn_ctx *conn_ctx;
    int ret;

    DEBUG(5,("Adding connection %lX\n", dbus_conn));
    conn_ctx = talloc_zero(ctx, struct sbus_conn_ctx);

    conn_ctx->ev = ev;
    conn_ctx->dbus_conn = dbus_conn;
    conn_ctx->connection_type = connection_type;
    conn_ctx->disconnect = 0;

    /* This will be replaced on the first call to sbus_conn_add_method_ctx() */
    conn_ctx->method_ctx_list = NULL;

    /* This can be overridden by a call to sbus_reconnect_init() */
    conn_ctx->retries = 0;
    conn_ctx->max_retries = 0;
    conn_ctx->reconnect_callback = NULL;

    *_conn_ctx = conn_ctx;

    ret = sbus_add_connection_int(_conn_ctx);
    if (ret != EOK) {
        talloc_free(conn_ctx);
    }
    return ret;
}

static int sbus_add_connection_int(struct sbus_conn_ctx **_conn_ctx)
{
    struct sbus_conn_ctx *conn_ctx = *_conn_ctx;
    struct sbus_generic_dbus_ctx *gen_ctx;
    dbus_bool_t dbret;

    /*
     * Set the default destructor
     * Connections can override this with
     * sbus_conn_set_destructor
     */
    sbus_conn_set_destructor(conn_ctx, NULL);

    gen_ctx = talloc_zero(conn_ctx, struct sbus_generic_dbus_ctx);
    if (!gen_ctx) {
        DEBUG(0, ("Out of memory!\n"));
        return ENOMEM;
    }
    gen_ctx->ev = conn_ctx->ev;
    gen_ctx->type = SBUS_CONNECTION;
    gen_ctx->dbus.conn = conn_ctx->dbus_conn;

    /* Set up DBusWatch functions */
    dbret = dbus_connection_set_watch_functions(conn_ctx->dbus_conn,
                                                sbus_add_watch,
                                                sbus_remove_watch,
                                                sbus_toggle_watch,
                                                gen_ctx, NULL);
    if (!dbret) {
        DEBUG(2,("Error setting up D-BUS connection watch functions\n"));
        return EIO;
    }

    /* Set up DBusTimeout functions */
    dbret = dbus_connection_set_timeout_functions(conn_ctx->dbus_conn,
                                                  sbus_add_timeout,
                                                  sbus_remove_timeout,
                                                  sbus_toggle_timeout,
                                                  gen_ctx, NULL);
    if (!dbret) {
        DEBUG(2,("Error setting up D-BUS server timeout functions\n"));
        /* FIXME: free resources ? */
        return EIO;
    }

    /* Set up dispatch handler */
    dbus_connection_set_wakeup_main_function(conn_ctx->dbus_conn,
                                             sbus_conn_wakeup_main,
                                             conn_ctx, NULL);

    /* Set up any method_contexts passed in */

    /* Attempt to dispatch immediately in case of opportunistic
     * services connecting before the handlers were all up.
     * If there are no messages to be dispatched, this will do
     * nothing.
     */
    sbus_conn_wakeup_main(conn_ctx);

    /* Return the new toplevel object */
    *_conn_ctx = conn_ctx;

    return EOK;
}

/*int sbus_new_connection(struct sbus_method_ctx *ctx, const char *address,
                             DBusConnection **connection,
                             sbus_conn_destructor_fn destructor)*/
int sbus_new_connection(TALLOC_CTX *ctx, struct tevent_context *ev,
                        const char *address,
                        struct sbus_conn_ctx **conn_ctx,
                        sbus_conn_destructor_fn destructor)
{
    DBusConnection *dbus_conn;
    DBusError dbus_error;
    int ret;

    dbus_error_init(&dbus_error);

    /* Open a shared D-BUS connection to the address */
    dbus_conn = dbus_connection_open(address, &dbus_error);
    if (!dbus_conn) {
        DEBUG(1, ("Failed to open connection: name=%s, message=%s\n",
                dbus_error.name, dbus_error.message));
        if (dbus_error_is_set(&dbus_error)) dbus_error_free(&dbus_error);
        return EIO;
    }

    ret = sbus_add_connection(ctx, ev, dbus_conn,
                              conn_ctx, SBUS_CONN_TYPE_SHARED);
    if (ret != EOK) {
        /* FIXME: release resources */
    }

    /* Store the address for later reconnection */
    (*conn_ctx)->address = talloc_strdup((*conn_ctx), address);

    dbus_connection_set_exit_on_disconnect((*conn_ctx)->dbus_conn, FALSE);

    /* Set connection destructor */
    sbus_conn_set_destructor(*conn_ctx, destructor);

    return ret;
}

/*
 * sbus_conn_set_destructor
 * Configures a callback to clean up this connection when it
 * is finalized.
 * @param conn_ctx The sbus_conn_ctx created
 * when this connection was established
 * @param destructor The destructor function that should be
 * called when the connection is finalized. If passed NULL,
 * this will reset the connection to the default destructor.
 */
void sbus_conn_set_destructor(struct sbus_conn_ctx *conn_ctx,
                              sbus_conn_destructor_fn destructor)
{
    if (!conn_ctx) return;

    conn_ctx->destructor = destructor;
    /* TODO: Should we try to handle the talloc_destructor too? */
}

int sbus_default_connection_destructor(void *ctx)
{
    struct sbus_conn_ctx *conn_ctx;
    conn_ctx = talloc_get_type(ctx, struct sbus_conn_ctx);

    DEBUG(5, ("Invoking default destructor on connection %lX\n",
              conn_ctx->dbus_conn));
    if (conn_ctx->connection_type == SBUS_CONN_TYPE_PRIVATE) {
        /* Private connections must be closed explicitly */
        dbus_connection_close(conn_ctx->dbus_conn);
    }
    else if (conn_ctx->connection_type == SBUS_CONN_TYPE_SHARED) {
        /* Shared connections are destroyed when their last reference is removed */
    }
    else {
        /* Critical Error! */
        DEBUG(1,("Critical Error, connection_type is neither shared nor private!\n"));
        return -1;
    }

    /* Remove object path */
    /* TODO: Remove object paths */

    dbus_connection_unref(conn_ctx->dbus_conn);
    return 0;
}

/*
 * sbus_get_connection
 * Utility function to retreive the DBusConnection object
 * from a sbus_conn_ctx
 */
DBusConnection *sbus_get_connection(struct sbus_conn_ctx *conn_ctx)
{
    return conn_ctx->dbus_conn;
}

void sbus_disconnect (struct sbus_conn_ctx *conn_ctx)
{
    if (conn_ctx == NULL) {
        return;
    }

    DEBUG(5,("Disconnecting %lX\n", conn_ctx->dbus_conn));

    /*******************************
     *  Referencing conn_ctx->dbus_conn */
    dbus_connection_ref(conn_ctx->dbus_conn);

    conn_ctx->disconnect = 1;

    /* Invoke the custom destructor, if it exists */
    if (conn_ctx->destructor) {
        conn_ctx->destructor(conn_ctx);
    }

    /* Unregister object paths */
    sbus_unreg_object_paths(conn_ctx);

    /* Disable watch functions */
    dbus_connection_set_watch_functions(conn_ctx->dbus_conn,
                                        NULL, NULL, NULL,
                                        NULL, NULL);
    /* Disable timeout functions */
    dbus_connection_set_timeout_functions(conn_ctx->dbus_conn,
                                          NULL, NULL, NULL,
                                          NULL, NULL);

    /* Disable dispatch status function */
    dbus_connection_set_dispatch_status_function(conn_ctx->dbus_conn,
                                                 NULL, NULL, NULL);

    /* Disable wakeup main function */
    dbus_connection_set_wakeup_main_function(conn_ctx->dbus_conn,
                                             NULL, NULL, NULL);

    /* Finalize the connection */
    sbus_default_connection_destructor(conn_ctx);

    dbus_connection_unref(conn_ctx->dbus_conn);
    /* Unreferenced conn_ctx->dbus_conn *
     ******************************/

    DEBUG(5,("Disconnected %lX\n", conn_ctx->dbus_conn));
}

static int sbus_reply_internal_error(DBusMessage *message,
                                     struct sbus_conn_ctx *conn_ctx) {
    DBusMessage *reply = dbus_message_new_error(message, DBUS_ERROR_IO_ERROR,
                                                "Internal Error");
    if (reply) {
        sbus_conn_send_reply(conn_ctx, reply);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* messsage_handler
 * Receive messages and process them
 */
DBusHandlerResult sbus_message_handler(DBusConnection *conn,
                                         DBusMessage *message,
                                         void *user_data)
{
    struct sbus_message_handler_ctx *ctx;
    const char *method;
    const char *path;
    const char *msg_interface;
    DBusMessage *reply = NULL;
    int i, ret;
    int found;

    if (!user_data) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    ctx = talloc_get_type(user_data, struct sbus_message_handler_ctx);

    method = dbus_message_get_member(message);
    DEBUG(9, ("Received SBUS method [%s]\n", method));
    path = dbus_message_get_path(message);
    msg_interface = dbus_message_get_interface(message);

    if (!method || !path || !msg_interface)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    /* Validate the D-BUS path */
    if (strcmp(path, ctx->method_ctx->path) != 0)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    /* Validate the method interface */
    if (strcmp(msg_interface, ctx->method_ctx->interface) == 0) {
        found = 0;
        for (i = 0; ctx->method_ctx->methods[i].method != NULL; i++) {
            if (strcmp(method, ctx->method_ctx->methods[i].method) == 0) {
                found = 1;
                ret = ctx->method_ctx->methods[i].fn(message, ctx->conn_ctx);
                if (ret != EOK) return sbus_reply_internal_error(message,
                                                                 ctx->conn_ctx);
                break;
            }
        }

        if (!found) {
            /* Reply DBUS_ERROR_UNKNOWN_METHOD */
            DEBUG(1, ("No matching method found for %s.\n", method));
            reply = dbus_message_new_error(message, DBUS_ERROR_UNKNOWN_METHOD, NULL);
            sbus_conn_send_reply(ctx->conn_ctx, reply);
            dbus_message_unref(reply);
        }
    }
    else {
        /* Special case: check for Introspection request
         * This is usually only useful for system bus connections
         */
        if (strcmp(msg_interface, DBUS_INTROSPECT_INTERFACE) == 0 &&
            strcmp(method, DBUS_INTROSPECT_METHOD) == 0)
        {
            if (ctx->method_ctx->introspect_fn) {
                /* If we have been asked for introspection data and we have
                 * an introspection function registered, user that.
                 */
                ret = ctx->method_ctx->introspect_fn(message, ctx->conn_ctx);
                if (ret != EOK) return sbus_reply_internal_error(message,
                                                                 ctx->conn_ctx);
            }
        }
        else
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    return DBUS_HANDLER_RESULT_HANDLED;
}

/* Adds a new D-BUS path message handler to the connection
 * Note: this must be a unique path.
 */
int sbus_conn_add_method_ctx(struct sbus_conn_ctx *conn_ctx,
                             struct sbus_method_ctx *method_ctx)
{
    DBusObjectPathVTable *connection_vtable;
    struct sbus_message_handler_ctx *msg_handler_ctx;
    TALLOC_CTX *tmp_ctx;

    dbus_bool_t dbret;
    if (!conn_ctx || !method_ctx) {
        return EINVAL;
    }

    if (_method_list_contains_path(conn_ctx->method_ctx_list, method_ctx)) {
        DEBUG(0, ("Cannot add method context with identical path.\n"));
        return EINVAL;
    }

    if (method_ctx->message_handler == NULL) {
        return EINVAL;
    }

    DLIST_ADD(conn_ctx->method_ctx_list, method_ctx);
    tmp_ctx = talloc_reference(conn_ctx, method_ctx);
    if (tmp_ctx != method_ctx) {
        /* talloc_reference only fails on insufficient memory */
        return ENOMEM;
    }

    /* Set up the vtable for the object path */
    connection_vtable = talloc_zero(conn_ctx, DBusObjectPathVTable);
    if (!connection_vtable) {
        return ENOMEM;
    }
    connection_vtable->message_function = method_ctx->message_handler;

    msg_handler_ctx = talloc_zero(conn_ctx, struct sbus_message_handler_ctx);
    if (!msg_handler_ctx) {
        talloc_free(connection_vtable);
        return ENOMEM;
    }
    msg_handler_ctx->conn_ctx = conn_ctx;
    msg_handler_ctx->method_ctx = method_ctx;

    dbret = dbus_connection_register_object_path(conn_ctx->dbus_conn,
                                                 method_ctx->path,
                                                 connection_vtable,
                                                 msg_handler_ctx);
    if (!dbret) {
        DEBUG(0, ("Could not register object path to the connection.\n"));
        return ENOMEM;
    }

    return EOK;
}

static int _method_list_contains_path(struct sbus_method_ctx *list,
                                      struct sbus_method_ctx *method)
{
    struct sbus_method_ctx *iter;

    if (!list || !method) {
        return 0; /* FALSE */
    }

    iter = list;
    while (iter != NULL) {
        if (strcmp(iter->path, method->path) == 0)
            return 1; /* TRUE */

        iter = iter->next;
    }

    return 0; /* FALSE */
}

static void sbus_unreg_object_paths(struct sbus_conn_ctx *conn_ctx)
{
    struct sbus_method_ctx *iter = conn_ctx->method_ctx_list;
    struct sbus_method_ctx *purge;

    while (iter != NULL) {
        dbus_connection_unregister_object_path(conn_ctx->dbus_conn,
                                               iter->path);
        DLIST_REMOVE(conn_ctx->method_ctx_list, iter);
        purge = iter;
        iter = iter->next;
        talloc_unlink(conn_ctx, purge);
    }
}

void sbus_conn_set_private_data(struct sbus_conn_ctx *conn_ctx, void *pvt_data)
{
    conn_ctx->pvt_data = pvt_data;
}

void *sbus_conn_get_private_data(struct sbus_conn_ctx *conn_ctx)
{
    return conn_ctx->pvt_data;
}

static void sbus_reconnect(struct tevent_context *ev,
                           struct tevent_timer *te,
                           struct timeval tv, void *data)
{
    struct sbus_conn_ctx *conn_ctx = talloc_get_type(data, struct sbus_conn_ctx);
    DBusConnection *dbus_conn;
    DBusError dbus_error;
    struct tevent_timer *event;
    struct sbus_method_ctx *iter;
    struct sbus_method_ctx *purge;
    int ret;

    DEBUG(3, ("Making reconnection attempt %d to [%s]\n",
              conn_ctx->retries, conn_ctx->address));
    /* Make a new connection to the D-BUS address */
    dbus_error_init(&dbus_error);
    dbus_conn = dbus_connection_open(conn_ctx->address, &dbus_error);
    if (dbus_conn) {
        /* We successfully reconnected. Set up mainloop
         * integration.
         */
        DEBUG(3, ("Reconnected to [%s]\n", conn_ctx->address));
        conn_ctx->dbus_conn = dbus_conn;
        ret = sbus_add_connection_int(&conn_ctx);
        if (ret != EOK) {
            dbus_connection_unref(dbus_conn);
            goto failed;
        }

        /* Remove object paths (the reconnection callback must re-add these */
        iter = conn_ctx->method_ctx_list;
        while (iter != NULL) {
            DLIST_REMOVE(conn_ctx->method_ctx_list, iter);
            purge = iter;
            iter = iter->next;
            talloc_unlink(conn_ctx, purge);
        }

        /* Reset retries to 0 to resume dispatch processing */
        conn_ctx->retries = 0;

        /* Notify the owner of this connection that the
         * reconnection was successful
         */
        conn_ctx->reconnect_callback(conn_ctx,
                                     SBUS_RECONNECT_SUCCESS,
                                     conn_ctx->reconnect_pvt);
        return;
    }

failed:
    /* Reconnection failed, try again in a few seconds */
    DEBUG(1, ("Failed to open connection: name=%s, message=%s\n",
                dbus_error.name, dbus_error.message));
    if (dbus_error_is_set(&dbus_error)) dbus_error_free(&dbus_error);

    conn_ctx->retries++;

    /* Check if we've passed our last chance or if we've lost track of
     * our retry count somehow
     */
    if (((conn_ctx->max_retries > 0) &&
         (conn_ctx->retries > conn_ctx->max_retries)) ||
        conn_ctx->retries <= 0) {
        conn_ctx->reconnect_callback(conn_ctx,
                                     SBUS_RECONNECT_EXCEEDED_RETRIES,
                                     conn_ctx->reconnect_pvt);
    }

    if (conn_ctx->retries == 2) {
        tv.tv_sec += 3; /* Wait 3 seconds before the second reconnect attempt */
    }
    else if (conn_ctx->retries == 3) {
        tv.tv_sec += 10; /* Wait 10 seconds before the third reconnect attempt */
    }
    else {
        tv.tv_sec += 30; /* Wait 30 seconds before all subsequent reconnect attempts */
    }
    event = tevent_add_timer(conn_ctx->ev, conn_ctx, tv, sbus_reconnect, conn_ctx);
    if (event == NULL) {
        conn_ctx->reconnect_callback(conn_ctx,
                                     SBUS_RECONNECT_ERROR,
                                     conn_ctx->reconnect_pvt);
    }
}

/* This function will free and recreate the sbus_conn_ctx,
 * calling functions need to be aware of this (and whether
 * they have attached a talloc destructor to the
 * sbus_conn_ctx.
 */
static int sbus_auto_reconnect(struct sbus_conn_ctx *conn_ctx)
{
    struct tevent_timer *te = NULL;
    struct timeval tv;

    conn_ctx->retries++;
    if ((conn_ctx->max_retries > 0) &&
        (conn_ctx->retries >= conn_ctx->max_retries)) {
        /* Return EAGAIN (to tell the calling process it
         * needs to create a new connection from scratch
         */
        return EAGAIN;
    }

    gettimeofday(&tv, NULL);
    tv.tv_sec += 1; /* Wait 1 second before the first reconnect attempt */
    te = tevent_add_timer(conn_ctx->ev, conn_ctx, tv, sbus_reconnect, conn_ctx);
    if (te == NULL) return EAGAIN;

    return EOK;
}

/* Max retries */
void sbus_reconnect_init(struct sbus_conn_ctx *conn_ctx,
                         int max_retries,
                         sbus_conn_reconn_callback_fn callback,
                         void *pvt)
{
    if(max_retries == 0 || callback == NULL) return;

    conn_ctx->retries = 0;
    conn_ctx->max_retries = max_retries;
    conn_ctx->reconnect_callback = callback;
    conn_ctx->reconnect_pvt = pvt;
}

bool sbus_conn_disconnecting(struct sbus_conn_ctx *conn_ctx)
{
    if (conn_ctx->disconnect == 1) return true;
    return false;
}

void sbus_conn_send_reply(struct sbus_conn_ctx *conn_ctx, DBusMessage *reply)
{
    dbus_connection_send(conn_ctx->dbus_conn, reply, NULL);
}

