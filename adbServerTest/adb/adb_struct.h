#ifndef _ADB_STRUCT_H_
#define _ADB_STRUCT_H_

//adb 用到的结构
#include <boost/thread/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/lockfree/queue.hpp>	//无锁队列
#include <boost/atomic.hpp>	//原子操作
#include <string>	
#include <set>
#include <vector>
#include "sysdeps.h"

#define boost_second_get(t)			(boost::get_system_time() + boost::posix_time::seconds(t))
#define boost_millisecond_get(t)	(boost::get_system_time() + boost::posix_time::milliseconds(t))

void bip_apacke_del_all(void* p);	//apacket队列 consume_all 批量删除用

typedef std::vector<unsigned char>	bufferstream;
typedef	std::string					sstring;
typedef	boost::lockfree::queue<void*>	BipPtrList, *BipLpList;
typedef boost::atomic_int				_aInt;					//原子整型操作 无需上锁
typedef boost::atomic<char*>			_achar;
typedef boost::atomic<long>				_along;

typedef boost::mutex								adb_buff_mutex;
typedef boost::mutex::scoped_lock					adb_mutex_lock;	//锁
typedef boost::system_time							adb_time;		//时间

typedef adb_buff_mutex								adb_usb_mutex;
typedef adb_mutex_lock								adb_write_lock;	//锁

#define ADB_MUTEX_DEFINE(x)		adb_buff_mutex x

extern adb_usb_mutex	log_lock;// 日志输出锁

#define delay(ms) boost::thread::sleep(boost::get_system_time()+boost::posix_time::milliseconds(ms)); //延迟  毫秒

#define adb_sleep_ms(ms)	delay(ms)

//--------------------------设备在线状态ID----------------------------------------
#define CS_ANY       -1
#define CS_OFFLINE    0
#define CS_BOOTLOADER 1
#define CS_DEVICE     2
#define CS_HOST       3
#define CS_RECOVERY   4
#define CS_NOPERM     5 /* Insufficient permissions to communicate with the device */
#define CS_SIDELOAD   6

//------------------------------数据传递的结构 类型-------------------------------

#define MAX_PAYLOAD 4096

#define A_SYNC 0x434e5953
#define A_CNXN 0x4e584e43
#define A_OPEN 0x4e45504f
#define A_OKAY 0x59414b4f
#define A_CLSE 0x45534c43
#define A_WRTE 0x45545257
#define A_AUTH 0x48545541

#define A_VERSION 0x01000000        // ADB protocol version

#define ADB_VERSION_MAJOR 1         // Used for help/version information
#define ADB_VERSION_MINOR 0         // Used for help/version information

#define ADB_SERVER_VERSION    31    // Increment this when we want to force users to start a new adb server

typedef struct amessage{
	unsigned command;       /* command identifier constant      */
	unsigned arg0;          /* first argument                   */
	unsigned arg1;          /* second argument                  */
	unsigned data_length;   /* length of payload (0 is allowed) */
	unsigned data_check;    /* checksum of data payload         */
	unsigned magic;         /* command ^ 0xffffffff             */
}amessage;

typedef struct apacket
{
	//apacket *next;

	unsigned len;
	unsigned char *ptr;

	amessage msg;
	unsigned char data[MAX_PAYLOAD];
}apacket, *apacketPtr;

typedef	boost::lockfree::queue<apacket*>	apacketlist, *apacketlistptr;	//数据包队列 用户 循环回收使用

//-----------------------------------读写操作 结构 类型-------------------------
#define  BIP_BUFFER_SIZE   4096
#define  BIPD(x)        do {} while (0)
#define  BIPDUMP(p,l)   BIPD(p)

typedef enum bipbuffer_type {	//数据接收时 处理略有区别
	bipbuffer_close = -1,
	bipbuffer_shell_sync,
	bipbuffer_shell_async,		//这个是交互式shell:
	bipbuffer_file_sync,		//文件传递
	bipbuffer_forward,			//转接
} bipbuffer_type;

typedef struct BipBufferRec_
{
	int                fdin;
	int                fdout;
	_aInt			   closed;
	BipLpList		   ptrlist;	//无锁队列指针 用于 传递 数据
} BipBufferRec, *BipBuffer;

typedef struct SocketPairRec_
{
	BipBufferRec  a2b_bip;
	BipBufferRec  b2a_bip;
	int           a_fd;		//标志数据的 传入 传出 通道
	int           used;
} SocketPairRec, *SocketPair;

//----------------------------------------设备信息结构--------------------------------------------
#define TOKEN_SIZE 20

typedef enum transport_type {
	kTransportUsb,
	kTransportLocal,
	kTransportAny,
	kTransportHost,
} transport_type;

typedef struct usb_handle usb_handle;


#endif	//_ADB_STRUCT_H_
