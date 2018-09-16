# Adb-Server-for-C-project
this is a code of C++ to access android adbd

After reading the ADB source code, I used Android's communication protocol to rewrite the project.

It can communicate directly with adbd without adb.exe.
You can copy it to your C++ project and use it directly.
It depends on boost, so you need to build your boost library.
You can see all the libraries in vs2013,and changed the path for your boost.
At present, it only supports USB connection communication, supports shell, push, pull, forward.
I'll add something else later, like conncet..

Issues can be used for communication.

Alipay: lfjking@qq.com
If this project is worth your donation,


英文翻译的不知道对不对，还是用中文说的清楚点。
这是个直接使用安卓adbd通讯协议写的一个C++对象工程。
可以将他复制到你的工程中直接调用CAdb 就可以开始进行ADB通讯了。
支持的指令有shell, push, pull, forward.
后续也许会增加 conncet 连接无线设备。
工程依赖boost。链接输入项中有显示需要用到的库。路径修改到自己的boost。
工程中有个简单的使用例子。

支付宝:lfjking@qq.com
如果觉得有价值捐助的话。
