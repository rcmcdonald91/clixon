/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand
  Copyright (C) 2020 Olof Hagsand and Rubicon Communications, LLC(Netgate)

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

  * libevhtp code  
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

/* The clixon evhtp code can be compiled with or without threading support 
 * The choice is set at libevhtp compile time by cmake. Eg:
 *    cmake -DEVHTP_DISABLE_EVTHR=ON # Disable threads.
 * Default in testing is disabled threads.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <fcntl.h>
#include <ctype.h>
#include <time.h>
#include <limits.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <assert.h>
#include <sys/stat.h> /* chmod */

/* evhtp */
#include <evhtp/evhtp.h>
#include <evhtp/sslutils.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

/* restconf */

#include "restconf_lib.h"       /* generic shared with plugins */
#include "restconf_handle.h"
#include "restconf_api.h"       /* generic not shared with plugins */
#include "restconf_err.h"
#include "restconf_root.h"

/* Command line options to be passed to getopt(3) */
#define RESTCONF_OPTS "hD:f:l:p:d:y:a:u:o:P:sc"

/* See see listen(5) */
#define SOCKET_LISTEN_BACKLOG 16

/* Need global variable to for signal handler XXX */
static clicon_handle _CLICON_HANDLE = NULL;

static struct evhtp_handle{
    evhtp_t           *eh_htp;
    struct event_base *eh_evbase;
    evhtp_ssl_cfg_t   *eh_ssl_config;
} _EVHTP_HANDLE = {0,};

static void
evhtp_terminate(struct evhtp_handle *eh)
{
    if (eh->eh_htp){
	evhtp_unbind_socket(eh->eh_htp);
	evhtp_free(eh->eh_htp);
    }
    if (eh->eh_evbase)
	event_base_free(eh->eh_evbase);
    if (eh->eh_ssl_config)
	free(eh->eh_ssl_config);
}

/*! Signall terminates process
 */
static void
restconf_sig_term(int arg)
{
    static int i=0;

    if (i++ == 0)
	clicon_log(LOG_NOTICE, "%s: %s: pid: %u Signal %d", 
		   __PROGRAM__, __FUNCTION__, getpid(), arg);
    else
	exit(-1);
    evhtp_terminate(&_EVHTP_HANDLE);
    if (_CLICON_HANDLE){
	//	stream_child_freeall(_CLICON_HANDLE);
	restconf_terminate(_CLICON_HANDLE);
    }
    clicon_exit_set(); /* checked in clixon_event_loop() */
    exit(-1);
}

static void
restconf_sig_child(int arg)
{
    int status;
    int pid;

    if ((pid = waitpid(-1, &status, 0)) != -1 && WIFEXITED(status)){
    }
}

static char*
evhtp_method2str(enum htp_method m)
{
    switch (m){
    case htp_method_GET:
	return "GET";
	break;
    case htp_method_HEAD:
	return "HEAD";
	break;
    case htp_method_POST:
	return "POST";
	break;
    case htp_method_PUT:
	return "PUT";
	break;
    case htp_method_DELETE:
	return "DELETE";
	break;
    case htp_method_MKCOL:
	return "MKCOL";
	break;
    case htp_method_COPY:
	return "COPY";
	break;
    case htp_method_MOVE:
	return "MOVE";
	break;
    case htp_method_OPTIONS:
	return "OPTIONS";
	break;
    case htp_method_PROPFIND:
	return "PROPFIND";
	break;
    case htp_method_PROPPATCH:
	return "PROPPATCH";
	break;
    case htp_method_LOCK:
	return "LOCK";
	break;
    case htp_method_UNLOCK:
	return "UNLOCK";
	break;
    case htp_method_TRACE:
	return "TRACE";
	break;
    case htp_method_CONNECT:
	return "CONNECT";
	break;
    case htp_method_PATCH:
	return "PATCH";
	break;
    default:
	return "UNKNOWN";
	break;
    }
}

static int
query_iterator(evhtp_header_t *hdr,
	       void           *arg)
{
    cvec   *qvec = (cvec *)arg;
    char   *key;
    char   *val;
    char   *valu = NULL;    /* unescaped value */
    cg_var *cv;

    key = hdr->key;
    val = hdr->val;
    if (uri_percent_decode(val, &valu) < 0)
	return -1;
    if ((cv = cvec_add(qvec, CGV_STRING)) == NULL){
	clicon_err(OE_UNIX, errno, "cvec_add");
	return -1;
    }
    cv_name_set(cv, key);
    cv_string_set(cv, valu);
    if (valu)
	free(valu); 
    return 0;
}

/*! Translate http header by capitalizing, prepend w HTTP_ and - -> _
 * Example: Host -> HTTP_HOST 
 */
static int
convert_fcgi(evhtp_header_t *hdr,
	     void           *arg)
{
    int           retval = -1;
    clicon_handle h = (clicon_handle)arg;
    cbuf         *cb = NULL;
    int           i;
    char          c;
    
    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_UNIX, errno, "cbuf_new");
	goto done;
    }
    /* convert key name */
    cprintf(cb, "HTTP_");
    for (i=0; i<strlen(hdr->key); i++){
	c = hdr->key[i] & 0xff;
	if (islower(c))
	    cprintf(cb, "%c", toupper(c));
	else if (c == '-')
	    cprintf(cb, "_");
	else
	    cprintf(cb, "%c", c);
    }
    if (restconf_param_set(h, cbuf_get(cb), hdr->val) < 0)
	goto done;
    retval = 0;
 done:
    if (cb)
	cbuf_free(cb);
    return retval;
}

/*! Map from evhtp information to "fcgi" type parameters used in clixon code
 *
 * While all these params come via one call in fcgi, the information must be taken from
 * several different places in evhtp 
 * @param[in]  h    Clicon handle
 * @param[in]  req  Evhtp request struct
 * @param[out] qvec Query parameters, ie the ?<id>=<val>&<id>=<val> stuff
 * @retval     1    OK continue
 * @retval     0    Fail, dont continue
 * @retval    -1    Error
 * The following parameters are set:
 * QUERY_STRING
 * REQUEST_METHOD
 * REQUEST_URI
 * HTTPS
 * HTTP_HOST
 * HTTP_ACCEPT
 * HTTP_CONTENT_TYPE
 * @note there may be more used by an application plugin
 */
static int
evhtp_params_set(clicon_handle    h,
		 evhtp_request_t *req,
    		 cvec            *qvec)
{
    int           retval = -1;
    htp_method    meth;
    evhtp_uri_t  *uri;
    evhtp_path_t *path;
    evhtp_ssl_t  *ssl = NULL;
    char         *subject;
    cvec         *cvv = NULL;
    char         *cn;

    
    if ((uri = req->uri) == NULL){
	clicon_err(OE_DAEMON, EFAULT, "No uri");
	goto done;
    }
    if ((path = uri->path) == NULL){
	clicon_err(OE_DAEMON, EFAULT, "No path");
	goto done;
    }
    meth = evhtp_request_get_method(req);

    /* QUERY_STRING in fcgi but go direct to the info instead of putting it in a string?
     * This is different from all else: Ie one could have re-created a string here but
     * that would mean double parsing,...
     */
    if (qvec && uri->query)
	if (evhtp_kvs_for_each(uri->query, query_iterator, qvec) < 0){
	    clicon_err(OE_CFG, errno, "evhtp_kvs_for_each");
	    goto done;
	}
    if (restconf_param_set(h, "REQUEST_METHOD", evhtp_method2str(meth)) < 0)
	goto done;
    if (restconf_param_set(h, "REQUEST_URI", path->full) < 0)
	goto done;
    clicon_debug(1, "%s proto:%d", __FUNCTION__, req->proto);
    if (req->proto != EVHTP_PROTO_10 &&
	req->proto != EVHTP_PROTO_11){
	if (restconf_badrequest(h, req)	< 0)
	    goto done;
	goto fail;
    }
    clicon_debug(1, "%s conn->ssl:%d", __FUNCTION__, req->conn->ssl?1:0);
    if ((ssl = req->conn->ssl) != NULL){
	if (restconf_param_set(h, "HTTPS", "https") < 0) /* some string or NULL */
	    goto done;
	/* SSL subject fields, eg CN (Common Name) , can add more here? */
	if ((subject = (char*)htp_sslutil_subject_tostr(req->conn->ssl)) != NULL){
	    if (str2cvec(subject, '/', '=', &cvv) < 0)
		goto done;
	    if ((cn = cvec_find_str(cvv, "CN")) != NULL){
		if (restconf_param_set(h, "SSL_CN", cn) < 0)
		    goto done;
	    }
	}
    }

    /* Translate all http headers by capitalizing, prepend w HTTP_ and - -> _
     * Example: Host -> HTTP_HOST 
     */
    if (evhtp_headers_for_each(req->headers_in, convert_fcgi, h) < 0)
	goto done;
    retval = 1;
 done:
    if (cvv)
	cvec_free(cvv);
    return retval;
 fail:
    retval = 0;
    goto done;
}

static int
print_header(evhtp_header_t *header,
	     void           *arg)
{
    //    clicon_handle  h = (clicon_handle)arg;
    
    clicon_debug(1, "%s %s %s",
		 __FUNCTION__, header->key, header->val);
    return 0;
}

static evhtp_res
cx_pre_accept(evhtp_connection_t *conn,
	      void               *arg)
{
    //    clicon_handle  h = (clicon_handle)arg;

    clicon_debug(1, "%s", __FUNCTION__);    
    return EVHTP_RES_OK;
}

static evhtp_res
cx_post_accept(evhtp_connection_t *conn,
	       void               *arg)
{
    //    clicon_handle  h = (clicon_handle)arg;

    clicon_debug(1, "%s", __FUNCTION__);    
    return EVHTP_RES_OK;
}

/*! Generic callback called if no other callbacks are matched
 */
static void
cx_gencb(evhtp_request_t *req,
	 void            *arg)
{
    evhtp_connection_t *conn;
    //    clicon_handle       h = arg;

    clicon_debug(1, "%s", __FUNCTION__);    
    if (req == NULL){
	errno = EINVAL;
	return;
    }
    if ((conn = evhtp_request_get_connection(req)) == NULL)
	goto done;
    htp_sslutil_add_xheaders(
        req->headers_out,
        conn->ssl,
        HTP_SSLUTILS_XHDR_ALL);
    evhtp_send_reply(req, EVHTP_RES_NOTFOUND);
 done:
    return; /* void */
}

/*! /.well-known callback
 * @see cx_genb
 */
static void
cx_path_wellknown(evhtp_request_t *req,
		  void            *arg)
{
    clicon_handle h = arg;
    int           ret;

    clicon_debug(1, "------------");
    /* input debug */
    if (clicon_debug_get())
	evhtp_headers_for_each(req->headers_in, print_header, h);
    /* get accepted connection */

    /* set fcgi-like paramaters (ignore query vector) */
    if ((ret = evhtp_params_set(h, req, NULL)) < 0)
	goto done;
    if (ret == 1){
	/* call generic function */
	if (api_well_known(h, req) < 0)
	    goto done;
    }
    /* Clear (fcgi) paramaters from this request */
    if (restconf_param_del_all(h) < 0)
	goto done;
 done:
    return; /* void */
}

/*! /restconf callback
 * @see cx_genb
 */
static void
cx_path_restconf(evhtp_request_t *req,
		 void            *arg)
{
    clicon_handle h = arg;
    int           ret;
    cvec         *qvec = NULL;

    clicon_debug(1, "------------");
    /* input debug */
    if (clicon_debug_get())
	evhtp_headers_for_each(req->headers_in, print_header, h);

    
    /* get accepted connection */
    /* Query vector, ie the ?a=x&b=y stuff */
    if ((qvec = cvec_new(0)) ==NULL){
	clicon_err(OE_UNIX, errno, "cvec_new");
	goto done;
    }
    /* set fcgi-like paramaters (ignore query vector) */
    if ((ret = evhtp_params_set(h, req, qvec)) < 0)
	goto done;
    if (ret == 1){
	/* call generic function */
	if (api_root_restconf(h, req, qvec) < 0)
	    goto done;
    }

    /* Clear (fcgi) paramaters from this request */
    if (restconf_param_del_all(h) < 0)
	goto done;
 done:
    if (qvec)
	cvec_free(qvec);
    return; /* void */
}

/*! Get Server cert info
 * @param[in]  h           Clicon handle
 * @param[out] ssl_config  evhtp ssl config struct
 */
static int
cx_get_certs(clicon_handle    h,
	     int              ssl_verify_clients,
	     evhtp_ssl_cfg_t *ssl_config)
{
    int         retval = -1;
    struct stat f_stat;
    char       *filename;

    if (ssl_config == NULL){
	clicon_err(OE_CFG, EINVAL, "Input parameter is NULL");
	goto done;
    }
    if ((filename = clicon_option_str(h, "CLICON_SSL_SERVER_CERT")) == NULL){
	clicon_err(OE_CFG, EFAULT, "CLICON_SSL_SERVER_CERT option missing");
	goto done;
    }
    if ((ssl_config->pemfile = strdup(filename)) == NULL){
	clicon_err(OE_CFG, errno, "strdup");
	goto done;
    }
    if (stat(ssl_config->pemfile, &f_stat) != 0) {
	clicon_err(OE_FATAL, errno, "Cannot load SSL cert '%s'", ssl_config->pemfile);
	goto done;
    }
    if ((filename = clicon_option_str(h, "CLICON_SSL_SERVER_KEY")) == NULL){
	clicon_err(OE_CFG, EFAULT, "CLICON_SSL_SERVER_KEY option missing");
	goto done;
    }
    if ((ssl_config->privfile = strdup(filename)) == NULL){
	clicon_err(OE_CFG, errno, "strdup");
	goto done;
    }
    if (stat(ssl_config->privfile, &f_stat) != 0) {
	clicon_err(OE_FATAL, errno, "Cannot load SSL key '%s'", ssl_config->privfile);
	goto done;
    }
    if (ssl_verify_clients){
	if ((filename = clicon_option_str(h, "CLICON_SSL_CA_CERT")) == NULL){
	    clicon_err(OE_CFG, EFAULT, "CLICON_SSL_CA_CERT option missing");
	    goto done;
	}
	if ((ssl_config->cafile = strdup(filename)) == NULL){
	    clicon_err(OE_CFG, errno, "strdup");
	    goto done;
	}
	if (stat(ssl_config->cafile, &f_stat) != 0) {
	    clicon_err(OE_FATAL, errno, "Cannot load SSL key '%s'", ssl_config->privfile);
	    goto done;
	}
    }
    retval = 0;
 done:
    return retval;
}

static int
cx_verify_certs(int pre_verify,
		evhtp_x509_store_ctx_t *store)
{
#ifdef NOTYET
    char                 buf[256];
    X509               * err_cert;
    int                  err;
    int                  depth;
    SSL                * ssl;
    evhtp_connection_t * connection;
    evhtp_ssl_cfg_t    * ssl_cfg;
    
    fprintf(stderr, "%s %d\n", __FUNCTION__, pre_verify);

    err_cert   = X509_STORE_CTX_get_current_cert(store);
    err        = X509_STORE_CTX_get_error(store);
    depth      = X509_STORE_CTX_get_error_depth(store);
    ssl        = X509_STORE_CTX_get_ex_data(store, SSL_get_ex_data_X509_STORE_CTX_idx());

    X509_NAME_oneline(X509_get_subject_name(err_cert), buf, 256);
    
    connection = SSL_get_app_data(ssl);
    ssl_cfg    = connection->htp->ssl_cfg;
#endif
    return pre_verify;
}

/*! Usage help routine
 * @param[in]  argv0  command line
 * @param[in]  h      Clicon handle
 */
static void
usage(clicon_handle h,
      char         *argv0)

{
    fprintf(stderr, "usage:%s [options]\n"
	    "where options are\n"
            "\t-h \t\t  Help\n"
	    "\t-D <level>\t  Debug level\n"
    	    "\t-f <file>\t  Configuration file (mandatory)\n"
	    "\t-l <s|f<file>> \t  Log on (s)yslog, (f)ile (syslog is default)\n"
	    "\t-p <dir>\t  Yang directory path (see CLICON_YANG_DIR)\n"
	    "\t-d <dir>\t  Specify restconf plugin directory dir (default: %s)\n"
	    "\t-y <file>\t  Load yang spec file (override yang main module)\n"
    	    "\t-a UNIX|IPv4|IPv6 Internal backend socket family\n"
    	    "\t-u <path|addr>\t  Internal socket domain path or IP addr (see -a)\n"
	    "\t-o <option>=<value> Set configuration option overriding config file (see clixon-config.yang)\n"
	    "\t-s\t\t  SSL server, https\n"
	    "\t-c\t\t  SSL verify client certs\n"
    	    "\t-P <port>\t  HTTP port (default 80, or 443 if -s is given)\n"
	    ,
	    argv0,
	    clicon_restconf_dir(h)
	    );
    exit(0);
}

    

/*! Main routine for libevhtp restconf
 */
int
main(int    argc,
     char **argv)
{
    int                retval = -1;
    char	      *argv0 = argv[0];
    int                c;
    clicon_handle      h;
    char              *dir;
    int                logdst = CLICON_LOG_SYSLOG;
    yang_stmt         *yspec = NULL;
    char              *str;
    clixon_plugin     *cp = NULL;
    cvec              *nsctx_global = NULL; /* Global namespace context */
    size_t             cligen_buflen;
    size_t             cligen_bufthreshold;
    uint16_t           defaultport;
    uint16_t           port = 0;
    int                dbg = 0;
    int                use_ssl = 0;
    int                ssl_verify_clients = 0;
    char              *restconf_ipv4_addr = NULL;
    char              *restconf_ipv6_addr = NULL;
    int                i;
    struct evhtp_handle *eh = &_EVHTP_HANDLE;

    /* In the startup, logs to stderr & debug flag set later */
    clicon_log_init(__PROGRAM__, LOG_INFO, logdst); 

    /* Create handle */
    if ((h = restconf_handle_init()) == NULL)
	goto done;

    _CLICON_HANDLE = h; /* for termination handling */

    while ((c = getopt(argc, argv, RESTCONF_OPTS)) != -1)
	switch (c) {
	case 'h':
	    usage(h, argv0);
	    break;
	case 'D' : /* debug */
	    if (sscanf(optarg, "%d", &dbg) != 1)
		usage(h, argv0);
	    break;
	 case 'f': /* override config file */
	    if (!strlen(optarg))
		usage(h, argv0);
	    clicon_option_str_set(h, "CLICON_CONFIGFILE", optarg);
	    break;
	 case 'l': /* Log destination: s|e|o */
	     if ((logdst = clicon_log_opt(optarg[0])) < 0)
		usage(h, argv0);
	    if (logdst == CLICON_LOG_FILE &&
		strlen(optarg)>1 &&
		clicon_log_file(optarg+1) < 0)
		goto done;
	   break;
	} /* switch getopt */

    /* 
     * Logs, error and debug to stderr or syslog, set debug level
     */
    clicon_log_init(__PROGRAM__, dbg?LOG_DEBUG:LOG_INFO, logdst); 

    clicon_debug_init(dbg, NULL); 
    clicon_log(LOG_NOTICE, "%s: %u Started", __PROGRAM__, getpid());
    if (set_signal(SIGTERM, restconf_sig_term, NULL) < 0){
	clicon_err(OE_DAEMON, errno, "Setting signal");
	goto done;
    }
    if (set_signal(SIGINT, restconf_sig_term, NULL) < 0){
	clicon_err(OE_DAEMON, errno, "Setting signal");
	goto done;
    }
    if (set_signal(SIGCHLD, restconf_sig_child, NULL) < 0){
	clicon_err(OE_DAEMON, errno, "Setting signal");
	goto done;
    }
    
    /* Find and read configfile */
    if (clicon_options_main(h) < 0)
	goto done;
    //    stream_path = clicon_option_str(h, "CLICON_STREAM_PATH");
    /* Start with http default port, but change this later if -s is set to https default port */
    if ((i = clicon_option_int(h, "CLICON_RESTCONF_HTTP_PORT")) < 0){
	clicon_err(OE_CFG, EINVAL, "CLICON_RESTCONF_HTTP_PORT not found");
	goto done;
    }
    defaultport = (uint16_t)i;
    
    /* Now rest of options, some overwrite option file */
    optind = 1;
    opterr = 0;
    while ((c = getopt(argc, argv, RESTCONF_OPTS)) != -1)
	switch (c) {
	case 'h' : /* help */
	case 'D' : /* debug */
	case 'f':  /* config file */
	case 'l':  /* log  */
	    break; /* see above */
	case 'p' : /* yang dir path */
	    if (clicon_option_add(h, "CLICON_YANG_DIR", optarg) < 0)
		goto done;
	    break;
	case 'd':  /* Plugin directory */
	    if (!strlen(optarg))
		usage(h, argv0);
	    clicon_option_str_set(h, "CLICON_RESTCONF_DIR", optarg);
	    break;
	case 'y' : /* Load yang spec file (override yang main module) */
	    clicon_option_str_set(h, "CLICON_YANG_MAIN_FILE", optarg);
	    break;
	case 'a': /* internal backend socket address family */
	    clicon_option_str_set(h, "CLICON_SOCK_FAMILY", optarg);
	    break;
	case 'u': /* internal backend socket unix domain path or ip host */
	    if (!strlen(optarg))
		usage(h, argv0);
	    clicon_option_str_set(h, "CLICON_SOCK", optarg);
	    break;
	case 'o':{ /* Configuration option */
	    char          *val;
	    if ((val = index(optarg, '=')) == NULL)
		usage(h, argv0);
	    *val++ = '\0';
	    if (clicon_option_add(h, optarg, val) < 0)
		goto done;
	    break;
	}
	case 's': /* ssl: use https */
	    use_ssl = 1;
	    /* Set to port - note can be overrifden by -P */
	    if ((i = clicon_option_int(h, "CLICON_RESTCONF_HTTPS_PORT")) < 0){
		clicon_err(OE_CFG, EINVAL, "CLICON_RESTCONF_HTTPS_PORT not found");
		goto done;
	    }
	    defaultport = (uint16_t)i;
	    break;
	case 'c': /* ssl: verify clients */
	    ssl_verify_clients = 1;
	    break;
	case 'P': /* http port */
	    if (!strlen(optarg))
		usage(h, argv0);
	    port=atoi(optarg);
	    break;
        default:
            usage(h, argv0);
            break;
	}
    argc -= optind;
    argv += optind;
    /* port = defaultport unless explicitly set -P */
    if (port == 0)
	port = defaultport;
    if (port == 0){
	clicon_err(OE_DAEMON, EINVAL, "Restconf bind port is 0");
	goto done;
    }
    /* Check server ssl certs */
    if (use_ssl){
	/* Init evhtp ssl config struct */
	if ((eh->eh_ssl_config = malloc(sizeof(evhtp_ssl_cfg_t))) == NULL){
	    clicon_err(OE_UNIX, errno, "malloc");
	    goto done;
	}
	memset(eh->eh_ssl_config, 0, sizeof(evhtp_ssl_cfg_t));
	eh->eh_ssl_config->ssl_opts = SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1;

	if (cx_get_certs(h, ssl_verify_clients, eh->eh_ssl_config) < 0)
	    goto done;
	eh->eh_ssl_config->x509_verify_cb = cx_verify_certs; /* Is extra verification necessary? */
	if (ssl_verify_clients){
	    eh->eh_ssl_config->verify_peer = SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
	    eh->eh_ssl_config->x509_verify_cb = cx_verify_certs;
	    eh->eh_ssl_config->verify_depth = 2;
	}
    }

    //    ssl_verify_mode             = htp_sslutil_verify2opts(optarg);
    assert(SSL_VERIFY_NONE == 0);
    /* Access the remaining argv/argc options (after --) w clicon-argv_get() */
    clicon_argv_set(h, argv0, argc, argv);
    
    /* Init evhtp */
    if ((eh->eh_evbase = event_base_new()) == NULL){
	clicon_err(OE_UNIX, errno, "event_base_new");
	goto done;
    }
    /* create a new evhtp_t instance */
    if ((eh->eh_htp = evhtp_new(eh->eh_evbase, NULL)) == NULL){
	clicon_err(OE_UNIX, errno, "evhtp_new");
	goto done;
    }
    /* Here the daemon either uses SSL or not, ie you cant seem to mix http and https :-( */
    if (use_ssl){
	if (evhtp_ssl_init(eh->eh_htp, eh->eh_ssl_config) < 0){
	    clicon_err(OE_UNIX, errno, "evhtp_new");
	    goto done;
	}
    }
#ifndef EVHTP_DISABLE_EVTHR
    evhtp_use_threads_wexit(eh->eh_htp, NULL, NULL, 4, NULL);
#endif

    /* Callback before the connection is accepted. */
    evhtp_set_pre_accept_cb(eh->eh_htp, cx_pre_accept, h);

    /* Callback right after a connection is accepted. */
    evhtp_set_post_accept_cb(eh->eh_htp, cx_post_accept, h);

    /* Callback to be executed for all /restconf api calls */
    if (evhtp_set_cb(eh->eh_htp, "/" RESTCONF_API, cx_path_restconf, h) == NULL){
    	clicon_err(OE_EVENTS, errno, "evhtp_set_cb");
    	goto done;
    }
    /* Callback to be executed for all /restconf api calls */
    if (evhtp_set_cb(eh->eh_htp, RESTCONF_WELL_KNOWN, cx_path_wellknown, h) == NULL){
    	clicon_err(OE_EVENTS, errno, "evhtp_set_cb");
    	goto done;
    }
    /* Generic callback called if no other callbacks are matched */
    evhtp_set_gencb(eh->eh_htp, cx_gencb, h);

    /* bind to a socket, optionally with specific protocol support formatting 
     */
    restconf_ipv4_addr = clicon_option_str(h, "CLICON_RESTCONF_IPV4_ADDR");
    restconf_ipv6_addr = clicon_option_str(h, "CLICON_RESTCONF_IPV6_ADDR");
    if ((restconf_ipv4_addr == NULL || strlen(restconf_ipv4_addr)==0) &&
	(restconf_ipv6_addr == NULL || strlen(restconf_ipv6_addr)==0)){
	clicon_err(OE_DAEMON, EINVAL, "There are no restconf IPv4 or IPv6  bind addresses");
	goto done;
    }
    if (restconf_ipv4_addr != NULL && strlen(restconf_ipv4_addr)){
	cbuf *cb;
	if ((cb = cbuf_new()) == NULL){
	    clicon_err(OE_UNIX, errno, "cbuf_new");
	    goto done;
	}
	cprintf(cb, "ipv4:%s", restconf_ipv4_addr);
	if (evhtp_bind_socket(eh->eh_htp,                  /* evhtp handle */
			      cbuf_get(cb),         /* string address, eg ipv4:<ipv4addr> */
			      port,                 /* port */
			      SOCKET_LISTEN_BACKLOG /* backlog flag, see listen(5)  */
			      ) < 0){
	    clicon_err(OE_UNIX, errno, "evhtp_bind_socket");
	    goto done;
	}
	if (cb)
	    cbuf_free(cb);
    }
    /* Eeh can only bind one */
    if (0 && restconf_ipv6_addr != NULL && strlen(restconf_ipv6_addr)){
	cbuf *cb;
	if ((cb = cbuf_new()) == NULL){
	    clicon_err(OE_UNIX, errno, "cbuf_new");
	    goto done;
	}
	cprintf(cb, "ipv6:%s", restconf_ipv6_addr);
	if (evhtp_bind_socket(eh->eh_htp,                  /* evhtp handle */
			      cbuf_get(cb),         /* string address, eg ipv6:<ipv6addr> */
      			      port,                 /* port */
			      SOCKET_LISTEN_BACKLOG /* backlog flag, see listen(5)  */
			      ) < 0){
	    clicon_err(OE_UNIX, errno, "evhtp_bind_socket");
	    goto done;
	}
	if (cb)
	    cbuf_free(cb);
    }
    /* Drop privileges to WWWUSER if started as root */
    if (restconf_drop_privileges(h, WWWUSER) < 0)
	goto done;

    /* Init cligen buffers */
    cligen_buflen = clicon_option_int(h, "CLICON_CLI_BUF_START");
    cligen_bufthreshold = clicon_option_int(h, "CLICON_CLI_BUF_THRESHOLD");
    cbuf_alloc_set(cligen_buflen, cligen_bufthreshold);

    /* Add (hardcoded) netconf features in case ietf-netconf loaded here
     * Otherwise it is loaded in netconf_module_load below
     */
    if (netconf_module_features(h) < 0)
	goto done;
    /* Create top-level yang spec and store as option */
    if ((yspec = yspec_new()) == NULL)
	goto done;
    clicon_dbspec_yang_set(h, yspec);
    /* Treat unknown XML as anydata */
    if (clicon_option_bool(h, "CLICON_YANG_UNKNOWN_ANYDATA") == 1)
	xml_bind_yang_unknown_anydata(1);
    
    /* Load restconf plugins before yangs are loaded (eg extension callbacks) */
    if ((dir = clicon_restconf_dir(h)) != NULL)
	if (clixon_plugins_load(h, CLIXON_PLUGIN_INIT, dir, NULL) < 0)
	    return -1;
    /* Create a pseudo-plugin to create extension callback to set the ietf-routing
     * yang-data extension for api-root top-level restconf function.
     */
    if (clixon_pseudo_plugin(h, "pseudo restconf", &cp) < 0)
	goto done;
    cp->cp_api.ca_extension = restconf_main_extension_cb;

    /* Load Yang modules
     * 1. Load a yang module as a specific absolute filename */
    if ((str = clicon_yang_main_file(h)) != NULL){
	if (yang_spec_parse_file(h, str, yspec) < 0)
	    goto done;
    }
    /* 2. Load a (single) main module */
    if ((str = clicon_yang_module_main(h)) != NULL){
	if (yang_spec_parse_module(h, str, clicon_yang_module_revision(h),
				   yspec) < 0)
	    goto done;
    }
    /* 3. Load all modules in a directory */
    if ((str = clicon_yang_main_dir(h)) != NULL){
	if (yang_spec_load_dir(h, str, yspec) < 0)
	    goto done;
    }
    /* Load clixon lib yang module */
    if (yang_spec_parse_module(h, "clixon-lib", NULL, yspec) < 0)
	goto done;
     /* Load yang module library, RFC7895 */
    if (yang_modules_init(h) < 0)
	goto done;

    /* Load yang restconf module */
    if (yang_spec_parse_module(h, "ietf-restconf", NULL, yspec)< 0)
	goto done;
    
    /* Add netconf yang spec, used as internal protocol */
    if (netconf_module_load(h) < 0)
	goto done;
    
    /* Add system modules */
     if (clicon_option_bool(h, "CLICON_STREAM_DISCOVERY_RFC8040") &&
	 yang_spec_parse_module(h, "ietf-restconf-monitoring", NULL, yspec)< 0)
	 goto done;
     if (clicon_option_bool(h, "CLICON_STREAM_DISCOVERY_RFC5277") &&
	 yang_spec_parse_module(h, "clixon-rfc5277", NULL, yspec)< 0)
	 goto done;

     /* Here all modules are loaded 
      * Compute and set canonical namespace context
      */
     if (xml_nsctx_yangspec(yspec, &nsctx_global) < 0)
	 goto done;
     if (clicon_nsctx_global_set(h, nsctx_global) < 0)
	 goto done;

     /* Dump configuration options on debug */
    if (dbg)      
	clicon_option_dump(h, dbg);

    /* Call start function in all plugins before we go interactive 
     */
     if (clixon_plugin_start_all(h) < 0)
	 goto done;

    event_base_loop(eh->eh_evbase, 0);

    retval = 0;
 done:
    clicon_debug(1, "restconf_main_evhtp done");
    //    stream_child_freeall(h);
    evhtp_terminate(&_EVHTP_HANDLE);    
    restconf_terminate(h);    
    return retval;
}
