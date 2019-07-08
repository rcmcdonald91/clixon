/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand and Benny Holmgren

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

 * 
 * Client-side functions for clicon_proto protocol
 * Historically this code was part of the clicon_cli application. But
 * it should (is?) be general enough to be used by other applications.
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/syslog.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include "clixon_queue.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_log.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_options.h"
#include "clixon_data.h"
#include "clixon_plugin.h"
#include "clixon_string.h"
#include "clixon_xpath_ctx.h"
#include "clixon_xpath.h"
#include "clixon_proto.h"
#include "clixon_err.h"
#include "clixon_err_string.h"
#include "clixon_netconf_lib.h"
#include "clixon_proto_client.h"

/*! Send internal netconf rpc from client to backend
 * @param[in]    h      CLICON handle
 * @param[in]    msg    Encoded message. Deallocate woth free
 * @param[out]   xret   Return value from backend as xml tree. Free w xml_free
 * @param[inout] sock0  If pointer exists, do not close socket to backend on success 
 *                      and return it here. For keeping a notify socket open
 * @note sock0 is if connection should be persistent, like a notification/subscribe api
 * @note xret is populated with yangspec according to standard handle yangspec
 */
int
clicon_rpc_msg(clicon_handle      h, 
	       struct clicon_msg *msg, 
	       cxobj            **xret0,
	       int               *sock0)
{
    int                retval = -1;
    char              *sock;
    int                port;
    char              *retdata = NULL;
    cxobj             *xret = NULL;
    yang_stmt         *yspec;

#ifdef RPC_USERNAME_ASSERT
    assert(strstr(msg->op_body, "username")!=NULL); /* XXX */
#endif
    clicon_debug(1, "%s request:%s", __FUNCTION__, msg->op_body);
    if ((sock = clicon_sock(h)) == NULL){
	clicon_err(OE_FATAL, 0, "CLICON_SOCK option not set");
	goto done;
    }
    /* What to do if inet socket? */
    switch (clicon_sock_family(h)){
    case AF_UNIX:
	if (clicon_rpc_connect_unix(msg, sock, &retdata, sock0) < 0){
#if 0
	    if (errno == ESHUTDOWN)
		/* Maybe could reconnect on a higher layer, but lets fail
		   loud and proud */
		cligen_exiting_set(cli_cligen(h), 1);
#endif
	    goto done;
	}
	break;
    case AF_INET:
	if ((port = clicon_sock_port(h)) < 0){
	    clicon_err(OE_FATAL, 0, "CLICON_SOCK option not set");
	    goto done;
	}
	if (port < 0){
	    clicon_err(OE_FATAL, 0, "CLICON_SOCK_PORT not set");
	    goto done;
	}
	if (clicon_rpc_connect_inet(msg, sock, port, &retdata, sock0) < 0)
	    goto done;
	break;
    }
    clicon_debug(1, "%s retdata:%s", __FUNCTION__, retdata);

    if (retdata){
 	yspec = clicon_dbspec_yang(h);
	if (xml_parse_string(retdata, yspec, &xret) < 0)
	    goto done;
    }
    if (xret0){
	*xret0 = xret;
	xret = NULL;
    }
    retval = 0;
 done:
    if (retdata)
	free(retdata);
    if (xret)
	xml_free(xret);
    return retval;
}

/*! Generic xml netconf clicon rpc
 * Want to go over to use netconf directly between client and server,...
 * @param[in]  h       clicon handle
 * @param[in]  xmlstr  XML netconf tree as string
 * @param[out] xret    Return XML netconf tree, error or OK (need to be freed)
 * @param[out] sp      Socket pointer for notification, otherwise NULL
 * @code
 *   cxobj *xret = NULL;
 *   if (clicon_rpc_netconf(h, "<rpc></rpc>", &xret, NULL) < 0)
 *	err;
 *   xml_free(xret);
 * @endcode
 * @see clicon_rpc_netconf_xml xml as tree instead of string
 */
int
clicon_rpc_netconf(clicon_handle  h, 
		   char          *xmlstr,
		   cxobj        **xret,
		   int           *sp)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;

    if ((msg = clicon_msg_encode("%s", xmlstr)) < 0)
	goto done;
    if (clicon_rpc_msg(h, msg, xret, sp) < 0)
	goto done;
    retval = 0;
 done:
    if (msg)
	free(msg);
    return retval;
}

/*! Generic xml netconf clicon rpc
 * Want to go over to use netconf directly between client and server,...
 * @param[in]  h       clicon handle
 * @param[in]  xml     XML netconf tree 
 * @param[out] xret    Return XML netconf tree, error or OK
 * @param[out] sp      Socket pointer for notification, otherwise NULL
 * @code
 *   cxobj *xret = NULL;
 *   int    s; 
 *   if (clicon_rpc_netconf_xml(h, x, &xret, &s) < 0)
 *	err;
 *   xml_free(xret);
 * @endcode

 * @see clicon_rpc_netconf xml as string instead of tree
 */
int
clicon_rpc_netconf_xml(clicon_handle  h, 
		       cxobj         *xml,
		       cxobj        **xret,
		       int           *sp)
{
    int                retval = -1;
    cbuf               *cb = NULL;

    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }
    if (clicon_xml2cbuf(cb, xml, 0, 0) < 0)
	goto done;
    if (clicon_rpc_netconf(h, cbuf_get(cb), xret, sp) < 0)
	goto done;
    retval = 0;
 done:
    if (cb)
	cbuf_free(cb);
    return retval;
}

/*! Generate and log clicon error function call from Netconf error message
 * @param[in]  prefix  Print this string (if given) before: "<prefix>: <error>"
 * @param[in]  xerr    Netconf error message on the level: <rpc-reply><rpc-error>
 */
int
clicon_rpc_generate_error(char  *prefix, 
			  cxobj *xerr)
{
    int    retval = -1;
    cbuf  *cb = NULL;

    if (netconf_err2cb(xerr, &cb) < 0)
	goto done;
    if (prefix)
	clicon_log(LOG_ERR, "%s: %s", prefix, cbuf_get(cb));
    else
	clicon_log(LOG_ERR, "%s", cbuf_get(cb));
    retval = 0;
 done:
    if (cb)
	cbuf_free(cb);
    return retval;
}

/*! Get database configuration
 * Same as clicon_proto_change just with a cvec instead of lvec
 * @param[in]  h        CLICON handle
 * @param[in]  db       Name of database
 * @param[in]  xpath    XPath (or "")
 * @param[in]  namespace Namespace associated w xpath
 * @param[out] xt       XML tree. Free with xml_free. 
 *                      Either <config> or <rpc-error>. 
 * @retval    0         OK
 * @retval   -1         Error, fatal or xml
 * @code
 *    cxobj *xt = NULL;
 *    if (clicon_rpc_get_config(h, "running", "/hello/world", "urn:example:hello", &xt) < 0)
 *       err;
 *   if ((xerr = xpath_first(xt, NULL,  "/rpc-error")) != NULL){
 *	clicon_rpc_generate_error("", xerr);
 *      err;
 *  }
 *    if (xt)
 *       xml_free(xt);
 * @endcode
 * @see clicon_rpc_generate_error
 */
int
clicon_rpc_get_config(clicon_handle       h, 
		      char               *db, 
		      char               *xpath,
		      char               *namespace,
		      cxobj             **xt)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cbuf              *cb = NULL;
    cxobj             *xret = NULL;
    cxobj             *xd;
    char              *username;
    
    if ((cb = cbuf_new()) == NULL)
	goto done;
    cprintf(cb, "<rpc");
    if ((username = clicon_username_get(h)) != NULL)
	cprintf(cb, " username=\"%s\"", username);
    if (namespace)
	cprintf(cb, " xmlns:nc=\"%s\"", NETCONF_BASE_NAMESPACE);
    cprintf(cb, "><get-config><source><%s/></source>", db);
    if (xpath && strlen(xpath)){
	if (namespace)
	    cprintf(cb, "<nc:filter nc:type=\"xpath\" nc:select=\"%s\" xmlns=\"%s\"/>",
		    xpath, namespace);
	else /* XXX shouldnt happen */
	    cprintf(cb, "<filter type=\"xpath\" select=\"%s\"/>", xpath);
    }
    cprintf(cb, "</get-config></rpc>");
    if ((msg = clicon_msg_encode("%s", cbuf_get(cb))) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    /* Send xml error back: first check error, then ok */
    if ((xd = xpath_first(xret, NULL, "/rpc-reply/rpc-error")) != NULL)
	xd = xml_parent(xd); /* point to rpc-reply */
    else if ((xd = xpath_first(xret, NULL, "/rpc-reply/data")) == NULL)
	if ((xd = xml_new("data", NULL, NULL)) == NULL)
	    goto done;
    if (xt){
	if (xml_rm(xd) < 0)
	    goto done;
	*xt = xd;
    }
    retval = 0;
  done:
    if (cb)
	cbuf_free(cb);
    if (xret)
	xml_free(xret);
    if (msg)
	free(msg);
    return retval;
}

/*! Send database entries as XML to backend daemon
 * @param[in] h          CLICON handle
 * @param[in] db         Name of database
 * @param[in] op         Operation on database item: OP_MERGE, OP_REPLACE
 * @param[in] xml        XML string. Ex: <config><a>..</a><b>...</b></config>
 * @retval    0          OK
 * @retval   -1          Error and logged to syslog
 * @note xml arg need to have <config> as top element
 * @code
 * if (clicon_rpc_edit_config(h, "running", OP_MERGE, 
 *                            "<config><a>4</a></config>") < 0)
 *    err;
 * @endcode
 */
int
clicon_rpc_edit_config(clicon_handle       h, 
		       char               *db, 
		       enum operation_type op,
		       char               *xmlstr)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cbuf              *cb = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;

    if ((cb = cbuf_new()) == NULL)
	goto done;
    cprintf(cb, "<rpc xmlns=\"%s\"", NETCONF_BASE_NAMESPACE);
    if ((username = clicon_username_get(h)) != NULL)
	cprintf(cb, " username=\"%s\"", username);
    cprintf(cb, "><edit-config><target><%s/></target>", db);
    cprintf(cb, "<default-operation>%s</default-operation>", 
	    xml_operation2str(op));
    if (xmlstr)
	cprintf(cb, "%s", xmlstr);
    cprintf(cb, "</edit-config></rpc>");
    if ((msg = clicon_msg_encode("%s", cbuf_get(cb))) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clicon_rpc_generate_error("Editing configuration", xerr);
	goto done;
    }
    retval = 0;
  done:
    if (xret)
	xml_free(xret);
    if (cb)
	cbuf_free(cb);
    if (msg)
	free(msg);
    return retval;
}

/*! Send a request to backend to copy a file from one location to another 
 * Note this assumes the backend can access these files and (usually) assumes
 * clients and servers have the access to the same filesystem.
 * @param[in] h        CLICON handle
 * @param[in] db1      src database, eg "running"
 * @param[in] db2      dst database, eg "startup"
 * @retval    0        OK
 * @retval   -1        Error and logged to syslog
 * @code
 * if (clicon_rpc_copy_config(h, "running", "startup") < 0)
 *    err;
 * @endcode
 */
int
clicon_rpc_copy_config(clicon_handle h, 
		       char         *db1, 
		       char         *db2)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;

    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode("<rpc username=\"%s\"><copy-config><source><%s/></source><target><%s/></target></copy-config></rpc>",
				 username?username:"",
				 db1, db2)) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clicon_rpc_generate_error("Copying configuration", xerr);
	goto done;
    }
    retval = 0;
 done:
    if (xret)
	xml_free(xret);
    if (msg)
	free(msg);
    return retval;
}

/*! Send a request to backend to delete a config database
 * @param[in] h        CLICON handle
 * @param[in] db       database, eg "running"
 * @retval    0        OK
 * @retval   -1        Error and logged to syslog
 * @code
 * if (clicon_rpc_delete_config(h, "startup") < 0)
 *    err;
 * @endcode
 */
int
clicon_rpc_delete_config(clicon_handle h, 
			 char         *db)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;

    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode("<rpc username=\"%s\"><edit-config><target><%s/></target><default-operation>none</default-operation><config operation=\"delete\"/></edit-config></rpc>",
				 username?username:"", db)) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clicon_rpc_generate_error("Deleting configuration", xerr);
	goto done;
    }
    retval = 0;
 done:
    if (xret)
	xml_free(xret);
    if (msg)
	free(msg);
    return retval;
}

/*! Lock a database
 * @param[in] h        CLICON handle
 * @param[in] db       database, eg "running"
 * @retval    0        OK
 * @retval   -1        Error and logged to syslog
 */
int
clicon_rpc_lock(clicon_handle h, 
		char         *db)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;

    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode("<rpc username=\"%s\"><lock><target><%s/></target></lock></rpc>",
				 username?username:"", db)) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clicon_rpc_generate_error("Locking configuration", xerr);
	goto done;
    }
    retval = 0;
 done:
    if (xret)
	xml_free(xret);
    if (msg)
	free(msg);
    return retval;
}

/*! Unlock a database
 * @param[in] h        CLICON handle
 * @param[in] db       database, eg "running"
 * @retval    0        OK
 * @retval   -1        Error and logged to syslog
 */
int
clicon_rpc_unlock(clicon_handle h, 
		  char         *db)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;

    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode("<rpc username=\"%s\"><unlock><target><%s/></target></unlock></rpc>", username?username:"", db)) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clicon_rpc_generate_error("Configuration unlock", xerr);
	goto done;
    }
    retval = 0;
 done:
    if (xret)
	xml_free(xret);
    if (msg)
	free(msg);
    return retval;
}

/*! Get database configuration and state data
 * @param[in]  h         Clicon handle
 * @param[in]  xpath     XPath in a filter stmt (or NULL/"" for no filter)
 * @param[in]  namespace Namespace associated w xpath
 * @param[out] xt        XML tree. Free with xml_free. 
 *                       Either <config> or <rpc-error>. 
 * @retval    0          OK
 * @retval   -1          Error, fatal or xml
 * @note if xpath is set but namespace is NULL, the default, netconf base 
 *       namespace will be used which is most probably wrong.
 * @code
 *    cxobj *xt = NULL;
 *    if (clicon_rpc_get(h, "/hello/world", "urn:example:hello", &xt) < 0)
 *       err;
 *   if ((xerr = xpath_first(xt, NULL, "/rpc-error")) != NULL){
 *	clicon_rpc_generate_error(xerr);
 *      err;
 *  }
 *    if (xt)
 *       xml_free(xt);
 * @endcode
 * @see clicon_rpc_generate_error
 */
int
clicon_rpc_get(clicon_handle       h, 
	       char               *xpath,
	       char               *namespace,
	       cxobj             **xt)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cbuf              *cb = NULL;
    cxobj             *xret = NULL;
    cxobj             *xd;
    char              *username;

    if ((cb = cbuf_new()) == NULL)
	goto done;
    cprintf(cb, "<rpc");
    if ((username = clicon_username_get(h)) != NULL)
	cprintf(cb, " username=\"%s\"", username);
    if (namespace)
	cprintf(cb, " xmlns:nc=\"%s\"", NETCONF_BASE_NAMESPACE);
    cprintf(cb, "><get>");
    if (xpath && strlen(xpath)) {
	if (namespace)
	    cprintf(cb, "<nc:filter nc:type=\"xpath\" nc:select=\"%s\" xmlns=\"%s\"/>",
		    xpath, namespace);
	else /* XXX shouldnt happen */
	    cprintf(cb, "<filter type=\"xpath\" select=\"%s\"/>", xpath);
    }
    cprintf(cb, "</get></rpc>");
    if ((msg = clicon_msg_encode("%s", cbuf_get(cb))) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    /* Send xml error back: first check error, then ok */
    if ((xd = xpath_first(xret, NULL, "/rpc-reply/rpc-error")) != NULL)
	xd = xml_parent(xd); /* point to rpc-reply */
    else if ((xd = xpath_first(xret, NULL, "/rpc-reply/data")) == NULL)
	if ((xd = xml_new("data", NULL, NULL)) == NULL)
	    goto done;
    if (xt){
	if (xml_rm(xd) < 0)
	    goto done;
	*xt = xd;
    }
    retval = 0;
  done:
    if (cb)
	cbuf_free(cb);
    if (xret)
	xml_free(xret);
    if (msg)
	free(msg);
    return retval;
}

/*! Close a (user) session
 * @param[in] h        CLICON handle
 * @retval    0        OK
 * @retval   -1        Error and logged to syslog
 */
int
clicon_rpc_close_session(clicon_handle h)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;

    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode("<rpc username=\"%s\"><close-session/></rpc>",
				 username?username:"")) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clicon_rpc_generate_error("Close session", xerr);
	goto done;
    }
    retval = 0;
 done:
    if (xret)
	xml_free(xret);
    if (msg)
	free(msg);
    return retval;
}

/*! Kill other user sessions
 * @param[in] h           CLICON handle
 * @param[in] session_id  Session id of other user session
 * @retval    0        OK
 * @retval   -1        Error and logged to syslog
 */
int
clicon_rpc_kill_session(clicon_handle h,
			int           session_id)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;

    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode("<rpc username=\"%s\"><kill-session><session-id>%d</session-id></kill-session></rpc>",
				 username?username:"", session_id)) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clicon_rpc_generate_error("Kill session", xerr);
	goto done;
    }
    retval = 0;
 done:
    if (xret)
	xml_free(xret);
    if (msg)
	free(msg);
    return retval;
}

/*! Send validate request to backend daemon
 * @param[in] h        CLICON handle
 * @param[in] db       Name of database
 * @retval    0        OK
 * @retval   -1        Error and logged to syslog
 */
int
clicon_rpc_validate(clicon_handle h, 
		    char         *db)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;

    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode("<rpc username=\"%s\"><validate><source><%s/></source></validate></rpc>", username?username:"", db)) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clicon_rpc_generate_error(CLIXON_ERRSTR_VALIDATE_FAILED, xerr);
	goto done;	
    }
    retval = 0;
 done:
    if (msg)
	free(msg);
    if (xret)
	xml_free(xret);
    return retval;
}

/*! Commit changes send a commit request to backend daemon
 * @param[in] h          CLICON handle
 * @retval    0        OK
 * @retval   -1        Error and logged to syslog
 */
int
clicon_rpc_commit(clicon_handle h)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;

    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode("<rpc username=\"%s\"><commit/></rpc>", username?username:"")) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clicon_rpc_generate_error(CLIXON_ERRSTR_COMMIT_FAILED, xerr);
	goto done;
    }
    retval = 0;
 done:
    if (xret)
	xml_free(xret);
    if (msg)
	free(msg);
    return retval;
}

/*! Discard all changes in candidate / revert to running
 * @param[in] h        CLICON handle
 * @retval    0        OK
 * @retval   -1        Error and logged to syslog
 */
int
clicon_rpc_discard_changes(clicon_handle h)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;

    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode("<rpc username=\"%s\"><discard-changes/></rpc>", username?username:"")) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clicon_rpc_generate_error("Discard changes", xerr);
	goto done;
    }
    retval = 0;
 done:
    if (xret)
	xml_free(xret);
    if (msg)
	free(msg);
    return retval;
}

/*! Create a new notification subscription
 * @param[in]   h        Clicon handle
 * @param{in]   stream   name of notificatio/log stream (CLICON is predefined)
 * @param{in]   filter   message filter, eg xpath for xml notifications
 * @param[out]  s0       socket returned where notification mesages will appear
 * @retval      0        OK
 * @retval      -1       Error and logged to syslog

 * @note When using netconf create-subsrciption,status and format is not supported
 */
int
clicon_rpc_create_subscription(clicon_handle    h,
			       char            *stream, 
			       char            *filter, 
			       int             *s0)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;

    username = clicon_username_get(h);
    if ((msg = clicon_msg_encode("<rpc username=\"%s\"><create-subscription xmlns=\"urn:ietf:params:xml:ns:netmod:notification\">"
				 "<stream>%s</stream>"
				 "<filter type=\"xpath\" select=\"%s\" />"
				 "</create-subscription></rpc>", 
				 username?username:"",
				 stream?stream:"", filter?filter:"")) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, s0) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clicon_rpc_generate_error("Create subscription", xerr);
	goto done;
    }
    retval = 0;
  done:
    if (xret)
	xml_free(xret);
    if (msg)
	free(msg);
    return retval;
}

/*! Send a debug request to backend server
 * @param[in] h        CLICON handle
 * @param[in] level    Debug level
 * @retval    0        OK
 * @retval   -1        Error and logged to syslog
 */
int
clicon_rpc_debug(clicon_handle h, 
		int           level)
{
    int                retval = -1;
    struct clicon_msg *msg = NULL;
    cxobj             *xret = NULL;
    cxobj             *xerr;
    char              *username;

    username = clicon_username_get(h);
    /* XXX: hardcoded example yang, should be clixon-config!!! */
    if ((msg = clicon_msg_encode("<rpc username=\"%s\"><debug xmlns=\"http://clicon.org/lib\"><level>%d</level></debug></rpc>", username?username:"", level)) == NULL)
	goto done;
    if (clicon_rpc_msg(h, msg, &xret, NULL) < 0)
	goto done;
    if ((xerr = xpath_first(xret, NULL, "//rpc-error")) != NULL){
	clicon_rpc_generate_error("Debug",xerr);
	goto done;
    }
    if (xpath_first(xret, NULL, "//rpc-reply/ok") == NULL){
	clicon_err(OE_XML, 0, "rpc error"); /* XXX extract info from rpc-error */
	goto done;
    }
    retval = 0;
 done:
    if (msg)
	free(msg);
    if (xret)
	xml_free(xret);
    return retval;
}

