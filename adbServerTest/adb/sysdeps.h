/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* this file contains system-dependent definitions used by ADB
 * they're related to threads, sockets and file descriptors
 */
#ifndef _ADB_SYSDEPS_H
#define _ADB_SYSDEPS_H

#include <win32_adb.h>
#include <windows.h>
#include <process.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>

#include <direct.h>


typedef struct { unsigned  tid; }  adb_thread_t;
typedef  void*  (*adb_thread_func_t)(void*  arg);
typedef  void(*win_thread_func_t)(void*  arg);
static inline int  adb_thread_create(adb_thread_t  *thread, adb_thread_func_t  func, void*  arg)
{
	thread->tid = _beginthread((win_thread_func_t)func, 0, arg);
	if (thread->tid == (unsigned)-1L) {
		return -1;
	}
	return 0;
}

inline int  adb_mkdir(const char*  path, int mode)
{
	return _mkdir(path);
}
#undef   mkdir
#define  mkdir  ___xxx_mkdir

static __inline__  int    adb_unlink(const char*  path)
{
	int  rc = unlink(path);

	if (rc == -1 && errno == EACCES) {
		/* unlink returns EACCES when the file is read-only, so we first */
		/* try to make it writable, then unlink again...                  */
		rc = chmod(path, _S_IREAD | _S_IWRITE);
		if (rc == 0)
			rc = unlink(path);
	}
	return rc;
}
#undef  unlink
#define unlink  ___xxx_unlink


#define OS_PATH_SEPARATOR '\\'
#define OS_PATH_SEPARATOR_STR "\\"
#define ENV_PATH_SEPARATOR_STR ";"

#define  lstat    stat   /* no symlinks on Win32 */

#define  S_ISLNK(m)   0   /* no symlinks on Win32 */

static __inline__  char*  adb_dirstart(const char*  path)
{
	char*  p = strchr((char*)path, '/');
	char*  p2 = strchr((char*)path, '\\');

	if (!p)
		p = p2;
	else if (p2 && p2 > p)
		p = p2;

	return p;
}

static __inline__  char*  adb_dirstop(const char*  path)
{
	char*  p = strrchr((char*)path, '/');
	char*  p2 = strrchr((char*)path, '\\');

	if (!p)
		p = p2;
	else if (p2 && p2 > p)
		p = p2;

	return p;
}



#endif /* _ADB_SYSDEPS_H */
