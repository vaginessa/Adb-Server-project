#ifndef _CATRANSPORT_H_
#define _CATRANSPORT_H_

#include "adb_func.h"
#include "Semaphore.h"
#include <boost/thread/shared_mutex.hpp>
#include <boost/enable_shared_from_this.hpp>

//本类目的是借助 智能指针 自动管理内存。最后退出的自动关闭USB通道清理所有内存
//通过变量kicked 判断是否已经断开

class Semaphore;	//预声明
//通道获取时使用的 读写锁
typedef boost::shared_mutex								bip_shared_mutex;
typedef boost::shared_lock<bip_shared_mutex>			bip_read_lock;
typedef boost::unique_lock<bip_shared_mutex>			bip_write_lock;

typedef struct aclient{
	_aInt		user,			//是否有用户在使用本通道
				biptype,		//执行类型
				local_id,		//本通道ID
				remote_id,		//与本地通讯的设备通道ID
				connected,		//连接状态
				closing;		//正在关闭
	void*		last_apacket;	//同步 和 forward 读取数据需要
	BipPtrList	ptrlist;		//无锁队列 用于 传递 数据

	aclient() :last_apacket(NULL), biptype(bipbuffer_close), connected(NULL), closing(NULL), local_id(NULL), remote_id(NULL), user(NULL), ptrlist(0){}
	~aclient(){ //释放队列
		bip_apacke_del_all(last_apacket);
		ptrlist.consume_all(bip_apacke_del_all);
	}
	inline void notify(size_t n = 1){ client_ctrol.Signal(n); }
	void wait(int w_sec = NULL){//等待数据接收 时间是秒  /*, [&](){return (!ptrlist.empty() || (user == 0)); }*/  //[&](){return (!ptrlist.empty();}
		if (w_sec){
			client_ctrol.Wait_time(w_sec);
		}else{
			client_ctrol.Wait();
		}
	}
	inline void re_init(){ client_ctrol.reset(); }
private:
	Semaphore	client_ctrol;	//信号量
}aclient, *aclientPtr;

typedef std::vector<aclientPtr>							client_list;	//用户通道表
#define	client_list_max									32				//用户通道表最大值
#define client_id_base									100				//用户通道ID号100起步


//错误代号
enum atransportError
{
	A_ERROR_NONE,			//无错误
	A_ERROR_OFFLINE,		//设备掉线
	A_ERROR_UN_DEVICE,		//设备尚未通过认证
	A_ERROR_CLIENT_CLOSE,	//用户通道关闭
};

typedef struct copyinfo
{
	copyinfo *next;
	const char *src;
	const char *dst;
	unsigned int time;
	unsigned int mode;
	unsigned int size;
	int flag;
	//char data[0];
}copyinfo;

class Catransport
	:public boost::enable_shared_from_this<Catransport>
{
//======================设备通讯数据========================
public:
	SocketPair	pair;	//传输所用的内存指针

	int fd;
	int transport_socket;
	_aInt connectCount;	//累计本设备链接到的对象数量
	_aInt ref_count;
	unsigned sync_token;
	_aInt connection_state;
	int online;
	transport_type type;

	/* usb handle or socket fd as needed */
	usb_handle *usb;
	int sfd;

	/* used to identify transports for clients */
	_achar serial;
	char *product;
	char *model;
	char *device;
	char *devpath;
	int adb_port; // Use for emulators (local transport)

	/* a list of adisconnect callbacks called when the transport is kicked */
	_aInt          kicked;

	void *key;
	unsigned char token[TOKEN_SIZE];
	//fdevent auth_fde;
	unsigned failed_auth_attempts;
//======================================================================================
public:	//用户操作数据对象
	bip_shared_mutex	bip_lock;	//用户列表操作锁
	client_list			userlist;	//用户通讯通道表
	apacketlist			loop_to_use_apacket;	//数据包循环使用
private://启动服务锁
	adb_usb_mutex	start_mutex;
	void*			atp;	//临时保存自身指针
private://实现线程同步 信号量
	Semaphore		d_write_ctrol, d_read_ctrol;	//d_write_ctrol 写入usb锁， d_read_ctrol 从usb读取数据锁
public://错误代号
	_aInt	t_error;
public:
	Catransport(usb_handle *h, const char *_serial, const char *_devpath, int state);
	~Catransport();	//析构 释放所有资源 并从 usb_handle 队列中剔除
	bool Start();	//启动服务通讯
	char *connection_state_name();
	void Clear();
	void free_ctrol();//集中释放释放同步锁 析构使用
private://释放资源
	void	remote_usb_clear();//usb设备资源清理
public:	//线程调用
	int read_from_remote(apacket *p);
	int write_to_remote(apacket *p);
	//void(*close)(atransport *t);	//不再需要手动清理
	inline void kick(){kicked = 1; }
	inline void handle_online(){ online = 1; }
	inline void handle_offline(){ online = 0; }
public://数据包处理
	apacketPtr	get_apacket();
	void		put_apacket(apacketPtr& p);
public://数据传输
	void send_auth_response(uint8_t *token, size_t token_size);
	void send_auth_publickey();
	void send_connect();
	void send_ready(unsigned local, unsigned remote);
	void send_close(unsigned local, unsigned remote);
	void send_packet(apacketPtr& p);
	void parse_banner(char *banner);
	void handle_packet(apacketPtr& p);
public://数据传输本地与用户端交互
	int local_client_enqueue(int remote_arg0, int local_arg1, apacketPtr& p);//数据发送至用户通道
	int smart_local_remote_enqueue(int local_id, const char *destination);	//打开设备通许通道 成功0
	void connect_to_remote(int local_id, const char *destination);	//对接本地与设备通道	A_OPEN
	int local_remote_enqueue(int local_id, apacketPtr& p);		//本地发送到设备数据 A_WRAE
	int	local_client_type_set(int _type, int local_id);	//标志通讯类型
public://用户交互时调用
	//shell
	int shell_send(const char *destination, int local_id);
	int shell_recv(sstring &ret, int local_id, int wait_times = 0);
	//forward sync 通用
	long client_read(unsigned char* retData, bufferstream* buffer, long len, aclientPtr local, int wait_times = 0);//从通道中读取指定长度到数据 retData buffer 数据可以传输到连个中到一个
	int client_write(void* sendData, long len, int local_id);	//向设备发送指定长度数据
	//forward
	int forward_connect(const char* remote_connect, int local_id);
	int forward_write(void* sendData, long len, int local_id);	//发送数据
	int forward_read(unsigned char* retData, long len, int local_id, int wait_times = 0);	//最长等待时间(秒)
	inline int forward_disconnect(int local_id){ return local_client_type_set(bipbuffer_close, local_id); }
	//sync 文件传递
	inline int sync_push(const char *lpath, const char *rpath, int verifyApk, int local_id){ 
		return do_sync_push(lpath, rpath, verifyApk, local_id); }
	inline int sync_push_buffer(unsigned char* buffer, size_t len, const char *rpath, int local_id) { 
		return do_sync_push_buffer(buffer, len, rpath, local_id); }
	inline int sync_pull(const char *rpath, const char *lpath, int local_id){ 
		return do_sync_pull(rpath, lpath, local_id); }
	inline int sync_pull_buffer(bufferstream &buffer, size_t &len, const char *rpath, int local_id){ 
		return do_sync_pull_buffer(buffer, len, rpath, local_id); }
	inline int sync_sync(const char *lpath, const char *rpath, int listonly, int local_id){ 
		return do_sync_sync(lpath, rpath, listonly, local_id); }
public://通道传输
	int	read_packet(int f, apacketPtr& packet);
	int	write_packet(int f, apacketPtr& packet);
	int client_get_id();	//通讯通道申请
	void client_free_id(int _id);	//通道关闭
	aclientPtr	client_get_ptr(int _id);	//获取通道对象指针
	void	client_init(aclientPtr a);		//将对象初始化
	void	client_clear(aclientPtr a);		//清空数据
	void	client_removeAll();				//释放所有对象
private://usb设备数据处理
	void wait_for_usb_read_packet(int f, BipBuffer bip);
	int	usb_read_packet(int f, apacketPtr& packet);
	int	usb_write_packet(int f, apacketPtr& packet);
	int remote_usb_read(apacket* p);
	int remote_usb_write(apacket* p);
private://sync 通讯流程
	//sync 通讯
	int sync_connect(int local_id);
	int sync_write(void* sendData, long len, int local_id);
	int sync_read(unsigned char* retData, long len, int local_id, int wait_times = 0);	//返回一个长度， 尾部变参返回最后一个包的指针保留未读完的数据 最长等待时间(秒)
	int sync_read_to_buffer(bufferstream &buffer, long len, int local_id, int wait_times = 0);	//返回一个长度， 尾部变参返回最后一个包的指针保留未读完的数据 最长等待时间(秒)
	inline int sync_disconnect(int local_id){ return local_client_type_set(bipbuffer_close, local_id); }
	//sync 流程 file_class_sync_client.cpp
	void sync_quit(int local_id);
	int sync_ls(const char *path, void *cookie, int local_id);
	int sync_readtime(const char *path, unsigned *timestamp, int local_id);
	int sync_start_readtime(const char *path, int local_id);
	int sync_finish_readtime(unsigned int *timestamp, unsigned int *mode, unsigned int *size, int local_id);
	int sync_readmode(const char *path, unsigned *mode, int local_id);
	int write_data_file(const char *path, int local_id);
	int write_data_buffer(char* file_buffer, int size, int local_id);
	int sync_send(const char *lpath, const char *rpath, unsigned mtime, _mode_t mode, int verifyApk, int local_id);
	int sync_send_buffer(unsigned char* buffer, size_t _len, const char *rpath, unsigned mtime, _mode_t mode, int local_id);
	int sync_recv(const char *rpath, const char *lpath, int local_id);
	int sync_recv_buffer(bufferstream &_buffer, size_t &_len, const char *rpath, int local_id);
	int copy_local_dir_remote(const char *lpath, const char *rpath, int checktimestamps, int listonly, int local_id);
	int do_sync_push(const char *lpath, const char *rpath, int verifyApk, int local_id);
	int remote_build_list(copyinfo **filelist, const char *rpath, const char *lpath, int local_id);
	int copy_remote_dir_local(const char *rpath, const char *lpath, int checktimestamps, int local_id);
	int do_sync_pull(const char *rpath, const char *lpath, int local_id);
	int do_sync_sync(const char *lpath, const char *rpath, int listonly, int local_id);
	int do_sync_push_buffer(unsigned char* buffer, size_t len, const char *rpath, int local_id);
	int do_sync_pull_buffer(bufferstream &buffer, size_t &len, const char *rpath, int local_id);
};

#endif	//_CATRANSPORT_H_