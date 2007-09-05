/*
 * atheme-services: A collection of minimalist IRC services   
 * help.c: Help system implementation.
 *
 * Copyright (c) 2005-2007 Atheme Project (http://www.atheme.org)           
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "atheme.h"

static helpentry_t *help_cmd_find(sourceinfo_t *si, char *cmd, list_t *list)
{
	node_t *n;
	helpentry_t *c;

	LIST_FOREACH(n, list->head)
	{
		c = n->data;

		if (!strcasecmp(c->name, cmd))
			return c;
	}

	command_fail(si, fault_nosuch_target, _("No help available for \2%s\2."), cmd);
	return NULL;
}

void help_display(sourceinfo_t *si, char *command, list_t *list)
{
	helpentry_t *c;
	FILE *help_file;
	char buf[BUFSIZE];

	/* take the command through the hash table */
	if ((c = help_cmd_find(si, command, list)))
	{
		if (c->file)
		{
			if (*c->file == '/')
				help_file = fopen(c->file, "r");
			else
			{
				snprintf(buf, sizeof buf, "%s/%s", SHAREDIR, c->file);
				if (nicksvs.no_nick_ownership && !strncmp(c->file, "help/nickserv/", 14))
					memcpy(buf + (sizeof(SHAREDIR) - 1) + 6, "userserv", 8);
				help_file = fopen(buf, "r");
			}

			if (!help_file)
			{
				command_fail(si, fault_nosuch_target, _("Could not get help file for \2%s\2."), command);
				return;
			}

			command_success_nodata(si, _("***** \2%s Help\2 *****"), si->service->name);

			while (fgets(buf, BUFSIZE, help_file))
			{
				strip(buf);

				replace(buf, sizeof(buf), "&nick&", si->service->disp);

				if (buf[0])
					command_success_nodata(si, "%s", buf);
				else
					command_success_nodata(si, " ");
			}

			fclose(help_file);

			command_success_nodata(si, _("***** \2End of Help\2 *****"));
		}
		else if (c->func)
		{
			command_success_nodata(si, _("***** \2%s Help\2 *****"), si->service->name);

			c->func(si);

			command_success_nodata(si, _("***** \2End of Help\2 *****"));
		}
		else
			command_fail(si, fault_nosuch_target, _("No help available for \2%s\2."), command);
	}
}

void help_addentry(list_t *list, char *topic, char *fname,
	void (*func)(sourceinfo_t *si))
{
	helpentry_t *he = smalloc(sizeof(helpentry_t));
	node_t *n;

	memset(he, 0, sizeof(helpentry_t));

	if (!list && !topic && !func && !fname)
	{
		slog(LG_DEBUG, "help_addentry(): invalid params");
		return;
	}

	/* further paranoia */
	if (!func && !fname)
	{
		slog(LG_DEBUG, "help_addentry(): invalid params");
		return;
	}

	he->name = sstrdup(topic);

	if (func != NULL)
		he->func = func;
	else if (fname != NULL)
		he->file = sstrdup(fname);

	n = node_create();

	node_add(he, n, list);
}

void help_delentry(list_t *list, char *name)
{
	node_t *n, *tn;
	helpentry_t *he;

	LIST_FOREACH_SAFE(n, tn, list->head)
	{
		he = n->data;

		if (!strcasecmp(he->name, name))
		{
			free(he->name);

			if (he->file != NULL)
				free(he->file);

			he->func = NULL;
			free(he);

			node_del(n, list);
			node_free(n);
		}
	}
}

/* vim:cinoptions=>s,e0,n0,f0,{0,}0,^0,=s,ps,t0,c3,+s,(2s,us,)20,*30,gs,hs
 * vim:ts=8
 * vim:sw=8
 * vim:noexpandtab
 */
