#include "Catransport.h"
#include <boost\locale.hpp>

void apacke_del_all(apacketPtr p){
	if (p){ free(p); }
}

Catransport::Catransport(usb_handle *h, const char *_serial, const char *_devpath, int state)
	:loop_to_use_apacket(0), 
	userlist(client_list_max, NULL),
	d_write_ctrol(5),
	d_read_ctrol(5),
	atp(NULL)
{
	D("transport: usb\n");
	sync_token = 1;
	connection_state = state;
	type = kTransportUsb;
	usb = h;
	connectCount = 0;
	ref_count = 0;
	online = 0;
	pair = NULL;
	serial = NULL;
	product = NULL;
	model = NULL;
	device = NULL;
	devpath = NULL;
	sfd = 0;
	key = NULL;
	kicked = 0;
	if (_serial){ serial = strdup(_serial); }
	if (_devpath){ devpath = strdup(_devpath); }
}

Catransport::~Catransport()	//析构 释放所有资源 并从 usb_handle 队列中剔除
{
	//释放资源
	D("transport: %s removing and free'ing %d\n", serial, transport_socket);
	/* IMPORTANT: the remove closes one half of the
	** socket pair.  The close closes the other half.
	*/
	while (ref_count != 0 || connectCount != 0)
	{
		Clear();
		delay(100);
	}
	pair->used = 0;
	Clear();

	if (pair){
		delete pair;
		pair = NULL;
	}
	if (usb){//从列表剔除设备
		usb_close(usb);
		usb = 0;
	}
	if (product)
		free(product);
	if (serial)
		free(serial);
	if (model)
		free(model);
	if (device)
		free(device);
	if (devpath)
		free(devpath);

	if (atp){ delete ((atsweakPtr*)atp); }//释放自身指针
	atp = NULL;

	client_removeAll();
	loop_to_use_apacket.consume_all(apacke_del_all);//批量删除
}

void	Catransport::Clear(){
	switch (type)
	{
	case kTransportUsb:{//usb通讯
		remote_usb_clear();
		break;
	}
	case kTransportLocal:
		break;
	case kTransportAny:
		break;
	case kTransportHost:
		break;
	default:
		break;
	}
	free_ctrol();
}

void Catransport::free_ctrol()	//释放同步锁
{
	//遍历通道表 将关闭的通道打开返回ID号
	bip_write_lock _wlick(bip_lock);
	int nCount = userlist.size();
	if (nCount){
		aclientPtr* ap = &userlist[0];
		aclientPtr	a = NULL;
		for (int i = 0; i < nCount; ++i){
			a = *ap;
			if (a){
				if (a->user){	//关闭所有通道并 通知退出
					a->user = 0;
					a->notify();
				}
			}
			++ap;
		}
	}
}

void	Catransport::remote_usb_clear()	//usb设备资源清理
{
	kick();			//剔除
	adb_close(fd, pair);//关闭通讯数据通道
	d_read_ctrol.Signal();
	d_write_ctrol.Signal();
	//usb_kick(usb);	//通道关闭
}
/**************************************************************/
/****				数据包处理					***************/
/**************************************************************/
apacketPtr	Catransport::get_apacket(){
	apacket *p = NULL;
	//对回收的数据包重复利用 避免频繁的创建释放资源
	if (!loop_to_use_apacket.pop(p)){ p = NULL; }
	if (!p){
		p = (apacket*)malloc(sizeof(apacket));
	}
	if (p) {
		memset(p, 0, sizeof(apacket) - MAX_PAYLOAD);
	}
	return p;
}

void		Catransport::put_apacket(apacketPtr& p){
	if (p){
		if (kicked){//已经断开 直接删除
			free(p);
		}else{//否则放到回收表循环使用 放入失败的话释放
			if (!loop_to_use_apacket.push(p)){ free(p); }
		}
	}
	p = NULL;
}
/**************************************************************/
/****				数据传输					***************/
/**************************************************************/

void Catransport::send_auth_response(uint8_t *token, size_t token_size){
	D("Calling send_auth_response\n");
	apacket *p = get_apacket();
	int ret;

	ret = adb_auth_sign(key, token, token_size, p->data);
	if (!ret) {
		D("Error signing the token\n");
		put_apacket(p);
		return;
	}

	p->msg.command = A_AUTH;
	p->msg.arg0 = ADB_AUTH_SIGNATURE;
	p->msg.data_length = ret;
	send_packet(p);
}

void Catransport::send_auth_publickey(){
	D("Calling send_auth_publickey\n");
	apacket *p = get_apacket();
	int ret;

	ret = adb_auth_get_userkey(p->data, sizeof(p->data));
	if (!ret) {
		D("Failed to get user public key\n");
		put_apacket(p);
		return;
	}

	p->msg.command = A_AUTH;
	p->msg.arg0 = ADB_AUTH_RSAPUBLICKEY;
	p->msg.data_length = ret;
	send_packet(p);
}

void Catransport::send_connect(){
	D("Calling send_connect \n");
	apacket *cp = get_apacket();
	cp->msg.command = A_CNXN;
	cp->msg.arg0 = A_VERSION;
	cp->msg.arg1 = MAX_PAYLOAD;
	cp->msg.data_length = fill_connect_data((char *)cp->data,
		sizeof(cp->data));
	send_packet(cp);
}

void Catransport::send_ready(unsigned local, unsigned remote){
	D("Calling send_ready \n");
	if (remote > 0){
		apacket *p = get_apacket();
		p->msg.command = A_OKAY;
		p->msg.arg0 = local;
		p->msg.arg1 = remote;
		send_packet(p);
	}
}

void Catransport::send_close(unsigned local, unsigned remote){
	D("Calling send_close \n");
	if (remote > 0){
		apacket *p = get_apacket();
		p->msg.command = A_CLSE;
		p->msg.arg0 = local;
		p->msg.arg1 = remote;
		send_packet(p);
	}
}

void Catransport::send_packet(apacketPtr& p){
	unsigned char *x;
	unsigned sum;
	unsigned count;
	p->msg.magic = p->msg.command ^ 0xffffffff;

	count = p->msg.data_length;

	x = (unsigned char *)p->data;
	sum = 0;
	while (count-- > 0){
		sum += *x++;
	}
	p->msg.data_check = sum;

	if (write_packet(transport_socket, p)){
		//fatal_errno("cannot enqueue packet on transport socket");
	}
}

void Catransport::parse_banner(char *banner){
	static const char *prop_seps = ";";
	static const char key_val_sep = '=';
	char *cp;
	char *type;

	D("parse_banner: %s\n", banner);
	type = banner;
	cp = strchr(type, ':');
	if (cp) {
		*cp++ = 0;
		/* Nothing is done with second field. */
		cp = strchr(cp, ':');
		if (cp) {
			char *save;
			char *key;
			key = adb_strtok_r(cp + 1, prop_seps, &save);
			while (key) {
				cp = strchr(key, key_val_sep);
				if (cp) {
					*cp++ = '\0';
					if (!strcmp(key, "ro.product.name"))
						qual_overwrite(&product, cp);
					else if (!strcmp(key, "ro.product.model"))
						qual_overwrite(&model, cp);
					else if (!strcmp(key, "ro.product.device"))
						qual_overwrite(&device, cp);
				}
				key = adb_strtok_r(NULL, prop_seps, &save);
			}
		}
	}

	if (!strcmp(type, "bootloader")){
		D("setting connection_state to CS_BOOTLOADER\n");
		connection_state = CS_BOOTLOADER;
		//update_transports();
		return;
	}

	if (!strcmp(type, "device")) {
		D("setting connection_state to CS_DEVICE\n");
		connection_state = CS_DEVICE;
		//update_transports();
		return;
	}

	if (!strcmp(type, "recovery")) {
		D("setting connection_state to CS_RECOVERY\n");
		connection_state = CS_RECOVERY;
		//update_transports();
		return;
	}
	if (!strcmp(type, "sideload")) {
		D("setting connection_state to CS_SIDELOAD\n");
		connection_state = CS_SIDELOAD;
		//update_transports();
		return;
	}
	connection_state = CS_HOST;
}

void Catransport::handle_packet(apacketPtr& p){
	print_packet("handle_packet():", p);

	switch (p->msg.command){
	case A_SYNC:
		if (p->msg.arg0){
			send_packet(p);
			//if (HOST) 
			send_connect();
		}
		else {
			connection_state = CS_OFFLINE;
			handle_offline();
			send_packet(p);
		}
		return;

	case A_CNXN: /* CONNECT(version, maxdata, "system-id-string") */
		/* XXX verify version, etc */
		if (connection_state != CS_OFFLINE) {
			connection_state = CS_OFFLINE;
			handle_offline();
		}

		parse_banner((char*)p->data);
		//PC端 HOST = 1 auth_enabled = 0
		//if (HOST || !auth_enabled) {
		if (!online){
			handle_online();
			//if (!HOST) 
			if (connection_state != CS_DEVICE){
				send_connect();
			}
		}
		else {//这里PC端注定是无作为的无需理会
			//send_auth_request(t);
		}
		break;
	case A_AUTH:
		if (p->msg.arg0 == ADB_AUTH_TOKEN) {
			key = adb_auth_nextkey(key);
			if (key) {
				send_auth_response(p->data, p->msg.data_length);
			}
			else {
				/* No more private keys to try, send the public key */
				send_auth_publickey();
			}
		}
		//这里是adbd端的处理 PC端无需理会
		//else if (p->msg.arg0 == ADB_AUTH_SIGNATURE) {
		//	////if (adb_auth_verify(t->token, p->data, p->msg.data_length)) {
		//	////	adb_auth_verified(t);
		//	////	t->failed_auth_attempts = 0;
		//	////}
		//	////else {
		//	////	if (t->failed_auth_attempts++ > 10)
		//	////		adb_sleep_ms(1000);
		//	////	send_auth_request(t);
		//	////}
		//}
		//else if (p->msg.arg0 == ADB_AUTH_RSAPUBLICKEY) {
		//	adb_auth_confirm_key(p->data, p->msg.data_length, t);
		//}
		break;
	case A_OPEN: /* OPEN(local-id, 0, "destination") */  //创建设备通讯socket
		//这里是adbd端处理 PC端忽略
		break;
	case A_OKAY: {/* READY(local-id, remote-id, "") */
		//pc端:p->msg.arg0:是remote_id, p->msg.arg1:local_id
		aclientPtr bip = client_get_ptr(p->msg.arg1);
		if (online) {//先处理掉通信
			if (bip){
				send_ready(p->msg.arg1, p->msg.arg0);
			}
			else{//通道打开失败 关闭通道
				send_close(0/*p->msg.arg1*/, p->msg.arg0);
			}
		}
		if (bip){//然后修改标志
			bip->remote_id = p->msg.arg0;
			bip->connected = 1;
			bip->notify();//通知有数据变动
		}
		break;}
	case A_CLSE:{ /* CLOSE(local-id, remote-id, "") */
		//pc端:p->msg.arg0:是remote_id, p->msg.arg1:local_id
		//通道传递完成
		if (online) {
			send_close(0/*p->msg.arg1*/, p->msg.arg0);
		}
		//修改标志
		aclientPtr bip = client_get_ptr(p->msg.arg1);
		if (bip){
			bip->biptype = bipbuffer_close;
			bip->remote_id = 0;//链接已关闭
			bip->connected = 0;//通道断开
			bip->notify();//通知有数据变动
		}
		break; }
	case A_WRTE:
		//pc端:p->msg.arg0:是remote_id, p->msg.arg1:local_id
		if (online) {
			unsigned  rid = p->msg.arg0,
					  lid = p->msg.arg1;
			if (local_client_enqueue(p->msg.arg0, p->msg.arg1, p) == 0){//  local_client_enqueue 之后 p 已经传递 不能再用
				send_ready(lid, rid);	//正常为接收前发送 send_ready  如果是 closing 则这里直接返回
			}
			return;
		}
		break;

	default:
		printf("handle_packet: what is %08x?!\n", p->msg.command);
	}
	put_apacket(p);
}

/**************************************************************/
/****				数据传输本地与用户端交互			*******/
/**************************************************************/
int Catransport::local_client_enqueue(int remote_arg0, int local_arg1, apacketPtr& p){
	//pc端:p->msg.arg0:是remote_id, p->msg.arg1:local_id
	aclientPtr bip = client_get_ptr(local_arg1);
	if (bip)
	{
		void* temp = p;
		long len = p->msg.data_length;	//数据长度
		while (!bip->ptrlist.push(temp)){
			if (kicked || !bip->user){ 
				put_apacket(p);
				if (!kicked){ send_close(0, remote_arg0); }
				bip->notify();//通知有数据变动
				return -1; }
		}
		p = NULL;
		bip->remote_id = remote_arg0;			//记录当前设备传输通道
		bip->notify();//通知有数据变动
		//if (bip->closing == 1){
		//	return 0;
		//}
		return 1;
	}else{//本地通道已关闭 关闭设备通道
		put_apacket(p);
		send_close(0, remote_arg0);
		return -1;
	}
}

int Catransport::smart_local_remote_enqueue(int local_id, const char *destination)	//打开设备通许通道
{
	unsigned len = strlen(destination);
	char *service = NULL;
	char* serial = NULL;
	transport_type ttype = kTransportAny;

	if (connection_state != CS_DEVICE){//设备尚未链接
		t_error = A_ERROR_UN_DEVICE;
		return -1;
	}

	/* don't bother if we can't decode the length */
	if (len < 4) return 0;

	if ((len < 1) || (len > 1024)) {
		D("SS(%d): bad size (%d)\n", local_id, len);
		goto fail;
	}
	if (len > MAX_PAYLOAD){
		D("SS(%d): overflow\n", local_id);
		goto fail;
	}

	D("SS(%d): '%s'\n", local_id, destination);

	service = (char*)destination;
	if (!strncmp(service, "host-serial:", strlen("host-serial:"))) {
		service += strlen("host-serial:");
	}
	else if (!strncmp(service, "host-usb:", strlen("host-usb:"))) {
		ttype = kTransportUsb;
		service += strlen("host-usb:");
	}
	else if (!strncmp(service, "host-local:", strlen("host-local:"))) {
		ttype = kTransportLocal;
		service += strlen("host-local:");
	}
	else if (!strncmp(service, "host:", strlen("host:"))) {
		ttype = kTransportAny;
		service += strlen("host:");
	}
	else {
		service = NULL;
	}

	if (service) {
		//这个是链接 设备 的指令 无需再去链接，这里本身就已经链接了
		//t_error = A_ERROR_NONE;
		return 0;
	}

	connect_to_remote(local_id, destination);
	aclientPtr a = client_get_ptr(local_id);
	if (a){
		if (a->connected == 0){ goto fail; }
	}
	return 0;

fail:
	return -1;
}

void Catransport::connect_to_remote(int local_id, const char *destination)		//对接本地与设备通道
{
	D("Connect_to_remote call RS(%d)\n", local_id);
	apacket *p = get_apacket();
	int len = strlen(destination);

	if ((len + 1) > MAX_PAYLOAD) {
		D("destination oversized\n");
	}
	if (p){
		sprintf_s((char*)p->data, MAX_PAYLOAD, "%s", destination);
		len = strlen((char*)p->data) + 1;
		p->msg.command = A_OPEN;
		p->msg.arg0 = local_id;
		p->msg.data_length = len;
		send_packet(p);
		//等待连接返回
		aclientPtr a = client_get_ptr(local_id);
		if (a){
			a->wait();
		}
	}
}

int Catransport::local_remote_enqueue(int local_id, apacketPtr& p)		//本地发送到设备数据 A_WRAE
{
	D("entered local_remote_enqueue WRITE local_id=%d\n",
		local_id);
	aclientPtr a = client_get_ptr(local_id);
	if (!a || a->biptype == bipbuffer_close || kicked){
		put_apacket(p);
		return -1;
	}
	p->msg.command = A_WRTE;
	p->msg.arg0 = local_id;
	p->msg.arg1 = a->remote_id;
	p->msg.data_length = p->len;
	send_packet(p);
	//a->wait();//等待设备返回信息
	return 0;
}

int	Catransport::local_client_type_set(int _type, int local_id)		//标志通讯类型
{
	aclientPtr a = client_get_ptr(local_id);
	if (!a){ return -1; }

	int _utype = a->biptype;
	//a->notify();
	if (_utype == bipbuffer_close){
		//关闭状态 则直接赋值
	}else{
		while (a->remote_id == 0 && (a->biptype != bipbuffer_close)){//已经发送过消息出去 那么要等待消息返回
			if (kicked){ t_error = A_ERROR_OFFLINE; return -1; }
			a->wait();//adb_sleep_ms(10);
		}
		if (a->remote_id > 0){
			a->closing = 1;
			send_ready(local_id, a->remote_id);
			send_close(0/*local_id*/, a->remote_id);
		}
		while (a->biptype != bipbuffer_close){
			if (kicked){ t_error = A_ERROR_OFFLINE; return -1; }
			a->wait();//adb_sleep_ms(10);
		}
	}
	client_clear(a);
	a->closing = 0;
	a->biptype = _type;
	return 0;
}

/**************************************************************/
/****				用户交互时调用						*******/
/**************************************************************/

int Catransport::shell_send(const char *destination, int local_id){
	int shellhead = 0;
	int _utype = -1;
	int len = strlen(destination), slen = strlen("shell:");

	if (kicked){ return -1; }

	if (connection_state != CS_DEVICE){//设备尚未链接
		//local_socket_faile_notify("now did't connect to device", (int)userbip);
		return -1;
	}
	if (len >= slen){//长度不足无需比较
		if (!strncmp(destination, "shell:", slen)) {//是shell 开头 判断是同步 还是交互
			_utype = bipbuffer_shell_sync;
			shellhead = 1;
			if (len == slen) {//是shell交互
				_utype = bipbuffer_shell_async;
			}
		}
	}
	//都不是那么保持 不变
	if (_utype > -1){
		if (local_client_type_set(_utype, local_id)){//关闭以前可能打开的其他通道
			return -1;
		}
	}

	if (len < MAX_PAYLOAD){
		aclientPtr a = client_get_ptr(local_id);
		if (!a){ return -1; }
		////转成UTF8试试
		//sstring	source_comm(destination);
		//sstring comm_utf8 = boost::locale::conv::to_utf<char>(source_comm, "UTF-8");
		if (shellhead)
		{
			if (a->biptype != bipbuffer_shell_async && a->biptype != bipbuffer_shell_sync){//不是shell模式则 不执行
				//local_socket_faile_notify("not run shell:\n", (int)userbip);
				return -1;
			}
			return smart_local_remote_enqueue(local_id, destination);
			//return smart_local_remote_enqueue(local_id, comm_utf8.c_str());//destination
		}
		else
		{
			apacket *p = get_apacket();
			if (a->biptype != bipbuffer_shell_async){//不是交互模式则 不执行
				//local_socket_faile_notify("not run shell:\n", (int)userbip);
				return -1;
			}
			sprintf_s((char*)p->data, MAX_PAYLOAD, "%s\n", destination);
			//sprintf_s((char*)p->data, MAX_PAYLOAD, "%s\n", comm_utf8);//destination
			p->len = strlen((char*)p->data) + 1;
			return local_remote_enqueue(local_id, p);
		}
	}
	return -1;
}

int Catransport::shell_recv(sstring &ret, int local_id, int wait_times /*= 0*/){
	void *_dst = NULL;
	aclientPtr bip = client_get_ptr(local_id);
	int succ = 0,
		endptr = 0,
		remote_id = 0;
	time_t maxtime = time(NULL) + wait_times;
	char* endchar = NULL;
	bool isAddChar = false;

	if (!bip){ return -1; }
	remote_id = bip->remote_id;
	do{
		if (kicked){ break; }

		send_ready(local_id, remote_id);	//通知设备可以发送数据了

		if (bip->ptrlist.pop(_dst)){//非空情况下 获取 数据
			maxtime = time(NULL) + wait_times;//时间重置
			apacket* ap = (apacket*)_dst;
			_dst = NULL;
			if (ap){
				ret.append((char*)ap->data, ap->msg.data_length);
				put_apacket(ap);
				if (bip->biptype == bipbuffer_shell_async){
					isAddChar = true;	//交互模式检测打开
					endptr = ret.length() - 5;
				}
			}
		}else if (bip->biptype != bipbuffer_shell_sync && bip->biptype != bipbuffer_shell_async){//关闭则退出
			succ = 1;
		}
		else if (isAddChar)
		{
			isAddChar = false;//交互模式检测关闭
			if (endptr >= 0){
				endchar = &ret[endptr];	//最后5个字符找# $
				while (*endchar){
					if (*endchar == '#' || *endchar == '$'){ 	//最后一个字符 是否是 # 或者 $
						succ = 1;
						break;
					}
					++endchar;
				}
			}
		}else{
			if (!succ){//无数据 没完成则等待
				bip->wait(wait_times);
			}
		}
		if (kicked){ break; }
		if (wait_times){
			if (maxtime < time(NULL)){ succ = 1; }//超时
		}
	} while (!succ);
	D("shell_recv ok\n");
	return 0;
}

long Catransport::client_read(unsigned char* retData, bufferstream* buffer, long len, aclientPtr local, int wait_times/* = 0*/)	//从通道中读取指定长度到数据
{
	aclientPtr userbip = local;
	if (!userbip){ return -1; }
	if (!retData && !buffer){ return -1; }//没有接收空间

	apacket* retLastApacket = (apacket*)userbip->last_apacket;
	int local_id = userbip->local_id,
		remote_id = userbip->remote_id;
	//每次读取返回一个包的数据 直到符合数据长度
	long length = 0,readlen = len; 
	int	 clen = 0;
	time_t maxtimes = time(NULL) + wait_times;
	unsigned char* _des = retData;	//地址计数

	//adb_sleep_ms(10);//延迟 等待数据返回
	//开始读取数据
	if (retLastApacket){
		if (retLastApacket->ptr == NULL){
			retLastApacket->ptr = retLastApacket->data;
			retLastApacket->len = retLastApacket->msg.data_length;
		}
	}
	while (readlen > 0 && (userbip->biptype != bipbuffer_close) && (kicked == 0))
	{
		if (retLastApacket){
			if (retLastApacket->len == 0){
				put_apacket(retLastApacket);
				retLastApacket = NULL;
				userbip->last_apacket = NULL;
			}
			else{
				clen = (readlen > retLastApacket->len) ? retLastApacket->len : readlen;
				if (_des){
					memcpy(_des, retLastApacket->ptr, clen);
					_des += clen;
				}else{
					buffer->insert(buffer->end(), retLastApacket->ptr, retLastApacket->ptr + clen);
				}
				readlen -= clen;
				retLastApacket->ptr += clen;
				retLastApacket->len -= clen;
				length += clen;
			}
			maxtimes = time(NULL) + wait_times;
		}
		else
		{
			send_ready(local_id, remote_id);	//通知设备发送数据
			if (userbip->ptrlist.pop(retLastApacket)){
				if (retLastApacket){
					userbip->last_apacket = retLastApacket;//保存最新的包
					retLastApacket->ptr = retLastApacket->data;
					retLastApacket->len = retLastApacket->msg.data_length;
				}
				maxtimes = time(NULL) + wait_times;
			}else{
				//没有数据时等待wait_times时间
				userbip->wait(wait_times);
			}
		}
		if (wait_times){
			if (maxtimes < time(NULL)){
				break;
			}
		}
	}
	return length;
}

int Catransport::client_write(void* sendData, long len, int local_id)		//向设备发送指定长度数据
{
	int rlen = len, slen = 0;
	unsigned char* ptr = (unsigned char*)sendData;
	apacket* p = NULL;

	while (rlen > 0)
	{
		slen = ((rlen > MAX_PAYLOAD) ? MAX_PAYLOAD : rlen);
		p = get_apacket();
		if (p){
			p->len = slen;
			memcpy(p->data, ptr, slen);
			if (local_remote_enqueue(local_id, p)){ //发送失败则退出
				return -1;
			}
			ptr += slen;
			rlen -= slen;
		}
	}
	return 0;
}

/***************************forward***********************************/
int Catransport::forward_connect(const char* remote_connect, int local_id){
	aclientPtr userbip = client_get_ptr(local_id);
	if (!userbip || (*remote_connect == 0)){ return -1; }

	if (connection_state != CS_DEVICE){//设备尚未链接
		t_error = A_ERROR_UN_DEVICE;
		return -1;
	}

	if (local_client_type_set(bipbuffer_forward, local_id)){//关闭以前可能打开的其他通道
		return -1;
	}

	apacket* p = (apacket*)userbip->last_apacket;
	userbip->last_apacket = NULL;
	if (p){ put_apacket(p); }

	connect_to_remote(local_id, remote_connect);
	if (userbip->connected){ 
		return 0; }
	return -1;
}

int Catransport::forward_write(void* sendData, long len, int local_id)		//发送数据
{
	aclientPtr a = client_get_ptr(local_id);
	if (!a){ return -1; }
	if (a->biptype != bipbuffer_forward){
		return -1;
	}
	return client_write(sendData, len, local_id);
}

//返回0表示读取完成 尾部变参返回最后一个包的指针保留未读完的数据 最长等待时间(秒)
int Catransport::forward_read(unsigned char* retData, long len, int local_id, int wait_times /*= 0*/){
	aclientPtr a = client_get_ptr(local_id);
	if (!a){ return -1; }
	if (a->biptype != bipbuffer_forward){
		return -1;
	}
	if (client_read(retData, NULL, len, a, wait_times) == len){
		return 0;
	}
	return -1;
}

/*************************sync 文件传递*************************/
int Catransport::sync_connect(int local_id){
	char* remote_connect = "sync:";
	aclientPtr userbip = client_get_ptr(local_id);
	if (!userbip || (*remote_connect == 0)){ return -1; }

	if (connection_state != CS_DEVICE){//设备尚未链接
		t_error = A_ERROR_UN_DEVICE;
		return -1;
	}

	if (local_client_type_set(bipbuffer_file_sync, local_id)){//关闭以前可能打开的其他通道
		return -1;
	}

	apacket* p = (apacket*)userbip->last_apacket;
	userbip->last_apacket = NULL;
	if (p){ put_apacket(p); }

	connect_to_remote(local_id, remote_connect);
	if (userbip->connected){
		return 0;
	}
	return -1;
}

int Catransport::sync_write(void* sendData, long len, int local_id){
	aclientPtr a = client_get_ptr(local_id);
	if (!a){ return -1; }
	if (a->biptype != bipbuffer_file_sync){
		return -1;
	}
	return client_write(sendData, len, local_id);
}

//返回一个长度， 尾部变参返回最后一个包的指针保留未读完的数据 最长等待时间(秒)
int Catransport::sync_read(unsigned char* retData, long len, int local_id, int wait_times /*= 0*/){
	aclientPtr a = client_get_ptr(local_id);
	if (!a){ return -1; }
	if (a->biptype != bipbuffer_file_sync){
		return -1;
	}
	if (client_read(retData, NULL, len, a, wait_times) == len){
		return 0;
	}
	return -1;
}

//返回一个长度， 尾部变参返回最后一个包的指针保留未读完的数据 最长等待时间(秒)
int Catransport::sync_read_to_buffer(bufferstream &buffer, long len, int local_id, int wait_times /*= 0*/){
	aclientPtr a = client_get_ptr(local_id);
	if (!a){ return -1; }
	if (a->biptype != bipbuffer_file_sync){
		return -1;
	}
	if (client_read(NULL, &buffer, len, a, wait_times) == len){
		return 0;
	}
	return -1;
}

/**************************************************************/
/****				服务操作					***************/
/**************************************************************/
bool Catransport::Start()	//启动服务通讯
{
	adb_mutex_lock	_slock(start_mutex);
	adb_thread_t user_thread_ptr,
				output_thread_ptr,
				input_thread_ptr;
	if (ref_count){ //已经启动了就无需再启动
		return true; 
	}
	if (connection_state != CS_NOPERM) {
		/* initial references are the two threads */

		pair = adb_socketpair();
		if (!pair) {
			//fatal_errno("cannot open transport socketpair");
			return false;
		}

		D("transport: %s (%d,%d) starting\n", serial, fd, sfd);

		int evID = pair->a_fd;
		transport_socket = evID;
		fd = evID + 1;

		if (!atp){
			atp = (void*)new atsweakPtr(shared_from_this());
		}
		if (!atp){ return false; }

		if (adb_thread_create(&input_thread_ptr, input_thread, atp)){
			D("cannot create input thread");
			delete atp;
			return false;
		}
		++ref_count;

		if (adb_thread_create(&output_thread_ptr, output_thread, atp)){
			D("cannot create output thread");
			delete atp;
			return false;
		}
		++ref_count;

		if (adb_thread_create(&user_thread_ptr, user_thread, atp)){
			D("cannot create input thread");
			delete atp;
			return false;
		}
		++ref_count;


		return true;
	}
	return false;
}

char* Catransport::connection_state_name(){
	switch (connection_state) {
	case CS_BOOTLOADER:
		return "bootloader";
	case CS_DEVICE:
		return "device";
	case CS_OFFLINE:
		return "offline";
	default:
		return "unknown";
	}
}

/**************************************************************/
/****				线程处理					***************/
/**************************************************************/

int Catransport::read_from_remote(apacket *p)
{
	switch (type)
	{
	case kTransportUsb:{	//usb通讯
		return remote_usb_read(p);
	}
	case kTransportLocal:	//虚拟设备socket通讯
		break;
	case kTransportAny:
		break;
	case kTransportHost:
		break;
	default:
		break;
	}
	return -1;
}

int Catransport::write_to_remote(apacket *p)
{
	switch (type)
	{
	case kTransportUsb:{	//usb通讯
		return remote_usb_write(p);
	}
	case kTransportLocal:	//虚拟设备socket通讯
		break;
	case kTransportAny:
		break;
	case kTransportHost:
		break;
	default:
		break;
	}
	return -1;
}
/**************************************************************/
/****				通道传输					***************/
/**************************************************************/

int	Catransport::read_packet(int f, apacketPtr& packet)
{
	switch (type)
	{
	case kTransportUsb:{	//usb通讯
		return usb_read_packet(f, packet);
	}
	case kTransportLocal:	//虚拟设备socket通讯
		break;
	case kTransportAny:
		break;
	case kTransportHost:
		break;
	default:
		break;
	}
	return -1;
}

int	Catransport::write_packet(int f, apacketPtr& packet)
{
	int ret = -1;
	switch (type)
	{
	case kTransportUsb:{	//usb通讯
		ret = usb_write_packet(f, packet);
		break;
	}
	case kTransportLocal:	//虚拟设备socket通讯
		break;
	case kTransportAny:
		break;
	case kTransportHost:
		break;
	default:
		break;
	}

	//通知 接收端接收
	if (f == fd){//这个是从设备接收到数据 要发送给 server 处理的 //通知 read_packet(transport_socket) 接收
		d_read_ctrol.Signal();
	}else{//否则就是 用户 发送 给 设备的数据 //通知 read_packet(fd) 接收
		d_write_ctrol.Signal();
	}
	return ret;
}

int Catransport::client_get_id()	//通讯通道申请
{
	//遍历通道表 将关闭的通道打开返回ID号
	int id = 0;
	bip_write_lock wlick(bip_lock);
	int nCount = userlist.size();
	if (nCount){
		aclientPtr* ap = &userlist[0];
		aclientPtr	a = NULL;
		for (int i = 0; i < nCount; ++i){
			a = *ap;
			if (a){
				if (!a->user){//通道无人使用则打开使用
					client_init(a);
					a->user = 1;
					id = client_id_base + i;
					a->local_id = id;
					break;
				}
			}else{//空则新增申请
				aclientPtr ac = new aclient();
				if (ac){
					ac->user = 1;
					(*ap) = ac;
					id = client_id_base + i;
					ac->local_id = id;
				}
				break;
			}
			++ap;
		}
	}
	return id;
}

void Catransport::client_free_id(int _id)	//通道关闭
{
	bip_write_lock wlick(bip_lock);
	int nCount = userlist.size();
	int id = _id - client_id_base;
	if (id < 0){ return; }
	if (id < nCount){
		aclientPtr a = userlist[id];
		if (a){
			client_init(a);
			a->notify();
		}
	}
}

aclientPtr	Catransport::client_get_ptr(int _id)	//获取通道对象指针
{
	bip_read_lock rlick(bip_lock);
	int nCount = userlist.size();
	int id = _id - client_id_base;
	aclientPtr a = NULL;
	if (id > -1){ 
		if (id < nCount){
			a = userlist[id];
			if (a){ 
				if (!a->user){ a = NULL; }
			}
		}
	}
	return a;
}

void	Catransport::client_init(aclientPtr a){
	if (a){
		client_clear(a);
		a->user = 0;
		a->biptype = bipbuffer_close;
		a->connected = 0;
		a->remote_id = 0;
		a->local_id = 0;
		a->closing = 0;
	}
}

void	Catransport::client_clear(aclientPtr a)			//清空数据
{
	if (a){
		apacketPtr p = NULL;
		bip_apacke_del_all(a->last_apacket);
		a->last_apacket = NULL;
		while (a->ptrlist.pop(p)){//适当回收
			put_apacket(p);}
		a->re_init();
	}
}

void	Catransport::client_removeAll()	//释放所有对象
{
	aclientPtr a = NULL;
	bip_write_lock wlick(bip_lock);

	for (size_t i = 0; i < userlist.size(); ++i){
		a = userlist[i];
		if (a){ 
			delete a; 
		}
	}
	userlist.clear();
}

/**************************************************************/
/****				数据处理					***************/
/**************************************************************/

void Catransport::wait_for_usb_read_packet(int f, BipBuffer bip)
{
	//通过时间片来循环待机 
	if (kicked == 0 && bip->closed == 0 && bip->ptrlist->empty()){
		if (f == fd){//这个是接收用户端 等待 write_packet(transport_socket) 发送数据
			d_write_ctrol.Wait();
		}else{//否则就是 接收 从设备读取到 等待 write_packet(fd) 发送数据
			d_read_ctrol.Wait();
		}
	}
}

int	Catransport::usb_read_packet(int f, apacketPtr& packet){
	if (pair && !kicked) {
		BipBuffer   bip;
		//transport_socket = pair->a_fd 
		if (f == pair->a_fd){
			bip = &pair->b2a_bip;
		}else{
			bip = &pair->a2b_bip;
		}
		//通过信号 等待数据送达
		int ret = 0;
		void* p = NULL;
		while (pair && !kicked)
		{
			if (bip->closed){
				ret = -1; break;
			}
			if (bip->ptrlist->empty()){//如果是空的那么等待
				wait_for_usb_read_packet(f, bip);
			}
			if (bip->ptrlist->pop(p)){
				break;
			}
		}
		//if (adb_read(f, pair, p)) {//返回非0为错
		//	return -1;
		//}
		put_apacket(packet);
		packet = (apacket*)p;
		if (!packet){ return -1; }
	}
	else {
		return -1;
	}
	return 0;
}

int	Catransport::usb_write_packet(int f, apacketPtr& packet){
	if (pair && packet && !kicked){
		void* p = (void*)packet;
		BipBuffer   bip;
		if (f == pair->a_fd){
			bip = &pair->a2b_bip;
		}else{
			bip = &pair->b2a_bip;
		}	

		int ret = -1;
		while (pair && !kicked)
		{
			if (bip->closed){
				put_apacket(packet);
				break;
			}
			if (bip->ptrlist->push(p)){
				ret = 0;
				break;
			}
		}
		packet = NULL;
		return ret;
	}
	else{
		put_apacket(packet);
		return -1;
	}
}

int Catransport::remote_usb_read(apacket* p)
{
	if(!usb){ return -1; }
	if (!p){ return 0; }
	if (usb_read(usb, &p->msg, sizeof(amessage))){
		D("remote usb: read terminated (message)\n");
		return -1;
	}

	if (check_header(p)) {
		D("remote usb: check_header failed\n");
		return -1;
	}

	if (p->msg.data_length) {
		if (usb_read(usb, p->data, p->msg.data_length)){
			D("remote usb: terminated (data)\n");
			return -1;
		}
	}

	if (check_data(p)) {
		D("remote usb: check_data failed\n");
		return -1;
	}

	return 0;
}

int Catransport::remote_usb_write(apacket* p)
{
	if (!usb){ return -1; }
	if (!p){ return 0; }
	unsigned size = p->msg.data_length;

	if (usb_write(usb, &p->msg, sizeof(amessage))) {
		D("remote usb: 1 - write terminated\n");
		return -1;
	}
	if (p->msg.data_length == 0) return 0;
	if (usb_write(usb, &p->data, size)) {
		D("remote usb: 2 - write terminated\n");
		return -1;
	}

	return 0;
}