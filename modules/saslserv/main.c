/*
 * Copyright (c) 2005 Atheme Development Group
 * Rights to this code are documented in doc/LICENSE.
 *
 * This file contains the main() routine.
 */

#include "atheme.h"
#include "uplink.h"

struct sasl_sourceinfo
{
	sourceinfo_t             parent;
	struct sasl_session     *sess;
};

static mowgli_list_t sessions;
static mowgli_list_t mechanisms;
static char mechlist_string[SASL_S2S_MAXLEN];
static bool hide_server_names;

static service_t *saslsvs = NULL;
static mowgli_eventloop_timer_t *delete_stale_timer = NULL;

static const char *
sasl_format_sourceinfo(sourceinfo_t *const restrict si, const bool full)
{
	static char result[BUFSIZE];

	const struct sasl_sourceinfo *const ssi = (const struct sasl_sourceinfo *) si;

	if (full)
		(void) snprintf(result, sizeof result, "SASL/%s:%s[%s]:%s",
		                ssi->sess->uid ? ssi->sess->uid : "?",
		                ssi->sess->host ? ssi->sess->host : "?",
		                ssi->sess->ip ? ssi->sess->ip : "?",
		                ssi->sess->server ? ssi->sess->server->name : "?");
	else
		(void) snprintf(result, sizeof result, "SASL(%s)",
		                ssi->sess->host ? ssi->sess->host : "?");

	return result;
}

static const char *
sasl_get_source_name(sourceinfo_t *const restrict si)
{
	static char result[HOSTLEN + NICKLEN + 10];
	char description[BUFSIZE];

	const struct sasl_sourceinfo *const ssi = (const struct sasl_sourceinfo *) si;

	if (ssi->sess->server && ! hide_server_names)
		(void) snprintf(description, sizeof description, "Unknown user on %s (via SASL)",
		                                                 ssi->sess->server->name);
	else
		(void) mowgli_strlcpy(description, "Unknown user (via SASL)", sizeof description);

	/* we can reasonably assume that si->v is non-null as this is part of the SASL vtable */
	if (si->sourcedesc)
		(void) snprintf(result, sizeof result, "<%s:%s>%s", description, si->sourcedesc,
		                si->smu ? entity(si->smu)->name : "");
	else
		(void) snprintf(result, sizeof result, "<%s>%s", description, si->smu ? entity(si->smu)->name : "");

	return result;
}

static struct sourceinfo_vtable sasl_vtable = {

	.description        = "SASL",
	.format             = sasl_format_sourceinfo,
	.get_source_name    = sasl_get_source_name,
	.get_source_mask    = sasl_get_source_name,
};

static void
sasl_sourceinfo_recreate(struct sasl_session *const restrict p)
{
	if (p->si)
		(void) object_unref(p->si);

	struct sasl_sourceinfo *const ssi = smalloc(sizeof *ssi);

	(void) object_init(object(ssi), "<sasl sourceinfo>", &free);

	ssi->parent.s = p->server;
	ssi->parent.connection = curr_uplink->conn;

	if (p->host)
		ssi->parent.sourcedesc = p->host;

	ssi->parent.service = saslsvs;
	ssi->parent.v = &sasl_vtable;

	ssi->parent.force_language = language_find("en");
	ssi->sess = p;

	p->si = &ssi->parent;
}

/* find an existing session by uid */
static struct sasl_session *
find_session(const char *const restrict uid)
{
	if (! uid)
		return NULL;

	mowgli_node_t *n;

	MOWGLI_ITER_FOREACH(n, sessions.head)
	{
		struct sasl_session *const p = n->data;

		if (p->uid && strcmp(p->uid, uid) == 0)
			return p;
	}

	return NULL;
}

/* create a new session if it does not already exist */
static struct sasl_session *
make_session(const char *const restrict uid, server_t *const restrict server)
{
	struct sasl_session *p;

	if (! (p = find_session(uid)))
	{
		p = smalloc(sizeof *p);
		(void) memset(p, 0x00, sizeof *p);

		p->uid = sstrdup(uid);
		p->server = server;

		mowgli_node_t *const n = mowgli_node_create();
		(void) mowgli_node_add(p, n, &sessions);
	}

	return p;
}

/* free a session and all its contents */
static void
destroy_session(struct sasl_session *const restrict p)
{
	if (p->flags & ASASL_NEED_LOG && p->authceid)
	{
		myuser_t *const mu = myuser_find_uid(p->authceid);

		if (mu && ! (ircd->flags & IRCD_SASL_USE_PUID))
			(void) logcommand(p->si, CMDLOG_LOGIN, "LOGIN (session timed out)");
	}

	mowgli_node_t *n, *tn;

	MOWGLI_ITER_FOREACH_SAFE(n, tn, sessions.head)
	{
		if (n->data == p)
		{
			(void) mowgli_node_delete(n, &sessions);
			(void) mowgli_node_free(n);
		}
	}

	if (p->mechptr && p->mechptr->mech_finish)
		(void) p->mechptr->mech_finish(p);

	if (p->si)
		(void) object_unref(p->si);

	(void) free(p->authceid);
	(void) free(p->authzeid);
	(void) free(p->certfp);
	(void) free(p->host);
	(void) free(p->buf);
	(void) free(p->uid);
	(void) free(p->ip);
	(void) free(p);
}

/* find a mechanism by name */
static struct sasl_mechanism *
find_mechanism(const char *const restrict name)
{
	mowgli_node_t *n;

	MOWGLI_ITER_FOREACH(n, mechanisms.head)
	{
		struct sasl_mechanism *const mptr = n->data;

		if (strcmp(mptr->name, name) == 0)
			return mptr;
	}

	(void) slog(LG_DEBUG, "find_mechanism(): cannot find mechanism '%s'!", name);

	return NULL;
}

static void
sasl_server_eob(server_t __attribute__((unused)) *const restrict s)
{
	/* new server online, push mechlist to make sure it's using the current one */
	(void) sasl_mechlist_sts(mechlist_string);
}

static void
mechlist_build_string(char *restrict ptr, const size_t buflen)
{
	size_t l = 0;
	mowgli_node_t *n;

	MOWGLI_ITER_FOREACH(n, mechanisms.head)
	{
		struct sasl_mechanism *const mptr = n->data;

		if (l + strlen(mptr->name) > buflen)
			break;

		(void) strcpy(ptr, mptr->name);

		ptr += strlen(mptr->name);
		*ptr++ = ',';
		l += strlen(mptr->name) + 1;
	}

	if (l)
		ptr--;

	*ptr = 0x00;
}

static void
mechlist_do_rebuild(void)
{
	(void) mechlist_build_string(mechlist_string, sizeof mechlist_string);

	/* push mechanism list to the network */
	if (me.connected)
		(void) sasl_mechlist_sts(mechlist_string);
}

static bool
may_impersonate(myuser_t *const source_mu, myuser_t *const target_mu)
{
	/* Allow same (although this function won't get called in that case anyway) */
	if (source_mu == target_mu)
		return true;

	char priv[BUFSIZE] = PRIV_IMPERSONATE_ANY;

	/* Check for wildcard priv */
	if (has_priv_myuser(source_mu, priv))
		return true;

	/* Check for target-operclass specific priv */
	const char *const classname = (target_mu->soper && target_mu->soper->classname) ?
	                                  target_mu->soper->classname : "user";
	(void) snprintf(priv, sizeof priv, PRIV_IMPERSONATE_CLASS_FMT, classname);
	if (has_priv_myuser(source_mu, priv))
		return true;

	/* Check for target-entity specific priv */
	(void) snprintf(priv, sizeof priv, PRIV_IMPERSONATE_ENTITY_FMT, entity(target_mu)->name);
	if (has_priv_myuser(source_mu, priv))
		return true;

	/* Allow modules to check too */
	hook_sasl_may_impersonate_t req = {

		.source_mu = source_mu,
		.target_mu = target_mu,
		.allowed = false,
	};

	(void) hook_call_sasl_may_impersonate(&req);

	return req.allowed;
}

/* authenticated, now double check that their account is ok for login */
static myuser_t *
login_user(struct sasl_session *const restrict p)
{
	/* source_mu is the user whose credentials we verified ("authentication id") */
	/* target_mu is the user who will be ultimately logged in ("authorization id") */

	myuser_t *const source_mu = myuser_find_uid(p->authceid);
	if (! source_mu)
		return NULL;

	myuser_t *target_mu = source_mu;

	if (p->authzeid && *p->authzeid)
	{
		if (! (target_mu = myuser_find_uid(p->authzeid)))
			return NULL;
	}
	else
	{
		(void) free(p->authzeid);
		p->authzeid = sstrdup(p->authceid);
	}

	if (metadata_find(source_mu, "private:freeze:freezer"))
	{
		(void) logcommand(p->si, CMDLOG_LOGIN, "failed LOGIN to \2%s\2 (frozen)", entity(source_mu)->name);
		return NULL;
	}

	if (target_mu != source_mu)
	{
		if (! may_impersonate(source_mu, target_mu))
		{
			(void) logcommand(p->si, CMDLOG_LOGIN, "denied IMPERSONATE by \2%s\2 to \2%s\2",
			                                       entity(source_mu)->name, entity(target_mu)->name);
			return NULL;
		}

		if (metadata_find(target_mu, "private:freeze:freezer"))
		{
			(void) logcommand(p->si, CMDLOG_LOGIN, "failed LOGIN to \2%s\2 (frozen)",
			                                       entity(target_mu)->name);
			return NULL;
		}
	}

	if (MOWGLI_LIST_LENGTH(&target_mu->logins) >= me.maxlogins)
	{
		(void) logcommand(p->si, CMDLOG_LOGIN, "failed LOGIN to \2%s\2 (too many logins)",
		                                       entity(target_mu)->name);
		return NULL;
	}

	/* Log it with the full n!u@h later */
	p->flags |= ASASL_NEED_LOG;

	/* We just did SASL authentication for a user.  With IRCds which do not
	 * have unique UIDs for users, we will likely be expecting the login
	 * data to be bursted.  As a result, we should give the core a heads'
	 * up that this is going to happen so that hooks will be properly
	 * fired...
	 */
	if (ircd->flags & IRCD_SASL_USE_PUID)
	{
		target_mu->flags &= ~MU_NOBURSTLOGIN;
		target_mu->flags |= MU_PENDINGLOGIN;
	}

	if (target_mu != source_mu)
		(void) logcommand(p->si, CMDLOG_LOGIN, "allowed IMPERSONATE by \2%s\2 to \2%s\2",
		                                       entity(source_mu)->name, entity(target_mu)->name);

	return target_mu;
}

/* output an arbitrary amount of data to the SASL client */
static void
sasl_write(char *const restrict target, const char *restrict data, const size_t length)
{
	size_t last = SASL_S2S_MAXLEN;
	size_t rem = length;

	while (rem)
	{
		const size_t nbytes = (rem > SASL_S2S_MAXLEN) ? SASL_S2S_MAXLEN : rem;

		char out[SASL_S2S_MAXLEN + 1];
		(void) memset(out, 0x00, sizeof out);
		(void) memcpy(out, data, nbytes);
		(void) sasl_sts(target, 'C', out);

		data += nbytes;
		rem -= nbytes;
		last = nbytes;
	}

	/* The end of a packet is indicated by a string not of the maximum length. If last piece was the
	 * maximum length, or if there was no data at all, send an empty string to finish the transaction.
	 */
	if (last == SASL_S2S_MAXLEN)
		(void) sasl_sts(target, 'C', "+");
}

/* abort an SASL session
 */
static inline void
sasl_session_abort(struct sasl_session *const restrict p)
{
	(void) sasl_sts(p->uid, 'D', "F");
	(void) destroy_session(p);
}

/* given an entire sasl message, advance session by passing data to mechanism
 * and feeding returned data back to client.
 */
static void
sasl_packet(struct sasl_session *const restrict p, const char *const restrict buf, const size_t len)
{
	int rc;

	void *out = NULL;
	size_t out_len = 0;

	/* First piece of data in a session is the name of
	 * the SASL mechanism that will be used.
	 */
	if (! p->mechptr)
	{
		(void) sasl_sourceinfo_recreate(p);

		if (! (p->mechptr = find_mechanism(buf)))
		{
			(void) sasl_sts(p->uid, 'M', mechlist_string);
			(void) sasl_session_abort(p);
			return;
		}

		if (p->mechptr->mech_start)
			rc = p->mechptr->mech_start(p, &out, &out_len);
		else
			rc = ASASL_MORE;
	}
	else
	{
		unsigned char inbuf[SASL_C2S_MAXLEN];
		size_t tlen = 0;

		if (len == 1 && *buf == '+')
			rc = p->mechptr->mech_step(p, NULL, 0, &out, &out_len);
		else if ((tlen = base64_decode(buf, inbuf, sizeof inbuf)) && tlen != (size_t) -1)
			rc = p->mechptr->mech_step(p, inbuf, tlen, &out, &out_len);
		else
			rc = ASASL_FAIL;

		if (tlen == (size_t) -1)
			(void) slog(LG_ERROR, "%s: base64_decode() failed", __func__);
	}

	/* Some progress has been made, reset timeout. */
	p->flags &= ~ASASL_MARKED_FOR_DELETION;

	if (rc == ASASL_DONE)
	{
		myuser_t *const mu = login_user(p);

		if (mu)
		{
			char *cloak = "*";
			metadata_t *md;

			if ((md = metadata_find(mu, "private:usercloak")))
				cloak = md->value;

			if (! (mu->flags & MU_WAITAUTH))
				(void) svslogin_sts(p->uid, "*", "*", cloak, mu);

			(void) sasl_sts(p->uid, 'D', "S");
			/* Will destroy session on introduction of user to net. */
		}
		else
			(void) sasl_session_abort(p);

		return;
	}

	if (rc == ASASL_MORE)
	{
		if (out && out_len)
		{
			char outbuf[SASL_C2S_MAXLEN * 2];
			const size_t rs = base64_encode(out, out_len, outbuf, sizeof outbuf);

			if (rs == (size_t) -1)
			{
				(void) slog(LG_ERROR, "%s: base64_encode() failed", __func__);
				(void) sasl_session_abort(p);
			}
			else
				(void) sasl_write(p->uid, outbuf, rs);
		}
		else
			(void) sasl_sts(p->uid, 'C', "+");

		(void) free(out);
		return;
	}

	/* We might have more information to construct a more accurate sourceinfo now?
	 * TODO: Investigate whether this is necessary
	 */
	(void) sasl_sourceinfo_recreate(p);

	/* If we reach this, they failed SASL auth, so if they were trying
	 * to identify as a specific user, bad_password them.
	 */
	if (p->authceid)
	{
		myuser_t *const mu = myuser_find_uid(p->authceid);

		if (mu)
		{
			(void) logcommand(p->si, CMDLOG_LOGIN, "failed LOGIN (%s) to \2%s\2 (bad password)",
			                                       p->mechptr->name, entity(mu)->name);
			(void) bad_password(p->si, mu);
		}
	}

	(void) free(out);
	(void) sasl_session_abort(p);
}

/* interpret an AUTHENTICATE message */
static void
sasl_input(sasl_message_t *const restrict smsg)
{
	struct sasl_session *const p = make_session(smsg->uid, smsg->server);

	size_t len = strlen(smsg->parv[0]);

	switch(smsg->mode)
	{
	case 'H':
		/* (H)ost information */
		p->host = sstrdup(smsg->parv[0]);
		p->ip   = sstrdup(smsg->parv[1]);

		if (smsg->parc >= 3 && strcmp(smsg->parv[2], "P") != 0)
			p->tls = true;

		return;

	case 'S':
		/* (S)tart authentication */
		if (strcmp(smsg->parv[0], "EXTERNAL") == 0)
		{
			if (smsg->parc < 2)
			{
				(void) slog(LG_DEBUG, "%s: client %s starting EXTERNAL authentication without a "
				                      "fingerprint", __func__, smsg->uid);

				(void) sasl_session_abort(p);
				return;
			}

			(void) free(p->certfp);

			p->certfp = sstrdup(smsg->parv[1]);
			p->tls = true;
		}

		(void) sasl_packet(p, smsg->parv[0], 0);
		return;

	case 'C':
		/* (C)lient data */
		if (p->len + len >= SASL_C2S_MAXLEN)
		{
			(void) sasl_session_abort(p);
			return;
		}

		p->buf = srealloc(p->buf, p->len + len + 1);
		(void) memcpy(p->buf + p->len, smsg->parv[0], len);
		p->len += len;

		/* Messages not exactly 400 bytes are the end of a packet. */
		if (len < 400)
		{
			p->buf[p->len] = 0x00;

			(void) sasl_packet(p, p->buf, p->len);
			(void) free(p->buf);

			p->buf = NULL;
			p->len = 0;
		}

		return;

	case 'D':
		/* (D)one -- when we receive it, means client abort */
		(void) destroy_session(p);
		return;
	}
}

/* clean up after a user who is finally on the net */
static void
sasl_newuser(hook_user_nick_t *const restrict data)
{
	/* If the user has been killed, don't do anything. */
	user_t *const u = data->u;
	if (! u)
		return;

	/* Not concerned unless it's a SASL login. */
	struct sasl_session *const p = find_session(u->uid);
	if (! p)
		return;

	/* We will log it ourselves, if needed */
	p->flags &= ~ASASL_NEED_LOG;

	/* Find the account */
	myuser_t *const mu = p->authzeid ? myuser_find_uid(p->authzeid) : NULL;
	if (! mu)
	{
		(void) notice(saslsvs->nick, u->nick, "Account %s dropped, login cancelled",
		                                      p->authzeid ? p->authzeid : "??");
		(void) destroy_session(p);

		/* We'll remove their ircd login in handle_burstlogin() */
		return;
	}

	struct sasl_mechanism *const mptr = p->mechptr;

	(void) destroy_session(p);
	(void) myuser_login(saslsvs, u, mu, false);
	(void) logcommand_user(saslsvs, u, CMDLOG_LOGIN, "LOGIN (%s)", mptr->name);
}

/* This function is run approximately once every 30 seconds.
 * It looks for flagged sessions, and deletes them, while
 * flagging all the others. This way stale sessions are deleted
 * after no more than 60 seconds.
 */
static void
delete_stale(void __attribute__((unused)) *const restrict vptr)
{
	mowgli_node_t *n, *tn;

	MOWGLI_ITER_FOREACH_SAFE(n, tn, sessions.head)
	{
		struct sasl_session *const p = n->data;

		if (p->flags & ASASL_MARKED_FOR_DELETION)
		{
			(void) mowgli_node_delete(n, &sessions);
			(void) destroy_session(p);
			(void) mowgli_node_free(n);
		}
		else
			p->flags |= ASASL_MARKED_FOR_DELETION;
	}
}

static void
sasl_mech_register(struct sasl_mechanism *const restrict mech)
{
	mowgli_node_t *const node = mowgli_node_create();

	(void) slog(LG_DEBUG, "sasl_mech_register(): registering %s", mech->name);
	(void) mowgli_node_add(mech, node, &mechanisms);
	(void) mechlist_do_rebuild();
}

static void
sasl_mech_unregister(struct sasl_mechanism *const restrict mech)
{
	(void) slog(LG_DEBUG, "sasl_mech_unregister(): unregistering %s", mech->name);

	mowgli_node_t *n, *tn;

	MOWGLI_ITER_FOREACH_SAFE(n, tn, sessions.head)
	{
		struct sasl_session *const session = n->data;

		if (session->mechptr == mech)
		{
			(void) slog(LG_DEBUG, "sasl_mech_unregister(): destroying session %s", session->uid);
			(void) destroy_session(session);
		}
	}
	MOWGLI_ITER_FOREACH_SAFE(n, tn, mechanisms.head)
	{
		if (n->data == mech)
		{
			(void) mowgli_node_delete(n, &mechanisms);
			(void) mowgli_node_free(n);
			(void) mechlist_do_rebuild();

			break;
		}
	}
}

static bool
sasl_authcid_can_login(struct sasl_session *const restrict p, const char *const restrict authcid,
                       myuser_t **const restrict muo)
{
	myuser_t *const mu = myuser_find_by_nick(authcid);

	if (! mu)
		return false;

	if (muo)
		*muo = mu;

	p->authceid = sstrdup(entity(mu)->id);

	if (p->authzeid && strcmp(p->authceid, p->authzeid) == 0)
		// authzid_can_login already ran the hook for this user
		return true;

	hook_user_login_check_t req = {

		.si         = p->si,
		.mu         = mu,
		.allowed    = true,
	};

	(void) hook_call_user_can_login(&req);

	if (! req.allowed)
		(void) logcommand(p->si, CMDLOG_LOGIN, "failed LOGIN to \2%s\2 (denied by hook)", entity(mu)->name);

	return req.allowed;
}

static bool
sasl_authzid_can_login(struct sasl_session *const restrict p, const char *const restrict authzid,
                       myuser_t **const restrict muo)
{
	myuser_t *const mu = myuser_find_by_nick(authzid);

	if (! mu)
		return false;

	if (muo)
		*muo = mu;

	p->authzeid = sstrdup(entity(mu)->id);

	if (p->authceid && strcmp(p->authceid, p->authzeid) == 0)
		// authcid_can_login already ran the hook for this user
		return true;

	hook_user_login_check_t req = {

		.si         = p->si,
		.mu         = mu,
		.allowed    = true,
	};

	(void) hook_call_user_can_login(&req);

	if (! req.allowed)
		(void) logcommand(p->si, CMDLOG_LOGIN, "failed LOGIN to \2%s\2 (denied by hook)", entity(mu)->name);

	return req.allowed;
}

/* main services client routine */
static void
saslserv(sourceinfo_t *const restrict si, const int parc, char **const restrict parv)
{
	/* this should never happen */
	if (parv[0][0] == '&')
	{
		(void) slog(LG_ERROR, "services(): got parv with local channel: %s", parv[0]);
		return;
	}

	/* make a copy of the original for debugging */
	char orig[BUFSIZE];
	(void) mowgli_strlcpy(orig, parv[parc - 1], sizeof orig);

	/* lets go through this to get the command */
	char *const cmd = strtok(parv[parc - 1], " ");
	char *const text = strtok(NULL, "");

	if (! cmd)
		return;

	if (*orig == '\001')
	{
		(void) handle_ctcp_common(si, cmd, text);
		return;
	}

	(void) command_fail(si, fault_noprivs, "This service exists to identify connecting clients to the network. "
	                                       "It has no public interface.");
}

static void
mod_init(module_t __attribute__((unused)) *const restrict m)
{
	(void) hook_add_event("sasl_input");
	(void) hook_add_sasl_input(&sasl_input);
	(void) hook_add_event("user_add");
	(void) hook_add_user_add(&sasl_newuser);
	(void) hook_add_event("server_eob");
	(void) hook_add_server_eob(&sasl_server_eob);
	(void) hook_add_event("sasl_may_impersonate");
	(void) hook_add_event("user_can_login");

	delete_stale_timer = mowgli_timer_add(base_eventloop, "sasl_delete_stale", &delete_stale, NULL, 30);
	saslsvs = service_add("saslserv", &saslserv);
	authservice_loaded++;

	(void) add_bool_conf_item("HIDE_SERVER_NAMES", &saslsvs->conf_table, 0, &hide_server_names, false);
}

static void
mod_deinit(const module_unload_intent_t __attribute__((unused)) intent)
{
	(void) hook_del_sasl_input(&sasl_input);
	(void) hook_del_user_add(&sasl_newuser);
	(void) hook_del_server_eob(&sasl_server_eob);

	(void) mowgli_timer_destroy(base_eventloop, delete_stale_timer);

	(void) del_conf_item("HIDE_SERVER_NAMES", &saslsvs->conf_table);

        if (saslsvs)
		(void) service_delete(saslsvs);

	authservice_loaded--;

	if (sessions.head)
		(void) slog(LG_ERROR, "saslserv/main: shutting down with a non-empty session list; "
		                      "a mechanism did not unregister itself! (BUG)");
}

/* This structure is imported by SASL mechanism modules */
const struct sasl_core_functions sasl_core_functions = {

	.mech_register      = &sasl_mech_register,
	.mech_unregister    = &sasl_mech_unregister,
	.authcid_can_login  = &sasl_authcid_can_login,
	.authzid_can_login  = &sasl_authzid_can_login,
};

SIMPLE_DECLARE_MODULE_V1("saslserv/main", MODULE_UNLOAD_CAPABILITY_OK)
