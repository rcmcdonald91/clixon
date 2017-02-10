/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2017 Olof Hagsand and Benny Holmgren

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
 * The exported interface to plugins. External apps (eg frontend netconf plugins)
 * should only include this file (not the netconf_*.h)
 */

#ifndef _CLIXON_NETCONF_H_
#define _CLIXON_NETCONF_H_

/*
 * Types
 */
typedef int (*netconf_cb_t)(
    clicon_handle h, 
    cxobj *xorig, /* Original request. */
    cxobj *xn,    /* Sub-tree (under xorig) at child: <rpc><xn></rpc> */
    cbuf  *cb,		    /* Output xml stream. For reply */
    cbuf  *cb_err,	    /* Error xml stream. For error reply */
    void  *arg               /* Argument given at netconf_register_callback() */
    );  

/*
 * Prototypes
 * (Duplicated. Also in netconf_*.h)
 */
int netconf_output(int s, cbuf *xf, char *msg);

int netconf_create_rpc_reply(cbuf *cb,            /* msg buffer */
			 cxobj *xr, /* orig request */
			 char *body,
			 int ok);

int netconf_register_callback(clicon_handle h,
			      netconf_cb_t cb,   /* Callback called */
			      void *arg,       /* Arg to send to callback */
			      char *tag);      /* Xml tag when callback is made */
int netconf_create_rpc_error(cbuf *xf,            /* msg buffer */
			     cxobj *xr, /* orig request */
			     char *tag, 
			     char *type,
			     char *severity, 
			     char *message, 
			     char *info);

void netconf_ok_set(int ok);
int netconf_ok_get(void);

int netconf_xpath(cxobj *xsearch,
		  cxobj *xfilter, 
		   cbuf *xf, cbuf *xf_err, 
		  cxobj *xt);


#endif /* _CLIXON_NETCONF_H_ */