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
| /getarchives + /get/ipc_* | 录像存档列表 + MP4 下载 |
| /put_voice | AAC ADTS 语音上传 → OH_AudioDecoder 解码 → OH_AudioRenderer 播放(对讲) |
| 移动侦测 | WebStream 帧差分(32×18 亮度网格,阈值 25,持续 5s),事件走 TSFN 回调 |
| UI | 网页端口设置、启动/停止网页、移动侦测开关、HTTP URL 显示 |

**验证**:serverinfo(401→auth→正确 JSON 文本)、camswitch 原生切换成功、
snapshot.bmp 1280×720 目检正确(USB 线+织物纹理)、MP4 录像 24MB(hvc1 结构正确)。

**已知限制**:MJPEG/snapshot.jpg 的 JPEG 画面可能偏暗或全黑(冷解码 EOS 时序问题,
BMP 抓拍正常);移动侦测依赖视频解码帧,需网页控制台运行;Opus 音频流(/audio.opus)未实现;
WebRTC / 多码率 / GL 级旋转未实现(独立工程量级)。

## P0 设置补齐(已完成)

已从 Android 的 56 项设置中优先补齐 4 项核心参数：

| 设置项 | Android Key | 鸿蒙实现 |
|---|---|---|
| 移动侦测超时 | `motion_timeout` (10-120s) | UI TextInput + WebStream::SetMotionTimeout() |
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

## 阶段 7(远期,未排期)

- Opus 音频流(/audio.opus):需移植 libopus(OHOS 三方库有现成移植)
- WebRTC:ICE/DTLS/SRTP/SDP 完整协议栈,独立工程
- 多码率:多编码器实例并行输出
- MJPEG JPEG 画面质量调优(冷解码时序/黑帧问题)
- 完整 UI 多页面复刻(ServerNgActivity 的所有设置面板)
