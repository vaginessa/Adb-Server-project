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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
//#include <sys/time.h>
#include <time.h>
#include <direct.h>
#include <limits.h>
#include <sys/types.h>
#include <zipfile/zipfile.h>

#include <iostream>
#include <fstream>

#include "Catransport.h"
#include "file_sync_service.h"

#define S_RDONLY       std::ios::in | std::ios::binary					/* open for reading only */
#define S_WRONLY       std::ios::out | std::ios::binary					/* open for writing only */
#define S_WNRONLY       std::ios::out | std::ios::binary | std::ios::trunc	/* open for writing only and remove the old file*/
#define S_RDWR         std::ios::in | std::ios::out | std::ios::binary  /* open for reading and writing */

typedef std::fstream	adbFile;

static int mkdirs(char *name)
{
	int ret;
	char *x = name + 1;

	for (;;) {
		x = adb_dirstart(x);
		if (x == 0) return 0;
		*x = 0;
		ret = adb_mkdir(name, 0775);
		*x = OS_PATH_SEPARATOR;
		if ((ret < 0) && (errno != EEXIST)) {
			return ret;
		}
		x++;
	}
	return 0;
}


adbFile* adb_file_open(const char*  path, int  options){
	DWORD  desiredAccess = 0;
	//DWORD  shareMode = FILE_SHARE_READ | FILE_SHARE_WRITE;

	switch (options) {
	case S_RDONLY:
	case S_WRONLY:
	case S_WNRONLY:
	case S_RDWR:
		desiredAccess = options;
		break;
	default:
		D("adb_open: invalid options (0x%0x)\n", options);
		errno = EINVAL;
		return NULL;
	}

	adbFile* f = new adbFile(path, desiredAccess);
	if (f){
		if (f->is_open()){
			return f;
		}
		f->close();
		delete f;
	}
	return NULL;
}

int adb_file_lseek(adbFile* f, int s_pos, int e_pos){
	int _ssize = 0;
	if (f){
		f->seekg(s_pos, e_pos);
		_ssize = f->tellg();
	}
	return _ssize;
}

void adb_file_close(adbFile* &f){
	if (f){
		f->close();
		delete f;
		f = NULL;
	}
}

int adb_file_read(adbFile*  f, void* buf, int len){
	if (f){
		return f->read((char*)buf, len).gcount();
	}
	return 0;
}

adbFile* adb_file_creat(const char*  path, int  options){
	bool _is_open = false;
	char* __name = strdup(path);
	mkdirs(__name);	//先确保路径没问题
	free(__name);
	{//打开一次 就可以创建了
		adbFile read(path, S_RDONLY);
		_is_open = read.is_open();
	}
	if (_is_open){
		return adb_file_open(path, options);
	}
	return NULL;
}

void Catransport::sync_quit(int local_id)
{
	aclientPtr a = client_get_ptr(local_id);
	if (a){
		if (a->biptype != bipbuffer_file_sync){
			return;
		}
		syncmsg msg;
		msg.req.id = ID_QUIT;
		msg.req.namelen = 0;
		sync_write(&msg.req, sizeof(msg.req), local_id);

		while (a->biptype != bipbuffer_close){//等待通道关闭
			if (kicked){ t_error = A_ERROR_OFFLINE; return; }
			adb_sleep_ms(10);
		}
	}
}

typedef void (*sync_ls_cb)(unsigned mode, unsigned size, unsigned time, const char *name, void *cookie);

int Catransport::sync_ls(const char *path, void *cookie, int local_id)
{
    syncmsg msg;
    char buf[257];
    int len;

    len = strlen(path);
    if(len > 1024) goto fail;

    msg.req.id = ID_LIST;
    msg.req.namelen = htoll(len);

	if (sync_write(&msg.req, sizeof(msg.req), local_id) || 
		sync_write((char*)path, len, local_id)){
        goto fail;
    }

    for(;;) {
		if (sync_read((unsigned char*)&msg.dent, sizeof(msg.dent), local_id)) break;
        //if(readx(fd, &msg.dent, sizeof(msg.dent))) break;
        if(msg.dent.id == ID_DONE) return 0;
        if(msg.dent.id != ID_DENT) break;

        len = ltohl(msg.dent.namelen);
        if(len > 256) break;

		if (sync_read((unsigned char*)buf, len, local_id)) break;
        buf[len] = 0;
    }

fail:
	sync_disconnect(local_id);
    return -1;
}

typedef struct syncsendbuf syncsendbuf;

struct syncsendbuf {
    unsigned id;
    unsigned size;
    char data[SYNC_DATA_MAX];
};

//static syncsendbuf send_buffer;

int Catransport::sync_readtime(const char *path, unsigned *timestamp, int local_id)
{
    syncmsg msg;
    int len = strlen(path);

    msg.req.id = ID_STAT;
    msg.req.namelen = htoll(len);

	if (sync_write(&msg.req, sizeof(msg.req), local_id) ||
		sync_write((char*)path, len, local_id)){
        return -1;
    }

	if (sync_read((unsigned char*)&msg.stat, sizeof(msg.stat), local_id)){
        return -1;
    }

    if(msg.stat.id != ID_STAT) {
        return -1;
    }

    *timestamp = ltohl(msg.stat.time);
    return 0;
}

int Catransport::sync_start_readtime(const char *path, int local_id)
{
    syncmsg msg;
    int len = strlen(path);

    msg.req.id = ID_STAT;
    msg.req.namelen = htoll(len);

	if (sync_write(&msg.req, sizeof(msg.req), local_id) || sync_write((char*)path, len, local_id)){
		return -1;
	}

    return 0;
}

int Catransport::sync_finish_readtime(unsigned int *timestamp,
	unsigned int *mode, unsigned int *size, int local_id)
{
    syncmsg msg;
	int msgsize = sizeof(msg.stat);
	if (sync_read((unsigned char*)&msg.stat, msgsize, local_id)){
		return -1;
	}   

    if(msg.stat.id != ID_STAT)
        return -1;

    *timestamp = ltohl(msg.stat.time);
    *mode = ltohl(msg.stat.mode);
    *size = ltohl(msg.stat.size);

    return 0;
}

int Catransport::sync_readmode(const char *path, unsigned *mode, int local_id)
{
    syncmsg msg;
    int len = strlen(path);
	int tsize = 0;

    msg.req.id = ID_STAT;
    msg.req.namelen = htoll(len);

	if (sync_write(&msg.req, sizeof(msg.req), local_id) || sync_write((char*)path, len, local_id)){
    //if(writex(fd, &msg.req, sizeof(msg.req)) ||
    //   writex(fd, path, len)) {
        return -1;
    }
	tsize = sizeof(msg.stat);
	if (sync_read((unsigned char*)&msg.stat, tsize, local_id)){
        return -1;
    }

    if(msg.stat.id != ID_STAT) {
        return -1;
    }

    *mode = ltohl(msg.stat.mode);
    return 0;
}

int Catransport::write_data_file(const char *path, int local_id)
{
    int err = 0;
	int total = 0;
	adbFile* lfd = NULL;
	syncsendbuf  sbufff;
	syncsendbuf *sbuf = &sbufff;

    lfd = adb_file_open(path, S_RDONLY);
    if(!lfd) {
        fprintf(stderr,"cannot open '%s': %s\n", path, strerror(errno));
        return -1;
    }

    sbuf->id = ID_DATA;
    for(;;) {
        int ret;

        ret = adb_file_read(lfd, sbuf->data, SYNC_DATA_MAX);
        if(!ret)
            break;

        if(ret < 0) {
            if(errno == EINTR)
                continue;
            fprintf(stderr,"cannot read '%s': %s\n", path, strerror(errno));
            break;
        }

        sbuf->size = htoll(ret);
		if (sync_write(sbuf, sizeof(unsigned) * 2 + ret, local_id)){
			err = -1;
			break;
		}
		total += ret;
    }
	if (!err)show_log("sync_send_success_len:%d\n", total);

    adb_file_close(lfd);
    return err;
}

int Catransport::write_data_buffer(char* file_buffer, int size, int local_id)
{
    int err = 0;
    int total = 0;
	syncsendbuf  sbufff;
	syncsendbuf *sbuf = &sbufff;

    sbuf->id = ID_DATA;
    while (total < size) {
        int count = size - total;
        if (count > SYNC_DATA_MAX) {
            count = SYNC_DATA_MAX;
        }

        memcpy(sbuf->data, &file_buffer[total], count);
        sbuf->size = htoll(count);
		if (sync_write(sbuf, sizeof(unsigned) * 2 + count, local_id)){
            err = -1;
            break;
        }
        total += count;
    }
	if (!err)show_log("sync_send_success_len:%d\n", total);
    return err;
}

int Catransport::sync_send(const char *lpath, const char *rpath,
	unsigned mtime, _mode_t mode, int verifyApk, int local_id)
{
    syncmsg msg;
    int len, r;
    //syncsendbuf *sbuf = &send_buffer;
	syncsendbuf sssbuf;
	syncsendbuf *sbuf = &sssbuf;
    char* file_buffer = NULL;
    int size = 0;
    char tmp[64];

    len = strlen(rpath);
    if(len > 1024) goto fail;

    snprintf(tmp, sizeof(tmp), ",%d", mode);
    r = strlen(tmp);

    if (verifyApk) {
        adbFile* lfd = NULL;
        zipfile_t zip;
        zipentry_t entry;
        int amt;

        // if we are transferring an APK file, then sanity check to make sure
        // we have a real zip file that contains an AndroidManifest.xml
        // this requires that we read the entire file into memory.
        lfd = adb_file_open(lpath, S_RDONLY);
        if(!lfd) {
            fprintf(stderr,"cannot open '%s': %s\n", lpath, strerror(errno));
            return -1;
        }

        size = adb_file_lseek(lfd, 0, SEEK_END);
        if (size == -1 || -1 == adb_file_lseek(lfd, 0, SEEK_SET)) {
            fprintf(stderr, "error seeking in file '%s'\n", lpath);
            adb_file_close(lfd);
            return 1;
        }

        file_buffer = (char *)malloc(size);
        if (file_buffer == NULL) {
            fprintf(stderr, "could not allocate buffer for '%s'\n",
                    lpath);
			adb_file_close(lfd);
            return 1;
        }
        amt = adb_file_read(lfd, file_buffer, size);
        if (amt != size) {
            fprintf(stderr, "error reading from file: '%s'\n", lpath);
			adb_file_close(lfd);
            free(file_buffer);
            return 1;
        }

		adb_file_close(lfd);

        zip = init_zipfile(file_buffer, size);
        if (zip == NULL) {
            fprintf(stderr, "file '%s' is not a valid zip file\n",
                    lpath);
            free(file_buffer);
            return 1;
        }

        entry = lookup_zipentry(zip, "AndroidManifest.xml");
        release_zipfile(zip);
        if (entry == NULL) {
            fprintf(stderr, "file '%s' does not contain AndroidManifest.xml\n",
                    lpath);
            free(file_buffer);
            return 1;
        }
    }

    msg.req.id = ID_SEND;
    msg.req.namelen = htoll(len + r);

	if (sync_write(&msg.req, sizeof(msg.req), local_id) ||
		sync_write((char*)rpath, len, local_id) ||
		sync_write(tmp, r, local_id))
	{
		free(file_buffer);
		goto fail;
	}

    if (file_buffer) {
		write_data_buffer(file_buffer, size, local_id);
        free(file_buffer);
    } else if (S_ISREG(mode))
		write_data_file(lpath, local_id);
    else
        goto fail;

    msg.data.id = ID_DONE;
    msg.data.size = htoll(mtime);
	if (sync_write(&msg.data, sizeof(msg.data), local_id)){
		goto fail;
	}

	if (sync_read((unsigned char*)&msg.status, sizeof(msg.status), local_id)){
		return -1;
	}

    if(msg.status.id != ID_OKAY) {
        if(msg.status.id == ID_FAIL) {
            len = ltohl(msg.status.msglen);
            if(len > 256) len = 256;
			if (sync_read((unsigned char*)sbuf->data, len, local_id, 2)){
                return -1;
            }
            sbuf->data[len] = 0;
        } else
            strcpy(sbuf->data, "unknown reason");

        fprintf(stderr,"failed to copy '%s' to '%s': %s\n", lpath, rpath, sbuf->data);
        return -1;
    }

    return 0;

fail:
    fprintf(stderr,"protocol failure\n");
	sync_disconnect(local_id);
    return -1;
}


int Catransport::sync_send_buffer(unsigned char* buffer, size_t _len, const char *rpath,
	unsigned mtime, _mode_t mode, int local_id)
{
	syncmsg msg;
	int len, r;
	syncsendbuf sssbuf;
	syncsendbuf *sbuf = &sssbuf;
	char* file_buffer = (char*)buffer;
	int size = _len;
	char tmp[64];

	len = strlen(rpath);
	if (len > 1024) goto fail;

	snprintf(tmp, sizeof(tmp), ",%d", mode);
	r = strlen(tmp);

	msg.req.id = ID_SEND;
	msg.req.namelen = htoll(len + r);

	if (sync_write(&msg.req, sizeof(msg.req), local_id) ||
		sync_write((char*)rpath, len, local_id) ||
		sync_write(tmp, r, local_id))
	{
		goto fail;
	}

	if (file_buffer) {
		write_data_buffer(file_buffer, size, local_id);
	}
	else
		goto fail;

	msg.data.id = ID_DONE;
	msg.data.size = htoll(mtime);
	if (sync_write(&msg.data, sizeof(msg.data), local_id)){
		goto fail;
	}

	if (sync_read((unsigned char*)&msg.status, sizeof(msg.status), local_id)){
		return -1;
	}

	if (msg.status.id != ID_OKAY) {
		if (msg.status.id == ID_FAIL) {
			len = ltohl(msg.status.msglen);
			if (len > 256) len = 256;
			if (sync_read((unsigned char*)sbuf->data, len, local_id, 2)){
				return -1;
			}
			sbuf->data[len] = 0;
		}
		else
			strcpy(sbuf->data, "unknown reason");

		fprintf(stderr, "failed to copy buffer to '%s': %s\n", rpath, sbuf->data);
		return -1;
	}

	return 0;

fail:
	fprintf(stderr, "protocol failure\n");
	sync_disconnect(local_id);
	return -1;
}

int Catransport::sync_recv(const char *rpath, const char *lpath, int local_id)
{
    syncmsg msg;
    int len;
	int total = 0;
    adbFile* lfd = NULL;
	syncsendbuf sssbuf;
    //char *buffer = send_buffer.data;
	char *buffer = sssbuf.data;
    unsigned id;

    len = strlen(rpath);
    if(len > 1024) return -1;

    msg.req.id = ID_RECV;
    msg.req.namelen = htoll(len);
	if (sync_write(&msg.req, sizeof(msg.req), local_id) ||
		sync_write((char*)rpath, len, local_id)){
        return -1;
    }
	if (sync_read((unsigned char*)&msg.data, sizeof(msg.data), local_id)){
        return -1;
    }
    id = msg.data.id;

    if((id == ID_DATA) || (id == ID_DONE)) {
        lfd = adb_file_creat(lpath, S_WNRONLY);	//0644
        if(!lfd) {
            fprintf(stderr,"cannot create '%s': %s\n", lpath, strerror(errno));
            return -1;
        }
        goto handle_data;
    } else {
        goto remote_error;
    }

    for(;;) {
		if (sync_read((unsigned char*)&msg.data, sizeof(msg.data), local_id)){
            return -1;
        }
        id = msg.data.id;

    handle_data:
        len = ltohl(msg.data.size);
        if(id == ID_DONE) break;
        if(id != ID_DATA) goto remote_error;
        if(len > SYNC_DATA_MAX) {
            fprintf(stderr,"data overrun\n");
            adb_file_close(lfd);
            return -1;
        }
		if (sync_read((unsigned char*)buffer, len, local_id)){
			adb_file_close(lfd);
            return -1;
        }

		if (!lfd->write(buffer, len).good()){
            fprintf(stderr,"cannot write '%s': %s\n", rpath, strerror(errno));
			adb_file_close(lfd);
            return -1;
        }
        total += len;
    }
	show_log("sync_load_success_len:%d\n", total);
	adb_file_close(lfd);
    return 0;

remote_error:
	adb_file_close(lfd);
    adb_unlink(lpath);

    if(id == ID_FAIL) {
        len = ltohl(msg.data.size);
        if(len > 256) len = 256;
		if (sync_read((unsigned char*)buffer, len, local_id)){
            return -1;
        }
        buffer[len] = 0;
    } else {
        memcpy(buffer, &id, 4);
        buffer[4] = 0;
        strcpy(buffer,"unknown reason");
    }
    fprintf(stderr,"failed to copy '%s' to '%s': %s\n", rpath, lpath, buffer);
    return 0;
}

int Catransport::sync_recv_buffer(bufferstream &_buffer, size_t &_len, const char *rpath, int local_id)
{
	syncmsg msg;
	int len;
	int total = 0;
	//adbFile* lfd = NULL;
	syncsendbuf sssbuf;
	//char *buffer = send_buffer.data;
	char *buffer = sssbuf.data;
	unsigned id;

	len = strlen(rpath);
	if (len > 1024) return -1;

	msg.req.id = ID_RECV;
	msg.req.namelen = htoll(len);
	if (sync_write(&msg.req, sizeof(msg.req), local_id) ||
		sync_write((char*)rpath, len, local_id)){
		return -1;
	}
	if (sync_read((unsigned char*)&msg.data, sizeof(msg.data), local_id)){
		return -1;
	}
	id = msg.data.id;

	if ((id == ID_DATA) || (id == ID_DONE)) {
		goto handle_data;
	}
	else {
		goto remote_error;
	}

	for (;;) {
		if (sync_read((unsigned char*)&msg.data, sizeof(msg.data), local_id)){
			return -1;
		}
		id = msg.data.id;

	handle_data:
		len = ltohl(msg.data.size);
		if (id == ID_DONE) break;
		if (id != ID_DATA) goto remote_error;
		if (len > SYNC_DATA_MAX) {
			fprintf(stderr, "data overrun\n");
			return -1;
		}
		if (sync_read_to_buffer(_buffer, len, local_id)){
			return -1;
		}

		total += len;
	}
	_len = total;
	show_log("sync_load_success_len:%d\n", total);
	return 0;

remote_error:

	if (id == ID_FAIL) {
		len = ltohl(msg.data.size);
		if (len > 256) len = 256;
		if (sync_read((unsigned char*)buffer, len, local_id)){
			return -1;
		}
		buffer[len] = 0;
	}
	else {
		memcpy(buffer, &id, 4);
		buffer[4] = 0;
		strcpy(buffer, "unknown reason");
	}
	fprintf(stderr, "failed to copy '%s' to buffer: %s\n", rpath, buffer);
	return 0;
}

/* --- */


static void do_sync_ls_cb(unsigned mode, unsigned size, unsigned time,
                          const char *name, void *cookie)
{
    printf("%08x %08x %08x %s\n", mode, size, time, name);
}
//
//int do_sync_ls(const char *path, BipBuffer bip, atransport* t)
//{
//    //int fd = adb_connect("sync:");
//	if (adb_sync_connect(bip, t) != 1) {
//    //if(fd < 0) {
//        //fprintf(stderr,"error: %s\n", adb_error());
//        return 1;
//    }
//
//    if(sync_ls(path, do_sync_ls_cb, 0, bip, t)) {
//        return 1;
//    } else {
//        sync_quit(bip, t);
//        return 0;
//    }
//}

typedef struct copyinfo copyinfo;

//struct copyinfo
//{
//    copyinfo *next;
//    const char *src;
//    const char *dst;
//    unsigned int time;
//    unsigned int mode;
//    unsigned int size;
//    int flag;
//    //char data[0];
//};

copyinfo *mkcopyinfo(const char *spath, const char *dpath,
                     const char *name, int isdir)
{
    int slen = strlen(spath);
    int dlen = strlen(dpath);
    int nlen = strlen(name);
    int ssize = slen + nlen + 2;
    int dsize = dlen + nlen + 2;

    copyinfo *ci = (copyinfo *)malloc(sizeof(copyinfo) + ssize + dsize);
    if(ci == 0) {
        fprintf(stderr,"out of memory\n");
        abort();
    }

    ci->next = 0;
    ci->time = 0;
    ci->mode = 0;
    ci->size = 0;
    ci->flag = 0;
    ci->src = (const char*)(ci + 1);
    ci->dst = ci->src + ssize;
    snprintf((char*) ci->src, ssize, isdir ? "%s%s/" : "%s%s", spath, name);
    snprintf((char*) ci->dst, dsize, isdir ? "%s%s/" : "%s%s", dpath, name);

//    fprintf(stderr,"mkcopyinfo('%s','%s')\n", ci->src, ci->dst);
    return ci;
}


static int local_build_list(copyinfo **filelist,
                            const char *lpath, const char *rpath)
{
 //   DIR *d;
 //   struct dirent *de;
	struct _finddata_t c_file;
	intptr_t   hFile; 
    struct stat st;
    copyinfo *dirlist = 0;
    copyinfo *ci, *next;

//    fprintf(stderr,"local_build_list('%s','%s')\n", lpath, rpath);

   // d = opendir(lpath);
//	if(d == 0)
	if(_chdir(lpath)) 
     {
        fprintf(stderr,"cannot open '%s': %s\n", lpath, strerror(errno));
        return -1;
    }
	else
	{
		hFile = _findfirst("*.*", &c_file); 
	}

   // while((de = readdir(d))) 
	do 
	{
        char stat_path[MAX_PATH];
        char *name = c_file.name;

        if(name[0] == '.') {
            if(name[1] == 0) continue;
            if((name[1] == '.') && (name[2] == 0)) continue;
        }

        /*
         * We could use d_type if HAVE_DIRENT_D_TYPE is defined, but reiserfs
         * always returns DT_UNKNOWN, so we just use stat() for all cases.
         */
        if (strlen(lpath) + strlen(c_file.name) + 1 > sizeof(stat_path))
		{
			_findnext(hFile, &c_file);
            continue;
		}
        strcpy(stat_path, lpath);
        strcat(stat_path, c_file.name);
        stat(stat_path, &st);

        if (S_IFDIR&st.st_mode) {
            ci = mkcopyinfo(lpath, rpath, name, 1);
            ci->next = dirlist;
            dirlist = ci;
        } else {
            ci = mkcopyinfo(lpath, rpath, name, 0);
            if(lstat(ci->src, &st)) {
             //   closedir(d);
				_findclose(hFile);
                fprintf(stderr,"cannot stat '%s': %s\n", ci->src, strerror(errno));
                return -1;
            }
            if(!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) {
                fprintf(stderr, "skipping special file '%s'\n", ci->src);
                free(ci);
            } else {
                ci->time = st.st_mtime;
                ci->mode = st.st_mode;
                ci->size = st.st_size;
                ci->next = *filelist;
                *filelist = ci;
            }
        }
    }while(_findnext(hFile, &c_file) == 0);

  //  closedir(d);
	_findclose(hFile); 

    for(ci = dirlist; ci != 0; ci = next) {
        next = ci->next;
        local_build_list(filelist, ci->src, ci->dst);
        free(ci);
    }

    return 0;
}


int Catransport::copy_local_dir_remote(const char *lpath, const char *rpath, int checktimestamps, int listonly, int local_id)
{
    copyinfo *filelist = 0;
    copyinfo *ci, *next;
    int pushed = 0;
    int skipped = 0;

    if((lpath[0] == 0) || (rpath[0] == 0)) return -1;
    if(lpath[strlen(lpath) - 1] != '/') {
        int  tmplen = strlen(lpath)+2;
        char *tmp =(char *) malloc(tmplen);
        if(tmp == 0) return -1;
        snprintf(tmp, tmplen, "%s/",lpath);
        lpath = tmp;
    }
    if(rpath[strlen(rpath) - 1] != '/') {
        int tmplen = strlen(rpath)+2;
        char *tmp =(char *) malloc(tmplen);
        if(tmp == 0) return -1;
        snprintf(tmp, tmplen, "%s/",rpath);
        rpath = tmp;
    }

    if(local_build_list(&filelist, lpath, rpath)) {
        return -1;
    }

    if(checktimestamps){
        for(ci = filelist; ci != 0; ci = ci->next) {
			if (sync_start_readtime(ci->dst, local_id)) {
                return 1;
            }
        }
        for(ci = filelist; ci != 0; ci = ci->next) {
            unsigned int timestamp, mode, size;
			if (sync_finish_readtime(&timestamp, &mode, &size, local_id))
                return 1;
            if(size == ci->size) {
                /* for links, we cannot update the atime/mtime */
                if((S_ISREG(ci->mode & mode) && timestamp == ci->time) ||
                    (S_ISLNK(ci->mode & mode) && timestamp >= ci->time))
                    ci->flag = 1;
            }
        }
    }
    for(ci = filelist; ci != 0; ci = next) {
        next = ci->next;
        if(ci->flag == 0) {
            fprintf(stderr,"%spush: %s -> %s\n", listonly ? "would " : "", ci->src, ci->dst);
            if(!listonly &&
				sync_send(ci->src, ci->dst, ci->time, ci->mode, 0, local_id)){/* no verify APK */
                return 1;
            }
            pushed++;
        } else {
            skipped++;
        }
        free(ci);
    }

    fprintf(stderr,"%d file%s pushed. %d file%s skipped.\n",
            pushed, (pushed == 1) ? "" : "s",
            skipped, (skipped == 1) ? "" : "s");

    return 0;
}


int Catransport::do_sync_push(const char *lpath, const char *rpath, int verifyApk, int local_id)
{
    struct stat st;
    unsigned mode;
    //int fd;

	if (sync_connect(local_id)) {
        return 1;
    }

    if(stat(lpath, &st)) {
        //fprintf(stderr,"cannot stat '%s': %s\n", lpath, strerror(errno));
		sync_quit(local_id);
        return 1;
    }

    if(S_ISDIR(st.st_mode)) {
		if (copy_local_dir_remote(lpath, rpath, 0, 0, local_id)) {
            return 1;
        } else {
			sync_quit(local_id);
        }
    } else {
		if (sync_readmode(rpath, &mode, local_id)) {
            return 1;
        }
        if((mode != 0) && S_ISDIR(mode)) {
                /* if we're copying a local file to a remote directory,
                ** we *really* want to copy to remotedir + "/" + localfilename
                */
            const char *name = adb_dirstop(lpath);
            if(name == 0) {
                name = lpath;
            } else {
                name++;
            }
            int  tmplen = strlen(name) + strlen(rpath) + 2;
            char *tmp = (char *)malloc(strlen(name) + strlen(rpath) + 2);
            if(tmp == 0) return 1;
            snprintf(tmp, tmplen, "%s/%s", rpath, name);
            rpath = tmp;
        }

		if (sync_send(lpath, rpath, st.st_mtime, st.st_mode, verifyApk, local_id)) {
            return 1;
        } else {
			sync_quit(local_id);
            return 0;
        }
    }

    return 0;
}


typedef struct {
    copyinfo **filelist;
    copyinfo **dirlist;
    const char *rpath;
    const char *lpath;
} sync_ls_build_list_cb_args;

void
sync_ls_build_list_cb(unsigned mode, unsigned size, unsigned time,
                      const char *name, void *cookie)
{
    sync_ls_build_list_cb_args *args = (sync_ls_build_list_cb_args *)cookie;
    copyinfo *ci;

    if (S_ISDIR(mode)) {
        copyinfo **dirlist = args->dirlist;

        /* Don't try recursing down "." or ".." */
        if (name[0] == '.') {
            if (name[1] == '\0') return;
            if ((name[1] == '.') && (name[2] == '\0')) return;
        }

        ci = mkcopyinfo(args->rpath, args->lpath, name, 1);
        ci->next = *dirlist;
        *dirlist = ci;
    } else if (S_ISREG(mode) || S_ISLNK(mode)) {
        copyinfo **filelist = args->filelist;

        ci = mkcopyinfo(args->rpath, args->lpath, name, 0);
        ci->time = time;
        ci->mode = mode;
        ci->size = size;
        ci->next = *filelist;
        *filelist = ci;
    } else {
        fprintf(stderr, "skipping special file '%s'\n", name);
    }
}

int Catransport::remote_build_list(copyinfo **filelist,	const char *rpath, const char *lpath, int local_id)
{
    copyinfo *dirlist = NULL;
    sync_ls_build_list_cb_args args;

    args.filelist = filelist;
    args.dirlist = &dirlist;
    args.rpath = rpath;
    args.lpath = lpath;

    /* Put the files/dirs in rpath on the lists. */
	if (sync_ls(rpath, (void *)&args, local_id)) {
        return 1;
    }

    /* Recurse into each directory we found. */
    while (dirlist != NULL) {
        copyinfo *next = dirlist->next;
		if (remote_build_list(filelist, dirlist->src, dirlist->dst, local_id)) {
            return 1;
        }
        free(dirlist);
        dirlist = next;
    }

    return 0;
}

int Catransport::copy_remote_dir_local(const char *rpath, const char *lpath,
	int checktimestamps, int local_id)
{
    copyinfo *filelist = 0;
    copyinfo *ci, *next;
    int pulled = 0;
    int skipped = 0;

    /* Make sure that both directory paths end in a slash. */
    if (rpath[0] == 0 || lpath[0] == 0) return -1;
    if (rpath[strlen(rpath) - 1] != '/') {
        int  tmplen = strlen(rpath) + 2;
        char *tmp =(char *) malloc(tmplen);
        if (tmp == 0) return -1;
        snprintf(tmp, tmplen, "%s/", rpath);
        rpath = tmp;
    }
    if (lpath[strlen(lpath) - 1] != '/') {
        int  tmplen = strlen(lpath) + 2;
        char *tmp = (char *)malloc(tmplen);
        if (tmp == 0) return -1;
        snprintf(tmp, tmplen, "%s/", lpath);
        lpath = tmp;
    }

    fprintf(stderr, "pull: building file list...\n");
    /* Recursively build the list of files to copy. */
	if (remote_build_list(&filelist, rpath, lpath, local_id)) {
        return -1;
    }

//#if 0
//    if (checktimestamps) {
//        for (ci = filelist; ci != 0; ci = ci->next) {
//            if (sync_start_readtime(fd, ci->dst)) {
//                return 1;
//            }
//        }
//        for (ci = filelist; ci != 0; ci = ci->next) {
//            unsigned int timestamp, mode, size;
//            if (sync_finish_readtime(fd, &timestamp, &mode, &size))
//                return 1;
//            if (size == ci->size) {
//                /* for links, we cannot update the atime/mtime */
//                if ((S_ISREG(ci->mode & mode) && timestamp == ci->time) ||
//                    (S_ISLNK(ci->mode & mode) && timestamp >= ci->time))
//                    ci->flag = 1;
//            }
//        }
//    }
//#endif
    for (ci = filelist; ci != 0; ci = next) {
        next = ci->next;
        if (ci->flag == 0) {
            fprintf(stderr, "pull: %s -> %s\n", ci->src, ci->dst);
			if (sync_recv(ci->src, ci->dst, local_id)) {
                return 1;
            }
            pulled++;
        } else {
            skipped++;
        }
        free(ci);
    }

    fprintf(stderr, "%d file%s pulled. %d file%s skipped.\n",
            pulled, (pulled == 1) ? "" : "s",
            skipped, (skipped == 1) ? "" : "s");

    return 0;
}

int Catransport::do_sync_pull(const char *rpath, const char *lpath, int local_id)
{
    unsigned mode;
    struct stat st;

    //int fd;

	if (sync_connect(local_id)) {
        //fprintf(stderr,"error: %s\n", adb_error());
        return 1;
    }

	if (sync_readmode(rpath, &mode, local_id)) {
        return 1;
    }
    if(mode == 0) {
        //fprintf(stderr,"remote object '%s' does not exist\n", rpath);
        return 1;
    }

    if(S_ISREG(mode) || S_ISLNK(mode) || S_ISCHR(mode) || S_ISBLK(mode)) {
        if(stat(lpath, &st) == 0) {
            if(S_ISDIR(st.st_mode)) {
                    /* if we're copying a remote file to a local directory,
                    ** we *really* want to copy to localdir + "/" + remotefilename
                    */
                const char *name = adb_dirstop(rpath);
                if(name == 0) {
                    name = rpath;
                } else {
                    name++;
                }
                int  tmplen = strlen(name) + strlen(lpath) + 2;
                char *tmp = (char *)malloc(tmplen);
                if(tmp == 0) return 1;
                snprintf(tmp, tmplen, "%s/%s", lpath, name);
                lpath = tmp;
            }
        }
        //BEGIN();
		if (sync_recv(rpath, lpath, local_id)) {
            return 1;
        } else {
            //END();
			sync_quit(local_id);
            return 0;
        }
    } else if(S_ISDIR(mode)) {
        //BEGIN();
		if (copy_remote_dir_local(rpath, lpath, 0, local_id)) {
            return 1;
        } else {
            //END();
			sync_quit(local_id);
            return 0;
        }
    } else {
        fprintf(stderr,"remote object '%s' not a file or directory\n", rpath);
        return 1;
    }
}

int Catransport::do_sync_sync(const char *lpath, const char *rpath, int listonly, int local_id)
{
    fprintf(stderr,"syncing %s...\n",rpath);

    //int fd = adb_connect("sync:");
    //if(fd < 0) {
	if (sync_connect(local_id)){
        //fprintf(stderr,"error: %s\n", adb_error());
        return 1;
    }

    //BEGIN();
	if (copy_local_dir_remote(lpath, rpath, 1, listonly, local_id)){
        return 1;
    } else {
        //END();
		sync_quit(local_id);
        return 0;
    }
}

int Catransport::do_sync_push_buffer(unsigned char* buffer, size_t len, const char *rpath, int local_id){
	struct stat st;
	unsigned mode = 0; //33206 is normal file
	//int fd;
	st.st_mtime = time(NULL);
	st.st_mode = 33206;	//is normal file

	if (sync_connect(local_id)) {
		//fprintf(stderr,"error: %s\n", adb_error());
		return 1;
	}

	if (sync_readmode(rpath, &mode, local_id)) {
		return 1;
	}
	if ((mode != 0) && S_ISDIR(mode)) {
		//local_socket_faile_notify("error: remote object does not a file", (int)bip);
		return 1;
	}
	if (sync_send_buffer(buffer, len, rpath, st.st_mtime, st.st_mode, local_id)) {
		return 1;
	}
	else {
		sync_quit(local_id);
		return 0;
	}

	return 1;
}

int Catransport::do_sync_pull_buffer(bufferstream &buffer, size_t &len, const char *rpath, int local_id){
	unsigned mode;
	//struct stat st;

	//int fd;

	if (sync_connect(local_id)) {
		//fprintf(stderr,"error: %s\n", adb_error());
		return 1;
	}

	if (sync_readmode(rpath, &mode, local_id)) {
		return 1;
	}
	if (mode == 0) {
		//fprintf(stderr,"remote object '%s' does not exist\n", rpath);
		return 1;
	}

	if (S_ISREG(mode) || S_ISLNK(mode) || S_ISCHR(mode) || S_ISBLK(mode)) {
		
		if (sync_recv_buffer(buffer, len, rpath, local_id)) {
			return 1;
		}
		else {
			sync_quit(local_id);
			return 0;
		}
	}
	else {
		fprintf(stderr, "remote object '%s' not a file\n", rpath);
		return 1;
	}
}