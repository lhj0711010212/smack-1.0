/*
 * This file is part of libsmack
 *
 * Copyright (C) 2010 Nokia Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 * Authors:
 * Jarkko Sakkinen <ext-jarkko.2.sakkinen@nokia.com>
 */

#include "smack.h"
#include <sys/types.h>
#include <attr/xattr.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <uthash.h>

#define SMACK_ACC_R 1
#define SMACK_ACC_W 2
#define SMACK_ACC_X 4
#define SMACK_ACC_A 16
#define SMACK_ACC_LEN 4

#define SMACK64 "security.SMACK64"
#define SMACK64_LEN 23

#define SMACK_PROC_PATH "/proc/%d/attr/current"
#define LINE_BUFFER_SIZE 255

struct smack_object {
	char object[SMACK64_LEN + 1];
	unsigned ac;
	UT_hash_handle hh;
};

struct smack_subject {
	char subject[SMACK64_LEN + 1];
	struct smack_object *objects;
	UT_hash_handle hh;
};

struct smack_rules {
	struct smack_subject *subjects;
};

struct smack_user {
	char *user;
	char label[SMACK64_LEN + 1];
	UT_hash_handle hh;
};

struct smack_users {
	struct smack_user *users;
};

static int update_rule(struct smack_subject **subjects,
		       const char *subject_str, const char *object_str,
		       unsigned ac);
static void destroy_rules(struct smack_subject **subjects);
static int update_user(struct smack_user **users,
		       const char *user, const char *label);
static void destroy_users(struct smack_user **users);
inline unsigned str_to_ac(const char *str);
inline void ac_to_str(unsigned ac, char *str, int format);

smack_rules_t smack_create_rules(void)
{
	struct smack_rules *result =
		calloc(1, sizeof(struct smack_rules));
	return result;
}

void smack_destroy_rules(smack_rules_t handle)
{
	destroy_rules(&handle->subjects);
	free(handle);
}

int smack_read_rules_from_file(smack_rules_t handle, const char *path,
			       const char *subject_filter)
{
	FILE *file;
	char *buf = NULL;
	const char *subject, *object, *access;
	unsigned ac;
	size_t size;
	struct smack_subject *subjects = NULL;
	int ret = 0;

	file = fopen(path, "r");
	if (file == NULL)
		return -1;

	while (ret == 0 && getline(&buf, &size, file) != -1) {
		subject = strtok(buf, " \n");
		object = strtok(NULL, " \n");
		access = strtok(NULL, " \n");

		if (subject == NULL || object == NULL || access == NULL ||
		    strtok(NULL, " \n") != NULL) {
			ret = -1;
		} else if (subject_filter == NULL ||
			 strcmp(subject, subject_filter) == 0) {
			ac = str_to_ac(access);
			ret = update_rule(&subjects, subject, object, ac);
		}

		free(buf);
		buf = NULL;
	}

	if (ferror(file))
		ret = -1;

	if (ret == 0) {
		destroy_rules(&handle->subjects);
		handle->subjects = subjects;
	} else {
		destroy_rules(&subjects);
	}

	free(buf);
	fclose(file);
	return ret;
}

int smack_write_rules_to_file(smack_rules_t handle, const char *path,
			      int format)
{
	struct smack_subject *s, *stmp;
	struct smack_object *o, *otmp;
	FILE *file;
	char str[6];
	int err;

	file = fopen(path, "w+");
	if (!file)
		return -1;

	HASH_ITER(hh, handle->subjects, s, stmp) {
		HASH_ITER(hh, s->objects, o, otmp) {
			if (format == SMACK_FORMAT_CONFIG) {
				ac_to_str(o->ac, str, SMACK_FORMAT_CONFIG);
				err = fprintf(file, "%s %s %s\n",
					      s->subject, o->object, str);
			} else if (format == SMACK_FORMAT_KERNEL) {
				ac_to_str(o->ac, str, SMACK_FORMAT_KERNEL);
				err = fprintf(file, "%-23s %-23s %4s\n",
					      s->subject, o->object, str);
			}

			if (err < 0) {
				fclose(file);
				return errno;
			}
		}
	}

	fclose(file);
	return 0;
}

int smack_add_rule(smack_rules_t handle, const char *subject, 
		   const char *object, const char *access_str)
{
	unsigned access;
	int ret;
	access = str_to_ac(access_str);
	ret = update_rule(&handle->subjects, subject, object, access);
	return ret == 0 ? 0  : -1;
}

void smack_remove_rule(smack_rules_t handle, const char *subject,
		       const char *object)
{
	struct smack_subject *s = NULL;
	struct smack_object *o = NULL;

	HASH_FIND_STR(handle->subjects, subject, s);
	if (s == NULL)
		return;

	HASH_FIND_STR(s->objects, object, o);
	if (o == NULL)
		return;

	HASH_DEL(s->objects, o);
	free(o);
}

void smack_remove_rules_by_subject(smack_rules_t handle, const char *subject)
{
	struct smack_subject *s = NULL;
	struct smack_object *o = NULL, *tmp = NULL;

	HASH_FIND_STR(handle->subjects, subject, s);
	if (s == NULL)
		return;

	HASH_ITER(hh, s->objects, o, tmp) {
		HASH_DEL(s->objects, o);
		free(o);
	}
}

void smack_remove_rules_by_object(smack_rules_t handle, const char *object)
{
	struct smack_subject *s = NULL, *tmp = NULL;
	struct smack_object *o = NULL;

	HASH_ITER(hh, handle->subjects, s, tmp) {
		HASH_FIND_STR(s->objects, object, o);
		HASH_DEL(s->objects, o);
		free(o);
	}
}

int smack_have_access_rule(smack_rules_t handle, const char *subject,
			   const char *object, const char *access_str)
{
	struct smack_subject *s = NULL;
	struct smack_object *o = NULL;
	unsigned ac;

	ac = str_to_ac(access_str);

	HASH_FIND_STR(handle->subjects, subject, s);
	if (s == NULL)
		return 0;

	HASH_FIND_STR(s->objects, object, o);
	if (o == NULL)
		return 0;

	return ((o->ac & ac) == ac);
}

smack_users_t smack_create_users()
{

	struct smack_users *result =
		calloc(1, sizeof(struct smack_users));
	return result;
}

void smack_destroy_users(smack_users_t handle)
{
	destroy_users(&handle->users);
	free(handle);
}

int smack_read_users_from_file(smack_users_t handle, const char *path)
{
	FILE *file;
	char *buf = NULL;
	size_t size;
	const char *user, *label;
	struct smack_user *users = NULL;
	int ret = 0;

	file = fopen(path, "r");
	if (file == NULL)
		return -1;

	while (ret == 0 && getline(&buf, &size, file) != -1) {
		user = strtok(buf, " \n");
		label = strtok(NULL, " \n");

		if (user == NULL || label == NULL ||
		    strtok(NULL, " \n") != NULL)
			ret = -1;
		else
			ret = update_user(&users, user, label);

		free(buf);
		buf = NULL;
	}

	if (ferror(file))
		ret = -1;

	if (ret == 0) {
		destroy_users(&handle->users);
		handle->users = users;
	} else {
		destroy_users(&users);
	}

	free(buf);
	fclose(file);
	return 0;
}

int smack_write_users_to_file(smack_users_t handle, const char *path)
{
	struct smack_user *u, *tmp;
	FILE *file;
	char str[6];
	int err;

	file = fopen(path, "w+");
	if (!file)
		return -1;

	HASH_ITER(hh, handle->users, u, tmp) {
		err = fprintf(file, "%s %s\n",
			      u->user, u->label);
		if (err < 0) {
			fclose(file);
			return errno;
		}
	}

	fclose(file);
	return 0;
}

int smack_set_smack_to_file(const char *path, const char *smack, int flags)
{
	size_t size;
	int ret;

	size = strlen(smack);
	if (size > SMACK64_LEN)
		return -1;

	if ((flags & SMACK_XATTR_SYMLINK) == 0)
		ret = setxattr(path, SMACK64, smack, size, 0);
	else
		ret = lsetxattr(path, SMACK64, smack, size, 0);

	return ret;
}

int smack_get_smack_from_file(const char *path, char **smack, int flags)
{
	ssize_t ret;
	char *buf;

	if ((flags & SMACK_XATTR_SYMLINK) == 0)
		ret = getxattr(path, SMACK64, NULL, 0);
	else
		ret = lgetxattr(path, SMACK64, NULL, 0);

	if (ret < 0)
		return -1;

	buf = malloc(ret + 1);

	if ((flags & SMACK_XATTR_SYMLINK) == 0)
		ret = getxattr(path, SMACK64, buf, ret);
	else
		ret = lgetxattr(path, SMACK64, buf, ret);

	if (ret < 0) {
		free(buf);
		return -1;
	}

	buf[ret] = '\0';
	*smack = buf;
	return 0;
}

int smack_get_smack_from_proc(int pid, char **smack)
{
	char buf[LINE_BUFFER_SIZE];
	FILE *file;

	snprintf(buf, LINE_BUFFER_SIZE, SMACK_PROC_PATH, pid);

	file = fopen(buf, "r");
	if (file == NULL)
		return -1;

	if (fgets(buf, LINE_BUFFER_SIZE, file) == NULL) {
		fclose(file);
		return -1;
	}

	fclose(file);
	*smack = strdup(buf);
	return *smack != NULL ? 0 : - 1;
}


static int update_rule(struct smack_subject **subjects,
		       const char *subject_str,
		       const char *object_str, unsigned ac)
{
	struct smack_subject *s = NULL;
	struct smack_object *o = NULL;

	if (strlen(subject_str) > SMACK64_LEN &&
	    strlen(object_str) > SMACK64_LEN)
		return -ERANGE;

	HASH_FIND_STR(*subjects, subject_str, s);
	if (s == NULL) {
		s = calloc(1, sizeof(struct smack_subject));
		strcpy(s->subject, subject_str);
		HASH_ADD_STR(*subjects, subject, s);
	}

	HASH_FIND_STR(s->objects, object_str, o);
	if (o == NULL) {
		o = calloc(1, sizeof(struct smack_object));
		strcpy(o->object, object_str);
		HASH_ADD_STR(s->objects, object, o);
	}

	o->ac = ac;
	return 0;
}

static void destroy_rules(struct smack_subject **subjects)
{
	struct smack_subject *s;
	struct smack_object *o;

	while (*subjects != NULL) {
		s = *subjects;
		while (s->objects != NULL) {
			o = s->objects;
			HASH_DEL(s->objects, o);
			free(o);
		}
		HASH_DEL(*subjects, s);
		free(s);
	}
}

static int update_user(struct smack_user **users,
		       const char *user, const char *label)
{
	struct smack_user *u = NULL;

	if (strlen(label) > SMACK64_LEN)
		return -ERANGE;

	HASH_FIND_STR(*users, user, u);
	if (u == NULL) {
		u = calloc(1, sizeof(struct smack_subject));
		u->user = strdup(user);
		HASH_ADD_STR(*users, user, u);
	}

	strcpy(u->label, label);
	return 0;
}

static void destroy_users(struct smack_user **users)
{
	struct smack_user *u, *tmp;

	HASH_ITER(hh, *users, u, tmp) {
		HASH_DEL(*users, u);
		free(u->user);
		free(u);
	}
}

inline unsigned str_to_ac(const char *str)
{
	int i, count;
	unsigned access;

	access = 0;

	count = strlen(str);
	for (i = 0; i < count; i++)
		switch (str[i]) {
		case 'r':
		case 'R':
			access |= SMACK_ACC_R;
			break;
		case 'w':
		case 'W':
			access |= SMACK_ACC_W;
			break;
		case 'x':
		case 'X':
			access |= SMACK_ACC_X;
			break;
		case 'a':
		case 'A':
			access |= SMACK_ACC_A;
			break;
		default:
			break;
		}

	return access;
}

inline void ac_to_str(unsigned access, char *str, int format)
{
	int i;
	if (format == SMACK_FORMAT_KERNEL) {
		str[0] = ((access & SMACK_ACC_R) != 0) ? 'r' : '-';
		str[1] = ((access & SMACK_ACC_W) != 0) ? 'w' : '-';
		str[2] = ((access & SMACK_ACC_X) != 0) ? 'x' : '-';
		str[3] = ((access & SMACK_ACC_A) != 0) ? 'a' : '-';
		str[4] = '\0';
	} else if (format == SMACK_FORMAT_CONFIG) {
		i = 0;
		if ((access & SMACK_ACC_R) != 0)
			str[i++] = 'r';
		if ((access & SMACK_ACC_W) != 0)
			str[i++] = 'w';
		if ((access & SMACK_ACC_X) != 0)
			str[i++] = 'x';
		if ((access & SMACK_ACC_A) != 0)
			str[i++] = 'a';
		str[i] = '\0';
	}
}

