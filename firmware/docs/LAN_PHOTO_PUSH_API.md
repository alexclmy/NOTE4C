# 局域网相册推送 API

本文档记录 2BP 相册固件在局域网 Wi-Fi 模式下的 HTTP 接口，方便 NAS、脚本或局域网服务定时推送图片到设备。

## 使用前提

设备需要先连接到局域网 Wi-Fi，然后在设备设置页打开 `局域网服务`。开启后设置页会显示设备当前局域网 IP，例如：

```text
<panel-ip>
```

后续接口都以这个 IP 为准：

```text
http://<panel-ip>
```

局域网服务打开后，浏览器访问 `http://设备IP/` 可以进入图片管理页面。NAS 或脚本也可以直接调用下面的 API。

## 图片格式要求

当前 `/upload` 接口不直接接收 JPG、PNG、WEBP 等常见图片文件，而是接收已经转换好的屏幕原始像素数据。

设备屏幕尺寸固定为：

```text
400 x 300
```

支持两种上传格式：

| format | 含义 | 文件大小 |
| --- | --- | --- |
| `1bpp` | 黑白 1 bit per pixel | `15000 bytes` |
| `bwry2bpp` 或 `2bpp` | 黑/白/黄/红四色 2 bits per pixel | `30000 bytes` |

如果从 NAS 定时推送普通图片，需要先在 NAS 侧把 JPG/PNG 转成上述 bin 格式，再调用 `/upload`。

## 快速检查服务状态

```bash
curl "http://<panel-ip>/status"
```

成功返回示例：

```json
{
  "status": "ready",
  "mode": "lan",
  "ip": "<panel-ip>",
  "url": "http://<panel-ip>/"
}
```

其中 `mode=lan` 表示当前是局域网 HTTP 服务；`mode=ap` 表示热点传图模式。

## 上传图片

上传 2BP 四色图片：

```bash
curl -X POST \
  "http://<panel-ip>/upload?format=bwry2bpp" \
  -H "Content-Type: application/octet-stream" \
  --data-binary "@/path/to/image_400x300_2bpp.bin"
```

上传 1BP 黑白图片：

```bash
curl -X POST \
  "http://<panel-ip>/upload?format=1bpp" \
  -H "Content-Type: application/octet-stream" \
  --data-binary "@/path/to/image_400x300_1bpp.bin"
```

成功返回示例：

```json
{
  "success": true,
  "id": "ap12345678901"
}
```

失败常见原因：

| 原因 | 表现 |
| --- | --- |
| 文件大小不对 | 返回 `需要400x300 2bpp四色数据` 或 `需要400x300 1bpp数据` |
| 设备 HTTP 服务未开启 | NAS 无法连接设备 IP |
| IP 变化 | 需要重新读取设备设置页显示的局域网 IP |
| 相册容量满 | 设备保存失败 |

## 查询图片列表

```bash
curl "http://<panel-ip>/photos"
```

返回示例：

```json
{
  "photos": [
    {
      "id": "ap12345678901",
      "title": "WiFi四色图片",
      "date": "2026-05-21",
      "location": "WiFi AP",
      "body": "手机 WiFi 传图 · 2 BP 四色",
      "width": 400,
      "height": 300,
      "size": 30000,
      "format": "bwry2bpp"
    }
  ]
}
```

可用字段说明：

| 字段 | 含义 |
| --- | --- |
| `id` | 图片 ID，后续读取、删除、编辑都用它 |
| `title` | 图片标题 |
| `date` | 日期字符串 |
| `location` | 地点 |
| `body` | 描述 |
| `width` / `height` | 图片尺寸 |
| `size` | 原始数据大小 |
| `format` | `1bpp` 或 `bwry2bpp` |

## 下载图片原始数据

```bash
curl \
  "http://<panel-ip>/photo?id=ap12345678901" \
  --output image.bin
```

返回内容是该图片保存时的原始 bin 数据。

## 删除图片

```bash
curl -X DELETE \
  "http://<panel-ip>/photo?id=ap12345678901"
```

成功返回：

```json
{"success":true}
```

## 更新图片信息

```bash
curl -X POST "http://<panel-ip>/photo/meta" \
  -H "Content-Type: application/json" \
  -d '{
    "id": "ap12345678901",
    "title": "每日照片",
    "date": "2026-05-21",
    "location": "NAS",
    "body": "NAS 每日自动推送"
  }'
```

成功返回：

```json
{"success":true}
```

## 调整图片顺序

上移一位：

```bash
curl -X POST "http://<panel-ip>/photos/move" \
  -H "Content-Type: application/json" \
  -d '{"id":"ap12345678901","delta":-1}'
```

下移一位：

```bash
curl -X POST "http://<panel-ip>/photos/move" \
  -H "Content-Type: application/json" \
  -d '{"id":"ap12345678901","delta":1}'
```

## 设置轮播周期

查询当前轮播设置：

```bash
curl "http://<panel-ip>/settings"
```

返回示例：

```json
{
  "success": true,
  "slideshow_interval": 5,
  "service_running": true,
  "mode": "lan",
  "ip": "<panel-ip>",
  "url": "http://<panel-ip>/"
}
```

设置轮播周期：

```bash
curl -X POST "http://<panel-ip>/settings" \
  -H "Content-Type: application/json" \
  -d '{"slideshow_interval":5}'
```

关闭本地 HTTP 服务：

```bash
curl -X POST "http://<panel-ip>/settings" \
  -H "Content-Type: application/json" \
  -d '{"service_enabled":false}'
```

关闭服务、关闭 Wi-Fi 并立即进入省电模式：

```bash
curl -X POST "http://<panel-ip>/settings" \
  -H "Content-Type: application/json" \
  -d '{"service_enabled":false,"wifi_enabled":false,"sleep":true}'
```

支持值：

| 值 | 含义 |
| --- | --- |
| `0` | 关闭轮播 |
| `5` | 5 分钟 |
| `10` | 10 分钟 |
| `30` | 30 分钟 |

## NAS 定时任务示例

本目录提供了一个可复用转换脚本：

```text
docs/inkscreen_image_converter.js
```

它抽取自设备管理 HTML 页面中的转换算法，用于把普通图片转换为设备 `/upload` 接口需要的原始 bin。命令行模式依赖 `sharp` 解码和缩放图片：

```bash
npm install sharp
node docs/inkscreen_image_converter.js input.jpg daily_400x300_2bpp.bin bwry2bpp
```

也可以生成黑白 1BP：

```bash
node docs/inkscreen_image_converter.js input.jpg daily_400x300_1bpp.bin 1bpp
```

假设 NAS 已经生成了一个 `daily_400x300_2bpp.bin`，可以用 cron 每天推送一次：

```bash
#!/bin/sh
DEVICE="<panel-ip>"
BIN="/volume1/photo/daily_400x300_2bpp.bin"

curl -fsS -X POST \
  "http://${DEVICE}/upload?format=bwry2bpp" \
  -H "Content-Type: application/octet-stream" \
  --data-binary "@${BIN}"
```

如果希望上传后补充描述，可以先解析返回的 `id`，再调用 `/photo/meta`。例如：

```bash
#!/bin/sh
DEVICE="<panel-ip>"
BIN="/volume1/photo/daily_400x300_2bpp.bin"

RESP=$(curl -fsS -X POST \
  "http://${DEVICE}/upload?format=bwry2bpp" \
  -H "Content-Type: application/octet-stream" \
  --data-binary "@${BIN}")

ID=$(printf "%s" "$RESP" | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')

if [ -n "$ID" ]; then
  TODAY=$(date +%F)
  curl -fsS -X POST "http://${DEVICE}/photo/meta" \
    -H "Content-Type: application/json" \
    -d "{\"id\":\"${ID}\",\"title\":\"每日照片\",\"date\":\"${TODAY}\",\"location\":\"NAS\",\"body\":\"NAS 自动推送\"}"
fi
```

## 推荐后续增强

目前 API 已经可以支持 NAS 定时推送，但如果要让 NAS 直接传 JPG/PNG，还需要新增一个固件端或 NAS 端转换流程。

推荐方案：

1. NAS 侧转换：在 NAS 上用脚本把 JPG/PNG 转为 `400x300 bwry2bpp bin`，再调用 `/upload`。这种方式最省设备内存。
2. 固件端新增 `/upload-image`：设备直接接收 JPG/PNG 并转换。开发更方便，但 ESP32 端内存和解码成本更高。

当前更建议使用方案 1。
