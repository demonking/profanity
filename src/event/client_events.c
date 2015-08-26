/*
 * client_events.c
 *
 * Copyright (C) 2012 - 2015 James Booth <boothj5@gmail.com>
 *
 * This file is part of Profanity.
 *
 * Profanity is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Profanity is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Profanity.  If not, see <http://www.gnu.org/licenses/>.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link the code of portions of this program with the OpenSSL library under
 * certain conditions as described in each individual source file, and
 * distribute linked combinations including the two.
 *
 * You must obey the GNU General Public License in all respects for all of the
 * code used other than OpenSSL. If you modify file(s) with this exception, you
 * may extend this exception to your version of the file(s), but you are not
 * obligated to do so. If you do not wish to do so, delete this exception
 * statement from your version. If you delete this exception statement from all
 * source files in the program, then also delete it here.
 *
 */

#include <stdlib.h>
#include <glib.h>

#include "config.h"
#include "log.h"
#include "ui/ui.h"
#include "window_list.h"
#include "xmpp/xmpp.h"
#ifdef HAVE_LIBOTR
#include "otr/otr.h"
#endif
#ifdef HAVE_LIBGPGME
#include "pgp/gpg.h"
#endif

jabber_conn_status_t
cl_ev_connect_jid(const char * const jid, const char * const passwd, const char * const altdomain, const int port)
{
    cons_show("Connecting as %s", jid);
    return jabber_connect_with_details(jid, passwd, altdomain, port);
}

jabber_conn_status_t
cl_ev_connect_account(ProfAccount *account)
{
    char *jid = account_create_full_jid(account);
    cons_show("Connecting with account %s as %s", account->name, jid);
    free(jid);

    return jabber_connect_with_account(account);
}

void
cl_ev_presence_send(const resource_presence_t presence_type, const char * const msg, const int idle)
{
    char *signed_status = NULL;

#ifdef HAVE_LIBGPGME
    char *account_name = jabber_get_account_name();
    ProfAccount *account = accounts_get_account(account_name);
    if (account->pgp_keyid) {
        signed_status = p_gpg_sign(msg, account->pgp_keyid);
    }
    account_free(account);
#endif

    presence_send(presence_type, msg, idle, signed_status);

    free(signed_status);
}

void
cl_ev_send_msg(ProfChatWin *chatwin, const char * const msg)
{
    chat_state_active(chatwin->state);

// OTR suported, PGP supported
#ifdef HAVE_LIBOTR
#ifdef HAVE_LIBGPGME
    prof_enc_t enc_mode = chatwin->enc_mode;
    if (enc_mode == PROF_ENC_NONE || enc_mode == PROF_ENC_OTR) {
        gboolean handled = otr_on_message_send(chatwin, msg);
        if (!handled) {
            char *id = message_send_chat(chatwin->barejid, msg);
            chat_log_msg_out(chatwin->barejid, msg);
            ui_outgoing_chat_msg(chatwin, msg, id, PROF_ENC_NONE);
            free(id);
        }
    } else { // enc_mode = PROF_ENC_PGP
        char *id = message_send_chat_pgp(chatwin->barejid, msg);
        chat_log_pgp_msg_out(chatwin->barejid, msg);
        ui_outgoing_chat_msg(chatwin, msg, id, PROF_ENC_PGP);
        free(id);
    }
    return;
#endif
#endif

// OTR supported, PGP unsupported
#ifdef HAVE_LIBOTR
#ifndef HAVE_LIBGPGME
    gboolean handled = otr_on_message_send(chatwin, msg);
    if (!handled) {
        char *id = message_send_chat(chatwin->barejid, msg);
        chat_log_msg_out(chatwin->barejid, msg);
        ui_outgoing_chat_msg(chatwin, msg, id, PROF_ENC_NONE);
        free(id);
    }
    return;
#endif
#endif

// OTR unsupported, PGP supported
#ifndef HAVE_LIBOTR
#ifdef HAVE_LIBGPGME
    prof_enc_t enc_mode = chatwin->enc_mode;
    if (enc_mode == PROF_ENC_NONE) {
        char *id = message_send_chat(chatwin->barejid, msg);
        chat_log_msg_out(chatwin->barejid, msg);
        ui_outgoing_chat_msg(chatwin, msg, id, PROF_ENC_NONE);
        free(id);
    } else if (enc_mode == PROF_ENC_PGP) {
        char *id = message_send_chat_pgp(chatwin->barejid, msg);
        chat_log_pgp_msg_out(chatwin->barejid, msg);
        ui_outgoing_chat_msg(chatwin, msg, id, PROF_ENC_PGP);
        free(id);
    }
    return;
#endif
#endif

// OTR unsupported, PGP unsupported
#ifndef HAVE_LIBOTR
#ifndef HAVE_LIBGPGME
    char *id = message_send_chat(chatwin->barejid, msg);
    chat_log_msg_out(chatwin->barejid, msg);
    ui_outgoing_chat_msg(chatwin, msg, id, PROF_ENC_NONE;
    free(id);
    return;
#endif
#endif
}

void
cl_ev_send_muc_msg(ProfMucWin *mucwin, const char * const msg)
{
    message_send_groupchat(mucwin->roomjid, msg);
}

void
cl_ev_send_priv_msg(ProfPrivateWin *privwin, const char * const msg)
{
    message_send_private(privwin->fulljid, msg);
    ui_outgoing_private_msg(privwin, msg);
}
