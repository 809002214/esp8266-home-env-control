# 固件目录

把编译好的 `ESP8266` 固件放到这个目录里，`Node-RED` 会通过静态文件方式提供下载。

## 方式 1：固定默认固件

默认推荐文件名：

- `esp8266-home-env-control.bin`

默认访问地址：

```text
http://你的设备IP:1880/firmware/esp8266-home-env-control.bin
```

如果你的 ESP8266 配网页里保持默认：

- `OTA下载端口`：`1880`
- `OTA固件路径`：`/firmware/esp8266-home-env-control.bin`

那设备收到 MQTT 主题：

- `home/room1/env01/ota/set`

并且负载是：

```text
UPDATE
```

就会从上面的地址下载并升级。

## 方式 2：版本化发布（推荐）

你可以在 Dashboard 的“固件发布与升级”区直接上传 `.bin`，系统会自动生成：

- `/data/public/firmware/releases/<版本号>/<文件名>.bin`
- `/data/public/firmware/releases/<版本号>/release.md`
- `/data/public/firmware/catalog.json`

对应 OTA 命令格式：

```text
UPDATE:/firmware/releases/<版本号>/<文件名>.bin
```
