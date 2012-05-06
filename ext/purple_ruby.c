/*
 * Author: yong@intridea.com 
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301  USA
 *
 */

#include <libpurple/account.h>
#include <libpurple/conversation.h>
#include <libpurple/core.h>
#include <libpurple/debug.h>
#include <libpurple/blist.h>
#include <libpurple/cipher.h>
#include <libpurple/eventloop.h>
#include <libpurple/ft.h>
#include <libpurple/log.h>
#include <libpurple/notify.h>
#include <libpurple/prefs.h>
#include <libpurple/prpl.h>
#include <libpurple/pounce.h>
#include <libpurple/request.h>
#include <libpurple/savedstatuses.h>
#include <libpurple/sound.h>
#include <libpurple/status.h>
#include <libpurple/util.h>
#include <libpurple/whiteboard.h>
#include <libpurple/network.h>

#include <ruby.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>

#ifndef RSTRING_PTR 
#define RSTRING_PTR(s) (RSTRING(s)->ptr) 
#endif 
#ifndef RSTRING_LEN 
#define RSTRING_LEN(s) (RSTRING(s)->len) 
#endif

#define PURPLE_GLIB_READ_COND  (G_IO_IN | G_IO_HUP | G_IO_ERR)
#define PURPLE_GLIB_WRITE_COND (G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL)

// Ruby to C
#define PURPLE_ACCOUNT(account) get_account_from_ruby_object(account)

#define PURPLE_BUDDY( buddy, buddy_pointer) Data_Get_Struct( buddy, PurpleBuddy, buddy_pointer )

// C to Ruby
#define RB_BLIST_BUDDY(purple_buddy_pointer) Data_Wrap_Struct(cBuddy, NULL, NULL, purple_buddy_pointer)
#define RB_ACCOUNT(purple_account_pointer) Data_Wrap_Struct(cAccount, NULL, NULL, purple_account_pointer)

typedef struct _PurpleGLibIOClosure {
	PurpleInputFunction function;
	guint result;
	gpointer data;
} PurpleGLibIOClosure;

static void purple_glib_io_destroy(gpointer data)
{
	g_free(data);
}

static PurpleAccount* get_account_from_ruby_object(VALUE acc){
	PurpleAccount* account = NULL;
	Data_Get_Struct( acc, PurpleAccount, account );
	return purple_accounts_find(account->username,account->protocol_id);
}

static gboolean purple_glib_io_invoke(GIOChannel *source, GIOCondition condition, gpointer data)
{
	PurpleGLibIOClosure *closure = data;
	PurpleInputCondition purple_cond = 0;

	if (condition & PURPLE_GLIB_READ_COND)
		purple_cond |= PURPLE_INPUT_READ;
	if (condition & PURPLE_GLIB_WRITE_COND)
		purple_cond |= PURPLE_INPUT_WRITE;

	closure->function(closure->data, g_io_channel_unix_get_fd(source),
			  purple_cond);

	return TRUE;
}

static guint glib_input_add(gint fd, PurpleInputCondition condition, PurpleInputFunction function,
							   gpointer data)
{
	PurpleGLibIOClosure *closure = g_new0(PurpleGLibIOClosure, 1);
	GIOChannel *channel;
	GIOCondition cond = 0;
	
	closure->function = function;
	closure->data = data;

	if (condition & PURPLE_INPUT_READ)
		cond |= PURPLE_GLIB_READ_COND;
	if (condition & PURPLE_INPUT_WRITE)
		cond |= PURPLE_GLIB_WRITE_COND;

	channel = g_io_channel_unix_new(fd);
	closure->result = g_io_add_watch_full(channel, G_PRIORITY_DEFAULT, cond,
					      purple_glib_io_invoke, closure, purple_glib_io_destroy);

	g_io_channel_unref(channel);
	return closure->result;
}

static PurpleEventLoopUiOps glib_eventloops = 
{
	g_timeout_add,
	g_source_remove,
	glib_input_add,
	g_source_remove,
	NULL,
#if GLIB_CHECK_VERSION(2,14,0)
	g_timeout_add_seconds,
#else
	NULL,
#endif

	/* padding */
	NULL,
	NULL,
	NULL
};

static VALUE cPurpleRuby;
static VALUE cConnectionError;
VALUE cAccount;
static VALUE cBuddy;
static VALUE cStatus;

const char* UI_ID = "purplegw";
static GMainLoop *main_loop = NULL;
static GHashTable* data_hash_table = NULL;
static GHashTable* fd_hash_table = NULL;
ID CALL;
extern PurpleAccountUiOps account_ops;

static VALUE im_handler = Qnil;
static VALUE signed_on_handler = Qnil;
static VALUE signed_off_handler = Qnil;
static VALUE connection_error_handler = Qnil;
static VALUE notify_message_handler = Qnil;
static VALUE user_info_handler = Qnil;
static VALUE request_handler = Qnil;
static VALUE blist_update_handler = Qnil;
static VALUE blist_ready_handler = Qnil;
static VALUE ipc_handler = Qnil;
static VALUE timer_handler = Qnil;
guint timer_timeout = 0;
VALUE new_buddy_handler = Qnil;

extern void
finch_connection_report_disconnect(PurpleConnection *gc, PurpleConnectionError reason,
		const char *text);
		
extern void finch_connections_init();

VALUE inspect_rb_obj(VALUE obj)
{
  return rb_funcall(obj, rb_intern("inspect"), 0, 0);
}

void set_callback(VALUE* handler, const char* handler_name)
{
  if (!rb_block_given_p()) {
    rb_raise(rb_eArgError, "%s: no block", handler_name);
  }
  
  if (Qnil != *handler) {
    rb_raise(rb_eArgError, "%s should only be assigned once", handler_name);
  }
  
  *handler = rb_block_proc();
  /*
  * If you create a Ruby object from C and store it in a C global variable without 
  * exporting it to Ruby, you must at least tell the garbage collector about it, 
  * lest ye be reaped inadvertently:
  */
  rb_global_variable(handler);
  
  if (rb_obj_class(*handler) != rb_cProc) {
    rb_raise(rb_eTypeError, "%s got unexpected value: %s", handler_name, 
       RSTRING_PTR(inspect_rb_obj(*handler)));
  }
}

void check_callback(VALUE handler, const char* handler_name){
  if (rb_obj_class(handler) != rb_cProc) {
    rb_raise(rb_eTypeError, "%s has unexpected value: %s",
      handler_name,
      RSTRING_PTR(inspect_rb_obj(handler)));
  }
}

void report_disconnect(PurpleConnection *gc, PurpleConnectionError reason, const char *text)
{
  if (Qnil != connection_error_handler) {
    VALUE args[3];
    args[0] = Data_Wrap_Struct(cAccount, NULL, NULL, purple_connection_get_account(gc));
    args[1] = INT2FIX(reason);
    args[2] = rb_str_new2(text);
    check_callback(connection_error_handler, "connection_error_handler");
    VALUE v = rb_funcall2(connection_error_handler, CALL, 3, args);
    
    if (v != Qnil && v != Qfalse) {
      finch_connection_report_disconnect(gc, reason, text);
    }
  }
}

static void* notify_message(PurpleNotifyMsgType type, 
	const char *title,
	const char *primary, 
	const char *secondary)
{
  if (notify_message_handler != Qnil) {
    VALUE args[4];
    args[0] = INT2FIX(type);
    args[1] = rb_str_new2(NULL == title ? "" : title);
    args[2] = rb_str_new2(NULL == primary ? "" : primary);
    args[3] = rb_str_new2(NULL == secondary ? "" : secondary);
    check_callback(notify_message_handler, "notify_message_handler");
    rb_funcall2(notify_message_handler, CALL, 4, args);
  }
  
  return NULL;
}

static void write_conv(PurpleConversation *conv, const char *who, const char *alias,
			const char *message, PurpleMessageFlags flags, time_t mtime)
{	
  if (im_handler != Qnil) {
    PurpleAccount* account = purple_conversation_get_account(conv);
    if (strcmp(purple_account_get_protocol_id(account), "prpl-msn") == 0 &&
         (strstr(message, "Message could not be sent") != NULL ||
          strstr(message, "Message was not sent") != NULL ||
          strstr(message, "Message may have not been sent") != NULL
         )
        ) {
      /* I have seen error like 'msn: Connection error from Switchboard server'.
       * In that case, libpurple will notify user with two regular im message.
       * The first message is an error message, the second one is the original message that failed to send.
       */
      notify_message(PURPLE_CONNECTION_ERROR_NETWORK_ERROR, message, purple_account_get_protocol_id(account), who);
    } else {
      VALUE args[3];
      args[0] = Data_Wrap_Struct(cAccount, NULL, NULL, account);
      args[1] = rb_str_new2(who);
      args[2] = rb_str_new2(message);
      check_callback(im_handler, "im_handler");
      rb_funcall2(im_handler, CALL, 3, args);
    }
  }
}

static void update_blist(PurpleBuddyList *list, PurpleBlistNode *node)
{
	if (blist_update_handler != Qnil && PURPLE_BLIST_NODE_IS_BUDDY(node)) {
		PurpleBuddy *buddy = (PurpleBuddy *)node;
		check_callback(blist_update_handler, "blist_update_handler");
		VALUE args[2];
		args[0] = RB_BLIST_BUDDY(buddy);
		args[1] = Data_Wrap_Struct(cAccount, NULL, NULL, purple_buddy_get_account(buddy));
		rb_funcall2(blist_update_handler, CALL, 2, args);		
	}
}
static PurpleConversationUiOps conv_uiops = 
{
	NULL,                      /* create_conversation  */
	NULL,                      /* destroy_conversation */
	NULL,                      /* write_chat           */
	NULL,                      /* write_im             */
	write_conv,           /* write_conv           */
	NULL,                      /* chat_add_users       */
	NULL,                      /* chat_rename_user     */
	NULL,                      /* chat_remove_users    */
	NULL,                      /* chat_update_user     */
	NULL,                      /* present              */
	NULL,                      /* has_focus            */
	NULL,                      /* custom_smiley_add    */
	NULL,                      /* custom_smiley_write  */
	NULL,                      /* custom_smiley_close  */
	NULL,                      /* send_confirm         */
	NULL,
	NULL,
	NULL,
	NULL
};

static PurpleBlistUiOps blist_uiops = 
{
	NULL,
	NULL,
	NULL,
	update_blist,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,                     
	NULL,                      
	NULL,                      
	NULL,                      
	NULL,                      
	NULL,                      

};

static PurpleConnectionUiOps connection_ops = 
{
	NULL, /* connect_progress */
	NULL, /* connected */
	NULL, /* disconnected */
	NULL, /* notice */
	NULL,
	NULL, /* network_connected */
	NULL, /* network_disconnected */
	report_disconnect,
	NULL,
	NULL,
	NULL
};

static void* request_action(const char *title, const char *primary, const char *secondary,
                            int default_action,
                            PurpleAccount *account, 
                            const char *who, 
                            PurpleConversation *conv,
                            void *user_data, 
                            size_t action_count, 
                            va_list actions)
{
  if (request_handler != Qnil) {
	  VALUE args[4];
    args[0] = rb_str_new2(NULL == title ? "" : title);
    args[1] = rb_str_new2(NULL == primary ? "" : primary);
    args[2] = rb_str_new2(NULL == secondary ? "" : secondary);
    args[3] = rb_str_new2(NULL == who ? "" : who);
    check_callback(request_handler, "request_handler");
    VALUE v = rb_funcall2(request_handler, CALL, 4, args);
	  
	  if (v != Qnil && v != Qfalse) {
	    /*const char *text =*/ va_arg(actions, const char *);
	    GCallback ok_cb = va_arg(actions, GCallback);
      ((PurpleRequestActionCb)ok_cb)(user_data, default_action);
    }
  }
  
  return NULL;
}

static void notify_user_info(PurpleConnection *gc, const char *who, PurpleNotifyUserInfo *user_info){
	PurpleAccount* account =	purple_connection_get_account (gc);
	PurpleBuddy* buddy = purple_find_buddy (account, who);
	if(user_info_handler != Qnil){
		VALUE args[2];
		args[0] = RB_BLIST_BUDDY( buddy );
		GList *l;
		VALUE hash = rb_hash_new();
		for (l = purple_notify_user_info_get_entries(user_info); l != NULL; l = l->next) {
			//PurpleNotifyUserInfoEntry *user_info_entry = l->data;			
			if (purple_notify_user_info_entry_get_label(l->data) && purple_notify_user_info_entry_get_value(l->data)){
				rb_hash_aset(hash, rb_str_new2(purple_notify_user_info_entry_get_label(l->data)), rb_str_new2(purple_notify_user_info_entry_get_value(l->data)));
			}
		}
		args[1] = hash;
		rb_funcall2(user_info_handler, CALL, 2, args);	
	}
}

static PurpleRequestUiOps request_ops =
{
	NULL,           /*request_input*/
	NULL,           /*request_choice*/
	request_action, /*request_action*/
	NULL,           /*request_fields*/
	NULL,           /*request_file*/
	NULL,           /*close_request*/
	NULL,           /*request_folder*/
	NULL,
	NULL,
	NULL,
	NULL
};

static PurpleNotifyUiOps notify_ops =
{
  notify_message, /*notify_message*/
  NULL,           /*notify_email*/ 
  NULL,           /*notify_emails*/
  NULL,           /*notify_formatted*/
  NULL,           /*notify_searchresults*/
  NULL,           /*notify_searchresults_new_rows*/
  notify_user_info,           /*notify_userinfo*/
  NULL,           /*notify_uri*/
  NULL,           /*close_notify*/
  NULL,
  NULL,
  NULL,
  NULL,
};

static PurpleCoreUiOps core_uiops = 
{
	NULL,
	NULL,
	NULL,
	NULL,

	/* padding */
	NULL,
	NULL,
	NULL,
	NULL
};

//I have tried to detect Ctrl-C using ruby's trap method,
//but it does not work as expected: it can not detect Ctrl-C
//until a network event occurs
static void sighandler(int sig)
{
  switch (sig) {
  case SIGINT:
  case SIGQUIT:
  case SIGTERM:
		g_main_loop_quit(main_loop);
		break;
	}
}

static VALUE init(int argc, VALUE* argv, VALUE self)
{
  VALUE debug, path;
  const char *prefs_path = NULL;
  
  if( rb_cv_get( self, "@@prefs_path" ) != Qnil ) {
    prefs_path = RSTRING_PTR( rb_cv_get( self, "@@prefs_path" ) );
  }
  
  rb_scan_args(argc, argv, "02", &debug, &path);

  signal(SIGCHLD, SIG_IGN);
  signal(SIGPIPE, SIG_IGN);
  signal(SIGINT, sighandler);
  signal(SIGQUIT, sighandler);
  signal(SIGTERM, sighandler);

  data_hash_table = g_hash_table_new(NULL, NULL);
  fd_hash_table = g_hash_table_new(NULL, NULL);

  purple_debug_set_enabled((NIL_P(debug) || debug == Qfalse) ? FALSE : TRUE);

  if (!NIL_P(path)) {
    Check_Type(path, T_STRING);   
		purple_util_set_user_dir(RSTRING_PTR(path));
	}

  purple_core_set_ui_ops(&core_uiops);
  purple_eventloop_set_ui_ops(&glib_eventloops);
  
  if (!purple_core_init(UI_ID)) {
		rb_raise(rb_eRuntimeError, "libpurple initialization failed");
	}
  
  purple_util_set_user_dir( (const char *) prefs_path );
  
  /* Create and load the buddylist. */
  purple_set_blist(purple_blist_new());
  purple_blist_load();
  
  /* Load the preferences. */
  purple_prefs_load();
  purple_prefs_set_bool( "/purple/logging/log_ims", FALSE );
  purple_prefs_set_bool( "/purple/logging/log_chats", FALSE );

  /* Load the pounces. */
  purple_pounces_load();

  return Qnil;
}

static VALUE watch_blist_user_info(VALUE self)
{
  purple_notify_set_ui_ops(&notify_ops);
  set_callback(&user_info_handler, "user_info_handler");
  return user_info_handler;
}

static VALUE watch_incoming_im(VALUE self)
{
  purple_conversations_set_ui_ops(&conv_uiops);
  set_callback(&im_handler, "im_handler");
  return im_handler;
}

static VALUE watch_blist_change(VALUE self)
{
  purple_blist_set_ui_ops(&blist_uiops);
  set_callback(&blist_update_handler, "blist_update_handler");
  return blist_update_handler;
}

static VALUE watch_notify_message(VALUE self)
{
  purple_notify_set_ui_ops(&notify_ops);
  set_callback(&notify_message_handler, "notify_message_handler");
  return notify_message_handler;
}

static VALUE watch_request(VALUE self)
{
  purple_request_set_ui_ops(&request_ops);
  set_callback(&request_handler, "request_handler");
  return request_handler;
}

static VALUE watch_new_buddy(VALUE self)
{
  purple_accounts_set_ui_ops(&account_ops);
  set_callback(&new_buddy_handler, "new_buddy_handler");
  return new_buddy_handler;
}

static void signed_on(PurpleConnection* connection)
{
  VALUE args[1];
  args[0] = Data_Wrap_Struct(cAccount, NULL, NULL, purple_connection_get_account(connection));
  check_callback(signed_on_handler, "signed_on_handler");
  rb_funcall2(signed_on_handler, CALL, 1, args);
}

static void signed_off(PurpleConnection* connection)
{
  VALUE args[1];
  args[0] = Data_Wrap_Struct(cAccount, NULL, NULL, purple_connection_get_account(connection));
  check_callback(signed_off_handler, "signed_off_handler");
  rb_funcall2(signed_off_handler, CALL, 1, args);
}

static VALUE watch_signed_on_event(VALUE self)
{
  set_callback(&signed_on_handler, "signed_on_handler");
  int handle;
	purple_signal_connect(purple_connections_get_handle(), "signed-on", &handle,
				PURPLE_CALLBACK(signed_on), NULL);
  return signed_on_handler;
}

static  VALUE watch_signed_off_event(VALUE self)
{
  set_callback(&signed_off_handler, "signed_off_handler");
  int handle;
	purple_signal_connect(purple_connections_get_handle(), "signed-off", &handle,
				PURPLE_CALLBACK(signed_off), NULL);
  return signed_off_handler;
}

static VALUE watch_connection_error(VALUE self)
{
  finch_connections_init();
  purple_connections_set_ui_ops(&connection_ops);
  
  set_callback(&connection_error_handler, "connection_error_handler");
  
  /*int handle;
	purple_signal_connect(purple_connections_get_handle(), "connection-error", &handle,
				PURPLE_CALLBACK(connection_error), NULL);*/
  return connection_error_handler;
}

static void _read_socket_handler(gpointer notused, int socket, PurpleInputCondition condition)
{
  char message[4096] = {0};
  int i = recv(socket, message, sizeof(message) - 1, 0);
  if (i > 0) {
    purple_debug_info("purple_ruby", "recv %d: %d\n", socket, i);
    
    gpointer str = g_hash_table_lookup(data_hash_table, (gpointer)socket);
    if (NULL == str) rb_raise(rb_eRuntimeError, "can not find socket: %d", socket);
    rb_str_append((VALUE)str, rb_str_new2(message));
  } else {
    purple_debug_info("purple_ruby", "close connection %d: %d %d\n", socket, i, errno);
    
    gpointer str = g_hash_table_lookup(data_hash_table, (gpointer)socket);
    if (NULL == str) {
      purple_debug_warning("purple_ruby", "can not find socket in data_hash_table %d\n", socket);
      return;
    }
    
    gpointer purple_fd = g_hash_table_lookup(fd_hash_table, (gpointer)socket);
    if (NULL == purple_fd) {
      purple_debug_warning("purple_ruby", "can not find socket in fd_hash_table %d\n", socket);
      return;
    }
    
    g_hash_table_remove(fd_hash_table, (gpointer)socket);
    g_hash_table_remove(data_hash_table, (gpointer)socket);
    purple_input_remove((guint)purple_fd);
    close(socket);
    
    VALUE args[1];
    args[0] = (VALUE)str;
    check_callback(ipc_handler, "ipc_handler");
    rb_funcall2(ipc_handler, CALL, 1, args);
  }
}

static void _accept_socket_handler(gpointer notused, int server_socket, PurpleInputCondition condition)
{
  /* Check that it is a read condition */
	if (condition != PURPLE_INPUT_READ)
		return;
		
	struct sockaddr_in their_addr; /* connector's address information */
	socklen_t sin_size = sizeof(struct sockaddr);
	int client_socket;
  if ((client_socket = accept(server_socket, (struct sockaddr *)&their_addr, &sin_size)) == -1) {
    purple_debug_warning("purple_ruby", "failed to accept %d: %d\n", client_socket, errno);
		return;
	}
	
	int flags = fcntl(client_socket, F_GETFL);
	fcntl(client_socket, F_SETFL, flags | O_NONBLOCK);
#ifndef _WIN32
	fcntl(client_socket, F_SETFD, FD_CLOEXEC);
#endif

  purple_debug_info("purple_ruby", "new connection: %d\n", client_socket);
	
	guint purple_fd = purple_input_add(client_socket, PURPLE_INPUT_READ, _read_socket_handler, NULL);
	
	g_hash_table_insert(data_hash_table, (gpointer)client_socket, (gpointer)rb_str_new2(""));
	g_hash_table_insert(fd_hash_table, (gpointer)client_socket, (gpointer)purple_fd);
}

static VALUE watch_incoming_ipc(VALUE self, VALUE serverip, VALUE port)
{
	struct sockaddr_in my_addr;
	int soc;
	int on = 1;

	/* Open a listening socket for incoming conversations */
	if ((soc = socket(PF_INET, SOCK_STREAM, 0)) < 0)
	{
		rb_raise(rb_eRuntimeError, "Cannot open socket: %s\n", g_strerror(errno));
		return Qnil;
	}

	if (setsockopt(soc, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0)
	{
		rb_raise(rb_eRuntimeError, "SO_REUSEADDR failed: %s\n", g_strerror(errno));
		return Qnil;
	}

	memset(&my_addr, 0, sizeof(struct sockaddr_in));
	my_addr.sin_family = AF_INET;
	my_addr.sin_addr.s_addr = inet_addr(RSTRING_PTR(serverip));
	my_addr.sin_port = htons(FIX2INT(port));
	if (bind(soc, (struct sockaddr*)&my_addr, sizeof(struct sockaddr)) != 0)
	{
		rb_raise(rb_eRuntimeError, "Unable to bind to port %d: %s\n", (int)FIX2INT(port), g_strerror(errno));
		return Qnil;
	}

	/* Attempt to listen on the bound socket */
	if (listen(soc, 10) != 0)
	{
		rb_raise(rb_eRuntimeError, "Cannot listen on socket: %s\n", g_strerror(errno));
		return Qnil;
	}

  set_callback(&ipc_handler, "ipc_handler");
  
	/* Open a watcher in the socket we have just opened */
	purple_input_add(soc, PURPLE_INPUT_READ, _accept_socket_handler, NULL);
	
	return port;
}

static gboolean
do_timeout(gpointer data)
{
	VALUE handler = data;
	check_callback(handler, "timer_handler");
	VALUE v = rb_funcall(handler, CALL, 0, 0);
	return (v == Qtrue);
}

static VALUE watch_timer(VALUE self, VALUE delay)
{
	set_callback(&timer_handler, "timer_handler");
	if (timer_timeout != 0)
		g_source_remove(timer_timeout);
	timer_timeout = g_timeout_add_full( G_PRIORITY_HIGH, delay, do_timeout, timer_handler, NULL );
	return delay;
}

static VALUE login(VALUE self, VALUE protocol, VALUE username, VALUE password)
{
  PurpleAccount* account = purple_account_new(RSTRING_PTR(username), RSTRING_PTR(protocol));
  if (NULL == account || NULL == account->presence) {
    rb_raise(rb_eRuntimeError, "No able to create account: %s", RSTRING_PTR(protocol));
  }
  purple_account_set_password(account, RSTRING_PTR(password));
  purple_account_set_remember_password(account, TRUE);
  purple_account_set_enabled(account, UI_ID, TRUE);
  PurpleSavedStatus *status = purple_savedstatus_new(NULL, PURPLE_STATUS_AVAILABLE);
	purple_savedstatus_activate(status);
	purple_accounts_add(account);
	return Data_Wrap_Struct(cAccount, NULL, NULL, account);
}

static VALUE logout(VALUE self)
{
  PurpleAccount *account;
  Data_Get_Struct(self, PurpleAccount, account);
  purple_account_disconnect(account);

  return Qnil;
}

static void main_loop_run2()
{
  main_loop = g_main_loop_new(NULL, FALSE);
  g_main_loop_run(main_loop);
  purple_core_quit();
  
#ifdef DEBUG_MEM_LEAK
  printf("QUIT!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
  if (im_handler == Qnil) rb_gc_unregister_address(&im_handler);
  if (signed_on_handler == Qnil) rb_gc_unregister_address(&signed_on_handler);
  if (signed_off_handler == Qnil) rb_gc_unregister_address(&signed_off_handler);
  if (connection_error_handler == Qnil) rb_gc_unregister_address(&connection_error_handler);
  if (notify_message_handler == Qnil) rb_gc_unregister_address(&notify_message_handler);
  if (request_handler == Qnil) rb_gc_unregister_address(&request_handler);
  if (ipc_handler == Qnil) rb_gc_unregister_address(&ipc_handler);
  if (timer_timeout != 0) g_source_remove(timer_timeout);
  if (timer_handler == Qnil) rb_gc_unregister_address(&timer_handler);
  if (new_buddy_handler == Qnil) rb_gc_unregister_address(&new_buddy_handler);
  rb_gc_start();
#endif
}

static VALUE main_loop_run( VALUE self ) {
  main_loop_run2();
  return Qnil;
}

static VALUE main_loop_stop(VALUE self)
{
  g_main_loop_quit(main_loop);
  return Qnil;
}

static VALUE send_im(VALUE self, VALUE name, VALUE message)
{
  PurpleAccount *account;
  Data_Get_Struct(self, PurpleAccount, account);
  
  if (purple_account_is_connected(account)) {
    int i = serv_send_im(purple_account_get_connection(account), RSTRING_PTR(name), RSTRING_PTR(message), 0);
    return INT2FIX(i);
  } else {
    return Qnil;
  }
}

static VALUE common_send(VALUE self, VALUE name, VALUE message)
{
  PurpleAccount *account;
  Data_Get_Struct(self, PurpleAccount, account);
  
  if (purple_account_is_connected(account)) {
     PurpleBuddy* buddy = purple_find_buddy(account, RSTRING_PTR(name));
     if (buddy != NULL) {
       PurpleConversation *conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, RSTRING_PTR(name), account);
       if (conv == NULL) {
         conv = purple_conversation_new(PURPLE_CONV_TYPE_IM,
                                        buddy->account, buddy->name);
       }
       purple_conv_im_send(PURPLE_CONV_IM(conv), RSTRING_PTR(message));
       return INT2FIX(0);
     } else {
       return Qnil;
     }
  } else {
    return Qnil;    
  }
}

static VALUE username(VALUE self)
{
  PurpleAccount *account;
  Data_Get_Struct(self, PurpleAccount, account);
  return rb_str_new2(purple_account_get_username(account));
}

static VALUE display_name(VALUE self)
{
  PurpleAccount *account;
  Data_Get_Struct(self, PurpleAccount, account);
  return rb_str_new2(purple_account_get_name_for_display(account));
}


static VALUE protocol_id(VALUE self)
{
  PurpleAccount *account;
  Data_Get_Struct(self, PurpleAccount, account);
  return rb_str_new2(purple_account_get_protocol_id(account));
}

static VALUE protocol_name(VALUE self)
{
  PurpleAccount *account;
  Data_Get_Struct(self, PurpleAccount, account);
  return rb_str_new2(purple_account_get_protocol_name(account));
}

static VALUE get_bool_setting(VALUE self, VALUE name, VALUE default_value)
{
  PurpleAccount *account;
  Data_Get_Struct(self, PurpleAccount, account);
  gboolean value = purple_account_get_bool(account, RSTRING_PTR(name), 
    (default_value == Qfalse || default_value == Qnil) ? FALSE : TRUE); 
  return (TRUE == value) ? Qtrue : Qfalse;
}

static VALUE get_string_setting(VALUE self, VALUE name, VALUE default_value)
{
  PurpleAccount *account;
  Data_Get_Struct(self, PurpleAccount, account);
  const char* value = purple_account_get_string(account, RSTRING_PTR(name), RSTRING_PTR(default_value));
  return (NULL == value) ? Qnil : rb_str_new2(value);
}

static VALUE list_protocols(VALUE self)
{
  VALUE array = rb_ary_new();
  
  GList *iter = purple_plugins_get_protocols();
  int i;
	for (i = 0; iter; iter = iter->next) {
		PurplePlugin *plugin = iter->data;
		PurplePluginInfo *info = plugin->info;
		if (info && info->name) {
		  char s[256];
			snprintf(s, sizeof(s) -1, "%s %s", info->id, info->name);
			rb_ary_push(array, rb_str_new2(s));
		}
	}
  
  return array;
}

static VALUE add_buddy(VALUE self, VALUE buddy)
{
  PurpleAccount *account = NULL;
  PurpleConnection *gc = NULL;
  
  Data_Get_Struct(self, PurpleAccount, account);
  gc = purple_account_get_connection( account );
  
	PurpleBuddy* pb = purple_buddy_new(account, RSTRING_PTR(buddy), NULL);
  
  char* group = _("Buddies");
  PurpleGroup* grp = purple_find_group(group);
	if (!grp)
	{
		grp = purple_group_new(group);
		purple_blist_add_group(grp, NULL);
	}
  
  purple_blist_add_buddy(pb, NULL, grp, NULL);
  purple_account_add_buddy(account, pb);
  serv_add_permit( gc, RSTRING_PTR(buddy) );
  
  return Qtrue;
}

static VALUE remove_buddy(VALUE self, VALUE buddy)
{
  PurpleAccount *account = NULL;
  PurpleConnection *gc = NULL;
  
  Data_Get_Struct(self, PurpleAccount, account);
  gc = purple_account_get_connection( account );
  
	PurpleBuddy* pb = purple_find_buddy(account, RSTRING_PTR(buddy));
	if (NULL == pb) {
	  rb_raise(rb_eRuntimeError, "Failed to remove buddy for %s : %s does not exist", purple_account_get_username(account), RSTRING_PTR(buddy));
	}
	
	char* group = _("Buddies");
  PurpleGroup* grp = purple_find_group(group);
	if (!grp)
	{
		grp = purple_group_new(group);
		purple_blist_add_group(grp, NULL);
	}
	
	purple_blist_remove_buddy(pb);
	purple_account_remove_buddy(account, pb, grp);
  serv_rem_permit( gc, RSTRING_PTR( buddy ) );
	
  return Qtrue;
}

static VALUE has_buddy(VALUE self, VALUE buddy)
{
  PurpleAccount *account;
  Data_Get_Struct(self, PurpleAccount, account);
  if (purple_find_buddy(account, RSTRING_PTR(buddy)) != NULL) {
    return Qtrue;
  } else {
    return Qfalse;
  }
}

static VALUE set_public_alias(VALUE self, VALUE nickname)
{
  PurpleAccount *account = PURPLE_ACCOUNT(self);
  PurpleConnection *connection = NULL;
  PurplePlugin *prpl = NULL;
  const char *alias = NULL;
  void (*set_alias) (PurpleConnection *gc, const char *alias);
  
  alias = RSTRING_PTR( nickname );
  
  connection = purple_account_get_connection( account );
  
  if (!connection) {
    purple_account_set_public_alias( account, alias, NULL, NULL );
    return Qnil;
  }

  prpl = purple_connection_get_prpl( connection );
  if (!g_module_symbol( prpl->handle, "set_alias", (void *) &set_alias ) ) {
    // purple_account_set_public_alias( account, alias, NULL, NULL );
    return Qnil;
  }

  set_alias( connection, alias );
  
  return Qnil;
}

static VALUE acc_delete(VALUE self)
{
  PurpleAccount *account;
  Data_Get_Struct(self, PurpleAccount, account);
  purple_accounts_delete(account);
  return Qnil;
}

static VALUE set_avatar_from_file( VALUE self, VALUE filepath ) {
  FILE *file = NULL;
  size_t file_len = 0;
  unsigned char *file_data = NULL;
  char *filename = NULL;
  unsigned char *icon_data = NULL;
  PurpleAccount *account = PURPLE_ACCOUNT(self);
  
  filename = RSTRING_PTR( filepath );
  
  file = fopen( filename, "rb" );
  
  if( file != NULL ) {
    // get file size
    fseek( file, 0, SEEK_END );
    file_len = ftell( file );
    fseek( file, 0, SEEK_SET );
    // read data
    file_data = g_malloc( file_len );
    fread( file_data, file_len, 1, file );

    // close file
    fclose( file );
  }
  else {
    rb_raise( rb_eRuntimeError, "Error when opening picture: %s", filename );
  }

  // set filename
  // purple_account_set_buddy_icon_path( account, filename );
  
  // set account icon
  icon_data = g_malloc( file_len );
  memcpy( icon_data, file_data, file_len );
  purple_buddy_icons_set_account_icon( account, icon_data, file_len );
  // purple_account_set_bool( account, "use-global-buddyicon", 1 );
  
  return Qtrue;
}

static VALUE account_is_connected( VALUE self ) {
  PurpleAccount *account = PURPLE_ACCOUNT(self);
  
  if( purple_account_is_connected( account ) == TRUE ) {
    return Qtrue;
  }
  else {
    return Qfalse;
  }
}

static VALUE account_get_buddies_list( VALUE self ) {
  PurpleAccount *account = PURPLE_ACCOUNT(self);
  PurpleBuddy *buddy = NULL;
  GList *iter = NULL;
  VALUE buddies = rb_ary_new();
  for( iter = (GList *) purple_find_buddies( account, NULL ); iter; iter = iter->next ) {
    buddy = iter->data;
    if( buddy != NULL && buddy->name != NULL ) {
      rb_ary_push( buddies, RB_BLIST_BUDDY( buddy ) );
    }
  }
  
  return buddies;
}

static VALUE set_prefs_path( VALUE self, VALUE path ) {
  rb_cv_set( self, "@@prefs_path", path );
  return Qnil;
}

static VALUE get_prefs_path( VALUE self ) {
  return rb_cv_get( self, "@@prefs_path" );
}

static VALUE account_send_typing( VALUE self, VALUE buddy_name ) {
  PurpleAccount *account = PURPLE_ACCOUNT( self);
  PurpleConnection *gc = NULL;
  
  gc = purple_account_get_connection( account );
  
  serv_send_typing( gc, RSTRING_PTR(buddy_name), PURPLE_TYPING );
  
  return Qtrue;
}

static VALUE set_personal_message( VALUE self, VALUE psm ) {
  PurplePlugin *prpl = NULL;
  PurpleAccount *account = PURPLE_ACCOUNT(self);
  void (*set_psm) (PurpleConnection *gc, const char *psm);
  PurpleConnection *gc = NULL;
  
  gc = purple_account_get_connection( account );
  
  if (!gc) {
    return;
  }

  prpl = purple_connection_get_prpl( gc );
  if (!g_module_symbol (prpl->handle, "msn_set_personal_message_cb", (void *) &set_psm)) {
    return;
  }

  set_psm( gc, (const char *) RSTRING_PTR( psm ) );
  
  return Qnil;
}

// PurpleRuby::Buddy
static VALUE buddy_get_name( VALUE self ) {
  PurpleBuddy *buddy = NULL;
  
  PURPLE_BUDDY( self, buddy );
  
  return rb_str_new2( purple_buddy_get_name( buddy ) );
}

static VALUE buddy_get_status( VALUE self ) {
  PurpleBuddy *buddy = NULL;
  PurpleStatus *status = NULL;
  PurpleStatusType *type = NULL;
  
  PURPLE_BUDDY( self, buddy );
  status = purple_presence_get_active_status( purple_buddy_get_presence( buddy ) );
  type = purple_status_get_type( status );
  
  return INT2NUM( purple_status_type_get_primitive( type ) );
}

static VALUE buddy_get_alias( VALUE self ) {
  PurpleBuddy *buddy = NULL;
  
  PURPLE_BUDDY( self, buddy );
  
  return rb_str_new2( purple_buddy_get_alias( buddy ));
}

static gboolean call_rb_block_false( gpointer data ) {
  VALUE block = data;
  rb_funcall( block, CALL, 0, 0 );
  return FALSE;
}

static gboolean call_rb_block_true( gpointer data ) {
  VALUE block = data;
  rb_funcall( block, CALL, 0, 0 );
  return TRUE;
}


static VALUE defer_execute( VALUE self ) {
  VALUE *rb_block = NULL;
  
  if (!rb_block_given_p()) {
    rb_raise(rb_eArgError, "defer_execute: no block given");
  }
  
  rb_block = rb_block_proc();
  
  g_idle_add( call_rb_block_false, rb_block );
  
  return Qtrue;
}

static VALUE add_periodic_timer( VALUE self, VALUE seconds ) {
  VALUE *rb_block = NULL;
  int secs = 0;
  
  secs = NUM2LONG( seconds );
  
  if (!rb_block_given_p()) {
    rb_raise(rb_eArgError, "add_periodic_timer: no block given");
  }
  
  rb_block = rb_block_proc();
  
  g_timeout_add( secs * 1000, call_rb_block_true, rb_block );
  
  return Qtrue;
}

static VALUE add_timer( VALUE self, VALUE seconds ) {
  VALUE *rb_block = NULL;
  int secs = 0;
  
  secs = NUM2LONG( seconds );
  
  if (!rb_block_given_p()) {
    rb_raise(rb_eArgError, "add_timer: no block given");
  }
  
  rb_block = rb_block_proc();
  
  g_timeout_add( secs * 1000, call_rb_block_false, rb_block );
  
  return Qtrue;
}

static VALUE run_one_loop( VALUE self ) {
  
  g_main_context_iteration(NULL, 0);
  
  return Qnil;
}

static VALUE buddy_get_avatar (VALUE self){
	
	PurpleBuddy *buddy = NULL;
	PURPLE_BUDDY( self, buddy );
	PurpleBuddyIcon *icon =	purple_buddy_get_icon(buddy);
	if (icon != NULL) {
		size_t size = NULL;
		gconstpointer data = purple_buddy_icon_get_data(icon, &size);	
		return rb_str_new(data, size);
	} else {
		return Qnil;
	}
	
}

static VALUE buddy_get_info (VALUE self){
	
	PurpleBuddy *buddy = NULL;
	PURPLE_BUDDY( self, buddy );
	PurpleAccount *account = buddy->account;
	PurpleConnection *gc = purple_account_get_connection (account);
	serv_get_info(gc, purple_buddy_get_name( buddy ));
	return Qnil;
}

static VALUE buddy_get_avatar_type (VALUE self){
	
	PurpleBuddy *buddy = NULL;
	gconstpointer data;
	size_t size;
  
	PURPLE_BUDDY( self, buddy );
	PurpleBuddyIcon *icon =	purple_buddy_get_icon(buddy);
	if (icon != NULL) {		
		return rb_str_new2( purple_buddy_icon_get_extension(icon) );	
	} else {
		return Qnil;
	}
	
}

static VALUE buddy_get_account (VALUE self){
	
	PurpleBuddy *buddy = NULL;
	PURPLE_BUDDY( self, buddy );
	return RB_ACCOUNT(buddy->account);
	
}

void Init_purple_ruby() 
{
  CALL = rb_intern("call");
  
  cPurpleRuby = rb_define_class("PurpleRuby", rb_cObject);
  rb_define_singleton_method(cPurpleRuby, "init", init, -1);
  rb_define_singleton_method(cPurpleRuby, "list_protocols", list_protocols, 0);
  rb_define_singleton_method(cPurpleRuby, "watch_blist_user_info", watch_blist_user_info, 0);
  rb_define_singleton_method(cPurpleRuby, "watch_signed_on_event", watch_signed_on_event, 0);
  rb_define_singleton_method(cPurpleRuby, "watch_signed_off_event", watch_signed_off_event, 0);
  rb_define_singleton_method(cPurpleRuby, "watch_connection_error", watch_connection_error, 0);
  rb_define_singleton_method(cPurpleRuby, "watch_incoming_im", watch_incoming_im, 0);
  rb_define_singleton_method(cPurpleRuby, "watch_notify_message", watch_notify_message, 0);
  rb_define_singleton_method(cPurpleRuby, "watch_request", watch_request, 0);
  rb_define_singleton_method(cPurpleRuby, "watch_new_buddy", watch_new_buddy, 0);
  rb_define_singleton_method(cPurpleRuby, "watch_incoming_ipc", watch_incoming_ipc, 2);
  rb_define_singleton_method(cPurpleRuby, "watch_timer", watch_timer, 1);
  rb_define_singleton_method(cPurpleRuby, "watch_blist_change", watch_blist_change, 0);
  rb_define_singleton_method(cPurpleRuby, "login", login, 3);
  rb_define_singleton_method(cPurpleRuby, "main_loop_run", main_loop_run, 0);
  rb_define_singleton_method(cPurpleRuby, "main_loop_stop", main_loop_stop, 0);
  rb_define_singleton_method(cPurpleRuby, "prefs_path=", set_prefs_path, 1);
  rb_define_singleton_method(cPurpleRuby, "prefs_path", get_prefs_path, 0);
  rb_define_singleton_method(cPurpleRuby, "defer", defer_execute, 0);
  rb_define_singleton_method(cPurpleRuby, "add_periodic_timer", add_periodic_timer, 1);
  rb_define_singleton_method(cPurpleRuby, "add_timer", add_timer, 1);
  rb_define_singleton_method(cPurpleRuby, "run_one_loop", run_one_loop, 0);
  
  rb_define_const(cPurpleRuby, "NOTIFY_MSG_ERROR", INT2NUM(PURPLE_NOTIFY_MSG_ERROR));
  rb_define_const(cPurpleRuby, "NOTIFY_MSG_WARNING", INT2NUM(PURPLE_NOTIFY_MSG_WARNING));
  rb_define_const(cPurpleRuby, "NOTIFY_MSG_INFO", INT2NUM(PURPLE_NOTIFY_MSG_INFO));
  
  cConnectionError = rb_define_class_under(cPurpleRuby, "ConnectionError", rb_cObject);
  rb_define_const(cConnectionError, "NETWORK_ERROR", INT2NUM(PURPLE_CONNECTION_ERROR_NETWORK_ERROR));
  rb_define_const(cConnectionError, "INVALID_USERNAME", INT2NUM(PURPLE_CONNECTION_ERROR_INVALID_USERNAME));
  rb_define_const(cConnectionError, "AUTHENTICATION_FAILED", INT2NUM(PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED));
  rb_define_const(cConnectionError, "AUTHENTICATION_IMPOSSIBLE", INT2NUM(PURPLE_CONNECTION_ERROR_AUTHENTICATION_IMPOSSIBLE));
  rb_define_const(cConnectionError, "NO_SSL_SUPPORT", INT2NUM(PURPLE_CONNECTION_ERROR_NO_SSL_SUPPORT));
  rb_define_const(cConnectionError, "ENCRYPTION_ERROR", INT2NUM(PURPLE_CONNECTION_ERROR_ENCRYPTION_ERROR));
  rb_define_const(cConnectionError, "NAME_IN_USE", INT2NUM(PURPLE_CONNECTION_ERROR_NAME_IN_USE));
  rb_define_const(cConnectionError, "INVALID_SETTINGS", INT2NUM(PURPLE_CONNECTION_ERROR_INVALID_SETTINGS));
  rb_define_const(cConnectionError, "CERT_NOT_PROVIDED", INT2NUM(PURPLE_CONNECTION_ERROR_CERT_NOT_PROVIDED));
  rb_define_const(cConnectionError, "CERT_UNTRUSTED", INT2NUM(PURPLE_CONNECTION_ERROR_CERT_UNTRUSTED));
  rb_define_const(cConnectionError, "CERT_EXPIRED", INT2NUM(PURPLE_CONNECTION_ERROR_CERT_EXPIRED));
  rb_define_const(cConnectionError, "CERT_NOT_ACTIVATED", INT2NUM(PURPLE_CONNECTION_ERROR_CERT_NOT_ACTIVATED));
  rb_define_const(cConnectionError, "CERT_HOSTNAME_MISMATCH", INT2NUM(PURPLE_CONNECTION_ERROR_CERT_HOSTNAME_MISMATCH));  
  rb_define_const(cConnectionError, "CERT_FINGERPRINT_MISMATCH", INT2NUM(PURPLE_CONNECTION_ERROR_CERT_FINGERPRINT_MISMATCH));
  rb_define_const(cConnectionError, "CERT_SELF_SIGNED", INT2NUM(PURPLE_CONNECTION_ERROR_CERT_SELF_SIGNED));
  rb_define_const(cConnectionError, "CERT_OTHER_ERROR", INT2NUM(PURPLE_CONNECTION_ERROR_CERT_OTHER_ERROR));
  rb_define_const(cConnectionError, "OTHER_ERROR", INT2NUM(PURPLE_CONNECTION_ERROR_OTHER_ERROR));  
  
  cAccount = rb_define_class_under(cPurpleRuby, "Account", rb_cObject);
  rb_define_method(cAccount, "connected?", account_is_connected, 0);
  rb_define_method(cAccount, "buddies", account_get_buddies_list, 0);
  rb_define_method(cAccount, "send_im", send_im, 2);
  rb_define_method(cAccount, "send_typing", account_send_typing, 1);
  rb_define_method(cAccount, "common_send", common_send, 2);
  rb_define_method(cAccount, "username", username, 0);
  rb_define_method(cAccount, "alias=", set_public_alias, 1);
  rb_define_method(cAccount, "avatar=", set_avatar_from_file, 1);
  rb_define_method(cAccount, "psm=", set_personal_message, 1);
  rb_define_method(cAccount, "protocol_id", protocol_id, 0);
  rb_define_method(cAccount, "protocol_name", protocol_name, 0);
  rb_define_method(cAccount, "get_bool_setting", get_bool_setting, 2);
  rb_define_method(cAccount, "get_string_setting", get_string_setting, 2);
  rb_define_method(cAccount, "add_buddy", add_buddy, 1);
  rb_define_method(cAccount, "remove_buddy", remove_buddy, 1);
  rb_define_method(cAccount, "has_buddy?", has_buddy, 1);
  rb_define_method(cAccount, "delete", acc_delete, 0);
  rb_define_method(cAccount, "display_name", display_name, 0);
  rb_define_method(cAccount, "logout", logout, 0);
  
  cBuddy = rb_define_class_under(cPurpleRuby, "Buddy", rb_cObject);
  rb_define_method( cBuddy, "name", buddy_get_name, 0 );
  rb_define_method( cBuddy, "status", buddy_get_status, 0 );
  rb_define_method( cBuddy, "avatar", buddy_get_avatar, 0 );
  rb_define_method( cBuddy, "get_info", buddy_get_info, 0 );
  rb_define_method( cBuddy, "account", buddy_get_account, 0 );
  rb_define_method( cBuddy, "avatar_type", buddy_get_avatar_type, 0 );
  rb_define_method( cBuddy, "alias", buddy_get_alias, 0 );

  
  cStatus = rb_define_class_under( cPurpleRuby, "Status", rb_cObject );
  rb_define_const(cStatus, "STATUS_UNSET", INT2NUM(PURPLE_STATUS_UNSET));
  rb_define_const(cStatus, "STATUS_OFFLINE", INT2NUM(PURPLE_STATUS_OFFLINE));
  rb_define_const(cStatus, "STATUS_AVAILABLE", INT2NUM(PURPLE_STATUS_AVAILABLE));
  rb_define_const(cStatus, "STATUS_UNAVAILABLE", INT2NUM(PURPLE_STATUS_UNAVAILABLE));
  rb_define_const(cStatus, "STATUS_INVISIBLE", INT2NUM(PURPLE_STATUS_INVISIBLE));
  rb_define_const(cStatus, "STATUS_AWAY", INT2NUM(PURPLE_STATUS_AWAY));
  rb_define_const(cStatus, "STATUS_EXTENDED_AWAY", INT2NUM(PURPLE_STATUS_EXTENDED_AWAY));
  rb_define_const(cStatus, "STATUS_MOBILE", INT2NUM(PURPLE_STATUS_MOBILE));
}

