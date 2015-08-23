/*
 * server_events.c
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

#include <string.h>
#include <stdlib.h>

#include "config.h"

#include "chat_session.h"
#include "log.h"
#include "muc.h"
#include "config/preferences.h"
#include "config/account.h"
#include "roster_list.h"
#include "window_list.h"

#ifdef HAVE_LIBOTR
#include "otr/otr.h"
#endif
#ifdef HAVE_LIBGPGME
#include "pgp/gpg.h"
#endif

#include "ui/ui.h"

void
sv_ev_login_account_success(char *account_name)
{
    ProfAccount *account = accounts_get_account(account_name);

#ifdef HAVE_LIBOTR
    otr_on_connect(account);
#endif

#ifdef HAVE_LIBGPGME
    p_gpg_on_connect(account->jid);
#endif

    ui_handle_login_account_success(account);

    // attempt to rejoin rooms with passwords
    GList *curr = muc_rooms();
    while (curr) {
        char *password = muc_password(curr->data);
        if (password) {
            char *nick = muc_nick(curr->data);
            presence_join_room(curr->data, nick, password);
        }
        curr = g_list_next(curr);
    }
    g_list_free(curr);

    log_info("%s logged in successfully", account->jid);
    account_free(account);
}

void
sv_ev_roster_received(void)
{
    if (prefs_get_boolean(PREF_ROSTER)) {
        ui_show_roster();
    }
}

void
sv_ev_lost_connection(void)
{
    cons_show_error("Lost connection.");
    roster_clear();
    muc_invites_clear();
    chat_sessions_clear();
    ui_disconnected();
#ifdef HAVE_LIBGPGME
    p_gpg_on_disconnect();
#endif
}

void
sv_ev_failed_login(void)
{
    cons_show_error("Login failed.");
    log_info("Login failed");
}

void
sv_ev_room_invite(jabber_invite_t invite_type,
    const char * const invitor, const char * const room,
    const char * const reason, const char * const password)
{
    if (!muc_active(room) && !muc_invites_contain(room)) {
        cons_show_room_invite(invitor, room, reason);
        muc_invites_add(room, password);
    }
}

void
sv_ev_room_broadcast(const char *const room_jid,
    const char * const message)
{
    if (muc_roster_complete(room_jid)) {
        ui_room_broadcast(room_jid, message);
    } else {
        muc_pending_broadcasts_add(room_jid, message);
    }
}

void
sv_ev_room_subject(const char * const room, const char * const nick, const char * const subject)
{
    muc_set_subject(room, subject);
    if (muc_roster_complete(room)) {
        ui_room_subject(room, nick, subject);
    }
}

void
sv_ev_room_history(const char * const room_jid, const char * const nick,
    GDateTime *timestamp, const char * const message)
{
    ui_room_history(room_jid, nick, timestamp, message);
}

void
sv_ev_room_message(const char * const room_jid, const char * const nick,
    const char * const message)
{
    ui_room_message(room_jid, nick, message);

    if (prefs_get_boolean(PREF_GRLOG)) {
        Jid *jid = jid_create(jabber_get_fulljid());
        groupchat_log_chat(jid->barejid, room_jid, nick, message);
        jid_destroy(jid);
    }
}

void
sv_ev_incoming_private_message(const char * const fulljid, char *message)
{
    ui_incoming_private_msg(fulljid, message, NULL);
}

void
sv_ev_outgoing_carbon(char *barejid, char *message)
{
    ui_outgoing_chat_msg_carbon(barejid, message);
}

void
sv_ev_incoming_carbon(char *barejid, char *resource, char *message)
{
    gboolean new_win = FALSE;
    ProfChatWin *chatwin = wins_get_chat(barejid);
    if (!chatwin) {
        ProfWin *window = wins_new_chat(barejid);
        chatwin = (ProfChatWin*)window;
        new_win = TRUE;
    }

    ui_incoming_msg(chatwin, resource, message, NULL, new_win);
    chat_log_msg_in(barejid, message);
}

void
sv_ev_incoming_message(char *barejid, char *resource, char *message, char *enc_message)
{
    gboolean new_win = FALSE;
    ProfChatWin *chatwin = wins_get_chat(barejid);
    if (!chatwin) {
        ProfWin *window = wins_new_chat(barejid);
        chatwin = (ProfChatWin*)window;
        new_win = TRUE;
    }

// OTR suported, PGP supported
#ifdef HAVE_LIBOTR
#ifdef HAVE_LIBGPGME
    prof_enc_t enc_mode = chatwin->enc_mode;
    if (enc_message) {
        if (enc_mode == PROF_ENC_OTR) {
            win_println((ProfWin*)chatwin, 0, "PGP encrypted message received whilst in OTR session.");
        } else { // PROF_ENC_NONE, PROF_ENC_PGP
            char *decrypted = p_gpg_decrypt(enc_message);
            if (decrypted) {
                if (enc_mode == PROF_ENC_NONE) {
                    win_println((ProfWin*)chatwin, 0, "PGP encryption enabled.");
                }
                ui_incoming_msg(chatwin, resource, decrypted, NULL, new_win);
                chat_log_pgp_msg_in(barejid, decrypted);
                chatwin->enc_mode = PROF_ENC_PGP;
                p_gpg_free_decrypted(decrypted);
            } else {
                ui_incoming_msg(chatwin, resource, message, NULL, new_win);
                chat_log_msg_in(barejid, message);
                chatwin->enc_mode = PROF_ENC_NONE;
            }
        }
    } else {
        if (enc_mode == PROF_ENC_PGP) {
            win_println((ProfWin*)chatwin, 0, "PGP encryption disabled.");
            ui_incoming_msg(chatwin, resource, message, NULL, new_win);
            chat_log_msg_in(barejid, message);
            chatwin->enc_mode = PROF_ENC_NONE;
        } else {
            gboolean decrypted = FALSE;
            char *otr_res = otr_on_message_recv(barejid, resource, message, &decrypted);
            if (otr_res) {
                ui_incoming_msg(chatwin, resource, otr_res, NULL, new_win);
                chat_log_otr_msg_in(barejid, otr_res, decrypted);
                otr_free_message(otr_res);
            }
        }
    }
    return;
#endif
#endif

// OTR supported, PGP unsupported
#ifdef HAVE_LIBOTR
#ifndef HAVE_LIBGPGME
    gboolean decrypted = FALSE;
    char *otr_res = otr_on_message_recv(barejid, resource, message, &decrypted);
    if (otr_res) {
        ui_incoming_msg(chatwin, resource, otr_res, NULL, new_win);
        chat_log_otr_msg_in(barejid, otr_res, decrypted);
        otr_free_message(otr_res);
    }
    return;
#endif
#endif

// OTR unsupported, PGP supported
#ifndef HAVE_LIBOTR
#ifdef HAVE_LIBGPGME
    if (enc_message) {
        char *decrypted = p_gpg_decrypt(enc_message);
        if (decrypted) {
            ui_incoming_msg(chatwin, resource, decrypted, NULL, new_win);
            chat_log_pgp_msg_in(barejid, decrypted);
            chatwin->enc_mode = PROF_ENC_PGP;
            p_gpg_free_decrypted(decrypted);
        } else {
            ui_incoming_msg(chatwin, resource, message, NULL, new_win);
            chat_log_msg_in(barejid, message);
            chatwin->enc_mode = PROF_ENC_NONE;
        }
    } else {
        ui_incoming_msg(chatwin, resource, message, NULL, new_win);
        chat_log_msg_in(barejid, message);
        chatwin->enc_mode = PROF_ENC_NONE;
    }
    return;
#endif
#endif

// OTR unsupported, PGP unsupported
#ifndef HAVE_LIBOTR
#ifndef HAVE_LIBGPGME
    ui_incoming_msg(chatwin, resource, message, NULL, new_win);
    chat_log_msg_in(barejid, message);
    chatwin->enc_mode = PROF_ENC_NONE;
    return;
#endif
#endif
}

void
sv_ev_delayed_private_message(const char * const fulljid, char *message, GDateTime *timestamp)
{
    ui_incoming_private_msg(fulljid, message, timestamp);
}

void
sv_ev_delayed_message(char *barejid, char *message, GDateTime *timestamp)
{
    gboolean new_win = FALSE;
    ProfChatWin *chatwin = wins_get_chat(barejid);
    if (!chatwin) {
        ProfWin *window = wins_new_chat(barejid);
        chatwin = (ProfChatWin*)window;
        new_win = TRUE;
    }

    ui_incoming_msg(chatwin, NULL, message, timestamp, new_win);
    chat_log_msg_in_delayed(barejid, message, timestamp);
}

void
sv_ev_message_receipt(char *barejid, char *id)
{
    ui_message_receipt(barejid, id);
}

void
sv_ev_typing(char *barejid, char *resource)
{
    ui_contact_typing(barejid, resource);
    if (ui_chat_win_exists(barejid)) {
        chat_session_recipient_typing(barejid, resource);
    }
}

void
sv_ev_paused(char *barejid, char *resource)
{
    if (ui_chat_win_exists(barejid)) {
        chat_session_recipient_paused(barejid, resource);
    }
}

void
sv_ev_inactive(char *barejid, char *resource)
{
    if (ui_chat_win_exists(barejid)) {
        chat_session_recipient_inactive(barejid, resource);
    }
}

void
sv_ev_gone(const char * const barejid, const char * const resource)
{
    ui_recipient_gone(barejid, resource);
    if (ui_chat_win_exists(barejid)) {
        chat_session_recipient_gone(barejid, resource);
    }
}

void
sv_ev_activity(const char * const barejid, const char * const resource, gboolean send_states)
{
    if (ui_chat_win_exists(barejid)) {
        chat_session_recipient_active(barejid, resource, send_states);
    }
}

void
sv_ev_subscription(const char *barejid, jabber_subscr_t type)
{
    switch (type) {
    case PRESENCE_SUBSCRIBE:
        /* TODO: auto-subscribe if needed */
        cons_show("Received authorization request from %s", barejid);
        log_info("Received authorization request from %s", barejid);
        ui_print_system_msg_from_recipient(barejid, "Authorization request, type '/sub allow' to accept or '/sub deny' to reject");
        if (prefs_get_boolean(PREF_NOTIFY_SUB)) {
            notify_subscription(barejid);
        }
        break;
    case PRESENCE_SUBSCRIBED:
        cons_show("Subscription received from %s", barejid);
        log_info("Subscription received from %s", barejid);
        ui_print_system_msg_from_recipient(barejid, "Subscribed");
        break;
    case PRESENCE_UNSUBSCRIBED:
        cons_show("%s deleted subscription", barejid);
        log_info("%s deleted subscription", barejid);
        ui_print_system_msg_from_recipient(barejid, "Unsubscribed");
        break;
    default:
        /* unknown type */
        break;
    }
}

void
sv_ev_contact_offline(char *barejid, char *resource, char *status)
{
    gboolean updated = roster_contact_offline(barejid, resource, status);

    if (resource && updated) {
        ui_contact_offline(barejid, resource, status);
    }

    rosterwin_roster();
    chat_session_remove(barejid);
}

void
sv_ev_contact_online(char *barejid, Resource *resource, GDateTime *last_activity, char *pgpsig)
{
    gboolean updated = roster_update_presence(barejid, resource, last_activity);

    if (updated) {
        ui_contact_online(barejid, resource, last_activity);
    }

#ifdef HAVE_LIBGPGME
    if (pgpsig) {
        p_gpg_verify(barejid, pgpsig);
    }
#endif

    rosterwin_roster();
    chat_session_remove(barejid);
}

void
sv_ev_leave_room(const char * const room)
{
    muc_leave(room);
    ui_leave_room(room);
}

void
sv_ev_room_destroy(const char * const room)
{
    muc_leave(room);
    ui_room_destroy(room);
}

void
sv_ev_room_destroyed(const char * const room, const char * const new_jid, const char * const password,
    const char * const reason)
{
    muc_leave(room);
    ui_room_destroyed(room, reason, new_jid, password);
}

void
sv_ev_room_kicked(const char * const room, const char * const actor, const char * const reason)
{
    muc_leave(room);
    ui_room_kicked(room, actor, reason);
}

void
sv_ev_room_banned(const char * const room, const char * const actor, const char * const reason)
{
    muc_leave(room);
    ui_room_banned(room, actor, reason);
}

void
sv_ev_room_occupant_offline(const char * const room, const char * const nick,
    const char * const show, const char * const status)
{
    muc_roster_remove(room, nick);

    char *muc_status_pref = prefs_get_string(PREF_STATUSES_MUC);
    if (g_strcmp0(muc_status_pref, "none") != 0) {
        ui_room_member_offline(room, nick);
    }
    prefs_free_string(muc_status_pref);
    occupantswin_occupants(room);
}

void
sv_ev_room_occupent_kicked(const char * const room, const char * const nick, const char * const actor,
    const char * const reason)
{
    muc_roster_remove(room, nick);
    ui_room_member_kicked(room, nick, actor, reason);
    occupantswin_occupants(room);
}

void
sv_ev_room_occupent_banned(const char * const room, const char * const nick, const char * const actor,
    const char * const reason)
{
    muc_roster_remove(room, nick);
    ui_room_member_banned(room, nick, actor, reason);
    occupantswin_occupants(room);
}

void
sv_ev_roster_update(const char * const barejid, const char * const name,
    GSList *groups, const char * const subscription, gboolean pending_out)
{
    roster_update(barejid, name, groups, subscription, pending_out);
    rosterwin_roster();
}

void
sv_ev_xmpp_stanza(const char * const msg)
{
    ui_handle_stanza(msg);
}

void
sv_ev_muc_self_online(const char * const room, const char * const nick, gboolean config_required,
    const char * const role, const char * const affiliation, const char * const actor, const char * const reason,
    const char * const jid, const char * const show, const char * const status)
{
    muc_roster_add(room, nick, jid, role, affiliation, show, status);
    char *old_role = muc_role_str(room);
    char *old_affiliation = muc_affiliation_str(room);
    muc_set_role(room, role);
    muc_set_affiliation(room, affiliation);

    // handle self nick change
    if (muc_nick_change_pending(room)) {
        muc_nick_change_complete(room, nick);
        ui_room_nick_change(room, nick);

    // handle roster complete
    } else if (!muc_roster_complete(room)) {
        if (muc_autojoin(room)) {
            ui_room_join(room, FALSE);
        } else {
            ui_room_join(room, TRUE);
        }

        iq_room_info_request(room, FALSE);

        muc_invites_remove(room);
        muc_roster_set_complete(room);

        // show roster if occupants list disabled by default
        if (!prefs_get_boolean(PREF_OCCUPANTS)) {
            GList *occupants = muc_roster(room);
            ui_room_roster(room, occupants, NULL);
            g_list_free(occupants);
        }

        char *subject = muc_subject(room);
        if (subject) {
            ui_room_subject(room, NULL, subject);
        }

        GList *pending_broadcasts = muc_pending_broadcasts(room);
        if (pending_broadcasts) {
            GList *curr = pending_broadcasts;
            while (curr) {
                ui_room_broadcast(room, curr->data);
                curr = g_list_next(curr);
            }
        }

        // room configuration required
        if (config_required) {
            muc_set_requires_config(room, TRUE);
            ui_room_requires_config(room);
        }

    // check for change in role/affiliation
    } else {
        if (prefs_get_boolean(PREF_MUC_PRIVILEGES)) {
            // both changed
            if ((g_strcmp0(role, old_role) != 0) && (g_strcmp0(affiliation, old_affiliation) != 0)) {
                ui_room_role_and_affiliation_change(room, role, affiliation, actor, reason);

            // role changed
            } else if (g_strcmp0(role, old_role) != 0) {
                ui_room_role_change(room, role, actor, reason);

            // affiliation changed
            } else if (g_strcmp0(affiliation, old_affiliation) != 0) {
                ui_room_affiliation_change(room, affiliation, actor, reason);
            }
        }
    }

    occupantswin_occupants(room);
}

void
sv_ev_muc_occupant_online(const char * const room, const char * const nick, const char * const jid,
    const char * const role, const char * const affiliation, const char * const actor, const char * const reason,
    const char * const show, const char * const status)
{
    Occupant *occupant = muc_roster_item(room, nick);

    const char *old_role = NULL;
    const char *old_affiliation = NULL;
    if (occupant) {
        old_role = muc_occupant_role_str(occupant);
        old_affiliation = muc_occupant_affiliation_str(occupant);
    }

    gboolean updated = muc_roster_add(room, nick, jid, role, affiliation, show, status);

    // not yet finished joining room
    if (!muc_roster_complete(room)) {
        return;
    }

    // handle nickname change
    char *old_nick = muc_roster_nick_change_complete(room, nick);
    if (old_nick) {
        ui_room_member_nick_change(room, old_nick, nick);
        free(old_nick);
        occupantswin_occupants(room);
        return;
    }

    // joined room
    if (!occupant) {
        char *muc_status_pref = prefs_get_string(PREF_STATUSES_MUC);
        if (g_strcmp0(muc_status_pref, "none") != 0) {
            ui_room_member_online(room, nick, role, affiliation, show, status);
        }
        prefs_free_string(muc_status_pref);
        occupantswin_occupants(room);
        return;
    }

    // presence updated
    if (updated) {
        char *muc_status_pref = prefs_get_string(PREF_STATUSES_MUC);
        if (g_strcmp0(muc_status_pref, "all") == 0) {
            ui_room_member_presence(room, nick, show, status);
        }
        prefs_free_string(muc_status_pref);
        occupantswin_occupants(room);

    // presence unchanged, check for role/affiliation change
    } else {
        if (prefs_get_boolean(PREF_MUC_PRIVILEGES)) {
            // both changed
            if ((g_strcmp0(role, old_role) != 0) && (g_strcmp0(affiliation, old_affiliation) != 0)) {
                ui_room_occupant_role_and_affiliation_change(room, nick, role, affiliation, actor, reason);

            // role changed
            } else if (g_strcmp0(role, old_role) != 0) {
                ui_room_occupant_role_change(room, nick, role, actor, reason);

            // affiliation changed
            } else if (g_strcmp0(affiliation, old_affiliation) != 0) {
                ui_room_occupant_affiliation_change(room, nick, affiliation, actor, reason);
            }
        }
        occupantswin_occupants(room);
    }
}
