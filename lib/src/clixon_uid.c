/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren
  Copyright (C) 2017-2019 Olof Hagsand
  Copyright (C) 2020-2021 Olof Hagsand and Rubicon Communications, LLC(Netgate)

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

  * uid, gid, privileges
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define _GNU_SOURCE 
#define __USE_GNU
#include <unistd.h> /* For setresuid */
#undef _GNU_SOURCE
#undef __USE_GNU
#include <stdarg.h>
#include <signal.h>
#include <syslog.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon_err.h"
#include "clixon_log.h"
#include "clixon_uid.h"

/*! Translate group name to gid. Return -1 if error or not found.
 * @param[in]   name  Name of group
 * @param[out]  gid   Group id
 * @retval  0   OK
 * @retval -1   Error. or not found
 */
int
group_name2gid(const char *name, 
	       gid_t      *gid)
{
    int           retval = -1;
    char          buf[1024]; 
    struct group  g0;
    struct group *gr = &g0;
    struct group *gtmp;
    
    gr = &g0; 
    /* This leaks memory in ubuntu */
    if (getgrnam_r(name, gr, buf, sizeof(buf), &gtmp) < 0){
	clicon_err(OE_UNIX, errno, "getgrnam_r(%s)", name);
	goto done;
    }
    if (gtmp == NULL){
	clicon_err(OE_UNIX, 0, "No such group: %s", name);
	goto done;
    }
    if (gid)
	*gid = gr->gr_gid;
    retval = 0;
 done:
    return retval;
}

/*! Translate user name to uid. Return -1 if error or not found.
 * @param[in]  name Name of user
 * @param[out] uid  User id
 * @retval     0    OK
 * @retval    -1    Error. or not found
 */
int
name2uid(const char *name,
	 uid_t      *uid)
{
    int            retval = -1;
    char           buf[1024]; 
    struct passwd  pwbuf;
    struct passwd *pwbufp = NULL;

    if (getpwnam_r(name, &pwbuf, buf, sizeof(buf), &pwbufp) != 0){
	clicon_err(OE_UNIX, errno, "getpwnam_r(%s)", name);
	goto done;
    }
    if (pwbufp == NULL){
	clicon_err(OE_UNIX, 0, "No such user: %s", name);
	goto done;
    }
    if (uid)
	*uid = pwbufp->pw_uid;
    retval = 0;
 done:
    return retval;
}

/*! Translate uid to user name
 * @param[in]  uid  User id
 * @param[out] name User name (Malloced, need to be freed)
 * @retval     0    OK
 * @retval     -1   Error. or not found
 */
int
uid2name(const uid_t uid,
	 char      **name)
{
    int            retval = -1;
    char           buf[1024]; 
    struct passwd  pwbuf = {0,};
    struct passwd *pwbufp = NULL;
    
    if (getpwuid_r(uid, &pwbuf, buf, sizeof(buf), &pwbufp) != 0){
	clicon_err(OE_UNIX, errno, "getpwuid_r(%u)", uid);
	goto done;
    }
    if (pwbufp == NULL){
	clicon_err(OE_UNIX, ENOENT, "No such user: %u", uid);
	goto done;
    }

    if (name){
	if ((*name = strdup(pwbufp->pw_name)) == NULL){
	    clicon_err(OE_UNIX, errno, "strdup");
	    goto done;
	}
    }
    retval = 0;
 done:
    return retval;
}

/* Privileges drop perm, temp and restore
 * @see https://www.usenix.org/legacy/events/sec02/full_papers/chen/chen.pdf
 */
 /*! Temporarily drop privileges 
 * @param[in]  new_uid
 */
int
drop_priv_temp(uid_t new_uid)
{
#ifdef HAVE_GETRESUID
    int retval = -1;
    
    /* XXX: implicit declaration of function 'setresuid' on travis */
    if (setresuid(-1, new_uid, geteuid()) < 0){
	clicon_err(OE_UNIX, errno, "setresuid");
	goto done;
    }
    if (geteuid() != new_uid){
	clicon_err(OE_UNIX, errno, "geteuid");
	goto done;
    }
    retval = 0;
 done:
    return retval;
#else
    clicon_debug(1, "%s Drop privileges not implemented on this platform since getresuid is not available", __FUNCTION__);
    return 0;
#endif
}

/*! Permanently drop privileges 
 * @param[in]  new_uid
 */
int
drop_priv_perm(uid_t new_uid)
{
#ifdef HAVE_GETRESUID
    int retval = -1;
    uid_t ruid;
    uid_t euid;
    uid_t suid;

    if (setresuid(new_uid, new_uid, new_uid) < 0){
	clicon_err(OE_UNIX, errno, "setresuid");
	goto done;
    }
    if (getresuid(&ruid, &euid, &suid) < 0){
	clicon_err(OE_UNIX, errno, "getresuid");
	goto done;
    }
    if (ruid != new_uid ||
	euid != new_uid ||
	suid != new_uid){
	clicon_err(OE_UNIX, EINVAL, "Non-matching uid");
	goto done;
    }
    retval = 0;
 done:
    return retval;
#else
    clicon_debug(1, "%s Drop privileges not implemented on this platform since getresuid is not available", __FUNCTION__);
    return 0;
#endif
}

/*! Restore privileges to saved level */
int
restore_priv(void)
{
#ifdef HAVE_GETRESUID
    int retval = -1;
    uid_t ruid;
    uid_t euid;
    uid_t suid;
    
    if (getresuid(&ruid, &euid, &suid) < 0){
	clicon_err(OE_UNIX, errno, "setresuid");
	goto done;
    }
    if (setresuid(-1, suid, -1) < 0){
	clicon_err(OE_UNIX, errno, "setresuid");
	goto done;
    }
    if (geteuid() != suid){
	clicon_err(OE_UNIX, EINVAL, "Non-matching uid");
	goto done;
    }
    retval = 0;
 done:
    return retval;
#else
    clicon_debug(1, "%s Drop privileges not implemented on this platform since getresuid is not available", __FUNCTION__);
    return 0;
#endif
}
