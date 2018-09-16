#include "CAdb.h"
#include "Catransport.h"//设备对象

#define atran_def(t)	atransportPtr *p = (atransportPtr*)m_atransport;	\
						atransportPtr& t = *p

CAdb::CAdb(const char* _name_serial)
	:name_serial(_name_serial), isok(false), m_atransport(NULL), m_local_id(NULL)
{
	atransportPtr* t = new atransportPtr(get_device(name_serial.c_str()));
	atransportPtr& temp = *t;
	if (temp)
	{
		if (temp->kicked){ //已经剔除无法使用
			temp.reset();
			delete t; 
			return; 
		}
		if (!temp->Start()){//启动设备通讯线程
			temp.reset();
			delete t;
			return;	//通讯启动失败
		}
		m_local_id = temp->client_get_id();
		if (m_local_id < client_id_base){//通道申请失败
			temp.reset();
			delete t;
			return;
		}
		++(temp->connectCount);	//链接数计数
		isok = true;
		m_atransport = (uint32_t)t;
	}
}

CAdb::CAdb(CAdb& parent) //创建子级
	:name_serial(parent.get_serial()), isok(false), m_atransport(NULL), m_local_id(NULL)
{
	atransportPtr* p = (atransportPtr*)parent.m_atransport;
	if (!p){ return; }
	atransportPtr* t = new atransportPtr(*p);//创建引用
	atransportPtr& temp = *t;
	if (temp)
	{
		if (temp->kicked){ //已经剔除无法使用
			temp.reset();
			delete t; 
			return; 
		}
		//既然是引用就无需判断启动了
		m_local_id = temp->client_get_id();
		if (m_local_id < client_id_base){//通道申请失败
			temp.reset();
			delete t;
			return;
		}
		++(temp->connectCount);	//链接数计数
		isok = true;
		m_atransport = (uint32_t)t;
	}
}

CAdb::~CAdb()
{
	int nCount = -1;
	if (!m_atransport){
		return;
	}
	atransportPtr* t = (atransportPtr*)m_atransport;
	atransportPtr& temp = *t;
	m_atransport = NULL;
	if (temp)
	{
		Close();
		//关闭当前链接的通道
		temp->client_free_id(m_local_id);
		--(temp->connectCount);
	}
	temp.reset();
	delete t;
}

void CAdb::InitAdb(){
	adb_auth_init();
	usb_vendors_init();
	devices_list.clear();
}

void CAdb::RepleaseAdb(){
	remove_device_All();
	checked_devices();
	remove_device_All();
}

void CAdb::FindDevices(){
	checked_devices();
	find_devices();
}

void CAdb::GetDevicesNameList(sstring& ret){
	get_device_name_list(ret);
}

bool CAdb::IsOk()
{
	return isok;
}

bool CAdb::IsUsbClose(){
	if (!m_atransport){ return true; }
	atran_def(t);
	if (t){
		return (t->kicked != 0);//usb设备已经断开
	}
	return true;
}

void CAdb::UsbClose(){
	if (!m_atransport){ return; }
	atran_def(t);
	if (t){
		Close();
		t->Clear();
	}
}

bool CAdb::IsClose(){
	if (!IsUsbClose()){//没断开判断其 通道是否已经关闭
		atran_def(t);
		if (t){
			aclientPtr a = t->client_get_ptr(m_local_id);
			if (a){
				return (a->biptype == bipbuffer_close);
			}
		}
	}
	return true;
}

bool CAdb::Close(){
	if (!m_atransport){ return false; }
	atran_def(t);
	if (t){
		return (t->local_client_type_set(bipbuffer_close, m_local_id) == 0);
	}
	return false;
}

bool CAdb::IsOnline(){
	if (!m_atransport){ return false; }
	atran_def(t);
	if (t){
		return (t->connection_state == CS_DEVICE && t->online != 0);
	}
	return false;
}

sstring& CAdb::get_serial(){
	return name_serial;
}

bool CAdb::Shell_send(const char* data){
	int ret = -1;
	if (!m_atransport){ return false; }
	atran_def(t);
	if (t && data[0]){
		ret = t->shell_send(data, m_local_id);
	}
	return (!ret);
}

bool CAdb::Shell_recv(sstring& ret, int wait_time /*= 0*/){
	int r = -1;
	if (!m_atransport){ return r; }
	atran_def(t);
	if (t){
		r = t->shell_recv(ret, m_local_id, wait_time);
	}
	return (!r);
}

bool CAdb::forwardconnect(const char* data){
	int ret = -1;
	if (!m_atransport){ return ret; }
	atran_def(t);
	if (t){
		ret = t->forward_connect(data, m_local_id);
	}
	return (ret == 0);
}

bool CAdb::forwardwrite(unsigned char* data, int len){
	int ret = -1;
	if (!data || len < 1){
		return ret;
	}
	if (!m_atransport){ return ret; }
	atran_def(t);
	if (t){
		ret = t->forward_write(data, len, m_local_id);
	}
	return (ret == 0);
}

bool CAdb::forwardread(unsigned char* data, int len, int wait_times /*= 0*/){
	int ret = -1;
	if (!data || len < 1){
		return ret;
	}
	if (!m_atransport){ return ret; }
	atran_def(t);
	if (t){
		ret = t->forward_read(data, len, m_local_id, wait_times);
	}
	return (ret == 0);
}

bool CAdb::forwarddisconnect(){
	int ret = 0;
	if (!m_atransport){ return ret; }
	atran_def(t);
	if (t){
		ret = t->forward_disconnect(m_local_id);
	}
	return (ret == 0);
}

void CAdb::forwardTest(){
	//"shell:LD_LIBRARY_PATH=/data/local/tmp /data/local/tmp/ctest -Q 100 -P 720x1280@720x1280/0 -S -n ctest"
	//"localabstract:ctest"
	CAdb adbforward(*this), adbshell(*this);

	const char* shellcomm = "shell:LD_LIBRARY_PATH=/data/local/tmp /data/local/tmp/ctest -Q 100 -P 720x1280@720x1280/0 -S -n ctest";
	const char* forwardcomm = "localabstract:ctest";

	if (adbshell.Shell_send(shellcomm))
	{
		sstring retss;
		adbshell.Shell_recv(retss,1);
		printf(retss.c_str());
		if (adbforward.forwardconnect(forwardcomm) == 1)
		{
			unsigned char buff[8192];
			int nlen = adbforward.forwardread(buff, 8192);
			printf((char*)buff);
			adbforward.forwarddisconnect();
		}
	}
}

bool  CAdb::sync_push(const char* lpath, const char* rpath){
	int ret = -1;
	if (!m_atransport){ return (ret == 0); }
	atran_def(t);
	if (t){
		ret = t->sync_push(lpath, rpath, NULL, m_local_id);
	}
	return (ret == 0);
}
bool  CAdb::sync_pull(const char* rpath, const char* lpath){
	int ret = -1;
	if (!m_atransport){ return (ret == 0); }
	atran_def(t);
	if (t){
		ret = t->sync_pull(rpath, lpath, m_local_id);
	}
	return (ret == 0);
}

bool   CAdb::sync_push_buffer(unsigned char* buffer, size_t len, const char* rpath){
	int ret = -1;
	if (!m_atransport){ return (ret == 0); }
	atran_def(t);
	if (t){
		ret = t->sync_push_buffer(buffer, len, rpath, m_local_id);
	}
	return (ret == 0);
}

bool   CAdb::sync_pull_buffer(bufferstream &buffer, size_t &len, const char* rpath){	//return is the recv length
	int ret = -1;
	if (!m_atransport){ return (ret == 0); }
	atran_def(t);
	if (t){
		ret = t->sync_pull_buffer(buffer, len, rpath, m_local_id);
	}
	return (ret == 0);
}