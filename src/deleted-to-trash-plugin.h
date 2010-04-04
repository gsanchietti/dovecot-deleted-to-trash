#ifndef __DELETED_TO_TRASH_PLUGIN_H
#define __DELETED_TO_TRASH_PLUGIN_H

#include "lib.h"
#include "mail-storage-private.h"
#include "mail-namespace.h"
#include "mailbox-list-private.h"

#define DEFAULT_TRASH_FOLDER "Trash"
#define TRASH_LIST_INITSIZE 128

#define DELETED_TO_TRASH_CONTEXT(obj) MODULE_CONTEXT(obj, deleted_to_trash_storage_module)
#define DELETED_TO_TRASH_MAIL_CONTEXT(obj) MODULE_CONTEXT(obj, deleted_to_trash_mail_module)
#define DELETED_TO_TRASH_LIST_CONTEXT(obj) MODULE_CONTEXT(obj, deleted_to_trash_mailbox_list_module)

struct last_copy_info
{
	void *transaction_context;
	ARRAY_DEFINE(mail_id, unsigned int);
	char *src_mailbox_name;
};

/* defined by imap, pop3, lda */
const char *deleted_to_trash_plugin_version = PACKAGE_VERSION;

static void (*deleted_to_trash_next_hook_mail_storage_created) (struct mail_storage *storage);
static void (*deleted_to_trash_next_hook_mailbox_list_created) (struct mailbox_list *list);

static MODULE_CONTEXT_DEFINE_INIT(deleted_to_trash_storage_module, &mail_storage_module_register);
static MODULE_CONTEXT_DEFINE_INIT(deleted_to_trash_mail_module, &mail_module_register);
static MODULE_CONTEXT_DEFINE_INIT(deleted_to_trash_mailbox_list_module, &mailbox_list_module_register);

static struct last_copy_info last_copy;

static char *trashfolder_name = NULL;

void deleted_to_trash_plugin_init(void);
void deleted_to_trash_plugin_deinit(void);

#endif
