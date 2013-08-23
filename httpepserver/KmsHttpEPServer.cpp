/*
 * Copyright (C) 2013 Kurento
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <libsoup/soup.h>
#include <uuid/uuid.h>

#include "KmsHttpEPServer.h"

#define OBJECT_NAME "HttpEPServer"

/* 36-byte string (plus tailing '\0') */
#define UUID_STR_SIZE 37

#define HTTP_EP_SERVER_ROOT_PATH "/"

#define KEY_HTTP_EP_SERVER "kms-http-ep-server"
#define KEY_HTTP_EP "kms-http-ep"
#define KEY_NEW_SAMPLE "kms-new-sample-signal"
#define KEY_EOS "kms-eos-signal"
#define KEY_FINISHED "kms-finish-signal"

#define GST_CAT_DEFAULT kms_http_ep_server_debug_category
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

#define KMS_HTTP_EP_SERVER_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE ((obj), KMS_TYPE_HTTP_EP_SERVER, KmsHttpEPServerPrivate))
struct _KmsHttpEPServerPrivate {
  SoupServer *server;
  GSList *handlers;
  gchar *iface;
  gint port;
};

/* class initialization */

G_DEFINE_TYPE_WITH_CODE (KmsHttpEPServer, kms_http_ep_server,
    G_TYPE_OBJECT,
    GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, OBJECT_NAME,
        0, "debug category for " OBJECT_NAME " element") )

/* properties */
enum {
  PROP_0,

  PROP_KMS_HTTP_EP_SERVER_PORT,
  PROP_KMS_HTTP_EP_SERVER_INTERFACE,

  N_PROPERTIES
};

#define KMS_HTTP_EP_SERVER_DEFAULT_PORT 0
#define KMS_HTTP_EP_SERVER_DEFAULT_INTERFACE NULL

static GParamSpec *obj_properties[N_PROPERTIES] = { NULL, };

/* signals */
enum {
  URL_REMOVED,
  LAST_SIGNAL
};

static guint obj_signals[LAST_SIGNAL] = { 0 };

struct handler_data {
  gpointer data;
  GDestroyNotify destroy;
  KmsHttpEPServer *server;
};

struct resolv_data {
  KmsHttpEPServerStartCallback cb;
  KmsHttpEPServer *server;
};

static void
kms_http_ep_server_remove_handler (const gchar *url, KmsHttpEPServer *self)
{
  GST_DEBUG ("Remove url: %s", url);
  soup_server_remove_handler (self->priv->server, url);

  g_signal_emit (G_OBJECT (self), obj_signals[URL_REMOVED], 0, url);
}

struct sample_data {
  KmsHttpEPServer *httpepserver;
  GstSample *sample;
  SoupMessage *msg;
};

static gboolean
msg_has_finished (SoupMessage *msg)
{
  gboolean *finished;

  finished = (gboolean *) g_object_get_data (G_OBJECT (msg), KEY_FINISHED);

  return *finished;
}

static gboolean
send_buffer_cb (gpointer data)
{
  struct sample_data *sdata = (struct sample_data *) data;
  GstBuffer *buffer;
  GstMapInfo info;

  if (msg_has_finished (sdata->msg) ) {
    GST_WARNING ("Client has closed underlaying HTTP connection. "
        "Buffer won't be sent");
    return FALSE;
  }

  buffer = gst_sample_get_buffer (sdata->sample);

  if (buffer == NULL)
    return FALSE;

  if (!gst_buffer_map (buffer, &info, GST_MAP_READ) ) {
    GST_WARNING ("Could not get buffer map");
    return FALSE;
  }

  soup_message_body_append (sdata->msg->response_body, SOUP_MEMORY_COPY,
      info.data, info.size);
  soup_server_unpause_message (sdata->httpepserver->priv->server, sdata->msg);

  gst_buffer_unmap (buffer, &info);
  return FALSE;
}

static void
destroy_sample_data (gpointer data)
{
  struct sample_data *sdata = (struct sample_data *) data;

  if (sdata->sample != NULL)
    gst_sample_unref (sdata->sample);

  if (sdata->httpepserver != NULL)
    g_object_unref (sdata->httpepserver);

  if (sdata->msg != NULL)
    g_object_unref (sdata->msg);

  g_slice_free (struct sample_data, sdata);
}

static GstFlowReturn
new_sample_handler (GstElement *httpep, gpointer data)
{
  SoupMessage *msg = (SoupMessage *) data;
  GstSample *sample = NULL;
  struct sample_data *sdata;

  GST_DEBUG ("New-sample");

  g_signal_emit_by_name (httpep, "pull-sample", &sample);

  if (sample == NULL)
    return GST_FLOW_ERROR;

  sdata = g_slice_new (struct sample_data);
  sdata->sample = gst_sample_ref (sample);
  sdata->msg = (SoupMessage *) g_object_ref (G_OBJECT (msg) );
  sdata->httpepserver = KMS_HTTP_EP_SERVER (g_object_ref (
      g_object_get_data (G_OBJECT (msg), KEY_HTTP_EP_SERVER) ) );

  /* Write buffer in the main context thread */
  g_idle_add_full (G_PRIORITY_HIGH_IDLE, send_buffer_cb, sdata,
      destroy_sample_data);

  gst_sample_unref (sample);
  return GST_FLOW_OK;
}

static void
get_recv_eos (GstElement *httep, gpointer data)
{
  SoupMessage *msg = (SoupMessage *) data;

  GST_DEBUG ("EOS received on HttpEndPoint %s", GST_ELEMENT_NAME (httep) );
  soup_message_body_complete (msg->response_body);
}

static void
finished_processing (SoupMessage *msg, gpointer data)
{
  GstElement *httpep = GST_ELEMENT (g_object_get_data (G_OBJECT (msg),
      KEY_HTTP_EP) );
  gboolean *finished;
  gulong *h1, *h2;

  GST_DEBUG ("MSG finished");
  finished = (gboolean *) g_object_get_data (G_OBJECT (msg), KEY_FINISHED);
  *finished = TRUE;

  /* Disconnect signals */
  h1 = (gulong *) g_object_get_data (G_OBJECT (msg), KEY_NEW_SAMPLE);
  h2 = (gulong *) g_object_get_data (G_OBJECT (msg), KEY_EOS);
  g_signal_handler_disconnect (httpep, *h1);
  g_signal_handler_disconnect (httpep, *h2);
}

static void
destroy_gboolean (gboolean *finished)
{
  g_slice_free (gboolean, finished);
}

static void
destroy_ulong (gulong *handlerid)
{
  g_slice_free (gulong, handlerid);
}

static void
kms_http_ep_server_get_handler (KmsHttpEPServer *self, SoupMessage *msg,
    GstElement *httpep)
{
  gboolean *finished;
  gulong *handlerid;

  /* TODO: Check wether we support client's capabilities before sending */
  /* back a response code 200 OK. Furthermore, we only provide support  */
  /* for webm in content type response */
  soup_message_set_status (msg, SOUP_STATUS_OK);
  soup_message_headers_set_content_type (msg->response_headers, "video/webm",
      NULL);
  soup_message_headers_set_encoding (msg->response_headers,
      SOUP_ENCODING_CHUNKED);

  finished = g_slice_new (gboolean);
  *finished = FALSE;

  g_object_set_data_full (G_OBJECT (msg), KEY_HTTP_EP_SERVER,
      g_object_ref (self), g_object_unref);
  g_object_set_data_full (G_OBJECT (msg), KEY_HTTP_EP,
      g_object_ref (httpep), g_object_unref);
  g_object_set_data_full (G_OBJECT (msg), KEY_FINISHED, finished,
      (GDestroyNotify) destroy_gboolean);

  g_signal_connect (G_OBJECT (msg), "finished",
      G_CALLBACK (finished_processing), NULL);

  handlerid = g_slice_new (gulong);
  *handlerid = g_signal_connect (httpep, "new-sample",
      G_CALLBACK (new_sample_handler), msg);
  g_object_set_data_full (G_OBJECT (msg), KEY_NEW_SAMPLE, handlerid,
      (GDestroyNotify) destroy_ulong);

  handlerid = g_slice_new (gulong);
  *handlerid = g_signal_connect (httpep, "eos", G_CALLBACK (get_recv_eos), msg);
  g_object_set_data_full (G_OBJECT (msg), KEY_EOS, handlerid,
      (GDestroyNotify) destroy_ulong);

  /* allow media stream to flow in HttpEndPoint pipeline */
  g_object_set (G_OBJECT (httpep), "start", TRUE, NULL);
}

static void
kms_http_ep_server_req_handler (SoupServer *server, SoupMessage *msg,
    const char *path, GHashTable *query, SoupClientContext *client,
    gpointer user_data)
{
  struct handler_data *hdata = (struct handler_data *) user_data;
  GSList *l;

  if (msg->method == SOUP_METHOD_GET) {
    kms_http_ep_server_get_handler (hdata->server, msg,
        GST_ELEMENT (hdata->data) );
  } else if (msg->method == SOUP_METHOD_POST) {
    /* TODO: Set HttpEndPoint to work in POST mode */
    GST_DEBUG ("Configure to work to attend POST requests");
    soup_message_set_status_full (msg, SOUP_STATUS_NOT_IMPLEMENTED,
        "Not implemented");
  } else {
    GST_WARNING ("HTTP operation %s is not allowed", msg->method);
    soup_message_set_status_full (msg, SOUP_STATUS_METHOD_NOT_ALLOWED,
        "Not allowed");
  }

  /* Remove handler */
  l = g_slist_find_custom (hdata->server->priv->handlers, path,
      (GCompareFunc) g_strcmp0);
  hdata->server->priv->handlers = g_slist_remove (hdata->server->priv->handlers,
      l->data);

  kms_http_ep_server_remove_handler (path, hdata->server);
}

static void
kms_http_ep_server_stop_impl (KmsHttpEPServer *self)
{
  if (self->priv->server == NULL) {
    GST_WARNING ("Server is not started");
    return;
  }

  /* Remove handlers */
  g_slist_foreach (self->priv->handlers,
      (GFunc) kms_http_ep_server_remove_handler, self);

  /* Stops processing for server */
  soup_server_quit (self->priv->server);
}

static void
destroy_handler_data (struct handler_data *hdata)
{
  if (hdata->destroy != NULL) {
    GST_DEBUG ("Destroying handler data: %P", hdata->data);
    hdata->destroy (hdata->data);
  }

  g_object_unref (hdata->server);
  g_slice_free (struct handler_data, hdata);
}

static gboolean
kms_http_ep_server_register_handler (KmsHttpEPServer *self, gchar *url,
    gpointer data, GDestroyNotify destroy)
{
  struct handler_data *hdata;
  GSList *l;

  l = g_slist_find_custom (self->priv->handlers, url, (GCompareFunc) g_strcmp0);

  if (l != NULL) {
    GST_ERROR ("URL %s already registered.", url);
    return FALSE;
  }

  self->priv->handlers = g_slist_prepend (self->priv->handlers, url);

  hdata = g_slice_new (struct handler_data);
  hdata->data = data;
  hdata->destroy = destroy;
  hdata->server = KMS_HTTP_EP_SERVER (g_object_ref (self) );

  soup_server_add_handler (self->priv->server, url,
      kms_http_ep_server_req_handler, hdata,
      (GDestroyNotify) destroy_handler_data);

  return TRUE;
}

static void
got_chunk_handler (SoupMessage *msg, SoupBuffer *chunk, gpointer data)
{
  GST_INFO ("Chunk callback %s", msg->method);
}

static void
got_headers_handler (SoupMessage *msg, gpointer data)
{
  KmsHttpEPServer *self = KMS_HTTP_EP_SERVER (data);
  SoupURI *uri = soup_message_get_uri (msg);
  const char *path = soup_uri_get_path (uri);
  GSList *l;

  if (g_strcmp0 (path, HTTP_EP_SERVER_ROOT_PATH) == 0) {
    soup_message_set_status_full (msg, SOUP_STATUS_NOT_IMPLEMENTED,
        "Not implemented");
    return;
  }

  l = g_slist_find_custom (self->priv->handlers, path,
      (GCompareFunc) g_strcmp0);

  if (l == NULL) {
    /* URL is not registered */
    soup_message_set_status_full (msg, SOUP_STATUS_NOT_FOUND,
        "Http end point not found");
    return;
  }

  /* Request allowed */

  if (msg->method == SOUP_METHOD_POST) {
    /* Get chunks without filling in body's data field after */
    /* the body is fully sent/received */
    soup_message_body_set_accumulate (msg->request_body, FALSE);
    g_signal_connect (msg, "got-chunk", G_CALLBACK (got_chunk_handler), data);
  }
}

static void
request_started_handler (SoupServer *server, SoupMessage *msg,
    SoupClientContext *client, gpointer data)
{
  g_signal_connect (msg, "got-headers", G_CALLBACK (got_headers_handler), data);
}

static void
kms_http_ep_server_create_server (KmsHttpEPServer *self, SoupAddress *addr)
{
  SoupSocket *listener;
  gchar *root_path;

  self->priv->server = soup_server_new (SOUP_SERVER_PORT, self->priv->port,
      SOUP_SERVER_INTERFACE, addr, NULL);

  root_path = g_strdup_printf (HTTP_EP_SERVER_ROOT_PATH);

  kms_http_ep_server_register_handler (self, root_path, g_object_ref (self),
      g_object_unref);

  /* Connect server signals handlers */
  g_signal_connect (self->priv->server, "request-started",
      G_CALLBACK (request_started_handler), self);

  soup_server_run_async (self->priv->server);

  listener = soup_server_get_listener (self->priv->server);

  if (!soup_socket_is_connected (listener) ) {
    GST_ERROR ("Server socket is not connected");
    return;
  }

  addr = soup_socket_get_local_address (listener);
  GST_DEBUG ("Http end point server running in %s:%d",
      soup_address_get_physical (addr), soup_address_get_port (addr) );
}

static void
soup_address_callback (SoupAddress *addr, guint status, gpointer user_data)
{
  struct resolv_data *rdata = (struct resolv_data *) user_data;
  GError *gerr = NULL;

  switch (status) {
  case SOUP_STATUS_OK:
    GST_DEBUG ("Domain name resolved");
    kms_http_ep_server_create_server (rdata->server, addr);
    break;
  case SOUP_STATUS_CANCELLED:
    g_set_error (&gerr, KMS_HTTP_EP_SERVER_ERROR,
        HTTPEPSERVER_RESOLVE_CANCELED_ERROR, "Domain name resolution canceled");
    break;
  case SOUP_STATUS_CANT_RESOLVE:
    g_set_error (&gerr, KMS_HTTP_EP_SERVER_ERROR,
        HTTPEPSERVER_CANT_RESOLVE_ERROR, "Domain name can not be resolved");
    break;
  default:
    g_set_error (&gerr, KMS_HTTP_EP_SERVER_ERROR,
        HTTPEPSERVER_UNEXPECTED_ERROR, "Domain name can not be resolved");
    break;
  }

  rdata->cb (rdata->server, gerr);
  g_object_unref (rdata->server);

  if (gerr != NULL)
    g_error_free (gerr);

  g_slice_free (struct resolv_data, rdata);
}

static void
kms_http_ep_server_start_impl (KmsHttpEPServer *self,
    KmsHttpEPServerStartCallback start_cb)
{
  struct resolv_data *rdata;
  SoupAddress *addr = NULL;

  if (self->priv->server != NULL) {
    GST_WARNING ("Server is already running");
    return;
  }

  if (self->priv->iface == NULL) {
    kms_http_ep_server_create_server (self, NULL);
    start_cb (self, NULL);
    return;
  }

  rdata = g_slice_new (struct resolv_data);
  rdata->cb = start_cb;
  rdata->server = KMS_HTTP_EP_SERVER ( g_object_ref (self) );

  addr = soup_address_new (self->priv->iface, self->priv->port);

  soup_address_resolve_async (addr, NULL,
      NULL /* FIXME: Add cancellable support */,
      (SoupAddressCallback) soup_address_callback, rdata);
}

static const gchar *
kms_http_ep_server_register_end_point_impl (KmsHttpEPServer *self,
    gpointer data, GDestroyNotify destroy)
{
  gchar *url;
  uuid_t uuid;
  gchar *uuid_str;

  uuid_str = (gchar *) g_malloc (UUID_STR_SIZE);
  uuid_generate (uuid);
  uuid_unparse (uuid, uuid_str);

  /* Create URL from uuid string and add it to list of handlers */
  url = g_strdup_printf ("/%s", uuid_str);
  g_free (uuid_str);

  if (!kms_http_ep_server_register_handler (self, url, data, destroy) ) {
    g_free (url);
    return NULL;
  }

  return url;
}

static void
kms_http_ep_server_dispose (GObject *obj)
{
  KmsHttpEPServer *self = KMS_HTTP_EP_SERVER (obj);

  GST_DEBUG_OBJECT (self, "dispose");

  if (self->priv->server) {
    soup_server_disconnect (self->priv->server);
    g_clear_object (&self->priv->server);
  }

  /* Chain up to the parent class */
  G_OBJECT_CLASS (kms_http_ep_server_parent_class)->dispose (obj);
}

static void
kms_http_ep_server_finalize (GObject *obj)
{
  KmsHttpEPServer *self = KMS_HTTP_EP_SERVER (obj);

  GST_DEBUG_OBJECT (self, "finalize");

  if (self->priv->iface) {
    g_free (self->priv->iface);
    self->priv->iface = NULL;
  }

  if (self->priv->handlers != NULL) {
    g_slist_free_full (self->priv->handlers, g_free);
    self->priv->handlers = NULL;
  }

  /* Chain up to the parent class */
  G_OBJECT_CLASS (kms_http_ep_server_parent_class)->finalize (obj);
}

static void
kms_http_ep_server_set_property (GObject *obj, guint prop_id,
    const GValue *value, GParamSpec *pspec)
{
  KmsHttpEPServer *self = KMS_HTTP_EP_SERVER (obj);

  switch (prop_id) {
  case PROP_KMS_HTTP_EP_SERVER_PORT:
    self->priv->port = g_value_get_int (value);
    break;
  case PROP_KMS_HTTP_EP_SERVER_INTERFACE:

    if (self->priv->iface != NULL)
      g_free (self->priv->iface);

    self->priv->iface = g_value_dup_string (value);
    break;
  default:
    /* We don't have any other property... */
    G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
    break;
  }
}

static void
kms_http_ep_server_get_property (GObject *obj, guint prop_id, GValue *value,
    GParamSpec *pspec)
{
  KmsHttpEPServer *self = KMS_HTTP_EP_SERVER (obj);

  switch (prop_id) {
  case PROP_KMS_HTTP_EP_SERVER_PORT:
    g_value_set_int (value, self->priv->port);
    break;
  case PROP_KMS_HTTP_EP_SERVER_INTERFACE:
    g_value_set_string (value, self->priv->iface);
    break;
  default:
    /* We don't have any other property... */
    G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
    break;
  }
}

static void
kms_http_ep_server_class_init (KmsHttpEPServerClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->set_property = kms_http_ep_server_set_property;
  gobject_class->get_property = kms_http_ep_server_get_property;
  gobject_class->dispose = kms_http_ep_server_dispose;
  gobject_class->finalize = kms_http_ep_server_finalize;

  /* Set public virtual methods */
  klass->start = kms_http_ep_server_start_impl;
  klass->stop = kms_http_ep_server_stop_impl;
  klass->register_end_point = kms_http_ep_server_register_end_point_impl;

  obj_properties[PROP_KMS_HTTP_EP_SERVER_PORT] =
    g_param_spec_int ("port",
        "port number",
        "The TCP port to listen on",
        0,
        G_MAXUSHORT,
        KMS_HTTP_EP_SERVER_DEFAULT_PORT,
        (GParamFlags) (G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE) );

  obj_properties[PROP_KMS_HTTP_EP_SERVER_INTERFACE] =
    g_param_spec_string ("interface",
        "IP address",
        "IP address of the network interface to run the server on",
        KMS_HTTP_EP_SERVER_DEFAULT_INTERFACE,
        (GParamFlags) (G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE) );

  g_object_class_install_properties (gobject_class,
      N_PROPERTIES,
      obj_properties);

  obj_signals[URL_REMOVED] =
    g_signal_new ("url-removed",
        G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_LAST,
        G_STRUCT_OFFSET (KmsHttpEPServerClass, url_removed), NULL, NULL,
        g_cclosure_marshal_VOID__STRING, G_TYPE_NONE, 1, G_TYPE_STRING);

  /* Registers a private structure for an instantiatable type */
  g_type_class_add_private (klass, sizeof (KmsHttpEPServerPrivate) );
}

static void
kms_http_ep_server_init (KmsHttpEPServer *self)
{
  self->priv = KMS_HTTP_EP_SERVER_GET_PRIVATE (self);

  /* Set default values */
  self->priv->server = NULL;
  self->priv->handlers = NULL;
  self->priv->port = KMS_HTTP_EP_SERVER_DEFAULT_PORT;
  self->priv->iface = KMS_HTTP_EP_SERVER_DEFAULT_INTERFACE;
}

/* Virtual public methods */
KmsHttpEPServer *
kms_http_ep_server_new (const char *optname1, ...)
{
  KmsHttpEPServer *self;

  va_list ap;

  va_start (ap, optname1);
  self = KMS_HTTP_EP_SERVER (g_object_new_valist (KMS_TYPE_HTTP_EP_SERVER,
      optname1, ap) );
  va_end (ap);

  return KMS_HTTP_EP_SERVER (self);
}

void
kms_http_ep_server_start (KmsHttpEPServer *self,
    KmsHttpEPServerStartCallback start_cb)
{
  g_return_if_fail (KMS_IS_HTTP_EP_SERVER (self) );

  KMS_HTTP_EP_SERVER_GET_CLASS (self)->start (self, start_cb);
}

void
kms_http_ep_server_stop (KmsHttpEPServer *self)
{
  g_return_if_fail (KMS_IS_HTTP_EP_SERVER (self) );

  KMS_HTTP_EP_SERVER_GET_CLASS (self)->stop (self);
}

const gchar *
kms_http_ep_server_register_end_point (KmsHttpEPServer *self,
    gpointer data, GDestroyNotify destroy)
{
  g_return_val_if_fail (KMS_IS_HTTP_EP_SERVER (self), NULL);

  return KMS_HTTP_EP_SERVER_GET_CLASS (self)->register_end_point (self, data,
      destroy);
}