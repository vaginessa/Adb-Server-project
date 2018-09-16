#include <Windows.h>
#include <win32_adb.h>
#include "adb_func.h"
#include "adb_auth.h"	//安卓 密钥
#include <map>

#include "file_sync_service.h"

//日志输出
void show_log(const char* _Format, ...){
#ifdef _ADB_LOG_SHOW_
	va_list marker = NULL;
	va_start(marker, _Format);
	int num_of_chars = _vscprintf(_Format, marker) + 1;
	char *log = (char*)malloc(num_of_chars);
	if (log){
		vsprintf(log, _Format, marker);
		OutputDebugStringA(log);
		free(log);
	}
	va_end(marker);
#endif // _ADB_LOG_SHOW_
}

void show_apacket(const char* label, apacket* p){
#ifdef _ADB_LOG_SHOW_
	char *tag;
	int _duppmax = 32;

	switch (p->msg.command){
	case A_SYNC: tag = "SYNC"; break;
	case A_CNXN: tag = "CNXN"; break;
	case A_OPEN: tag = "OPEN"; break;
	case A_OKAY: tag = "OKAY"; break;
	case A_CLSE: tag = "CLSE"; break;
	case A_WRTE: tag = "WRTE"; break;
	case A_AUTH: tag = "AUTH"; break;
	default: tag = "????"; break;
	}
	int len = p->len ? p->len : p->msg.data_length;
	char* buff = "\0";
	if (len){
		buff = (char*)p->data;
	}
	show_log(" %s: %s arg0<%08x> arg1<%08x> data_length<%d> \n data:%s\n",
		label, tag, p->msg.arg0, p->msg.arg1, len, buff);
#endif //_ADB_LOG_SHOW_
}

/**************************************************************************/
/**************************************************************************/
/*****                                                                *****/
/*****    USB API				                                      *****/
/*****                                                                *****/
/**************************************************************************/
/**************************************************************************/

//安卓设备
#define ADB_CLASS              0xff
#define ADB_SUBCLASS           0x42
#define ADB_PROTOCOL           0x1

#ifdef HAVE_BIG_ENDIAN
#define H4(x)	(((x) & 0xFF000000) >> 24) | (((x) & 0x00FF0000) >> 8) | (((x) & 0x0000FF00) << 8) | (((x) & 0x000000FF) << 24)
static inline void fix_endians(apacket *p)
{
	p->msg.command = H4(p->msg.command);
	p->msg.arg0 = H4(p->msg.arg0);
	p->msg.arg1 = H4(p->msg.arg1);
	p->msg.data_length = H4(p->msg.data_length);
	p->msg.data_check = H4(p->msg.data_check);
	p->msg.magic = H4(p->msg.magic);
}
#else
#define fix_endians(p) do {} while (0)
#endif

static adb_usb_mutex	usb_lock;// 列表操作锁

adb_usb_mutex	log_lock;// 日志输出锁

//=========================================USB===========================================

unsigned host_to_le32(unsigned n)
{
	return n;
}


int is_adb_interface(int vid, int pid, int usb_class, int usb_subclass, int usb_protocol)
{
	unsigned i;
	for (i = 0; i < vendorIdCount; i++) {
		if (vid == vendorIds[i]) {
			if (usb_class == ADB_CLASS && usb_subclass == ADB_SUBCLASS &&
				usb_protocol == ADB_PROTOCOL) {
				return 1;
			}

			return 0;
		}
	}
	return 0;
}

//=======================================================================================================================

atransportPtr& get_device(const char* name_serial){
	adb_mutex_lock	_lock(atransport_lock);
	return devices_list[name_serial];
}

void	remove_device_All(){
	int nCount = 0;
	adb_mutex_lock	_lock(atransport_lock);
	for (devicesMap::iterator it = devices_list.begin(); it != devices_list.end(); ++it)
	{
		++nCount;
		atransportPtr &t = it->second;
		if (t){
			t->Clear();}
	}
	if (nCount == 0){ devices_list.clear(); }
}

void	checked_devices(){//检查设备如果是剩下自身引用的删除
	adb_mutex_lock	_lock(atransport_lock);
	for (devicesMap::iterator it = devices_list.begin(); it != devices_list.end(); /*++it*/)
	{
		atransportPtr &t = it->second;
		if (t.unique() && t->kicked != 0){
			t.reset();
			devices_list.erase(it++);
		}else{
			++it; }
	}
}

void get_device_name_list(sstring& retlist){
	adb_mutex_lock	_lock(atransport_lock);
	for (devicesMap::iterator it = devices_list.begin(); it != devices_list.end(); ++it)
	{
		atransportPtr &t = it->second;
		if (t->kicked == 0){//没有被剔除的设备返回
			retlist += it->first;
			retlist += "|";
		}
	}
}

/**************************************************************************/
/**************************************************************************/
/*****                                                                *****/
/*****    传递数据结构 函数		                                      *****/
/*****                                                                *****/
/**************************************************************************/
/**************************************************************************/

void bip_buffer_init(BipBuffer  buffer)
{
	buffer->fdin = 0;
	buffer->fdout = 0;
	buffer->closed = 0;
	buffer->ptrlist = new BipPtrList(0);	//自动列表
}

void bip_buffer_close(BipBuffer  bip)
{
	bip->closed = 1;
}

void bip_apacke_del_all(void* p){//apacket队列 consume_all 批量删除用
	if (p){ free((apacketPtr)p); }
}

void	bip_buffer_done(BipBuffer  bip)
{
	BIPD(("bip_buffer_done: %d->%d\n", bip->fdin, bip->fdout));
	if (bip){
		BipLpList temp = bip->ptrlist;
		bip->ptrlist = NULL;
		apacket* p = NULL;
		if (temp){
			temp->consume_all(bip_apacke_del_all);
			delete temp;
		}
	}
}

int	bip_buffer_write(BipBuffer  bip, void* & _scr)
{
	//D("bip_buffer_write: enter %d->%d addr %d\n", bip->fdin, bip->fdout, (int)_scr);

	int succ = 0;
	do{
		if (!bip->ptrlist){ return -1; }
		if (bip->closed) {
			errno = EPIPE;
			//D("bip_buffer_write: closed %d->%d addr %d\n", bip->fdin, bip->fdout, (int)_scr);
			return -1;
		}
		//无锁队列直接写入
		if (bip->ptrlist->push(_scr)){//直到可以写入
			_scr = NULL;//传入指针后 去除指向
			succ = 1;
		}
	} while (!succ);
	//D("bip_buffer_write: exit %d->%d addr %d\n", bip->fdin, bip->fdout, (int)_scr);
	return 0;
}

int	bip_buffer_read(BipBuffer  bip, void*& _dst)
{
	//D("bip_buffer_read: enter %d->%d addr %d\n", bip->fdin, bip->fdout, (int)_dst);
	int succ = 0;
	do{
		if (!bip->ptrlist){ return -1; }
		if (bip->closed) {
			errno = EPIPE;
			//D("bip_buffer_read: closed %d->%d addr %d\n", bip->fdin, bip->fdout, (int)_dst);
			return -1;
		}
		if (bip->ptrlist->pop(_dst)){//非空情况下 获取 数据 否则 等待
			succ = 1;
		}
	} while (!succ);
	//D("bip_buffer_read: exit %d->%d addr %d\n", bip->fdin, bip->fdout, (int)_dst);
	return 0;
}


SocketPair  adb_socketpair()
{
	SocketPair  pair = new SocketPairRec();

	//pair = (SocketPair)malloc(sizeof(*pair));
	if (pair == NULL) {
		//D("adb_socketpair: not enough memory to allocate pipes\n");
		goto Fail;
	}

	bip_buffer_init(&pair->a2b_bip);
	bip_buffer_init(&pair->b2a_bip);

	pair->used = 2;
	pair->a_fd = 100;//标志 设备传出传入为 非 1

	//-----------------可用情况待定-------
	pair->a2b_bip.fdin = 1;
	pair->a2b_bip.fdout = 2;
	pair->b2a_bip.fdin = 2;
	pair->b2a_bip.fdout = 1;
	//------------------------------------
	return pair;

Fail:
	return NULL;
}

int	_fh_socketpair_close(int f, SocketPair  _pair)
{
	if (_pair) {
		SocketPair  pair = _pair;

		bip_buffer_close(&pair->b2a_bip);
		bip_buffer_close(&pair->a2b_bip);

		if (pair->used == 0){
			bip_buffer_done(&pair->b2a_bip);
			bip_buffer_done(&pair->a2b_bip);
			pair->a_fd = NULL;
		}
	}
	return 0;
}

int	_fh_socketpair_lseek(int  f, int pos, int  origin)
{
	//errno = ESPIPE;
	return -1;
}

int	_fh_socketpair_read(int f, SocketPair  _pair, void*& _dst)
{
	SocketPair  pair = _pair;
	BipBuffer   bip;
	if (!pair)
		return -1;

	if (f == pair->a_fd)
		bip = &pair->b2a_bip;
	else
		bip = &pair->a2b_bip;

	return bip_buffer_read(bip, _dst);
}

int	_fh_socketpair_write(int f, SocketPair  _pair, void*& _scr)
{
	SocketPair  pair = _pair;
	BipBuffer   bip;
	if (!pair)
		return -1;

	if (f == pair->a_fd)
		bip = &pair->a2b_bip;
	else
		bip = &pair->b2a_bip;

	return bip_buffer_write(bip, _scr);
}

int  adb_read(int f, SocketPair  _pair, void*& _dst)
{
	if (_pair == NULL) {
		return -1;
	}
	return _fh_socketpair_read(f, _pair, _dst);
}


int  adb_write(int f, SocketPair  _pair, void* &_scr)
{
	if (_pair == NULL) {
		return -1;
	}
	return _fh_socketpair_write(f, _pair, _scr);
}


int  adb_lseek(int  fd, int  pos, int  where)
{
	if (!fd) {
		return -1;
	}
	return _fh_socketpair_lseek(fd, pos, where);
}

int  adb_close(int f, SocketPair  _pair)
{
	if (!_pair) {
		return -1;
	}
	//D("adb_close: %s\n", f->name);
	_fh_socketpair_close(f, _pair);
	return 0;
}

int check_header(apacket *p)
{
	if (!p){ return -1; }
	if (p->msg.magic != (p->msg.command ^ 0xffffffff)) {
		//D("check_header(): invalid magic\n");
		return -1;
	}

	if (p->msg.data_length > MAX_PAYLOAD) {
		//D("check_header(): %d > MAX_PAYLOAD\n", p->msg.data_length);
		return -1;
	}

	return 0;
}

int check_data(apacket *p)
{
	unsigned count, sum;
	unsigned char *x;

	if (!p){ return -1; }

	count = p->msg.data_length;
	x = p->data;
	sum = 0;
	while (count-- > 0) {
		sum += *x++;
	}

	if (sum != p->msg.data_check) {
		return -1;
	}
	else {
		return 0;
	}
}


size_t fill_connect_data(char *buf, size_t bufsize)
{
	return snprintf(buf, bufsize, "host::") + 1;
}

/* qual_overwrite is used to overwrite a qualifier string.  dst is a
* pointer to a char pointer.  It is assumed that if *dst is non-NULL, it
* was malloc'ed and needs to freed.  *dst will be set to a dup of src.
*/
void qual_overwrite(char **dst, const char *src)
{
	if (!dst)
		return;

	free(*dst);
	*dst = NULL;

	if (!src || !*src)
		return;

	*dst = strdup(src);
}


char *adb_strtok_r(char *s, const char *delim, char **last)
{
	char *spanp;
	int c, sc;
	char *tok;


	if (s == NULL && (s = *last) == NULL)
		return (NULL);

	/*
	* Skip (span) leading delimiters (s += strspn(s, delim), sort of).
	*/
cont:
	c = *s++;
	for (spanp = (char *)delim; (sc = *spanp++) != 0;) {
		if (c == sc)
			goto cont;
	}

	if (c == 0) {		/* no non-delimiter characters */
		*last = NULL;
		return (NULL);
	}
	tok = s - 1;

	/*
	* Scan token (scan for delimiters: s += strcspn(s, delim), sort of).
	* Note that delim must have one NUL; we stop if we see that, too.
	*/
	for (;;) {
		c = *s++;
		spanp = (char *)delim;
		do {
			if ((sc = *spanp++) == c) {
				if (c == 0)
					s = NULL;
				else
					s[-1] = 0;
				*last = s;
				return (tok);
			}
		} while (sc != 0);
	}
	/* NOTREACHED */
}

void *output_thread(void *_t)
{
	atransportPtr t = (*(atsweakPtr *)_t).lock();

	apacket *p = NULL;
	int _online = 0,
		fd = 0;

	if (t){
		_online = 1;
		fd = t->fd;
		D("%s: starting transport output thread on fd %d, SYNC online (%d)\n",
			t->serial, t->fd, t->sync_token + 1);

		p = t->get_apacket();
		p->msg.command = A_SYNC;
		p->msg.arg0 = 1;
		p->msg.arg1 = ++(t->sync_token);
		p->msg.magic = A_SYNC ^ 0xffffffff;
		if (t->write_packet(fd, p)) {
			t->put_apacket(p);
			D("%s: failed to write SYNC packet\n", t->serial);
			goto oops;
		}
		D("%s: data pump started\n", t->serial);
	}

	while (_online) {
		if (!p){ p = t->get_apacket(); }
		if (!p){ continue; }
		if (t->read_from_remote(p) == 0){
			print_packet("read_from_remote():", p);
			if (t->write_packet(fd, p)){
				t->put_apacket(p);
				D("%s: failed to write apacket to transport\n", t->serial);
				goto oops;
			}
		}
		else {
			D("%s: remote read failed for transport\n", t->serial);
			t->put_apacket(p);
			break;
		}
	}

	if (t){
		D("%s: SYNC offline for transport\n", t->serial);
		p = t->get_apacket();
		p->msg.command = A_SYNC;
		p->msg.arg0 = 0;
		p->msg.arg1 = 0;
		p->msg.magic = A_SYNC ^ 0xffffffff;
		if (t->write_packet(fd, p)) {
			t->put_apacket(p);
			D("%s: failed to write SYNC apacket to transport", t->serial);
		}
	}

oops:
	if (t){
		D("%s: transport output thread is exiting\n", t->serial);
		t->Clear();
		--(t->ref_count);
		t.reset();
	}
	return 0;
}


void *input_thread(void *_t)
{
	atransportPtr t = (*(atsweakPtr *)_t).lock();

	apacket *p = NULL;
	int active = 0,
		fd = 0,
		_online = 0;

	if (t){
		_online = 1;
		fd = t->fd;
		D("%s: starting transport input thread, reading from fd %d\n",
			t->serial, fd);
	}

	while (_online){
		if (t->read_packet(fd, p)) {
			D("%s: failed to read apacket from transport on fd %d\n",
				t->serial, t->fd);
			break;
		}
		if (!p){ continue; }
		
		if (p->msg.command == A_SYNC){
			if (p->msg.arg0 == 0) {
				D("%s: transport SYNC offline\n", t->serial);
				t->put_apacket(p);
				break;
			}
			else {
				if (p->msg.arg1 == t->sync_token) {
					D("%s: transport SYNC online\n", t->serial);
					active = 1;
				}
				else {
					D("%s: transport ignoring SYNC %d != %d\n",
						t->serial, p->msg.arg1, t->sync_token);
				}
			}
		}
		else {
			if (active) {
				show_apacket("write_to_remote():", p);
				t->write_to_remote(p);
			}
			else {
				D("%s: transport ignoring packet while offline\n", t->serial);
			}
		}
		t->put_apacket(p);
	}

	if (t){
		D("%s: transport input thread is exiting, fd %d\n", t->serial, t->fd);
		t->Clear();
		--(t->ref_count);
		t.reset();
	}
	return 0;
}

void *user_thread(void *_t)	///用户输入接收
{
	atransportPtr t = (*(atsweakPtr *)_t).lock();

	apacket *p = NULL;
	int __online = 0,
		tfd = 0;

	if (t){ 
		__online = 1; 
		tfd = t->transport_socket;
	}

	while (__online)
	{
		p = NULL;
		if (t->read_packet(tfd, p)){
			D("%s: failed to read packet from transport socket on fd \n", t->serial);
			break;
		}
		else if (!p){
			//空包
		}else {
			D("%s: read packet from transport socket on addr : %d\n", t->serial, (int)p);
			t->handle_packet(p);
		}
		if (t->kicked){
			__online = 0;
		}
	}

	if (t){
		D("%s: transport user thread is exiting, fd %d\n", t->serial, t->fd);
		t->Clear();
		--(t->ref_count);
		t.reset();
	}
	return 0;
}
