# Windows 版 nc.exe 参数兼容实现说明

源码文件：

- `nc.c`

## 支持的参数

| 参数 | 含义 |
|---|---|
| `-n` | 只使用数字 IPv4 地址，不解析 DNS / 主机名 |
| `-w <seconds>` | 超时时间，单位秒 |
| `-l` | 监听模式 |
| `-p <port>` | 端口。监听模式下为监听端口；连接模式下为本地源端口 |
| `-v` | verbose，显示连接、监听、来源地址等过程信息 |
| `-c <command>` | 连接建立后执行指定命令；不指定时默认执行 `cmd.exe /Q` |
| `-h` / `--help` | 打印帮助 |

## 连接模式

格式：

```bat
nc.exe [-n] [-v] [-w seconds] [-p local_port] [-c command] <host> <port>
```

示例：

```bat
nc.exe -n -w 3 127.0.0.1 1337
```

含义：

- `-n`：`127.0.0.1` 必须是数字 IPv4，不做 DNS 解析
- `-w 3`：连接 3 秒内没有建立则退出
- `127.0.0.1`：目标地址
- `1337`：目标端口

带详细输出：

```bat
nc.exe -v -n -w 3 127.0.0.1 1337
```

指定本地源端口：

```bat
nc.exe -v -n -w 3 -p 4444 127.0.0.1 1337
```

指定连接后执行的命令：

```bat
nc.exe -v -n -w 3 -c "cmd.exe /Q" 127.0.0.1 1337
```

指定 PowerShell：

```bat
nc.exe -v -n -w 3 -c "powershell.exe -NoLogo -NoProfile" 127.0.0.1 1337
```

## 监听模式

格式一：

```bat
nc.exe -l -p <port>
```

格式二：

```bat
nc.exe -l <port>
```

示例：

```bat
nc.exe -l -p 1337
```

或：

```bat
nc.exe -l 1337
```

带详细输出：

```bat
nc.exe -l -v -p 1337
```

带 accept 超时：

```bat
nc.exe -l -v -w 10 -p 1337
```

含义：

- 监听 `0.0.0.0:1337`
- 最多等待 10 秒
- 10 秒内没有连接则退出

监听后执行指定命令：

```bat
nc.exe -l -v -p 1337 -c "cmd.exe /Q"
```

监听后执行 PowerShell：

```bat
nc.exe -l -v -p 1337 -c "powershell.exe -NoLogo -NoProfile"
```

## 编译命令

### MinGW / GCC

```bat
gcc -O2 -o nc.exe nc.c -lws2_32
```

### MSVC

```bat
cl /O2 nc.c ws2_32.lib
```

## 参数兼容行为

### `-p`

监听模式：

```bat
nc.exe -l -p 1337
```

此时 `-p 1337` 表示监听端口。

连接模式：

```bat
nc.exe -p 4444 127.0.0.1 1337
```

此时 `-p 4444` 表示本地源端口。

### `-w`

连接模式：

```bat
nc.exe -w 3 127.0.0.1 1337
```

此时 `-w 3` 表示 connect 阶段最多等待 3 秒。

监听模式：

```bat
nc.exe -l -w 10 -p 1337
```

此时 `-w 10` 表示 accept 阶段最多等待 10 秒。

### `-v`

连接模式会输出：

- 正在连接的地址和端口
- 本地源端口，如果指定了 `-p`
- 连接超时设置，如果指定了 `-w`
- 连接成功信息

监听模式会输出：

- 正在监听的地址和端口
- accept 超时设置，如果指定了 `-w`
- 客户端来源 IP 和端口

### `-c`

不指定 `-c` 时，默认执行：

```bat
cmd.exe /Q
```

指定 `-c` 时，执行传入的完整命令行：

```bat
nc.exe -l -p 1337 -c "cmd.exe /Q"
```

```bat
nc.exe -l -p 1337 -c "powershell.exe -NoLogo -NoProfile"
```

`-c` 后面的内容需要作为一个参数传入；命令中包含空格时使用双引号包起来。
