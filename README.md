# harmony-IPCamera

Android 项目 `IPCameraJavaRestored`(com.shenyaocn.android.WebCam)的 HarmonyOS 移植工程。
当前阶段:**RTSP 流媒体核心 POC**(路线 B:系统 Kit 替代 + 按 JNI 契约重新实现)。

## 关键背景

- Android 侧**没有 native 源码**(只有预编译 .so,arm64-v8a 二进制无法在鸿蒙 musl/libc 上运行),
  自研 native 件按 Java 侧 JNI 契约**重新实现**;开源件(FFmpeg/OpenH264/Opus/libjpeg)后续用
  OpenHarmony 三方库移植替代。
- 原生库在 JNI_OnLoad 里的 APK 签名校验(exit(0))不需要迁移;鸿蒙侧等价做法是 ArkTS 用
  bundleManager 校验 HAP 签名证书哈希。
- Android 侧完整契约:82 个 native 方法分布在 9 个包装类
  (Adts.Encoder / LibJpeg.Decoder / OggOpus.Codec / OpenCV.MotionDetection /
  OpenH264.Encoder+Decoder / RTMPPublisher.RTMPPublisher+FlvPublisher / RTSPStreaming.RTSPStreaming)。
  本 POC 先落地最核心的 RTSPStreaming(9 个方法)。

## 已实现(POC)

| 组件 | 位置 | 说明 |
|---|---|---|
| RTSP/RTP 服务端 | `entry/src/main/cpp/rtsp_server.{h,cpp}` | 替代 libstreaming.so:RTSP 1.0(OPTIONS/DESCRIBE/SETUP/PLAY/PAUSE/TEARDOWN/GET_PARAMETER)、H.264 RFC6184(单 NAL + FU-A)、ADTS AAC RFC3640(AAC-hbr)、UDP 与 TCP-interleaved 双传输、Basic 认证、SDP(sprop-parameter-sets / AAC config 自动从码流提取) |
| 测试视频源 | `entry/src/main/cpp/test_source.{h,cpp}` | OH_AVCodec H.264 硬编(buffer 模式:RegisterCallback → PushInputBuffer / FreeOutputBuffer),彩条 + 移动方块,验证"系统编解码器替代 FFmpeg/OpenH264"这条路线 |
| napi 绑定 | `entry/src/main/cpp/napi_init.cpp` | 模块名 `streaming`,与 Android JNI 面一一对应(见下表);状态回调走 napi_threadsafe_function |
| ArkTS 演示页 | `entry/src/main/ets/pages/Index.ets` | 启停服务 / 启停测试流 / 客户端事件日志 / 显示 rtsp URL(wifiManager 取本机 IP) |

### napi 接口 ↔ Android JNI 契约对应

| Android (RTSPStreaming.java) | HarmonyOS (libstreaming.so) |
|---|---|
| nativeCreate() | createRtspServer() |
| nativeStartPublish(path,user,pass,addr?,port,w,h,fps,?,bool) | rtspStart(server, {port, path, user, password}) |
| nativeStopPublish() | rtspStop(server) |
| nativeIsRunning() | rtspIsRunning(server) |
| nativeSendH264Packet(buf, len) | rtspSendH264(server, buffer, tsUs) |
| nativeSendAdtsPacket(buf, len) | rtspSendAdts(server, buffer, tsUs) |
| nativeSetStatusCallback(cb) | rtspSetStatusCallback(server, cb)(线程安全函数) |
| —(新增) | rtspClientCount / rtspStartTestPattern / rtspStopTestPattern |

## 构建与运行

```bash
# 命令行构建(未签名 HAP)
export DEVECO_SDK_HOME="C:/Program Files/Huawei/DevEco Studio/sdk"
"C:/Program Files/Huawei/DevEco Studio/tools/node/node.exe" \
  "C:/Program Files/Huawei/DevEco Studio/tools/hvigor/bin/hvigorw.js" \
  --mode module -p product=default assembleHap --no-daemon
```

1. DevEco Studio 打开工程,File → Project Structure → Signing Configs 勾选
   **Automatically generate signature**(当前构建产物为未签名 HAP)。
2. 运行到真机/模拟器(需与 PC 同一 WLAN)。
3. 点 **启动服务** → **开启测试流**。
4. PC 验证:
   ```bash
   ffplay -fflags nobuffer rtsp://<设备IP>:8554/live
   # 或 VLC: rtsp://admin:admin@<设备IP>:8554/live
   ```

## 真机验证记录(2026-09-27,华为畅享 90 Plus / HarmonyOS 7.0)

端到端验证通过(OH_AVCodec 硬编 → Annex-B → RTP → RTSP 会话 → 标准客户端):

- `libstreaming.so` 真机加载、napi 模块注册正常;
- OH_AVCodec H.264 硬编启动(640x480@15,High Profile 64001e,SPS/PPS 自动入 SDP);
- RTSP 全流程:OPTIONS 200 → DESCRIBE 无凭据 401 / 带凭据 200(含 sprop-parameter-sets)
  → SETUP(TCP interleaved)200 → PLAY 200;
- RTP 流:4 秒 61 包,seq 严格连续,marker 每帧一置,90kHz 时间戳增量 6000(精确 15fps)。

**踩坑记录(重要平台差异)**:ArkTS 运行时把 `napi_create_external` 的对象标记为
`napi_valuetype napi_external`,对它调 `napi_unwrap` 会抛异常(`napi_pending_exception`),
必须用 `napi_get_value_external` 读回指针;`napi_unwrap` 只适用于 `napi_wrap` 包装的普通对象。

验证工具链:hdc(DevEco SDK `toolchains/hdc.exe`)→ `fport tcp:18554 tcp:8554` →
PC 端 Python RTSP 客户端(OPTIONS/DESCRIBE/SETUP/PLAY + RTP 解析,脚本见工程外 `rtsp_test.py`)。
注意:锁屏状态下 `aa start` 会被拒(开发者模式策略,错误码 10106102),需解锁后拉起。

## 已知边界(POC 范围)

- 真机硬编输出、VLC/ffplay 实播、多客户端并发未验证(需设备)。
- 音频轨已实现(RFC3640)但无测试源;计划走 OH_AVCodec AAC 编码或 AudioCapturer。
- RTMP(libRTMPPublisher / FlvPublisher 契约)未开始;建议用 OHOS 移植的 librtmp 重写。
- 无前台保活(backgroundTaskManager 长时任务待接);息屏会被系统挂起。
- 只支持 H.264 轨;HEVC(sendHEVCPacket 契约)待加(RFC 7798)。

## 阶段 2(已完成,真机验证通过)

| 功能 | 实现 | 真机验证 |
|---|---|---|
| 相机采集 | Camera Kit NDK(ohcamera)→ CaptureSession → PreviewOutput 渲染进编码器 surface → OH_AVCodec H.264(surface 模式)→ RTSP 视频轨 | 720p,74帧/6s,FU-A 分片,seq 连续 |
| 麦克风采集 | OH_AudioCapturer(48k mono S16LE)→ OH_AudioEncoder AAC(旧式 data API)→ 自组 ADTS 头 → RTSP 音频轨(RFC 3640) | 301包/6s = 精确 AAC 帧率,SDP 自动出现 `config=1188` |
| 运行时权限 | EntryAbility onCreate `requestPermissionsFromUser`(CAMERA/MICROPHONE) | 权限框正常,授权后可用 |
| 长时任务保活 | startBackgroundRunning(AUDIO_RECORDING)+ module.json5 `backgroundModes: ["audioRecording"]` | 连续任务挂载成功 |

**踩坑记录(续)**:
- 长时任务:TASK_KEEPING 模式在华为设备上被禁(9800005),必须用 `AUDIO_RECORDING` 且在 module.json5 ability 里声明 `backgroundModes`,否则报 "bgMode is invalid"。
- 麦克风洪泛:audio encoder 队列空时推 size=0 输入会导致空帧风暴(6秒4万包);正确做法是持有输入缓冲(OH_AVMemory),等 OnReadData 回调来了再填再推。
- surface 模式下编码器输出的 pts 不可靠,RTSP 时间戳用服务端单调时钟替代。
- `Camera_Device*` 指向 GetSupportedCameras 返回的数组,DeleteSupportedCameras 后悬空 → CreateCameraInput INVALID_ARGUMENT(7400101);须先建 Input 再删数组。
- 音频编码器是 @since-9 旧式 API(SetCallback + PushInputData/FreeOutputData + OH_AVMemory),与视频的新式 RegisterCallback/PushInputBuffer 不同。
- `OH_AudioStreamBuilder_SetCapturerCallback` 按值传回调结构体;`Destroy` 在新 SDK 中已纠正拼写(旧文档的 Destory 不存在)。

**已知调优项**:surface 模式下编码器忽略 I 帧间隔(全 IDR 输出,实际码率高于 CBR 设定);相机无预览画面(XComponent 待加);未处理传感器旋转。

## 阶段 3:RTMP 推流(已完成,闭环验证通过)

`rtmp_publisher.cpp`:自研极简 RTMP 客户端 + FLV 封装(替代 libRTMPPublisher/FlvPublisher,
无第三方依赖)。握手(C0/C1/S0/S1/S2)、SetChunkSize、AMF0 connect/createStream/publish、
@setDataFrame onMetaData、AVC 序列头(AVCDecoderConfigurationRecord)+ AVCC 媒体帧(含 FLV
VideoTagHeader,关键帧 0x17/帧间 0x27)、AAC 序列头(AudioSpecificConfig)+ 原始帧、
服务器消息跳帧解析 + Window Ack。编码输出从相机/麦克风**双路分发**(RTSP + RTMP 同时推)。

验证:PC 端 Python 极简 RTMP 服务器(`rtmp_server.py`)+ `hdc rport tcp:11935 tcp:11935`
反向转发,手机推流到 127.0.0.1:11935 闭环。结果:握手/三命令/onMetaData/双序列头全通过,
视频 244 帧/20s 时间轴精确,音频 995 包 = 精确 AAC 帧率,零协议警告。UI 上在推流地址输入框
填 `rtmp://host:1935/app/streamkey` 后点"开启推流"即可,RTSP 与 RTMP 同时直播。

**RTMP 踩坑记录**:①C1 必须 1536 字节(写成 1537 会让整个消息流错位 1 字节,症状是服务器
解析出乱码消息);②chunk 消息头的 stream id 是**小端**(Put4 大端会让 publish 的 sid 变
0x01000000);③相机编码器的 SPS/PPS 是**独立缓冲**输出(不与 IDR 同帧),序列头必须在参数
齐全时立即发,不能等"当前帧带参数"才发;④FLV 媒体帧必须带 VideoTagHeader(0x17/0x27 +
AVCPacketType + CTS),裸 AVCC 会被服务器判为 codecId=0;⑤sender 线程自退出后 Stop() 仍
必须 join(joinable 线程随析构会 std::terminate 崩溃)。

## 阶段 4(已完成,真机验证通过)

| 功能 | 说明 |
|---|---|
| H.265 RTSP 轨 | RFC 7798 打包(2 字节 NAL 头 + FU type 49),SDP 自动带 sprop-vps/sps/pps;按帧内参数集锚点(VPS/SPS/PPS type 32/33/34 且第二字节 0x01)自适应识别编码格式,支持运行中切换 |
| I 帧间隔修复 | **OH_MD_KEY_I_FRAME_INTERVAL 单位是毫秒**(此前误写 2=每 2ms 一个 I 帧=全 IDR);改 2000ms 后 P 帧成为主体(NAL 分布 {1:4164, 5:63}) |
| XComponent 本地预览 | 相机会话挂第二个 PreviewOutput 到 XComponent surface;surface 在页面加载即创建,全局记住 id 在相机启动时一并入初始会话配置(事后 BeginConfig 重配会弄断编码器 surface 生产者);两个 PreviewOutput 都要 Start |
| 设置页 | 分辨率/码率/编码(H264/H265)/前后摄/端口/路径/账号密码,preferences 持久化,重启相机/服务后生效 |

**阶段 4 踩坑**:①I_FRAME_INTERVAL 单位毫秒;②XComponent surface 先于相机创建,必须全局缓存 id;③会话运行中 BeginConfig 重配会断编码器 surface 供帧,新输出必须在初始配置里加;④第二个 PreviewOutput 忘记 Start 则黑屏;⑤rtsp_server.cpp 用 hilog 需自定义 LOG_DOMAIN/LOG_TAG(头文件默认 LOG_TAG=NULL 会静默吞日志,曾让排查误入歧途);⑥hdc 原生构建失败时 hvigor 仍可能报"BUILD SUCCESSFUL"(ArkTS 过了)——必须 grep error 或检查产物字符串。

**已知限制**:编码器输出画面方向未做旋转校正(取景可能横竖颠倒);RTMP 推流仅支持 H.264 模式(HEVC 需 enhanced-RTMP);相机被其他应用占用时启动失败会走错误回调。

## 阶段 5(已完成,真机验证通过)

| 功能 | 说明 |
|---|---|
| 旋转处理 | OH_CameraDevice_GetCameraOrientation 查询传感器方向;录像通过 OH_AVMuxer_SetRotation 写入旋转矩阵(播放器自动校正);流内容级旋转需 GL 中转,列为限制 |
| enhanced-RTMP H.265 | RtmpPublisher 按参数集锚点自动识别编码;H.265 走 hvc1 fourCC + HEVCDecoderConfigurationRecord 序列头 + 长度前缀 NALU 帧;onMetaData videocodecid='hvc1' |
| MP4 录像 | OH_AVMuxer(buffer 模式 WriteSampleBuffer);启动时从 RTSP 服务器缓存注入参数集(编码器只在流起点发一次参数!);H264/H265+AAC 双轨;SetRotation 写入方向;独立写线程 + 有界队列 |
| 抓拍 BMP | 缓存最新关键帧,抓拍时系统 H.264/H.265 解码器解一帧 NV12 → BT.601 转 RGB → 24 位 BMP,后台线程异步完成,结果走状态回调 |

真机验证:enhanced-RTMP H265 闭环(356 包/355 帧/keyframes=6/时间轴 19.4s/零警告);
MP4 录像 24MB(hvc1+hvcC+mp4a 结构校验通过);抓拍 BMP 1280x720 画面经人工目检正确。

**阶段 5 踩坑**:①编码器参数集只在流起点输出一次——中流启动的录像/抓拍必须从 RTSP 服务器缓存注入,否则永远等不到参数;②OH_AVMuxer 无 Release,是 Destroy;AddTrack 无 MediaType 参数(靠 mime 推断);③链接 libnative_media_avmuxer.so;④enhanced-RTMP 的 packetType 在 flags 字节低 4 位(不是 fourCC 之后);⑤uitest inputText 注入文本会丢字符且键盘改变布局——自动化测试避免依赖文本输入;⑥ninja 增量构建可能漏掉 python 写入的源文件修改——怀疑产物陈旧时用字符串字面量校验(校验代码注释是无效的,注释不进二进制)。

## 阶段 6(已完成,真机验证通过)——Web 控制台

| 功能 | 实现 |
|---|---|
| HTTP 服务器 | http_server.cpp:Basic 认证、Keep-Alive、多端点路由 |
| /serverinfo /size | 运行时间/分辨率/编码/客户端数/移动侦测/手电筒状态 |
| /snapshot.jpg /getsnapshot | WebStream 冷解码缓存关键帧→NV12→自研 JPEG 编码器(自建合法 Huffman 表) |
| /video | multipart/x-mixed-replace MJPEG(相同 JPEG 管线) |
| /light | 原生手电筒开关(OH_CameraManager_SetTorchMode) |
| /camswitch | 原生前/后摄切换(带预览面的完整相机重启) |
| /getarchives + /get/ipc_* + /get/MD/IPS_* | 录像存档列表(手工录像 + 移动侦测录像)+ MP4 下载 |
| /put_voice | AAC ADTS 语音上传 → OH_AudioDecoder 解码 → OH_AudioRenderer 播放(对讲) |
| 移动侦测 | WebStream 帧差分(32×18 亮度网格 3×3 均值采样,阈值 25),事件走 TSFN 回调;开关/自动录像见下节 |
| UI | 网页端口设置、启动/停止网页、移动侦测开关、HTTP URL 显示 |

**验证**:serverinfo(401→auth→正确 JSON 文本)、camswitch 原生切换成功、
snapshot.bmp 1280×720 目检正确(USB 线+织物纹理)、MP4 录像 24MB(hvc1 结构正确)。

**已知限制**:MJPEG/snapshot.jpg 的 JPEG 画面可能偏暗或全黑(冷解码 EOS 时序问题,
BMP 抓拍正常);Opus 音频流(/audio.opus)未实现;
WebRTC / 多码率 / GL 级旋转未实现(独立工程量级)。

## P0 设置补齐(已完成)

已从 Android 的 56 项设置中优先补齐 4 项核心参数：

| 设置项 | Android Key | 鸿蒙实现 |
|---|---|---|
| 移动侦测超时 | `motion_timeout` (10-120s) | UI TextInput + WebStream::SetMotionTimeout()(两端都钳位到 10-120,默认 15) |
| I 帧间隔 | `keyframe_interval` / `hevc_keyframe_interval` (1-10s) | UI TextInput + CameraStreamer::Start(iFrameMs) |
| H.265 码率 | `server_hevc_bitrate` | 复用码率选择（选 H.265 时自动应用） |
| RTSP 格式 | `rtsp_format` (H264/HEVC) | 已有 codec Select，标签对齐为"RTSP 格式" |

## P1 OSD 文字/水印叠加(已完成,真机验证通过)

对照 Android `preferences.xml` 文字叠加子页(17 个设置项,除 `text_font`/`custom_font` 字体
选择外实现 15 项)实现编码端 OSD 渲染:

| 功能 | 实现 |
|---|---|
| 叠加内容 | 时间戳(`yyyy-MM-dd HH:mm:ss`,与 Android 格式一致)/设备名(`deviceInfo.marketName+productModel`)/电量(`BAT:xx%`,`batteryInfo`,⚡AC/⚡Wireless 充电后缀)/GPS 经纬度+速度(`geoLocationManager` 5s 轮询,km/h 与 mph 双单位)/自定义文字 |
| 文字样式 | 位置(四角)/边距(0-100)/字体(常规·加粗·斜体·粗斜体)/颜色(`#RRGGBBAA`,默认白) |
| 水印图片 | 图库选图 → ImageKit 解码 RGBA_8888(长边限 1280)→ native 侧按"最大占画面面积百分比(1-100%)"缩放叠加,位置/边距可配 |
| 设置持久化 | preferences 按 Android 原始 key(`osd_padding_int`/`text_position`/`display_timestamp`/`watermark_overlay` 等)保存,重启自动恢复 |
| 生效方式 | 除总开关外全部热生效(逐帧读取配置);总开关切换自动重启相机切换管线 |

**渲染架构(三次方案迭代后的最终形态)**:OSD 开启时相机不再直连编码器 surface,而是经
`OsdPipeline` CPU 合成管线——相机 PreviewOutput 渲入 `OH_NativeImage` 消费面 → 工作线程
`AcquireNativeWindowBuffer` 取最新 NV12(读取最新、丢弃旧帧)→ `OsdRenderer` 用 OH_Drawing
Typography 每秒栅格化一次文字层(系统全局 FontCollection)→ 文字/水印直接 α 混合进 YUV 平面
(BT.601,Y 逐像素精确、UV 按 2×2 块均值)→ 编码器切换为 **buffer 模式**,经
`OnNeedInputBuffer/PushInputBuffer` 送 NV12。录像/RTSP/RTMP/Web 控制台全部自动携带 OSD。

**方案演进与踩坑(重要)**:
1. **GL/EGL 路线被设备驱动否决**:相机 NV12 缓冲 `OH_NativeImage_UpdateSurfaceImage` 内部
   `eglCreateImageKHR` 失败(`NATIVE_ERROR_EGL_API_FAILED`,每帧必现,设备仅提供 fmt=1003
   YUV 预览 profile,无 RGBA)。
2. **编码器 surface 软件写入被 HDI 否决**:生产者侧 `OH_NativeWindow_NativeWindowRequestBuffer`
   对编码器窗口返回 `NATIVE_ERROR_UNKNOWN`(改 CPU usage 亦然)。
3. **CPU 消费相机缓冲的两个致命坑**(`libsurface.z.so` 崩溃,均在相机侧 binder 线程):
   - 每帧 `OH_NativeBuffer_Unmap` 拆掉队列仍在复用的映射 → `BufferQueue::ReuseBuffer→
     SetMetadata` 内 vendor gralloc SEGV;
   - 每帧 `OH_NativeBuffer_Unreference` 把队列 free list 还持有的 SurfaceBuffer 释放 →
     `PopFromFreeListLocked` 空指针。
   **正解:按队列槽位建立一次性映射缓存(Map 一次,流期间绝不 Unmap/Unreference,Stop 时统一清理)**。
4. **`OH_ConsumerSurface_SetDefaultUsage(CPU_READ|WRITE)` 同样触发 ReuseBuffer 崩溃**,改用
   `OH_NativeImage_Create(0,0)`(与 GL 版相同队列风味)。
5. 自建 `OH_Drawing_CreateFontCollection` 在纯 native 线程上 `CreateTypographyHandler` 内
   SEGV(内部 shared_ptr 为垃圾值)——**必须用 `OH_Drawing_GetFontCollectionGlobalInstance()`**。

真机验证(华为畅享 90 Plus / HarmonyOS 7.0):720p 稳定推流 15s+,RTSP 取帧目检左上角三行
白字(`2026-07-28 06:28:27` / `华为畅享 60 Plus JUY-AL50` / `BAT:100%`)清晰可读;开关切换、
样式热调、水印选图链路可用。

**已知限制**:GPS 行需授权(未授权时显示 "GPS Waiting");水印缩放为最近邻;编码器 buffer
模式下 pts 由本机单调钟生成;测试机存在微信浮窗抢前台导致后台被杀(与 OSD 无关,开麦克风
连续任务保活可规避)。

## P2 自动启动系列(已完成,真机验证通过 2026-09-28)

对照 Android 四项自启动设置(沿用原 key 持久化):

| 设置项 | Android Key | 鸿蒙实现 |
|---|---|---|
| 启动即开服务 | `auto_rtsp_after_turn_on` | 页面加载后自动 `rtspStart`(Index.aboutToAppear) |
| 启动即推 RTMP | `auto_rtmp_after_turn_on` | 服务起来后自动 `rtmpStart`(连接异步化,失败走错误回调) |
| 客户端连入自动开摄 | `start_camera_on_connect`(默认开) | 状态回调 Connected 事件 → `startCameraNow()` |
| 开机自启 | `Start_on_boot` | **平台受限**:HarmonyOS 第三方应用无法编程式授权自启,`autoStartupManager` 仅提供 `getAutoStartupStatusForSelf()` 查询(API 21)。UI 保留开关并持久化,开启时提示用户到 系统设置>应用>IPCamera>启动管理 手动放行,并回读系统状态输出到日志 |

**真机验证**(华为畅享 90 Plus):拉起应用零点击即 "● RUNNING"(自启服务);PC 端
Python RTMP 测试服务器收到完整 handshake→connect→createStream→publish→onMetaData(自启
RTMP);停相机后 PC 端 ffplay 连入 RTSP,4 秒内相机自动重启推流(连入开摄)。

## P3 录制增强(已完成,真机验证通过 2026-09-28)

| 设置项 | Android Key | 鸿蒙实现 |
|---|---|---|
| 4GB 限制 | `four_gb_limit`(默认开) | StreamRecorder 计字节数,≥3968MiB(32 位 sample offset 安全线)关闭当前文件并滚动到下一段 |
| 定时分段 | `each_segment_length`(分钟,默认 10) | 每段可配时长,到点滚动;`0` 关闭 |
| 分段命名 | — | `ipc_<ts>.mp4 → ipc_<ts>_2.mp4 → _3…`,天然匹配 `/getarchives` 的 `ipc_*.mp4` 过滤 |
| 段间连续性 | — | 复用启动时捕获的 VPS/SPS/PPS 与音频配置重建 muxer;PTS 在新段从 0 重锚(下一写样本作为新基准),不丢样本(同线程紧邻完成关闭与重开) |
| 接口 | — | `rtspStartRecord(handle, path, {fourGbLimit, segmentMinutes})` 可选第三参 |

**不移植项(平台限制)**:`save_to_mkv`——OH_AVMuxer 仅支持 MP4/M4A,无 MKV 封装;`save_to_sdcard`——鸿蒙三方应用无公共存储写权限(沙盒 filesDir 即存档目录);`mp4_format` 独立录像编码选择——录像跟随 RTSP 编码(单编码器架构,与 Android 行为一致化)。

**真机验证**(分段=1 分钟,录约 95 秒):`ipc_232304.mp4`(60.04s,92.6MB)+
`ipc_232304_2.mp4`(35.3s,55MB),分段边界精确到帧;两段 ffprobe 均为 h264+aac
1280x720 双轨、结构完整可独立播放。

## /audio.opus 实时音频流(实现完成;真机受限,端点行为已验证)

Android Web 控制台的听声端点,鸿蒙补齐:

- **无需移植 libopus**:系统 `OH_AudioEncoder` 自带 `audio/opus` 编码器(API 11+,`OH_AVCODEC_MIMETYPE_AUDIO_OPUS`),48k 单声道 32kbps。
- `opus_stream.cpp`:MicStreamer 新增 PCM tap(采集回调直通)→ Opus 编码器(旧式 audio data API,与 AAC 编码同款)→ **自研 Ogg Opus 封装**(Ogg 页/CRC32 多项式 0x04c11db7/段表/OpusHead+OpusTags 头,granule 由源 pts 换算)→ 多客户端广播(每客户端有界队列,慢客户端 8s 队满断开)。
- 生命周期:编码器随首个客户端建、末个客户端销毁;`/audio.opus` 在麦克风未运行时返回 503,设备无 Opus 编码器时返回 501(运行时探测一次)。
- Web 面板新增 `<audio src='/audio.opus' controls>`,与 Android `/audio.opus` 语义一致。

**真机结论(华为畅享 90 Plus,HarmonyOS 7.0/JDY-AL50)**:该固件把 Opus 编码器注册进了
系统能力表(`OH.Media.Codec.Encoder.Audio.Opus`,声道 1-2、码率 6k-510k、48k 均在范围内,
按名创建实例也成功),但**底层插件拒绝一切 Configure(全部 AV_ERR_UNSUPPORT)**——逐一
排除:S16LE/F32LE/bare、单声道/立体声、有无先注册回调、CreateByName,结论为厂商固件
实际缺失 Opus 编码器插件。端点降级行为已验证:麦克风未开 → 503,编码器不可用 → 501
"opus unavail"。完整链路(Ogg 封装、多客户端广播、PCM tap、编码器随首末客户端起停)
在有可用 Opus 编码器的设备/模拟器上即自动激活。

排查附带收获(通用坑):**OH_AVCodec 的 Configure 是一次性状态转换——Configure 失败后
编码器实例不可复用,后续 Configure 一律 AV_ERR_INVALID_STATE(错误码 8);多配置变体尝试
必须每个变体重新 Create**。

## 阶段 8:ONVIF/播放器侧(已完成,真机验证通过 2026-09-28)

对照 Android 播放器侧四大 Activity 的鸿蒙实现:

| Android | 鸿蒙 | 说明 |
|---|---|---|
| OnvifScannerActivity | `OnvifScan.ets` + `onvif_client.cpp` | WS-Discovery Probe(UDP 组播 239.255.255.250:3702)→ ProbeMatch 去重 → SOAP GetProfiles + GetStreamUri(HTTP,自研极简 SOAP/解析,无 XML 库);扫描结果列 Model/XAddrs/流地址,一键复制;`onvifScan()` napi 异步(async work + Promise,返回 JSON) |
| LiveVideoActivity | `Player.ets` | **平台限制**:鸿蒙 AVPlayer 无 RTSP 协议支持。实时画面走 Web 组件加载自家 MJPEG(`/video?pw=<密码>` 免 Basic token 参数——Web 组件无法带 Authorization 头);rtsp:// 地址点击播放即复制到剪贴板供 VLC/ffplay 使用 |
| MediaPlayer(Plus)Activity | `Playback.ets` | AVPlayer + XComponent(surface 模式)回放 filesDir 下 `ipc_*.mp4` 录像,自动循环,暂停/停止 |
| RTMPListActivity | `RtmpAddrs.ets` | RTMP 地址列表(preferences `rtmpAddrList`),增/删/设为当前推流地址 |

真机验证:①PC 端 Python ONVIF 模拟器(WS-Discovery 应答 + SOAP 服务,temp 目录 `onvif_sim.py`),
手机扫描页完整显示 SimCam-T100 / XAddrs / `rtsp://…:8554/simcam/profile_1` / profile_1
(全链路:组播发现→Profile→流地址);②回放页播放 6 秒实测录像,state=playing,画面目检正确;
③Player 页 Web 组件渲染自家 MJPEG 实时画面目检正确;④token 认证 curl 双向验证(对 200 multipart、
错 401)。

**附带修复**:`deviceIp()` 字节序 bug——鸿蒙 `wifiManager.getIpInfo().ipAddress` 是**网络序(大端)**
数值,原 Index 的移位拆解方向反了(显示 123.110.168.192),两页统一改为从高字节拆解。

**验证环境坑**:手机→PC 的 WS-Discovery 组播与 SOAP 被默认 Windows Defender 防火墙拦截,
需入站放行 UDP 3702 + TCP 8000(已加规则 `ONVIF-Sim-UDP3702/TCP8000`,测试机保留);
模拟器本机组播自测可绕过防火墙先验证逻辑。鸿蒙返回键 uitest keyEvent 码为 **2**(155 无效)。

**已知边界**:ONVIF 发现无鉴权 GetProfiles/GetStreamUri(多数相机无需);流地址不直接内嵌播放
(无系统 RTSP 组件);扫描依赖 AP 转发组播(家用 AP 默认泛洪,企业网可能过滤)。

## 移动侦测对齐 Android(真机验证通过 2026-09-29,华为畅享 90 Plus / HarmonyOS 7.0)

Android 的"移动侦测"实质是**侦测命中即自动录像**:`BaseServerActivity` 收到 true → `F0()` 写
`DCIM/IPCamera/MD/IPS_yyyy-MM-dd.HH.mm.ss.SSSS.mp4`,`motion_timeout`(默认 15s,10-120)秒内
无新命中 → 停录;开启时 `G0()` 先 `R0()` 拉起服务器,再延迟 10 秒 `F0` 武装检测器
("Motion Detection will delay 10 seconds to open")。本轮把这条链路整体搬到鸿蒙:

| 环节 | Android | 鸿蒙实现 |
|---|---|---|
| 检测器武装 | 开启后 10 秒(postDelayed StartPreviewRunnable → `MotionDetection.start()`) | `WebStream::SetMotionEnabled(true)` 记 10s 期限(`kArmDelayUs`),到期前只刷新基准网格不上报 |
| 取帧 | 相机预览/解码帧直供 `detectNV21`/`detectBuffer`/`detectImage`,与网页无关 | WebStream 解码线程由"消费方引用"驱动:网页控制台(`IncClients`)与移动侦测(`motionDemand_`)各自计数,任一存在即运行——**不再依赖网页控制台开着** |
| 判定 | native `libmotiondetection.so`(无源码) | 32×18 亮度网格 3×3 均值采样,单格变化阈值 25,变化 ≥10 格为命中 |
| 停止条件 | 每次命中重启 15s 定时器,到期回调 false | 保持窗口 `motionHoldUs_`(默认 15s,钳位 10-120),同一帧循环里算下降沿 |
| 事件上报 | 每次检测结果都回调(`c(boolean)`) | 只在状态跳变时回调:`MOTION detected` / `MOTION cleared`(走 TSFN) |
| 命中动作 | 起服务器 + 录像到 `DCIM/IPCamera/MD/` | `Index.ets` 编排:起服务器/相机 → 建 `filesDir/MD/` → 录 `IPS_<yyyy-MM-dd.HH.mm.ss.SSSS>.mp4`(同一 `StreamRecorder`,沿用 four_gb_limit/分段设置) |
| 超时动作 | `X0()` 停计时 + 停录 | `MOTION cleared` → `stopRecord()` 收尾 |
| 网页可见 | 存档列表含 `DCIM/IPCamera/MD` | `/getarchives` 列出 `MD/IPS_*.mp4`,`/get/MD/IPS_*.mp4` 下载(路由与归档回调都按前缀白名单校验,禁 `..`/越目录) |
| 手动录像冲突 | 移动侦测开启时录像菜单不可用 | 移动侦测开启时"开启录像"按钮禁用;开启移动侦测会先停掉手工录像 |

**顺带修复**:WebStream 的 `SetParams` 原来只注入一次,相机换分辨率/编码后解码尺寸失配会让
网格在错误几何上取值 → 改为几何/编码变化时重新注入(`webParamsInjected`/`webParamsWidth`/
`webParamsH265` 原子),JPEG 与移动侦测都拿到真实帧大小;JPEG 编码现在只在有网页客户端时执行
(纯侦测场景不做无用编码)。

**仍未移植(Android 侧配套能力)**:命中通知邮件(`mail_smtp_notify` + 快照附件)、OneDrive/FTP 录像
上传、录制中每 `M` 分钟自动分段时另存快照(`ServerUiTimerTask` → `Z()`)。这三项依赖 SMTP/FTP/
云盘客户端,属 P4 独立工程;分段本身已由 `StreamRecorder` 的 `segmentMinutes` 覆盖。

**语义差异(有意为之)**:①Android 是 4fps(250ms)检测,鸿蒙冷解码 2fps(500ms),命中/收尾各有
≤0.5s 延迟;②判定算法无法逐位复刻(原库无源码),只对齐了"输入几何(降采样亮度)、时间语义
(命中保持 + 超时收尾)、上报节奏"这三层契约;③鸿蒙无 DCIM/媒体库写入,录像落在应用沙盒
`filesDir/MD`,通过网页存档下载,而不是进系统相册。

### 真机验证记录(2026-09-29)

| 环节 | 证据 |
|---|---|
| 开关即起服务/相机 | 应用日志:`RTSP server listening on :8554/live` → `camera started for motion detection` → `motion detection ON (timeout=15s, arms in 10s, auto record MD/)` 三条连续出现 |
| 10 秒武装延迟 | 22:45:05 开关打开,前 6 秒 0 条录像;首条 `recording to .../MD/IPS_2026-09-29.22.45.20.0490.mp4` 出现在 22:45:20(t+14.5s = 10s 延迟 + 相机启动 + 下一关键帧) |
| 命中即自动录像 | 22:35:12 检测到真人运动 → 同一时刻 `MOTION detected` + `motion record started (MD/IPS_2026-09-29.22.35.12.0498.mp4)` |
| 超时停录(15s 保持窗口) | 最后一次检测 22:37:49.678 + 15s = `recording stopped` 22:38:04.678,分秒不差 |
| 关开关立即停录 | 22:41:35 点"移动侦测:关" → 22:41:36.328 `recording stopped` |
| 再次命中重新开录 | 相机重启后 3 秒(22:39:37.647)开出新文件 `IPS_2026-09-29.22.39.37.0646.mp4` |
| 成片有效 | ffprobe:超时收尾片 314,029,693 字节 / 165.59s / 3331 帧 / h264 1280x720(时长=有帧区间 22:35:12.5→22:37:57);抽帧目检为现场画面 + OSD 时间戳 |
| 网页归档可见可下载 | `/getarchives` 列出 `MD/IPS_*.mp4 <size>`;`/get/MD/IPS_*.mp4` 下载 314MB 全量落盘 |
| 手动录像互斥 | 录制中布局:`移动侦测:录制中` 可点、`开启录像` disabled |
| 超时钳位 | 设置文件写入 `motionTimeout=5` 后重启,界面显示 `10`(下限钳位) |

**真机测试发现并修掉的两个 bug(均为既有代码,不是移动侦测逻辑本身)**:

1. **`/get/MD/...` 下载 404**:HTTP 层在路由前对路径做了 `ToLower`(`HandleRequest` 收到的是
   小写路径),`/get/ipc_*` 本来就全小写所以一直正常,带大写的 `MD/IPS_` 永远匹配不上。
   修法:路由按折叠形式匹配 `/get/md/ips_`,归档回调再把大小写还原成磁盘上的
   `MD/IPS_<date>.mp4`(其余字符是数字/点/横线,大小写无歧义)。
2. **大文件下载被截断**:`SendAll` 的 5 秒 deadline 是**整段内容**的预算,不是"停滞"超时——
   服务端 5 秒内把 283MB 灌进 socket 后主动断连(curl exit 18),314MB 的移动侦测录像永远下不完。
   修法:每写入成功就续期(`deadline = now + kSendDeadline`),语义变成"5 秒无进展才放弃",
   对慢客户端宽容、对卡死客户端仍然会断开。
   复测:同一 URL 由 `size=0/283183060`(截断)变为 `size=314029693`(全量,10.1s)。

**观察(既有行为,非本轮引入)**:录像码率远高于设置值——同一设置下用户既有录像为 5.36Mbps 与
19.2Mbps,移动侦测录像 15.2~16.3Mbps(设置 2Mbps)。720p/20fps 下这个码率异常偏高,疑似
buffer(OSD)模式下 `OH_MD_KEY_BITRATE` 未作为上限生效;后果是移动侦测录像很大(3 分钟 314MB,
约 7GB/小时)。建议后续单独排查码率控制(与本轮契约对齐无关)。

## 阶段 7(远期,未排期)

- Opus 音频流(/audio.opus):需移植 libopus(OHOS 三方库有现成移植)
- WebRTC:ICE/DTLS/SRTP/SDP 完整协议栈,独立工程
- 多码率:多编码器实例并行输出
- MJPEG JPEG 画面质量调优(冷解码时序/黑帧问题)
- 完整 UI 多页面复刻(ServerNgActivity 的所有设置面板)
