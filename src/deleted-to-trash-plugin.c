/* Copyright (C) 2008 steven.xu@lba.ca, 2010 Lex Brugman <lex dot brugman at gmail dot com> */
#include "deleted-to-trash-plugin.h"
#include "array.h"
#include "str.h"
#include "str-sanitize.h"

#include <stdlib.h>


struct mail_namespace *
get_users_inbox_namespace(struct mail_user *user)
{
	struct mail_namespace *ns = NULL;
	struct mail_namespace *curns = NULL;

	/* find the inbox namespace */
	for(curns = user->namespaces; curns != NULL; curns = curns->next)
	{
		if(curns->flags & NAMESPACE_FLAG_INBOX)
		{
			ns = curns;
			break;
		}
		i_free(curns);
	}

	return ns;
}

static int
search_deleted_id(struct mail *_mail)
{
	unsigned int deleted_id = _mail->uid;
	int ret = 0;

	/**
	 * only if the copy and delete box are the same, otherwise, we need to reset the last copy data
	 * since copy always follows a delete in IMAP (when moving an email from one folder to another folder).
	 */
	if(last_copy.src_mailbox_name != NULL && strcmp(_mail->box->name, last_copy.src_mailbox_name) == 0)
	{
		unsigned int count = 0;
		unsigned int i = 0;
		unsigned int const *mail_ids = NULL;

		mail_ids = array_get(&last_copy.mail_id, &count);
		for(i = 0; i < count; i++)
		{
			if(mail_ids[i] == deleted_id)
			{
				ret = 1;
				break;
			}
		}
	}
	else
	{
		if(array_count(&last_copy.mail_id) > 0)
		{
			array_free(&last_copy.mail_id);
			i_array_init(&last_copy.mail_id, TRASH_LIST_INITSIZE);
		}
		if(last_copy.src_mailbox_name != NULL)
		{
			i_free(last_copy.src_mailbox_name);
			last_copy.src_mailbox_name = NULL;
		}
	}

	return ret;
}

static struct mailbox *
mailbox_open_or_create(struct mail_storage *storage, const char *name)
{
	struct mailbox *box;
	enum mail_error error;

	box = mailbox_open(&storage, name, NULL, MAILBOX_OPEN_FAST | MAILBOX_OPEN_KEEP_RECENT | MAILBOX_OPEN_NO_INDEX_FILES);
	if(box == NULL)
	{
		(void)mail_storage_get_last_error(storage, &error);
		if(error == MAIL_ERROR_NOTFOUND)
		{
			/* try creating it */
			if(mail_storage_mailbox_create(storage, name, FALSE) >= 0)
			{
				/* and try opening again */
				box = mailbox_open(&storage, name, NULL, MAILBOX_OPEN_FAST | MAILBOX_OPEN_KEEP_RECENT);
			}
		}
	}

	return box;
}

static int
copy_deleted_mail_to_trash(struct mail *_mail)
{
	int ret;
	struct mailbox *trash_box = NULL;
	struct mail_namespace *ns = NULL;

	ns = get_users_inbox_namespace(_mail->box->storage->ns->user);
	trash_box = mailbox_open_or_create(ns->storage, trashfolder_name);

	if(trash_box != NULL)
	{
		i_info("opening %s succeeded", trashfolder_name);
		struct mailbox_transaction_context *dest_trans;
		struct mail_save_context *save_ctx;
		struct mail_keywords *keywords;
		const char *const *keywords_list;

		dest_trans = mailbox_transaction_begin(trash_box, MAILBOX_TRANSACTION_FLAG_EXTERNAL);

		keywords_list = mail_get_keywords(_mail);
		keywords = str_array_length(keywords_list) == 0 ? NULL : mailbox_keywords_create_valid(trash_box, keywords_list);
		/* dovecot won't set the delete flag at this moment */
		save_ctx = mailbox_save_alloc(dest_trans);
		/* remove the deleted flag for the new copy */
		enum mail_flags flags = mail_get_flags(_mail);
		flags &= ~MAIL_DELETED;
		/* copy the message to the Trash folder */
		mailbox_save_set_flags(save_ctx, flags, keywords);
		ret = mailbox_copy(&save_ctx, _mail);
		mailbox_keywords_free(trash_box, &keywords);

		if(ret < 0)
		{
			mailbox_transaction_rollback(&dest_trans);
			i_info("copying to %s failed", trashfolder_name);
		}
		else
		{
			ret = mailbox_transaction_commit(&dest_trans);
			i_info("copying to %s succeeded", trashfolder_name);
		}

		mailbox_close(&trash_box);
		i_free(trash_box);
	}
	else
	{
		i_info("opening %s failed", trashfolder_name);
		ret = -1;
	}

	return ret;
}

static void
deleted_to_trash_mail_update_flags(struct mail *_mail, enum modify_type modify_type, enum mail_flags flags)
{
	struct mail_private *mail = (struct mail_private *)_mail;
	union mail_module_context *lmail = DELETED_TO_TRASH_MAIL_CONTEXT(mail);
	enum mail_flags old_flags, new_flags;

	old_flags = mail_get_flags(_mail);
	lmail->super.update_flags(_mail, modify_type, flags);

	new_flags = old_flags;
	switch(modify_type) {
		case MODIFY_ADD:
			new_flags |= flags;
			break;
		case MODIFY_REMOVE:
			new_flags &= ~flags;
			break;
		case MODIFY_REPLACE:
			new_flags = flags;
			break;
	}

	if(((old_flags ^ new_flags) & MAIL_DELETED) != 0)
	{
		struct mail_namespace *ns = NULL;
		ns = get_users_inbox_namespace(_mail->box->storage->ns->user);

		/* marked as deleted and not deleted from the Trash folder */
		if(new_flags & MAIL_DELETED && !(strcmp(_mail->box->name, trashfolder_name) == 0 && strcmp(_mail->box->storage->ns->prefix, ns->prefix) == 0))
		{
			if(search_deleted_id(_mail))
			{
				i_info("email uid=%d was already copied to %s", _mail->uid, trashfolder_name);
			}
			else
			{
				if(copy_deleted_mail_to_trash(_mail) < 0)
				{
					i_fatal("failed to copy %d to %s", _mail->uid, trashfolder_name);
				}
			}
		}
	}
}

static struct mail *
deleted_to_trash_mail_alloc(struct mailbox_transaction_context *t, enum mail_fetch_field wanted_fields, struct mailbox_header_lookup_ctx *wanted_headers)
{
	union mailbox_module_context *lbox = DELETED_TO_TRASH_CONTEXT(t->box);
	union mail_module_context *lmail;
	struct mail *_mail;
	struct mail_private *mail;

	_mail = lbox->super.mail_alloc(t, wanted_fields, wanted_headers);
	mail = (struct mail_private *)_mail;

	lmail = p_new(mail->pool, union mail_module_context, 1);
	lmail->super = mail->v;

	mail->v.update_flags = deleted_to_trash_mail_update_flags;
	MODULE_CONTEXT_SET_SELF(mail, deleted_to_trash_mail_module, lmail);

	return _mail;
}

static int
deleted_to_trash_copy(struct mail_save_context *save_ctx, struct mail *mail)
{
	int ret = 0;

	/* one IMAP copy command can contain many mails ID's, so, this function will be called multiple times */
	union mailbox_module_context *lbox = DELETED_TO_TRASH_CONTEXT(save_ctx->transaction->box);
	ret = lbox->super.copy(save_ctx, mail);
	if(ret >= 0)
	{
		/* if the copy transaction context changes, a new copy could have been started, so add additional conditions to check the folder name */
		i_info("from %s to %s, previous action from %s", mail->box->name, save_ctx->transaction->box->name, last_copy.src_mailbox_name);
		if(last_copy.transaction_context == save_ctx->transaction && last_copy.src_mailbox_name != NULL && strcmp(last_copy.src_mailbox_name, mail->box->name) == 0)
		{
			array_append(&last_copy.mail_id, &mail->uid, 1);
			i_info("nr %i", array_count(&last_copy.mail_id));
		}
		else
		{ /* if copying from Trash to some other folder, we don't mark it */
			last_copy.transaction_context = save_ctx->transaction;
			if(array_count(&last_copy.mail_id) > 0)
			{
				array_free(&last_copy.mail_id);
				i_array_init(&last_copy.mail_id, TRASH_LIST_INITSIZE);
			}

			if(strcmp(mail->box->name, trashfolder_name) != 0 )
			{
				array_append(&last_copy.mail_id, &mail->uid, 1);
				last_copy.src_mailbox_name = i_strdup(mail->box->name);
			}
			else
			{
				i_info("from %s!", trashfolder_name);
			}
		}
	}

	return ret;
}

static int
deleted_to_trash_transaction_commit(struct mailbox_transaction_context *t, uint32_t *uid_validity_r, uint32_t *first_saved_uid_r, uint32_t *last_saved_uid_r)
{
	union mailbox_module_context *lbox = DELETED_TO_TRASH_CONTEXT(t->box);
	if(last_copy.transaction_context == t)
	{
		last_copy.transaction_context = NULL;
	}
	int ret = lbox->super.transaction_commit(t, uid_validity_r, first_saved_uid_r, last_saved_uid_r);

	return ret;
}

static void
deleted_to_trash_transaction_rollback(struct mailbox_transaction_context *t)
{
	union mailbox_module_context *lbox = DELETED_TO_TRASH_CONTEXT(t->box);
	if(last_copy.transaction_context == t)
	{
		last_copy.transaction_context = NULL;
	}
	lbox->super.transaction_rollback(t);
}

static struct mailbox *
deleted_to_trash_mailbox_open(struct mail_storage *storage, const char *name, struct istream *input, enum mailbox_open_flags flags)
{
	union mail_storage_module_context *lstorage = DELETED_TO_TRASH_CONTEXT(storage);
	struct mailbox *box;
	union mailbox_module_context *lbox;

	box = lstorage->super.mailbox_open(storage, name, input, flags);
	if(box != NULL)
	{
		lbox = p_new(box->pool, union mailbox_module_context, 1);
		lbox->super = box->v;

		box->v.mail_alloc = deleted_to_trash_mail_alloc;
		box->v.copy = deleted_to_trash_copy;
		box->v.transaction_commit = deleted_to_trash_transaction_commit;
		box->v.transaction_rollback = deleted_to_trash_transaction_rollback;
		MODULE_CONTEXT_SET_SELF(box, deleted_to_trash_storage_module, lbox);
	}

	return box;
}

static int
deleted_to_trash_mailbox_list_delete(struct mailbox_list *list, const char *name)
{
	int ret = 0;
	union mailbox_list_module_context *llist = DELETED_TO_TRASH_LIST_CONTEXT(list);

	if(llist->super.delete_mailbox(list, name) < 0)
	{
		ret = -1;
	}

	return ret;
}

static void
deleted_to_trash_mail_storage_created(struct mail_storage *storage)
{
	union mail_storage_module_context *lstorage;

	lstorage = p_new(storage->pool, union mail_storage_module_context, 1);
	lstorage->super = storage->v;
	storage->v.mailbox_open = deleted_to_trash_mailbox_open;

	MODULE_CONTEXT_SET_SELF(storage, deleted_to_trash_storage_module, lstorage);

	if(deleted_to_trash_next_hook_mail_storage_created != NULL)
	{
		deleted_to_trash_next_hook_mail_storage_created(storage);
	}
}

static void
deleted_to_trash_mailbox_list_created(struct mailbox_list *list)
{
	union mailbox_list_module_context *llist;

	llist = p_new(list->pool, union mailbox_list_module_context, 1);
	llist->super = list->v;
	list->v.delete_mailbox = deleted_to_trash_mailbox_list_delete;

	MODULE_CONTEXT_SET_SELF(list, deleted_to_trash_mailbox_list_module, llist);

	if(deleted_to_trash_next_hook_mailbox_list_created != NULL)
	{
		deleted_to_trash_next_hook_mailbox_list_created(list);
	}
}

void
deleted_to_trash_plugin_init(void)
{
	trashfolder_name = getenv("DELETED_TO_TRASH_FOLDER");
	if(trashfolder_name == NULL)
	{
		trashfolder_name = DEFAULT_TRASH_FOLDER;
	}

	deleted_to_trash_next_hook_mail_storage_created = hook_mail_storage_created;
	hook_mail_storage_created = deleted_to_trash_mail_storage_created;

	deleted_to_trash_next_hook_mailbox_list_created = hook_mailbox_list_created;
	hook_mailbox_list_created = deleted_to_trash_mailbox_list_created;

	last_copy.transaction_context = NULL;
	i_array_init(&last_copy.mail_id, TRASH_LIST_INITSIZE);
	last_copy.src_mailbox_name = NULL;
}

void
deleted_to_trash_plugin_deinit(void)
{
	hook_mail_storage_created = deleted_to_trash_next_hook_mail_storage_created;
	hook_mailbox_list_created = deleted_to_trash_next_hook_mailbox_list_created;

	if(last_copy.src_mailbox_name != NULL)
	{
		i_free(last_copy.src_mailbox_name);
	}
	array_free(&last_copy.mail_id);
}
