/*
 * commands.c
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

#include "config.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <uuid/uuid.h>
#include <glib.h>

#include "chat_session.h"
#include "command/commands.h"
#include "command/command.h"
#include "common.h"
#include "config/accounts.h"
#include "config/account.h"
#include "config/preferences.h"
#include "config/theme.h"
#include "contact.h"
#include "roster_list.h"
#include "jid.h"
#include "log.h"
#include "muc.h"
#ifdef HAVE_LIBOTR
#include "otr/otr.h"
#endif
#ifdef HAVE_LIBGPGME
#include "pgp/gpg.h"
#endif
#include "profanity.h"
#include "tools/autocomplete.h"
#include "tools/parser.h"
#include "tools/tinyurl.h"
#include "xmpp/xmpp.h"
#include "xmpp/bookmark.h"
#include "ui/ui.h"
#include "window_list.h"
#include "event/client_events.h"
#include "event/ui_events.h"

static void _update_presence(const resource_presence_t presence,
    const char * const show, gchar **args);
static gboolean _cmd_set_boolean_preference(gchar *arg, const char * const command,
    const char * const display, preference_t pref);
//static void _cmd_show_filtered_help(char *heading, gchar *cmd_filter[], int filter_size);
static void _who_room(ProfWin *window, const char * const command, gchar **args);
static void _who_roster(ProfWin *window, const char * const command, gchar **args);

extern GHashTable *commands;

gboolean
cmd_execute_default(ProfWin *window, const char * inp)
{
    // handle escaped commands - treat as normal message
    if (g_str_has_prefix(inp, "//")) {
        inp++;

    // handle unknown commands
    } else if ((inp[0] == '/') && (!g_str_has_prefix(inp, "/me "))) {
        cons_show("Unknown command: %s", inp);
        cons_alert();
        return TRUE;
    }

    // handle non commands in non chat windows
    if (window->type != WIN_CHAT && window->type != WIN_MUC && window->type != WIN_PRIVATE) {
        cons_show("Unknown command: %s", inp);
        return TRUE;
    }

    jabber_conn_status_t status = jabber_get_connection_status();
    if (status != JABBER_CONNECTED) {
        ui_current_print_line("You are not currently connected.");
        return TRUE;
    }

    switch (window->type) {
    case WIN_CHAT:
    {
        ProfChatWin *chatwin = (ProfChatWin*)window;
        assert(chatwin->memcheck == PROFCHATWIN_MEMCHECK);
        cl_ev_send_msg(chatwin, inp);
        break;
    }
    case WIN_PRIVATE:
    {
        ProfPrivateWin *privatewin = (ProfPrivateWin*)window;
        assert(privatewin->memcheck == PROFPRIVATEWIN_MEMCHECK);
        cl_ev_send_priv_msg(privatewin, inp);
        break;
    }
    case WIN_MUC:
    {
        ProfMucWin *mucwin = (ProfMucWin*)window;
        assert(mucwin->memcheck == PROFMUCWIN_MEMCHECK);
        cl_ev_send_muc_msg(mucwin, inp);
        break;
    }
    default:
        break;
    }

    return TRUE;
}

gboolean
cmd_execute_alias(ProfWin *window, const char * const inp, gboolean *ran)
{
    if (inp[0] != '/') {
        ran = FALSE;
        return TRUE;
    }

    char *alias = strdup(inp+1);
    char *value = prefs_get_alias(alias);
    free(alias);
    if (value) {
        *ran = TRUE;
        return cmd_process_input(window, value);
    }

    *ran = FALSE;
    return TRUE;
}

gboolean
cmd_connect(ProfWin *window, const char * const command, gchar **args)
{
    jabber_conn_status_t conn_status = jabber_get_connection_status();
    if ((conn_status != JABBER_DISCONNECTED) && (conn_status != JABBER_STARTED)) {
        cons_show("You are either connected already, or a login is in process.");
        return TRUE;
    }

    gchar *opt_keys[] = { "server", "port", NULL };
    gboolean parsed;

    GHashTable *options = parse_options(&args[args[0] ? 1 : 0], opt_keys, &parsed);
    if (!parsed) {
        cons_bad_cmd_usage(command);
        cons_show("");
        return TRUE;
    }

    char *altdomain = g_hash_table_lookup(options, "server");

    int port = 0;
    if (g_hash_table_contains(options, "port")) {
        char *port_str = g_hash_table_lookup(options, "port");
        char *err_msg = NULL;
        gboolean res = strtoi_range(port_str, &port, 1, 65535, &err_msg);
        if (!res) {
            cons_show(err_msg);
            cons_show("");
            free(err_msg);
            port = 0;
            return TRUE;
        }
    }

    char *user = args[0];
    char *def = prefs_get_string(PREF_DEFAULT_ACCOUNT);
    if (!user) {
        if (def) {
            user = def;
            cons_show("Using default account %s.", user);
        } else {
            cons_show("No default account.");
            g_free(def);
            return TRUE;
        }
    }

    char *lower = g_utf8_strdown(user, -1);
    char *jid;
    g_free(def);

    // connect with account
    ProfAccount *account = accounts_get_account(lower);
    if (account) {
        // use password if set
        if (account->password) {
            conn_status = cl_ev_connect_account(account);

        // use eval_password if set
        } else if (account->eval_password) {
            gboolean res = account_eval_password(account);
            if (res) {
                conn_status = cl_ev_connect_account(account);
                free(account->password);
                account->password = NULL;
            } else {
                cons_show("Error evaluating password, see logs for details.");
                g_free(lower);
                account_free(account);
                return TRUE;
            }

        // no account password setting, prompt
        } else {
            account->password = ui_ask_password();
            conn_status = cl_ev_connect_account(account);
            free(account->password);
            account->password = NULL;
        }

        jid = account_create_full_jid(account);
        account_free(account);

    // connect with JID
    } else {
        jid = strdup(lower);
        char *passwd = ui_ask_password();
        conn_status = cl_ev_connect_jid(jid, passwd, altdomain, port);
        free(passwd);
    }

    if (conn_status == JABBER_DISCONNECTED) {
        cons_show_error("Connection attempt for %s failed.", jid);
        log_info("Connection attempt for %s failed", jid);
    }

    options_destroy(options);
    g_free(lower);
    free(jid);

    return TRUE;
}

gboolean
cmd_account(ProfWin *window, const char * const command, gchar **args)
{
    char *subcmd = args[0];

    if (subcmd == NULL) {
        if (jabber_get_connection_status() != JABBER_CONNECTED) {
            cons_bad_cmd_usage(command);
        } else {
            ProfAccount *account = accounts_get_account(jabber_get_account_name());
            cons_show_account(account);
            account_free(account);
        }
    } else if (strcmp(subcmd, "list") == 0) {
        gchar **accounts = accounts_get_list();
        cons_show_account_list(accounts);
        g_strfreev(accounts);
    } else if (strcmp(subcmd, "show") == 0) {
        char *account_name = args[1];
        if (account_name == NULL) {
            cons_bad_cmd_usage(command);
        } else {
            ProfAccount *account = accounts_get_account(account_name);
            if (account == NULL) {
                cons_show("No such account.");
                cons_show("");
            } else {
                cons_show_account(account);
                account_free(account);
            }
        }
    } else if (strcmp(subcmd, "add") == 0) {
        char *account_name = args[1];
        if (account_name == NULL) {
            cons_bad_cmd_usage(command);
        } else {
            accounts_add(account_name, NULL, 0);
            cons_show("Account created.");
            cons_show("");
        }
    } else if (strcmp(subcmd, "remove") == 0) {
        char *account_name = args[1];
        if(!account_name) {
            cons_bad_cmd_usage(command);
        } else {
            char *def = prefs_get_string(PREF_DEFAULT_ACCOUNT);
            if(accounts_remove(account_name)){
                cons_show("Account %s removed.", account_name);
                if(def && strcmp(def, account_name) == 0){
                    prefs_set_string(PREF_DEFAULT_ACCOUNT, NULL);
                    cons_show("Default account removed because the corresponding account was removed.");
                }
            } else {
                cons_show("Failed to remove account %s.", account_name);
                cons_show("Either the account does not exist, or an unknown error occurred.");
            }
            cons_show("");
            g_free(def);
        }
    } else if (strcmp(subcmd, "enable") == 0) {
        char *account_name = args[1];
        if (account_name == NULL) {
            cons_bad_cmd_usage(command);
        } else {
            if (accounts_enable(account_name)) {
                cons_show("Account enabled.");
                cons_show("");
            } else {
                cons_show("No such account: %s", account_name);
                cons_show("");
            }
        }
    } else if (strcmp(subcmd, "disable") == 0) {
        char *account_name = args[1];
        if (account_name == NULL) {
            cons_bad_cmd_usage(command);
        } else {
            if (accounts_disable(account_name)) {
                cons_show("Account disabled.");
                cons_show("");
            } else {
                cons_show("No such account: %s", account_name);
                cons_show("");
            }
        }
    } else if (strcmp(subcmd, "rename") == 0) {
        if (g_strv_length(args) != 3) {
            cons_bad_cmd_usage(command);
        } else {
            char *account_name = args[1];
            char *new_name = args[2];

            if (accounts_rename(account_name, new_name)) {
                cons_show("Account renamed.");
                cons_show("");
            } else {
                cons_show("Either account %s doesn't exist, or account %s already exists.", account_name, new_name);
                cons_show("");
            }
        }
    } else if (strcmp(subcmd, "default") == 0) {
        if(g_strv_length(args) == 1){
            char *def = prefs_get_string(PREF_DEFAULT_ACCOUNT);

            if(def){
                cons_show("The default account is %s.", def);
                free(def);
            } else {
                cons_show("No default account.");
            }
        } else if(g_strv_length(args) == 2){
            if(strcmp(args[1], "off") == 0){
                prefs_set_string(PREF_DEFAULT_ACCOUNT, NULL);
                cons_show("Removed default account.");
            } else {
                cons_bad_cmd_usage(command);
            }
        } else if(g_strv_length(args) == 3) {
            if(strcmp(args[1], "set") == 0){
                if(accounts_get_account(args[2])){
                    prefs_set_string(PREF_DEFAULT_ACCOUNT, args[2]);
                    cons_show("Default account set to %s.", args[2]);
                } else {
                    cons_show("Account %s does not exist.", args[2]);
                }
            } else {
                cons_bad_cmd_usage(command);
            }
        } else {
            cons_bad_cmd_usage(command);
        }
    } else if (strcmp(subcmd, "set") == 0) {
        if (g_strv_length(args) != 4) {
            cons_bad_cmd_usage(command);
        } else {
            char *account_name = args[1];
            char *property = args[2];
            char *value = args[3];

            if (!accounts_account_exists(account_name)) {
                cons_show("Account %s doesn't exist", account_name);
                cons_show("");
            } else {
                if (strcmp(property, "jid") == 0) {
                    Jid *jid = jid_create(args[3]);
                    if (jid == NULL) {
                        cons_show("Malformed jid: %s", value);
                    } else {
                        accounts_set_jid(account_name, jid->barejid);
                        cons_show("Updated jid for account %s: %s", account_name, jid->barejid);
                        if (jid->resourcepart) {
                            accounts_set_resource(account_name, jid->resourcepart);
                            cons_show("Updated resource for account %s: %s", account_name, jid->resourcepart);
                        }
                        cons_show("");
                    }
                    jid_destroy(jid);
                } else if (strcmp(property, "server") == 0) {
                    accounts_set_server(account_name, value);
                    cons_show("Updated server for account %s: %s", account_name, value);
                    cons_show("");
                } else if (strcmp(property, "port") == 0) {
                    int port;
                    char *err_msg = NULL;
                    gboolean res = strtoi_range(value, &port, 1, 65535, &err_msg);
                    if (!res) {
                        cons_show(err_msg);
                        cons_show("");
                        free(err_msg);
                        return TRUE;
                    } else {
                        accounts_set_port(account_name, port);
                        cons_show("Updated port for account %s: %s", account_name, value);
                        cons_show("");
                    }
                } else if (strcmp(property, "resource") == 0) {
                    accounts_set_resource(account_name, value);
                    if (jabber_get_connection_status() == JABBER_CONNECTED) {
                        cons_show("Updated resource for account %s: %s, you will need to reconnect to pick up the change.", account_name, value);
                    } else {
                        cons_show("Updated resource for account %s: %s", account_name, value);
                    }
                    cons_show("");
                } else if (strcmp(property, "password") == 0) {
                    if(accounts_get_account(account_name)->eval_password) {
                        cons_show("Cannot set password when eval_password is set.");
                    } else {
                        accounts_set_password(account_name, value);
                        cons_show("Updated password for account %s", account_name);
                        cons_show("");
                    }
                } else if (strcmp(property, "eval_password") == 0) {
                    if(accounts_get_account(account_name)->password) {
                        cons_show("Cannot set eval_password when password is set.");
                    } else {
                        accounts_set_eval_password(account_name, value);
                        cons_show("Updated eval_password for account %s", account_name);
                        cons_show("");
                    }
                } else if (strcmp(property, "muc") == 0) {
                    accounts_set_muc_service(account_name, value);
                    cons_show("Updated muc service for account %s: %s", account_name, value);
                    cons_show("");
                } else if (strcmp(property, "nick") == 0) {
                    accounts_set_muc_nick(account_name, value);
                    cons_show("Updated muc nick for account %s: %s", account_name, value);
                    cons_show("");
                } else if (strcmp(property, "otr") == 0) {
                    if ((g_strcmp0(value, "manual") != 0)
                            && (g_strcmp0(value, "opportunistic") != 0)
                            && (g_strcmp0(value, "always") != 0)) {
                        cons_show("OTR policy must be one of: manual, opportunistic or always.");
                    } else {
                        accounts_set_otr_policy(account_name, value);
                        cons_show("Updated OTR policy for account %s: %s", account_name, value);
                        cons_show("");
                    }
                } else if (strcmp(property, "status") == 0) {
                    if (!valid_resource_presence_string(value) && (strcmp(value, "last") != 0)) {
                        cons_show("Invalid status: %s", value);
                    } else {
                        accounts_set_login_presence(account_name, value);
                        cons_show("Updated login status for account %s: %s", account_name, value);
                    }
                    cons_show("");
                } else if (strcmp(property, "pgpkeyid") == 0) {
#ifdef HAVE_LIBGPGME
                    if (!p_gpg_valid_key(value)) {
                        cons_show("Invalid PGP key ID specified, see /pgp keys");
                    } else {
                        accounts_set_pgp_keyid(account_name, value);
                        cons_show("Updated PGP key ID for account %s: %s", account_name, value);
                    }
#else
                    cons_show("PGP support is not included in this build.");
#endif
                    cons_show("");
                } else if (valid_resource_presence_string(property)) {
                    int intval;
                    char *err_msg = NULL;
                    gboolean res = strtoi_range(value, &intval, -128, 127, &err_msg);
                    if (res) {
                        resource_presence_t presence_type = resource_presence_from_string(property);
                        switch (presence_type)
                        {
                            case (RESOURCE_ONLINE):
                                accounts_set_priority_online(account_name, intval);
                                break;
                            case (RESOURCE_CHAT):
                                accounts_set_priority_chat(account_name, intval);
                                break;
                            case (RESOURCE_AWAY):
                                accounts_set_priority_away(account_name, intval);
                                break;
                            case (RESOURCE_XA):
                                accounts_set_priority_xa(account_name, intval);
                                break;
                            case (RESOURCE_DND):
                                accounts_set_priority_dnd(account_name, intval);
                                break;
                        }

                        jabber_conn_status_t conn_status = jabber_get_connection_status();
                        if (conn_status == JABBER_CONNECTED) {
                            char *connected_account = jabber_get_account_name();
                            resource_presence_t last_presence = accounts_get_last_presence(connected_account);

                            if (presence_type == last_presence) {
                                char *message = jabber_get_presence_message();
                                cl_ev_presence_send(last_presence, message, 0);
                            }
                        }
                        cons_show("Updated %s priority for account %s: %s", property, account_name, value);
                        cons_show("");
                    } else {
                        cons_show(err_msg);
                        free(err_msg);
                    }
                } else {
                    cons_show("Invalid property: %s", property);
                    cons_show("");
                }
            }
        }
    } else if (strcmp(subcmd, "clear") == 0) {
        if (g_strv_length(args) != 3) {
            cons_bad_cmd_usage(command);
        } else {
            char *account_name = args[1];
            char *property = args[2];

            if (!accounts_account_exists(account_name)) {
                cons_show("Account %s doesn't exist", account_name);
                cons_show("");
            } else {
                if (strcmp(property, "password") == 0) {
                    accounts_clear_password(account_name);
                    cons_show("Removed password for account %s", account_name);
                    cons_show("");
                } else if (strcmp(property, "eval_password") == 0) {
                    accounts_clear_eval_password(account_name);
                    cons_show("Removed eval password for account %s", account_name);
                    cons_show("");
                } else if (strcmp(property, "server") == 0) {
                    accounts_clear_server(account_name);
                    cons_show("Removed server for account %s", account_name);
                    cons_show("");
                } else if (strcmp(property, "port") == 0) {
                    accounts_clear_port(account_name);
                    cons_show("Removed port for account %s", account_name);
                    cons_show("");
                } else if (strcmp(property, "otr") == 0) {
                    accounts_clear_otr(account_name);
                    cons_show("OTR policy removed for account %s", account_name);
                    cons_show("");
                } else if (strcmp(property, "pgpkeyid") == 0) {
                    accounts_clear_pgp_keyid(account_name);
                    cons_show("Removed PGP key ID for account %s", account_name);
                    cons_show("");
                } else {
                    cons_show("Invalid property: %s", property);
                    cons_show("");
                }
            }
        }
    } else {
        cons_bad_cmd_usage(command);
        cons_show("");
    }

    return TRUE;
}

gboolean
cmd_sub(ProfWin *window, const char * const command, gchar **args)
{
    jabber_conn_status_t conn_status = jabber_get_connection_status();

    if (conn_status != JABBER_CONNECTED) {
        cons_show("You are currently not connected.");
        return TRUE;
    }

    char *subcmd, *jid;
    subcmd = args[0];
    jid = args[1];

    if (subcmd == NULL) {
        cons_bad_cmd_usage(command);
        return TRUE;
    }

    if (strcmp(subcmd, "sent") == 0) {
        cons_show_sent_subs();
        return TRUE;
    }

    if (strcmp(subcmd, "received") == 0) {
        cons_show_received_subs();
        return TRUE;
    }

    if ((window->type != WIN_CHAT) && (jid == NULL)) {
        cons_show("You must specify a contact.");
        return TRUE;
    }

    if (jid == NULL) {
        ProfChatWin *chatwin = (ProfChatWin*)window;
        assert(chatwin->memcheck == PROFCHATWIN_MEMCHECK);
        jid = chatwin->barejid;
    }

    Jid *jidp = jid_create(jid);

    if (strcmp(subcmd, "allow") == 0) {
        presence_subscription(jidp->barejid, PRESENCE_SUBSCRIBED);
        cons_show("Accepted subscription for %s", jidp->barejid);
        log_info("Accepted subscription for %s", jidp->barejid);
    } else if (strcmp(subcmd, "deny") == 0) {
        presence_subscription(jidp->barejid, PRESENCE_UNSUBSCRIBED);
        cons_show("Deleted/denied subscription for %s", jidp->barejid);
        log_info("Deleted/denied subscription for %s", jidp->barejid);
    } else if (strcmp(subcmd, "request") == 0) {
        presence_subscription(jidp->barejid, PRESENCE_SUBSCRIBE);
        cons_show("Sent subscription request to %s.", jidp->barejid);
        log_info("Sent subscription request to %s.", jidp->barejid);
    } else if (strcmp(subcmd, "show") == 0) {
        PContact contact = roster_get_contact(jidp->barejid);
        if ((contact == NULL) || (p_contact_subscription(contact) == NULL)) {
            if (window->type == WIN_CHAT) {
                ui_current_print_line("No subscription information for %s.", jidp->barejid);
            } else {
                cons_show("No subscription information for %s.", jidp->barejid);
            }
        } else {
            if (window->type == WIN_CHAT) {
                if (p_contact_pending_out(contact)) {
                    ui_current_print_line("%s subscription status: %s, request pending.",
                        jidp->barejid, p_contact_subscription(contact));
                } else {
                    ui_current_print_line("%s subscription status: %s.", jidp->barejid,
                        p_contact_subscription(contact));
                }
            } else {
                if (p_contact_pending_out(contact)) {
                    cons_show("%s subscription status: %s, request pending.",
                        jidp->barejid, p_contact_subscription(contact));
                } else {
                    cons_show("%s subscription status: %s.", jidp->barejid,
                        p_contact_subscription(contact));
                }
            }
        }
    } else {
        cons_bad_cmd_usage(command);
    }

    jid_destroy(jidp);

    return TRUE;
}

gboolean
cmd_disconnect(ProfWin *window, const char * const command, gchar **args)
{
    if (jabber_get_connection_status() == JABBER_CONNECTED) {
        char *jid = strdup(jabber_get_fulljid());
        cons_show("%s logged out successfully.", jid);
        jabber_disconnect();
        roster_clear();
        muc_invites_clear();
        chat_sessions_clear();
        ui_disconnected();
#ifdef HAVE_LIBGPGME
        p_gpg_on_disconnect();
#endif
        free(jid);
    } else {
        cons_show("You are not currently connected.");
    }

    return TRUE;
}

gboolean
cmd_quit(ProfWin *window, const char * const command, gchar **args)
{
    log_info("Profanity is shutting down...");
    exit(0);
    return FALSE;
}

gboolean
cmd_wins(ProfWin *window, const char * const command, gchar **args)
{
    if (args[0] == NULL) {
        cons_show_wins();
    } else if (strcmp(args[0], "tidy") == 0) {
        if (ui_tidy_wins()) {
            cons_show("Windows tidied.");
        } else {
            cons_show("No tidy needed.");
        }
    } else if (strcmp(args[0], "prune") == 0) {
        ui_prune_wins();
    } else if (strcmp(args[0], "swap") == 0) {
        if ((args[1] == NULL) || (args[2] == NULL)) {
            cons_bad_cmd_usage(command);
        } else {
            int source_win = atoi(args[1]);
            int target_win = atoi(args[2]);
            if ((source_win == 1) || (target_win == 1)) {
                cons_show("Cannot move console window.");
            } else if (source_win == 10 || target_win == 10) {
                cons_show("Window 10 does not exist");
            } else if (source_win != target_win) {
                gboolean swapped = ui_swap_wins(source_win, target_win);
                if (swapped) {
                    cons_show("Swapped windows %d <-> %d", source_win, target_win);
                } else {
                    cons_show("Window %d does not exist", source_win);
                }
            } else {
                cons_show("Same source and target window supplied.");
            }
        }
    } else {
        cons_bad_cmd_usage(command);
    }

    return TRUE;
}

gboolean
cmd_winstidy(ProfWin *window, const char * const command, gchar **args)
{
    gboolean result = _cmd_set_boolean_preference(args[0], command, "Wins Auto Tidy", PREF_WINS_AUTO_TIDY);

    if (result && g_strcmp0(args[0], "on") == 0) {
        ui_tidy_wins();
    }

    return result;
}

gboolean
cmd_win(ProfWin *window, const char * const command, gchar **args)
{
    int num = atoi(args[0]);

    ProfWin *focuswin = wins_get_by_num(num);
    if (!focuswin) {
        cons_show("Window %d does not exist.", num);
    } else {
        ui_ev_focus_win(focuswin);
    }

    return TRUE;
}

static void
_cmd_help_cmd_list(const char * const tag)
{
    cons_show("");
    ProfWin *console = wins_get_console();
    if (tag) {
        win_vprint(console, '-', 0, NULL, 0, THEME_WHITE_BOLD, "", "%s commands", tag);
    } else {
        win_print(console, '-', 0, NULL, 0, THEME_WHITE_BOLD, "", "All commands");
    }

    GList *ordered_commands = NULL;
    GHashTableIter iter;
    gpointer key;
    gpointer value;

    g_hash_table_iter_init(&iter, commands);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        Command *pcmd = (Command *)value;
        if (tag) {
            if (cmd_has_tag(pcmd, tag)) {
                ordered_commands = g_list_insert_sorted(ordered_commands, pcmd->cmd, (GCompareFunc)g_strcmp0);
            }
        } else {
            ordered_commands = g_list_insert_sorted(ordered_commands, pcmd->cmd, (GCompareFunc)g_strcmp0);
        }
    }

    int maxlen = 0;
    GList *curr = ordered_commands;
    while (curr) {
        gchar *cmd = curr->data;
        int len = strlen(cmd);
        if (len > maxlen) maxlen = len;
        curr = g_list_next(curr);
    }

    GString *cmds = g_string_new("");
    curr = ordered_commands;
    int count = 0;
    while (curr) {
        gchar *cmd = curr->data;
        if (count == 5) {
            cons_show(cmds->str);
            g_string_free(cmds, TRUE);
            cmds = g_string_new("");
            count = 0;
        }
        g_string_append_printf(cmds, "%-*s", maxlen + 1, cmd);
        curr = g_list_next(curr);
        count++;
    }
    cons_show(cmds->str);
    g_string_free(cmds, TRUE);
    g_list_free(ordered_commands);
    g_list_free(curr);

    cons_show("");
    cons_show("Use /help [command] without the leading slash, for help on a specific command");
    cons_show("");
}

gboolean
cmd_help(ProfWin *window, const char * const command, gchar **args)
{
    int num_args = g_strv_length(args);
    if (num_args == 0) {
        cons_help();
    } else if (strcmp(args[0], "commands") == 0) {
        if (args[1]) {
            if (!cmd_valid_tag(args[1])) {
                cons_bad_cmd_usage(command);
            } else {
                _cmd_help_cmd_list(args[1]);
            }
        } else {
            _cmd_help_cmd_list(NULL);
        }
    } else if (strcmp(args[0], "navigation") == 0) {
        cons_navigation_help();
    } else {
        char *cmd = args[0];
        char cmd_with_slash[1 + strlen(cmd) + 1];
        sprintf(cmd_with_slash, "/%s", cmd);

        Command *command = g_hash_table_lookup(commands, cmd_with_slash);
        if (command) {
            cons_show_help(command);
        } else {
            cons_show("No such command.");
        }
        cons_show("");
    }

    return TRUE;
}

gboolean
cmd_about(ProfWin *window, const char * const command, gchar **args)
{
    ui_about();
    return TRUE;
}

gboolean
cmd_prefs(ProfWin *window, const char * const command, gchar **args)
{
    if (args[0] == NULL) {
        cons_prefs();
        cons_show("Use the /account command for preferences for individual accounts.");
    } else if (strcmp(args[0], "ui") == 0) {
        cons_show("");
        cons_show_ui_prefs();
        cons_show("");
    } else if (strcmp(args[0], "desktop") == 0) {
        cons_show("");
        cons_show_desktop_prefs();
        cons_show("");
    } else if (strcmp(args[0], "chat") == 0) {
        cons_show("");
        cons_show_chat_prefs();
        cons_show("");
    } else if (strcmp(args[0], "log") == 0) {
        cons_show("");
        cons_show_log_prefs();
        cons_show("");
    } else if (strcmp(args[0], "conn") == 0) {
        cons_show("");
        cons_show_connection_prefs();
        cons_show("");
    } else if (strcmp(args[0], "presence") == 0) {
        cons_show("");
        cons_show_presence_prefs();
        cons_show("");
    } else if (strcmp(args[0], "otr") == 0) {
        cons_show("");
        cons_show_otr_prefs();
        cons_show("");
    } else if (strcmp(args[0], "pgp") == 0) {
        cons_show("");
        cons_show_pgp_prefs();
        cons_show("");
    } else {
        cons_bad_cmd_usage(command);
    }

    return TRUE;
}

gboolean
cmd_theme(ProfWin *window, const char * const command, gchar **args)
{
    // list themes
    if (g_strcmp0(args[0], "list") == 0) {
        GSList *themes = theme_list();
        cons_show_themes(themes);
        g_slist_free_full(themes, g_free);

    // load a theme
    } else if (g_strcmp0(args[0], "load") == 0) {
        if (args[1] == NULL) {
            cons_bad_cmd_usage(command);
        } else if (theme_load(args[1])) {
            ui_load_colours();
            prefs_set_string(PREF_THEME, args[1]);
            if (prefs_get_boolean(PREF_ROSTER)) {
                ui_show_roster();
            } else {
                ui_hide_roster();
            }
            if (prefs_get_boolean(PREF_OCCUPANTS)) {
                ui_show_all_room_rosters();
            } else {
                ui_hide_all_room_rosters();
            }
            ui_redraw();
            cons_show("Loaded theme: %s", args[1]);
        } else {
            cons_show("Couldn't find theme: %s", args[1]);
        }

    // show colours
    } else if (g_strcmp0(args[0], "colours") == 0) {
        cons_theme_colours();
    } else {
        cons_bad_cmd_usage(command);
    }

    return TRUE;
}

static void
_who_room(ProfWin *window, const char * const command, gchar **args)
{
    if ((g_strv_length(args) == 2) && args[1]) {
        cons_show("Argument group is not applicable to chat rooms.");
        return;
    }

    // bad arg
    if (args[0] &&
            (g_strcmp0(args[0], "online") != 0) &&
            (g_strcmp0(args[0], "available") != 0) &&
            (g_strcmp0(args[0], "unavailable") != 0) &&
            (g_strcmp0(args[0], "away") != 0) &&
            (g_strcmp0(args[0], "chat") != 0) &&
            (g_strcmp0(args[0], "xa") != 0) &&
            (g_strcmp0(args[0], "dnd") != 0) &&
            (g_strcmp0(args[0], "any") != 0) &&
            (g_strcmp0(args[0], "moderator") != 0) &&
            (g_strcmp0(args[0], "participant") != 0) &&
            (g_strcmp0(args[0], "visitor") != 0) &&
            (g_strcmp0(args[0], "owner") != 0) &&
            (g_strcmp0(args[0], "admin") != 0) &&
            (g_strcmp0(args[0], "member") != 0) &&
            (g_strcmp0(args[0], "outcast") != 0)) {
        cons_bad_cmd_usage(command);
        return;
    }

    ProfMucWin *mucwin = (ProfMucWin*)window;
    assert(mucwin->memcheck == PROFMUCWIN_MEMCHECK);

    // presence filter
    if (args[0] == NULL ||
            (g_strcmp0(args[0], "online") == 0) ||
            (g_strcmp0(args[0], "available") == 0) ||
            (g_strcmp0(args[0], "unavailable") == 0) ||
            (g_strcmp0(args[0], "away") == 0) ||
            (g_strcmp0(args[0], "chat") == 0) ||
            (g_strcmp0(args[0], "xa") == 0) ||
            (g_strcmp0(args[0], "dnd") == 0) ||
            (g_strcmp0(args[0], "any") == 0)) {

        char *presence = args[0];
        GList *occupants = muc_roster(mucwin->roomjid);

        // no arg, show all contacts
        if ((presence == NULL) || (g_strcmp0(presence, "any") == 0)) {
            ui_room_roster(mucwin->roomjid, occupants, NULL);

        // available
        } else if (strcmp("available", presence) == 0) {
            GList *filtered = NULL;

            while (occupants) {
                Occupant *occupant = occupants->data;
                if (muc_occupant_available(occupant)) {
                    filtered = g_list_append(filtered, occupant);
                }
                occupants = g_list_next(occupants);
            }

            ui_room_roster(mucwin->roomjid, filtered, "available");

        // unavailable
        } else if (strcmp("unavailable", presence) == 0) {
            GList *filtered = NULL;

            while (occupants) {
                Occupant *occupant = occupants->data;
                if (!muc_occupant_available(occupant)) {
                    filtered = g_list_append(filtered, occupant);
                }
                occupants = g_list_next(occupants);
            }

            ui_room_roster(mucwin->roomjid, filtered, "unavailable");

        // show specific status
        } else {
            GList *filtered = NULL;

            while (occupants) {
                Occupant *occupant = occupants->data;
                const char *presence_str = string_from_resource_presence(occupant->presence);
                if (strcmp(presence_str, presence) == 0) {
                    filtered = g_list_append(filtered, occupant);
                }
                occupants = g_list_next(occupants);
            }

            ui_room_roster(mucwin->roomjid, filtered, presence);
        }

        g_list_free(occupants);

    // role or affiliation filter
    } else {
        if (g_strcmp0(args[0], "moderator") == 0) {
            ui_show_room_role_list(mucwin, MUC_ROLE_MODERATOR);
            return;
        }
        if (g_strcmp0(args[0], "participant") == 0) {
            ui_show_room_role_list(mucwin, MUC_ROLE_PARTICIPANT);
            return;
        }
        if (g_strcmp0(args[0], "visitor") == 0) {
            ui_show_room_role_list(mucwin, MUC_ROLE_VISITOR);
            return;
        }

        if (g_strcmp0(args[0], "owner") == 0) {
            ui_show_room_affiliation_list(mucwin, MUC_AFFILIATION_OWNER);
            return;
        }
        if (g_strcmp0(args[0], "admin") == 0) {
            ui_show_room_affiliation_list(mucwin, MUC_AFFILIATION_ADMIN);
            return;
        }
        if (g_strcmp0(args[0], "member") == 0) {
            ui_show_room_affiliation_list(mucwin, MUC_AFFILIATION_MEMBER);
            return;
        }
        if (g_strcmp0(args[0], "outcast") == 0) {
            ui_show_room_affiliation_list(mucwin, MUC_AFFILIATION_OUTCAST);
            return;
        }
    }
}

static void
_who_roster(ProfWin *window, const char * const command, gchar **args)
{
    char *presence = args[0];

    // bad arg
    if (presence
            && (strcmp(presence, "online") != 0)
            && (strcmp(presence, "available") != 0)
            && (strcmp(presence, "unavailable") != 0)
            && (strcmp(presence, "offline") != 0)
            && (strcmp(presence, "away") != 0)
            && (strcmp(presence, "chat") != 0)
            && (strcmp(presence, "xa") != 0)
            && (strcmp(presence, "dnd") != 0)
            && (strcmp(presence, "any") != 0)) {
        cons_bad_cmd_usage(command);
        return;
    }

    char *group = NULL;
    if ((g_strv_length(args) == 2) && args[1]) {
        group = args[1];
    }

    cons_show("");
    GSList *list = NULL;
    if (group) {
        list = roster_get_group(group);
        if (list == NULL) {
            cons_show("No such group: %s.", group);
            return;
        }
    } else {
        list = roster_get_contacts();
        if (list == NULL) {
            cons_show("No contacts in roster.");
            return;
        }
    }

    // no arg, show all contacts
    if ((presence == NULL) || (g_strcmp0(presence, "any") == 0)) {
        if (group) {
            if (list == NULL) {
                cons_show("No contacts in group %s.", group);
            } else {
                cons_show("%s:", group);
                cons_show_contacts(list);
            }
        } else {
            if (list == NULL) {
                cons_show("You have no contacts.");
            } else {
                cons_show("All contacts:");
                cons_show_contacts(list);
            }
        }

    // available
    } else if (strcmp("available", presence) == 0) {
        GSList *filtered = NULL;

        GSList *curr = list;
        while (curr) {
            PContact contact = curr->data;
            if (p_contact_is_available(contact)) {
                filtered = g_slist_append(filtered, contact);
            }
            curr = g_slist_next(curr);
        }

        if (group) {
            if (filtered == NULL) {
                cons_show("No contacts in group %s are %s.", group, presence);
            } else {
                cons_show("%s (%s):", group, presence);
                cons_show_contacts(filtered);
            }
        } else {
            if (filtered == NULL) {
                cons_show("No contacts are %s.", presence);
            } else {
                cons_show("Contacts (%s):", presence);
                cons_show_contacts(filtered);
            }
        }
        g_slist_free(filtered);

    // unavailable
    } else if (strcmp("unavailable", presence) == 0) {
        GSList *filtered = NULL;

        GSList *curr = list;
        while (curr) {
            PContact contact = curr->data;
            if (!p_contact_is_available(contact)) {
                filtered = g_slist_append(filtered, contact);
            }
            curr = g_slist_next(curr);
        }

        if (group) {
            if (filtered == NULL) {
                cons_show("No contacts in group %s are %s.", group, presence);
            } else {
                cons_show("%s (%s):", group, presence);
                cons_show_contacts(filtered);
            }
        } else {
            if (filtered == NULL) {
                cons_show("No contacts are %s.", presence);
            } else {
                cons_show("Contacts (%s):", presence);
                cons_show_contacts(filtered);
            }
        }
        g_slist_free(filtered);

    // online, available resources
    } else if (strcmp("online", presence) == 0) {
        GSList *filtered = NULL;

        GSList *curr = list;
        while (curr) {
            PContact contact = curr->data;
            if (p_contact_has_available_resource(contact)) {
                filtered = g_slist_append(filtered, contact);
            }
            curr = g_slist_next(curr);
        }

        if (group) {
            if (filtered == NULL) {
                cons_show("No contacts in group %s are %s.", group, presence);
            } else {
                cons_show("%s (%s):", group, presence);
                cons_show_contacts(filtered);
            }
        } else {
            if (filtered == NULL) {
                cons_show("No contacts are %s.", presence);
            } else {
                cons_show("Contacts (%s):", presence);
                cons_show_contacts(filtered);
            }
        }
        g_slist_free(filtered);

    // offline, no available resources
    } else if (strcmp("offline", presence) == 0) {
        GSList *filtered = NULL;

        GSList *curr = list;
        while (curr) {
            PContact contact = curr->data;
            if (!p_contact_has_available_resource(contact)) {
                filtered = g_slist_append(filtered, contact);
            }
            curr = g_slist_next(curr);
        }

        if (group) {
            if (filtered == NULL) {
                cons_show("No contacts in group %s are %s.", group, presence);
            } else {
                cons_show("%s (%s):", group, presence);
                cons_show_contacts(filtered);
            }
        } else {
            if (filtered == NULL) {
                cons_show("No contacts are %s.", presence);
            } else {
                cons_show("Contacts (%s):", presence);
                cons_show_contacts(filtered);
            }
        }
        g_slist_free(filtered);

    // show specific status
    } else {
        GSList *filtered = NULL;

        GSList *curr = list;
        while (curr) {
            PContact contact = curr->data;
            if (strcmp(p_contact_presence(contact), presence) == 0) {
                filtered = g_slist_append(filtered, contact);
            }
            curr = g_slist_next(curr);
        }

        if (group) {
            if (filtered == NULL) {
                cons_show("No contacts in group %s are %s.", group, presence);
            } else {
                cons_show("%s (%s):", group, presence);
                cons_show_contacts(filtered);
            }
        } else {
            if (filtered == NULL) {
                cons_show("No contacts are %s.", presence);
            } else {
                cons_show("Contacts (%s):", presence);
                cons_show_contacts(filtered);
            }
        }
        g_slist_free(filtered);
    }

    g_slist_free(list);
}

gboolean
cmd_who(ProfWin *window, const char * const command, gchar **args)
{
    jabber_conn_status_t conn_status = jabber_get_connection_status();

    if (conn_status != JABBER_CONNECTED) {
        cons_show("You are not currently connected.");
    } else if (window->type == WIN_MUC) {
        _who_room(window, command, args);
    } else {
        _who_roster(window, command, args);
    }

    if (window->type != WIN_CONSOLE && window->type != WIN_MUC) {
        ui_statusbar_new(1);
    }

    return TRUE;
}

gboolean
cmd_msg(ProfWin *window, const char * const command, gchar **args)
{
    char *usr = args[0];
    char *msg = args[1];

    jabber_conn_status_t conn_status = jabber_get_connection_status();

    if (conn_status != JABBER_CONNECTED) {
        cons_show("You are not currently connected.");
        return TRUE;
    }

    // send private message when in MUC room
    if (window->type == WIN_MUC) {
        ProfMucWin *mucwin = (ProfMucWin*)window;
        assert(mucwin->memcheck == PROFMUCWIN_MEMCHECK);
        if (muc_roster_contains_nick(mucwin->roomjid, usr)) {
            GString *full_jid = g_string_new(mucwin->roomjid);
            g_string_append(full_jid, "/");
            g_string_append(full_jid, usr);

            ProfPrivateWin *privwin = wins_get_private(full_jid->str);
            if (!privwin) {
                privwin = ui_ev_new_private_win(full_jid->str);
            }
            ui_ev_focus_win((ProfWin*)privwin);

            if (msg) {
                cl_ev_send_priv_msg(privwin, msg);
            }

            g_string_free(full_jid, TRUE);

        } else {
            ui_current_print_line("No such participant \"%s\" in room.", usr);
        }

        return TRUE;

    // send chat message
    } else {
        char *barejid = roster_barejid_from_name(usr);
        if (barejid == NULL) {
            barejid = usr;
        }

        ProfChatWin *chatwin = wins_get_chat(barejid);
        if (!chatwin) {
            chatwin = ui_ev_new_chat_win(barejid);
        }
        ui_ev_focus_win((ProfWin*)chatwin);

        if (msg) {
            cl_ev_send_msg(chatwin, msg);
        } else {
#ifdef HAVE_LIBOTR
            if (otr_is_secure(barejid)) {
                ui_gone_secure(barejid, otr_is_trusted(barejid));
            }
#endif
        }

        return TRUE;
    }
}

gboolean
cmd_group(ProfWin *window, const char * const command, gchar **args)
{
    jabber_conn_status_t conn_status = jabber_get_connection_status();

    if (conn_status != JABBER_CONNECTED) {
        cons_show("You are not currently connected.");
        return TRUE;
    }

    // list all groups
    if (args[0] == NULL) {
        GSList *groups = roster_get_groups();
        GSList *curr = groups;
        if (curr) {
            cons_show("Groups:");
            while (curr) {
                cons_show("  %s", curr->data);
                curr = g_slist_next(curr);
            }

            g_slist_free_full(groups, g_free);
        } else {
            cons_show("No groups.");
        }
        return TRUE;
    }

    // show contacts in group
    if (strcmp(args[0], "show") == 0) {
        char *group = args[1];
        if (group == NULL) {
            cons_bad_cmd_usage(command);
            return TRUE;
        }

        GSList *list = roster_get_group(group);
        cons_show_roster_group(group, list);
        return TRUE;
    }

    // add contact to group
    if (strcmp(args[0], "add") == 0) {
        char *group = args[1];
        char *contact = args[2];

        if ((group == NULL) || (contact == NULL)) {
            cons_bad_cmd_usage(command);
            return TRUE;
        }

        char *barejid = roster_barejid_from_name(contact);
        if (barejid == NULL) {
            barejid = contact;
        }

        PContact pcontact = roster_get_contact(barejid);
        if (pcontact == NULL) {
            cons_show("Contact not found in roster: %s", barejid);
            return TRUE;
        }

        if (p_contact_in_group(pcontact, group)) {
            const char *display_name = p_contact_name_or_jid(pcontact);
            ui_contact_already_in_group(display_name, group);
        } else {
            roster_send_add_to_group(group, pcontact);
        }

        return TRUE;
    }

    // remove contact from group
    if (strcmp(args[0], "remove") == 0) {
        char *group = args[1];
        char *contact = args[2];

        if ((group == NULL) || (contact == NULL)) {
            cons_bad_cmd_usage(command);
            return TRUE;
        }

        char *barejid = roster_barejid_from_name(contact);
        if (barejid == NULL) {
            barejid = contact;
        }

        PContact pcontact = roster_get_contact(barejid);
        if (pcontact == NULL) {
            cons_show("Contact not found in roster: %s", barejid);
            return TRUE;
        }

        if (!p_contact_in_group(pcontact, group)) {
            const char *display_name = p_contact_name_or_jid(pcontact);
            ui_contact_not_in_group(display_name, group);
        } else {
            roster_send_remove_from_group(group, pcontact);
        }

        return TRUE;
    }

    cons_bad_cmd_usage(command);
    return TRUE;
}

gboolean
cmd_roster(ProfWin *window, const char * const command, gchar **args)
{
    jabber_conn_status_t conn_status = jabber_get_connection_status();

    // show roster
    if (args[0] == NULL) {
        if (conn_status != JABBER_CONNECTED) {
            cons_show("You are not currently connected.");
            return TRUE;
        }

        GSList *list = roster_get_contacts();
        cons_show_roster(list);
        g_slist_free(list);
        return TRUE;

    // show roster, only online contacts
    } else if(g_strcmp0(args[0], "online") == 0){
        if (conn_status != JABBER_CONNECTED) {
            cons_show("You are not currently connected.");
            return TRUE;
        }

        GSList *list = roster_get_contacts_online();
        cons_show_roster(list);
        g_slist_free(list);
        return TRUE;

    // set roster size
    } else if (g_strcmp0(args[0], "size") == 0) {
        if (!args[1]) {
            cons_bad_cmd_usage(command);
            return TRUE;
        }
        int intval = 0;
        char *err_msg = NULL;
        gboolean res = strtoi_range(args[1], &intval, 1, 99, &err_msg);
        if (res) {
            prefs_set_roster_size(intval);
            cons_show("Roster screen size set to: %d%%", intval);
            if (conn_status == JABBER_CONNECTED && prefs_get_boolean(PREF_ROSTER)) {
                wins_resize_all();
            }
            return TRUE;
        } else {
            cons_show(err_msg);
            free(err_msg);
            return TRUE;
        }

    // show/hide roster
    } else if (g_strcmp0(args[0], "show") == 0) {
        if (args[1] == NULL) {
            cons_show("Roster enabled.");
            prefs_set_boolean(PREF_ROSTER, TRUE);
            if (conn_status == JABBER_CONNECTED) {
                ui_show_roster();
            }
            return TRUE;
        } else if (g_strcmp0(args[1], "offline") == 0) {
            cons_show("Roster offline enabled");
            prefs_set_boolean(PREF_ROSTER_OFFLINE, TRUE);
            if (conn_status == JABBER_CONNECTED) {
                rosterwin_roster();
            }
            return TRUE;
        } else if (g_strcmp0(args[1], "resource") == 0) {
            cons_show("Roster resource enabled");
            prefs_set_boolean(PREF_ROSTER_RESOURCE, TRUE);
            if (conn_status == JABBER_CONNECTED) {
                rosterwin_roster();
            }
            return TRUE;
        } else if (g_strcmp0(args[1], "empty") == 0) {
            cons_show("Roster empty enabled");
            prefs_set_boolean(PREF_ROSTER_EMPTY, TRUE);
            if (conn_status == JABBER_CONNECTED) {
                rosterwin_roster();
            }
            return TRUE;
        } else {
            cons_bad_cmd_usage(command);
            return TRUE;
        }
    } else if (g_strcmp0(args[0], "hide") == 0) {
        if (args[1] == NULL) {
            cons_show("Roster disabled.");
            prefs_set_boolean(PREF_ROSTER, FALSE);
            if (conn_status == JABBER_CONNECTED) {
                ui_hide_roster();
            }
            return TRUE;
        } else if (g_strcmp0(args[1], "offline") == 0) {
            cons_show("Roster offline disabled");
            prefs_set_boolean(PREF_ROSTER_OFFLINE, FALSE);
            if (conn_status == JABBER_CONNECTED) {
                rosterwin_roster();
            }
            return TRUE;
        } else if (g_strcmp0(args[1], "resource") == 0) {
            cons_show("Roster resource disabled");
            prefs_set_boolean(PREF_ROSTER_RESOURCE, FALSE);
            if (conn_status == JABBER_CONNECTED) {
                rosterwin_roster();
            }
            return TRUE;
        } else if (g_strcmp0(args[1], "empty") == 0) {
            cons_show("Roster empty disabled");
            prefs_set_boolean(PREF_ROSTER_EMPTY, FALSE);
            if (conn_status == JABBER_CONNECTED) {
                rosterwin_roster();
            }
            return TRUE;
        } else {
            cons_bad_cmd_usage(command);
            return TRUE;
        }
    // roster grouping
    } else if (g_strcmp0(args[0], "by") == 0) {
        if (g_strcmp0(args[1], "group") == 0) {
            cons_show("Grouping roster by roster group");
            prefs_set_string(PREF_ROSTER_BY, "group");
            if (conn_status == JABBER_CONNECTED) {
                rosterwin_roster();
            }
            return TRUE;
        } else if (g_strcmp0(args[1], "presence") == 0) {
            cons_show("Grouping roster by presence");
            prefs_set_string(PREF_ROSTER_BY, "presence");
            if (conn_status == JABBER_CONNECTED) {
                rosterwin_roster();
            }
            return TRUE;
        } else if (g_strcmp0(args[1], "none") == 0) {
            cons_show("Roster grouping disabled");
            prefs_set_string(PREF_ROSTER_BY, "none");
            if (conn_status == JABBER_CONNECTED) {
                rosterwin_roster();
            }
            return TRUE;
        } else {
            cons_bad_cmd_usage(command);
            return TRUE;
        }
    // add contact
    } else if (strcmp(args[0], "add") == 0) {
        if (conn_status != JABBER_CONNECTED) {
            cons_show("You are not currently connected.");
            return TRUE;
        }
        char *jid = args[1];
        if (jid == NULL) {
            cons_bad_cmd_usage(command);
        } else {
            char *name = args[2];
            roster_send_add_new(jid, name);
        }
        return TRUE;

    // remove contact
    } else if (strcmp(args[0], "remove") == 0) {
        if (conn_status != JABBER_CONNECTED) {
            cons_show("You are not currently connected.");
            return TRUE;
        }
        char *jid = args[1];
        if (jid == NULL) {
            cons_bad_cmd_usage(command);
        } else {
            roster_send_remove(jid);
        }
        return TRUE;

    } else if (strcmp(args[0], "remove_all") == 0) {
        if (g_strcmp0(args[1], "contacts") != 0) {
            cons_bad_cmd_usage(command);
            return TRUE;
        }
        if (conn_status != JABBER_CONNECTED) {
            cons_show("You are not currently connected.");
            return TRUE;
        }

        GSList *all = roster_get_contacts();
        GSList *curr = all;
        while (curr) {
            PContact contact = curr->data;
            roster_send_remove(p_contact_barejid(contact));
            curr = g_slist_next(curr);
        }

        g_slist_free(all);
        return TRUE;

    // change nickname
    } else if (strcmp(args[0], "nick") == 0) {
        if (conn_status != JABBER_CONNECTED) {
            cons_show("You are not currently connected.");
            return TRUE;
        }
        char *jid = args[1];
        if (jid == NULL) {
            cons_bad_cmd_usage(command);
            return TRUE;
        }

        char *name = args[2];
        if (name == NULL) {
            cons_bad_cmd_usage(command);
            return TRUE;
        }

        // contact does not exist
        PContact contact = roster_get_contact(jid);
        if (contact == NULL) {
            cons_show("Contact not found in roster: %s", jid);
            return TRUE;
        }

        const char *barejid = p_contact_barejid(contact);
        roster_change_name(contact, name);
        GSList *groups = p_contact_groups(contact);
        roster_send_name_change(barejid, name, groups);

        cons_show("Nickname for %s set to: %s.", jid, name);

        return TRUE;

    // remove nickname
    } else if (strcmp(args[0], "clearnick") == 0) {
        if (conn_status != JABBER_CONNECTED) {
            cons_show("You are not currently connected.");
            return TRUE;
        }
        char *jid = args[1];
        if (jid == NULL) {
            cons_bad_cmd_usage(command);
            return TRUE;
        }

        // contact does not exist
        PContact contact = roster_get_contact(jid);
        if (contact == NULL) {
            cons_show("Contact not found in roster: %s", jid);
            return TRUE;
        }

        const char *barejid = p_contact_barejid(contact);
        roster_change_name(contact, NULL);
        GSList *groups = p_contact_groups(contact);
        roster_send_name_change(barejid, NULL, groups);

        cons_show("Nickname for %s removed.", jid);

        return TRUE;
    } else {
        cons_bad_cmd_usage(command);
        return TRUE;
    }
}

gboolean
cmd_resource(ProfWin *window, const char * const command, gchar **args)
{
    char *cmd = args[0];
    char *setting = NULL;
    if (g_strcmp0(cmd, "message") == 0) {
        setting = args[1];
        if (!setting) {
            cons_bad_cmd_usage(command);
            return TRUE;
        } else {
            return _cmd_set_boolean_preference(setting, command, "Message resource", PREF_RESOURCE_MESSAGE);
        }
    } else if (g_strcmp0(cmd, "title") == 0) {
        setting = args[1];
        if (!setting) {
            cons_bad_cmd_usage(command);
            return TRUE;
        } else {
            return _cmd_set_boolean_preference(setting, command, "Title resource", PREF_RESOURCE_TITLE);
        }
    }

    if (window->type != WIN_CHAT) {
        cons_show("Resource can only be changed in chat windows.");
        return TRUE;
    }
    ProfChatWin *chatwin = (ProfChatWin*)window;

    if (g_strcmp0(cmd, "set") == 0) {
        char *resource = args[1];
        if (!resource) {
            cons_bad_cmd_usage(command);
            return TRUE;
        }

#ifdef HAVE_LIBOTR
        if (otr_is_secure(chatwin->barejid)) {
            cons_show("Cannot choose resource during an OTR session.");
            return TRUE;
        }
#endif

        PContact contact = roster_get_contact(chatwin->barejid);
        if (!contact) {
            cons_show("Cannot choose resource for contact not in roster.");
            return TRUE;
        }

        if (!p_contact_get_resource(contact, resource)) {
            cons_show("No such resource %s.", resource);
            return TRUE;
        }

        chatwin->resource_override = strdup(resource);
        chat_state_free(chatwin->state);
        chatwin->state = chat_state_new();
        chat_session_resource_override(chatwin->barejid, resource);
        return TRUE;

    } else if (g_strcmp0(cmd, "off") == 0) {
        FREE_SET_NULL(chatwin->resource_override);
        chat_state_free(chatwin->state);
        chatwin->state = chat_state_new();
        chat_session_remove(chatwin->barejid);
        return TRUE;
    } else {
        cons_bad_cmd_usage(command);
        return TRUE;
    }
}

gboolean
cmd_status(ProfWin *window, const char * const command, gchar **args)
{
    char *usr = args[0];

    jabber_conn_status_t conn_status = jabber_get_connection_status();

    if (conn_status != JABBER_CONNECTED) {
        cons_show("You are not currently connected.");
        return TRUE;
    }

    switch (window->type)
    {
        case WIN_MUC:
            if (usr) {
                ProfMucWin *mucwin = (ProfMucWin*)window;
                assert(mucwin->memcheck == PROFMUCWIN_MEMCHECK);
                Occupant *occupant = muc_roster_item(mucwin->roomjid, usr);
                if (occupant) {
                    win_show_occupant(window, occupant);
                } else {
                    win_vprint(window, '-', 0, NULL, 0, 0, "", "No such participant \"%s\" in room.", usr);
                }
            } else {
                ui_current_print_line("You must specify a nickname.");
            }
            break;
        case WIN_CHAT:
            if (usr) {
                ui_current_print_line("No parameter required when in chat.");
            } else {
                ProfChatWin *chatwin = (ProfChatWin*)window;
                assert(chatwin->memcheck == PROFCHATWIN_MEMCHECK);
                PContact pcontact = roster_get_contact(chatwin->barejid);
                if (pcontact) {
                    win_show_contact(window, pcontact);
                } else {
                    win_println(window, 0, "Error getting contact info.");
                }
            }
            break;
        case WIN_PRIVATE:
            if (usr) {
                ui_current_print_line("No parameter required when in chat.");
            } else {
                ProfPrivateWin *privatewin = (ProfPrivateWin*)window;
                assert(privatewin->memcheck == PROFPRIVATEWIN_MEMCHECK);
                Jid *jid = jid_create(privatewin->fulljid);
                Occupant *occupant = muc_roster_item(jid->barejid, jid->resourcepart);
                if (occupant) {
                    win_show_occupant(window, occupant);
                } else {
                    win_println(window, 0, "Error getting contact info.");
                }
                jid_destroy(jid);
            }
            break;
        case WIN_CONSOLE:
            if (usr) {
                char *usr_jid = roster_barejid_from_name(usr);
                if (usr_jid == NULL) {
                    usr_jid = usr;
                }
                cons_show_status(usr_jid);
            } else {
                cons_bad_cmd_usage(command);
            }
            break;
        default:
            break;
    }

    return TRUE;
}

gboolean
cmd_info(ProfWin *window, const char * const command, gchar **args)
{
    char *usr = args[0];

    jabber_conn_status_t conn_status = jabber_get_connection_status();
    PContact pcontact = NULL;

    if (conn_status != JABBER_CONNECTED) {
        cons_show("You are not currently connected.");
        return TRUE;
    }

    switch (window->type)
    {
        case WIN_MUC:
            if (usr) {
                ProfMucWin *mucwin = (ProfMucWin*)window;
                assert(mucwin->memcheck == PROFMUCWIN_MEMCHECK);
                Occupant *occupant = muc_roster_item(mucwin->roomjid, usr);
                if (occupant) {
                    win_show_occupant_info(window, mucwin->roomjid, occupant);
                } else {
                    ui_current_print_line("No such occupant \"%s\" in room.", usr);
                }
            } else {
                ProfMucWin *mucwin = (ProfMucWin*)window;
                assert(mucwin->memcheck == PROFMUCWIN_MEMCHECK);
                iq_room_info_request(mucwin->roomjid, TRUE);
                ui_show_room_info(mucwin);
                return TRUE;
            }
            break;
        case WIN_CHAT:
            if (usr) {
                ui_current_print_line("No parameter required when in chat.");
            } else {
                ProfChatWin *chatwin = (ProfChatWin*)window;
                assert(chatwin->memcheck == PROFCHATWIN_MEMCHECK);
                PContact pcontact = roster_get_contact(chatwin->barejid);
                if (pcontact) {
                    win_show_info(window, pcontact);
                } else {
                    win_println(window, 0, "Error getting contact info.");
                }
            }
            break;
        case WIN_PRIVATE:
            if (usr) {
                ui_current_print_line("No parameter required when in chat.");
            } else {
                ProfPrivateWin *privatewin = (ProfPrivateWin*)window;
                assert(privatewin->memcheck == PROFPRIVATEWIN_MEMCHECK);
                Jid *jid = jid_create(privatewin->fulljid);
                Occupant *occupant = muc_roster_item(jid->barejid, jid->resourcepart);
                if (occupant) {
                    win_show_occupant_info(window, jid->barejid, occupant);
                } else {
                    win_println(window, 0, "Error getting contact info.");
                }
                jid_destroy(jid);
            }
            break;
        case WIN_CONSOLE:
            if (usr) {
                char *usr_jid = roster_barejid_from_name(usr);
                if (usr_jid == NULL) {
                    usr_jid = usr;
                }
                pcontact = roster_get_contact(usr_jid);
                if (pcontact) {
                    cons_show_info(pcontact);
                } else {
                    cons_show("No such contact \"%s\" in roster.", usr);
                }
            } else {
                cons_bad_cmd_usage(command);
            }
            break;
        default:
            break;
    }

    return TRUE;
}

gboolean
cmd_caps(ProfWin *window, const char * const command, gchar **args)
{
    jabber_conn_status_t conn_status = jabber_get_connection_status();
    PContact pcontact = NULL;
    Occupant *occupant = NULL;

    if (conn_status != JABBER_CONNECTED) {
        cons_show("You are not currently connected.");
        return TRUE;
    }

    switch (window->type)
    {
        case WIN_MUC:
            if (args[0]) {
                ProfMucWin *mucwin = (ProfMucWin*)window;
                assert(mucwin->memcheck == PROFMUCWIN_MEMCHECK);
                occupant = muc_roster_item(mucwin->roomjid, args[0]);
                if (occupant) {
                    Jid *jidp = jid_create_from_bare_and_resource(mucwin->roomjid, args[0]);
                    cons_show_caps(jidp->fulljid, occupant->presence);
                    jid_destroy(jidp);
                } else {
                    cons_show("No such participant \"%s\" in room.", args[0]);
                }
            } else {
                cons_show("No nickname supplied to /caps in chat room.");
            }
            break;
        case WIN_CHAT:
        case WIN_CONSOLE:
            if (args[0]) {
                Jid *jid = jid_create(args[0]);

                if (jid->fulljid == NULL) {
                    cons_show("You must provide a full jid to the /caps command.");
                } else {
                    pcontact = roster_get_contact(jid->barejid);
                    if (pcontact == NULL) {
                        cons_show("Contact not found in roster: %s", jid->barejid);
                    } else {
                        Resource *resource = p_contact_get_resource(pcontact, jid->resourcepart);
                        if (resource == NULL) {
                            cons_show("Could not find resource %s, for contact %s", jid->barejid, jid->resourcepart);
                        } else {
                            cons_show_caps(jid->fulljid, resource->presence);
                        }
                    }
                }
                jid_destroy(jid);
            } else {
                cons_show("You must provide a jid to the /caps command.");
            }
            break;
        case WIN_PRIVATE:
            if (args[0]) {
                cons_show("No parameter needed to /caps when in private chat.");
            } else {
                ProfPrivateWin *privatewin = (ProfPrivateWin*)window;
                assert(privatewin->memcheck == PROFPRIVATEWIN_MEMCHECK);
                Jid *jid = jid_create(privatewin->fulljid);
                if (jid) {
                    occupant = muc_roster_item(jid->barejid, jid->resourcepart);
                    cons_show_caps(jid->resourcepart, occupant->presence);
                    jid_destroy(jid);
                }
            }
            break;
        default:
            break;
    }

    return TRUE;
}


gboolean
cmd_software(ProfWin *window, const char * const command, gchar **args)
{
    jabber_conn_status_t conn_status = jabber_get_connection_status();
    Occupant *occupant = NULL;

    if (conn_status != JABBER_CONNECTED) {
        cons_show("You are not currently connected.");
        return TRUE;
    }

    switch (window->type)
    {
        case WIN_MUC:
            if (args[0]) {
                ProfMucWin *mucwin = (ProfMucWin*)window;
                assert(mucwin->memcheck == PROFMUCWIN_MEMCHECK);
                occupant = muc_roster_item(mucwin->roomjid, args[0]);
                if (occupant) {
                    Jid *jid = jid_create_from_bare_and_resource(mucwin->roomjid, args[0]);
                    iq_send_software_version(jid->fulljid);
                    jid_destroy(jid);
                } else {
                    cons_show("No such participant \"%s\" in room.", args[0]);
                }
            } else {
                cons_show("No nickname supplied to /software in chat room.");
            }
            break;
        case WIN_CHAT:
            if (args[0]) {
                cons_show("No parameter needed to /software when in chat.");
            } else {
                ProfChatWin *chatwin = (ProfChatWin*)window;
                assert(chatwin->memcheck == PROFCHATWIN_MEMCHECK);

                char *resource = NULL;
                ChatSession *session = chat_session_get(chatwin->barejid);
                if (chatwin->resource_override) {
                    resource = chatwin->resource_override;
                } else if (session && session->resource) {
                    resource = session->resource;
                }

                if (resource) {
                    GString *fulljid = g_string_new(chatwin->barejid);
                    g_string_append_printf(fulljid, "/%s", resource);
                    iq_send_software_version(fulljid->str);
                    g_string_free(fulljid, TRUE);
                } else {
                    win_println(window, 0, "Unknown resource for /software command.");
                }
            }
            break;
        case WIN_CONSOLE:
            if (args[0]) {
                Jid *myJid = jid_create(jabber_get_fulljid());
                Jid *jid = jid_create(args[0]);

                if (jid == NULL || jid->fulljid == NULL) {
                    cons_show("You must provide a full jid to the /software command.");
                } else if (g_strcmp0(jid->barejid, myJid->barejid) == 0) {
                    cons_show("Cannot request software version for yourself.");
                } else {
                    iq_send_software_version(jid->fulljid);
                }
                jid_destroy(myJid);
                jid_destroy(jid);
            } else {
                cons_show("You must provide a jid to the /software command.");
            }
            break;
        case WIN_PRIVATE:
            if (args[0]) {
                cons_show("No parameter needed to /software when in private chat.");
            } else {
                ProfPrivateWin *privatewin = (ProfPrivateWin*)window;
                assert(privatewin->memcheck == PROFPRIVATEWIN_MEMCHECK);
                iq_send_software_version(privatewin->fulljid);
            }
            break;
        default:
            break;
    }

    return TRUE;
}

gboolean
cmd_join(ProfWin *window, const char * const command, gchar **args)
{
    jabber_conn_status_t conn_status = jabber_get_connection_status();
    if (conn_status != JABBER_CONNECTED) {
        cons_show("You are not currently connected.");
        return TRUE;
    }

    if (args[0] == NULL) {
        uuid_t uuid;
        uuid_generate(uuid);
        char *uuid_str = malloc(sizeof(char) * 37);
        uuid_unparse_lower(uuid, uuid_str);

        char *account_name = jabber_get_account_name();
        ProfAccount *account = accounts_get_account(account_name);

        GString *room_str = g_string_new("");
        g_string_append_printf(room_str, "private-chat-%s@%s", uuid_str, account->muc_service);

        presence_join_room(room_str->str, account->muc_nick, NULL);
        muc_join(room_str->str, account->muc_nick, NULL, FALSE);

        g_string_free(room_str, TRUE);
        free(uuid_str);
        account_free(account);

        return TRUE;
    }

    Jid *room_arg = jid_create(args[0]);
    if (room_arg == NULL) {
        cons_show_error("Specified room has incorrect format.");
        cons_show("");
        return TRUE;
    }

    char *room = NULL;
    char *nick = NULL;
    char *passwd = NULL;
    GString *room_str = g_string_new("");
    char *account_name = jabber_get_account_name();
    ProfAccount *account = accounts_get_account(account_name);

    // full room jid supplied (room@server)
    if (room_arg->localpart) {
        room = args[0];

    // server not supplied (room), use account preference
    } else {
        g_string_append(room_str, args[0]);
        g_string_append(room_str, "@");
        g_string_append(room_str, account->muc_service);
        room = room_str->str;
    }

    // Additional args supplied
    gchar *opt_keys[] = { "nick", "password", NULL };
    gboolean parsed;

    GHashTable *options = parse_options(&args[1], opt_keys, &parsed);
    if (!parsed) {
        cons_bad_cmd_usage(command);
        cons_show("");
        jid_destroy(room_arg);
        return TRUE;
    }

    nick = g_hash_table_lookup(options, "nick");
    passwd = g_hash_table_lookup(options, "password");

    options_destroy(options);

    // In the case that a nick wasn't provided by the optional args...
    if (!nick) {
        nick = account->muc_nick;
    }

    // When no password, check for invite with password
    if (!passwd) {
        passwd = muc_invite_password(room);
    }

    if (!muc_active(room)) {
        presence_join_room(room, nick, passwd);
        muc_join(room, nick, passwd, FALSE);
    } else if (muc_roster_complete(room)) {
        ui_switch_to_room(room);
    }

    jid_destroy(room_arg);
    g_string_free(room_str, TRUE);
    account_free(account);

    return TRUE;
}

gboolean
cmd_invite(ProfWin *window, const char * const command, gchar **args)
{
    char *contact = args[0];
    char *reason = args[1];
    jabber_conn_status_t conn_status = jabber_get_connection_status();

    if (conn_status != JABBER_CONNECTED) {
        cons_show("You are not currently connected.");
        return TRUE;
    }

    if (window->type != WIN_MUC) {
        cons_show("You must be in a chat room to send an invite.");
        return TRUE;
    }

    char *usr_jid = roster_barejid_from_name(contact);
    if (usr_jid == NULL) {
        usr_jid = contact;
    }

    ProfMucWin *mucwin = (ProfMucWin*)window;
    assert(mucwin->memcheck == PROFMUCWIN_MEMCHECK);
    message_send_invite(mucwin->roomjid, usr_jid, reason);
    if (reason) {
        cons_show("Room invite sent, contact: %s, room: %s, reason: \"%s\".",
            contact, mucwin->roomjid, reason);
    } else {
        cons_show("Room invite sent, contact: %s, room: %s.",
            contact, mucwin->roomjid);
    }

    return TRUE;
}

gboolean
cmd_invites(ProfWin *window, const char * const command, gchar **args)
{
    GSList *invites = muc_invites();
    cons_show_room_invites(invites);
    g_slist_free_full(invites, g_free);
    return TRUE;
}

gboolean
cmd_decline(ProfWin *window, const char * const command, gchar **args)
{
    if (!muc_invites_contain(args[0])) {
        cons_show("No such invite exists.");
    } else {
        muc_invites_remove(args[0]);
        cons_show("Declined invite to %s.", args[0]);
    }

    return TRUE;
}

gboolean
cmd_form_field(ProfWin *window, char *tag, gchar **args)
{
    if (window->type != WIN_MUC_CONFIG) {
        return TRUE;
    }

    ProfMucConfWin *confwin = (ProfMucConfWin*)window;
    DataForm *form = confwin->form;
    if (form) {
        if (!form_tag_exists(form, tag)) {
            ui_current_print_line("Form does not contain a field with tag %s", tag);
            return TRUE;
        }

        form_field_type_t field_type = form_get_field_type(form, tag);
        char *cmd = NULL;
        char *value = NULL;
        gboolean valid = FALSE;
        gboolean added = FALSE;
        gboolean removed = FALSE;

        switch (field_type) {
        case FIELD_BOOLEAN:
            value = args[0];
            if (g_strcmp0(value, "on") == 0) {
                form_set_value(form, tag, "1");
                ui_current_print_line("Field updated...");
                ui_show_form_field(window, form, tag);
            } else if (g_strcmp0(value, "off") == 0) {
                form_set_value(form, tag, "0");
                ui_current_print_line("Field updated...");
                ui_show_form_field(window, form, tag);
            } else {
                ui_current_print_line("Invalid command, usage:");
                ui_show_form_field_help(confwin, tag);
                ui_current_print_line("");
            }
            break;

        case FIELD_TEXT_PRIVATE:
        case FIELD_TEXT_SINGLE:
        case FIELD_JID_SINGLE:
            value = args[0];
            if (value == NULL) {
                ui_current_print_line("Invalid command, usage:");
                ui_show_form_field_help(confwin, tag);
                ui_current_print_line("");
            } else {
                form_set_value(form, tag, value);
                ui_current_print_line("Field updated...");
                ui_show_form_field(window, form, tag);
            }
            break;
        case FIELD_LIST_SINGLE:
            value = args[0];
            if ((value == NULL) || !form_field_contains_option(form, tag, value)) {
                ui_current_print_line("Invalid command, usage:");
                ui_show_form_field_help(confwin, tag);
                ui_current_print_line("");
            } else {
                form_set_value(form, tag, value);
                ui_current_print_line("Field updated...");
                ui_show_form_field(window, form, tag);
            }
            break;

        case FIELD_TEXT_MULTI:
            cmd = args[0];
            if (cmd) {
                value = args[1];
            }
            if ((g_strcmp0(cmd, "add") != 0) && (g_strcmp0(cmd, "remove"))) {
                ui_current_print_line("Invalid command, usage:");
                ui_show_form_field_help(confwin, tag);
                ui_current_print_line("");
                break;
            }
            if (value == NULL) {
                ui_current_print_line("Invalid command, usage:");
                ui_show_form_field_help(confwin, tag);
                ui_current_print_line("");
                break;
            }
            if (g_strcmp0(cmd, "add") == 0) {
                form_add_value(form, tag, value);
                ui_current_print_line("Field updated...");
                ui_show_form_field(window, form, tag);
                break;
            }
            if (g_strcmp0(args[0], "remove") == 0) {
                if (!g_str_has_prefix(value, "val")) {
                    ui_current_print_line("Invalid command, usage:");
                    ui_show_form_field_help(confwin, tag);
                    ui_current_print_line("");
                    break;
                }
                if (strlen(value) < 4) {
                    ui_current_print_line("Invalid command, usage:");
                    ui_show_form_field_help(confwin, tag);
                    ui_current_print_line("");
                    break;
                }

                int index = strtol(&value[3], NULL, 10);
                if ((index < 1) || (index > form_get_value_count(form, tag))) {
                    ui_current_print_line("Invalid command, usage:");
                    ui_show_form_field_help(confwin, tag);
                    ui_current_print_line("");
                    break;
                }

                removed = form_remove_text_multi_value(form, tag, index);
                if (removed) {
                    ui_current_print_line("Field updated...");
                    ui_show_form_field(window, form, tag);
                } else {
                    ui_current_print_line("Could not remove %s from %s", value, tag);
                }
            }
            break;
        case FIELD_LIST_MULTI:
            cmd = args[0];
            if (cmd) {
                value = args[1];
            }
            if ((g_strcmp0(cmd, "add") != 0) && (g_strcmp0(cmd, "remove"))) {
                ui_current_print_line("Invalid command, usage:");
                ui_show_form_field_help(confwin, tag);
                ui_current_print_line("");
                break;
            }
            if (value == NULL) {
                ui_current_print_line("Invalid command, usage:");
                ui_show_form_field_help(confwin, tag);
                ui_current_print_line("");
                break;
            }
            if (g_strcmp0(args[0], "add") == 0) {
                valid = form_field_contains_option(form, tag, value);
                if (valid) {
                    added = form_add_unique_value(form, tag, value);
                    if (added) {
                        ui_current_print_line("Field updated...");
                        ui_show_form_field(window, form, tag);
                    } else {
                        ui_current_print_line("Value %s already selected for %s", value, tag);
                    }
                } else {
                    ui_current_print_line("Invalid command, usage:");
                    ui_show_form_field_help(confwin, tag);
                    ui_current_print_line("");
                }
                break;
            }
            if (g_strcmp0(args[0], "remove") == 0) {
                valid = form_field_contains_option(form, tag, value);
                if (valid == TRUE) {
                    removed = form_remove_value(form, tag, value);
                    if (removed) {
                        ui_current_print_line("Field updated...");
                        ui_show_form_field(window, form, tag);
                    } else {
                        ui_current_print_line("Value %s is not currently set for %s", value, tag);
                    }
                } else {
                    ui_current_print_line("Invalid command, usage:");
                    ui_show_form_field_help(confwin, tag);
                    ui_current_print_line("");
                }
            }
            break;
        case FIELD_JID_MULTI:
            cmd = args[0];
            if (cmd) {
                value = args[1];
            }
            if ((g_strcmp0(cmd, "add") != 0) && (g_strcmp0(cmd, "remove"))) {
                ui_current_print_line("Invalid command, usage:");
                ui_show_form_field_help(confwin, tag);
                ui_current_print_line("");
                break;
            }
            if (value == NULL) {
                ui_current_print_line("Invalid command, usage:");
                ui_show_form_field_help(confwin, tag);
                ui_current_print_line("");
                break;
            }
            if (g_strcmp0(args[0], "add") == 0) {
                added = form_add_unique_value(form, tag, value);
                if (added) {
                    ui_current_print_line("Field updated...");
                    ui_show_form_field(window, form, tag);
                } else {
                    ui_current_print_line("JID %s already exists in %s", value, tag);
                }
                break;
            }
            if (g_strcmp0(args[0], "remove") == 0) {
                removed = form_remove_value(form, tag, value);
                if (removed) {
                    ui_current_print_line("Field updated...");
                    ui_show_form_field(window, form, tag);
                } else {
                    ui_current_print_line("Field %s does not contain %s", tag, value);
                }
            }
            break;

        default:
            break;
        }
    }

    return TRUE;
}

gboolean
cmd_form(ProfWin *window, const char * const command, gchar **args)
{
    jabber_conn_status_t conn_status = jabber_get_connection_status();

    if (conn_status != JABBER_CONNECTED) {
        cons_show("You are not currently connected.");
        return TRUE;
    }

    if (window->type != WIN_MUC_CONFIG) {
        cons_show("Command '/form' does not apply to this window.");
        return TRUE;
    }

    if ((g_strcmp0(args[0], "submit") != 0) &&
            (g_strcmp0(args[0], "cancel") != 0) &&
            (g_strcmp0(args[0], "show") != 0) &&
            (g_strcmp0(args[0], "help") != 0)) {
        cons_bad_cmd_usage(command);
        return TRUE;
    }

    ProfMucConfWin *confwin = (ProfMucConfWin*)window;
    assert(confwin->memcheck == PROFCONFWIN_MEMCHECK);

    if (g_strcmp0(args[0], "show") == 0) {
        ui_show_form(confwin);
        return TRUE;
    }

    if (g_strcmp0(args[0], "help") == 0) {
        char *tag = args[1];
        if (tag) {
            ui_show_form_field_help(confwin, tag);
        } else {
            ui_show_form_help(confwin);

            const gchar **help_text = NULL;
            Command *command = g_hash_table_lookup(commands, "/form");

            if (command) {
                help_text = command->help.synopsis;
            }

            ui_show_lines((ProfWin*) confwin, help_text);
        }
        ui_current_print_line("");
        return TRUE;
    }

    if (g_strcmp0(args[0], "submit") == 0) {
        iq_submit_room_config(confwin->roomjid, confwin->form);
    }

    if (g_strcmp0(args[0], "cancel") == 0) {
        iq_room_config_cancel(confwin->roomjid);
    }

    if ((g_strcmp0(args[0], "submit") == 0) || (g_strcmp0(args[0], "cancel") == 0)) {
        if (confwin->form) {
            cmd_autocomplete_remove_form_fields(confwin->form);
        }
        wins_close_current();
        ProfWin *new_current = (ProfWin*)wins_get_muc(confwin->roomjid);
        if (!new_current) {
            new_current = wins_get_console();
        }
        ui_ev_focus_win(new_current);
    }

    return TRUE;
}

gboolean
cmd_kick(ProfWin *window, const char * const command, gchar **args)
{
    jabber_conn_status_t conn_status = jabber_get_connection_status();

    if (conn_status != JABBER_CONNECTED) {
        cons_show("You are not currently connected.");
        return TRUE;
    }

    if (window->type != WIN_MUC) {
        cons_show("Command '/kick' only applies in chat rooms.");
        return TRUE;
    }

    ProfMucWin *mucwin = (ProfMucWin*)window;
    assert(mucwin->memcheck == PROFMUCWIN_MEMCHECK);

    char *nick = args[0];
    if (nick) {
        if (muc_roster_contains_nick(mucwin->roomjid, nick)) {
            char *reason = args[1];
            iq_room_kick_occupant(mucwin->roomjid, nick, reason);
        } else {
            win_vprint((ProfWin*) mucwin, '!', 0, NULL, 0, 0, "", "Occupant does not exist: %s", nick);
        }
    } else {
        cons_bad_cmd_usage(command);
    }

    return TRUE;
}

gboolean
cmd_ban(ProfWin *window, const char * const command, gchar **args)
{
    jabber_conn_status_t conn_status = jabber_get_connection_status();

    if (conn_status != JABBER_CONNECTED) {
        cons_show("You are not currently connected.");
        return TRUE;
    }

    if (window->type != WIN_MUC) {
        cons_show("Command '/ban' only applies in chat rooms.");
        return TRUE;
    }

    ProfMucWin *mucwin = (ProfMucWin*)window;
    assert(mucwin->memcheck == PROFMUCWIN_MEMCHECK);

    char *jid = args[0];
    if (jid) {
        char *reason = args[1];
        iq_room_affiliation_set(mucwin->roomjid, jid, "outcast", reason);
    } else {
        cons_bad_cmd_usage(command);
    }
    return TRUE;
}

gboolean
cmd_subject(ProfWin *window, const char * const command, gchar **args)
{
    jabber_conn_status_t conn_status = jabber_get_connection_status();

    if (conn_status != JABBER_CONNECTED) {
        cons_show("You are not currently connected.");
        return TRUE;
    }

    if (window->type != WIN_MUC) {
        cons_show("Command '/room' does not apply to this window.");
        return TRUE;
    }

    ProfMucWin *mucwin = (ProfMucWin*)window;
    assert(mucwin->memcheck == PROFMUCWIN_MEMCHECK);

    if (args[0] == NULL) {
        char *subject = muc_subject(mucwin->roomjid);
        if (subject) {
            win_vprint(window, '!', 0, NULL, NO_EOL, THEME_ROOMINFO, "", "Room subject: ");
            win_vprint(window, '!', 0, NULL, NO_DATE, 0, "", "%s", subject);
        } else {
            win_print(window, '!', 0, NULL, 0, THEME_ROOMINFO, "", "Room has no subject");
        }
        return TRUE;
    }

    if (g_strcmp0(args[0], "set") == 0) {
        if (args[1]) {
            message_send_groupchat_subject(mucwin->roomjid, args[1]);
        } else {
            cons_bad_cmd_usage(command);
        }
        return TRUE;
    }

    if (g_strcmp0(args[0], "clear") == 0) {
        message_send_groupchat_subject(mucwin->roomjid, NULL);
        return TRUE;
    }

    cons_bad_cmd_usage(command);
    return TRUE;
}

gboolean
cmd_affiliation(ProfWin *window, const char * const command, gchar **args)
{
    jabber_conn_status_t conn_status = jabber_get_connection_status();

    if (conn_status != JABBER_CONNECTED) {
        cons_show("You are not currently connected.");
        return TRUE;
    }

    if (window->type != WIN_MUC) {
        cons_show("Command '/affiliation' does not apply to this window.");
        return TRUE;
    }

    char *cmd = args[0];
    if (cmd == NULL) {
        cons_bad_cmd_usage(command);
        return TRUE;
    }

    char *affiliation = args[1];
    if (affiliation &&
            (g_strcmp0(affiliation, "owner") != 0) &&
            (g_strcmp0(affiliation, "admin") != 0) &&
            (g_strcmp0(affiliation, "member") != 0) &&
            (g_strcmp0(affiliation, "none") != 0) &&
            (g_strcmp0(affiliation, "outcast") != 0)) {
        cons_bad_cmd_usage(command);
        return TRUE;
    }

    ProfMucWin *mucwin = (ProfMucWin*)window;
    assert(mucwin->memcheck == PROFMUCWIN_MEMCHECK);

    if (g_strcmp0(cmd, "list") == 0) {
        if (!affiliation) {
            iq_room_affiliation_list(mucwin->roomjid, "owner");
            iq_room_affiliation_list(mucwin->roomjid, "admin");
            iq_room_affiliation_list(mucwin->roomjid, "member");
            iq_room_affiliation_list(mucwin->roomjid, "outcast");
        } else if (g_strcmp0(affiliation, "none") == 0) {
            win_print((ProfWin*) mucwin, '!', 0, NULL, 0, 0, "", "Cannot list users with no affiliation.");
        } else {
            iq_room_affiliation_list(mucwin->roomjid, affiliation);
        }
        return TRUE;
    }

    if (g_strcmp0(cmd, "set") == 0) {
        if (!affiliation) {
            cons_bad_cmd_usage(command);
            return TRUE;
        }

        char *jid = args[2];
        if (jid == NULL) {
            cons_bad_cmd_usage(command);
            return TRUE;
        } else {
            char *reason = args[3];
            iq_room_affiliation_set(mucwin->roomjid, jid, affiliation, reason);
            return TRUE;
        }
    }

    cons_bad_cmd_usage(command);
    return TRUE;
}

gboolean
cmd_role(ProfWin *window, const char * const command, gchar **args)
{
    jabber_conn_status_t conn_status = jabber_get_connection_status();

    if (conn_status != JABBER_CONNECTED) {
        cons_show("You are not currently connected.");
        return TRUE;
    }

    if (window->type != WIN_MUC) {
        cons_show("Command '/role' does not apply to this window.");
        return TRUE;
    }

    char *cmd = args[0];
    if (cmd == NULL) {
        cons_bad_cmd_usage(command);
        return TRUE;
    }

    char *role = args[1];
    if (role &&
            (g_strcmp0(role, "visitor") != 0) &&
            (g_strcmp0(role, "participant") != 0) &&
            (g_strcmp0(role, "moderator") != 0) &&
            (g_strcmp0(role, "none") != 0)) {
        cons_bad_cmd_usage(command);
        return TRUE;
    }

    ProfMucWin *mucwin = (ProfMucWin*)window;
    assert(mucwin->memcheck == PROFMUCWIN_MEMCHECK);

    if (g_strcmp0(cmd, "list") == 0) {
        if (!role) {
            iq_room_role_list(mucwin->roomjid, "moderator");
            iq_room_role_list(mucwin->roomjid, "participant");
            iq_room_role_list(mucwin->roomjid, "visitor");
        } else if (g_strcmp0(role, "none") == 0) {
            win_print((ProfWin*) mucwin, '!', 0, NULL, 0, 0, "", "Cannot list users with no role.");
        } else {
            iq_room_role_list(mucwin->roomjid, role);
        }
        return TRUE;
    }

    if (g_strcmp0(cmd, "set") == 0) {
        if (!role) {
            cons_bad_cmd_usage(command);
            return TRUE;
        }

        char *nick = args[2];
        if (nick == NULL) {
            cons_bad_cmd_usage(command);
            return TRUE;
        } else {
            char *reason = args[3];
            iq_room_role_set(mucwin->roomjid, nick, role, reason);
            return TRUE;
        }
    }

    cons_bad_cmd_usage(command);
    return TRUE;
}

gboolean
cmd_room(ProfWin *window, const char * const command, gchar **args)
{
    jabber_conn_status_t conn_status = jabber_get_connection_status();

    if (conn_status != JABBER_CONNECTED) {
        cons_show("You are not currently connected.");
        return TRUE;
    }

    if (window->type != WIN_MUC) {
        cons_show("Command '/room' does not apply to this window.");
        return TRUE;
    }

    if ((g_strcmp0(args[0], "accept") != 0) &&
            (g_strcmp0(args[0], "destroy") != 0) &&
            (g_strcmp0(args[0], "config") != 0)) {
        cons_bad_cmd_usage(command);
        return TRUE;
    }

    ProfMucWin *mucwin = (ProfMucWin*)window;
    assert(mucwin->memcheck == PROFMUCWIN_MEMCHECK);
    int num = wins_get_num(window);

    int ui_index = num;
    if (ui_index == 10) {
        ui_index = 0;
    }

    if (g_strcmp0(args[0], "accept") == 0) {
        gboolean requires_config = muc_requires_config(mucwin->roomjid);
        if (!requires_config) {
            win_print(window, '!', 0, NULL, 0, THEME_ROOMINFO, "", "Current room does not require configuration.");
            return TRUE;
        } else {
            iq_confirm_instant_room(mucwin->roomjid);
            muc_set_requires_config(mucwin->roomjid, FALSE);
            win_print(window, '!', 0, NULL, 0, THEME_ROOMINFO, "", "Room unlocked.");
            return TRUE;
        }
    }

    if (g_strcmp0(args[0], "destroy") == 0) {
        iq_destroy_room(mucwin->roomjid);
        return TRUE;
    }

    if (g_strcmp0(args[0], "config") == 0) {
        ProfMucConfWin *confwin = wins_get_muc_conf(mucwin->roomjid);

        if (confwin) {
            ui_ev_focus_win((ProfWin*)confwin);
        } else {
            iq_request_room_config_form(mucwin->roomjid);
        }
        return TRUE;
    }

    return TRUE;
}

gboolean
cmd_occupants(ProfWin *window, const char * const command, gchar **args)
{
    jabber_conn_status_t conn_status = jabber_get_connection_status();

    if (conn_status != JABBER_CONNECTED) {
        cons_show("You are not currently connected.");
        return TRUE;
    }

    if (g_strcmp0(args[0], "size") == 0) {
        if (!args[1]) {
            cons_bad_cmd_usage(command);
            return TRUE;
        } else {
            int intval = 0;
            char *err_msg = NULL;
            gboolean res = strtoi_range(args[1], &intval, 1, 99, &err_msg);
            if (res) {
                prefs_set_occupants_size(intval);
                cons_show("Occupants screen size set to: %d%%", intval);
                wins_resize_all();
                return TRUE;
            } else {
                cons_show(err_msg);
                free(err_msg);
                return TRUE;
            }
        }
    }

    if (g_strcmp0(args[0], "default") == 0) {
        if (g_strcmp0(args[1], "show") == 0) {
            if (g_strcmp0(args[2], "jid") == 0) {
                cons_show("Occupant jids enabled.");
                prefs_set_boolean(PREF_OCCUPANTS_JID, TRUE);
            } else {
                cons_show("Occupant list enabled.");
                prefs_set_boolean(PREF_OCCUPANTS, TRUE);
            }
            return TRUE;
        } else if (g_strcmp0(args[1], "hide") == 0) {
            if (g_strcmp0(args[2], "jid") == 0) {
                cons_show("Occupant jids disabled.");
                prefs_set_boolean(PREF_OCCUPANTS_JID, FALSE);
            } else {
                cons_show("Occupant list disabled.");
                prefs_set_boolean(PREF_OCCUPANTS, FALSE);
            }
            return TRUE;
        } else {
            cons_bad_cmd_usage(command);
            return TRUE;
        }
    }

    if (window->type != WIN_MUC) {
        cons_show("Cannot apply setting when not in chat room.");
        return TRUE;
    }

    ProfMucWin *mucwin = (ProfMucWin*)window;
    assert(mucwin->memcheck == PROFMUCWIN_MEMCHECK);

    if (g_strcmp0(args[0], "show") == 0) {
        if (g_strcmp0(args[1], "jid") == 0) {
            mucwin->showjid = TRUE;
            ui_room_update_occupants(mucwin->roomjid);
        } else {
            ui_room_show_occupants(mucwin->roomjid);
        }
    } else if (g_strcmp0(args[0], "hide") == 0) {
        if (g_strcmp0(args[1], "jid") == 0) {
            mucwin->showjid = FALSE;
            ui_room_update_occupants(mucwin->roomjid);
        } else {
            ui_room_hide_occupants(mucwin->roomjid);
        }
    } else {
        cons_bad_cmd_usage(command);
    }

    return TRUE;
}

gboolean
cmd_rooms(ProfWin *window, const char * const command, gchar **args)
{
    jabber_conn_status_t conn_status = jabber_get_connection_status();

    if (conn_status != JABBER_CONNECTED) {
        cons_show("You are not currently connected.");
        return TRUE;
    }

    if (args[0] == NULL) {
        ProfAccount *account = accounts_get_account(jabber_get_account_name());
        iq_room_list_request(account->muc_service);
        account_free(account);
    } else {
        iq_room_list_request(args[0]);
    }

    return TRUE;
}

gboolean
cmd_bookmark(ProfWin *window, const char * const command, gchar **args)
{
    jabber_conn_status_t conn_status = jabber_get_connection_status();

    if (conn_status != JABBER_CONNECTED) {
        cons_show("You are not currently connected.");
        return TRUE;
    }

    gchar *cmd = args[0];
    if (window->type == WIN_MUC && cmd == NULL) {
        // default to current nickname, password, and autojoin "on"
        ProfMucWin *mucwin = (ProfMucWin*)window;
        assert(mucwin->memcheck == PROFMUCWIN_MEMCHECK);
        char *nick = muc_nick(mucwin->roomjid);
        char *password = muc_password(mucwin->roomjid);
        gboolean added = bookmark_add(mucwin->roomjid, nick, password, "on");
        if (added) {
            ui_current_print_formatted_line('!', 0, "Bookmark added for %s.", mucwin->roomjid);
        } else {
            ui_current_print_formatted_line('!', 0, "Bookmark already exists for %s.", mucwin->roomjid);
        }
        return TRUE;

    } else {
        if (cmd == NULL) {
            cons_bad_cmd_usage(command);
            return TRUE;
        }

        if (strcmp(cmd, "list") == 0) {
            const GList *bookmarks = bookmark_get_list();
            cons_show_bookmarks(bookmarks);
        } else {
            char *jid = args[1];
            if (jid == NULL) {
                cons_bad_cmd_usage(command);
                cons_show("");
                return TRUE;
            }

            if (strcmp(cmd, "remove") == 0) {
                gboolean removed = bookmark_remove(jid);
                if (removed) {
                    cons_show("Bookmark removed for %s.", jid);
                } else {
                    cons_show("No bookmark exists for %s.", jid);
                }
                return TRUE;
            }

            if (strcmp(cmd, "join") == 0) {
                gboolean joined = bookmark_join(jid);
                if (!joined) {
                    cons_show("No bookmark exists for %s.", jid);
                }
                return TRUE;
            }

            gchar *opt_keys[] = { "autojoin", "nick", "password", NULL };
            gboolean parsed;

            GHashTable *options = parse_options(&args[2], opt_keys, &parsed);
            if (!parsed) {
                cons_bad_cmd_usage(command);
                cons_show("");
                return TRUE;
            }

            char *nick = g_hash_table_lookup(options, "nick");
            char *password = g_hash_table_lookup(options, "password");
            char *autojoin = g_hash_table_lookup(options, "autojoin");

            if (autojoin) {
                if ((strcmp(autojoin, "on") != 0) && (strcmp(autojoin, "off") != 0)) {
                    cons_bad_cmd_usage(command);
                    cons_show("");
                    return TRUE;
                }
            }

            if (strcmp(cmd, "add") == 0) {
                if (strchr(jid, '@')==NULL) {
                    cons_show("Can't add bookmark with JID '%s'; should be '%s@domain.tld'", jid, jid);
                } else {
                    gboolean added = bookmark_add(jid, nick, password, autojoin);
                    if (added) {
                        cons_show("Bookmark added for %s.", jid);
                    } else {
                        cons_show("Bookmark already exists, use /bookmark update to edit.");
                    }
                }
            } else if (strcmp(cmd, "update") == 0) {
                gboolean updated = bookmark_update(jid, nick, password, autojoin);
                if (updated) {
                    cons_show("Bookmark updated.");
                } else {
                    cons_show("No bookmark exists for %s.", jid);
                }
            } else {
                cons_bad_cmd_usage(command);
            }

            options_destroy(options);
        }
    }

    return TRUE;
}

gboolean
cmd_disco(ProfWin *window, const char * const command, gchar **args)
{
    jabber_conn_status_t conn_status = jabber_get_connection_status();

    if (conn_status != JABBER_CONNECTED) {
        cons_show("You are not currenlty connected.");
        return TRUE;
    }

    GString *jid = g_string_new("");
    if (args[1]) {
        jid = g_string_append(jid, args[1]);
    } else {
        Jid *jidp = jid_create(jabber_get_fulljid());
        jid = g_string_append(jid, jidp->domainpart);
        jid_destroy(jidp);
    }

    if (g_strcmp0(args[0], "info") == 0) {
        iq_disco_info_request(jid->str);
    } else {
        iq_disco_items_request(jid->str);
    }

    g_string_free(jid, TRUE);

    return TRUE;
}

gboolean
cmd_nick(ProfWin *window, const char * const command, gchar **args)
{
    jabber_conn_status_t conn_status = jabber_get_connection_status();

    if (conn_status != JABBER_CONNECTED) {
        cons_show("You are not currently connected.");
        return TRUE;
    }
    if (window->type != WIN_MUC) {
        cons_show("You can only change your nickname in a chat room window.");
        return TRUE;
    }

    ProfMucWin *mucwin = (ProfMucWin*)window;
    assert(mucwin->memcheck == PROFMUCWIN_MEMCHECK);
    char *nick = args[0];
    presence_change_room_nick(mucwin->roomjid, nick);

    return TRUE;
}

gboolean
cmd_alias(ProfWin *window, const char * const command, gchar **args)
{
    char *subcmd = args[0];

    if (strcmp(subcmd, "add") == 0) {
        char *alias = args[1];
        if (alias == NULL) {
            cons_bad_cmd_usage(command);
            return TRUE;
        } else {
            char *alias_p = alias;
            GString *ac_value = g_string_new("");
            if (alias[0] == '/') {
                g_string_append(ac_value, alias);
                alias_p = &alias[1];
            } else {
                g_string_append(ac_value, "/");
                g_string_append(ac_value, alias);
            }

            char *value = args[2];
            if (value == NULL) {
                cons_bad_cmd_usage(command);
                g_string_free(ac_value, TRUE);
                return TRUE;
            } else if (cmd_exists(ac_value->str)) {
                cons_show("Command or alias '%s' already exists.", ac_value->str);
                g_string_free(ac_value, TRUE);
                return TRUE;
            } else {
                prefs_add_alias(alias_p, value);
                cmd_autocomplete_add(ac_value->str);
                cmd_alias_add(alias_p);
                cons_show("Command alias added %s -> %s", ac_value->str, value);
                g_string_free(ac_value, TRUE);
                return TRUE;
            }
        }
    } else if (strcmp(subcmd, "remove") == 0) {
        char *alias = args[1];
        if (alias == NULL) {
            cons_bad_cmd_usage(command);
            return TRUE;
        } else {
            if (alias[0] == '/') {
                alias = &alias[1];
            }
            gboolean removed = prefs_remove_alias(alias);
            if (!removed) {
                cons_show("No such command alias /%s", alias);
            } else {
                GString *ac_value = g_string_new("/");
                g_string_append(ac_value, alias);
                cmd_autocomplete_remove(ac_value->str);
                cmd_alias_remove(alias);
                g_string_free(ac_value, TRUE);
                cons_show("Command alias removed -> /%s", alias);
            }
            return TRUE;
        }
    } else if (strcmp(subcmd, "list") == 0) {
        GList *aliases = prefs_get_aliases();
        cons_show_aliases(aliases);
        prefs_free_aliases(aliases);
        return TRUE;
    } else {
        cons_bad_cmd_usage(command);
        return TRUE;
    }
}

gboolean
cmd_tiny(ProfWin *window, const char * const command, gchar **args)
{
    char *url = args[0];

    if (window->type != WIN_CHAT && window->type != WIN_MUC && window->type != WIN_PRIVATE) {
        cons_show("/tiny can only be used in chat windows");
        return TRUE;
    }

    if (!tinyurl_valid(url)) {
        win_vprint(window, '-', 0, NULL, 0, THEME_ERROR, "", "/tiny, badly formed URL: %s", url);
        return TRUE;
    }

    char *tiny = tinyurl_get(url);
    if (!tiny) {
        win_print(window, '-', 0, NULL, 0, THEME_ERROR, "", "Couldn't create tinyurl.");
        return TRUE;
    }

    switch (window->type){
    case WIN_CHAT:
    {
        ProfChatWin *chatwin = (ProfChatWin*)window;
        assert(chatwin->memcheck == PROFCHATWIN_MEMCHECK);
        cl_ev_send_msg(chatwin, tiny);
        break;
    }
    case WIN_PRIVATE:
    {
        ProfPrivateWin *privatewin = (ProfPrivateWin*)window;
        assert(privatewin->memcheck == PROFPRIVATEWIN_MEMCHECK);
        cl_ev_send_priv_msg(privatewin, tiny);
        break;
    }
    case WIN_MUC:
    {
        ProfMucWin *mucwin = (ProfMucWin*)window;
        assert(mucwin->memcheck == PROFMUCWIN_MEMCHECK);
        cl_ev_send_muc_msg(mucwin, tiny);
        break;
    }
    default:
        break;
    }

    free(tiny);

    return TRUE;
}

gboolean
cmd_clear(ProfWin *window, const char * const command, gchar **args)
{
    ui_clear_win(window);
    return TRUE;
}

gboolean
cmd_close(ProfWin *window, const char * const command, gchar **args)
{
    jabber_conn_status_t conn_status = jabber_get_connection_status();
    int index = 0;
    int count = 0;

    if (args[0] == NULL) {
        index = wins_get_current_num();
    } else if (strcmp(args[0], "all") == 0) {
        count = ui_close_all_wins();
        if (count == 0) {
            cons_show("No windows to close.");
        } else if (count == 1) {
            cons_show("Closed 1 window.");
        } else {
            cons_show("Closed %d windows.", count);
        }
        return TRUE;
    } else if (strcmp(args[0], "read") == 0) {
        count = ui_close_read_wins();
        if (count == 0) {
            cons_show("No windows to close.");
        } else if (count == 1) {
            cons_show("Closed 1 window.");
        } else {
            cons_show("Closed %d windows.", count);
        }
        return TRUE;
    } else {
        index = atoi(args[0]);
    }

    if (index < 0 || index == 10) {
        cons_show("No such window exists.");
        return TRUE;
    }

    if (index == 1) {
        cons_show("Cannot close console window.");
        return TRUE;
    }

    ProfWin *toclose = wins_get_by_num(index);
    if (!toclose) {
        cons_show("Window is not open.");
        return TRUE;
    }

    // check for unsaved form
    if (ui_win_has_unsaved_form(index)) {
        ui_current_print_line("You have unsaved changes, use /form submit or /form cancel");
        return TRUE;
    }

    // handle leaving rooms, or chat
    if (conn_status == JABBER_CONNECTED) {
        ui_close_connected_win(index);
    }

    // close the window
    ui_close_win(index);
    cons_show("Closed window %d", index);

    // Tidy up the window list.
    if (prefs_get_boolean(PREF_WINS_AUTO_TIDY)) {
        ui_tidy_wins();
    }

    return TRUE;
}

gboolean
cmd_leave(ProfWin *window, const char * const command, gchar **args)
{
    jabber_conn_status_t conn_status = jabber_get_connection_status();
    int index = wins_get_current_num();

    if (window->type != WIN_MUC) {
        cons_show("You can only use the /leave command in a chat room.");
        cons_alert();
        return TRUE;
    }

    // handle leaving rooms, or chat
    if (conn_status == JABBER_CONNECTED) {
        ui_close_connected_win(index);
    }

    // close the window
    ui_close_win(index);

    return TRUE;
}

gboolean
cmd_privileges(ProfWin *window, const char * const command, gchar **args)
{
    gboolean result = _cmd_set_boolean_preference(args[0], command, "MUC privileges", PREF_MUC_PRIVILEGES);

    ui_redraw_all_room_rosters();

    return result;
}

gboolean
cmd_beep(ProfWin *window, const char * const command, gchar **args)
{
    return _cmd_set_boolean_preference(args[0], command, "Sound", PREF_BEEP);
}

gboolean
cmd_presence(ProfWin *window, const char * const command, gchar **args)
{
    return _cmd_set_boolean_preference(args[0], command, "Contact presence", PREF_PRESENCE);
}

gboolean
cmd_wrap(ProfWin *window, const char * const command, gchar **args)
{
    gboolean result = _cmd_set_boolean_preference(args[0], command, "Word wrap", PREF_WRAP);

    wins_resize_all();

    return result;
}

gboolean
cmd_time(ProfWin *window, const char * const command, gchar **args)
{
    if (g_strcmp0(args[0], "statusbar") == 0) {
        if (args[1] == NULL) {
            cons_show("Current status bar time format is '%s'.", prefs_get_string(PREF_TIME_STATUSBAR));
            return TRUE;
        } else if (g_strcmp0(args[1], "set") == 0 && args[2] != NULL) {
            prefs_set_string(PREF_TIME_STATUSBAR, args[2]);
            cons_show("Status bar time format set to '%s'.", args[2]);
            ui_redraw();
            return TRUE;
        } else if (g_strcmp0(args[1], "off") == 0) {
            prefs_set_string(PREF_TIME_STATUSBAR, "");
            cons_show("Status bar time display disabled.");
            ui_redraw();
            return TRUE;
        } else {
            cons_bad_cmd_usage(command);
            return TRUE;
        }
    } else if (g_strcmp0(args[0], "main") == 0) {
        if (args[1] == NULL) {
            cons_show("Current time format is '%s'.", prefs_get_string(PREF_TIME));
            return TRUE;
        } else if (g_strcmp0(args[1], "set") == 0 && args[2] != NULL) {
            prefs_set_string(PREF_TIME, args[2]);
            cons_show("Time format set to '%s'.", args[2]);
            wins_resize_all();
            return TRUE;
        } else if (g_strcmp0(args[1], "off") == 0) {
            prefs_set_string(PREF_TIME, "");
            cons_show("Time display disabled.");
            wins_resize_all();
            return TRUE;
        } else {
            cons_bad_cmd_usage(command);
            return TRUE;
        }
    } else {
        cons_bad_cmd_usage(command);
        return TRUE;
    }
}

gboolean
cmd_states(ProfWin *window, const char * const command, gchar **args)
{
    gboolean result = _cmd_set_boolean_preference(args[0], command, "Sending chat states",
        PREF_STATES);

    // if disabled, disable outtype and gone
    if (result == TRUE && (strcmp(args[0], "off") == 0)) {
        prefs_set_boolean(PREF_OUTTYPE, FALSE);
        prefs_set_gone(0);
    }

    return result;
}

gboolean
cmd_titlebar(ProfWin *window, const char * const command, gchar **args)
{
    if (g_strcmp0(args[0], "show") != 0 && g_strcmp0(args[0], "goodbye") != 0) {
        cons_bad_cmd_usage(command);
        return TRUE;
    }
    if (g_strcmp0(args[0], "show") == 0 && g_strcmp0(args[1], "off") == 0) {
        ui_clear_win_title();
    }
    if (g_strcmp0(args[0], "show") == 0) {
        return _cmd_set_boolean_preference(args[1], command, "Titlebar show", PREF_TITLEBAR_SHOW);
    } else {
        return _cmd_set_boolean_preference(args[1], command, "Titlebar goodbye", PREF_TITLEBAR_GOODBYE);
    }
}

gboolean
cmd_outtype(ProfWin *window, const char * const command, gchar **args)
{
    gboolean result = _cmd_set_boolean_preference(args[0], command,
        "Sending typing notifications", PREF_OUTTYPE);

    // if enabled, enable states
    if (result == TRUE && (strcmp(args[0], "on") == 0)) {
        prefs_set_boolean(PREF_STATES, TRUE);
    }

    return result;
}

gboolean
cmd_gone(ProfWin *window, const char * const command, gchar **args)
{
    char *value = args[0];

    gint period = atoi(value);
    prefs_set_gone(period);
    if (period == 0) {
        cons_show("Automatic leaving conversations after period disabled.");
    } else if (period == 1) {
        cons_show("Leaving conversations after 1 minute of inactivity.");
    } else {
        cons_show("Leaving conversations after %d minutes of inactivity.", period);
    }

    // if enabled, enable states
    if (period > 0) {
        prefs_set_boolean(PREF_STATES, TRUE);
    }

    return TRUE;
}


gboolean
cmd_notify(ProfWin *window, const char * const command, gchar **args)
{
    char *kind = args[0];

    // bad kind
    if ((strcmp(kind, "message") != 0) && (strcmp(kind, "typing") != 0) &&
            (strcmp(kind, "remind") != 0) && (strcmp(kind, "invite") != 0) &&
            (strcmp(kind, "sub") != 0) && (strcmp(kind, "room") != 0)) {
        cons_bad_cmd_usage(command);

    // set message setting
    } else if (strcmp(kind, "message") == 0) {
        if (strcmp(args[1], "on") == 0) {
            cons_show("Message notifications enabled.");
            prefs_set_boolean(PREF_NOTIFY_MESSAGE, TRUE);
        } else if (strcmp(args[1], "off") == 0) {
            cons_show("Message notifications disabled.");
            prefs_set_boolean(PREF_NOTIFY_MESSAGE, FALSE);
        } else if (strcmp(args[1], "current") == 0) {
            if (g_strcmp0(args[2], "on") == 0) {
                cons_show("Current window message notifications enabled.");
                prefs_set_boolean(PREF_NOTIFY_MESSAGE_CURRENT, TRUE);
            } else if (g_strcmp0(args[2], "off") == 0) {
                cons_show("Current window message notifications disabled.");
                prefs_set_boolean(PREF_NOTIFY_MESSAGE_CURRENT, FALSE);
            } else {
                cons_show("Usage: /notify message current on|off");
            }
        } else if (strcmp(args[1], "text") == 0) {
            if (g_strcmp0(args[2], "on") == 0) {
                cons_show("Showing text in message notifications enabled.");
                prefs_set_boolean(PREF_NOTIFY_MESSAGE_TEXT, TRUE);
            } else if (g_strcmp0(args[2], "off") == 0) {
                cons_show("Showing text in message notifications disabled.");
                prefs_set_boolean(PREF_NOTIFY_MESSAGE_TEXT, FALSE);
            } else {
                cons_show("Usage: /notify message text on|off");
            }
        } else {
            cons_show("Usage: /notify message on|off");
        }

    // set room setting
    } else if (strcmp(kind, "room") == 0) {
        if (strcmp(args[1], "on") == 0) {
            cons_show("Chat room notifications enabled.");
            prefs_set_string(PREF_NOTIFY_ROOM, "on");
        } else if (strcmp(args[1], "off") == 0) {
            cons_show("Chat room notifications disabled.");
            prefs_set_string(PREF_NOTIFY_ROOM, "off");
        } else if (strcmp(args[1], "mention") == 0) {
            cons_show("Chat room notifications enabled on mention.");
            prefs_set_string(PREF_NOTIFY_ROOM, "mention");
        } else if (strcmp(args[1], "current") == 0) {
            if (g_strcmp0(args[2], "on") == 0) {
                cons_show("Current window chat room message notifications enabled.");
                prefs_set_boolean(PREF_NOTIFY_ROOM_CURRENT, TRUE);
            } else if (g_strcmp0(args[2], "off") == 0) {
                cons_show("Current window chat room message notifications disabled.");
                prefs_set_boolean(PREF_NOTIFY_ROOM_CURRENT, FALSE);
            } else {
                cons_show("Usage: /notify room current on|off");
            }
        } else if (strcmp(args[1], "text") == 0) {
            if (g_strcmp0(args[2], "on") == 0) {
                cons_show("Showing text in chat room message notifications enabled.");
                prefs_set_boolean(PREF_NOTIFY_ROOM_TEXT, TRUE);
            } else if (g_strcmp0(args[2], "off") == 0) {
                cons_show("Showing text in chat room message notifications disabled.");
                prefs_set_boolean(PREF_NOTIFY_ROOM_TEXT, FALSE);
            } else {
                cons_show("Usage: /notify room text on|off");
            }
        } else {
            cons_show("Usage: /notify room on|off|mention");
        }

    // set typing setting
    } else if (strcmp(kind, "typing") == 0) {
        if (strcmp(args[1], "on") == 0) {
            cons_show("Typing notifications enabled.");
            prefs_set_boolean(PREF_NOTIFY_TYPING, TRUE);
        } else if (strcmp(args[1], "off") == 0) {
            cons_show("Typing notifications disabled.");
            prefs_set_boolean(PREF_NOTIFY_TYPING, FALSE);
        } else if (strcmp(args[1], "current") == 0) {
            if (g_strcmp0(args[2], "on") == 0) {
                cons_show("Current window typing notifications enabled.");
                prefs_set_boolean(PREF_NOTIFY_TYPING_CURRENT, TRUE);
            } else if (g_strcmp0(args[2], "off") == 0) {
                cons_show("Current window typing notifications disabled.");
                prefs_set_boolean(PREF_NOTIFY_TYPING_CURRENT, FALSE);
            } else {
                cons_show("Usage: /notify typing current on|off");
            }
        } else {
            cons_show("Usage: /notify typing on|off");
        }

    // set invite setting
    } else if (strcmp(kind, "invite") == 0) {
        if (strcmp(args[1], "on") == 0) {
            cons_show("Chat room invite notifications enabled.");
            prefs_set_boolean(PREF_NOTIFY_INVITE, TRUE);
        } else if (strcmp(args[1], "off") == 0) {
            cons_show("Chat room invite notifications disabled.");
            prefs_set_boolean(PREF_NOTIFY_INVITE, FALSE);
        } else {
            cons_show("Usage: /notify invite on|off");
        }

    // set subscription setting
    } else if (strcmp(kind, "sub") == 0) {
        if (strcmp(args[1], "on") == 0) {
            cons_show("Subscription notifications enabled.");
            prefs_set_boolean(PREF_NOTIFY_SUB, TRUE);
        } else if (strcmp(args[1], "off") == 0) {
            cons_show("Subscription notifications disabled.");
            prefs_set_boolean(PREF_NOTIFY_SUB, FALSE);
        } else {
            cons_show("Usage: /notify sub on|off");
        }

    // set remind setting
    } else if (strcmp(kind, "remind") == 0) {
        gint period = atoi(args[1]);
        prefs_set_notify_remind(period);
        if (period == 0) {
            cons_show("Message reminders disabled.");
        } else if (period == 1) {
            cons_show("Message reminder period set to 1 second.");
        } else {
            cons_show("Message reminder period set to %d seconds.", period);
        }

    } else {
        cons_show("Unknown command: %s.", kind);
    }

    return TRUE;
}

gboolean
cmd_inpblock(ProfWin *window, const char * const command, gchar **args)
{
    char *subcmd = args[0];
    char *value = args[1];

    if (g_strcmp0(subcmd, "timeout") == 0) {
        if (value == NULL) {
            cons_bad_cmd_usage(command);
            return TRUE;
        }

        int intval = 0;
        char *err_msg = NULL;
        gboolean res = strtoi_range(value, &intval, 1, 1000, &err_msg);
        if (res) {
            cons_show("Input blocking set to %d milliseconds.", intval);
            prefs_set_inpblock(intval);
            ui_input_nonblocking(FALSE);
        } else {
            cons_show(err_msg);
            free(err_msg);
        }

        return TRUE;
    }

    if (g_strcmp0(subcmd, "dynamic") == 0) {
        if (value == NULL) {
            cons_bad_cmd_usage(command);
            return TRUE;
        }

        if (g_strcmp0(value, "on") != 0 && g_strcmp0(value, "off") != 0) {
            cons_show("Dynamic must be one of 'on' or 'off'");
            return TRUE;
        }

        return _cmd_set_boolean_preference(value, command, "Dynamic input blocking", PREF_INPBLOCK_DYNAMIC);
    }

    cons_bad_cmd_usage(command);

    return TRUE;
}

gboolean
cmd_log(ProfWin *window, const char * const command, gchar **args)
{
    char *subcmd = args[0];
    char *value = args[1];

    if (strcmp(subcmd, "maxsize") == 0) {
        if (value == NULL) {
            cons_bad_cmd_usage(command);
            return TRUE;
        }

        int intval = 0;
        char *err_msg = NULL;
        gboolean res = strtoi_range(value, &intval, PREFS_MIN_LOG_SIZE, INT_MAX, &err_msg);
        if (res) {
            prefs_set_max_log_size(intval);
            cons_show("Log maxinum size set to %d bytes", intval);
        } else {
            cons_show(err_msg);
            free(err_msg);
        }
        return TRUE;
    }

    if (strcmp(subcmd, "rotate") == 0) {
        if (value == NULL) {
            cons_bad_cmd_usage(command);
            return TRUE;
        }
        return _cmd_set_boolean_preference(value, command, "Log rotate", PREF_LOG_ROTATE);
    }

    if (strcmp(subcmd, "shared") == 0) {
        if (value == NULL) {
            cons_bad_cmd_usage(command);
            return TRUE;
        }
        gboolean result = _cmd_set_boolean_preference(value, command, "Shared log", PREF_LOG_SHARED);
        log_reinit();
        return result;
    }

    if (strcmp(subcmd, "where") == 0) {
        char *logfile = get_log_file_location();
        cons_show("Log file: %s", logfile);
        return TRUE;
    }

    cons_bad_cmd_usage(command);

    /* TODO: make 'level' subcommand for debug level */

    return TRUE;
}

gboolean
cmd_reconnect(ProfWin *window, const char * const command, gchar **args)
{
    char *value = args[0];

    int intval = 0;
    char *err_msg = NULL;
    gboolean res = strtoi_range(value, &intval, 0, INT_MAX, &err_msg);
    if (res) {
        prefs_set_reconnect(intval);
        if (intval == 0) {
            cons_show("Reconnect disabled.", intval);
        } else {
            cons_show("Reconnect interval set to %d seconds.", intval);
        }
    } else {
        cons_show(err_msg);
        cons_bad_cmd_usage(command);
        free(err_msg);
    }

    return TRUE;
}

gboolean
cmd_autoping(ProfWin *window, const char * const command, gchar **args)
{
    char *value = args[0];

    int intval = 0;
    char *err_msg = NULL;
    gboolean res = strtoi_range(value, &intval, 0, INT_MAX, &err_msg);
    if (res) {
        prefs_set_autoping(intval);
        iq_set_autoping(intval);
        if (intval == 0) {
            cons_show("Autoping disabled.", intval);
        } else {
            cons_show("Autoping interval set to %d seconds.", intval);
        }
    } else {
        cons_show(err_msg);
        cons_bad_cmd_usage(command);
        free(err_msg);
    }

    return TRUE;
}

gboolean
cmd_ping(ProfWin *window, const char * const command, gchar **args)
{
    jabber_conn_status_t conn_status = jabber_get_connection_status();

    if (conn_status != JABBER_CONNECTED) {
        cons_show("You are not currenlty connected.");
        return TRUE;
    }

    iq_send_ping(args[0]);

    if (args[0] == NULL) {
        cons_show("Pinged server...");
    } else {
        cons_show("Pinged %s...", args[0]);
    }
    return TRUE;
}

gboolean
cmd_autoaway(ProfWin *window, const char * const command, gchar **args)
{
    char *setting = args[0];
    char *value = args[1];

    if ((strcmp(setting, "mode") != 0) && (strcmp(setting, "time") != 0) &&
            (strcmp(setting, "message") != 0) && (strcmp(setting, "check") != 0)) {
        cons_show("Setting must be one of 'mode', 'time', 'message' or 'check'");
        return TRUE;
    }

    if (strcmp(setting, "mode") == 0) {
        if ((strcmp(value, "idle") != 0) && (strcmp(value, "away") != 0) &&
                (strcmp(value, "off") != 0)) {
            cons_show("Mode must be one of 'idle', 'away' or 'off'");
        } else {
            prefs_set_string(PREF_AUTOAWAY_MODE, value);
            cons_show("Auto away mode set to: %s.", value);
        }

        return TRUE;
    }

    if (strcmp(setting, "time") == 0) {
        int minutesval = 0;
        char *err_msg = NULL;
        gboolean res = strtoi_range(value, &minutesval, 1, INT_MAX, &err_msg);
        if (res) {
            prefs_set_autoaway_time(minutesval);
            cons_show("Auto away time set to: %d minutes.", minutesval);
        } else {
            cons_show(err_msg);
            free(err_msg);
        }

        return TRUE;
    }

    if (strcmp(setting, "message") == 0) {
        if (strcmp(value, "off") == 0) {
            prefs_set_string(PREF_AUTOAWAY_MESSAGE, NULL);
            cons_show("Auto away message cleared.");
        } else {
            prefs_set_string(PREF_AUTOAWAY_MESSAGE, value);
            cons_show("Auto away message set to: \"%s\".", value);
        }

        return TRUE;
    }

    if (strcmp(setting, "check") == 0) {
        return _cmd_set_boolean_preference(value, command, "Online check", PREF_AUTOAWAY_CHECK);
    }

    return TRUE;
}

gboolean
cmd_priority(ProfWin *window, const char * const command, gchar **args)
{
    jabber_conn_status_t conn_status = jabber_get_connection_status();

    if (conn_status != JABBER_CONNECTED) {
        cons_show("You are not currently connected.");
        return TRUE;
    }

    char *value = args[0];

    int intval = 0;
    char *err_msg = NULL;
    gboolean res = strtoi_range(value, &intval, -128, 127, &err_msg);
    if (res) {
        accounts_set_priority_all(jabber_get_account_name(), intval);
        resource_presence_t last_presence = accounts_get_last_presence(jabber_get_account_name());
        cl_ev_presence_send(last_presence, jabber_get_presence_message(), 0);
        cons_show("Priority set to %d.", intval);
    } else {
        cons_show(err_msg);
        free(err_msg);
    }

    return TRUE;
}

gboolean
cmd_statuses(ProfWin *window, const char * const command, gchar **args)
{
    if (strcmp(args[0], "console") != 0 &&
            strcmp(args[0], "chat") != 0 &&
            strcmp(args[0], "muc") != 0) {
        cons_bad_cmd_usage(command);
        return TRUE;
    }

    if (strcmp(args[1], "all") != 0 &&
            strcmp(args[1], "online") != 0 &&
            strcmp(args[1], "none") != 0) {
        cons_bad_cmd_usage(command);
        return TRUE;
    }

    if (strcmp(args[0], "console") == 0) {
        prefs_set_string(PREF_STATUSES_CONSOLE, args[1]);
        if (strcmp(args[1], "all") == 0) {
            cons_show("All presence updates will appear in the console.");
        } else if (strcmp(args[1], "online") == 0) {
            cons_show("Only online/offline presence updates will appear in the console.");
        } else {
            cons_show("Presence updates will not appear in the console.");
        }
    }

    if (strcmp(args[0], "chat") == 0) {
        prefs_set_string(PREF_STATUSES_CHAT, args[1]);
        if (strcmp(args[1], "all") == 0) {
            cons_show("All presence updates will appear in chat windows.");
        } else if (strcmp(args[1], "online") == 0) {
            cons_show("Only online/offline presence updates will appear in chat windows.");
        } else {
            cons_show("Presence updates will not appear in chat windows.");
        }
    }

    if (strcmp(args[0], "muc") == 0) {
        prefs_set_string(PREF_STATUSES_MUC, args[1]);
        if (strcmp(args[1], "all") == 0) {
            cons_show("All presence updates will appear in chat room windows.");
        } else if (strcmp(args[1], "online") == 0) {
            cons_show("Only join/leave presence updates will appear in chat room windows.");
        } else {
            cons_show("Presence updates will not appear in chat room windows.");
        }
    }

    return TRUE;
}

gboolean
cmd_vercheck(ProfWin *window, const char * const command, gchar **args)
{
    int num_args = g_strv_length(args);

    if (num_args == 0) {
        cons_check_version(TRUE);
        return TRUE;
    } else {
        return _cmd_set_boolean_preference(args[0], command, "Version checking", PREF_VERCHECK);
    }
}

gboolean
cmd_xmlconsole(ProfWin *window, const char * const command, gchar **args)
{
    if (!ui_xmlconsole_exists()) {
        ui_create_xmlconsole_win();
    } else {
        ui_open_xmlconsole_win();
    }

    return TRUE;
}

gboolean
cmd_flash(ProfWin *window, const char * const command, gchar **args)
{
    return _cmd_set_boolean_preference(args[0], command, "Screen flash", PREF_FLASH);
}

gboolean
cmd_intype(ProfWin *window, const char * const command, gchar **args)
{
    return _cmd_set_boolean_preference(args[0], command, "Show contact typing", PREF_INTYPE);
}

gboolean
cmd_splash(ProfWin *window, const char * const command, gchar **args)
{
    return _cmd_set_boolean_preference(args[0], command, "Splash screen", PREF_SPLASH);
}

gboolean
cmd_autoconnect(ProfWin *window, const char * const command, gchar **args)
{
    if (strcmp(args[0], "off") == 0) {
        prefs_set_string(PREF_CONNECT_ACCOUNT, NULL);
        cons_show("Autoconnect account disabled.");
    } else if (strcmp(args[0], "set") == 0) {
        prefs_set_string(PREF_CONNECT_ACCOUNT, args[1]);
        cons_show("Autoconnect account set to: %s.", args[1]);
    } else {
        cons_bad_cmd_usage(command);
    }
    return true;
}

gboolean
cmd_chlog(ProfWin *window, const char * const command, gchar **args)
{
    gboolean result = _cmd_set_boolean_preference(args[0], command, "Chat logging", PREF_CHLOG);

    // if set to off, disable history
    if (result == TRUE && (strcmp(args[0], "off") == 0)) {
        prefs_set_boolean(PREF_HISTORY, FALSE);
    }

    return result;
}

gboolean
cmd_grlog(ProfWin *window, const char * const command, gchar **args)
{
    gboolean result = _cmd_set_boolean_preference(args[0], command, "Groupchat logging", PREF_GRLOG);

    return result;
}

gboolean
cmd_history(ProfWin *window, const char * const command, gchar **args)
{
    gboolean result = _cmd_set_boolean_preference(args[0], command, "Chat history", PREF_HISTORY);

    // if set to on, set chlog
    if (result == TRUE && (strcmp(args[0], "on") == 0)) {
        prefs_set_boolean(PREF_CHLOG, TRUE);
    }

    return result;
}

gboolean
cmd_carbons(ProfWin *window, const char * const command, gchar **args)
{
    gboolean result = _cmd_set_boolean_preference(args[0], command, "Message carbons preference", PREF_CARBONS);

    jabber_conn_status_t conn_status = jabber_get_connection_status();

    if (conn_status == JABBER_CONNECTED) {
        // enable carbons
        if (strcmp(args[0], "on") == 0) {
            iq_enable_carbons();
        }
        else if (strcmp(args[0], "off") == 0){
            iq_disable_carbons();
        }
    }

    return result;
}

gboolean
cmd_receipts(ProfWin *window, const char * const command, gchar **args)
{
    if (g_strcmp0(args[0], "send") == 0) {
        return _cmd_set_boolean_preference(args[1], command, "Send delivery receipts", PREF_RECEIPTS_SEND);
    } else if (g_strcmp0(args[0], "request") == 0) {
        return _cmd_set_boolean_preference(args[1], command, "Request delivery receipets", PREF_RECEIPTS_REQUEST);
    } else {
        cons_bad_cmd_usage(command);
        return TRUE;
    }
}

gboolean
cmd_away(ProfWin *window, const char * const command, gchar **args)
{
    _update_presence(RESOURCE_AWAY, "away", args);
    return TRUE;
}

gboolean
cmd_online(ProfWin *window, const char * const command, gchar **args)
{
    _update_presence(RESOURCE_ONLINE, "online", args);
    return TRUE;
}

gboolean
cmd_dnd(ProfWin *window, const char * const command, gchar **args)
{
    _update_presence(RESOURCE_DND, "dnd", args);
    return TRUE;
}

gboolean
cmd_chat(ProfWin *window, const char * const command, gchar **args)
{
    _update_presence(RESOURCE_CHAT, "chat", args);
    return TRUE;
}

gboolean
cmd_xa(ProfWin *window, const char * const command, gchar **args)
{
    _update_presence(RESOURCE_XA, "xa", args);
    return TRUE;
}

gboolean
cmd_pgp(ProfWin *window, const char * const command, gchar **args)
{
#ifdef HAVE_LIBGPGME
    if (args[0] == NULL) {
        cons_bad_cmd_usage(command);
        return TRUE;
    }

    if (g_strcmp0(args[0], "log") == 0) {
        char *choice = args[1];
        if (g_strcmp0(choice, "on") == 0) {
            prefs_set_string(PREF_PGP_LOG, "on");
            cons_show("PGP messages will be logged as plaintext.");
            if (!prefs_get_boolean(PREF_CHLOG)) {
                cons_show("Chat logging is currently disabled, use '/chlog on' to enable.");
            }
        } else if (g_strcmp0(choice, "off") == 0) {
            prefs_set_string(PREF_PGP_LOG, "off");
            cons_show("PGP message logging disabled.");
        } else if (g_strcmp0(choice, "redact") == 0) {
            prefs_set_string(PREF_PGP_LOG, "redact");
            cons_show("PGP messages will be logged as '[redacted]'.");
            if (!prefs_get_boolean(PREF_CHLOG)) {
                cons_show("Chat logging is currently disabled, use '/chlog on' to enable.");
            }
        } else {
            cons_bad_cmd_usage(command);
        }
        return TRUE;
    }

    if (g_strcmp0(args[0], "keys") == 0) {
        GHashTable *keys = p_gpg_list_keys();
        if (!keys || g_hash_table_size(keys) == 0) {
            cons_show("No keys found");
            return TRUE;
        }

        cons_show("PGP keys:");
        GList *keylist = g_hash_table_get_keys(keys);
        GList *curr = keylist;
        while (curr) {
            ProfPGPKey *key = g_hash_table_lookup(keys, curr->data);
            cons_show("  %s", key->name);
            cons_show("    ID          : %s", key->id);
            cons_show("    Fingerprint : %s", key->fp);
            if (key->secret) {
                cons_show("    Type        : PUBLIC, PRIVATE");
            } else {
                cons_show("    Type        : PUBLIC");
            }
            curr = g_list_next(curr);
        }
        g_list_free(keylist);
        p_gpg_free_keys(keys);
        return TRUE;
    }

    if (g_strcmp0(args[0], "setkey") == 0) {
        jabber_conn_status_t conn_status = jabber_get_connection_status();
        if (conn_status != JABBER_CONNECTED) {
            cons_show("You are not currently connected.");
            return TRUE;
        }

        char *jid = args[1];
        if (!args[1]) {
            cons_bad_cmd_usage(command);
            return TRUE;
        }

        char *keyid = args[2];
        if (!args[2]) {
            cons_bad_cmd_usage(command);
            return TRUE;
        }

        gboolean res = p_gpg_addkey(jid, keyid);
        if (!res) {
            cons_show("Key ID not found.");
        } else {
            cons_show("Key %s set for %s.", keyid, jid);
        }

        return TRUE;
    }

    if (g_strcmp0(args[0], "fps") == 0) {
        jabber_conn_status_t conn_status = jabber_get_connection_status();
        if (conn_status != JABBER_CONNECTED) {
            cons_show("You are not currently connected.");
            return TRUE;
        }
        GHashTable *fingerprints = p_gpg_fingerprints();
        GList *jids = g_hash_table_get_keys(fingerprints);
        if (!jids) {
            cons_show("No PGP fingerprints available.");
            return TRUE;
        }

        cons_show("Known PGP fingerprints:");
        GList *curr = jids;
        while (curr) {
            char *jid = curr->data;
            char *fingerprint = g_hash_table_lookup(fingerprints, jid);
            cons_show("  %s: %s", jid, fingerprint);
            curr = g_list_next(curr);
        }
        g_list_free(jids);
        return TRUE;
    }

    if (g_strcmp0(args[0], "libver") == 0) {
        const char *libver = p_gpg_libver();
        if (!libver) {
            cons_show("Could not get libgpgme version");
            return TRUE;
        }

        GString *fullstr = g_string_new("Using libgpgme version ");
        g_string_append(fullstr, libver);
        cons_show("%s", fullstr->str);
        g_string_free(fullstr, TRUE);

        return TRUE;
    }

    if (g_strcmp0(args[0], "start") == 0) {
        jabber_conn_status_t conn_status = jabber_get_connection_status();
        if (conn_status != JABBER_CONNECTED) {
            cons_show("You must be connected to start PGP encrpytion.");
            return TRUE;
        }

        if (window->type != WIN_CHAT && args[1] == NULL) {
            cons_show("You must be in a regular chat window to start PGP encrpytion.");
            return TRUE;
        }

        ProfChatWin *chatwin = NULL;

        if (args[1]) {
            char *contact = args[1];
            char *barejid = roster_barejid_from_name(contact);
            if (barejid == NULL) {
                barejid = contact;
            }

            chatwin = wins_get_chat(barejid);
            if (!chatwin) {
                chatwin = ui_ev_new_chat_win(barejid);
            }
            ui_ev_focus_win((ProfWin*)chatwin);
        } else {
            chatwin = (ProfChatWin*)window;
            assert(chatwin->memcheck == PROFCHATWIN_MEMCHECK);
        }

        if (chatwin->enc_mode == PROF_ENC_OTR) {
            ui_current_print_formatted_line('!', 0, "You must end the OTR session to start PGP encryption.");
            return TRUE;
        }

        if (chatwin->enc_mode == PROF_ENC_PGP) {
            ui_current_print_formatted_line('!', 0, "You have already started PGP encryption.");
            return TRUE;
        }

        ProfAccount *account = accounts_get_account(jabber_get_account_name());
        if (!p_gpg_valid_key(account->pgp_keyid)) {
            ui_current_print_formatted_line('!', 0, "You must specify a valid PGP key ID for this account to start PGP encryption.");
            account_free(account);
            return TRUE;
        }
        account_free(account);

        if (!p_gpg_available(chatwin->barejid)) {
            ui_current_print_formatted_line('!', 0, "No PGP key found for %s.", chatwin->barejid);
            return TRUE;
        }

        chatwin->enc_mode = PROF_ENC_PGP;
        ui_current_print_formatted_line('!', 0, "PGP encyption enabled.");
        return TRUE;
    }

    if (g_strcmp0(args[0], "end") == 0) {
        jabber_conn_status_t conn_status = jabber_get_connection_status();
        if (conn_status != JABBER_CONNECTED) {
            cons_show("You are not currently connected.");
            return TRUE;
        }

        if (window->type != WIN_CHAT) {
            cons_show("You must be in a regular chat window to end PGP encrpytion.");
            return TRUE;
        }

        ProfChatWin *chatwin = (ProfChatWin*)window;
        if (chatwin->enc_mode != PROF_ENC_PGP) {
            ui_current_print_formatted_line('!', 0, "PGP encryption is not currently enabled.");
            return TRUE;
        }

        chatwin->enc_mode = PROF_ENC_NONE;
        ui_current_print_formatted_line('!', 0, "PGP encyption disabled.");
        return TRUE;
    }

    cons_bad_cmd_usage(command);
    return TRUE;
#else
    cons_show("This version of Profanity has not been built with PGP support enabled");
    return TRUE;
#endif

}

gboolean
cmd_otr(ProfWin *window, const char * const command, gchar **args)
{
#ifdef HAVE_LIBOTR
    if (args[0] == NULL) {
        cons_bad_cmd_usage(command);
        return TRUE;
    }

    if (strcmp(args[0], "log") == 0) {
        char *choice = args[1];
        if (g_strcmp0(choice, "on") == 0) {
            prefs_set_string(PREF_OTR_LOG, "on");
            cons_show("OTR messages will be logged as plaintext.");
            if (!prefs_get_boolean(PREF_CHLOG)) {
                cons_show("Chat logging is currently disabled, use '/chlog on' to enable.");
            }
        } else if (g_strcmp0(choice, "off") == 0) {
            prefs_set_string(PREF_OTR_LOG, "off");
            cons_show("OTR message logging disabled.");
        } else if (g_strcmp0(choice, "redact") == 0) {
            prefs_set_string(PREF_OTR_LOG, "redact");
            cons_show("OTR messages will be logged as '[redacted]'.");
            if (!prefs_get_boolean(PREF_CHLOG)) {
                cons_show("Chat logging is currently disabled, use '/chlog on' to enable.");
            }
        } else {
            cons_bad_cmd_usage(command);
        }
        return TRUE;

    } else if (strcmp(args[0], "libver") == 0) {
        char *version = otr_libotr_version();
        cons_show("Using libotr version %s", version);
        return TRUE;

    } else if (strcmp(args[0], "policy") == 0) {
        if (args[1] == NULL) {
            char *policy = prefs_get_string(PREF_OTR_POLICY);
            cons_show("OTR policy is now set to: %s", policy);
            prefs_free_string(policy);
            return TRUE;
        }

        char *choice = args[1];
        if ((g_strcmp0(choice, "manual") != 0) &&
                (g_strcmp0(choice, "opportunistic") != 0) &&
                (g_strcmp0(choice, "always") != 0)) {
            cons_show("OTR policy can be set to: manual, opportunistic or always.");
            return TRUE;
        }

        char *contact = args[2];
        if (contact == NULL) {
            prefs_set_string(PREF_OTR_POLICY, choice);
            cons_show("OTR policy is now set to: %s", choice);
            return TRUE;
        } else {
            if (jabber_get_connection_status() != JABBER_CONNECTED) {
                cons_show("You must be connected to set the OTR policy for a contact.");
                return TRUE;
            }
            char *contact_jid = roster_barejid_from_name(contact);
            if (contact_jid == NULL) {
                contact_jid = contact;
            }
            accounts_add_otr_policy(jabber_get_account_name(), contact_jid, choice);
            cons_show("OTR policy for %s set to: %s", contact_jid, choice);
            return TRUE;
        }
    }

    if (jabber_get_connection_status() != JABBER_CONNECTED) {
        cons_show("You must be connected with an account to load OTR information.");
        return TRUE;
    }

    if (strcmp(args[0], "gen") == 0) {
        ProfAccount *account = accounts_get_account(jabber_get_account_name());
        otr_keygen(account);
        account_free(account);
        return TRUE;

    } else if (strcmp(args[0], "myfp") == 0) {
        if (!otr_key_loaded()) {
            ui_current_print_formatted_line('!', 0, "You have not generated or loaded a private key, use '/otr gen'");
            return TRUE;
        }

        char *fingerprint = otr_get_my_fingerprint();
        ui_current_print_formatted_line('!', 0, "Your OTR fingerprint: %s", fingerprint);
        free(fingerprint);
        return TRUE;

    } else if (strcmp(args[0], "theirfp") == 0) {
        if (window->type != WIN_CHAT) {
            ui_current_print_line("You must be in a regular chat window to view a recipient's fingerprint.");
            return TRUE;
        }

        ProfChatWin *chatwin = (ProfChatWin*)window;
        assert(chatwin->memcheck == PROFCHATWIN_MEMCHECK);
        if (chatwin->enc_mode != PROF_ENC_OTR) {
            ui_current_print_formatted_line('!', 0, "You are not currently in an OTR session.");
            return TRUE;
        }

        char *fingerprint = otr_get_their_fingerprint(chatwin->barejid);
        ui_current_print_formatted_line('!', 0, "%s's OTR fingerprint: %s", chatwin->barejid, fingerprint);
        free(fingerprint);
        return TRUE;

    } else if (strcmp(args[0], "start") == 0) {
        // recipient supplied
        if (args[1]) {
            char *contact = args[1];
            char *barejid = roster_barejid_from_name(contact);
            if (barejid == NULL) {
                barejid = contact;
            }

            ProfChatWin *chatwin = wins_get_chat(barejid);
            if (!chatwin) {
                chatwin = ui_ev_new_chat_win(barejid);
            }
            ui_ev_focus_win((ProfWin*)chatwin);

            if (chatwin->enc_mode == PROF_ENC_PGP) {
                ui_current_print_formatted_line('!', 0, "You must disable PGP encryption before starting an OTR session.");
                return TRUE;
            }

            if (chatwin->enc_mode == PROF_ENC_OTR) {
                ui_current_print_formatted_line('!', 0, "You are already in an OTR session.");
                return TRUE;
            }

            if (!otr_key_loaded()) {
                ui_current_print_formatted_line('!', 0, "You have not generated or loaded a private key, use '/otr gen'");
                return TRUE;
            }

            if (!otr_is_secure(barejid)) {
                char *otr_query_message = otr_start_query();
                char *id = message_send_chat_otr(barejid, otr_query_message);
                free(id);
                return TRUE;
            }

            ui_gone_secure(barejid, otr_is_trusted(barejid));
            return TRUE;

        // no recipient, use current chat
        } else {
            if (window->type != WIN_CHAT) {
                ui_current_print_line("You must be in a regular chat window to start an OTR session.");
                return TRUE;
            }

            ProfChatWin *chatwin = (ProfChatWin*)window;
            assert(chatwin->memcheck == PROFCHATWIN_MEMCHECK);
            if (chatwin->enc_mode == PROF_ENC_PGP) {
                ui_current_print_formatted_line('!', 0, "You must disable PGP encryption before starting an OTR session.");
                return TRUE;
            }

            if (chatwin->enc_mode == PROF_ENC_OTR) {
                ui_current_print_formatted_line('!', 0, "You are already in an OTR session.");
                return TRUE;
            }

            if (!otr_key_loaded()) {
                ui_current_print_formatted_line('!', 0, "You have not generated or loaded a private key, use '/otr gen'");
                return TRUE;
            }

            char *otr_query_message = otr_start_query();
            char *id = message_send_chat_otr(chatwin->barejid, otr_query_message);
            free(id);
            return TRUE;
        }

    } else if (strcmp(args[0], "end") == 0) {
        if (window->type != WIN_CHAT) {
            ui_current_print_line("You must be in a regular chat window to use OTR.");
            return TRUE;
        }

        ProfChatWin *chatwin = (ProfChatWin*)window;
        assert(chatwin->memcheck == PROFCHATWIN_MEMCHECK);
        if (chatwin->enc_mode != PROF_ENC_OTR) {
            ui_current_print_formatted_line('!', 0, "You are not currently in an OTR session.");
            return TRUE;
        }

        ui_gone_insecure(chatwin->barejid);
        otr_end_session(chatwin->barejid);
        return TRUE;

    } else if (strcmp(args[0], "trust") == 0) {
        if (window->type != WIN_CHAT) {
            ui_current_print_line("You must be in an OTR session to trust a recipient.");
            return TRUE;
        }

        ProfChatWin *chatwin = (ProfChatWin*)window;
        assert(chatwin->memcheck == PROFCHATWIN_MEMCHECK);
        if (chatwin->enc_mode != PROF_ENC_OTR) {
            ui_current_print_formatted_line('!', 0, "You are not currently in an OTR session.");
            return TRUE;
        }

        ui_trust(chatwin->barejid);
        otr_trust(chatwin->barejid);
        return TRUE;

    } else if (strcmp(args[0], "untrust") == 0) {
        if (window->type != WIN_CHAT) {
            ui_current_print_line("You must be in an OTR session to untrust a recipient.");
            return TRUE;
        }

        ProfChatWin *chatwin = (ProfChatWin*)window;
        assert(chatwin->memcheck == PROFCHATWIN_MEMCHECK);
        if (chatwin->enc_mode != PROF_ENC_OTR) {
            ui_current_print_formatted_line('!', 0, "You are not currently in an OTR session.");
            return TRUE;
        }

        ui_untrust(chatwin->barejid);
        otr_untrust(chatwin->barejid);
        return TRUE;

    } else if (strcmp(args[0], "secret") == 0) {
        if (window->type != WIN_CHAT) {
            ui_current_print_line("You must be in an OTR session to trust a recipient.");
            return TRUE;
        }

        ProfChatWin *chatwin = (ProfChatWin*)window;
        assert(chatwin->memcheck == PROFCHATWIN_MEMCHECK);
        if (chatwin->enc_mode != PROF_ENC_OTR) {
            ui_current_print_formatted_line('!', 0, "You are not currently in an OTR session.");
            return TRUE;
        }

        char *secret = args[1];
        if (secret == NULL) {
            cons_bad_cmd_usage(command);
            return TRUE;
        }

        otr_smp_secret(chatwin->barejid, secret);
        return TRUE;

    } else if (strcmp(args[0], "question") == 0) {
        char *question = args[1];
        char *answer = args[2];
        if (question == NULL || answer == NULL) {
            cons_bad_cmd_usage(command);
            return TRUE;
        }

        if (window->type != WIN_CHAT) {
            ui_current_print_line("You must be in an OTR session to trust a recipient.");
            return TRUE;
        }

        ProfChatWin *chatwin = (ProfChatWin*)window;
        assert(chatwin->memcheck == PROFCHATWIN_MEMCHECK);
        if (chatwin->enc_mode != PROF_ENC_OTR) {
            ui_current_print_formatted_line('!', 0, "You are not currently in an OTR session.");
            return TRUE;
        }

        otr_smp_question(chatwin->barejid, question, answer);
        return TRUE;

    } else if (strcmp(args[0], "answer") == 0) {
        if (window->type != WIN_CHAT) {
            ui_current_print_line("You must be in an OTR session to trust a recipient.");
            return TRUE;
        }

        ProfChatWin *chatwin = (ProfChatWin*)window;
        assert(chatwin->memcheck == PROFCHATWIN_MEMCHECK);
        if (chatwin->enc_mode != PROF_ENC_OTR) {
            ui_current_print_formatted_line('!', 0, "You are not currently in an OTR session.");
            return TRUE;
        }

        char *answer = args[1];
        if (answer == NULL) {
            cons_bad_cmd_usage(command);
            return TRUE;
        }

        otr_smp_answer(chatwin->barejid, answer);
        return TRUE;

    } else {
        cons_bad_cmd_usage(command);
        return TRUE;
    }
#else
    cons_show("This version of Profanity has not been built with OTR support enabled");
    return TRUE;
#endif
}

gboolean
cmd_encwarn(ProfWin *window, const char * const command, gchar **args)
{
    return _cmd_set_boolean_preference(args[0], command, "Encryption warning message", PREF_ENC_WARN);
}

// helper function for status change commands
static void
_update_presence(const resource_presence_t resource_presence,
    const char * const show, gchar **args)
{
    char *msg = NULL;
    int num_args = g_strv_length(args);
    if (num_args == 1) {
        msg = args[0];
    }

    jabber_conn_status_t conn_status = jabber_get_connection_status();

    if (conn_status != JABBER_CONNECTED) {
        cons_show("You are not currently connected.");
    } else {
        cl_ev_presence_send(resource_presence, msg, 0);
        ui_update_presence(resource_presence, msg, show);
    }
}

// helper function for boolean preference commands
static gboolean
_cmd_set_boolean_preference(gchar *arg, const char * const command,
    const char * const display, preference_t pref)
{
    GString *enabled = g_string_new(display);
    g_string_append(enabled, " enabled.");

    GString *disabled = g_string_new(display);
    g_string_append(disabled, " disabled.");

    if (arg == NULL) {
        cons_bad_cmd_usage(command);
    } else if (strcmp(arg, "on") == 0) {
        cons_show(enabled->str);
        prefs_set_boolean(pref, TRUE);
    } else if (strcmp(arg, "off") == 0) {
        cons_show(disabled->str);
        prefs_set_boolean(pref, FALSE);
    } else {
        cons_bad_cmd_usage(command);
    }

    g_string_free(enabled, TRUE);
    g_string_free(disabled, TRUE);

    return TRUE;
}

//static void
//_cmd_show_filtered_help(char *heading, gchar *cmd_filter[], int filter_size)
//{
//    ProfWin *console = wins_get_console();
//    cons_show("");
//    win_print(console, '-', NULL, 0, THEME_WHITE_BOLD, "", heading);
//
//    GList *ordered_commands = NULL;
//    int i;
//    for (i = 0; i < filter_size; i++) {
//        Command *pcmd = g_hash_table_lookup(commands, cmd_filter[i]);
//        ordered_commands = g_list_insert_sorted(ordered_commands, pcmd->cmd, (GCompareFunc)g_strcmp0);
//    }
//
//    int maxlen = 0;
//    GList *curr = ordered_commands;
//    while (curr) {
//        gchar *cmd = curr->data;
//        int len = strlen(cmd);
//        if (len > maxlen) maxlen = len;
//        curr = g_list_next(curr);
//    }
//
//    GString *cmds = g_string_new("");
//    curr = ordered_commands;
//    int count = 0;
//    while (curr) {
//        gchar *cmd = curr->data;
//        if (count == 5) {
//            cons_show(cmds->str);
//            g_string_free(cmds, TRUE);
//            cmds = g_string_new("");
//            count = 0;
//        }
//        g_string_append_printf(cmds, "%-*s", maxlen + 1, cmd);
//        curr = g_list_next(curr);
//        count++;
//    }
//    cons_show(cmds->str);
//    g_string_free(cmds, TRUE);
//    g_list_free(ordered_commands);
//    g_list_free(curr);
//
//    cons_show("");
//    cons_show("Use /help [command] without the leading slash, for help on a specific command");
//    cons_show("");
//}
