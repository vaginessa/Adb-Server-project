#ifndef _CADB_H_
#define _CADB_H_

#include <string>
#include <vector>
#include <boost/atomic.hpp>	//原子操作

typedef	std::string					sstring;
typedef std::vector<unsigned char>	bufferstream;

class CAdb
{
public:
	CAdb(const char* _name_serial);
	CAdb(CAdb& parent);	//创建子级
	~CAdb();
	static void InitAdb();
	static void RepleaseAdb();
	static void FindDevices();
	static void GetDevicesNameList(sstring& ret);
	bool IsOk();
	bool IsUsbClose();
	void UsbClose();
	bool IsClose();
	bool Close();
	bool IsOnline();
public:
	sstring& get_serial();
public:
	bool Shell_send(const char* data);
	bool Shell_recv(sstring& ret, int wait_time = 0);
public:
	bool forwardconnect(const char* data);
	bool forwardwrite(unsigned char* data, int len);
	bool forwardread(unsigned char* data, int len, int wait_times = 0);
	bool forwarddisconnect();
	void forwardTest();
public:
	bool  sync_push(const char* lpath, const char* rpath);
	bool  sync_pull(const char* rpath, const char* lpath);
	bool  sync_push_buffer(unsigned char* buffer, size_t len, const char* rpath);
	bool  sync_pull_buffer(bufferstream &buffer, size_t &len, const char* rpath);	//return is the recv length
public:
	int			m_local_id;		//链接设备通信时本地通道ID
	uint32_t	m_atransport;	//设备对象句柄
	bool		isok;
private:
	sstring		name_serial;
};

#endif	//_CADB_H_