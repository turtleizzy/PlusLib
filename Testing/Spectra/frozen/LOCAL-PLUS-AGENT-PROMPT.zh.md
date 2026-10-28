请在本机 PlusLib/ndicapi 项目实现随附 `PROTOCOL.zh.md + SCHEMA.json` 的 NDI raw IGTL v1.0 发送端；RMT端另有唯一实现线程，请先共享同份冻结合同，任何wire修改先同时反馈，禁止各自悄改字段。本任务不要求修改RMT源码，不自行提交/push或部署到当前采集默认配置。

目标：将同次BX得到的设备markerXYZ/状态/原始pose直接网络送主程序统一显示和保存，免手工解析sidecar。保留现TRANSFORM接口和既有sidecar过渡对账，新增独立、显式启用raw IGTL端口（默认建议18945），标准STRING/UTF8、设备名NDI_RAW_V1、header1、header timestamp0，四类session/frame/error/end，准确字段以SCHEMA及合同为准。不是改XML就能完成；不要把新raw内容塞成TRANSFORM，也不要通过现Plus滤时/插值后pose反拼raw帧。

先读本地AGENTS、确认源码/二进制/原可用构建与串口配置，按最小变更实现。保留当前Spectra字节间发送节奏NDICAPI_SERIAL_WRITE_DELAY_US和已修正的分包/长度/CRC读取；不新增独立串口reader、不额外poll、不用pose×ROM生成单球。生产主链是同次BX一次生成不可变内存snapshot SSOT，分发network与sidecar；不得读sidecar再解析回网络。

优先实现B3现场所需：实时raw连接/版本、每工具每slot状态/XYZ/当前有效数、是否重复/陈旧/隔离、丢失与不完整。不要硬编码两工具或A–D四球；工具pose有效与marker全数有效不同。defined_slots不知时明确分母未知，身份不知坐标放unidentified_positions，不能补上一值/零值。rawpose只读本BXpose_present；legacy提交矩阵另放plus_submission且绝不能当raw有效值。

按合同实现session/acquisition/source身份与seq；counter各工具保存原值，FIRST/NEW/REPEAT与wrap/reset不确定域分开，回退或同counter内容变动隔离，恢复不能仅靠flush、CRC或counter递增。无事务ID的协议限制要诚实保留，hostpoll序号不冒充设备曝光帧。源monotonic和UTC分开带clock_domain，不能伪称跨机同步/曝光时间。

网络worker必须有界队列/字节预算，不在串口producer做socket/磁盘/JSON I/O；DROP_NEW记seq缺口和sticky incomplete。完善end计数/digest、幂等Stop、异常Start清理和错误结构化上报，不仅LOG。sender发送完成不等对端落盘，移除sidecar须等待接收端耐久收尾和逐帧对账验收。

一并处理此前5项源码条件问题（行号为上传spectra-current-20261026-v2目标）：
1) ndicapi_bx_capture.cxx::ndiCaptureBX 133–165：A超时、Bflush后迟到A会被当B。显式失同步/恢复；不得承诺无事务ID仍能认证关联。
2) 同函数131–145与ndicapi_serial_unix.cxx::ndiSerialWrite389–425、ndiSerialRead449–450、ndicapi.cxx::ndiSerialProbe801–830：deadline写前已计时但共享write不受约束，generic read EAGAIN/EINTR可无限重试；capture自己的接收循环会回顶查期限，勿误改。
3) vtkPlusNDITracker::InternalStopRecording581–586：recorder Stop false仍PLUS_SUCCESS，只日志；使archive失败可机读且工具状态语义不混淆。
4) PlusNDIMarkerRecorder::Stop235–253：仅drop导致首次false但Failed=false，第二次可true；保持首次final结果和原summary。
5) Recorder::Start196–220（197/217–219）与driver541–545：thread创建失败Started未复位，重试already-started；清理资源并保留失败证据，允许新会话重试。
这些未在已验录制中观察到，不据此判旧数据失效。完整静态审查见此前BX-REVIEW/RECORDER-REVIEW；不要把它说成已完成运行时故障复现。

离线先做schema/跨字段验证、现成synthetic golden和真实正常BX字节golden；补正常多slot/缺球/OOV/status4/unknown/NaN状态转换、counter不连续、迟到回复、任意TCP分片/粘包、CRC/尺寸/重复key/seq乱序/重连、queue/短写/Stop/Start与所有写盘失败回归。raw错误不能停掉原科学录制。然后用本机Spectra在原pacing下同次BX20Hz做持续录制和显式故障恢复回归；报告实际时长/计数/积压/有效唯一帧，不仅平均行频。

交付：源diff与版本/二进制hash、endpoint配置开关（默认off）、真实/合成golden及来源、离线+串口测试命令结果、SSOT字段到旧sidecar无损映射、收尾计数/摘要、未验证项和协议歧义。若合同与真实源码/厂商状态语义冲突，明确最小证据并联系两端协调，不猜、不改标准、不悄改schema、不放宽QC凑通过。
