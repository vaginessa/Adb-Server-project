// adbServerTest.cpp : 定义控制台应用程序的入口点。
//

#include "stdafx.h"
#include <iostream>
#include <fstream>
#include "adb\CAdb.h"

int _tmain(int argc, _TCHAR* argv[])
{
	sstring devicelist;
	CAdb::InitAdb();
	CAdb::FindDevices();
	CAdb::GetDevicesNameList(devicelist);
	int sii = devicelist.find("|");
	if (sii < 0){ return 0; }

	sstring name(&devicelist[0], sii);
	printf("device name:%s\n", name.c_str());
	CAdb adb(name.c_str());
	if (adb.IsOk()){
		while (!adb.IsOnline())
		{
			if (adb.IsUsbClose()){ return 0; }
		}
		while (!adb.IsUsbClose())
		{
			sstring sendstr, retstr;
			std::getline(std::cin, sendstr);
			adb.Shell_send(sendstr.c_str());
			adb.Shell_recv(retstr);
			printf("recv:\n%s\n", retstr.c_str());
		}
	}
	CAdb::RepleaseAdb();

	return 0;
}


